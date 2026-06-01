/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/common.h"
#include "api/searchcache.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "sam/sam.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

     class DocumentsSAMTrainingDedupe
     {
       public:
         explicit DocumentsSAMTrainingDedupe(size_t MaxEntries)
             : Max(MaxEntries)
         {
         }

         size_t Size()
         {
              std::lock_guard<std::mutex> Lock(Mutex);
              return LastSeen.size();
         }

         void Clear()
         {
              std::lock_guard<std::mutex> Lock(Mutex);
              LastSeen.clear();
              Order.clear();
              LastPurgeMS = 0;
         }

         bool ShouldAllow(const std::string& Key, uint64_t NowMS, uint64_t WindowMS, uint64_t RetentionMS = 0)
         {
              if (WindowMS == 0 || Key.empty())
              {
                   return true;
              }

              std::lock_guard<std::mutex> Lock(Mutex);

              PurgeExpiredLocked(NowMS, RetentionMS);

              auto It = LastSeen.find(Key);
              if (It != LastSeen.end())
              {
                   if (NowMS >= It->second && (NowMS - It->second) < WindowMS)
                   {
                        return false;
                   }
                   It->second = NowMS;
                   return true;
              }

              LastSeen.emplace(Key, NowMS);
              Order.push_back(Key);

              while (Order.size() > Max)
              {
                   LastSeen.erase(Order.front());
                   Order.pop_front();
              }

              return true;
         }

       private:
         const size_t Max;
         std::mutex Mutex;
         std::unordered_map<std::string, uint64_t> LastSeen;
         std::deque<std::string> Order;
         uint64_t LastPurgeMS = 0;

         void PurgeExpiredLocked(uint64_t NowMS, uint64_t RetentionMS)
         {
              if (RetentionMS == 0)
              {
                   return;
              }

              constexpr uint64_t kMinPurgeIntervalMS = 60ULL * 1000ULL;
              if (LastPurgeMS > 0 && NowMS > LastPurgeMS && (NowMS - LastPurgeMS) < kMinPurgeIntervalMS)
              {
                   return;
              }

              LastPurgeMS = NowMS;
              const uint64_t ExpireBefore = NowMS > RetentionMS ? (NowMS - RetentionMS) : 0ULL;

              for (auto It = Order.begin(); It != Order.end();)
              {
                   const auto SeenIt = LastSeen.find(*It);
                   if (SeenIt == LastSeen.end())
                   {
                        It = Order.erase(It);
                        continue;
                   }

                   if (SeenIt->second > 0 && SeenIt->second < ExpireBefore)
                   {
                        LastSeen.erase(SeenIt);
                        It = Order.erase(It);
                        continue;
                   }

                   ++It;
              }
         }
     };

     static DocumentsSAMTrainingDedupe gSearchIdeaDedupe(32768);
     static DocumentsSAMTrainingDedupe gInteractionDedupe(32768);

     class SAMInteractionAbuseGuard
     {
       public:
         struct StatsSnapshot
         {
              size_t ActorMinute = 0;
              size_t ActorHour = 0;
              size_t DocQueryHour = 0;
         };

         StatsSnapshot Snapshot()
         {
              std::lock_guard<std::mutex> Lock(Mutex);
              StatsSnapshot S;
              S.ActorMinute = ActorMinute.size();
              S.ActorHour = ActorHour.size();
              S.DocQueryHour = DocQueryHour.size();
              return S;
         }

         void Clear()
         {
              std::lock_guard<std::mutex> Lock(Mutex);
              ActorMinute.clear();
              ActorHour.clear();
              DocQueryHour.clear();
              LastPurgeMS = 0;
         }

         bool ShouldAllow(const std::string& ActorKey,
                          const std::string& Collection,
                          const std::string& Query,
                          const std::string& DocumentID,
                          uint64_t NowMS,
                          int RetentionDays,
                          int MaxPerMinute,
                          int MaxPerHour,
                          int MaxPerDocQueryPerHour)
         {
              if (ActorKey.empty() || Collection.empty() || Query.empty() || DocumentID.empty())
              {
                   return true;
              }

              PurgeExpiredLocked(NowMS, RetentionDays);

              if (!ApplyActorLimitsLocked(ActorKey, NowMS, MaxPerMinute, MaxPerHour))
              {
                   return false;
              }

              if (!ApplyDocQueryBurstLimitLocked(Collection, Query, DocumentID, NowMS, MaxPerDocQueryPerHour))
              {
                   return false;
              }

              return true;
         }

       private:
         struct TimeWindowCounter
         {
              std::deque<uint64_t> Samples;
              uint64_t LastSeenMS = 0;
         };

         std::mutex Mutex;
         uint64_t LastPurgeMS = 0;
         std::unordered_map<std::string, TimeWindowCounter> ActorMinute;
         std::unordered_map<std::string, TimeWindowCounter> ActorHour;
         std::unordered_map<std::string, TimeWindowCounter> DocQueryHour;

         static void PruneDeque(std::deque<uint64_t>& Values, uint64_t NowMS, uint64_t WindowMS)
         {
              while (!Values.empty())
              {
                   const uint64_t TS = Values.front();
                   if (TS > NowMS)
                   {
                        Values.pop_front();
                        continue;
                   }
                   if ((NowMS - TS) < WindowMS)
                   {
                        break;
                   }
                   Values.pop_front();
              }
         }

         void PurgeExpiredLocked(uint64_t NowMS, int RetentionDays)
         {
              std::lock_guard<std::mutex> Lock(Mutex);

              constexpr uint64_t kMinPurgeIntervalMS = 60ULL * 1000ULL;
              if (LastPurgeMS > 0 && NowMS > LastPurgeMS && (NowMS - LastPurgeMS) < kMinPurgeIntervalMS)
              {
                   return;
              }

              LastPurgeMS = NowMS;

              const uint64_t RetentionMS =
                   RetentionDays <= 0 ? 0ULL : static_cast<uint64_t>(RetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
              const uint64_t ExpireBefore = RetentionMS == 0 ? 0ULL : (NowMS > RetentionMS ? (NowMS - RetentionMS) : 0ULL);

              auto PurgeMap = [&](auto& Map)
              {
                   for (auto It = Map.begin(); It != Map.end();)
                   {
                        const uint64_t LastSeen = It->second.LastSeenMS;
                        if (RetentionMS > 0 && LastSeen > 0 && LastSeen < ExpireBefore)
                        {
                             It = Map.erase(It);
                             continue;
                        }
                        ++It;
                   }
              };

              PurgeMap(ActorMinute);
              PurgeMap(ActorHour);
              PurgeMap(DocQueryHour);
         }

         bool ApplyActorLimitsLocked(const std::string& ActorKey, uint64_t NowMS, int MaxPerMinute, int MaxPerHour)
         {
              std::lock_guard<std::mutex> Lock(Mutex);

              if (MaxPerMinute > 0)
              {
                   TimeWindowCounter& Counter = ActorMinute[ActorKey];
                   Counter.LastSeenMS = NowMS;
                   PruneDeque(Counter.Samples, NowMS, 60ULL * 1000ULL);
                   if (Counter.Samples.size() >= static_cast<size_t>(MaxPerMinute))
                   {
                        return false;
                   }
                   Counter.Samples.push_back(NowMS);
              }

              if (MaxPerHour > 0)
              {
                   TimeWindowCounter& Counter = ActorHour[ActorKey];
                   Counter.LastSeenMS = NowMS;
                   PruneDeque(Counter.Samples, NowMS, 60ULL * 60ULL * 1000ULL);
                   if (Counter.Samples.size() >= static_cast<size_t>(MaxPerHour))
                   {
                        return false;
                   }
                   Counter.Samples.push_back(NowMS);
              }

              return true;
         }

         bool ApplyDocQueryBurstLimitLocked(const std::string& Collection,
                                           const std::string& Query,
                                           const std::string& DocumentID,
                                           uint64_t NowMS,
                                           int MaxPerDocQueryPerHour)
         {
              if (MaxPerDocQueryPerHour <= 0)
              {
                   return true;
              }

              std::lock_guard<std::mutex> Lock(Mutex);

              const std::string Key = Collection + "\n" + Query + "\n" + DocumentID;
              TimeWindowCounter& Counter = DocQueryHour[Key];
              Counter.LastSeenMS = NowMS;
              PruneDeque(Counter.Samples, NowMS, 60ULL * 60ULL * 1000ULL);
              if (Counter.Samples.size() >= static_cast<size_t>(MaxPerDocQueryPerHour))
              {
                   return false;
              }
              Counter.Samples.push_back(NowMS);
              return true;
         }
     };

     static SAMInteractionAbuseGuard gSamInteractionAbuseGuard;

     static std::string NormalizeControlToken(std::string Value)
     {
          const size_t Start = Value.find_first_not_of(" \t\r\n");

          if (Start == std::string::npos)
          {
               return "";
          }

          const size_t End = Value.find_last_not_of(" \t\r\n");
          Value = Value.substr(Start, End - Start + 1);

          std::transform(Value.begin(), Value.end(), Value.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          return Value;
     }

     static bool IsTruthyControlToken(const std::string& Value)
     {
          const std::string Token = NormalizeControlToken(Value);
          return Token == "1" || Token == "true" || Token == "yes" || Token == "on" || Token == "skip";
     }

     static bool IsFalsyControlToken(const std::string& Value)
     {
          const std::string Token = NormalizeControlToken(Value);
          return Token == "0" || Token == "false" || Token == "no" || Token == "off";
     }

     static bool ShouldSkipSAMRecording(const HttpRequest& Request)
     {
          const auto SkipIt = Request.QueryParams.find("skip");
          if (SkipIt != Request.QueryParams.end() && IsTruthyControlToken(SkipIt->second))
          {
               return true;
          }

          const auto SkipRecordIt = Request.QueryParams.find("skip_record");
          if (SkipRecordIt != Request.QueryParams.end() && IsTruthyControlToken(SkipRecordIt->second))
          {
               return true;
          }

          const auto NoRecordIt = Request.QueryParams.find("no_record");
          if (NoRecordIt != Request.QueryParams.end() && IsTruthyControlToken(NoRecordIt->second))
          {
               return true;
          }

          const auto RecordIt = Request.QueryParams.find("record");
          if (RecordIt != Request.QueryParams.end() && IsFalsyControlToken(RecordIt->second))
          {
               return true;
          }

          auto HeaderIt = Request.Headers.find("X-HLQ-Skip-SAM-Record");
          if (HeaderIt == Request.Headers.end())
          {
               HeaderIt = Request.Headers.find("x-hlq-skip-sam-record");
          }

          return HeaderIt != Request.Headers.end() && IsTruthyControlToken(HeaderIt->second);
     }

     static bool ShouldRecordSAMSearchIdea(const HttpRequest& Request,
                                   const std::string& Collection,
                                   const std::string& Query)
     {
          if (ShouldSkipSAMRecording(Request))
          {
               return false;
          }

          if (!Instance || !Instance->Config || !Instance->Config->GetSamRecordSearchIdeas())
          {
               return false;
          }

          const int WindowMs = Instance->Config->GetSamSearchIdeaDedupeWindowMs();
          if (WindowMs <= 0)
          {
               return true;
          }

          const uint64_t NowMS = static_cast<uint64_t>(NowMs());
          const uint64_t WindowMS = static_cast<uint64_t>(WindowMs);
          const int RetentionDays = Instance->Config->GetSamActorMetadataRetentionDays();
          const uint64_t RetentionMS =
               RetentionDays <= 0 ? 0ULL : static_cast<uint64_t>(RetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
          const std::string ActorKey = Request.APIKeyID.empty() ? Request.RemoteAddress : Request.APIKeyID;
          const std::string Key = ActorKey + "\n" + Collection + "\n" + Query;
          return gSearchIdeaDedupe.ShouldAllow(Key, NowMS, WindowMS, RetentionMS);
     }

     bool ShouldRecordSAMInteraction(const HttpRequest& Request,
                                     const std::string& Collection,
                                     const std::string& Query,
                                     const std::string& DocumentID)
     {
          if (ShouldSkipSAMRecording(Request))
          {
               return false;
          }

          if (!Instance || !Instance->Config || !Instance->Config->GetSamRecordInteractions())
          {
               return false;
          }

          {
               const uint64_t NowMS = static_cast<uint64_t>(NowMs());
               const int RetentionDays = Instance->Config->GetSamActorMetadataRetentionDays();
               const int MaxPerMinute = Instance->Config->GetSamInteractionMaxPerMinute();
               const int MaxPerHour = Instance->Config->GetSamInteractionMaxPerHour();
               const int MaxPerDocQueryPerHour = Instance->Config->GetSamInteractionMaxPerDocQueryPerHour();
               const std::string ActorKey = Request.APIKeyID.empty() ? Request.RemoteAddress : Request.APIKeyID;

               if (!gSamInteractionAbuseGuard.ShouldAllow(ActorKey,
                                                         Collection,
                                                         Query,
                                                         DocumentID,
                                                         NowMS,
                                                         RetentionDays,
                                                         MaxPerMinute,
                                                         MaxPerHour,
                                                         MaxPerDocQueryPerHour))
               {
                    return false;
               }
          }

          const int WindowMs = Instance->Config->GetSamInteractionDedupeWindowMs();
          if (WindowMs <= 0)
          {
               return true;
          }

          const uint64_t NowMS = static_cast<uint64_t>(NowMs());
          const uint64_t WindowMS = static_cast<uint64_t>(WindowMs);
          const int RetentionDays = Instance->Config->GetSamActorMetadataRetentionDays();
          const uint64_t RetentionMS =
               RetentionDays <= 0 ? 0ULL : static_cast<uint64_t>(RetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
          const std::string ActorKey = Request.APIKeyID.empty() ? Request.RemoteAddress : Request.APIKeyID;
          const std::string Key = ActorKey + "\n" + Collection + "\n" + Query + "\n" + DocumentID;
          return gInteractionDedupe.ShouldAllow(Key, NowMS, WindowMS, RetentionMS);
     }

static std::vector<SAM::SearchIdeaDocumentRef> BuildSAMIdeaDocumentsFromLookupHits(const std::vector<SAM::LookupHit> &Hits,
                                                                                   size_t MaxDocuments = 6)
{
     std::vector<SAM::SearchIdeaDocumentRef> Documents;
     std::unordered_set<std::string> Seen;

     for (const auto &Hit : Hits)
     {
          if (Hit.DocumentID.empty() || !Seen.insert(Hit.DocumentID).second)
          {
               continue;
          }

          Documents.push_back({Hit.DocumentID, Hit.Title, std::max(0.05, Hit.Breakdown.FinalScore > 0.0 ? Hit.Breakdown.FinalScore : Hit.MatchedScore)});

          if (Documents.size() >= MaxDocuments)
          {
               break;
          }
     }

     return Documents;
}

static nlohmann::json BuildDocumentJSON(const Document &Doc)
{
     nlohmann::json J;

     if (!Doc.ID.empty())
     {
          J["id"] = Doc.ID;
     }
     if (!Doc.Title.empty())
     {
          J["title"] = Doc.Title;
     }

     if (!Doc.Content.empty())
     {
          J["content"] = Doc.Content;
     }

     if (!Doc.Fields.empty())
     {
          for (const auto &Pair : Doc.Fields)
          {
               J[Pair.first] = Pair.second;
          }
     }

     if (Doc.Score != 0.0)
     {
          J["score"] = Doc.Score;
     }

     if (Doc.Timestamp != 0)
     {
          J["timestamp"] = Doc.Timestamp;
     }

     return J;
}

static std::string TrimCopy(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static std::string ToLowerCopy(const std::string &Value)
{
     std::string Out = Value;
     std::transform(Out.begin(), Out.end(), Out.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Out;
}

static bool IsTruthyToken(const std::string &Value)
{
     const std::string Token = ToLowerCopy(TrimCopy(Value));
     return Token == "1" || Token == "true" || Token == "yes" || Token == "on" || Token == "all";
}

static bool IsLocalHostName(const std::string &Host)
{
     const std::string Lower = ToLowerCopy(Host);
     return Lower == "localhost" || Lower == "127.0.0.1" || Lower == "::1" || Lower == "0.0.0.0";
}

static std::string NormalizeNodeEndpointKey(const std::string &RawEndpoint)
{
     std::string Host;
     int Port = 0;
     std::string Scheme;

     if (!ParseSharedNodeEndpoint(RawEndpoint, Host, Port, &Scheme))
     {
          return ToLowerCopy(TrimCopy(RawEndpoint));
     }

     std::ostringstream Key;
     Key << ToLowerCopy(Host) << ":" << Port;
     return Key.str();
}

struct SAMDistributedNode
{
     std::string Raw;
     std::string Host;
     int Port = 0;
     bool IsLocal = false;
};

static std::vector<std::string> ParseCommaSeparatedSAMValues(const std::string &Input)
{
     std::vector<std::string> Values;
     std::stringstream Stream(Input);
     std::string Token;

     while (std::getline(Stream, Token, ','))
     {
          Token = TrimCopy(Token);

          if (!Token.empty())
          {
               Values.push_back(Token);
          }
     }

     return Values;
}

static bool BuildSAMDistributedNodes(std::vector<SAMDistributedNode> &OutNodes)
{
     OutNodes.clear();

     if (!(Instance && Instance->Config))
     {
          return false;
     }

     const std::vector<std::string> ClusterNodes = Instance->Config->GetClusterNodes();
     const std::vector<std::string> SlaveNodes = Instance->Config->GetSlaveNodes();
     std::unordered_set<std::string> ReplicaEndpoints;

     for (const auto &Replica : SlaveNodes)
     {
          ReplicaEndpoints.insert(NormalizeNodeEndpointKey(Replica));
     }

     std::unordered_set<std::string> Seen;

     for (const auto &Node : ClusterNodes)
     {
          const std::string EndpointKey = NormalizeNodeEndpointKey(Node);

          if (EndpointKey.empty() || ReplicaEndpoints.find(EndpointKey) != ReplicaEndpoints.end() ||
              !Seen.insert(EndpointKey).second)
          {
               continue;
          }

          SAMDistributedNode Entry;
          std::string Scheme;

          if (!ParseSharedNodeEndpoint(Node, Entry.Host, Entry.Port, &Scheme))
          {
               continue;
          }

          Entry.Raw = Node;
          Entry.IsLocal = IsLocalHostName(Entry.Host);
          OutNodes.push_back(std::move(Entry));
     }

     return !OutNodes.empty();
}

static double GetSAMHitSortScore(const SAM::LookupHit &Hit)
{
     return Hit.Breakdown.FinalScore > 0.0 ? Hit.Breakdown.FinalScore : Hit.MatchedScore;
}

static std::vector<SAM::LookupHit> MergeDistributedSAMHits(const std::vector<SAM::LookupHit> &Hits,
                                                           size_t Limit)
{
     std::unordered_map<std::string, SAM::LookupHit> BestHits;

     for (const auto &Hit : Hits)
     {
          const std::string Key = Hit.Collection + "\n" + Hit.DocumentID;
          auto Existing = BestHits.find(Key);

          if (Existing == BestHits.end())
          {
               BestHits.emplace(Key, Hit);
               continue;
          }

          const double ExistingScore = GetSAMHitSortScore(Existing->second);
          const double CandidateScore = GetSAMHitSortScore(Hit);

          if (CandidateScore > ExistingScore)
          {
               Existing->second = Hit;
               continue;
          }

          Existing->second.EvidenceCount = std::max(Existing->second.EvidenceCount, Hit.EvidenceCount);
     }

     std::vector<SAM::LookupHit> Merged;
     Merged.reserve(BestHits.size());

     for (auto &Pair : BestHits)
     {
          Merged.push_back(std::move(Pair.second));
     }

     std::sort(Merged.begin(), Merged.end(),
               [](const SAM::LookupHit &Left, const SAM::LookupHit &Right)
               {
                    const double LeftScore = GetSAMHitSortScore(Left);
                    const double RightScore = GetSAMHitSortScore(Right);

                    if (LeftScore != RightScore)
                    {
                         return LeftScore > RightScore;
                    }

                    if (Left.EvidenceCount != Right.EvidenceCount)
                    {
                         return Left.EvidenceCount > Right.EvidenceCount;
                    }

                    if (Left.Collection != Right.Collection)
                    {
                         return Left.Collection < Right.Collection;
                    }

                    return Left.DocumentID < Right.DocumentID;
               });

     if (Limit > 0 && Merged.size() > Limit)
     {
          Merged.resize(Limit);
     }

     return Merged;
}

static SAM::LookupHit ParseSAMLookupHitJSON(const nlohmann::json &HitJSON)
{
     SAM::LookupHit Hit;
     Hit.Collection = HitJSON.value("collection", "");
     Hit.DocumentID = HitJSON.value("id", "");
     Hit.Title = HitJSON.value("title", "");
     Hit.MatchedTerm = HitJSON.value("term", "");
     Hit.MatchedKind = HitJSON.value("kind", "");
     Hit.MatchedSource = HitJSON.value("source", "");
     Hit.MatchedPath = HitJSON.value("matched_path", "");
     Hit.TermOrigin = HitJSON.value("term_origin", "");
     Hit.EvidenceCount = static_cast<size_t>(std::max<int64_t>(0, HitJSON.value("evidence_count", 0)));
     Hit.MatchedScore = HitJSON.value("score", 0.0);
     Hit.MatchedSignal = HitJSON.value("signal", 0.0);

     if (HitJSON.contains("score_breakdown") && HitJSON["score_breakdown"].is_object())
     {
          const nlohmann::json &Breakdown = HitJSON["score_breakdown"];
          Hit.Breakdown.TermScore = Breakdown.value("term_score", 0.0);
          Hit.Breakdown.SourceDocScore = Breakdown.value("source_doc_score", 0.0);
          Hit.Breakdown.SemanticScore = Breakdown.value("semantic_score", 0.0);
          Hit.Breakdown.SemanticVectorScore = Breakdown.value("semantic_vector_score", 0.0);
          Hit.Breakdown.EvidenceBonus = Breakdown.value("evidence_bonus", 0.0);
          Hit.Breakdown.DocPrior = Breakdown.value("doc_prior", 0.0);
          Hit.Breakdown.RankPriorScore = Breakdown.value("rank_prior_score", 0.0);
          Hit.Breakdown.RankPriorMultiplier = Breakdown.value("rank_prior_multiplier", 1.0);
          Hit.Breakdown.SemanticBonus = Breakdown.value("semantic_bonus", 0.0);
          Hit.Breakdown.SourceDocBonus = Breakdown.value("source_doc_bonus", 0.0);
          Hit.Breakdown.FinalScore = Breakdown.value("final_score", 0.0);
     }

     Hit.Explain = HitJSON.value("explain", "");
     return Hit;
}

static nlohmann::json BuildSAMHitJSON(const SAM::LookupHit &Hit, bool IncludeExplain)
{
     nlohmann::json HitJSON = {
          {"collection", Hit.Collection},
          {"id", Hit.DocumentID},
          {"title", Hit.Title},
          {"term", Hit.MatchedTerm},
          {"kind", Hit.MatchedKind},
          {"source", Hit.MatchedSource},
          {"matched_path", Hit.MatchedPath},
          {"term_origin", Hit.TermOrigin},
          {"evidence_count", Hit.EvidenceCount},
          {"score", Hit.MatchedScore},
          {"signal", Hit.MatchedSignal},
          {"score_breakdown", {
               {"term_score", Hit.Breakdown.TermScore},
               {"source_doc_score", Hit.Breakdown.SourceDocScore},
               {"semantic_score", Hit.Breakdown.SemanticScore},
               {"semantic_vector_score", Hit.Breakdown.SemanticVectorScore},
               {"evidence_bonus", Hit.Breakdown.EvidenceBonus},
               {"doc_prior", Hit.Breakdown.DocPrior},
               {"rank_prior_score", Hit.Breakdown.RankPriorScore},
               {"rank_prior_multiplier", Hit.Breakdown.RankPriorMultiplier},
               {"semantic_bonus", Hit.Breakdown.SemanticBonus},
               {"source_doc_bonus", Hit.Breakdown.SourceDocBonus},
               {"final_score", Hit.Breakdown.FinalScore}
          }}
     };

     if (IncludeExplain && !Hit.Explain.empty())
     {
          HitJSON["explain"] = Hit.Explain;
     }

     return HitJSON;
}

static int CompareNaturalString(const std::string &A, const std::string &B)
{
     size_t I = 0;
     size_t J = 0;

     while (I < A.length() && J < B.length())
     {
          if (std::isdigit(static_cast<unsigned char>(A[I])) && std::isdigit(static_cast<unsigned char>(B[J])))
          {
               size_t NumStartA = I;
               size_t NumStartB = J;

               while (I < A.length() && std::isdigit(static_cast<unsigned char>(A[I])))
               {
                    I++;
               }

               while (J < B.length() && std::isdigit(static_cast<unsigned char>(B[J])))
               {
                    J++;
               }

               const long long NumA = std::stoll(A.substr(NumStartA, I - NumStartA));
               const long long NumB = std::stoll(B.substr(NumStartB, J - NumStartB));

               if (NumA != NumB)
               {
                    return (NumA < NumB) ? -1 : 1;
               }
          }
          else
          {
               const char CharA = std::tolower(static_cast<unsigned char>(A[I]));
               const char CharB = std::tolower(static_cast<unsigned char>(B[J]));

               if (CharA != CharB)
               {
                    return (CharA < CharB) ? -1 : 1;
               }

               I++;
               J++;
          }
     }

     if (I < A.length())
     {
          return 1;
     }

     if (J < B.length())
     {
          return -1;
     }

     return 0;
}

static bool ParseNonNegativeIntParam(const std::map<std::string, std::string> &Params,
                                     const std::string &Key,
                                     int DefaultValue,
                                     int &OutValue)
{
     OutValue = DefaultValue;

     const auto It = Params.find(Key);

     if (It == Params.end())
     {
          return true;
     }

     try
     {
          const int Parsed = std::stoi(It->second);

          if (Parsed < 0)
          {
               return false;
          }

          OutValue = Parsed;
          return true;
     }
     catch (...)
     {
          return false;
     }
}

static bool ExtractSAMDocumentPathParts(const std::string &Path,
                                        std::string &Collection,
                                        std::string &DocumentID)
{
     Collection.clear();
     DocumentID.clear();

     std::string Normalized = Path;
     const size_t QueryPos = Normalized.find('?');

     if (QueryPos != std::string::npos)
     {
          Normalized = Normalized.substr(0, QueryPos);
     }

     if (Normalized.size() > 1 && Normalized.back() == '/')
     {
          Normalized.pop_back();
     }

     const std::string Prefix = "/sam/documents/";

     if (Normalized.rfind(Prefix, 0) != 0)
     {
          return false;
     }

     const std::string Remainder = Normalized.substr(Prefix.size());
     const size_t SlashPos = Remainder.find('/');

     if (SlashPos == std::string::npos || SlashPos == 0 || SlashPos + 1 >= Remainder.size())
     {
          return false;
     }

     Collection = Remainder.substr(0, SlashPos);
     DocumentID = Remainder.substr(SlashPos + 1);
     return !Collection.empty() && !DocumentID.empty();
}

static bool ExtractSAMLabelAddPathParts(const std::string &Path,
                                        std::string &Collection,
                                        std::string &DocumentID)
{
     Collection.clear();
     DocumentID.clear();

     std::string Normalized = Path;
     const size_t QueryPos = Normalized.find('?');

     if (QueryPos != std::string::npos)
     {
          Normalized = Normalized.substr(0, QueryPos);
     }

     if (Normalized.size() > 1 && Normalized.back() == '/')
     {
          Normalized.pop_back();
     }

     const std::string Prefix = "/sam/label/add/";

     if (Normalized.rfind(Prefix, 0) != 0)
     {
          return false;
     }

     const std::string Remainder = Normalized.substr(Prefix.size());
     const size_t SlashPos = Remainder.find('/');

     if (SlashPos == std::string::npos || SlashPos == 0 || SlashPos + 1 >= Remainder.size())
     {
          return false;
     }

     Collection = Remainder.substr(0, SlashPos);
     DocumentID = Remainder.substr(SlashPos + 1);
     return !Collection.empty() && !DocumentID.empty();
}

static std::vector<std::string> SplitManualLabels(const std::string &RawValue)
{
     std::vector<std::string> Labels;
     std::string Current;

     for (char C : RawValue)
     {
          if (C == ',' || C == '\n' || C == '\r' || C == '\t')
          {
               std::string Label = TrimCopy(Current);

               if (!Label.empty())
               {
                    Labels.push_back(Label);
               }

               Current.clear();
               continue;
          }

          Current.push_back(C);
     }

     std::string Label = TrimCopy(Current);

     if (!Label.empty())
     {
          Labels.push_back(Label);
     }

     return Labels;
}

static std::vector<std::string> ParseExistingLabels(const std::string &RawValue)
{
     const std::string Trimmed = TrimCopy(RawValue);

     if (Trimmed.empty())
     {
          return {};
     }

     try
     {
          nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

          if (Parsed.is_array())
          {
               std::vector<std::string> Labels;

               for (const auto &Item : Parsed)
               {
                    std::string Label;

                    if (Item.is_string())
                    {
                         Label = TrimCopy(Item.get<std::string>());
                    }
                    else if (!Item.is_null())
                    {
                         Label = TrimCopy(Item.dump());
                    }

                    if (!Label.empty())
                    {
                         Labels.push_back(Label);
                    }
               }

               return Labels;
          }
     }
     catch (...)
     {
     }

     return SplitManualLabels(Trimmed);
}

static std::vector<std::string> ExtractManualLabelsFromRequest(const HttpRequest &Request)
{
     std::vector<std::string> Labels;

     const auto QueryLabelIt = Request.QueryParams.find("label");
     if (QueryLabelIt != Request.QueryParams.end())
     {
          const std::vector<std::string> QueryLabels = SplitManualLabels(QueryLabelIt->second);
          Labels.insert(Labels.end(), QueryLabels.begin(), QueryLabels.end());
     }

     const auto QueryLabelsIt = Request.QueryParams.find("labels");
     if (QueryLabelsIt != Request.QueryParams.end())
     {
          const std::vector<std::string> QueryLabels = SplitManualLabels(QueryLabelsIt->second);
          Labels.insert(Labels.end(), QueryLabels.begin(), QueryLabels.end());
     }

     const std::string Body = TrimCopy(Request.Body);

     if (!Body.empty())
     {
          try
          {
               nlohmann::json Parsed = nlohmann::json::parse(Body);

               if (Parsed.is_object())
               {
                    if (Parsed.contains("label"))
                    {
                         if (Parsed["label"].is_string())
                         {
                              const std::vector<std::string> BodyLabels = SplitManualLabels(Parsed["label"].get<std::string>());
                              Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
                         }
                         else if (!Parsed["label"].is_null())
                         {
                              Labels.push_back(TrimCopy(Parsed["label"].dump()));
                         }
                    }

                    if (Parsed.contains("labels"))
                    {
                         if (Parsed["labels"].is_array())
                         {
                              for (const auto &Item : Parsed["labels"])
                              {
                                   if (Item.is_string())
                                   {
                                        const std::vector<std::string> BodyLabels = SplitManualLabels(Item.get<std::string>());
                                        Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
                                   }
                                   else if (!Item.is_null())
                                   {
                                        Labels.push_back(TrimCopy(Item.dump()));
                                   }
                              }
                         }
                         else if (Parsed["labels"].is_string())
                         {
                              const std::vector<std::string> BodyLabels = SplitManualLabels(Parsed["labels"].get<std::string>());
                              Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
                         }
                    }
               }
               else if (Parsed.is_string())
               {
                    const std::vector<std::string> BodyLabels = SplitManualLabels(Parsed.get<std::string>());
                    Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
               }
               else if (Parsed.is_array())
               {
                    for (const auto &Item : Parsed)
                    {
                         if (Item.is_string())
                         {
                              const std::vector<std::string> BodyLabels = SplitManualLabels(Item.get<std::string>());
                              Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
                         }
                         else if (!Item.is_null())
                         {
                              Labels.push_back(TrimCopy(Item.dump()));
                         }
                    }
               }
          }
          catch (...)
          {
               std::string Text = Body;
               const size_t ColonPos = Text.find(':');
               const size_t EqualsPos = Text.find('=');

               if (ColonPos != std::string::npos &&
                   ToLowerCopy(TrimCopy(Text.substr(0, ColonPos))) == "label")
               {
                    Text = Text.substr(ColonPos + 1);
               }
               else if (EqualsPos != std::string::npos &&
                        ToLowerCopy(TrimCopy(Text.substr(0, EqualsPos))) == "label")
               {
                    Text = Text.substr(EqualsPos + 1);
               }

               const std::vector<std::string> BodyLabels = SplitManualLabels(Text);
               Labels.insert(Labels.end(), BodyLabels.begin(), BodyLabels.end());
          }
     }

     std::vector<std::string> CleanLabels;
     std::unordered_set<std::string> Seen;

     for (const std::string &LabelValue : Labels)
     {
          const std::string Label = TrimCopy(LabelValue);
          const std::string Key = ToLowerCopy(Label);

          if (!Label.empty() && Seen.insert(Key).second)
          {
               CleanLabels.push_back(Label);
          }
     }

     return CleanLabels;
}

/* HandleListDocuments lists documents in a collection with pagination and sorting. */

HttpResponse SearchAPI::HandleListDocuments(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleListDocuments: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     int OffsetVal = 0;
     int LimitVal = 100;
     bool IncludeCreatedAtVal = true;
     std::string SortByStr = "";

     auto OffsetIt = Request.QueryParams.find("offset");

     if (OffsetIt != Request.QueryParams.end())
     {
          try
          {
               OffsetVal = std::stoi(OffsetIt->second);

               if (OffsetVal < 0)
               {
                    OffsetVal = 0;
               }
          }
          catch (...)
          {
          }
     }

     auto LimitIt = Request.QueryParams.find("limit");

     if (LimitIt != Request.QueryParams.end())
     {
          try
          {
               LimitVal = std::stoi(LimitIt->second);

               if (LimitVal < 1)
               {
                    LimitVal = 1;
               }

               if (LimitVal > 1000)
               {
                    LimitVal = 1000;
               }
          }
          catch (...)
          {
          }
     }

     auto SortByIt = Request.QueryParams.find("sort_by");

     if (SortByIt != Request.QueryParams.end())
     {
          SortByStr = SortByIt->second;
     }
     else
     {
          std::vector<std::string> DefaultSortBy = ResolveDefaultCollectionSortBy(CollectionName);

          if (!DefaultSortBy.empty())
          {
               SortByStr = DefaultSortBy.front();
          }
     }

     auto IncludeDateIt = Request.QueryParams.find("include_created_at");

     if (IncludeDateIt != Request.QueryParams.end())
     {
          std::string Value = IncludeDateIt->second;

          std::transform(Value.begin(), Value.end(), Value.begin(), ::tolower);

          IncludeCreatedAtVal = (Value == "true" || Value == "1" || Value == "yes");
     }

     const bool RequiresGlobalSort = !SortByStr.empty();
     std::vector<Document> Documents;
     const auto AppendStorageDocuments = [&Documents](const std::vector<Document> &StorageDocs)
     {
          Documents.reserve(Documents.size() + StorageDocs.size());

          for (const auto &StorageDoc : StorageDocs)
          {
               Document DocObj;

               DocObj.ID = StorageDoc.ID;
               DocObj.Title = StorageDoc.Title;
               DocObj.Content = StorageDoc.Content;
               DocObj.Fields = StorageDoc.Fields;
               DocObj.Score = StorageDoc.Score;
               DocObj.Timestamp = StorageDoc.Timestamp;

               Documents.push_back(DocObj);
          }
     };

     try
     {
          Documents.clear();

          if (RequiresGlobalSort)
          {
               const int BatchLimit = 1000;
               int BatchOffset = 0;

               while (true)
               {
                    const auto StorageDocs = HybridStorageManagerInstance().ListDocuments(CollectionName, BatchLimit, BatchOffset);

                    if (StorageDocs.empty())
                    {
                         break;
                    }

                    AppendStorageDocuments(StorageDocs);

                    if (static_cast<int>(StorageDocs.size()) < BatchLimit)
                    {
                         break;
                    }

                    BatchOffset += static_cast<int>(StorageDocs.size());
               }
          }
          else
          {
               const auto StorageDocs = HybridStorageManagerInstance().ListDocuments(CollectionName, LimitVal, OffsetVal);
               AppendStorageDocuments(StorageDocs);
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception listing documents: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to list documents: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception listing documents.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while listing documents\"}";

          return Response;
     }

     int TotalVal = 0;

     try
     {
          TotalVal = static_cast<int>(HybridStorageManagerInstance().GetCollectionDocumentCount(CollectionName));

          if (TotalVal == 0 && !Documents.empty())
          {
               TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "HandleListDocuments: Metadata says 0 docs but found " + std::to_string(Documents.size()) + " documents - metadata is wrong, using estimate.");
               }
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception getting document count: " + std::string(E.what()) + " - using Documents.size() as fallback.");
          }

          TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception getting document count - using Documents.size() as fallback.");
          }

          TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));
     }

     if (Documents.empty() && OffsetVal >= TotalVal)
     {
          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"documents\":[],\"total\":" + std::to_string(TotalVal) + "}";

          return Response;
     }

     if (!SortByStr.empty())
     {
          std::string FieldNameVal = SortByStr;
          bool DescendingVal = false;

          if (SortByStr.find(":desc") != std::string::npos)
          {
               FieldNameVal = SortByStr.substr(0, SortByStr.find(":desc"));
               DescendingVal = true;
          }
          else if (SortByStr.find(":asc") != std::string::npos)
          {
               FieldNameVal = SortByStr.substr(0, SortByStr.find(":asc"));
               DescendingVal = false;
          }

          std::sort(Documents.begin(), Documents.end(), [&FieldNameVal, DescendingVal](const Document &A, const Document &B)
                    {
                         std::string AValue;
                         std::string BValue;

                         if (FieldNameVal == "id")
                         {
                              AValue = A.ID;
                              BValue = B.ID;
                         }
                         else if (FieldNameVal == "title")
                         {
                              AValue = A.Title;
                              BValue = B.Title;
                         }
                         else if (FieldNameVal == "created_at" || FieldNameVal == "timestamp")
                         {
                              long long ATimestamp = A.Timestamp;
                              long long BTimestamp = B.Timestamp;

                              if (ATimestamp != BTimestamp)
                              {
                                   return DescendingVal ? ATimestamp > BTimestamp : ATimestamp < BTimestamp;
                              }

                              int IDCmp = CompareNaturalString(A.ID, B.ID);
                              if (IDCmp != 0)
                              {
                                   return IDCmp < 0;
                              }

                              return CompareNaturalString(A.Title, B.Title) < 0;
                         }
                         else
                         {
                              AValue = A.Fields.count(FieldNameVal) ? A.Fields.at(FieldNameVal) : "";
                              BValue = B.Fields.count(FieldNameVal) ? B.Fields.at(FieldNameVal) : "";
                         }

                         if (AValue != BValue)
                         {
                              auto ParseDateFunc = [](const std::string &DateStr, std::chrono::system_clock::time_point &TP) -> bool
                              {
                                   if (DateStr.empty())
                                   {
                                        return false;
                                   }

                                   struct tm TMStruct = {};
                                   std::istringstream SS(DateStr);

                                   SS >> std::get_time(&TMStruct, "%Y-%m-%dT%H:%M:%S");

                                   if (SS.fail())
                                   {
                                        SS.clear();
                                        SS.str(DateStr);
                                        SS >> std::get_time(&TMStruct, "%Y-%m-%d %H:%M:%S");

                                        if (SS.fail())
                                        {
                                             return false;
                                        }
                                   }

                                   TP = std::chrono::system_clock::from_time_t(std::mktime(&TMStruct));

                                   return true;
                              };

                              std::chrono::system_clock::time_point TP1;
                              std::chrono::system_clock::time_point TP2;

                              if (ParseDateFunc(AValue, TP1) && ParseDateFunc(BValue, TP2))
                              {
                                   if (TP1 < TP2)
                                   {
                                        return !DescendingVal;
                                   }

                                   if (TP1 > TP2)
                                   {
                                        return DescendingVal;
                                   }

                                   return false;
                              }

                              try
                              {
                                   double ANum = std::stod(AValue);
                                   double BNum = std::stod(BValue);

                                   if (ANum != BNum)
                                   {
                                        return DescendingVal ? ANum > BNum : ANum < BNum;
                                   }
                              }
                              catch (...)
                              {
                                   int Cmp = CompareNaturalString(AValue, BValue);
                                   if (Cmp != 0)
                                   {
                                        return DescendingVal ? (Cmp > 0) : (Cmp < 0);
                                   }
                              }
                         }

                         int IDCmp = CompareNaturalString(A.ID, B.ID);
                         if (IDCmp != 0)
                         {
                              return IDCmp < 0;
                         }

                         return CompareNaturalString(A.Title, B.Title) < 0;
                    });

          if (OffsetVal > 0 || static_cast<int>(Documents.size()) > LimitVal)
          {
               const size_t SliceStart = static_cast<size_t>(std::min(OffsetVal, static_cast<int>(Documents.size())));
               const size_t SliceEnd = std::min(Documents.size(), SliceStart + static_cast<size_t>(LimitVal));

               if (SliceStart >= Documents.size())
               {
                    Documents.clear();
               }
               else
               {
                    Documents = std::vector<Document>(Documents.begin() + static_cast<std::ptrdiff_t>(SliceStart),
                                                      Documents.begin() + static_cast<std::ptrdiff_t>(SliceEnd));
               }
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"documents\":[";

     for (size_t I = 0; I < Documents.size(); ++I)
     {
          if (I > 0)
          {
               Response.Body += ",";
          }

          Response.Body += "{";
          Response.Body += "\"id\":\"" + EscapeJSONString(Documents[I].ID) + "\"";

          if (!Documents[I].Title.empty())
          {
               Response.Body += ",\"title\":\"" + EscapeJSONString(Documents[I].Title) + "\"";
          }

          if (!Documents[I].Content.empty())
          {
               Response.Body += ",\"content\":\"" + EscapeJSONString(Documents[I].Content) + "\"";
          }

          Response.Body += ",\"score\":" + std::to_string(Documents[I].Score);
          Response.Body += ",\"timestamp\":" + std::to_string(Documents[I].Timestamp);

          if (IncludeCreatedAtVal && Documents[I].Timestamp > 0)
          {
               auto TimePointVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(Documents[I].Timestamp));
               time_t TimeTVal = std::chrono::system_clock::to_time_t(TimePointVal);
               auto MSSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(TimePointVal.time_since_epoch()).count();
               long long MSVal = MSSinceEpoch % 1000;

               struct tm TMBuf;
               struct tm *TM = gmtime_r(&TimeTVal, &TMBuf);

               if (TM)
               {
                    std::ostringstream OSS;

                    OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
                    OSS << '.' << std::setfill('0') << std::setw(3) << MSVal << 'Z';

                    Response.Body += ",\"created_at\":\"" + OSS.str() + "\"";
               }
               else
               {
                    Response.Body += ",\"created_at\":\"" + std::to_string(Documents[I].Timestamp) + "\"";
               }
          }

          for (const auto &Field : Documents[I].Fields)
          {
               if (Field.first == "_collection")
               {
                    continue;
               }

               Response.Body += ",\"" + EscapeJSONString(Field.first) + "\":";

               const std::string &FieldValueVal = Field.second;

               if (!FieldValueVal.empty())
               {
                    char *EndPtr = nullptr;

                    std::strtod(FieldValueVal.c_str(), &EndPtr);

                    bool IsNumberVal = false;

                    if (EndPtr != nullptr && EndPtr == FieldValueVal.c_str() + FieldValueVal.length())
                    {
                         if (!FieldValueVal.empty() && (std::isdigit(FieldValueVal[0]) || FieldValueVal[0] == '+' || FieldValueVal[0] == '-' || FieldValueVal[0] == '.'))
                         {
                              IsNumberVal = true;
                         }
                    }

                    if (IsNumberVal)
                    {
                         Response.Body += FieldValueVal;
                    }
                    else if (FieldValueVal == "true" || FieldValueVal == "false")
                    {
                         Response.Body += FieldValueVal;
                    }
                    else
                    {
                         Response.Body += "\"" + EscapeJSONString(FieldValueVal) + "\"";
                    }
               }
               else
               {
                    Response.Body += "\"\"";
               }
          }

          Response.Body += "}";
     }

     Response.Body += "],\"total\":" + std::to_string(TotalVal) + "}";

     return Response;
}

/* HandleAddDocument adds a single document to a collection. */

HttpResponse SearchAPI::HandleAddDocument(const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleAddDocument ENTRY - method=" + Request.Method + " path=" + Request.Path + ".");
     }

     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (Request.Body.size() > 20 * 1024 * 1024)
     {
          HttpResponse Response(Status::PAYLOAD_TOO_LARGE, StatusText(Status::PAYLOAD_TOO_LARGE), "application/json");

          Response.Body = "{\"error\":\"Document too large\",\"message\":\"Document exceeds maximum size of 20MB\",\"max_size\":\"20MB\"}";

          return Response;
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Collection name extracted: '" + CollectionName + "'.");
     }

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     bool CollectionExistsVal = false;

     try
     {
          CollectionExistsVal = HybridStorageManagerInstance().CollectionExists(CollectionName);
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception checking collection existence for '" + CollectionName + "': " + std::string(E.what()) + " - will attempt to auto-create via AddDocument.");
          }
     }

     if (!CollectionExistsVal && Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Collection '" + CollectionName + "' does not exist - AddDocument will auto-create it.");
     }

     Document DocumentObj;
     std::string ParseErrorStr;

     if (!ParseDocumentFromJSON(Request.Body, DocumentObj, &ParseErrorStr))
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          std::string ErrorDetails = ParseErrorStr.empty() ? "Failed to parse document JSON" : ParseErrorStr;

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(ErrorDetails) + "\",\"details\":\"" + EscapeJSONString(ErrorDetails) + "\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "JSON parse failed: " + ErrorDetails + " (body size: " + std::to_string(Request.Body.size()) + ").");
          }

          ConsoleWriter::WriteError("JSON parse failed: " + ErrorDetails);

          return Response;
     }

     bool HasMeaningfulContent = false;

     if (!DocumentObj.Title.empty() || !DocumentObj.Content.empty())
     {
          HasMeaningfulContent = true;
     }
     else if (!DocumentObj.Fields.empty())
     {
          for (const auto &[Key, Value] : DocumentObj.Fields)
          {
               if (Key != "invalid" && Key != "score" && Key != "timestamp" && !Value.empty())
               {
                    HasMeaningfulContent = true;

                    break;
               }
          }
     }

     if (!HasMeaningfulContent)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document\",\"message\":\"Document must contain at least one meaningful field (title, content, or valid custom field)\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Document validation failed: no meaningful fields.");
          }

          return Response;
     }

     if (DocumentObj.ID.empty())
     {
          std::string AutoID = "doc_" + std::to_string([&]() -> int64_t
                                                       {
                                                            if (auto *Inst = Instance; Inst)
                                                            {
                                                                 return Inst->NowMs();
                                                            }

                                                            return static_cast<int64_t>(NowMs());
                                                       }()) +
                               "_" + std::to_string(rand() % 1000000);

          DocumentObj.ID = AutoID;

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "Auto-generated document ID: " + AutoID + ".");
          }
     }

     if (DocumentObj.ID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID must be a non-empty string\"}";

          return Response;
     }

     if (DocumentObj.ID.size() > 256)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Document ID too long\",\"message\":\"Document ID must be between 1 and 256 characters\",\"max_length\":256}";

          return Response;
     }

     for (char C : DocumentObj.ID)
     {
          if (!std::isalnum(C) && C != '_' && C != '-' && C != '.')
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

               Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID can only contain alphanumeric characters, underscores, hyphens, and dots\"}";

               return Response;
          }
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentObj.ID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpRequest ProxyReq = Request;
               nlohmann::json DocJSON = BuildDocumentJSON(DocumentObj);
               ProxyReq.Body = DocJSON.dump();
               ProxyReq.Headers["Content-Type"] = "application/json";

               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(ProxyReq, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward document to target node." : ProxyError);
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Calling HybridStorageManagerInstance().");
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "[DUPLICATE_CHECK_START] HandleAddDocument: Checking for duplicate - collection='" + CollectionName + "' doc_id='" + DocumentObj.ID + "'.");
     }

     try
     {
          auto &StorageManagerRef = HybridStorageManagerInstance();

          if (Instance && Instance->Database)
          {
               if (CollectionName.empty() || DocumentObj.ID.empty())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "[VALIDATION_FAIL] HandleAddDocument: Empty collection or doc_id - REJECTING.");
                    }

                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Collection name and document ID cannot be empty\"}";

                    return Response;
               }

               std::string DocKey = "doc:" + CollectionName + ":" + DocumentObj.ID;

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "[API_CHECK] Calling Database->Exists() for key: " + DocKey + ".");
               }

               bool DocumentExistsVal = Instance->Database->Exists(DocKey);

               if (DocumentExistsVal)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "[UPSERT] Document ID '" + DocumentObj.ID + "' exists in collection '" + CollectionName + "' - will be updated.");
                    }
               }
               else
               {
                    Instance->Logs->Normal("search_api", "[INSERT] Document ID '" + DocumentObj.ID + "' not found in collection '" + CollectionName + "' - will be inserted.");
               }
          }
          else
          {
               if (StorageManagerRef.CollectionExists(CollectionName))
               {
                    Document ExistingDoc = StorageManagerRef.GetDocument(CollectionName, DocumentObj.ID);

                    if (!ExistingDoc.ID.empty() && ExistingDoc.ID == DocumentObj.ID)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("search_api", "[UPSERT_FALLBACK] Document with ID '" + DocumentObj.ID + "' already exists in collection '" + CollectionName + "' - will be updated.");
                         }
                    }
               }
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception checking for existing document: " + std::string(E.what()) + " - proceeding to storage layer for upsert.");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception checking for existing document - proceeding to storage layer for upsert.");
          }
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreAddDocument, CollectionName, DocumentObj, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "add_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = false;

     try
     {
          auto &StorageManagerRef = HybridStorageManagerInstance();
          Document StorageDoc;

          StorageDoc.ID = DocumentObj.ID;
          StorageDoc.Title = DocumentObj.Title;
          StorageDoc.Content = DocumentObj.Content;
          StorageDoc.Fields = DocumentObj.Fields;
          StorageDoc.Score = DocumentObj.Score;

          if (DocumentObj.Timestamp == 0)
          {
               if (Instance)
               {
                    StorageDoc.Timestamp = Instance->NowMs();
               }
               else
               {
                    StorageDoc.Timestamp = NowMs();
               }
          }
          else
          {
               StorageDoc.Timestamp = DocumentObj.Timestamp;
          }

          SuccessVal = StorageManagerRef.AddDocument(CollectionName, StorageDoc);
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception adding document: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to add document: " + EscapeJSONString(E.what()) + "\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception adding document.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while adding document\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     if (!SuccessVal)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to add document to storage\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "add_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was written locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentObj.ID) + "\"}";
          return JournalResponse;
     }

     FOREACH_MOD(OnAddDocument, CollectionName, DocumentObj.ID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "add_document", &ReplicationError))
     {
          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          Response.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was written locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentObj.ID) + "\"}";
          return Response;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     HttpResponse Response(Status::CREATED, StatusText(Status::CREATED), "application/json");
     nlohmann::json ResultJSON;

     ResultJSON["message"] = "Document added successfully";
     ResultJSON["id"] = DocumentObj.ID;
     ResultJSON["code"] = Code::DOCUMENT_CREATED;
     ResultJSON["code_text"] = CodeText(Code::DOCUMENT_CREATED);

     Response.Body = ResultJSON.dump();

     return Response;
}

/* HandleUpdateDocument updates a document by ID (partial update). */

HttpResponse SearchAPI::HandleUpdateDocument(const HttpRequest &Request)
{
     if (Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID validation failed\"}";

          return Response;
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward update to target node." : ProxyError);
          }
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleDeleteDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     Document ExistingDoc;

     try
     {
          Document StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);

          ExistingDoc.ID = StorageDoc.ID;
          ExistingDoc.Title = StorageDoc.Title;
          ExistingDoc.Content = StorageDoc.Content;
          ExistingDoc.Fields = StorageDoc.Fields;
          ExistingDoc.Score = StorageDoc.Score;
          ExistingDoc.Timestamp = StorageDoc.Timestamp;
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception getting document for update: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to retrieve document: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception getting document for update.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while retrieving document\"}";

          return Response;
     }

     if (ExistingDoc.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     nlohmann::json UpdateJSON;

     try
     {
          UpdateJSON = nlohmann::json::parse(Request.Body);
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }

     Document DocumentObj = ExistingDoc;

     DocumentObj.ID = DocumentID;

     if (UpdateJSON.contains("title"))
     {
          if (UpdateJSON["title"].is_string())
          {
               DocumentObj.Title = UpdateJSON["title"].get<std::string>();
               std::string TitleError;

               if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid title value\",\"message\":\"" + EscapeJSONString(TitleError) + "\"}";

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Title validation failed: " + TitleError + ".");
                    }

                    return Response;
               }
          }
          else if (!UpdateJSON["title"].is_null())
          {
               DocumentObj.Title = UpdateJSON["title"].dump();
               std::string TitleError;

               if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid title value\",\"message\":\"" + EscapeJSONString(TitleError) + "\"}";

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Title validation failed: " + TitleError + ".");
                    }

                    return Response;
               }
          }
          else
          {
               DocumentObj.Title = "";
          }
     }

     if (UpdateJSON.contains("content"))
     {
          if (UpdateJSON["content"].is_string())
          {
               DocumentObj.Content = UpdateJSON["content"].get<std::string>();
               std::string ContentError;

               if (!ValidateFieldValue(DocumentObj.Content, &ContentError, "content"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid content value\",\"message\":\"" + EscapeJSONString(ContentError) + "\"}";

                    return Response;
               }
          }
          else if (!UpdateJSON["content"].is_null())
          {
               DocumentObj.Content = UpdateJSON["content"].dump();
               std::string ContentError;

               if (!ValidateFieldValue(DocumentObj.Content, &ContentError, "content"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid content value\",\"message\":\"" + EscapeJSONString(ContentError) + "\"}";

                    return Response;
               }
          }
          else
          {
               DocumentObj.Content = "";
          }
     }

     for (const auto &[Key, Value] : UpdateJSON.items())
     {
          if (Key == "id" || Key == "title" || Key == "content")
          {
               continue;
          }

          if (Value.is_null())
          {
               DocumentObj.Fields.erase(Key);
          }
          else if (Value.is_string())
          {
               std::string Val = Value.get<std::string>();
               std::string FieldError;

               if (!ValidateFieldValue(Val, &FieldError, Key))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid field value for '" + Key + "'\",\"message\":\"" + EscapeJSONString(FieldError) + "\"}";

                    return Response;
               }

               DocumentObj.Fields[Key] = Val;
          }
          else
          {
               DocumentObj.Fields[Key] = Value.dump();
          }
     }

     Document StorageDoc;

     StorageDoc.ID = DocumentObj.ID;
     StorageDoc.Title = DocumentObj.Title;
     StorageDoc.Content = DocumentObj.Content;
     StorageDoc.Fields = DocumentObj.Fields;
     StorageDoc.Score = DocumentObj.Score;
     StorageDoc.Timestamp = Instance ? Instance->NowMs() : static_cast<uint64_t>(time(nullptr) * 1000);

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateDocument, CollectionName, DocumentObj, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "update_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().AddDocument(CollectionName, StorageDoc);

     if (!SuccessVal)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to update document in storage\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "update_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was updated locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Document updated successfully\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
     FOREACH_MOD(OnUpdateDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "update_document", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was updated locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleDeleteDocument deletes a single document by ID. */

HttpResponse SearchAPI::HandleDeleteDocument(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward delete to target node." : ProxyError);
          }
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleDeleteDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "delete_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().DeleteDocument(CollectionName, DocumentID);

     if (!SuccessVal)
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was deleted locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"deleted\":true,\"message\":\"Document deleted successfully\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
     FOREACH_MOD(OnDeleteDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;

     if (!ReplicateWriteRequest(Request, "delete_document", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was deleted locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleDeleteDocumentsByFilter deletes multiple documents matching a filter. */

HttpResponse SearchAPI::HandleDeleteDocumentsByFilter(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteDocuments, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::unordered_map<std::string, std::string> SearchParams;

     if (!Request.QueryParams.empty())
     {
          SearchParams.insert(Request.QueryParams.begin(), Request.QueryParams.end());
     }

     if (!Request.Body.empty())
     {
          try
          {
               nlohmann::json RequestJSON = nlohmann::json::parse(Request.Body);

               if (RequestJSON.contains("query"))
               {
                    if (RequestJSON["query"].is_string())
                    {
                         SearchParams["q"] = RequestJSON["query"].get<std::string>();
                    }
                    else if (RequestJSON["query"].is_object())
                    {
                         if (RequestJSON["query"].contains("q") && RequestJSON["query"]["q"].is_string())
                         {
                              SearchParams["q"] = RequestJSON["query"]["q"].get<std::string>();
                         }

                         if (RequestJSON["query"].contains("filter_by") && RequestJSON["query"]["filter_by"].is_string())
                         {
                              SearchParams["filter_by"] = RequestJSON["query"]["filter_by"].get<std::string>();
                         }
                    }
               }
          }
          catch (const std::exception &)
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }
     }

     ComprehensiveSearchQuery SearchQueryObj = ParseComprehensiveSearchQuery(SearchParams);
     SearchQueryObj.PerPage = std::min(10000, std::max(1, SearchQueryObj.PerPage));

     ComprehensiveSearchResult SearchResultVal = PerformComprehensiveSearch(CollectionName, SearchQueryObj);

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!SearchResultVal.Hits.empty() &&
         !PrepareReplicationOutboxRecord(Request, "delete_documents", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     int DeletedVal = 0;
     int FailedVal = 0;

     for (const auto &HitObj : SearchResultVal.Hits)
     {
          if (!HitObj.Document.count("id"))
          {
               continue;
          }

          std::string DocID = HitObj.Document.at("id");

          if (HybridStorageManagerInstance().DeleteDocument(CollectionName, DocID))
          {
               DeletedVal++;
          }
          else
          {
               FailedVal++;
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json ResultJSON;

     ResultJSON["deleted"] = DeletedVal;
     ResultJSON["failed"] = FailedVal;
     ResultJSON["total"] = static_cast<int>(SearchResultVal.Hits.size());

     Response.Body = ResultJSON.dump();
     if (DeletedVal > 0)
     {
          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_documents", &ReplicationJournalError))
          {
               HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Delete-by-filter was applied locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\"}";
               return JournalResponse;
          }

          FOREACH_MOD(OnDeleteDocuments, CollectionName, static_cast<uint64_t>(DeletedVal), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          BumpCollectionMutationVersion(CollectionName);

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "delete_documents", &ReplicationError))
          {
               HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Delete-by-filter was applied locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\"}";
               return ReplicationResponse;
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
     }
     else
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
     }

     return Response;
}

/* HandleGetDocument gets a single document by its ID. */

HttpResponse SearchAPI::HandleGetDocument(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID validation failed\"}";

          return Response;
     }

     auto RouteIt = Request.QueryParams.find("route");
     const bool HasRoute = (RouteIt != Request.QueryParams.end() && !RouteIt->second.empty());
     if (HasRoute)
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;
          if (!ResolveDistributedRoute(RouteIt->second, &TargetHost, &TargetPort, &IsLocal))
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Invalid distributed route target.",
                                         "Use route=local or route=<host[:port]> for a configured distributed node.");
          }

          if (!IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Routed document fetch unavailable.",
                                         ProxyError.empty() ? "Failed to forward document request to the routed node." : ProxyError);
          }
     }

     bool CollectionExistsLocally = HybridStorageManagerInstance().CollectionExists(CollectionName);
     Document DocObj;
     bool LocalDocLoaded = false;

     try
     {
          Document StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);

          if (!StorageDoc.ID.empty())
          {
               DocObj.ID = StorageDoc.ID;
               DocObj.Title = StorageDoc.Title;
               DocObj.Content = StorageDoc.Content;
               DocObj.Fields = StorageDoc.Fields;
               DocObj.Score = StorageDoc.Score;
               DocObj.Timestamp = StorageDoc.Timestamp;
               LocalDocLoaded = true;
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception in GetDocument: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to retrieve document: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception in GetDocument.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while retrieving document\"}";

          return Response;
     }

     if (!HasRoute && !LocalDocLoaded && ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "HandleGetDocument: distributed fetch failed for '" + DocumentID + "' on " + TargetHost + ":" + std::to_string(TargetPort) + ", falling back to local lookup. Error: " + (ProxyError.empty() ? std::string("unknown proxy error") : ProxyError) + ".");
               }
          }
     }

     if (!CollectionExistsLocally)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleGetDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleGetDocument: Collection found, calling GetDocument('" + CollectionName + "', '" + DocumentID + "').");
     }

     if (DocObj.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     bool IncludeCreatedAtVal = true;
     auto IncludeDateIt = Request.QueryParams.find("include_created_at");

     if (IncludeDateIt != Request.QueryParams.end())
     {
          std::string Value = IncludeDateIt->second;

          std::transform(Value.begin(), Value.end(), Value.begin(), ::tolower);

          IncludeCreatedAtVal = (Value == "true" || Value == "1" || Value == "yes");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{";
     Response.Body += "\"id\":\"" + EscapeJSONString(DocObj.ID) + "\"";

     if (!DocObj.Title.empty())
     {
          Response.Body += ",\"title\":\"" + EscapeJSONString(DocObj.Title) + "\"";
     }

     if (!DocObj.Content.empty())
     {
          Response.Body += ",\"content\":\"" + EscapeJSONString(DocObj.Content) + "\"";
     }

     if (IncludeCreatedAtVal && DocObj.Timestamp > 0)
     {
          auto TimePointVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(DocObj.Timestamp));
          time_t TimeTVal = std::chrono::system_clock::to_time_t(TimePointVal);
          auto MSSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(TimePointVal.time_since_epoch()).count();
          long long MSVal = MSSinceEpoch % 1000;

          struct tm TMBuf;
          struct tm *TM = gmtime_r(&TimeTVal, &TMBuf);

          if (TM)
          {
               std::ostringstream OSS;

               OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
               OSS << '.' << std::setfill('0') << std::setw(3) << MSVal << 'Z';

               Response.Body += ",\"created_at\":\"" + OSS.str() + "\"";
          }
          else
          {
               Response.Body += ",\"created_at\":\"" + std::to_string(DocObj.Timestamp) + "\"";
          }
     }

     for (const auto &Field : DocObj.Fields)
     {
          Response.Body += ",\"" + EscapeJSONString(Field.first) + "\":";

          const std::string &FieldValueVal = Field.second;

          if (!FieldValueVal.empty())
          {
               char *EndPtr = nullptr;

               std::strtod(FieldValueVal.c_str(), &EndPtr);

               bool IsNumberVal = false;

               if (EndPtr != nullptr && EndPtr == FieldValueVal.c_str() + FieldValueVal.length())
               {
                    if (!FieldValueVal.empty() && (std::isdigit(FieldValueVal[0]) || FieldValueVal[0] == '+' || FieldValueVal[0] == '-' || FieldValueVal[0] == '.'))
                    {
                         IsNumberVal = true;
                    }
               }

               if (IsNumberVal)
               {
                    Response.Body += FieldValueVal;
               }
               else if (FieldValueVal == "true" || FieldValueVal == "false")
               {
                    Response.Body += FieldValueVal;
               }
               else
               {
                    Response.Body += "\"" + EscapeJSONString(FieldValueVal) + "\"";
               }
          }
          else
          {
               Response.Body += "\"\"";
          }
     }

     Response.Body += "}";

     return Response;
}

HttpResponse SearchAPI::HandleGetDocumentContext(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     const std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::DOCUMENT_INVALID_ID,
                                    "Invalid document ID",
                                    "Document ID validation failed.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     Document DocObj;

     try
     {
          DocObj = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);
     }
     catch (const std::exception& E)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Internal server error",
                                    "Failed to retrieve document context: " + std::string(E.what()) + ".");
     }
     catch (...)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Internal server error",
                                    "Unknown error occurred while retrieving document context.");
     }

     if (DocObj.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["id"] = DocumentID;

     bool Pending = false;
     std::vector<llm::ContextSuggestion> Suggestions;

     if (Instance && Instance->LLM)
     {
          Suggestions = Instance->LLM->GetDocumentContext(CollectionName, DocumentID, &Pending);

          if (Suggestions.empty())
          {
               Suggestions = Instance->LLM->BuildDocumentContext(CollectionName, DocObj, 5);
               Instance->LLM->StoreDocumentContext(CollectionName, DocumentID, Suggestions);
          }
     }

     Root["pending"] = Pending;
     Root["count"] = Suggestions.size();
     Root["suggestions"] = nlohmann::json::array();

     for (const auto& Suggestion : Suggestions)
     {
          Root["suggestions"].push_back({
               {"text", Suggestion.Text},
               {"kind", Suggestion.Kind},
               {"relation", Suggestion.Relation},
               {"confidence", Suggestion.Confidence},
               {"evidence", Suggestion.Evidence},
               {"scope", Suggestion.Scope},
               {"provisional", Suggestion.Provisional}
          });
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMRebuild(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";

     if (CollectionName.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing collection",
                                    "Query parameter 'collection' is required.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     std::string ErrorMessage;
     bool AlreadyRunning = false;

     if (!Instance->Sam->StartRecreateCollectionAsync(CollectionName, &AlreadyRunning, &ErrorMessage))
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM rebuild failed",
                                    ErrorMessage.empty() ? std::string("Unable to start SAM rebuild.") : ErrorMessage);
     }

     FOREACH_MOD(OnSamIndexing,
                 CollectionName,
                 "rebuild",
                 AlreadyRunning,
                 Request.RemoteAddress,
                 Request.APIKeyID,
                 !Request.APIKeyID.empty());

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["started"] = !AlreadyRunning;
     Root["running"] = true;
     Root["message"] = AlreadyRunning ? "SAM rebuild already running" : "SAM rebuild started";

     HttpResponse Response(AlreadyRunning ? Status::OK : Status::ACCEPTED,
                           StatusText(AlreadyRunning ? Status::OK : Status::ACCEPTED),
                           "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMSearch(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const auto QueryIt = Request.QueryParams.find("q");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";
     const std::string Query = (QueryIt != Request.QueryParams.end()) ? TrimCopy(QueryIt->second) : "";
     const auto AllIt = Request.QueryParams.find("all");
     const auto CollectionsIt = Request.QueryParams.find("collections");
     const bool SearchAll = (AllIt != Request.QueryParams.end() && IsTruthyToken(AllIt->second)) ||
                            (CollectionName.empty() && CollectionsIt != Request.QueryParams.end() &&
                             !TrimCopy(CollectionsIt->second).empty());

     if (Query.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing SAM search parameters",
                                    SearchAll
                                         ? "Query parameter 'q' is required."
                                         : "Query parameters 'collection' and 'q' are required.");
     }

     if (!SearchAll && CollectionName.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing SAM search parameters",
                                    "Query parameters 'collection' and 'q' are required.");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     int LimitVal = 100;

      if (!ParseNonNegativeIntParam(Request.QueryParams, "limit", 100, LimitVal) || LimitVal <= 0)
      {
           return BuildErrorResponse(Status::BAD_REQUEST,
                                     Code::SEARCH_INVALID_PARAMETER,
                                     "Invalid limit",
                                     "Query parameter 'limit' must be a positive integer.");
      }

     const bool Distributed = ShouldAttemptDistributedSearch(Request);
     const bool IncludeExplain = Instance->Config && Instance->Config->GetSam25DebugExplain();
     const std::string CacheCollection = SearchAll ? std::string("*") : CollectionName;

     HttpResponse CachedResponse;
     if (SearchResponseCache::Get("sam", Request, CacheCollection, CachedResponse))
     {
          return CachedResponse;
     }

     struct SAMCoreQueryPlan
     {
          bool Active = false;
          std::string LookupQuery;
          std::unordered_set<std::string> AllowedDocumentIDs;
          std::vector<SAM::LookupHit> FallbackHits;
     };

     auto QueryUsesCoreSyntax = [](const std::string &QueryText) -> bool
     {
          if (QueryText.empty())
          {
               return false;
          }

          if (QueryText.find_first_of(":~^[]{}!*?") != std::string::npos)
          {
               return true;
          }

          std::istringstream Stream(QueryText);
          std::string Token;

          while (Stream >> Token)
          {
               std::string Lower = ToLowerCopy(Token);

               if (Lower == "and" || Lower == "or" || Lower == "not" || Lower == "to")
               {
                    return true;
               }
          }

          return false;
     };

     auto StripSAMCoreSyntaxForLookup = [](const std::string &QueryText) -> std::string
     {
          std::stringstream Stream(QueryText);
          std::string Token;
          std::vector<std::string> Terms;
          bool SkipNext = false;
          bool SkippingRange = false;

          while (Stream >> Token)
          {
               std::string Lower = ToLowerCopy(Token);

               if (SkippingRange)
               {
                    if (Token.find(']') != std::string::npos || Token.find('}') != std::string::npos)
                    {
                         SkippingRange = false;
                    }

                    continue;
               }

               if (Lower == "and" || Lower == "or" || Lower == "to")
               {
                    continue;
               }

               if (Lower == "not")
               {
                    SkipNext = true;
                    continue;
               }

               if (Lower == "is:casesensitive" || Lower == "is:case_sensitive" || Lower == "is:case-sensitive" ||
                   Lower == "do:casesensitive" || Lower == "do:case_sensitive" || Lower == "do:case-sensitive")
               {
                    continue;
               }

               bool Prohibited = false;
               while (!Token.empty() && (Token.front() == '!' || Token.front() == '-' || Token.front() == '+'))
               {
                    if (Token.front() == '!' || Token.front() == '-')
                    {
                         Prohibited = true;
                    }

                    Token.erase(0, 1);
               }

               if (SkipNext || Prohibited)
               {
                    SkipNext = false;
                    continue;
               }

               const size_t ColonPos = Token.find(':');
               if (ColonPos != std::string::npos)
               {
                    const std::string Value = Token.substr(ColonPos + 1);
                    if (!Value.empty() && (Value.front() == '[' || Value.front() == '{'))
                    {
                         if (Value.find(']') == std::string::npos && Value.find('}') == std::string::npos)
                         {
                              SkippingRange = true;
                         }

                         continue;
                    }

                    Token = Value;
               }

               const size_t BoostPos = Token.find('^');
               if (BoostPos != std::string::npos)
               {
                    Token = Token.substr(0, BoostPos);
               }

               const size_t FuzzyPos = Token.find('~');
               if (FuzzyPos != std::string::npos)
               {
                    Token = Token.substr(0, FuzzyPos);
               }

               Token = TrimCopy(Token);
               while (!Token.empty() && (Token.front() == '"' || Token.front() == '\''))
               {
                    Token.erase(0, 1);
               }

               while (!Token.empty() && (Token.back() == '"' || Token.back() == '\'' ||
                                         Token.back() == ')' || Token.back() == '('))
               {
                    Token.pop_back();
               }

               if (!Token.empty())
               {
                    Terms.push_back(Token);
               }
          }

          std::ostringstream Lookup;
          for (size_t Index = 0; Index < Terms.size(); ++Index)
          {
               if (Index > 0)
               {
                    Lookup << ' ';
               }

               Lookup << Terms[Index];
          }

          return Lookup.str();
     };

     auto BuildCoreCompatibleSAMHit = [](const std::string &Collection,
                                         const std::string &MatchedTerm,
                                         const SearchHit &CoreHit) -> SAM::LookupHit
     {
          SAM::LookupHit Hit;
          Hit.Collection = Collection;

          auto IDIt = CoreHit.Document.find("id");
          if (IDIt != CoreHit.Document.end())
          {
               Hit.DocumentID = IDIt->second;
          }

          auto TitleIt = CoreHit.Document.find("title");
          if (TitleIt != CoreHit.Document.end())
          {
               Hit.Title = TitleIt->second;
          }

          Hit.MatchedTerm = MatchedTerm;
          Hit.MatchedKind = "core_search";
          Hit.MatchedSource = "core_search";
          Hit.TermOrigin = "core_search";
          Hit.MatchedPath = "core_search_compat";
          Hit.EvidenceCount = 1;

          const double CoreScore = std::max<double>(0.05,
               std::max({static_cast<double>(CoreHit.HybridScore),
                         static_cast<double>(CoreHit.VectorScore),
                         static_cast<double>(CoreHit.TextMatch)}));
          Hit.MatchedScore = CoreScore;
          Hit.MatchedSignal = 1.0;
          Hit.Breakdown.SourceDocScore = CoreScore;
          Hit.Breakdown.FinalScore = CoreScore;
          return Hit;
     };

     auto BuildSAMCoreQueryPlan = [&](const std::string &Collection,
                                      const std::string &RawQuery,
                                      size_t Limit) -> SAMCoreQueryPlan
     {
          SAMCoreQueryPlan Plan;

          if (Collection.empty() || !QueryUsesCoreSyntax(RawQuery))
          {
               return Plan;
          }

          Plan.Active = true;
          Plan.LookupQuery = StripSAMCoreSyntaxForLookup(RawQuery);

          std::unordered_map<std::string, std::string> Params;
          Params["q"] = RawQuery;
          Params["limit"] = std::to_string(std::max<size_t>(Limit, 1000));
          Params["exhaustive_search"] = "true";

          ComprehensiveSearchQuery CoreQuery = ParseComprehensiveSearchQuery(Params);
          CoreQuery.EnableAnalytics = false;
          CoreQuery.PreserveMatchedHits = true;
          CoreQuery.PerPage = std::max<int>(CoreQuery.PerPage, static_cast<int>(std::min<size_t>(1000, std::max<size_t>(Limit, 1000))));

          ComprehensiveSearchResult CoreResult = PerformComprehensiveSearch(Collection, CoreQuery);
          const std::string MatchedTerm = Plan.LookupQuery.empty() ? RawQuery : Plan.LookupQuery;

          for (const auto &CoreHit : CoreResult.Hits)
          {
               auto IDIt = CoreHit.Document.find("id");
               if (IDIt == CoreHit.Document.end() || IDIt->second.empty())
               {
                    continue;
               }

               if (!Plan.AllowedDocumentIDs.insert(IDIt->second).second)
               {
                    continue;
               }

               if (Plan.FallbackHits.size() < Limit)
               {
                    Plan.FallbackHits.push_back(BuildCoreCompatibleSAMHit(Collection, MatchedTerm, CoreHit));
               }
          }

          return Plan;
     };

     auto ApplySAMCoreQueryPlan = [](std::vector<SAM::LookupHit> &Hits,
                                     const SAMCoreQueryPlan &Plan,
                                     size_t Limit)
     {
          if (!Plan.Active)
          {
               return;
          }

          std::unordered_set<std::string> Seen;
          std::vector<SAM::LookupHit> Filtered;
          Filtered.reserve(Hits.size() + Plan.FallbackHits.size());

          for (const auto &Hit : Hits)
          {
               if (Plan.AllowedDocumentIDs.find(Hit.DocumentID) == Plan.AllowedDocumentIDs.end())
               {
                    continue;
               }

               if (!Seen.insert(Hit.DocumentID).second)
               {
                    continue;
               }

               Filtered.push_back(Hit);
          }

          for (const auto &Hit : Plan.FallbackHits)
          {
               if (Limit > 0 && Filtered.size() >= Limit)
               {
                    break;
               }

               if (!Seen.insert(Hit.DocumentID).second)
               {
                    continue;
               }

               Filtered.push_back(Hit);
          }

          Hits.swap(Filtered);
     };

     auto BuildSAMIndexingJSON = [&]()
     {
          nlohmann::json StatusJSON;
          StatusJSON["known"] = false;
          StatusJSON["running"] = false;
          StatusJSON["completed"] = false;
          StatusJSON["needs_retry"] = false;
          StatusJSON["retry_scheduled"] = false;
          StatusJSON["pending"] = 0;
          StatusJSON["indexed"] = 0;
          StatusJSON["failed"] = 0;
          StatusJSON["total"] = 0;
          StatusJSON["active_search_count"] = 0;
          StatusJSON["search_running"] = false;

          if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen() || CollectionName.empty())
          {
               return StatusJSON;
          }

          SAM::CollectionJobStatus JobStatus;
          const bool HasJobStatus = Instance->Sam->GetCollectionJobStatus(CollectionName, JobStatus);
          const std::vector<SAM::SearchActivityEntry> ActiveSearches =
               Instance->Sam->GetActiveSearchActivities(CollectionName);

          StatusJSON["known"] = HasJobStatus;
          StatusJSON["active_search_count"] = ActiveSearches.size();
          StatusJSON["search_running"] = !ActiveSearches.empty();

          if (HasJobStatus)
          {
               StatusJSON["running"] = JobStatus.Running;
               StatusJSON["completed"] = JobStatus.Completed;
               StatusJSON["needs_retry"] = JobStatus.NeedsRetry;
               StatusJSON["retry_scheduled"] = JobStatus.RetryScheduled;
               StatusJSON["pending"] = JobStatus.PendingDocuments;
               StatusJSON["indexed"] = JobStatus.IndexedDocuments;
               StatusJSON["failed"] = JobStatus.FailedDocuments;
               StatusJSON["total"] = JobStatus.TotalDocuments;
               StatusJSON["error"] = JobStatus.ErrorMessage;
               StatusJSON["source"] = JobStatus.Source;
          }
          else
          {
               StatusJSON["needs_retry"] = Instance->Sam->HasPendingCollectionRebuild(CollectionName);
          }

          return StatusJSON;
     };

     auto BuildResponse = [&](const std::string &CollectionLabel,
                              const std::vector<SAM::LookupHit> &Hits,
                              const std::string &ExecutionMode,
                              const std::vector<std::string> &Collections = std::vector<std::string>())
     {
          const nlohmann::json SAMIndexingJSON = BuildSAMIndexingJSON();
          const bool SAMIndexingInProgress =
               SAMIndexingJSON.value("running", false) ||
               SAMIndexingJSON.value("retry_scheduled", false);
          nlohmann::json Root;
          Root["ok"] = true;
          Root["collection"] = CollectionLabel;
          Root["query"] = Query;
          Root["count"] = Hits.size();
          Root["indexing_in_progress"] = SAMIndexingInProgress;
          Root["sam_indexing"] = SAMIndexingJSON;
          Root["execution_mode"] = ExecutionMode;
          Root["hits"] = nlohmann::json::array();

          if (!Collections.empty())
          {
               Root["collections"] = Collections;
          }

          for (const auto &Hit : Hits)
          {
               Root["hits"].push_back(BuildSAMHitJSON(Hit, IncludeExplain));
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
          Response.Headers["X-HLQ-Execution-Mode"] = ExecutionMode;
          Response.Body = Root.dump();
          SearchResponseCache::Put("sam", Request, CacheCollection, Response);
          return Response;
     };

     if (SearchAll)
     {
          std::vector<std::string> TargetCollections;

          if (CollectionsIt != Request.QueryParams.end())
          {
               TargetCollections = ParseCommaSeparatedSAMValues(CollectionsIt->second);
          }

          std::vector<std::string> CollectionsToSearch;
          std::unordered_set<std::string> SeenCollections;

          if (Distributed)
          {
               HttpRequest CollectionsRequest = Request;
               CollectionsRequest.Method = "GET";
               CollectionsRequest.Path = "/collections/distributed";
               CollectionsRequest.Body.clear();

               HttpResponse CollectionsResponse = HandleListCollectionsDistributed(CollectionsRequest);

               if (CollectionsResponse.StatusCode < 200 || CollectionsResponse.StatusCode >= 300)
               {
                    return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                              Code::SEARCH_INVALID_PARAMETER,
                                              "SAM distributed search unavailable",
                                              "Unable to enumerate distributed collections for SAM search.");
               }

               try
               {
                    const nlohmann::json CollectionsRoot = nlohmann::json::parse(CollectionsResponse.Body);

                    if (CollectionsRoot.contains("collections") && CollectionsRoot["collections"].is_array())
                    {
                         for (const auto &Entry : CollectionsRoot["collections"])
                         {
                              const std::string Name = TrimCopy(Entry.value("name", ""));

                              if (!Name.empty() && SeenCollections.insert(Name).second)
                              {
                                   CollectionsToSearch.push_back(Name);
                              }
                         }
                    }
               }
               catch (const std::exception &)
               {
                    return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                              Code::SEARCH_INVALID_PARAMETER,
                                              "SAM distributed search unavailable",
                                              "Failed to parse distributed collection list for SAM search.");
               }
          }
          else
          {
               for (const auto &Name : HybridStorageManagerInstance().ListCollections())
               {
                    if (!Name.empty() && SeenCollections.insert(Name).second)
                    {
                         CollectionsToSearch.push_back(Name);
                    }
               }
          }

          if (!TargetCollections.empty())
          {
               std::unordered_set<std::string> Allowed(TargetCollections.begin(), TargetCollections.end());
               CollectionsToSearch.erase(std::remove_if(CollectionsToSearch.begin(), CollectionsToSearch.end(),
                                                        [&](const std::string &Name)
                                                        {
                                                             return Allowed.find(Name) == Allowed.end();
                                                        }),
                                         CollectionsToSearch.end());
          }

          std::vector<SAM::LookupHit> AggregateHits;

          for (const auto &Collection : CollectionsToSearch)
          {
               HttpRequest SubRequest = Request;
               SubRequest.QueryParams["collection"] = Collection;
               SubRequest.QueryParams["skip"] = "1";
               SubRequest.QueryParams.erase("all");
               SubRequest.QueryParams.erase("collections");

               HttpResponse SubResponse = HandleSAMSearch(SubRequest);

               if (SubResponse.StatusCode < 200 || SubResponse.StatusCode >= 300)
               {
                    continue;
               }

               try
               {
                    const nlohmann::json SubRoot = nlohmann::json::parse(SubResponse.Body);

                    if (!SubRoot.contains("hits") || !SubRoot["hits"].is_array())
                    {
                         continue;
                    }

                    for (const auto &HitJSON : SubRoot["hits"])
                    {
                         AggregateHits.push_back(ParseSAMLookupHitJSON(HitJSON));
                     }
               }
               catch (const std::exception &)
               {
               }
          }

          return BuildResponse("*",
                               MergeDistributedSAMHits(AggregateHits, static_cast<size_t>(LimitVal)),
                               Distributed ? "distributed-global-sam" : "global-sam",
                               CollectionsToSearch);
     }

     auto RouteIt = Request.QueryParams.find("route");
     const bool HasRoute = (RouteIt != Request.QueryParams.end() && !TrimCopy(RouteIt->second).empty());
     const auto SkipIt = Request.QueryParams.find("skip");
     const bool SkipRecord = (SkipIt != Request.QueryParams.end() && IsTruthyToken(SkipIt->second));
     bool RouteIsLocal = false;
     std::string RoutedHost;
     int RoutedPort = 0;

     if (HasRoute && !ResolveDistributedRoute(RouteIt->second, &RoutedHost, &RoutedPort, &RouteIsLocal))
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid route",
                                    "Use route=local or route=<host[:port]> for a configured distributed node.");
     }

     std::vector<SAM::LookupHit> AggregateHits;
     bool LocalCollectionExists = HybridStorageManagerInstance().CollectionExists(CollectionName);
     bool ExecutedRemote = false;

     const std::string DistributedMode = Instance && Instance->Config ? Instance->Config->GetDistributedSearchMode() : "disabled";
     bool IncludeLocal = LocalCollectionExists;

     if (Distributed && !RouteIsLocal && (DistributedMode == "remote_only" || DistributedMode == "strict_remote"))
     {
          IncludeLocal = false;
     }

     if (HasRoute && !RouteIsLocal)
     {
          IncludeLocal = false;
     }

     if (HasRoute && RouteIsLocal)
     {
          IncludeLocal = true;
     }

     if (IncludeLocal)
     {
          const SAMCoreQueryPlan CoreQueryPlan =
               BuildSAMCoreQueryPlan(CollectionName, Query, static_cast<size_t>(LimitVal));
          std::vector<SAM::LookupHit> LocalHits;

          if (!CoreQueryPlan.Active || !CoreQueryPlan.LookupQuery.empty())
          {
               LocalHits = Instance->Sam->Lookup(CollectionName,
                                                 CoreQueryPlan.Active ? CoreQueryPlan.LookupQuery : Query,
                                                 static_cast<size_t>(LimitVal));
          }

          ApplySAMCoreQueryPlan(LocalHits, CoreQueryPlan, static_cast<size_t>(LimitVal));
          AggregateHits.insert(AggregateHits.end(), LocalHits.begin(), LocalHits.end());

          if (Instance->Sam->IsOpen() && !SkipRecord &&
              ShouldRecordSAMSearchIdea(Request, CollectionName, Query))
          {
               const auto IdeaDocuments = BuildSAMIdeaDocumentsFromLookupHits(LocalHits);
               std::string RecordError;

               if (!Instance->Sam->RecordSearchIdea(CollectionName, Query, IdeaDocuments, &RecordError) &&
                   Instance && Instance->Logs)
               {
                    const std::string ErrorText =
                         RecordError.empty() ? std::string("unknown SAM history write failure") : RecordError;
                    Instance->Logs->Normal("search_api",
                                           "Failed to record SAM search idea for collection '" + CollectionName +
                                                "': " + ErrorText + ".");
               }
          }
     }

     if (Distributed && !RouteIsLocal)
     {
          std::vector<SAMDistributedNode> Nodes;
          BuildSAMDistributedNodes(Nodes);

          if (HasRoute)
          {
               Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
                                          [&](const SAMDistributedNode &Node)
                                          {
                                               return ToLowerCopy(Node.Host) != ToLowerCopy(RoutedHost) || Node.Port != RoutedPort;
                                          }),
                           Nodes.end());
          }

          for (const auto &Node : Nodes)
          {
               if (Node.IsLocal)
               {
                    continue;
               }

               HttpRequest ProxyRequest = Request;
               ProxyRequest.QueryParams["collection"] = CollectionName;
               ProxyRequest.QueryParams["distributed"] = "off";
               ProxyRequest.QueryParams.erase("all");
               ProxyRequest.QueryParams.erase("collections");
               ProxyRequest.QueryParams.erase("route");
               ProxyRequest.Headers["X-HLQ-Distributed-Hop"] = "1";

               HttpResponse ProxyResponse;
               std::string ProxyError;

               if (!ProxyDistributedRequest(ProxyRequest, Node.Host, Node.Port, &ProxyResponse, &ProxyError))
               {
                    continue;
               }

               if (ProxyResponse.StatusCode < 200 || ProxyResponse.StatusCode >= 300)
               {
                    continue;
               }

               ExecutedRemote = true;

               try
               {
                    const nlohmann::json RemoteRoot = nlohmann::json::parse(ProxyResponse.Body);

                    if (!RemoteRoot.contains("hits") || !RemoteRoot["hits"].is_array())
                    {
                         continue;
                    }

                    for (const auto &HitJSON : RemoteRoot["hits"])
                    {
                         AggregateHits.push_back(ParseSAMLookupHitJSON(HitJSON));
                    }
               }
               catch (const std::exception &)
               {
               }
          }
     }

     if (AggregateHits.empty())
     {
          if (!LocalCollectionExists && !ExecutedRemote)
          {
               return BuildErrorResponse(Status::NOT_FOUND,
                                         Code::COLLECTION_NOT_FOUND,
                                         "Collection not found",
                                         "The specified collection does not exist locally or on distributed search nodes.");
          }

          if (Distributed && (DistributedMode == "strict_remote") && !ExecutedRemote)
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed SAM search unavailable",
                                         "No distributed search nodes available after excluding replication replicas.");
          }
     }

     return BuildResponse(CollectionName,
                          MergeDistributedSAMHits(AggregateHits, static_cast<size_t>(LimitVal)),
                          Distributed ? "distributed-sam" : "sam");
}

HttpResponse SearchAPI::HandleSAMStatus(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collections"] = nlohmann::json::array();
     Root["running_collections"] = nlohmann::json::array();
     Root["active_searches"] = nlohmann::json::array();
     Root["dropped_pending_search_ideas"] = Instance->Sam->GetDroppedPendingSearchIdeaJobs();
     Root["dropped_pending_search_interactions"] = Instance->Sam->GetDroppedPendingSearchInteractionJobs();

     const std::map<std::string, SAM::CollectionJobStatus> AllStatuses = Instance->Sam->GetAllCollectionJobStatuses();
     const std::vector<SAM::SearchActivityEntry> ActiveSearches = Instance->Sam->GetActiveSearchActivities(CollectionName);
     size_t RunningCount = 0;
     size_t IndexedDocumentsTotal = 0;
     size_t FailedDocumentsTotal = 0;
     size_t PendingDocumentsTotal = 0;
     size_t SourceDocumentsTotal = 0;

     for (const auto &Entry : AllStatuses)
     {
          IndexedDocumentsTotal += Entry.second.IndexedDocuments;
          FailedDocumentsTotal += Entry.second.FailedDocuments;
          PendingDocumentsTotal += Entry.second.PendingDocuments;
          SourceDocumentsTotal += Entry.second.TotalDocuments;

          if (!CollectionName.empty() && Entry.first != CollectionName)
          {
               continue;
          }

          if (CollectionName.empty() && !Entry.second.Running)
          {
               continue;
          }

          if (!CollectionName.empty() && !HybridStorageManagerInstance().CollectionExists(CollectionName))
          {
               return BuildErrorResponse(Status::NOT_FOUND,
                                        Code::COLLECTION_NOT_FOUND,
                                         "Collection not found",
                                         "The specified collection does not exist.");
          }

          nlohmann::json JobJson = {
               {"collection", Entry.first},
               {"known", true},
               {"running", Entry.second.Running},
               {"completed", Entry.second.Completed},
               {"needs_retry", Entry.second.NeedsRetry},
               {"retry_scheduled", Entry.second.RetryScheduled},
               {"indexed", Entry.second.IndexedDocuments},
               {"failed", Entry.second.FailedDocuments},
               {"pending", Entry.second.PendingDocuments},
               {"total", Entry.second.TotalDocuments},
               {"error", Entry.second.ErrorMessage},
               {"source", Entry.second.Source}
          };

          Root["collections"].push_back(JobJson);

          if (Entry.second.Running)
          {
               Root["running_collections"].push_back(Entry.first);
               RunningCount++;
          }
     }

     for (const auto &Activity : ActiveSearches)
     {
          Root["active_searches"].push_back({
               {"sequence", Activity.Sequence},
               {"collection", Activity.Collection},
               {"query", Activity.Query},
               {"normalized_query", Activity.NormalizedQuery},
               {"started_ms", Activity.StartedMS},
               {"completed_ms", Activity.CompletedMS},
               {"result_count", Activity.ResultCount},
               {"running", Activity.Running}
          });
     }

     if (!CollectionName.empty() && AllStatuses.find(CollectionName) == AllStatuses.end())
     {
          if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
          {
               return BuildErrorResponse(Status::NOT_FOUND,
                                         Code::COLLECTION_NOT_FOUND,
                                         "Collection not found",
                                         "The specified collection does not exist.");
          }

          Root["collection"] = CollectionName;
          Root["known"] = false;
          Root["running"] = false;
          Root["completed"] = false;
          Root["indexed"] = 0;
          Root["failed"] = 0;
          Root["pending"] = 0;
          Root["total"] = 0;
          Root["needs_retry"] = Instance->Sam->HasPendingCollectionRebuild(CollectionName);
          Root["retry_scheduled"] = false;
          Root["error"] = std::string();
          Root["source"] = std::string();
          Root["active_search_count"] = ActiveSearches.size();
          Root["search_running"] = !ActiveSearches.empty();
          Root["message"] = Root["needs_retry"].get<bool>()
               ? "Automatic SAM retry is queued for this collection."
               : "No SAM rebuild has been recorded for this collection.";
     }
     else if (!CollectionName.empty())
     {
          const SAM::CollectionJobStatus &JobStatus = AllStatuses.at(CollectionName);
          SAM::LexicalSyncInfo LexicalInfo;
          const bool HasLexicalInfo = Instance->Sam->GetLexicalSyncInfo(CollectionName, LexicalInfo);
          SAM::SearchActivityEntry LatestSearch;
          const bool HasLatestSearch = Instance->Sam->GetLatestSearchActivity(CollectionName, LatestSearch);
          Root["collection"] = CollectionName;
          Root["known"] = true;
          Root["running"] = JobStatus.Running;
          Root["completed"] = JobStatus.Completed;
          Root["needs_retry"] = JobStatus.NeedsRetry;
          Root["retry_scheduled"] = JobStatus.RetryScheduled;
          Root["indexed"] = JobStatus.IndexedDocuments;
          Root["failed"] = JobStatus.FailedDocuments;
          Root["pending"] = JobStatus.PendingDocuments;
          Root["total"] = JobStatus.TotalDocuments;
          Root["error"] = JobStatus.ErrorMessage;
          Root["source"] = JobStatus.Source;
          Root["active_search_count"] = ActiveSearches.size();
          Root["search_running"] = !ActiveSearches.empty();

          if (HasLexicalInfo)
          {
               Root["lexical_sync"] = {
                    {"collection_synonyms_synced", LexicalInfo.CollectionSynonymsSynced},
                    {"collection_synonym_groups", LexicalInfo.CollectionSynonymGroups},
                    {"collection_synonyms_synced_at_ms", LexicalInfo.CollectionSynonymsSyncedAtMS},
                    {"global_synonyms_synced", LexicalInfo.GlobalSynonymsSynced},
                    {"global_synonym_groups", LexicalInfo.GlobalSynonymGroups},
                    {"global_synonyms_synced_at_ms", LexicalInfo.GlobalSynonymsSyncedAtMS},
                    {"collection_stopwords_synced", LexicalInfo.CollectionStopwordsSynced},
                    {"collection_stopwords", LexicalInfo.CollectionStopwords},
                    {"collection_stopwords_synced_at_ms", LexicalInfo.CollectionStopwordsSyncedAtMS},
                    {"global_stopwords_synced", LexicalInfo.GlobalStopwordsSynced},
                    {"global_stopwords", LexicalInfo.GlobalStopwords},
                    {"global_stopwords_synced_at_ms", LexicalInfo.GlobalStopwordsSyncedAtMS}
               };
          }

          if (HasLatestSearch)
          {
               Root["latest_search"] = {
                    {"sequence", LatestSearch.Sequence},
                    {"collection", LatestSearch.Collection},
                    {"query", LatestSearch.Query},
                    {"normalized_query", LatestSearch.NormalizedQuery},
                    {"started_ms", LatestSearch.StartedMS},
                    {"completed_ms", LatestSearch.CompletedMS},
                    {"result_count", LatestSearch.ResultCount},
                    {"running", LatestSearch.Running}
               };
          }

          Root["message"] = JobStatus.Running ? "SAM indexing is running."
                                              : (JobStatus.RetryScheduled ? "Automatic SAM retry is queued after source mutations."
                                                                         : (JobStatus.NeedsRetry ? "SAM rebuild needs retry after source mutations."
                                                                                                : (!ActiveSearches.empty() ? "SAM is analyzing recent searches."
                                                                                                                           : (JobStatus.Completed ? "SAM indexing is idle."
                                                                                                                                                  : "SAM indexing has not started."))));
     }
     else
     {
          Root["running_count"] = RunningCount;
          Root["known_count"] = AllStatuses.size();
          Root["indexed_total"] = IndexedDocumentsTotal;
          Root["failed_total"] = FailedDocumentsTotal;
          Root["pending_total"] = PendingDocumentsTotal;
          Root["source_total"] = SourceDocumentsTotal;
          Root["active_search_count"] = ActiveSearches.size();
          Root["search_running"] = !ActiveSearches.empty();

          Root["message"] = RunningCount > 0 ? "SAM indexing is running in the background."
                                             : (!ActiveSearches.empty() ? "SAM is analyzing recent searches."
                                                                        : "No SAM collections are currently indexing.");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMPause(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     const auto PauseIt = Request.QueryParams.find("pause");
     const std::string PauseToken = (PauseIt != Request.QueryParams.end()) ? TrimCopy(PauseIt->second) : "";

     if (PauseToken.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing pause value",
                                    "Query parameter 'pause' is required (unix ms timestamp, or 0 to clear).");
     }

     uint64_t RequestedUntilMS = 0;

     try
     {
          RequestedUntilMS = static_cast<uint64_t>(std::stoull(PauseToken));
     }
     catch (const std::exception &)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid pause value",
                                    "Query parameter 'pause' must be an integer unix ms timestamp, or 0 to clear.");
     }

     const uint64_t NowMS = static_cast<uint64_t>(NowMs());
     uint64_t AppliedUntilMS = 0;

     if (RequestedUntilMS > NowMS)
     {
          constexpr uint64_t kMaxPauseWindowMS = 5ULL * 60ULL * 1000ULL;

          AppliedUntilMS = std::min<uint64_t>(RequestedUntilMS, NowMS + kMaxPauseWindowMS);
     }

     Instance->Sam->SetAutoIndexPauseUntilMS(AppliedUntilMS);
     const size_t ClearedQueuedAutoIndexJobs =
          (AppliedUntilMS > NowMS) ? Instance->Sam->ClearQueuedAutoIndexJobs() : 0;
     const uint64_t EffectivePauseUntilMS = Instance->Sam->GetAutoIndexPauseUntilMS();
     const std::string PauseReason = Instance->Sam->GetAutoIndexPauseReason(NowMS);
     const bool FlushInProgress = Instance->Sam->IsFlushInProgress();

     if (Instance->Logs)
     {
          if (AppliedUntilMS > NowMS)
          {
               Instance->Logs->Normal("sam",
                                      "SAM auto-index paused until " +
                                           std::to_string(AppliedUntilMS) +
                                           " ms; cleared " +
                                           std::to_string(ClearedQueuedAutoIndexJobs) +
                                           " queued auto-index job(s).");
          }
          else
          {
               Instance->Logs->Normal("sam", "SAM auto-index pause cleared.");
          }
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["requested_pause_until_ms"] = RequestedUntilMS;
     Root["manual_pause_until_ms"] = AppliedUntilMS;
     Root["pause_until_ms"] = EffectivePauseUntilMS;
     Root["paused"] = (EffectivePauseUntilMS > NowMS) || FlushInProgress;
     Root["pause_reason"] = PauseReason;
     Root["flush_in_progress"] = FlushInProgress;
     Root["cleared_queued_auto_index_jobs"] = ClearedQueuedAutoIndexJobs;
     Root["note"] = "Pauses only automatic SAM background jobs (auto-index). Manual /sam/rebuild is unaffected.";

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMImprove(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     size_t Limit = 0;
     const auto LimitIt = Request.QueryParams.find("limit");

     if (LimitIt != Request.QueryParams.end() && !TrimCopy(LimitIt->second).empty())
     {
          try
          {
               Limit = static_cast<size_t>(std::stoull(TrimCopy(LimitIt->second)));
          }
          catch (const std::exception &)
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Invalid limit value",
                                         "Query parameter 'limit' must be a non-negative integer.");
          }
     }

     const bool Force = Request.QueryParams.find("force") != Request.QueryParams.end() &&
                        IsTruthyToken(Request.QueryParams.at("force"));
     const SAM::ImprovementStats Stats = Instance->Sam->ImproveIdleCollectionsDetailed(Limit, Force);
     const size_t Improved = Stats.TotalImproved();

     nlohmann::json Root;
     Root["ok"] = true;
     Root["improved"] = Improved;
     Root["improved_collections"] = Stats.ImprovedCollections;
     Root["optimized_ideas"] = Stats.OptimizedIdeas;
     Root["pruned_ideas"] = Stats.PrunedIdeas;
     Root["pruned_terms"] = Stats.PrunedTerms;
     Root["skipped_busy"] = Stats.SkippedBusy;
     Root["skipped_stale_index"] = Stats.SkippedStaleIndex;
     Root["skipped_pending_rebuild"] = Stats.SkippedPendingRebuild;
     Root["skipped_cancelled"] = Stats.SkippedCancelled;
     Root["skipped_not_indexed"] = Stats.SkippedNotIndexed;
     Root["skipped_llm_unavailable"] = Stats.SkippedLLMUnavailable;
     Root["skipped_throttled"] = Stats.SkippedThrottled;
     Root["skipped_paused"] = Stats.SkippedPaused;
     Root["skipped_flush_in_progress"] = Stats.SkippedFlushInProgress;
     Root["skipped_no_database"] = Stats.SkippedNoDatabase;
     Root["limit"] = Limit;
     Root["force"] = Force;
     Root["message"] = Improved > 0
          ? "SAM improvement pass completed."
          : "No idle/current SAM collections were eligible for improvement.";

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMFlushActorMetadata(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["cleared"] = nlohmann::json::object();

     const size_t SearchIdeaEntries = gSearchIdeaDedupe.Size();
     const size_t InteractionEntries = gInteractionDedupe.Size();
     const auto AbuseSnapshot = gSamInteractionAbuseGuard.Snapshot();

     gSearchIdeaDedupe.Clear();
     gInteractionDedupe.Clear();
     gSamInteractionAbuseGuard.Clear();

     Root["cleared"]["search_idea_dedupe_entries"] = SearchIdeaEntries;
     Root["cleared"]["interaction_dedupe_entries"] = InteractionEntries;
     Root["cleared"]["actor_minute_entries"] = AbuseSnapshot.ActorMinute;
     Root["cleared"]["actor_hour_entries"] = AbuseSnapshot.ActorHour;
     Root["cleared"]["doc_query_hour_entries"] = AbuseSnapshot.DocQueryHour;
     Root["message"] = "Cleared in-memory SAM actor metadata caches.";

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMHistory(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";

     if (!CollectionName.empty() && !HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     int LimitVal = 100;
     bool InteractionsOnly = false;
     const auto InteractionsOnlyIt = Request.QueryParams.find("interactions_only");
     if (InteractionsOnlyIt != Request.QueryParams.end())
     {
          std::string BoolValue = TrimCopy(InteractionsOnlyIt->second);
          std::transform(BoolValue.begin(), BoolValue.end(), BoolValue.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });
          InteractionsOnly = (BoolValue == "1" || BoolValue == "true" ||
                              BoolValue == "yes" || BoolValue == "on");
     }

     if (!ParseNonNegativeIntParam(Request.QueryParams, "limit", 100, LimitVal) || LimitVal <= 0)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid limit",
                                    "Query parameter 'limit' must be a positive integer.");
     }

     const std::vector<SAM::SearchIdeaEntry> History =
          Instance->Sam->GetSearchIdeaHistory(CollectionName, static_cast<size_t>(LimitVal));

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["limit"] = LimitVal;
     Root["kept_limit"] = 100;
     Root["interactions_only"] = InteractionsOnly;
     Root["history"] = nlohmann::json::array();

     for (const auto &Entry : History)
     {
          if (InteractionsOnly && Entry.InteractionUses == 0)
          {
               continue;
          }

          nlohmann::json BestMatch = nlohmann::json::object();
          std::string Conclusion = Entry.ResolvedConclusion;

          if (!Entry.Documents.empty())
          {
               const auto &TopDocument = Entry.Documents.front();
               BestMatch = {
                    {"id", TopDocument.DocumentID},
                    {"title", TopDocument.Title},
                    {"score", TopDocument.Score}
               };

               if (Conclusion.empty() && !TopDocument.Title.empty())
               {
                    Conclusion = "Most indicated result: " + TopDocument.Title;
               }
               else if (Conclusion.empty() && !TopDocument.DocumentID.empty())
               {
                    Conclusion = "Most indicated result: " + TopDocument.DocumentID;
               }
          }

          nlohmann::json HistoryJson = {
               {"collection", Entry.Collection},
               {"query", Entry.Query},
               {"normalized_query", Entry.NormalizedQuery},
               {"first_seen_ms", Entry.FirstSeenMS},
               {"last_seen_ms", Entry.LastSeenMS},
               {"uses", Entry.Uses},
               {"interaction_uses", Entry.InteractionUses},
               {"last_interaction_ms", Entry.LastInteractionMS},
               {"resolved_interpretation", Entry.ResolvedInterpretation},
               {"resolved_conclusion", Entry.ResolvedConclusion},
               {"resolved_at_ms", Entry.ResolvedAtMS},
               {"resolved_uses", Entry.ResolvedUses},
               {"best_match", BestMatch},
               {"conclusion", Conclusion},
               {"documents", nlohmann::json::array()},
               {"suggestions", nlohmann::json::array()},
               {"resolved_candidates", nlohmann::json::array()},
               {"resolved_ranked_terms", nlohmann::json::array()}
          };

          for (const auto &Document : Entry.Documents)
          {
               HistoryJson["documents"].push_back({
                    {"id", Document.DocumentID},
                    {"title", Document.Title},
                    {"score", Document.Score},
                    {"interaction_uses", Document.InteractionUses},
                    {"last_interaction_ms", Document.LastInteractionMS}
               });

               if (!Document.Title.empty())
               {
                    HistoryJson["suggestions"].push_back(Document.Title);
               }
          }

          for (const auto &Candidate : Entry.ResolvedCandidates)
          {
               HistoryJson["resolved_candidates"].push_back({
                    {"text", Candidate.Text},
                    {"weight", Candidate.Weight}
               });
          }

          for (const auto &RankedTerm : Entry.ResolvedRankedTerms)
          {
               HistoryJson["resolved_ranked_terms"].push_back({
                    {"text", RankedTerm.Text},
                    {"weight", RankedTerm.Weight}
               });
          }

          Root["history"].push_back(std::move(HistoryJson));
     }

     Root["count"] = Root["history"].size();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMDebug(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";

     if (!CollectionName.empty() && !HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     int LimitVal = 100;
     int SinceVal = 0;

     if (!ParseNonNegativeIntParam(Request.QueryParams, "limit", 100, LimitVal) || LimitVal <= 0)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid limit",
                                    "Query parameter 'limit' must be a positive integer.");
     }

     if (!ParseNonNegativeIntParam(Request.QueryParams, "since", 0, SinceVal))
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid since",
                                    "Query parameter 'since' must be a non-negative integer.");
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["since"] = SinceVal;
     Root["latest_sequence"] = Instance->Sam->GetLatestDebugSequence();
     Root["events"] = nlohmann::json::array();

     SAM::CollectionJobStatus JobStatus;
     const bool HasJobStatus = !CollectionName.empty() && Instance->Sam->GetCollectionJobStatus(CollectionName, JobStatus);
     Root["running"] = HasJobStatus && JobStatus.Running;
     Root["known"] = HasJobStatus;

     const std::vector<SAM::DebugEvent> Events = Instance->Sam->GetDebugEvents(CollectionName,
                                                                               static_cast<uint64_t>(SinceVal),
                                                                               static_cast<size_t>(LimitVal));

     for (const auto &Event : Events)
     {
          Root["events"].push_back({
               {"sequence", Event.Sequence},
               {"collection", Event.Collection},
               {"message", Event.Message}
          });
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMListDocuments(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const auto CollectionIt = Request.QueryParams.find("collection");
     const std::string CollectionName = (CollectionIt != Request.QueryParams.end()) ? TrimCopy(CollectionIt->second) : "";

     if (CollectionName.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing collection",
                                    "Query parameter 'collection' is required.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     int OffsetVal = 0;
     int LimitVal = 100;

     if (!ParseNonNegativeIntParam(Request.QueryParams, "offset", 0, OffsetVal))
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid offset",
                                    "Query parameter 'offset' must be a non-negative integer.");
     }

     if (!ParseNonNegativeIntParam(Request.QueryParams, "limit", 100, LimitVal) || LimitVal <= 0)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid limit",
                                    "Query parameter 'limit' must be a positive integer.");
     }

     const std::vector<SAM::DocumentEntry> Entries = Instance->Sam->ListDocuments(CollectionName,
                                                                                  static_cast<size_t>(LimitVal),
                                                                                  static_cast<size_t>(OffsetVal));
     SAM::CollectionJobStatus JobStatus;
     const bool HasJobStatus = Instance->Sam->GetCollectionJobStatus(CollectionName, JobStatus);
     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["offset"] = OffsetVal;
     Root["limit"] = LimitVal;
     Root["count"] = Entries.size();
     Root["rebuilding"] = HasJobStatus && JobStatus.Running;
     Root["job"] = {
          {"known", HasJobStatus},
          {"running", HasJobStatus && JobStatus.Running},
          {"completed", HasJobStatus && JobStatus.Completed},
          {"indexed", HasJobStatus ? JobStatus.IndexedDocuments : 0},
          {"failed", HasJobStatus ? JobStatus.FailedDocuments : 0},
          {"error", HasJobStatus ? JobStatus.ErrorMessage : std::string()}
     };
     Root["documents"] = nlohmann::json::array();

     for (const auto &Entry : Entries)
     {
          nlohmann::json TermsJSON = nlohmann::json::array();

          for (const auto &Term : Entry.Terms)
          {
               TermsJSON.push_back({
                    {"text", Term.Text},
                    {"kind", Term.Kind},
                    {"source", Term.Source},
                    {"score", Term.Score},
                    {"signal", Term.Signal}
               });
          }

          Root["documents"].push_back({
               {"collection", Entry.Collection},
               {"id", Entry.DocumentID},
               {"title", Entry.Title},
               {"lang", Entry.Lang},
               {"label", Entry.Label},
               {"terms", TermsJSON}
          });
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMGetDocument(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     std::string DocumentID;

     if (!ExtractSAMDocumentPathParts(Request.Path, CollectionName, DocumentID))
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid SAM path",
                                    "Expected /sam/documents/<collection>/<document-id>.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "SAM unavailable",
                                    "Secondary Assistant Manager is not initialized.");
     }

     SAM::DocumentEntry Entry;
     std::string ErrorMessage;
     SAM::CollectionJobStatus JobStatus;
     const bool HasJobStatus = Instance->Sam->GetCollectionJobStatus(CollectionName, JobStatus);

     if (!Instance->Sam->GetDocumentEntry(CollectionName, DocumentID, Entry, &ErrorMessage))
     {
          const Document SourceDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);

          if (HasJobStatus && JobStatus.Running && !SourceDoc.ID.empty())
          {
               return BuildErrorResponse(Status::CONFLICT,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "SAM document currently indexing",
                                         "Background SAM indexing is still running for this collection. Try again in a moment.");
          }

          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::DOCUMENT_NOT_FOUND,
                                    "SAM document not found",
                                    ErrorMessage.empty() ? std::string("The specified SAM document does not exist.") : ErrorMessage);
     }

     Document StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);
     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = Entry.Collection;
     Root["id"] = Entry.DocumentID;
     Root["title"] = Entry.Title;
     Root["lang"] = Entry.Lang;
     Root["label"] = Entry.Label;
     Root["format"] = Entry.Format;
     Root["analysis"] = {
          {"subject", Entry.Subject},
          {"summary", Entry.Summary},
          {"aliases", Entry.Aliases},
          {"descriptors", Entry.Descriptors},
          {"queries", Entry.Queries}
     };
     Root["terms"] = nlohmann::json::array();

     for (const auto &Term : Entry.Terms)
     {
          Root["terms"].push_back({
               {"text", Term.Text},
               {"kind", Term.Kind},
               {"source", Term.Source},
               {"score", Term.Score},
               {"signal", Term.Signal}
          });
     }

     Root["rebuilding"] = HasJobStatus && JobStatus.Running;
     Root["job"] = {
          {"known", HasJobStatus},
          {"running", HasJobStatus && JobStatus.Running},
          {"completed", HasJobStatus && JobStatus.Completed},
          {"indexed", HasJobStatus ? JobStatus.IndexedDocuments : 0},
          {"failed", HasJobStatus ? JobStatus.FailedDocuments : 0},
          {"error", HasJobStatus ? JobStatus.ErrorMessage : std::string()}
     };
     Root["document"] = BuildDocumentJSON(StorageDoc);

     const auto InteractionQueryIt = Request.QueryParams.find("interaction_query");
     const std::string InteractionQuery = (InteractionQueryIt != Request.QueryParams.end())
          ? TrimCopy(InteractionQueryIt->second)
          : std::string();

     if (!InteractionQuery.empty() &&
         Instance && Instance->Sam && Instance->Sam->IsOpen() &&
         ShouldRecordSAMInteraction(Request, CollectionName, InteractionQuery, Entry.DocumentID))
     {
          SAM::SearchIdeaDocumentRef InteractionDocument;
          InteractionDocument.DocumentID = Entry.DocumentID;
          InteractionDocument.Title = Entry.Title;
          InteractionDocument.Score = 0.0;
          InteractionDocument.InteractionUses = 1;
          InteractionDocument.LastInteractionMS = static_cast<uint64_t>(NowMs());

          std::string InteractionError;
          if (!Instance->Sam->RecordSearchInteraction(CollectionName,
                                                      InteractionQuery,
                                                      InteractionDocument,
                                                      &InteractionError))
          {
               Root["interaction_recorded"] = false;
               Root["interaction_error"] = InteractionError;
          }
          else
          {
               Root["interaction_recorded"] = true;
               Root["interaction_query"] = InteractionQuery;

               FOREACH_MOD(OnSamInteraction,
                           CollectionName,
                           InteractionQuery,
                           Entry.DocumentID,
                           Request.RemoteAddress,
                           Request.APIKeyID,
                           !Request.APIKeyID.empty());
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

HttpResponse SearchAPI::HandleSAMAddDocumentLabel(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     std::string DocumentID;

     if (!ExtractSAMLabelAddPathParts(Request.Path, CollectionName, DocumentID))
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Invalid SAM label path",
                                    "Expected /sam/label/add/<collection>/<document-id>.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::COLLECTION_NOT_FOUND,
                                    "Collection not found",
                                    "The specified collection does not exist.");
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;

               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward SAM label update to target node." : ProxyError);
          }
     }

     const std::vector<std::string> RequestedLabels = ExtractManualLabelsFromRequest(Request);

     if (RequestedLabels.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Missing label",
                                    "Provide a label with ?label=..., JSON {\"label\":\"...\"}, or a body like label:queen of pop.");
     }

     Document StorageDoc;

     try
     {
          StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);
     }
     catch (const std::exception &E)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Document lookup failed",
                                    std::string("Failed to retrieve document: ") + E.what());
     }
     catch (...)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Document lookup failed",
                                    "Unknown error occurred while retrieving document.");
     }

     if (StorageDoc.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND,
                                    Code::DOCUMENT_NOT_FOUND,
                                    "Document not found",
                                    "The specified document does not exist in this collection.");
     }

     std::vector<std::string> Labels = ParseExistingLabels(StorageDoc.Fields["labels"]);
     std::unordered_set<std::string> ExistingKeys;

     for (const std::string &Label : Labels)
     {
          ExistingKeys.insert(ToLowerCopy(TrimCopy(Label)));
     }

     std::vector<std::string> AddedLabels;

     for (const std::string &Label : RequestedLabels)
     {
          std::string LabelError;

          if (!ValidateFieldValue(Label, &LabelError, "label"))
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Invalid label",
                                         LabelError.empty() ? "The label value is invalid." : LabelError);
          }

          const std::string Key = ToLowerCopy(TrimCopy(Label));

          if (ExistingKeys.insert(Key).second)
          {
               Labels.push_back(Label);
               AddedLabels.push_back(Label);
          }
     }

     nlohmann::json LabelsJSON = nlohmann::json::array();

     for (const std::string &Label : Labels)
     {
          LabelsJSON.push_back(Label);
     }

     StorageDoc.Fields["labels"] = LabelsJSON.dump();
     StorageDoc.Timestamp = Instance ? Instance->NowMs() : static_cast<uint64_t>(time(nullptr) * 1000);

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateDocument,
                                                              CollectionName,
                                                              StorageDoc,
                                                              Request.RemoteAddress,
                                                              Request.APIKeyID,
                                                              !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "sam_add_document_label", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     if (!HybridStorageManagerInstance().AddDocument(CollectionName, StorageDoc))
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);

          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Label update failed",
                                    "Failed to persist the updated document.");
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "sam_add_document_label", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document label was updated locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return JournalResponse;
     }

     FOREACH_MOD(OnUpdateDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["id"] = DocumentID;
     Root["added"] = AddedLabels;
     Root["labels"] = Labels;
     Root["sam_index_requested"] = Instance && Instance->Sam && Instance->Sam->IsOpen();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "sam_add_document_label", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document label was updated locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);
     return Response;
}

/* HandleBulkImportDocuments imports multiple documents. */

HttpResponse SearchAPI::HandleBulkImportDocuments(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     nlohmann::json Payload;

     try
     {
          Payload = nlohmann::json::parse(Request.Body);
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(E.what()) + "\",\"details\":\"" + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }

     if (!Payload.contains("documents") || !Payload["documents"].is_array())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid payload\",\"message\":\"Expected 'documents' array in request body\",\"details\":\"Expected 'documents' array\"}";

          return Response;
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreBulkImportDocuments, CollectionName, static_cast<uint64_t>(Payload["documents"].size()), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::vector<Document> Documents;
     std::vector<std::string> ParseErrors;

     Documents.reserve(Payload["documents"].size());

     for (const auto &DocJSON : Payload["documents"])
     {
          if (!DocJSON.is_object())
          {
               ParseErrors.push_back("Document entry is not a JSON object");

               continue;
          }

          Document DocumentObj;
          std::string ParseErrorStr;

          if (!ParseDocumentFromJSON(DocJSON, DocumentObj, &ParseErrorStr))
          {
               ParseErrors.push_back("Parse error: " + ParseErrorStr);

               continue;
          }

          Documents.push_back(DocumentObj);
     }

     std::unordered_set<std::string> SeenIDs;
     std::unordered_set<std::string> DuplicateIDs;
     size_t FailedCount = ParseErrors.size();
     std::vector<std::string> ErrorMessages = ParseErrors;
     std::vector<Document> UniqueDocuments;

     UniqueDocuments.reserve(Documents.size());

     for (const auto &DocObj : Documents)
     {
          if (DocObj.ID.empty())
          {
               continue;
          }

          if (SeenIDs.find(DocObj.ID) != SeenIDs.end())
          {
               DuplicateIDs.insert(DocObj.ID);
               FailedCount++;
               ErrorMessages.push_back("Duplicate document ID in batch: '" + DocObj.ID + "'");

               continue;
          }

          SeenIDs.insert(DocObj.ID);
          UniqueDocuments.push_back(DocObj);
     }

     size_t RemoteImportedCount = 0;

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::vector<Document> LocalDocs;
          std::map<std::string, std::vector<Document>> RemoteDocs;

          for (const auto &DocObj : UniqueDocuments)
          {
               std::string TargetHost;
               int TargetPort = 0;
               bool IsLocal = false;

               if (SelectDistributedNodeForKey(DocObj.ID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
               {
                    std::string Key = TargetHost + ":" + std::to_string(TargetPort);
                    RemoteDocs[Key].push_back(DocObj);
               }
               else
               {
                    LocalDocs.push_back(DocObj);
               }
          }

          for (const auto &NodePair : RemoteDocs)
          {
               const std::string &Key = NodePair.first;
               size_t ColonPos = Key.rfind(':');
               if (ColonPos == std::string::npos)
               {
                    FailedCount += NodePair.second.size();
                    continue;
               }

               std::string Host = Key.substr(0, ColonPos);
               int Port = std::stoi(Key.substr(ColonPos + 1));

               nlohmann::json PayloadJSON;
               PayloadJSON["documents"] = nlohmann::json::array();
               for (const auto &DocObj : NodePair.second)
               {
                    PayloadJSON["documents"].push_back(BuildDocumentJSON(DocObj));
               }

               HttpRequest ProxyReq = Request;
               ProxyReq.Body = PayloadJSON.dump();
               ProxyReq.Headers["Content-Type"] = "application/json";

               HttpResponse ProxyResp;
               std::string ProxyError;
               if (!ProxyDistributedRequest(ProxyReq, Host, Port, &ProxyResp, &ProxyError))
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back(ProxyError.empty() ? "Failed to forward bulk import to " + Key : ProxyError);
                    }
                    continue;
               }

               if (ProxyResp.StatusCode < 200 || ProxyResp.StatusCode >= 300)
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Remote bulk import failed on " + Key + " with status " + std::to_string(ProxyResp.StatusCode));
                    }
                    continue;
               }

               try
               {
                    nlohmann::json RespJSON = nlohmann::json::parse(ProxyResp.Body);
                    RemoteImportedCount += RespJSON.value("imported", 0);
                    FailedCount += RespJSON.value("failed", 0);
                    if (RespJSON.contains("errors") && RespJSON["errors"].is_array() && ErrorMessages.size() < 100)
                    {
                         for (const auto &ErrVal : RespJSON["errors"])
                         {
                              if (ErrVal.is_string())
                              {
                                   ErrorMessages.push_back(ErrVal.get<std::string>());
                              }
                         }
                    }
               }
               catch (...)
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Failed to parse remote bulk import response from " + Key);
                    }
               }
          }

          UniqueDocuments = std::move(LocalDocs);
     }

     size_t BatchChunkSize = 2000;
     size_t ImportedCount = RemoteImportedCount;
     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     const bool IsResyncImport = (Request.Headers.count("X-HLQ-Resync-Session") || Request.Headers.count("x-hlq-resync-session"));
     bool AssumeNewDocuments = false;

     auto AssumeNewIt = Request.QueryParams.find("assume_new");

     if (AssumeNewIt != Request.QueryParams.end())
     {
          std::string Value = AssumeNewIt->second;
          std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C)
                         { return static_cast<char>(std::tolower(C)); });
          AssumeNewDocuments = (Value == "1" || Value == "true" || Value == "yes" || Value == "on");
     }

     auto BatchSizeIt = Request.QueryParams.find("batch_size");

     if (BatchSizeIt != Request.QueryParams.end())
     {
          try
          {
               const size_t RequestedBatchSize = static_cast<size_t>(std::stoul(BatchSizeIt->second));

               if (RequestedBatchSize > 0)
               {
                    BatchChunkSize = std::min<size_t>(RequestedBatchSize, 10000);
               }
          }
          catch (...)
          {
          }
     }

     if (!UniqueDocuments.empty() &&
         !PrepareReplicationOutboxRecord(Request, "bulk_import", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     for (size_t ChunkStart = 0; ChunkStart < UniqueDocuments.size(); ChunkStart += BatchChunkSize)
     {
          size_t ChunkEnd = std::min(ChunkStart + BatchChunkSize, UniqueDocuments.size());
          std::vector<Document> Chunk(UniqueDocuments.begin() + ChunkStart, UniqueDocuments.begin() + ChunkEnd);

          try
          {
               bool BatchResultVal = false;

               try
               {
                    std::vector<Document> StorageDocs;
                    StorageDocs.reserve(Chunk.size());

                    for (const auto &DocObj : Chunk)
                    {
                         Document StorageDoc;

                         StorageDoc.ID = DocObj.ID;
                         StorageDoc.Title = DocObj.Title;
                         StorageDoc.Content = DocObj.Content;
                         StorageDoc.Fields = DocObj.Fields;
                         StorageDoc.Score = DocObj.Score;
                         StorageDoc.Timestamp = DocObj.Timestamp;

                         StorageDocs.push_back(StorageDoc);
                    }

                    size_t BatchInsertedCount = HybridStorageManagerInstance().AddDocumentsBatch(CollectionName, StorageDocs, AssumeNewDocuments);
                    BatchResultVal = (BatchInsertedCount > 0);

                    if (BatchResultVal)
                    {
                         ImportedCount += BatchInsertedCount;
                    }
               }
               catch (const std::exception &E)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Exception in bulk import batch (size=" + std::to_string(Chunk.size()) + ", collection='" + CollectionName + "'): " + std::string(E.what()) + ".");
                    }

                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Batch import failed: " + std::string(E.what()));
                    }

                    BatchResultVal = false;
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Unknown exception in bulk import batch (size=" + std::to_string(Chunk.size()) + ", collection='" + CollectionName + "').");
                    }

                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Batch import failed: Unknown error occurred");
                    }

                    BatchResultVal = false;
               }

               if (!BatchResultVal)
               {
                    size_t IndividualSuccessCount = 0;

                    for (const auto &DocObj : Chunk)
                    {
                         if (Instance && Instance->Database)
                         {
                              std::string DocKey = "doc:" + CollectionName + ":" + DocObj.ID;
                              std::string ExistingData = Instance->Database->Get(DocKey);

                              if (!ExistingData.empty())
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Normal("search_api", "Bulk import fallback: Skipping duplicate document ID '" + DocObj.ID + "' in collection '" + CollectionName + "'.");
                                   }

                                   continue;
                              }
                         }

                         try
                         {
                              Document StorageDoc;

                              StorageDoc.ID = DocObj.ID;
                              StorageDoc.Title = DocObj.Title;
                              StorageDoc.Content = DocObj.Content;
                              StorageDoc.Fields = DocObj.Fields;
                              StorageDoc.Score = DocObj.Score;
                              StorageDoc.Timestamp = DocObj.Timestamp;

                              if (HybridStorageManagerInstance().AddDocument(CollectionName, StorageDoc))
                              {
                                   IndividualSuccessCount++;
                              }
                              else
                              {
                                   FailedCount++;
                                   std::string DocIDStr = DocObj.ID.empty() ? "<unknown>" : DocObj.ID;

                                   if (ErrorMessages.size() < 100)
                                   {
                                        ErrorMessages.push_back("Failed to add document '" + DocIDStr + "'");
                                   }
                              }
                         }
                         catch (...)
                         {
                              FailedCount++;
                         }
                    }

                    ImportedCount += IndividualSuccessCount;
               }
          }
          catch (...)
          {
               FailedCount += Chunk.size();
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json ResponseJSON;

     ResponseJSON["message"] = "Bulk import completed";
     ResponseJSON["imported"] = ImportedCount;
     ResponseJSON["failed"] = FailedCount;

     if (!ErrorMessages.empty())
     {
          ResponseJSON["errors"] = ErrorMessages;
     }

     Response.Body = ResponseJSON.dump();

     if (ImportedCount > 0)
     {
          if (IsResyncImport)
          {
               MaybeTriggerCrashInjection("replication_resync_import");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!ReplicationOutboxID.empty() &&
              !MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "bulk_import", &ReplicationJournalError))
          {
               HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Bulk import completed locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\"}";
               return JournalResponse;
          }

          FOREACH_MOD(OnBulkImportDocuments, CollectionName, static_cast<uint64_t>(ImportedCount), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          BumpCollectionMutationVersion(CollectionName);

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "bulk_import", &ReplicationError))
          {
               HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Bulk import completed locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\"}";
               return ReplicationResponse;
          }
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleUpdateByQuery updates documents matching a query. */

HttpResponse SearchAPI::HandleUpdateByQuery(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateByQuery, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     /* This endpoint would normally take a query and an update document. */

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Update by query completed\",\"updated\":0}";

     FOREACH_MOD(OnUpdateByQuery, CollectionName, static_cast<uint64_t>(0), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}

/* HandleDeleteByQuery deletes documents matching a query. */

HttpResponse SearchAPI::HandleDeleteByQuery(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteByQuery, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     /* This endpoint would normally take a query and delete matching documents. */

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Delete by query completed\",\"deleted\":0}";

     FOREACH_MOD(OnDeleteByQuery, CollectionName, static_cast<uint64_t>(0), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}
