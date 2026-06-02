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
#include <cctype>
#include <ctime>
#include <rocksdb/write_batch.h>
#include <sstream>

#include "sam/internal.h"

std::string TrimEvalValue(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

std::string NormalizeEvalText(const std::string& Value)
{
     std::string Normalized;
     Normalized.reserve(Value.size());
     bool LastWasSpace = false;

     for (unsigned char C : Value)
     {
          if (std::isalnum(C))
          {
               Normalized.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '_' || C == '/' || C == '.')
          {
               if (!Normalized.empty() && !LastWasSpace)
               {
                    Normalized.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     return TrimEvalValue(Normalized);
}

std::string BuildLookupEvaluationStateKey(const std::string& Collection)
{
     return "sam:eval_state:" + Collection;
}

std::string BuildLookupEvaluationEventKey(const std::string& Collection,
                                          uint64_t TimestampMS,
                                          const std::string& Query)
{
     const size_t Hash = std::hash<std::string>{}(Query);
     return "sam:eval:" + Collection + ":" + std::to_string(TimestampMS) + ":" + std::to_string(Hash);
}

std::string BuildTermUsageKey(const std::string& DocumentID, const std::string& Term)
{
     return DocumentID + "\n" + NormalizeEvalText(Term);
}

bool IsPrunableGeneratedTerm(const SAM::TermEntry& Term)
{
     if (Term.Kind != "query" && Term.Kind != "descriptor")
     {
          return false;
     }

     const std::string& Source = Term.Source;
     const bool Generated =
          Source.rfind("llm_", 0) == 0 ||
          Source.rfind("profile_", 0) == 0 ||
          Source.rfind("iterative_", 0) == 0 ||
          Source == "query_refine" ||
          Source == "query_expand" ||
          Source == "context_pair";

     return Generated && (Term.Score < 0.80 || Term.Signal < 0.82);
}

bool HasAnyPathPrefix(const std::vector<SAM::LookupHit>& Hits, const std::string& Prefix)
{
     for (const auto& Hit : Hits)
     {
          if (Hit.MatchedPath.rfind(Prefix, 0) == 0)
          {
               return true;
          }
     }

     return false;
}

bool HasSemanticAssist(const std::vector<SAM::LookupHit>& Hits)
{
     for (const auto& Hit : Hits)
     {
          if (Hit.MatchedPath == "semantic_profile" ||
              Hit.MatchedPath == "semantic_hybrid" ||
              Hit.Breakdown.SemanticScore > 0.0 ||
              Hit.Breakdown.SemanticVectorScore > 0.0 ||
              Hit.Breakdown.SemanticBonus > 0.0)
          {
               return true;
          }
     }

     return false;
}

bool HasHybridEvidence(const std::vector<SAM::LookupHit>& Hits)
{
     for (const auto& Hit : Hits)
     {
          if (Hit.MatchedPath == "hybrid" ||
              Hit.MatchedPath == "semantic_hybrid" ||
              (Hit.Breakdown.TermScore > 0.0 &&
               (Hit.Breakdown.SourceDocScore > 0.0 || Hit.Breakdown.SemanticScore > 0.0)))
          {
               return true;
          }
     }

     return false;
}

SAMEvaluationCalibration ParseCalibrationFromState(const nlohmann::json& Root)
{
     SAMEvaluationCalibration Calibration;
     const nlohmann::json Summary = Root.contains("summary") && Root["summary"].is_object()
                                         ? Root["summary"]
                                         : nlohmann::json::object();

     Calibration.Samples = Summary.value("samples", 0U);
     Calibration.EmptyRatio = Summary.value("empty_ratio", 0.0);
     Calibration.FallbackRatio = Summary.value("fallback_ratio", 0.0);
     Calibration.GraphAssistRatio = Summary.value("graph_assist_ratio", 0.0);
     Calibration.SemanticAssistRatio = Summary.value("semantic_assist_ratio", 0.0);
     Calibration.HybridRatio = Summary.value("hybrid_ratio", 0.0);
     Calibration.AvgTopScore = Summary.value("avg_top_score", 0.0);
     Calibration.AdaptiveVariantBudget = Summary.value("adaptive_variant_budget", 6U);
     Calibration.AdaptiveGraphBudget = Summary.value("adaptive_graph_budget", 4U);
     Calibration.AdaptiveSemanticBudget = Summary.value("adaptive_semantic_budget", 8U);
     Calibration.AdaptiveVariantBudget = std::max<size_t>(4, std::min<size_t>(24, Calibration.AdaptiveVariantBudget));
     Calibration.AdaptiveGraphBudget = std::max<size_t>(2, std::min<size_t>(16, Calibration.AdaptiveGraphBudget));
     Calibration.AdaptiveSemanticBudget = std::max<size_t>(4, std::min<size_t>(24, Calibration.AdaptiveSemanticBudget));
     return Calibration;
}

void RecomputeCalibrationSummary(nlohmann::json& Root)
{
     SAMEvaluationCalibration Calibration;
     Calibration.AdaptiveVariantBudget = 6;
     Calibration.AdaptiveGraphBudget = 4;
     Calibration.AdaptiveSemanticBudget = 8;

     const nlohmann::json Events = Root.contains("events") && Root["events"].is_array()
                                       ? Root["events"]
                                       : nlohmann::json::array();

     for (const auto& Event : Events)
     {
          if (!Event.is_object())
          {
               continue;
          }

          ++Calibration.Samples;
          Calibration.EmptyRatio += Event.value("empty", false) ? 1.0 : 0.0;
          Calibration.FallbackRatio += Event.value("fallback", false) ? 1.0 : 0.0;
          Calibration.GraphAssistRatio += Event.value("graph_assist", false) ? 1.0 : 0.0;
          Calibration.SemanticAssistRatio += Event.value("semantic_assist", false) ? 1.0 : 0.0;
          Calibration.HybridRatio += Event.value("hybrid", false) ? 1.0 : 0.0;
          Calibration.AvgTopScore += Event.value("top_score", 0.0);
     }

     if (Calibration.Samples > 0)
     {
          const double Samples = static_cast<double>(Calibration.Samples);
          Calibration.EmptyRatio /= Samples;
          Calibration.FallbackRatio /= Samples;
          Calibration.GraphAssistRatio /= Samples;
          Calibration.SemanticAssistRatio /= Samples;
          Calibration.HybridRatio /= Samples;
          Calibration.AvgTopScore /= Samples;
     }

     if (Calibration.EmptyRatio > 0.28 || Calibration.AvgTopScore < 0.84)
     {
          Calibration.AdaptiveVariantBudget += 4;
          Calibration.AdaptiveSemanticBudget += 4;
     }

     if (Calibration.GraphAssistRatio > 0.12 || Calibration.FallbackRatio > 0.18)
     {
          Calibration.AdaptiveGraphBudget += 4;
     }

     if (Calibration.SemanticAssistRatio > 0.20)
     {
          Calibration.AdaptiveSemanticBudget += 4;
     }

     if (Calibration.HybridRatio > 0.28)
     {
          Calibration.AdaptiveVariantBudget += 2;
          Calibration.AdaptiveGraphBudget += 2;
     }

     Calibration.AdaptiveVariantBudget = std::max<size_t>(4, std::min<size_t>(24, Calibration.AdaptiveVariantBudget));
     Calibration.AdaptiveGraphBudget = std::max<size_t>(2, std::min<size_t>(16, Calibration.AdaptiveGraphBudget));
     Calibration.AdaptiveSemanticBudget = std::max<size_t>(4, std::min<size_t>(24, Calibration.AdaptiveSemanticBudget));

     Root["summary"] = {
          {"samples", Calibration.Samples},
          {"empty_ratio", Calibration.EmptyRatio},
          {"fallback_ratio", Calibration.FallbackRatio},
          {"graph_assist_ratio", Calibration.GraphAssistRatio},
          {"semantic_assist_ratio", Calibration.SemanticAssistRatio},
          {"hybrid_ratio", Calibration.HybridRatio},
          {"avg_top_score", Calibration.AvgTopScore},
          {"adaptive_variant_budget", Calibration.AdaptiveVariantBudget},
          {"adaptive_graph_budget", Calibration.AdaptiveGraphBudget},
          {"adaptive_semantic_budget", Calibration.AdaptiveSemanticBudget}
     };
}

bool CaptureLookupEvaluation(rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& Query,
                             const std::vector<SAM::LookupHit>& Hits,
                             std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Lookup evaluation capture requires an open database and collection.";
          }

          return false;
     }

     const uint64_t TimestampMS = static_cast<uint64_t>(std::time(nullptr)) * 1000ULL;
     const std::string NormalizedQuery = NormalizeEvalText(Query);
     const bool Empty = Hits.empty();
     const bool Fallback = HasAnyPathPrefix(Hits, "source_doc_fallback");
     const bool GraphAssist = HasAnyPathPrefix(Hits, "intent_graph");
     const bool SemanticAssist = HasSemanticAssist(Hits);
     const bool Hybrid = HasHybridEvidence(Hits);
     const double TopScore = Empty ? 0.0 : Hits.front().Breakdown.FinalScore;
     const std::string TopPath = Empty ? "" : Hits.front().MatchedPath;
     const std::string TopDocumentID = Empty ? "" : Hits.front().DocumentID;

     nlohmann::json Event = {
          {"ts_ms", TimestampMS},
          {"query", TrimEvalValue(Query)},
          {"normalized_query", NormalizedQuery},
          {"empty", Empty},
          {"fallback", Fallback},
          {"graph_assist", GraphAssist},
          {"semantic_assist", SemanticAssist},
          {"hybrid", Hybrid},
          {"hit_count", Hits.size()},
          {"top_score", TopScore},
          {"top_path", TopPath},
          {"top_document_id", TopDocumentID}
     };

     const std::string EventKey = BuildLookupEvaluationEventKey(Collection, TimestampMS, NormalizedQuery);
     const rocksdb::Status EventStatus =
          Database->Put(rocksdb::WriteOptions(), EventKey, Event.dump());

     if (!EventStatus.ok() && ErrorMessage && ErrorMessage->empty())
     {
          *ErrorMessage = EventStatus.ToString();
     }

     nlohmann::json Root;
     std::string RawState;
     const rocksdb::Status StateStatus =
          Database->Get(rocksdb::ReadOptions(), BuildLookupEvaluationStateKey(Collection), &RawState);

     if (StateStatus.ok() && !RawState.empty())
     {
          try
          {
               Root = nlohmann::json::parse(RawState);
          }
          catch (...)
          {
               Root = nlohmann::json::object();
          }
     }

     if (!Root.is_object())
     {
          Root = nlohmann::json::object();
     }

     Root["collection"] = Collection;

     if (!Root.contains("events") || !Root["events"].is_array())
     {
          Root["events"] = nlohmann::json::array();
     }

     Root["events"].push_back(Event);

     if (!Root.contains("term_usage") || !Root["term_usage"].is_object())
     {
          Root["term_usage"] = nlohmann::json::object();
     }

     for (const auto& Hit : Hits)
     {
          if (Hit.Collection != Collection ||
              Hit.DocumentID.empty() ||
              Hit.MatchedTerm.empty() ||
              Hit.MatchedPath.rfind("sam_term", 0) != 0)
          {
               continue;
          }

          const std::string UsageKey = BuildTermUsageKey(Hit.DocumentID, Hit.MatchedTerm);
          nlohmann::json& Usage = Root["term_usage"][UsageKey];

          if (!Usage.is_object())
          {
               Usage = nlohmann::json::object();
          }

          Usage["hits"] = Usage.value("hits", 0U) + 1;
          Usage["last_used_ms"] = TimestampMS;
     }

     constexpr size_t kMaxTrackedTermUsage = 4096;

     if (Root["term_usage"].size() > kMaxTrackedTermUsage)
     {
          std::vector<std::pair<uint64_t, std::string>> RankedUsage;
          RankedUsage.reserve(Root["term_usage"].size());

          for (auto Iterator = Root["term_usage"].begin(); Iterator != Root["term_usage"].end(); ++Iterator)
          {
               const uint64_t LastUsedMS = Iterator.value().is_object()
                    ? Iterator.value().value("last_used_ms", 0ULL)
                    : 0ULL;
               RankedUsage.emplace_back(LastUsedMS, Iterator.key());
          }

          std::sort(RankedUsage.begin(), RankedUsage.end());

          for (size_t Index = 0; Index + kMaxTrackedTermUsage < RankedUsage.size(); ++Index)
          {
               Root["term_usage"].erase(RankedUsage[Index].second);
          }
     }

     while (Root["events"].size() > 256)
     {
          Root["events"].erase(Root["events"].begin());
     }

     RecomputeCalibrationSummary(Root);

     const rocksdb::Status SaveStatus =
          Database->Put(rocksdb::WriteOptions(), BuildLookupEvaluationStateKey(Collection), Root.dump());

     if (!SaveStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = SaveStatus.ToString();
          }

          return false;
     }

     return true;
}

bool PruneUnusedSAMTermsLocked(rocksdb::DB* Database,
                               const std::string& Collection,
                               size_t* PrunedTerms,
                               std::string* ErrorMessage)
{
     if (PrunedTerms)
     {
          *PrunedTerms = 0;
     }

     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Unused SAM term pruning requires an open database and collection.";
          }

          return false;
     }

     std::string RawState;
     const rocksdb::Status StateStatus =
          Database->Get(rocksdb::ReadOptions(), BuildLookupEvaluationStateKey(Collection), &RawState);

     if (!StateStatus.ok() || RawState.empty())
     {
          return true;
     }

     nlohmann::json State;

     try
     {
          State = nlohmann::json::parse(RawState);
     }
     catch (...)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to parse stored SAM lookup evaluation state.";
          }

          return false;
     }

     if (!State.is_object())
     {
          State = nlohmann::json::object();
     }

     const nlohmann::json Summary =
          State.contains("summary") && State["summary"].is_object()
               ? State["summary"]
               : nlohmann::json::object();
     const size_t Samples = Summary.value("samples", 0U);
     constexpr size_t kMinEvaluationSamples = 48;
     constexpr size_t kProbationSamples = 32;
     constexpr size_t kMaxPrunedTermsPerPass = 64;

     if (Samples < kMinEvaluationSamples)
     {
          return true;
     }

     if (!State.contains("term_usage") || !State["term_usage"].is_object())
     {
          State["term_usage"] = nlohmann::json::object();
     }

     if (!State.contains("term_review") || !State["term_review"].is_object())
     {
          State["term_review"] = nlohmann::json::object();
     }

     nlohmann::json& TermUsage = State["term_usage"];
     nlohmann::json& TermReview = State["term_review"];
     rocksdb::WriteBatch Batch;
     bool StateChanged = false;
     size_t RemovedTerms = 0;
     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() &&
          Iterator->key().starts_with(ManifestPrefix) &&
          RemovedTerms < kMaxPrunedTermsPerPass;
          Iterator->Next())
     {
          nlohmann::json Manifest;
          SAM::DocumentEntry Entry;

          try
          {
               Manifest = nlohmann::json::parse(Iterator->value().ToString());
          }
          catch (...)
          {
               continue;
          }

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !Manifest.contains("terms") ||
              !Manifest["terms"].is_array())
          {
               continue;
          }

          std::vector<SAM::TermEntry> KeptTerms;
          nlohmann::json KeptTermJSON = nlohmann::json::array();
          bool ManifestChanged = false;

          for (const auto& TermJSON : Manifest["terms"])
          {
               if (!TermJSON.is_object())
               {
                    KeptTermJSON.push_back(TermJSON);
                    continue;
               }

               SAM::TermEntry Term;
               Term.Text = TermJSON.value("text", "");
               Term.Kind = TermJSON.value("kind", "");
               Term.Source = TermJSON.value("source", "");
               Term.Score = TermJSON.value("score", 0.0);
               Term.Signal = TermJSON.value("signal", 0.0);
               const std::string UsageKey = BuildTermUsageKey(Entry.DocumentID, Term.Text);
               const bool Used = TermUsage.contains(UsageKey) &&
                                 TermUsage[UsageKey].is_object() &&
                                 TermUsage[UsageKey].value("hits", 0U) > 0;

               if (!IsPrunableGeneratedTerm(Term) || Used)
               {
                    if (Used && TermReview.erase(UsageKey) > 0)
                    {
                         StateChanged = true;
                    }

                    KeptTerms.push_back(Term);
                    KeptTermJSON.push_back(TermJSON);
                    continue;
               }

               if (RemovedTerms >= kMaxPrunedTermsPerPass)
               {
                    KeptTerms.push_back(Term);
                    KeptTermJSON.push_back(TermJSON);
                    continue;
               }

               if (!TermReview.contains(UsageKey))
               {
                    TermReview[UsageKey] = {
                         {"first_sample", Samples},
                         {"last_review_sample", Samples}
                    };
                    StateChanged = true;
                    KeptTerms.push_back(Term);
                    KeptTermJSON.push_back(TermJSON);
                    continue;
               }

               nlohmann::json& Review = TermReview[UsageKey];

               if (!Review.is_object())
               {
                    Review = nlohmann::json::object();
               }

               const size_t FirstSample = Review.value("first_sample", Samples);
               const size_t LastReviewSample = Review.value("last_review_sample", 0U);

               if (LastReviewSample != Samples)
               {
                    Review["last_review_sample"] = Samples;
                    StateChanged = true;
               }

               if (Samples < FirstSample || (Samples - FirstSample) < kProbationSamples)
               {
                    KeptTerms.push_back(Term);
                    KeptTermJSON.push_back(TermJSON);
                    continue;
               }

               Batch.Delete(BuildTermKey(Term.Text, Collection, Entry.DocumentID));
               TermReview.erase(UsageKey);
               TermUsage.erase(UsageKey);
               StateChanged = true;
               ManifestChanged = true;
               ++RemovedTerms;
          }

          if (!ManifestChanged)
          {
               continue;
          }

          if (Manifest.contains("semantic_index") && Manifest["semantic_index"].is_array())
          {
               for (const auto& SemanticJSON : Manifest["semantic_index"])
               {
                    if (!SemanticJSON.is_object())
                    {
                         continue;
                    }

                    const std::string Text = SemanticJSON.value("text", "");
                    const std::string Kind = SemanticJSON.value("kind", "semantic");

                    if (!Text.empty())
                    {
                         Batch.Delete(BuildSemanticProfileKey(Text, Collection, Entry.DocumentID, Kind));
                    }
               }
          }

          Manifest["terms"] = std::move(KeptTermJSON);
          const SAMSemanticProfile Profile =
               BuildSemanticProfile(Entry.Title.empty() ? Entry.DocumentID : Entry.Title, KeptTerms);
          const std::vector<SAMSemanticIndexEntry> SemanticIndex = BuildSemanticIndexEntries(Profile, 32);
          StoreSemanticProfileJSON(Manifest, Profile);
          Manifest["semantic_index"] = nlohmann::json::array();

          for (const auto& SemanticEntry : SemanticIndex)
          {
               Manifest["semantic_index"].push_back({
                    {"text", SemanticEntry.Text},
                    {"kind", SemanticEntry.Kind}
               });

               nlohmann::json Payload = {
                    {"collection", Collection},
                    {"id", Entry.DocumentID},
                    {"title", Entry.Title},
                    {"term", SemanticEntry.Text},
                    {"kind", SemanticEntry.Kind}
               };
               Batch.Put(BuildSemanticProfileKey(SemanticEntry.Text,
                                                 Collection,
                                                 Entry.DocumentID,
                                                 SemanticEntry.Kind),
                         Payload.dump());
          }

          Batch.Put(BuildDocManifestKey(Collection, Entry.DocumentID), Manifest.dump());
     }

     if (!StateChanged)
     {
          return true;
     }

     Batch.Put(BuildLookupEvaluationStateKey(Collection), State.dump());
     const rocksdb::Status WriteStatus = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     if (PrunedTerms)
     {
          *PrunedTerms = RemovedTerms;
     }

     return true;
}

bool LoadLookupEvaluationCalibration(rocksdb::DB* Database,
                                     const std::string& Collection,
                                     SAMEvaluationCalibration& Calibration,
                                     std::string* ErrorMessage)
{
     Calibration = SAMEvaluationCalibration();

     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Lookup evaluation calibration requires an open database and collection.";
          }

          return false;
     }

     std::string RawState;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildLookupEvaluationStateKey(Collection), &RawState);

     if (!Status.ok() || RawState.empty())
     {
          return true;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawState);
          Calibration = ParseCalibrationFromState(Root);
          return true;
     }
     catch (...)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to parse stored SAM lookup evaluation state.";
          }

          return false;
     }
}
