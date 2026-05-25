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
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/apikeys.h"
#include "api/searchapi.h"
#include "api/common.h"
#include "api/searchcache.h"
#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "sam/sam.h"
#include "sql/sql.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

namespace
{
     class SAMTrainingDedupe
     {
       public:
         explicit SAMTrainingDedupe(size_t MaxEntries)
             : Max(MaxEntries)
         {
         }

         bool ShouldAllow(const std::string& Key, uint64_t NowMS, uint64_t WindowMS)
         {
              if (WindowMS == 0 || Key.empty())
              {
                   return true;
              }

              std::lock_guard<std::mutex> Lock(Mutex);

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
     };

     static SAMTrainingDedupe gSearchIdeaDedupe(16384);

     std::string NormalizeControlToken(std::string Value)
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

     bool IsTruthyControlToken(const std::string& Value)
     {
          const std::string Token = NormalizeControlToken(Value);
          return Token == "1" || Token == "true" || Token == "yes" || Token == "on" || Token == "skip";
     }

     bool IsFalsyControlToken(const std::string& Value)
     {
          const std::string Token = NormalizeControlToken(Value);
          return Token == "0" || Token == "false" || Token == "no" || Token == "off";
     }

     bool ShouldSkipSAMRecording(const HttpRequest& Request)
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

     bool ShouldRecordSAMSearchIdea(const HttpRequest& Request,
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
          const std::string ActorKey = Request.APIKeyID.empty() ? Request.RemoteAddress : Request.APIKeyID;
          const std::string Key = ActorKey + "\n" + Collection + "\n" + Query;
          return gSearchIdeaDedupe.ShouldAllow(Key, NowMS, WindowMS);
     }
}

/* Stores the maybe-suggestion policy for a document search response. */

struct DocumentMaybeSettings
{
     bool Enabled = false;
     int MinResults = -1;
     int Limit = 5;
};

/* Builds the analytics payload that modules receive after search execution. */

static SearchEvent BuildSearchEvent(const std::string &Query,
                                    const std::string &AnalyticsTag,
                                    const std::string &Collection,
                                    uint64_t SearchTimeMS,
                                    uint64_t Found,
                                    uint64_t Returned,
                                    const std::string &RequesterIP,
                                    const std::string &RequesterUser,
                                    bool Authenticated,
                                    bool Distributed)
{
     SearchEvent Event;
     Event.Query = Query;
     Event.AnalyticsTag = AnalyticsTag;
     Event.Collection = Collection;
     Event.SearchTimeMS = SearchTimeMS;
     Event.Found = Found;
     Event.Returned = Returned;
     Event.RequesterIP = RequesterIP;
     Event.RequesterUser = RequesterUser;
     Event.Authenticated = Authenticated;
     Event.Distributed = Distributed;
     return Event;
}

/* Returns a normalized score for merged hit ordering across mixed search modes. */

static float ScoreForMergedHit(const SearchHit &Hit)
{
     const float base_score = Hit.HybridScore > 0.0f ? Hit.HybridScore : (Hit.VectorScore > 0.0f ? Hit.VectorScore : Hit.TextMatch);
     const float weight = (std::isfinite(Hit.Weight) && Hit.Weight > 0.0f) ? Hit.Weight : 1.0f;
     return base_score * weight;
}

static std::vector<SAM::SearchIdeaDocumentRef> BuildSAMSearchIdeaDocuments(const std::vector<SearchHit> &Hits,
                                                                           size_t MaxDocuments = 6)
{
     std::vector<SAM::SearchIdeaDocumentRef> Documents;
     std::unordered_set<std::string> Seen;

     for (const auto &Hit : Hits)
     {
          auto IDIt = Hit.Document.find("id");

          if (IDIt == Hit.Document.end() || IDIt->second.empty() || !Seen.insert(IDIt->second).second)
          {
               continue;
          }

          SAM::SearchIdeaDocumentRef Document;
          Document.DocumentID = IDIt->second;

          auto TitleIt = Hit.Document.find("title");
          if (TitleIt != Hit.Document.end())
          {
               Document.Title = TitleIt->second;
          }

          Document.Score = std::max(0.05f, ScoreForMergedHit(Hit));
          Documents.push_back(std::move(Document));

          if (Documents.size() >= MaxDocuments)
          {
               break;
          }
     }

     return Documents;
}

/* Parses an optional integer parameter used by maybe-suggestion settings. */

static int ParseMaybeInt(const std::unordered_map<std::string, std::string> &Params, const std::string &Key, int DefaultValue)
{
     auto It = Params.find(Key);

     if (It == Params.end())
     {
          return DefaultValue;
     }

     try
     {
          return std::stoi(It->second);
     }
     catch (...)
     {
          return DefaultValue;
     }
}

/* Reads the maybe parameters and normalizes them into safe limits. */

static DocumentMaybeSettings ParseDocumentMaybeSettings(const std::unordered_map<std::string, std::string> &Params)
{
     DocumentMaybeSettings Settings;

     Settings.MinResults = ParseMaybeInt(Params, "maybe_min", -1);
     Settings.Limit = ParseMaybeInt(Params, "maybe_limit", 5);

     if (Settings.MinResults >= 0)
     {
          Settings.Enabled = true;
     }

     if (Settings.Limit < 1)
     {
          Settings.Limit = 1;
     }

     if (Settings.Limit > 20)
     {
          Settings.Limit = 20;
     }

     return Settings;
}

static bool ParseScalarJSONValue(const std::string &Value, nlohmann::json *OutValue)
{
     if (!OutValue)
     {
          return false;
     }

     if (Value == "true")
     {
          *OutValue = true;
          return true;
     }

     if (Value == "false")
     {
          *OutValue = false;
          return true;
     }

     char *End = nullptr;
     const double NumericValue = std::strtod(Value.c_str(), &End);
     if (End != Value.c_str() && End != nullptr && *End == '\0' && std::isfinite(NumericValue))
     {
          if (Value.find_first_of(".eE") == std::string::npos)
          {
               try
               {
                    *OutValue = std::stoll(Value);
               }
               catch (...)
               {
                    *OutValue = NumericValue;
               }
          }
          else
          {
               *OutValue = NumericValue;
          }
          return true;
     }

     *OutValue = Value;
     return true;
}

static std::string FormatTimestampMsAsISO8601Local(std::uint64_t TimestampMs)
{
     auto TimePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(TimestampMs));
     time_t TimeValue = std::chrono::system_clock::to_time_t(TimePoint);
     const long long Milliseconds = static_cast<long long>(TimestampMs % 1000ULL);

     struct tm TmBuffer;
     struct tm *TmPtr = gmtime_r(&TimeValue, &TmBuffer);
     if (!TmPtr)
     {
          return "";
     }

     std::ostringstream Stream;
     Stream << std::put_time(TmPtr, "%Y-%m-%dT%H:%M:%S");
     Stream << '.' << std::setfill('0') << std::setw(3) << Milliseconds << 'Z';
     return Stream.str();
}

static std::string BuildDistinctKey(const SearchHit &Hit, const SQLTranslationResult &SQLResult);

static HttpResponse BuildCollectionNotFoundSQLResponse()
{
     HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

     nlohmann::json Root = nlohmann::json::object();
     Root["error"] = "Collection not found.";
     Root["message"] = "The specified collection does not exist.";
     Root["code"] = Code::COLLECTION_NOT_FOUND;
     Root["code_text"] = CodeText(Code::COLLECTION_NOT_FOUND);

     Response.Body = Root.dump();
     return Response;
}

static bool IsBuiltInSQLFieldName(const std::string &FieldName)
{
     return FieldName == "id" || FieldName == "title" || FieldName == "content" ||
            FieldName == "timestamp" || FieldName == "created_at" ||
            FieldName == "_text_match" || FieldName == "score";
}

static bool SQLFieldExistsInCollection(const std::string &FieldName, const CollectionConfig &Config)
{
     return IsBuiltInSQLFieldName(FieldName) || Config.Fields.find(FieldName) != Config.Fields.end();
}

static bool IsSQLProjectedOutputField(const SQLTranslationResult &SQLResult, const std::string &FieldName)
{
     for (const auto &Field : SQLResult.SelectFields)
     {
          if (Field.OutputName == FieldName)
          {
               return true;
          }
     }

     for (const auto &AggSpec : SQLResult.AggregateSpecs)
     {
          if (AggSpec.OutputName == FieldName)
          {
               return true;
          }
     }

     return false;
}

static bool IsAllowedSQLAggregateFunction(const std::string &FunctionName)
{
     return FunctionName == "COUNT" ||
            FunctionName == "AVG" ||
            FunctionName == "SUM" ||
            FunctionName == "MIN" ||
            FunctionName == "MAX";
}

static bool ValidateSQLRequestedFields(const SQLTranslationResult &SQLResult,
                                       const CollectionConfig &Config,
                                       std::string *Error)
{
     auto SetError = [&](const std::string &Message)
     {
          if (Error)
          {
               *Error = Message;
          }
          return false;
     };

     for (const auto &Field : SQLResult.SelectFields)
     {
          if (!SQLFieldExistsInCollection(Field.SourceName, Config))
          {
               return SetError("SQL field '" + Field.SourceName + "' does not exist in collection '" + Config.Name + "'.");
          }
     }

     for (const auto &AggSpec : SQLResult.AggregateSpecs)
     {
          if (AggSpec.CountAll || AggSpec.FieldName.empty())
          {
               continue;
          }

          if (!SQLFieldExistsInCollection(AggSpec.FieldName, Config))
          {
               return SetError("SQL aggregate field '" + AggSpec.FieldName + "' does not exist in collection '" + Config.Name + "'.");
          }
     }

     for (const auto &GroupField : SQLResult.Query.GroupBy)
     {
          if (!SQLFieldExistsInCollection(GroupField, Config))
          {
               return SetError("SQL GROUP BY field '" + GroupField + "' does not exist in collection '" + Config.Name + "'.");
          }
     }

     for (const auto &SortSpec : SQLResult.Query.SortBy)
     {
          const std::size_t ColonPos = SortSpec.rfind(':');
          const std::string SortField = ColonPos == std::string::npos ? SortSpec : SortSpec.substr(0, ColonPos);

          if (IsSQLProjectedOutputField(SQLResult, SortField))
          {
               continue;
          }

          if (!SQLFieldExistsInCollection(SortField, Config))
          {
               return SetError("SQL ORDER BY field '" + SortField + "' does not exist in collection '" + Config.Name + "'.");
          }
     }

     return true;
}

static bool ValidateSQLExecutionPolicy(const HttpRequest &Request,
                                       const SQLTranslationResult &SQLResult,
                                       const CollectionConfig *Config,
                                       std::string *Error)
{
     auto SetError = [&](const std::string &Message)
     {
          if (Error)
          {
               *Error = Message;
          }

          return false;
     };

     if (!SQLResult.Valid)
     {
          return true;
     }

     const bool IsTopLevelSQLRoute = (Request.Path == "/sql");

     if (!IsTopLevelSQLRoute &&
         SQLResult.Type != SQLTranslationResult::StatementType::Select)
     {
          if (SQLResult.Type == SQLTranslationResult::StatementType::Insert ||
              SQLResult.Type == SQLTranslationResult::StatementType::Delete ||
              SQLResult.Type == SQLTranslationResult::StatementType::Drop)
          {
               return SetError("SQL write statements must use the top-level /sql endpoint.");
          }

          if (SQLResult.Type == SQLTranslationResult::StatementType::ShowCollections)
          {
               return SetError("SHOW COLLECTIONS must use the top-level /sql endpoint.");
          }

          if (SQLResult.Type == SQLTranslationResult::StatementType::Update)
          {
               return SetError("SQL UPDATE is not supported by hlquery yet.");
          }

          return SetError("This SQL statement type is not allowed on collection-bound search endpoints.");
     }

     if (IsTopLevelSQLRoute &&
         SQLResult.Type != SQLTranslationResult::StatementType::Select &&
         SQLResult.Type != SQLTranslationResult::StatementType::Insert &&
         SQLResult.Type != SQLTranslationResult::StatementType::Delete &&
         SQLResult.Type != SQLTranslationResult::StatementType::Drop &&
         SQLResult.Type != SQLTranslationResult::StatementType::ShowCollections &&
         SQLResult.Type != SQLTranslationResult::StatementType::Update)
     {
          return SetError("This SQL statement type is not allowed on the /sql endpoint.");
     }

     if (SQLResult.Type != SQLTranslationResult::StatementType::Select)
     {
          return true;
     }

     if (Config && !ValidateSQLRequestedFields(SQLResult, *Config, Error))
     {
          return false;
     }

     for (const auto &AggSpec : SQLResult.AggregateSpecs)
     {
          if (!IsAllowedSQLAggregateFunction(AggSpec.FunctionName))
          {
               return SetError("SQL aggregate function '" + AggSpec.FunctionName + "' is not allowed for execution.");
          }
     }

     if (SQLResult.GroupedAggregates || SQLResult.AggregateOnly)
     {
          for (const auto &SortSpec : SQLResult.Query.SortBy)
          {
               const std::size_t ColonPos = SortSpec.rfind(':');
               const std::string SortField = ColonPos == std::string::npos ? SortSpec : SortSpec.substr(0, ColonPos);

               if (!IsSQLProjectedOutputField(SQLResult, SortField))
               {
                    return SetError("SQL ORDER BY field '" + SortField + "' must match a selected field or aggregate output in grouped or aggregate queries.");
               }
          }
     }

     return true;
}

struct SQLRow
{
     nlohmann::json Values;
     std::size_t FirstSeen = 0;
};

static bool TryParseNumericValue(const std::string &Value, double *OutValue)
{
     if (!OutValue)
     {
          return false;
     }

     char *End = nullptr;
     const double ParsedValue = std::strtod(Value.c_str(), &End);
     if (End == Value.c_str() || End == nullptr || *End != '\0' || !std::isfinite(ParsedValue))
     {
          return false;
     }

     *OutValue = ParsedValue;
     return true;
}

static std::string JSONScalarToFilterValue(const nlohmann::json &Value)
{
     if (Value.is_null())
     {
          return "NULL";
     }

     if (Value.is_boolean())
     {
          return Value.get<bool>() ? "true" : "false";
     }

     if (Value.is_number_integer())
     {
          return std::to_string(Value.get<long long>());
     }

     if (Value.is_number_unsigned())
     {
          return std::to_string(Value.get<unsigned long long>());
     }

     if (Value.is_number_float())
     {
          std::ostringstream Stream;
          Stream << Value.get<double>();
          return Stream.str();
     }

     if (Value.is_string())
     {
          return Value.get<std::string>();
     }

     return Value.dump();
}

static bool BuildSQLRowValueFromHit(const SearchHit &Hit,
                                    const std::string &SourceField,
                                    const std::string &OutputField,
                                    nlohmann::json *OutValue)
{
     if (!OutValue)
     {
          return false;
     }

     if (SourceField == "created_at")
     {
          auto TimestampIt = Hit.Document.find("timestamp");
          if (TimestampIt != Hit.Document.end())
          {
               try
               {
                    const std::uint64_t TimestampMs = std::stoull(TimestampIt->second);
                    const std::string CreatedAt = FormatTimestampMsAsISO8601Local(TimestampMs);
                    if (!CreatedAt.empty())
                    {
                         *OutValue = CreatedAt;
                         return true;
                    }
               }
               catch (...)
               {
               }
          }
     }

     auto FieldIt = Hit.Document.find(SourceField);
     if (FieldIt == Hit.Document.end())
     {
          if (OutputField == "_text_match")
          {
               *OutValue = Hit.TextMatch;
               return true;
          }

          if (OutputField == "score")
          {
               const float WeightedScore =
                    (Hit.HybridScore > 0.0f ? Hit.HybridScore : (Hit.VectorScore > 0.0f ? Hit.VectorScore : Hit.TextMatch)) *
                    ((std::isfinite(Hit.Weight) && Hit.Weight > 0.0f) ? Hit.Weight : 1.0f);
               *OutValue = WeightedScore;
               return true;
          }

          return false;
     }

     return ParseScalarJSONValue(FieldIt->second, OutValue);
}

static std::vector<SQLRow> BuildSQLRowsFromHits(const SQLTranslationResult &SQLResult,
                                                const ComprehensiveSearchQuery &Query,
                                                const std::vector<SearchHit> &Hits)
{
     std::vector<SQLRow> Rows;
     Rows.reserve(Hits.size());

     for (std::size_t Index = 0; Index < Hits.size(); ++Index)
     {
          SQLRow Row;
          Row.FirstSeen = Index;

          if (SQLResult.SelectFields.empty())
          {
               for (const auto &Pair : Hits[Index].Document)
               {
                    nlohmann::json Value;
                    ParseScalarJSONValue(Pair.second, &Value);
                    Row.Values[Pair.first] = Value;
               }

               if (Query.IncludeCreatedAt)
               {
                    nlohmann::json CreatedAt;
                    if (BuildSQLRowValueFromHit(Hits[Index], "created_at", "created_at", &CreatedAt))
                    {
                         Row.Values["created_at"] = CreatedAt;
                    }
               }
          }
          else
          {
               for (const auto &Field : SQLResult.SelectFields)
               {
                    nlohmann::json Value;
                    if (BuildSQLRowValueFromHit(Hits[Index], Field.SourceName, Field.OutputName, &Value))
                    {
                         Row.Values[Field.OutputName] = Value;
                    }
                    else
                    {
                         Row.Values[Field.OutputName] = nullptr;
                    }
               }
          }

          Rows.push_back(std::move(Row));
     }

     return Rows;
}

static double ComputeSQLAggregateMetric(const SQLAggregateSpec &AggSpec, const std::vector<SearchHit> &Hits)
{
     if (AggSpec.FunctionName == "COUNT")
     {
          if (AggSpec.CountAll || AggSpec.FieldName.empty())
          {
               return static_cast<double>(Hits.size());
          }

          if (AggSpec.Distinct)
          {
               std::unordered_set<std::string> DistinctValues;

               for (const auto &HitObj : Hits)
               {
                    auto FieldIt = HitObj.Document.find(AggSpec.FieldName);
                    if (FieldIt != HitObj.Document.end() && !FieldIt->second.empty() && FieldIt->second != "null")
                    {
                         DistinctValues.insert(FieldIt->second);
                    }
               }

               return static_cast<double>(DistinctValues.size());
          }

          double MetricValue = 0.0;

          for (const auto &HitObj : Hits)
          {
               auto FieldIt = HitObj.Document.find(AggSpec.FieldName);
               if (FieldIt != HitObj.Document.end() && !FieldIt->second.empty() && FieldIt->second != "null")
               {
                    MetricValue += 1.0;
               }
          }

          return MetricValue;
     }

     double SumValue = 0.0;
     double CountValue = 0.0;
     double MinValue = 0.0;
     double MaxValue = 0.0;
     bool HasValue = false;
     std::unordered_set<std::string> DistinctValues;

     for (const auto &HitObj : Hits)
     {
          auto FieldIt = HitObj.Document.find(AggSpec.FieldName);
          if (FieldIt == HitObj.Document.end())
          {
               continue;
          }

          if (AggSpec.Distinct && !DistinctValues.insert(FieldIt->second).second)
          {
               continue;
          }

          double ParsedValue = 0.0;
          if (!TryParseNumericValue(FieldIt->second, &ParsedValue))
          {
               continue;
          }

          if (!HasValue)
          {
               MinValue = ParsedValue;
               MaxValue = ParsedValue;
               HasValue = true;
          }
          else
          {
               MinValue = std::min(MinValue, ParsedValue);
               MaxValue = std::max(MaxValue, ParsedValue);
          }

          SumValue += ParsedValue;
          CountValue += 1.0;
     }

     if (AggSpec.FunctionName == "AVG")
     {
          return CountValue > 0.0 ? (SumValue / CountValue) : 0.0;
     }

     if (AggSpec.FunctionName == "SUM")
     {
          return SumValue;
     }

     if (AggSpec.FunctionName == "MIN")
     {
          return HasValue ? MinValue : 0.0;
     }

     if (AggSpec.FunctionName == "MAX")
     {
          return HasValue ? MaxValue : 0.0;
     }

     return 0.0;
}

static std::string ResolveSQLSortOutputField(const SQLTranslationResult &SQLResult, const std::string &SortField)
{
     for (const auto &Field : SQLResult.SelectFields)
     {
          if (Field.SourceName == SortField || Field.OutputName == SortField)
          {
               return Field.OutputName;
          }
     }

     for (const auto &AggSpec : SQLResult.AggregateSpecs)
     {
          if (AggSpec.FieldName == SortField || AggSpec.OutputName == SortField)
          {
               return AggSpec.OutputName;
          }
     }

     return SortField;
}

static int CompareSQLJSONValues(const nlohmann::json &Left, const nlohmann::json &Right)
{
     if (Left.is_number() && Right.is_number())
     {
          const double LeftValue = Left.get<double>();
          const double RightValue = Right.get<double>();
          if (LeftValue < RightValue)
          {
               return -1;
          }
          if (LeftValue > RightValue)
          {
               return 1;
          }
          return 0;
     }

     if (Left.is_boolean() && Right.is_boolean())
     {
          const bool LeftValue = Left.get<bool>();
          const bool RightValue = Right.get<bool>();
          if (LeftValue == RightValue)
          {
               return 0;
          }
          return LeftValue ? 1 : -1;
     }

     const std::string LeftValue = Left.is_string() ? Left.get<std::string>() : Left.dump();
     const std::string RightValue = Right.is_string() ? Right.get<std::string>() : Right.dump();
     if (LeftValue < RightValue)
     {
          return -1;
     }
     if (LeftValue > RightValue)
     {
          return 1;
     }
     return 0;
}

static void SortSQLRows(std::vector<SQLRow> &Rows, const SQLTranslationResult &SQLResult)
{
     if (SQLResult.Query.SortBy.empty())
     {
          return;
     }

     std::stable_sort(Rows.begin(), Rows.end(),
                      [&](const SQLRow &Left, const SQLRow &Right)
                      {
                           for (const auto &SortSpec : SQLResult.Query.SortBy)
                           {
                                const std::size_t ColonPos = SortSpec.rfind(':');
                                const std::string SortField = ResolveSQLSortOutputField(
                                     SQLResult, ColonPos == std::string::npos ? SortSpec : SortSpec.substr(0, ColonPos));
                                const bool Descending = (ColonPos != std::string::npos && SortSpec.substr(ColonPos + 1) == "desc");

                                const nlohmann::json LeftValue = Left.Values.contains(SortField) ? Left.Values.at(SortField) : nlohmann::json(nullptr);
                                const nlohmann::json RightValue = Right.Values.contains(SortField) ? Right.Values.at(SortField) : nlohmann::json(nullptr);
                                const int Compare = CompareSQLJSONValues(LeftValue, RightValue);
                                if (Compare == 0)
                                {
                                     continue;
                                }

                                return Descending ? (Compare > 0) : (Compare < 0);
                           }

                           return Left.FirstSeen < Right.FirstSeen;
                      });
}

static void ApplySQLHaving(std::vector<SQLRow> &Rows, const SQLTranslationResult &SQLResult)
{
     if (SQLResult.HavingFilter.empty())
     {
          return;
     }

     std::vector<SearchHit> RowHits;
     RowHits.reserve(Rows.size());

     for (const auto &Row : Rows)
     {
          SearchHit Hit;
          for (auto It = Row.Values.begin(); It != Row.Values.end(); ++It)
          {
               Hit.Document[It.key()] = JSONScalarToFilterValue(It.value());
          }
          RowHits.push_back(std::move(Hit));
     }

     const std::vector<SearchHit> FilteredHits = SearchAPI::GetInstance().ApplyFiltersForSQL(RowHits, SQLResult.HavingFilter);
     std::vector<SQLRow> FilteredRows;
     FilteredRows.reserve(FilteredHits.size());

     for (const auto &Hit : FilteredHits)
     {
          for (const auto &Row : Rows)
          {
               bool Matches = true;

               for (const auto &Pair : Hit.Document)
               {
                    if (!Row.Values.contains(Pair.first) || JSONScalarToFilterValue(Row.Values.at(Pair.first)) != Pair.second)
                    {
                         Matches = false;
                         break;
                    }
               }

               if (Matches)
               {
                    FilteredRows.push_back(Row);
                    break;
               }
          }
     }

     Rows = std::move(FilteredRows);
}

static std::string BuildSQLRowsResponse(const std::vector<SQLRow> &Rows,
                                        int Found,
                                        int OutOf,
                                        int Page,
                                        int PerPage,
                                        int Offset,
                                        std::uint64_t SearchTimeMS,
                                        bool Grouped,
                                        const std::vector<std::string> &GroupBy)
{
     const int SafePerPage = PerPage > 0 ? PerPage : 100;
     const int SafeOffset = Offset >= 0 ? Offset : 0;
     const int SafePage = SafePerPage > 0 ? ((SafeOffset / SafePerPage) + 1) : (Page > 0 ? Page : 1);

     nlohmann::json Root = nlohmann::json::object();
     Root["rows"] = nlohmann::json::array();
     for (const auto &Row : Rows)
     {
          Root["rows"].push_back(Row.Values);
     }

     const int TotalPages = (Found <= 0) ? 0 : ((Found + SafePerPage - 1) / SafePerPage);
     Root["found"] = Found;
     Root["out_of"] = OutOf;
     Root["page"] = SafePage;
     Root["per_page"] = SafePerPage;
     Root["offset"] = SafeOffset;
     Root["total_pages"] = TotalPages;
     Root["has_next_page"] = (SafeOffset + SafePerPage) < Found;
     Root["has_prev_page"] = SafeOffset > 0 && TotalPages > 0;
     Root["search_time_ms"] = SearchTimeMS;
     Root["sql_grouped"] = Grouped;
     if (Grouped)
     {
          Root["group_by"] = GroupBy;
     }

     return Root.dump();
}

static HttpResponse BuildShowCollectionsSQLResponse(const HttpRequest &Request)
{
     auto ParseRequestInt = [&](const std::string &Key, int DefaultValue)
     {
          auto It = Request.QueryParams.find(Key);
          if (It == Request.QueryParams.end())
          {
               return DefaultValue;
          }

          try
          {
               return std::stoi(It->second);
          }
          catch (...)
          {
               return DefaultValue;
          }
     };

     const int Page = std::max(1, ParseRequestInt("page", 1));
     const int PerPage = std::max(1, ParseRequestInt("per_page", 20));
     const int Offset = std::max(0, (Page - 1) * PerPage);

     HttpRequest CollectionsRequest = Request;
     CollectionsRequest.Method = "GET";
     CollectionsRequest.Path = "/collections";
     CollectionsRequest.QueryParams.clear();
     CollectionsRequest.QueryParams["offset"] = std::to_string(Offset);
     CollectionsRequest.QueryParams["limit"] = std::to_string(PerPage);

     HttpResponse CollectionsResponse = SearchAPI::GetInstance().HandleListCollections(CollectionsRequest);
     if (CollectionsResponse.StatusCode < 200 || CollectionsResponse.StatusCode >= 300)
     {
          return CollectionsResponse;
     }

     nlohmann::json Root;
     try
     {
          Root = nlohmann::json::parse(CollectionsResponse.Body);
     }
     catch (const nlohmann::json::exception &E)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_QUERY,
                                    "Invalid SQL query.",
                                    std::string("Failed to parse collections response: ") + E.what());
     }

     std::vector<SQLRow> Rows;
     if (Root.contains("collections") && Root["collections"].is_array())
     {
          Rows.reserve(Root["collections"].size());

          for (const auto &Entry : Root["collections"])
          {
               if (!Entry.is_object())
               {
                    continue;
               }

               SQLRow Row;

               if (Entry.contains("name"))
               {
                    Row.Values["name"] = Entry["name"];
               }

               if (Entry.contains("num_documents"))
               {
                    Row.Values["num_documents"] = Entry["num_documents"];
               }

               if (Entry.contains("created_at"))
               {
                    Row.Values["created_at"] = Entry["created_at"];
               }

               if (Entry.contains("metadata") && !Entry["metadata"].is_null())
               {
                    Row.Values["metadata"] = Entry["metadata"];
               }

               Rows.push_back(std::move(Row));
          }
     }

     const int Found = Root.contains("found") ? Root["found"].get<int>() : static_cast<int>(Rows.size());
     const int OutOf = Root.contains("total") ? Root["total"].get<int>() : Found;
     const int ResponsePage = Root.contains("page") ? Root["page"].get<int>() : Page;
     const int ResponsePerPage = Root.contains("limit") ? Root["limit"].get<int>() : PerPage;
     const int ResponseOffset = Root.contains("offset") ? Root["offset"].get<int>() : Offset;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = BuildSQLRowsResponse(Rows, Found, OutOf, ResponsePage, ResponsePerPage, ResponseOffset, 0, false, {});
     return Response;
}

static std::string BuildDistinctSQLResponse(const SQLTranslationResult &SQLResult,
                                            const ComprehensiveSearchQuery &Query,
                                            const ComprehensiveSearchResult &SearchResult)
{
     std::vector<SearchHit> DistinctHits;
     std::unordered_set<std::string> Seen;
     DistinctHits.reserve(SearchResult.MatchedHits.size());

     for (const auto &Hit : SearchResult.MatchedHits)
     {
          const std::string Key = BuildDistinctKey(Hit, SQLResult);
          if (!Seen.insert(Key).second)
          {
               continue;
          }

          SearchHit ProjectedHit = Hit;
          if (!Query.IncludeFields.empty())
          {
               std::map<std::string, std::string> ProjectedDocument;
               for (const auto &Field : Query.IncludeFields)
               {
                    auto It = ProjectedHit.Document.find(Field);
                    if (It != ProjectedHit.Document.end())
                    {
                         ProjectedDocument[Field] = It->second;
                    }
               }
               ProjectedHit.Document = std::move(ProjectedDocument);
          }

          DistinctHits.push_back(std::move(ProjectedHit));
     }

     const std::vector<SearchHit> PagedDistinctHits = SearchAPI::GetInstance().ApplyPaginationForSQL(DistinctHits, Query.Page, Query.PerPage, Query.Offset);
     const std::vector<SQLRow> Rows = BuildSQLRowsFromHits(SQLResult, Query, PagedDistinctHits);
     return BuildSQLRowsResponse(Rows,
                                 static_cast<int>(DistinctHits.size()),
                                 static_cast<int>(DistinctHits.size()),
                                 Query.Page,
                                 Query.PerPage,
                                 Query.Offset,
                                 SearchResult.SearchTimeMS,
                                 false,
                                 {});
}

static std::string BuildAggregateOnlySQLResponse(const SQLTranslationResult &SQLResult,
                                                 const ComprehensiveSearchQuery &Query,
                                                 const ComprehensiveSearchResult &SearchResult)
{
     SQLRow AggregateRow;
     AggregateRow.FirstSeen = 0;
     for (const auto &AggSpec : SQLResult.AggregateSpecs)
     {
          AggregateRow.Values[AggSpec.OutputName] = ComputeSQLAggregateMetric(AggSpec, SearchResult.MatchedHits);
     }

     std::vector<SQLRow> AggregateRows;
     AggregateRows.push_back(AggregateRow);
     ApplySQLHaving(AggregateRows, SQLResult);

     nlohmann::json Root = nlohmann::json::object();
     Root["hits"] = nlohmann::json::array();
     Root["found"] = AggregateRows.empty() ? 0 : SearchResult.Found;
     Root["out_of"] = AggregateRows.empty() ? 0 : SearchResult.OutOf;
     Root["page"] = Query.Page > 0 ? Query.Page : 1;
     Root["per_page"] = Query.PerPage > 0 ? Query.PerPage : 1;
     Root["total_pages"] = Root["out_of"].get<int>() > 0 ? 1 : 0;
     Root["has_next_page"] = false;
     Root["has_prev_page"] = false;
     Root["indexing_in_progress"] = SearchResult.IndexingInProgress;
     Root["aggregations"] = nlohmann::json::object();
     Root["rows"] = nlohmann::json::array();

     if (!AggregateRows.empty())
     {
          Root["rows"].push_back(AggregateRows[0].Values);

          for (const auto &AggSpec : SQLResult.AggregateSpecs)
          {
               nlohmann::json Aggregation = nlohmann::json::object();
               std::string AggregationType = AggSpec.FunctionName;
               std::transform(AggregationType.begin(), AggregationType.end(), AggregationType.begin(), [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               Aggregation["type"] = AggregationType;
               Aggregation["metrics"] = nlohmann::json::object();
               Aggregation["metrics"]["value"] = AggregateRows[0].Values[AggSpec.OutputName];
               Root["aggregations"][AggSpec.OutputName] = Aggregation;
          }
     }

     Root["search_time_ms"] = SearchResult.SearchTimeMS;
     return Root.dump();
}

static std::string BuildSelectSQLResponse(const SQLTranslationResult &SQLResult,
                                          const ComprehensiveSearchQuery &Query,
                                          const ComprehensiveSearchResult &SearchResult)
{
     const std::vector<SearchHit> &SourceHits = SearchResult.MatchedHits.empty() ? SearchResult.Hits : SearchResult.MatchedHits;
     std::vector<SQLRow> Rows = BuildSQLRowsFromHits(SQLResult, Query, SourceHits);
     SortSQLRows(Rows, SQLResult);

     const int TotalRows = static_cast<int>(Rows.size());
     const int SafePerPage = Query.PerPage > 0 ? Query.PerPage : 100;
     const std::size_t StartOffset = static_cast<std::size_t>(std::max(0, Query.Offset));
     const std::size_t EndOffset = std::min<std::size_t>(StartOffset + static_cast<std::size_t>(SafePerPage), Rows.size());
     std::vector<SQLRow> PagedRows;

     if (StartOffset < Rows.size())
     {
          PagedRows.insert(PagedRows.end(), Rows.begin() + static_cast<std::ptrdiff_t>(StartOffset), Rows.begin() + static_cast<std::ptrdiff_t>(EndOffset));
     }

     return BuildSQLRowsResponse(PagedRows,
                                 TotalRows,
                                 TotalRows,
                                 SearchResult.Page,
                                 SearchResult.PerPage,
                                 Query.Offset,
                                 SearchResult.SearchTimeMS,
                                 false,
                                 {});
}

static std::string BuildGroupedSQLResponse(const SQLTranslationResult &SQLResult,
                                           const ComprehensiveSearchQuery &Query,
                                           const ComprehensiveSearchResult &SearchResult)
{
     struct GroupState
     {
          std::vector<SearchHit> Hits;
          std::map<std::string, std::string> RowValues;
          std::size_t FirstSeen = 0;
     };

     std::map<std::string, GroupState> Groups;
     std::size_t HitIndex = 0;

     for (const auto &HitObj : SearchResult.MatchedHits)
     {
          std::string GroupKey;
          std::map<std::string, std::string> OutputValues;

          for (const auto &Field : SQLResult.SelectFields)
          {
               auto FieldIt = HitObj.Document.find(Field.SourceName);
               const std::string FieldValue = (FieldIt == HitObj.Document.end()) ? "" : FieldIt->second;
               OutputValues[Field.OutputName] = FieldValue;
               GroupKey += Field.SourceName;
               GroupKey += '=';
               GroupKey += FieldValue;
               GroupKey.push_back('\x1f');
          }

          auto InsertResult = Groups.emplace(GroupKey, GroupState{});
          GroupState &State = InsertResult.first->second;
          if (InsertResult.second)
          {
               State.RowValues = OutputValues;
               State.FirstSeen = HitIndex;
          }

          State.Hits.push_back(HitObj);
          ++HitIndex;
     }

     std::vector<SQLRow> Rows;
     Rows.reserve(Groups.size());

     for (auto &GroupPair : Groups)
     {
          SQLRow Row;
          Row.FirstSeen = GroupPair.second.FirstSeen;

          for (const auto &Field : SQLResult.SelectFields)
          {
               nlohmann::json Value;
               ParseScalarJSONValue(GroupPair.second.RowValues[Field.OutputName], &Value);
               Row.Values[Field.OutputName] = Value;
          }

          for (const auto &AggSpec : SQLResult.AggregateSpecs)
          {
               Row.Values[AggSpec.OutputName] = ComputeSQLAggregateMetric(AggSpec, GroupPair.second.Hits);
          }

          Rows.push_back(std::move(Row));
     }

     ApplySQLHaving(Rows, SQLResult);
     SortSQLRows(Rows, SQLResult);
     const int TotalRows = static_cast<int>(Rows.size());
     const int SafePerPage = Query.PerPage > 0 ? Query.PerPage : 100;
     const std::size_t StartOffset = static_cast<std::size_t>(std::max(0, Query.Offset));
     const std::size_t EndOffset = std::min<std::size_t>(StartOffset + static_cast<std::size_t>(SafePerPage), Rows.size());
     std::vector<SQLRow> PagedRows;
     if (StartOffset < Rows.size())
     {
          PagedRows.insert(PagedRows.end(), Rows.begin() + static_cast<std::ptrdiff_t>(StartOffset), Rows.begin() + static_cast<std::ptrdiff_t>(EndOffset));
     }
     return BuildSQLRowsResponse(PagedRows,
                                 TotalRows,
                                 TotalRows,
                                 Query.Page,
                                 Query.PerPage,
                                 Query.Offset,
                                 SearchResult.SearchTimeMS,
                                 true,
                                 SQLResult.Query.GroupBy);
}

/* Parses boolean-like transport flags from request parameter maps. */

static bool ParseTruthyFlag(const std::unordered_map<std::string, std::string> &Params,
                            const std::string &Key,
                            bool DefaultValue = false)
{
     auto It = Params.find(Key);

     if (It == Params.end())
     {
          return DefaultValue;
     }

     std::string Value = It->second;
     std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });

     if (Value == "1" || Value == "true" || Value == "yes" || Value == "on" || Value == "force")
     {
          return true;
     }

     if (Value == "0" || Value == "false" || Value == "no" || Value == "off")
     {
          return false;
     }

     return DefaultValue;
}

/* Returns whether a query string uses wildcard operators that may be disabled. */

static bool SearchQueryUsesWildcard(const std::string &QueryText)
{
     return QueryText.find('*') != std::string::npos || QueryText.find('?') != std::string::npos;
}

/* Extracts unique collection names from the distributed collections response body. */

static std::vector<std::string> ParseCollectionNamesFromDistributedList(const std::string &Body)
{
     std::vector<std::string> Names;
     std::unordered_set<std::string> Seen;

     try
     {
          nlohmann::json Root = nlohmann::json::parse(Body);
          if (!Root.contains("collections") || !Root["collections"].is_array())
          {
               return Names;
          }

          for (const auto &Entry : Root["collections"])
          {
               std::string Name;

               if (Entry.is_string())
               {
                    Name = Entry.get<std::string>();
               }
               else if (Entry.is_object() && Entry.contains("name") && Entry["name"].is_string())
               {
                    Name = Entry["name"].get<std::string>();
               }

               if (!Name.empty() && Seen.insert(Name).second)
               {
                    Names.push_back(Name);
               }
          }
     }
     catch (...)
     {
          return {};
     }

     return Names;
}

struct SQLParamApplyResult
{
     bool Ok = true;
     std::string Collection;
     std::string Details;
     bool CollectionMismatch = false;
     SQLTranslationResult Translation;
     std::unordered_map<std::string, std::string> DerivedParams;
};

/* Applies an optional SQL query parameter by translating it into native search params. */

static SQLParamApplyResult ApplySQLSearchParams(std::unordered_map<std::string, std::string> &Params,
                                                const std::string &PathCollection)
{
     SQLParamApplyResult Result;
     auto SQLIt = Params.find("sql");
     if (SQLIt == Params.end() || SQLIt->second.empty())
     {
          SQLIt = Params.find("exec");
     }

     if (SQLIt == Params.end() || SQLIt->second.empty())
     {
          return Result;
     }

     SQLService LocalSQL;
     auto SQLResult = (Instance && Instance->SQL) ? Instance->SQL->Parse(SQLIt->second)
                                                  : LocalSQL.Parse(SQLIt->second);
     Result.Translation = SQLResult;

     if (!SQLResult.Valid)
     {
          Result.Ok = false;
          Result.Details = SQLResult.Error.empty() ? "Failed to parse sql parameter." : SQLResult.Error;
          return Result;
     }

     if (!PathCollection.empty() && !SQLResult.Collection.empty() && SQLResult.Collection != PathCollection)
     {
          Result.Ok = false;
          Result.CollectionMismatch = true;
          Result.Details = "The collection in the SQL FROM clause must match the request path collection.";
          return Result;
     }

     Result.Collection = SQLResult.Collection;

     if (SQLResult.Type == SQLTranslationResult::StatementType::Insert ||
         SQLResult.Type == SQLTranslationResult::StatementType::Update ||
         SQLResult.Type == SQLTranslationResult::StatementType::Drop)
     {
          return Result;
     }

     if (SQLResult.Type == SQLTranslationResult::StatementType::ShowCollections)
     {
          return Result;
     }

     Params.erase("q");
     Params.erase("query_by");
     Params.erase("filter_by");
     Params.erase("sort_by");
     Params.erase("include_fields");
     Params.erase("limit");
     Params.erase("offset");
     Params.erase("page");
     Params.erase("per_page");
     Params.erase("group_by");
     Params.erase("group_limit");
     Params.erase("aggregations");

     Params["q"] = SQLResult.Query.Q.empty() ? "*" : SQLResult.Query.Q;
     Result.DerivedParams["q"] = Params["q"];

     if (!SQLResult.Query.FilterBy.empty())
     {
          Params["filter_by"] = SQLResult.Query.FilterBy;
          Result.DerivedParams["filter_by"] = SQLResult.Query.FilterBy;
     }

     if (!SQLResult.Query.SortBy.empty())
     {
          std::string sort_by;

          for (size_t index = 0; index < SQLResult.Query.SortBy.size(); ++index)
          {
               if (index > 0)
               {
                    sort_by += ",";
               }

               sort_by += SQLResult.Query.SortBy[index];
          }

          Params["sort_by"] = sort_by;
          Result.DerivedParams["sort_by"] = sort_by;
     }

     if (!SQLResult.Query.IncludeFields.empty())
     {
          std::string include_fields;
          bool NeedsCreatedAt = false;
          bool HasTimestampBackingField = false;

          for (size_t index = 0; index < SQLResult.Query.IncludeFields.size(); ++index)
          {
               if (index > 0)
               {
                    include_fields += ",";
               }

               include_fields += SQLResult.Query.IncludeFields[index];

               if (SQLResult.Query.IncludeFields[index] == "created_at")
               {
                    NeedsCreatedAt = true;
               }
               else if (SQLResult.Query.IncludeFields[index] == "timestamp")
               {
                    HasTimestampBackingField = true;
               }
          }

          if (NeedsCreatedAt)
          {
               Params["include_created_at"] = "true";
               Result.DerivedParams["include_created_at"] = "true";
          }

          if (NeedsCreatedAt && !HasTimestampBackingField)
          {
               include_fields += ",timestamp";
          }

          Params["include_fields"] = include_fields;
          Result.DerivedParams["include_fields"] = include_fields;
     }

     if (!SQLResult.Query.GroupBy.empty())
     {
          std::string group_by;

          for (size_t index = 0; index < SQLResult.Query.GroupBy.size(); ++index)
          {
               if (index > 0)
               {
                    group_by += ",";
               }

               group_by += SQLResult.Query.GroupBy[index];
          }

          Params["group_by"] = group_by;
          Result.DerivedParams["group_by"] = group_by;

          if (SQLResult.Query.GroupLimit > 0 && SQLResult.Query.GroupLimit != 3)
          {
               Params["group_limit"] = std::to_string(SQLResult.Query.GroupLimit);
               Result.DerivedParams["group_limit"] = std::to_string(SQLResult.Query.GroupLimit);
          }
     }

     if (!SQLResult.Query.Aggregations.empty())
     {
          Params["aggregations"] = SQLResult.Query.Aggregations;
          Result.DerivedParams["aggregations"] = SQLResult.Query.Aggregations;
     }

     if (SQLResult.Query.PerPage != 100)
     {
          Params["limit"] = std::to_string(SQLResult.Query.PerPage);
          Result.DerivedParams["limit"] = std::to_string(SQLResult.Query.PerPage);
     }

     if (SQLResult.Query.Page > 1 && SQLResult.Query.PerPage > 0)
     {
          Params["offset"] = std::to_string((SQLResult.Query.Page - 1) * SQLResult.Query.PerPage);
          Result.DerivedParams["offset"] = std::to_string((SQLResult.Query.Page - 1) * SQLResult.Query.PerPage);
     }

     return Result;
}

static std::string BuildDistinctKey(const SearchHit &Hit, const SQLTranslationResult &SQLResult)
{
     std::string Key;

     if (!SQLResult.SelectFields.empty())
     {
          for (const auto &Field : SQLResult.SelectFields)
          {
               auto It = Hit.Document.find(Field.SourceName);
               Key += Field.SourceName;
               Key.push_back('=');
               Key += (It == Hit.Document.end()) ? "" : It->second;
               Key.push_back('\x1f');
          }

          return Key;
     }

     for (const auto &Pair : Hit.Document)
     {
          Key += Pair.first;
          Key.push_back('=');
          Key += Pair.second;
          Key.push_back('\x1f');
     }

     return Key;
}

static void ApplySQLDistinct(ComprehensiveSearchResult &SearchResultObj, const SQLTranslationResult &SQLResult)
{
     if (!SQLResult.Distinct || SQLResult.GroupedAggregates || SQLResult.AggregateOnly)
     {
          return;
     }

     std::vector<SearchHit> DistinctHits;
     std::unordered_set<std::string> Seen;
     DistinctHits.reserve(SearchResultObj.Hits.size());

     for (const auto &Hit : SearchResultObj.Hits)
     {
          const std::string Key = BuildDistinctKey(Hit, SQLResult);

          if (!Seen.insert(Key).second)
          {
               continue;
          }

          DistinctHits.push_back(Hit);
     }

     SearchResultObj.Hits = std::move(DistinctHits);
     SearchResultObj.Found = static_cast<int>(SearchResultObj.Hits.size());
     SearchResultObj.OutOf = static_cast<int>(SearchResultObj.Hits.size());
}

/* HandleSearch main search endpoint. */

/*
 * SearchAPI::HandleSearch implementation.
 */

HttpResponse SearchAPI::HandleSearch(const HttpRequest &Request)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleSearch called: method=" + Request.Method + " path=" + Request.Path + ".");
     }

     if (Request.Method != "GET" && Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     std::unordered_map<std::string, std::string> Params;

     /* GET requests consume query parameters, while POST requests consume JSON bodies. */

     if (Request.Method == "GET")
     {
          Params.insert(Request.QueryParams.begin(), Request.QueryParams.end());
     }
     else
     {
          try
          {
               Params = ParseSearchParamsFromJSON(Request.Body);
          }
          catch (...)
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid JSON body.", "Failed to parse search parameters.");
          }
     }

     const SQLParamApplyResult SQLApplyResult = ApplySQLSearchParams(Params, CollectionName);

     if (!SQLApplyResult.Ok)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    SQLApplyResult.CollectionMismatch ? Code::SEARCH_INVALID_PARAMETER : Code::SEARCH_INVALID_QUERY,
                                    SQLApplyResult.CollectionMismatch ? "SQL collection mismatch." : "Invalid SQL query.",
                                    SQLApplyResult.Details);
     }

     if (SQLApplyResult.Translation.Valid)
     {
          std::string SQLPolicyError;

          if (!ValidateSQLExecutionPolicy(Request, SQLApplyResult.Translation, nullptr, &SQLPolicyError))
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_QUERY,
                                         "Invalid SQL query.",
                                         SQLPolicyError);
          }
     }

     if (CollectionName.empty())
     {
          CollectionName = SQLApplyResult.Collection;
     }

     if (!CollectionName.empty())
     {
          CollectionName = ResolveCollectionName(CollectionName);
     }

     if (SQLApplyResult.Translation.Valid &&
         SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::ShowCollections)
     {
          return BuildShowCollectionsSQLResponse(Request);
     }

     if (CollectionName.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::COLLECTION_INVALID_NAME,
                                    "Invalid collection name.",
                                    "Could not extract collection name from path or SQL statement.");
     }

     if (SQLApplyResult.Translation.Valid &&
         (SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Select ||
          SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Insert ||
          SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Delete ||
          SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Drop) &&
         !HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildCollectionNotFoundSQLResponse();
     }

     if (SQLApplyResult.Translation.Valid &&
         SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Insert)
     {
          HttpRequest InsertRequest = Request;
          InsertRequest.Method = "POST";
          InsertRequest.Path = "/collections/" + CollectionName + "/documents";
          InsertRequest.QueryParams.clear();
          InsertRequest.Body = SQLApplyResult.Translation.InsertDocument.dump();
          InsertRequest.Headers["Content-Type"] = "application/json";
          return HandleAddDocument(InsertRequest);
     }

     if (SQLApplyResult.Translation.Valid &&
         SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Delete)
     {
          HttpRequest DeleteRequest = Request;
          DeleteRequest.Method = "DELETE";
          DeleteRequest.Path = "/collections/" + CollectionName + "/documents";
          DeleteRequest.Body.clear();
          DeleteRequest.QueryParams.clear();
          DeleteRequest.QueryParams.insert(SQLApplyResult.DerivedParams.begin(), SQLApplyResult.DerivedParams.end());
          DeleteRequest.QueryParams["distributed"] = "off";
          return HandleDeleteDocumentsByFilter(DeleteRequest);
     }

     if (SQLApplyResult.Translation.Valid &&
         SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Drop)
     {
          HttpRequest DeleteCollectionRequest = Request;
          DeleteCollectionRequest.Method = "DELETE";
          DeleteCollectionRequest.Path = "/collections/" + CollectionName;
          DeleteCollectionRequest.Body.clear();
          DeleteCollectionRequest.QueryParams.clear();
          return HandleDeleteCollection(DeleteCollectionRequest);
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildCollectionNotFoundSQLResponse();
     }

     if (SQLApplyResult.Translation.Valid)
     {
          CollectionConfig CollectionConfigVal;
          if (HybridStorageManagerInstance().GetCollectionConfig(CollectionName, CollectionConfigVal))
          {
               std::string SQLPolicyError;
               if (!ValidateSQLExecutionPolicy(Request, SQLApplyResult.Translation, &CollectionConfigVal, &SQLPolicyError))
               {
                    return BuildErrorResponse(Status::BAD_REQUEST,
                                              Code::SEARCH_INVALID_QUERY,
                                              "Invalid SQL query.",
                                              SQLPolicyError);
               }
          }
     }

     ComprehensiveSearchQuery SearchQueryObj = ParseComprehensiveSearchQuery(Params);
     const bool IsGroupedSQLQuery = SQLApplyResult.Translation.Valid && SQLApplyResult.Translation.GroupedAggregates;
     const bool IsAggregateOnlySQLQuery = SQLApplyResult.Translation.Valid && SQLApplyResult.Translation.AggregateOnly;
     const bool IsDistinctSQLQuery = SQLApplyResult.Translation.Valid && SQLApplyResult.Translation.Distinct &&
                                     !SQLApplyResult.Translation.GroupedAggregates && !SQLApplyResult.Translation.AggregateOnly;
     const bool IsSQLSelectQuery = SQLApplyResult.Translation.Valid &&
                                   SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Select;
     const bool NeedsCustomSQLExecution = IsSQLSelectQuery;

     if (IsSQLSelectQuery && !SQLApplyResult.Translation.HasExplicitLimit && SQLApplyResult.Translation.Query.Page <= 1)
     {
          SearchQueryObj.PerPage = std::numeric_limits<int>::max();
          SearchQueryObj.Page = 1;
     }

     /* API key embedded filters are always AND-ed into the user-supplied filter expression. */

     /* Scoped search: Append embedded filters from API key. */

     if (!Request.EmbeddedFilters.empty())
     {
          if (!SearchQueryObj.FilterBy.empty())
          {
               SearchQueryObj.FilterBy = "(" + SearchQueryObj.FilterBy + ") && (" + Request.EmbeddedFilters + ")";
          }
          else
          {
               SearchQueryObj.FilterBy = Request.EmbeddedFilters;
          }
     }

     /* Validation thresholds for query parameters. */

     const size_t MaxQueryBytes = 8192;
     const size_t MaxFilterBytes = 8192;
     const size_t MaxFieldBytes = 128;

     std::string ValidationError;

     /* Validate untrusted query fields before they reach the search execution layer. */

     if (!ValidateQueryInput(SearchQueryObj.Q, &ValidationError, MaxQueryBytes, "q"))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_QUERY, "Invalid search query.", ValidationError);
     }

     if (SearchQueryUsesWildcard(SearchQueryObj.Q) &&
         Instance && Instance->Config && !Instance->Config->GetIndexingEnableWildcards())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_QUERY,
                                    "Wildcard search disabled.",
                                    "Wildcard document search is disabled by configuration (indexing.enable_wildcards=false).");
     }

     if (!ValidateQueryInput(SearchQueryObj.FilterBy, &ValidationError, MaxFilterBytes, "filter_by"))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid search parameter.", ValidationError);
     }

     for (const auto &Field : SearchQueryObj.QueryBy)
     {
          if (!ValidateQueryInput(Field, &ValidationError, MaxFieldBytes, "query_by"))
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid search parameter.", ValidationError);
          }
     }

     ComprehensiveSearchResult SearchResultObj;
     const DocumentMaybeSettings MaybeSettings = ParseDocumentMaybeSettings(Params);
     const auto DistributedOverrideIt = Request.QueryParams.find("distributed");
     const bool HasExplicitDistributedOverride = (DistributedOverrideIt != Request.QueryParams.end() &&
                                                 !DistributedOverrideIt->second.empty());
     const bool SupportsDistributedExecution = !IsSQLSelectQuery &&
                                              (SearchQueryObj.Aggregations.empty() || HasExplicitDistributedOverride);

     HttpResponse CachedResponse;
     if (!NeedsCustomSQLExecution &&
         SearchResponseCache::Get("search", Request, CollectionName, CachedResponse))
     {
          return CachedResponse;
     }

     /* Try distributed execution first when routing policy requests cross-node search. */

     if (SupportsDistributedExecution && ShouldAttemptDistributedSearch(Request))
     {
          std::string DistributedError;

          if (TryDistributedSearch(Request, CollectionName, SearchQueryObj, &SearchResultObj, &DistributedError))
          {
               HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
               Response.Headers["X-HLQ-Execution-Mode"] = "distributed";
               ApplySQLDistinct(SearchResultObj, SQLApplyResult.Translation);
               Response.Body = GenerateComprehensiveSearchResponse(SearchResultObj, SearchQueryObj);
               AttachSearchResponseMeta(Response, SearchQueryObj, Request, CollectionName);
               SearchResponseCache::Put("search", Request, CollectionName, Response);

               if (!SearchQueryObj.Q.empty() && SearchQueryObj.Q != "*" &&
                   Instance && Instance->Sam && Instance->Sam->IsOpen() &&
                   ShouldRecordSAMSearchIdea(Request, CollectionName, SearchQueryObj.Q))
               {
                    const auto IdeaDocuments = BuildSAMSearchIdeaDocuments(SearchResultObj.Hits);
                    if (Instance->Sam->RecordSearchIdea(CollectionName, SearchQueryObj.Q, IdeaDocuments))
                    {
                         FOREACH_MOD(OnSamSearch,
                                     CollectionName,
                                     SearchQueryObj.Q,
                                     static_cast<uint64_t>(IdeaDocuments.size()),
                                     Request.RemoteAddress,
                                     Request.APIKeyID,
                                     !Request.APIKeyID.empty());
                    }
               }

               /* Analytics fire for the final distributed response the same as local results. */

               if (SearchQueryObj.EnableAnalytics)
               {
                    const std::string SearchCollection = CollectionName.empty() ? "*" : CollectionName;
                    const uint64_t SearchTimeMS = static_cast<uint64_t>(std::max(0.0f, SearchResultObj.SearchTimeMS));
                    const uint64_t Found = SearchResultObj.Found < 0 ? 0U : static_cast<uint64_t>(SearchResultObj.Found);
                    const uint64_t Returned = static_cast<uint64_t>(SearchResultObj.Hits.size());
                    const bool Authenticated = !Request.APIKeyID.empty();

                    const SearchEvent DocumentEvent = BuildSearchEvent(
                         SearchQueryObj.Q,
                         SearchQueryObj.AnalyticsTag,
                         SearchCollection,
                         SearchTimeMS,
                         Found,
                         Returned,
                         Request.RemoteAddress,
                         Request.APIKeyID,
                         Authenticated,
                         true);

                    FOREACH_MOD(OnSearchDocument, DocumentEvent);
               }
               return Response;
          }

          /* Strict distributed mode rejects the request instead of silently falling back locally. */

          if (IsStrictDistributedMode())
          {
               std::string Details = DistributedError.empty() ? "No distributed nodes available to execute query." : DistributedError;
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed search unavailable.",
                                         Details);
          }
     }

     /* Local execution remains the fallback path when distributed mode is optional. */

     if (NeedsCustomSQLExecution)
     {
          ComprehensiveSearchQuery AnalyticsQuery = SearchQueryObj;
          AnalyticsQuery.GroupBy.clear();
          AnalyticsQuery.PreserveMatchedHits = true;

          if (IsSQLSelectQuery)
          {
               AnalyticsQuery.SortBy.clear();
               AnalyticsQuery.Page = 1;
               AnalyticsQuery.PerPage = std::numeric_limits<int>::max();
               AnalyticsQuery.Offset = 0;
          }

          if (IsGroupedSQLQuery || IsAggregateOnlySQLQuery || IsDistinctSQLQuery)
          {
               AnalyticsQuery.Aggregations.clear();
          }
          SearchResultObj = PerformComprehensiveSearch(CollectionName, AnalyticsQuery);
     }
     else
     {
          SearchResultObj = PerformComprehensiveSearch(CollectionName, SearchQueryObj);
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Headers["X-HLQ-Execution-Mode"] = SupportsDistributedExecution ? "local" : "local-aggregations";

     if (IsGroupedSQLQuery)
     {
          Response.Body = BuildGroupedSQLResponse(SQLApplyResult.Translation, SearchQueryObj, SearchResultObj);
     }
     else if (IsAggregateOnlySQLQuery)
     {
          Response.Body = BuildAggregateOnlySQLResponse(SQLApplyResult.Translation, SearchQueryObj, SearchResultObj);
     }
     else if (IsDistinctSQLQuery)
     {
          Response.Body = BuildDistinctSQLResponse(SQLApplyResult.Translation, SearchQueryObj, SearchResultObj);
     }
     else if (SQLApplyResult.Translation.Type == SQLTranslationResult::StatementType::Select)
     {
          ApplySQLDistinct(SearchResultObj, SQLApplyResult.Translation);
          Response.Body = BuildSelectSQLResponse(SQLApplyResult.Translation, SearchQueryObj, SearchResultObj);
     }
     else
     {
          ApplySQLDistinct(SearchResultObj, SQLApplyResult.Translation);
          Response.Body = GenerateComprehensiveSearchResponse(SearchResultObj, SearchQueryObj);
     }

     /* maybe suggestions are injected only when the result count is below the caller threshold. */

     if (MaybeSettings.Enabled && !SearchQueryObj.CaseSensitive && SearchResultObj.Found >= 0 && SearchResultObj.Found < MaybeSettings.MinResults && !SearchQueryObj.Q.empty())
     {
          HttpRequest MaybeRequest = Request;

          MaybeRequest.Method = "GET";
          MaybeRequest.Path = "/collections/" + CollectionName + "/documents/maybe";
          MaybeRequest.Body.clear();
          MaybeRequest.QueryParams.clear();
          MaybeRequest.QueryParams["q"] = SearchQueryObj.Q;
          MaybeRequest.QueryParams["limit"] = std::to_string(MaybeSettings.Limit);

          HttpResponse MaybeResponse = HandleMaybe(MaybeRequest);

          if (MaybeResponse.StatusCode >= 200 && MaybeResponse.StatusCode < 300)
          {
               try
               {
                    nlohmann::json RootJSON = nlohmann::json::parse(Response.Body);
                    nlohmann::json MaybeJSON = nlohmann::json::parse(MaybeResponse.Body);

                    MaybeJSON["threshold"] = MaybeSettings.MinResults;
                    MaybeJSON["limit"] = MaybeSettings.Limit;

                    RootJSON["maybe"] = MaybeJSON;
                    Response.Body = RootJSON.dump();
               }
               catch (...)
               {
               }
          }
     }

     AttachSearchResponseMeta(Response, SearchQueryObj, Request, CollectionName);

     /* Analytics are emitted after the response body is finalized so counts match the payload. */

     if (!SearchQueryObj.Q.empty() && SearchQueryObj.Q != "*" &&
         Instance && Instance->Sam && Instance->Sam->IsOpen() &&
         ShouldRecordSAMSearchIdea(Request, CollectionName, SearchQueryObj.Q))
     {
          const auto IdeaDocuments = BuildSAMSearchIdeaDocuments(SearchResultObj.Hits);
          if (Instance->Sam->RecordSearchIdea(CollectionName, SearchQueryObj.Q, IdeaDocuments))
          {
               FOREACH_MOD(OnSamSearch,
                           CollectionName,
                           SearchQueryObj.Q,
                           static_cast<uint64_t>(IdeaDocuments.size()),
                           Request.RemoteAddress,
                           Request.APIKeyID,
                           !Request.APIKeyID.empty());
          }
     }

     if (SearchQueryObj.EnableAnalytics)
     {
          const std::string SearchCollection = CollectionName.empty() ? "*" : CollectionName;
          const uint64_t SearchTimeMS = static_cast<uint64_t>(std::max(0.0f, SearchResultObj.SearchTimeMS));
          const uint64_t Found = SearchResultObj.Found < 0 ? 0U : static_cast<uint64_t>(SearchResultObj.Found);
          const uint64_t Returned = static_cast<uint64_t>(SearchResultObj.Hits.size());
          const bool Authenticated = !Request.APIKeyID.empty();

          const SearchEvent DocumentEvent = BuildSearchEvent(
               SearchQueryObj.Q,
               SearchQueryObj.AnalyticsTag,
               SearchCollection,
               SearchTimeMS,
               Found,
               Returned,
               Request.RemoteAddress,
               Request.APIKeyID,
               Authenticated,
               false);

          FOREACH_MOD(OnSearchDocument, DocumentEvent);
     }

     if (!NeedsCustomSQLExecution)
     {
          SearchResponseCache::Put("search", Request, CollectionName, Response);
     }

     return Response;
}

HttpResponse SearchAPI::HandleGlobalSearch(const HttpRequest &Request)
{
     if (Request.Method != "GET" && Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::unordered_map<std::string, std::string> Params;
     std::vector<std::string> RequestedCollections;

     /* Global search accepts collection targeting from either query params or JSON payloads. */

     if (Request.Method == "GET")
     {
          Params.insert(Request.QueryParams.begin(), Request.QueryParams.end());
          auto ItCollections = Params.find("collections");
          if (ItCollections != Params.end())
          {
               RequestedCollections = ParseCommaSeparated(ItCollections->second);
          }
          if (RequestedCollections.empty())
          {
               auto ItCollection = Params.find("collection");
               if (ItCollection != Params.end() && !ItCollection->second.empty())
               {
                    RequestedCollections.push_back(ItCollection->second);
               }
          }
     }
     else
     {
          try
          {
               nlohmann::json Body = nlohmann::json::parse(Request.Body);
               Params = ParseSearchParamsFromJSON(Request.Body);

               if (Body.contains("collections"))
               {
                    if (Body["collections"].is_array())
                    {
                         for (const auto &Entry : Body["collections"])
                         {
                              if (Entry.is_string())
                              {
                                   RequestedCollections.push_back(Entry.get<std::string>());
                              }
                         }
                    }
                    else if (Body["collections"].is_string())
                    {
                         RequestedCollections = ParseCommaSeparated(Body["collections"].get<std::string>());
                    }
               }

               if (RequestedCollections.empty() && Body.contains("collection") && Body["collection"].is_string())
               {
                    RequestedCollections.push_back(Body["collection"].get<std::string>());
               }
          }
          catch (...)
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid JSON body.", "Failed to parse global search payload.");
          }
     }

     const SQLParamApplyResult SQLApplyResult = ApplySQLSearchParams(Params, "");

     if (!SQLApplyResult.Ok)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    SQLApplyResult.CollectionMismatch ? Code::SEARCH_INVALID_PARAMETER : Code::SEARCH_INVALID_QUERY,
                                    SQLApplyResult.CollectionMismatch ? "SQL collection mismatch." : "Invalid SQL query.",
                                    SQLApplyResult.Details);
     }

     if (RequestedCollections.empty())
     {
          auto ItCollections = Params.find("collections");
          if (ItCollections != Params.end())
          {
               RequestedCollections = ParseCommaSeparated(ItCollections->second);
          }
     }

     if (RequestedCollections.empty())
     {
          auto ItCollection = Params.find("collection");
          if (ItCollection != Params.end() && !ItCollection->second.empty())
          {
               RequestedCollections.push_back(ItCollection->second);
          }
     }

     if (RequestedCollections.empty() && !SQLApplyResult.Collection.empty())
     {
          RequestedCollections.push_back(SQLApplyResult.Collection);
     }

     const bool UseDistributedCollections = ShouldAttemptDistributedSearch(Request) &&
                                            ParseTruthyFlag(Params, "distributed_collections", false);
     const auto AvailableCollections = HybridStorageManagerInstance().ListCollections();
     std::unordered_set<std::string> AvailableSet(AvailableCollections.begin(), AvailableCollections.end());

     std::vector<std::string> TargetCollections;

     /* distributed_collections expands the target set with names reported by remote nodes. */

     if (UseDistributedCollections)
     {
          std::unordered_set<std::string> Seen;
          if (RequestedCollections.empty())
          {
               for (const auto &LocalCollection : AvailableCollections)
               {
                    if (!LocalCollection.empty() && Seen.insert(LocalCollection).second)
                    {
                         TargetCollections.push_back(LocalCollection);
                    }
               }

               HttpRequest DistCollectionsRequest = Request;
               DistCollectionsRequest.Method = "GET";
               DistCollectionsRequest.Path = "/collections/distributed";
               DistCollectionsRequest.Body.clear();
               DistCollectionsRequest.QueryParams.clear();

               HttpResponse DistCollectionsResponse = HandleListCollectionsDistributed(DistCollectionsRequest);

               /* Merge remote collection names without duplicating local collection entries. */

               if (DistCollectionsResponse.StatusCode >= 200 && DistCollectionsResponse.StatusCode < 300)
               {
                    auto DistributedCollections = ParseCollectionNamesFromDistributedList(DistCollectionsResponse.Body);
                    for (const auto &Collection : DistributedCollections)
                    {
                         if (!Collection.empty() && Seen.insert(Collection).second)
                         {
                              TargetCollections.push_back(Collection);
                         }
                    }
               }

               if (TargetCollections.empty())
               {
                    TargetCollections = AvailableCollections;
               }
          }
          else
          {
               for (const auto &Collection : RequestedCollections)
               {
                    if (Collection.empty() || Seen.find(Collection) != Seen.end())
                    {
                         continue;
                    }
                    Seen.insert(Collection);
                    TargetCollections.push_back(Collection);
               }
          }
     }
     else if (RequestedCollections.empty())
     {
          TargetCollections = AvailableCollections;
     }
     else
     {
          std::unordered_set<std::string> Seen;
          for (const auto &Collection : RequestedCollections)
          {
               if (Collection.empty() || Seen.find(Collection) != Seen.end())
               {
                    continue;
               }
               Seen.insert(Collection);
               if (AvailableSet.find(Collection) != AvailableSet.end())
               {
                    TargetCollections.push_back(Collection);
               }
          }
     }

     if (TargetCollections.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found.", "No target collections were found for global search.");
     }

     ComprehensiveSearchQuery BaseQuery = ParseComprehensiveSearchQuery(Params);

     const size_t MaxQueryBytes = 8192;
     const size_t MaxFilterBytes = 8192;
     const size_t MaxFieldBytes = 128;
     std::string ValidationError;

     /* Reuse the same input validation rules as single-collection document search. */

     if (!ValidateQueryInput(BaseQuery.Q, &ValidationError, MaxQueryBytes, "q"))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_QUERY, "Invalid search query.", ValidationError);
     }

     if (SearchQueryUsesWildcard(BaseQuery.Q) &&
         Instance && Instance->Config && !Instance->Config->GetIndexingEnableWildcards())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::SEARCH_INVALID_QUERY,
                                    "Wildcard search disabled.",
                                    "Wildcard document search is disabled by configuration (indexing.enable_wildcards=false).");
     }

     if (!ValidateQueryInput(BaseQuery.FilterBy, &ValidationError, MaxFilterBytes, "filter_by"))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid search parameter.", ValidationError);
     }

     for (const auto &Field : BaseQuery.QueryBy)
     {
          if (!ValidateQueryInput(Field, &ValidationError, MaxFieldBytes, "query_by"))
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid search parameter.", ValidationError);
          }
     }

     std::vector<SearchHit> AllHits;
     std::map<std::string, std::map<std::string, int>> FacetCounts;
     bool IndexingInProgress = false;
     bool PartialResults = false;
     float MaxSearchTime = 0.0f;
     size_t ExecutedCollections = 0;
     std::string DistributedError;
     std::vector<std::map<std::string, std::string>> GlobalDistributedDiagnostics;

     /* Execute one collection at a time, then merge hits and facet counts into a global view. */

     for (const auto &CollectionName : TargetCollections)
     {
          ComprehensiveSearchQuery QueryForCollection = BaseQuery;
          QueryForCollection.AllowScanFallback = (TargetCollections.size() == 1);

          std::string FiltersToApply = Request.EmbeddedFilters;

          /* Collection-level API key rules are checked independently for every target collection. */

          if (!Request.APIKeyID.empty())
          {
               auto *KeyObj = APIKeyManager::Instance().GetKey(Request.APIKeyID);
               if (KeyObj)
               {
                    if (!KeyObj->CanAccessCollection(CollectionName) || !KeyObj->HasAction(CollectionName, APIKeyAction::SEARCH))
                    {
                         continue;
                    }
                    FiltersToApply = KeyObj->GetEmbeddedFilters(CollectionName);
               }
          }

          if (!FiltersToApply.empty())
          {
               if (!QueryForCollection.FilterBy.empty())
               {
                    QueryForCollection.FilterBy = "(" + QueryForCollection.FilterBy + ") && (" + FiltersToApply + ")";
               }
               else
               {
                    QueryForCollection.FilterBy = FiltersToApply;
               }
          }

          ComprehensiveSearchResult CollectionResult;
          bool UsedDistributedResult = false;

          /* Each target collection may use remote execution before falling back locally. */

          if (ShouldAttemptDistributedSearch(Request))
          {
               HttpRequest DistributedCollectionRequest = Request;
               DistributedCollectionRequest.Method = "GET";
               DistributedCollectionRequest.Path = "/collections/" + CollectionName + "/documents/search";
               DistributedCollectionRequest.Body.clear();
               DistributedCollectionRequest.QueryParams.clear();
               for (const auto &Param : Params)
               {
                    if (Param.first == "collection" || Param.first == "collections" || Param.first == "distributed_collections")
                    {
                         continue;
                    }
                    DistributedCollectionRequest.QueryParams[Param.first] = Param.second;
               }

               UsedDistributedResult = TryDistributedSearch(DistributedCollectionRequest, CollectionName, QueryForCollection, &CollectionResult, &DistributedError);
               if (!UsedDistributedResult && IsStrictDistributedMode())
               {
                    continue;
               }
          }

          if (!UsedDistributedResult)
          {
               CollectionResult = PerformComprehensiveSearch(CollectionName, QueryForCollection);
          }

          /* Diagnostics are preserved per collection so the caller can inspect merge provenance. */

          for (auto Diagnostic : CollectionResult.DistributedDiagnostics)
          {
               Diagnostic["collection"] = CollectionName;
               GlobalDistributedDiagnostics.push_back(std::move(Diagnostic));
          }

          /* Stamp the source collection into each merged document before appending it. */

	          if (AllHits.capacity() < AllHits.size() + CollectionResult.Hits.size())
	          {
	               AllHits.reserve(AllHits.size() + CollectionResult.Hits.size());
	          }

	          for (auto Hit : CollectionResult.Hits)
	          {
	               Hit.Document["_collection"] = CollectionName;
	               AllHits.push_back(std::move(Hit));
	          }

          for (const auto &FacetPair : CollectionResult.Facets)
          {
               for (const auto &Count : FacetPair.second.Counts)
               {
                    if (!Count.Value.empty() && Count.Count > 0)
                    {
                         FacetCounts[FacetPair.first][Count.Value] += Count.Count;
                    }
               }
          }

          IndexingInProgress = IndexingInProgress || CollectionResult.IndexingInProgress;
          PartialResults = PartialResults || CollectionResult.PartialResults;
          MaxSearchTime = std::max(MaxSearchTime, CollectionResult.SearchTimeMS);
          ExecutedCollections++;
     }

     /* Strict distributed mode only succeeds when at least one remote-capable execution completed. */

     if (ExecutedCollections == 0 && IsStrictDistributedMode())
     {
          std::string Details = DistributedError.empty() ? "No distributed nodes available to execute global search query." : DistributedError;
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Distributed search unavailable.",
                                    Details);
     }

     /* Global hit ordering uses explicit sort rules first, then normalized search scores. */

     if (!BaseQuery.SortBy.empty())
     {
          AllHits = ApplySorting(AllHits, BaseQuery.SortBy);
     }
     else
     {
          std::stable_sort(AllHits.begin(), AllHits.end(),
                           [](const SearchHit &A, const SearchHit &B)
                           {
                                return ScoreForMergedHit(A) > ScoreForMergedHit(B);
                           });
     }

     ComprehensiveSearchResult GlobalResult;
     GlobalResult.Page = BaseQuery.Page < 1 ? 1 : BaseQuery.Page;
     GlobalResult.PerPage = BaseQuery.PerPage < 1 ? 100 : BaseQuery.PerPage;
     GlobalResult.IndexingInProgress = IndexingInProgress;
     GlobalResult.PartialResults = PartialResults;
     if (PartialResults)
     {
          GlobalResult.PartialReason = "one_or_more_collections_partial";
     }
     GlobalResult.SearchTimeMS = MaxSearchTime;
     GlobalResult.DistributedDiagnostics = std::move(GlobalDistributedDiagnostics);
     GlobalResult.OutOf = (AllHits.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                              ? std::numeric_limits<int>::max()
                              : static_cast<int>(AllHits.size());
     GlobalResult.Found = GlobalResult.OutOf;

     const std::size_t PageIndex = static_cast<std::size_t>(GlobalResult.Page - 1);
     const std::size_t PerPageSize = static_cast<std::size_t>(GlobalResult.PerPage);

     /* Pagination is applied after the full merged result set has been sorted. */

     if (PageIndex <= (std::numeric_limits<std::size_t>::max() / PerPageSize))
     {
          const std::size_t Start = PageIndex * PerPageSize;

          if (Start < AllHits.size())
          {
               const std::size_t End = std::min(AllHits.size(), Start + PerPageSize);
               GlobalResult.Hits.insert(GlobalResult.Hits.end(),
                                        AllHits.begin() + static_cast<std::vector<SearchHit>::difference_type>(Start),
                                        AllHits.begin() + static_cast<std::vector<SearchHit>::difference_type>(End));
          }
     }

     for (const auto &FacetPair : FacetCounts)
     {
          FacetResult Result;
          Result.FieldName = FacetPair.first;
          for (const auto &CountPair : FacetPair.second)
          {
               FacetCount Count;
               Count.Value = CountPair.first;
               Count.Count = CountPair.second;
               Result.Counts.push_back(Count);
          }
          GlobalResult.Facets[FacetPair.first] = Result;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Headers["X-HLQ-Execution-Mode"] = ShouldAttemptDistributedSearch(Request) ? "distributed-global" : "global";
     Response.Body = GenerateComprehensiveSearchResponse(GlobalResult, BaseQuery);
     AttachSearchResponseMeta(Response, BaseQuery, Request, "*");

     /* Collection-search analytics report a wildcard collection because the scope is merged. */

     if (BaseQuery.EnableAnalytics)
     {
          const uint64_t SearchTimeMS = static_cast<uint64_t>(std::max(0.0f, GlobalResult.SearchTimeMS));
          const uint64_t Found = GlobalResult.Found < 0 ? 0U : static_cast<uint64_t>(GlobalResult.Found);
          const uint64_t Returned = static_cast<uint64_t>(GlobalResult.Hits.size());
          const bool Authenticated = !Request.APIKeyID.empty();
          const bool Distributed = ShouldAttemptDistributedSearch(Request);

          const SearchEvent CollectionEvent = BuildSearchEvent(
               BaseQuery.Q,
               BaseQuery.AnalyticsTag,
               "*",
               SearchTimeMS,
               Found,
               Returned,
               Request.RemoteAddress,
               Request.APIKeyID,
               Authenticated,
               Distributed);

          FOREACH_MOD(OnSearchCollection, CollectionEvent);
     }

     return Response;
}

/* HandleMultiSearch multi-collection search endpoint. */

/*
 * SearchAPI::HandleMultiSearch implementation.
 */

HttpResponse SearchAPI::HandleMultiSearch(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     auto SearchRequests = ParseMultiSearchRequest(Request.Body);

     /* Strict distributed mode validates remote availability before processing any sub-request. */

     if (ShouldAttemptDistributedSearch(Request) && IsStrictDistributedMode())
     {
          std::string DistributedError;
          ComprehensiveSearchResult DummyResult;
          (void)TryDistributedSearch(Request, "", ComprehensiveSearchQuery{}, &DummyResult, &DistributedError);

          std::string Details = DistributedError.empty() ? "No distributed nodes available to execute multi-search query." : DistributedError;
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Distributed search unavailable.",
                                    Details);
     }

     nlohmann::json Results = nlohmann::json::array();

     /* Every multi-search entry is executed independently and appended to a parallel results array. */

     for (size_t I = 0; I < SearchRequests.size(); ++I)
     {
          std::string Col = SearchRequests[I].first;

          ComprehensiveSearchQuery QueryObj = SearchRequests[I].second;

          /* Scoped search: Append embedded filters from API key. */

          std::string FiltersToApply;

          /* Per-collection API key checks can reject one sub-query without failing the whole batch. */

          if (!Request.APIKeyID.empty())
          {
               auto *KeyObj = APIKeyManager::Instance().GetKey(Request.APIKeyID);

               if (KeyObj)
               {
                    if (!KeyObj->CanAccessCollection(Col) || !KeyObj->HasAction(Col, APIKeyAction::SEARCH))
                    {
                         nlohmann::json err;
                         err["error"] = "Access to collection '" + Col + "' not allowed for this key";
                         err["code"] = static_cast<int>(Code::COLLECTION_NOT_FOUND);
                         Results.push_back(err);
                         continue;
                    }

                    FiltersToApply = KeyObj->GetEmbeddedFilters(Col);
               }
               else
               {
                    FiltersToApply = Request.EmbeddedFilters;
               }
          }
          else
          {
               FiltersToApply = Request.EmbeddedFilters;
          }

          if (!FiltersToApply.empty())
          {
               if (!QueryObj.FilterBy.empty())
               {
                    QueryObj.FilterBy = "(" + QueryObj.FilterBy + ") && (" + FiltersToApply + ")";
               }
               else
               {
                    QueryObj.FilterBy = FiltersToApply;
               }
          }

          /* Validation thresholds for query parameters. */

          const size_t MaxQueryBytes = 8192;
          const size_t MaxFilterBytes = 8192;
          const size_t MaxFieldBytes = 128;

          std::string ValidationError;

          /* Validation failures are returned inline as result objects for the current batch entry. */

          if (!ValidateQueryInput(QueryObj.Q, &ValidationError, MaxQueryBytes, "q"))
          {
               nlohmann::json err;
               err["error"] = "Invalid search query";
               err["message"] = ValidationError;
               err["code"] = static_cast<int>(Code::SEARCH_INVALID_QUERY);
               Results.push_back(err);
               continue;
          }

          if (SearchQueryUsesWildcard(QueryObj.Q) &&
              Instance && Instance->Config && !Instance->Config->GetIndexingEnableWildcards())
          {
               nlohmann::json err;
               err["error"] = "Wildcard search disabled";
               err["message"] = "Wildcard document search is disabled by configuration (indexing.enable_wildcards=false).";
               err["code"] = static_cast<int>(Code::SEARCH_INVALID_QUERY);
               Results.push_back(err);
               continue;
          }

          if (!ValidateQueryInput(QueryObj.FilterBy, &ValidationError, MaxFilterBytes, "filter_by"))
          {
               nlohmann::json err;
               err["error"] = "Invalid search parameter";
               err["message"] = ValidationError;
               err["code"] = static_cast<int>(Code::SEARCH_INVALID_PARAMETER);
               Results.push_back(err);
               continue;
          }

          bool InvalidQueryBy = false;

          for (const auto &Field : QueryObj.QueryBy)
          {
               if (!ValidateQueryInput(Field, &ValidationError, MaxFieldBytes, "query_by"))
               {
                    nlohmann::json err;
                    err["error"] = "Invalid search parameter";
                    err["message"] = ValidationError;
                    err["code"] = static_cast<int>(Code::SEARCH_INVALID_PARAMETER);
                    Results.push_back(err);
                    InvalidQueryBy = true;
                    break;
               }
          }

          if (InvalidQueryBy)
          {
               continue;
          }

          ComprehensiveSearchResult ResultObj;
          bool UsedDistributed = false;

          /* Multi-search uses the same distributed-first policy as the single search endpoint. */

          if (ShouldAttemptDistributedSearch(Request))
          {
               std::string DistributedError;
               if (TryDistributedSearch(Request, Col, QueryObj, &ResultObj, &DistributedError))
               {
                    UsedDistributed = true;
               }
          }

          if (!UsedDistributed)
          {
               ResultObj = PerformComprehensiveSearch(Col, QueryObj);
          }

          /* Response rendering is converted back into JSON so the batch response stays structured. */

          nlohmann::json result_json;
          try
          {
               result_json = nlohmann::json::parse(GenerateComprehensiveSearchResponse(ResultObj, QueryObj));
          }
          catch (const nlohmann::json::exception &E)
          {
               nlohmann::json err;
               err["error"] = "Failed to render search response";
               err["code"] = static_cast<int>(Code::SEARCH_INVALID_PARAMETER);
               err["message"] = E.what();
               Results.push_back(err);
               continue;
          }

          if (QueryObj.EnableAnalytics)
          {
               const std::string SearchCollection = Col.empty() ? "*" : Col;
               const uint64_t SearchTimeMS = static_cast<uint64_t>(std::max(0.0f, ResultObj.SearchTimeMS));
               const uint64_t Found = ResultObj.Found < 0 ? 0U : static_cast<uint64_t>(ResultObj.Found);
               const uint64_t Returned = static_cast<uint64_t>(ResultObj.Hits.size());
               const bool Authenticated = !Request.APIKeyID.empty();

               const SearchEvent CollectionEvent = BuildSearchEvent(
                    QueryObj.Q,
                    QueryObj.AnalyticsTag,
                    SearchCollection,
                    SearchTimeMS,
                    Found,
                    Returned,
                    Request.RemoteAddress,
                    Request.APIKeyID,
                    Authenticated,
                    UsedDistributed);

               FOREACH_MOD(OnSearchCollection, CollectionEvent);
          }

          Results.push_back(result_json);
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json Root;
     Root["results"] = Results;
     Response.Body = Root.dump();

     return Response;
}

/* PerformComprehensiveSearch core search orchestration logic. */

/*
 * SearchAPI::PerformComprehensiveSearch implementation.
 */

ComprehensiveSearchResult SearchAPI::PerformComprehensiveSearch(const std::string &Collection, const ComprehensiveSearchQuery &Query)
{
     ComprehensiveSearchResult ResultObj;

     constexpr std::size_t MaxPostProcessingHits = 10000;

     /* The result object tracks page metadata even before hit generation begins. */

     ResultObj.Page = Query.Page;
     ResultObj.PerPage = Query.PerPage;

     auto StartTime = Instance->Now();

     std::vector<SearchHit> Hits;
     std::string RankingMode = "lexical";

     /* Choose the search engine path from the presence of vector inputs and hybrid alpha. */

     if (!Query.VectorQueryStr.empty() || !Query.Embedding.empty())
     {
          if (Query.HybridAlpha > 0.0f && Query.HybridAlpha < 1.0f)
          {
               Hits = ProcessHybridSearch(Collection, Query);
               RankingMode = "hybrid";
          }
          else
          {
               Hits = ProcessVectorSearch(Collection, Query);
               RankingMode = "vector";
          }
     }
     else
     {
          Hits = ProcessLexicalSearch(Collection, Query);
          RankingMode = "lexical";
     }

     /* Filters run after raw hit generation so all search modes share one filter path. */

     if (!Query.FilterBy.empty())
     {
          Hits = ApplyFilters(Hits, Query.FilterBy);
     }

     if (Query.VectorQueryStr.empty() && Query.Embedding.empty() && !Query.Q.empty() && Query.Q != "*")
     {
          size_t indexed_count = Instance->SearchIndex->GetDocumentCount(Collection);
          size_t collection_docs = HybridStorageManagerInstance().GetCollectionDocumentCount(Collection);

          /* Probe storage when counters say zero so indexing status remains accurate during warmup. */

          if (collection_docs == 0)
          {
               auto probe = HybridStorageManagerInstance().ListDocuments(Collection, 1, 0);
               if (!probe.empty())
               {
                    collection_docs = 1;
               }
          }

          bool indexing = HybridStorageManagerInstance().IsCollectionIndexing(Collection);
          const bool index_incomplete = collection_docs > 0 && indexed_count < collection_docs;

          /* Mark indexing-in-progress when searchable data exists but the lexical index is incomplete. */

          if ((collection_docs > 0 && indexing) || index_incomplete || (indexed_count == 0 && !Hits.empty()))
          {
               ResultObj.IndexingInProgress = true;
               ResultObj.PartialResults = true;
               ResultObj.PartialReason = indexing ? "indexing_in_progress" : "index_incomplete";
          }
     }

     const int MaxResultCount = std::numeric_limits<int>::max();
     ResultObj.Found = (Hits.size() > static_cast<std::size_t>(MaxResultCount))
                           ? MaxResultCount
                           : static_cast<int>(Hits.size());
     ResultObj.OutOf = ResultObj.Found;

     /* Collection and module weights are applied before sorting so custom ranking affects final order. */

     ApplyCollectionRankWeights(Hits, Collection);
     ApplyModuleWeights(Hits, Collection, Query, RankingMode);

     if (!Query.SortBy.empty())
     {
          Hits = ApplySorting(Hits, Query.SortBy);
     }
     else
     {
          const std::vector<std::string> DefaultSortBy = ResolveDefaultCollectionSortBy(Collection);

          if (!DefaultSortBy.empty())
          {
               Hits = ApplySorting(Hits, DefaultSortBy);
          }
          else
          {
               std::stable_sort(Hits.begin(), Hits.end(), [this](const SearchHit &A, const SearchHit &B)
                                {
                                     return GetEffectiveScore(A) > GetEffectiveScore(B);
                                });
          }
     }

     if (Query.PreserveMatchedHits)
     {
          ResultObj.MatchedHits = Hits;
     }

     const std::vector<SearchHit> *PostProcessingHits = &Hits;
     std::vector<SearchHit> LimitedPostProcessingHits;

     /* Expensive facet and aggregation work is capped to protect large result scans. */

     if (Hits.size() > MaxPostProcessingHits && (!Query.FacetBy.empty() || !Query.Aggregations.empty()))
     {
          LimitedPostProcessingHits.assign(Hits.begin(), Hits.begin() + static_cast<std::ptrdiff_t>(MaxPostProcessingHits));
          PostProcessingHits = &LimitedPostProcessingHits;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api",
                                      "Capping facet/aggregation post-processing to " + std::to_string(MaxPostProcessingHits) +
                                      " hits out of " + std::to_string(Hits.size()) + " for collection '" + Collection + "'.");
          }
     }

     /* Facets and aggregations run before grouping and pagination so counts reflect the larger hit set. */

     if (!Query.FacetBy.empty())
     {
          ResultObj.Facets = GenerateFacets(*PostProcessingHits, Query.FacetBy, Query.FacetQuery, Query.MaxFacetValues);
     }

     if (!Query.Aggregations.empty())
     {
          ResultObj.Aggregations = GenerateAggregations(*PostProcessingHits, Query.Aggregations);
     }

     if (!Query.GroupBy.empty())
     {
          Hits = ApplyGrouping(Hits, Query.GroupBy, Query.GroupLimit);
     }

     /* Pagination happens after scoring, optional grouping, and facet precomputation. */

     if (Query.Offset > 0)
     {
          Hits = ApplyPaginationForSQL(Hits, Query.Page, Query.PerPage, Query.Offset);
     }
     else
     {
          Hits = ApplyPagination(Hits, Query.Page, Query.PerPage);
     }

     /* Field projection trims the payload after pagination to avoid extra document copying. */

     if (!Query.IncludeFields.empty() || !Query.ExcludeFields.empty())
     {
          for (auto &HitObj : Hits)
          {
               HitObj.Document = ApplyFieldSelection(HitObj.Document, Query.IncludeFields, Query.ExcludeFields);
          }
     }

     if (Query.Highlight && !Query.Q.empty() && Query.Q != "*")
     {
          for (auto &HitObj : Hits)
          {
               std::vector<std::string> FieldsToHighlight = Query.HighlightFields;

               /* Default highlighting covers all user-facing fields except ids and timestamps. */

               if (FieldsToHighlight.empty())
               {
                    for (const auto &FieldPair : HitObj.Document)
                    {
                         if (FieldPair.first != "id" && FieldPair.first != "timestamp")
                         {
                              FieldsToHighlight.push_back(FieldPair.first);
                         }
                    }
               }

               HitObj.Highlights = GenerateHighlights(HitObj.Document, Query.Q, FieldsToHighlight);
          }
     }

     ResultObj.Hits = Hits;

     auto EndTime = Instance->Now();

     /* Search time captures the complete orchestration path, including post-processing. */

     ResultObj.SearchTimeMS = std::chrono::duration<float, std::milli>(EndTime - StartTime).count();

     return ResultObj;
}
