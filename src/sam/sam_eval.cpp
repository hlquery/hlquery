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
#include <sstream>

#include "sam/sam_internal.h"

namespace
{
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
