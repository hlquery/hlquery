/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "api/searchapi.h"
#include "core/hlquery.h"
#include "vendor/json/json.hpp"

/* GenerateComprehensiveSearchResponse generates a JSON response for a search. */

std::string SearchAPI::GenerateComprehensiveSearchResponse(const ComprehensiveSearchResult &Result, const ComprehensiveSearchQuery &Query)
{
     std::ostringstream JSON;

     JSON << "{";

     JSON << "\"hits\":[";

     for (std::size_t I = 0; I < Result.Hits.size(); ++I)
     {
          if (I > 0)
          {
               JSON << ",";
          }

          const auto &HitObj = Result.Hits[I];

          JSON << "{";

          JSON << "\"document\":{";

          bool FirstField = true;

          for (const auto &FieldPair : HitObj.Document)
          {
               if (!FirstField)
               {
                    JSON << ",";
               }

               JSON << "\"" << EscapeJSONString(FieldPair.first) << "\":\"" << EscapeJSONString(FieldPair.second) << "\"";

               FirstField = false;
          }

	     auto TimestampIt = HitObj.Document.find("timestamp");
	     if (Query.IncludeCreatedAt && TimestampIt != HitObj.Document.end())
	     {
	          std::uint64_t TimestampVal = 0;

               /* Parse timestamp value and ignore invalid inputs. */

	          try
	          {
	               TimestampVal = std::stoull(TimestampIt->second);
	          }
               catch (...)
               {
                    /* Invalid timestamp value. */
               }

               if (TimestampVal > 0)
               {
                    auto TimePointVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(TimestampVal));

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

                         if (!FirstField)
                         {
                              JSON << ",";
                         }

                         JSON << "\"created_at\":\"" << OSS.str() << "\"";
                    }
               }
          }

	          JSON << "}";

	          JSON << ",\"text_match\":" << HitObj.TextMatch;

	          JSON << ",\"_text_match\":" << HitObj.TextMatch;

	          JSON << ",\"weight\":" << HitObj.Weight;

          const float weighted_score = (HitObj.HybridScore > 0.0f ? HitObj.HybridScore : (HitObj.VectorScore > 0.0f ? HitObj.VectorScore : HitObj.TextMatch)) *
                                       ((std::isfinite(HitObj.Weight) && HitObj.Weight > 0.0f) ? HitObj.Weight : 1.0f);

	          JSON << ",\"score\":" << weighted_score;

	          if (!Query.VectorQueryStr.empty() || !Query.Embedding.empty())
	          {
	               JSON << ",\"vector_score\":" << HitObj.VectorScore;
	          }

	          if (HitObj.HybridScore > 0)
	          {
	               JSON << ",\"hybrid_score\":" << HitObj.HybridScore;
	          }

	          if (!HitObj.Highlights.empty())
	          {
	               JSON << ",\"highlights\":{";

	               bool FirstHighlight = true;

               for (const auto &Highlight : HitObj.Highlights)
               {
                    if (!FirstHighlight)
                    {
                         JSON << ",";
                    }

                    std::string EscapedValue = EscapeJSONString(Highlight.second);

                    JSON << "\"" << EscapeJSONString(Highlight.first) << "\":\"" << EscapedValue << "\"";

                    FirstHighlight = false;
               }

	               JSON << "}";
	          }

	          JSON << "}";
	     }

     JSON << "],";

     JSON << "\"found\":" << Result.Found << ",";

     JSON << "\"out_of\":" << Result.OutOf << ",";

     JSON << "\"page\":" << Result.Page << ",";

     JSON << "\"per_page\":" << Result.PerPage << ",";

     const int SafePerPage = Result.PerPage > 0 ? Result.PerPage : 1;
     const std::size_t OutOfSize = Result.OutOf > 0 ? static_cast<std::size_t>(Result.OutOf) : 0;
     const std::size_t PerPageSize = static_cast<std::size_t>(SafePerPage);
     const std::size_t TotalPagesSize = (OutOfSize + PerPageSize - 1) / PerPageSize;
     const int TotalPages = (TotalPagesSize > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                                ? std::numeric_limits<int>::max()
                                : static_cast<int>(TotalPagesSize);

     JSON << "\"total_pages\":" << TotalPages << ",";

     JSON << "\"has_next_page\":" << (Result.Page < TotalPages ? "true" : "false") << ",";

     JSON << "\"has_prev_page\":" << (Result.Page > 1 ? "true" : "false") << ",";

     JSON << "\"indexing_in_progress\":" << (Result.IndexingInProgress ? "true" : "false") << ",";

     if (!Result.Facets.empty())
     {
          JSON << "\"facets\":{";

          bool FirstFacet = true;

          for (const auto &FacetPair : Result.Facets)
          {
               if (!FirstFacet)
               {
                    JSON << ",";
               }

               JSON << "\"" << EscapeJSONString(FacetPair.first) << "\":[";

               for (std::size_t I = 0; I < FacetPair.second.Counts.size(); ++I)
               {
                    if (I > 0)
                    {
                         JSON << ",";
                    }

                    const auto &CountVal = FacetPair.second.Counts[I];

                    JSON << "{\"value\":\"" << EscapeJSONString(CountVal.Value) << "\",\"count\":" << CountVal.Count << "}";
               }

               JSON << "]";

               FirstFacet = false;
          }

          JSON << "},";
     }

     if (!Result.Aggregations.empty())
     {
          JSON << "\"aggregations\":{";

          bool FirstAggregation = true;

          for (const auto &AggregationPair : Result.Aggregations)
          {
               if (!FirstAggregation)
               {
                    JSON << ",";
               }

               JSON << "\"" << EscapeJSONString(AggregationPair.first) << "\":{";
               JSON << "\"type\":\"" << EscapeJSONString(AggregationPair.second.Type) << "\"";

               if (!AggregationPair.second.Metrics.empty())
               {
                    JSON << ",\"metrics\":{";

                    bool FirstMetric = true;

                    for (const auto &MetricPair : AggregationPair.second.Metrics)
                    {
                         if (!FirstMetric)
                         {
                              JSON << ",";
                         }

                         JSON << "\"" << EscapeJSONString(MetricPair.first) << "\":" << MetricPair.second;
                         FirstMetric = false;
                    }

                    JSON << "}";
               }

               if (!AggregationPair.second.Buckets.empty())
               {
                    JSON << ",\"buckets\":[";

                    for (std::size_t I = 0; I < AggregationPair.second.Buckets.size(); ++I)
                    {
                         if (I > 0)
                         {
                              JSON << ",";
                         }

                         const auto &Bucket = AggregationPair.second.Buckets[I];
                         JSON << "{\"key\":\"" << EscapeJSONString(Bucket.Key) << "\",\"doc_count\":" << Bucket.DocCount << "}";
                    }

                    JSON << "]";
               }

               JSON << "}";
               FirstAggregation = false;
          }

          JSON << "},";
     }

     if (!Result.DistributedDiagnostics.empty())
     {
          JSON << "\"distributed_diagnostics\":[";

          for (std::size_t I = 0; I < Result.DistributedDiagnostics.size(); ++I)
          {
               if (I > 0)
               {
                    JSON << ",";
               }

               JSON << "{";

               bool FirstField = true;
               for (const auto &FieldPair : Result.DistributedDiagnostics[I])
               {
                    if (!FirstField)
                    {
                         JSON << ",";
                    }

                    JSON << "\"" << EscapeJSONString(FieldPair.first) << "\":\"" << EscapeJSONString(FieldPair.second) << "\"";
                    FirstField = false;
               }

               JSON << "}";
          }

          JSON << "],";
     }

     JSON << "\"search_time_ms\":" << Result.SearchTimeMS;

     JSON << "}";

     return JSON.str();
}

/* GenerateHTMLSearchResponse generates an HTML response for a search. */

std::string SearchAPI::GenerateHTMLSearchResponse(const ComprehensiveSearchResult &Result, const ComprehensiveSearchQuery &Query, const std::string &CollectionName)
{
     (void)CollectionName;

     std::ostringstream HTML;

     HTML << "<html><head><title>Search Results</title></head><body>";

     HTML << "<h1>Results for: " << EscapeHTMLString(Query.Q) << "</h1>";

     HTML << "<ul>";

     for (const auto &HitObj : Result.Hits)
     {
          HTML << "<li>";

          HTML << "<b>" << EscapeHTMLString(HitObj.Document.count("title") ? HitObj.Document.at("title") : "No Title") << "</b>";

          HTML << " (Score: " << HitObj.TextMatch << ")";

          HTML << "</li>";
     }

     HTML << "</ul>";

     HTML << "</body></html>";

     return HTML.str();
}

/* GenerateFacets generates facet counts for search hits. */

std::map<std::string, FacetResult> SearchAPI::GenerateFacets(const std::vector<SearchHit> &Hits, const std::vector<std::string> &FacetBy, const std::map<std::string, std::string> &FacetQuery, int MaxFacetValues)
{
     (void)FacetQuery;

     std::map<std::string, FacetResult> Facets;

     for (const auto &FieldName : FacetBy)
     {
          FacetResult FacetRes;

          FacetRes.FieldName = FieldName;

          std::map<std::string, int> Counts;

          for (const auto &HitObj : Hits)
          {
               if (HitObj.Document.count(FieldName))
               {
                    std::string Value = HitObj.Document.at(FieldName);

                    Counts[Value]++;
               }
          }

          for (const auto &CountPair : Counts)
          {
               FacetCount FacetCountVal;

               FacetCountVal.Value = CountPair.first;

               FacetCountVal.Count = CountPair.second;

               FacetRes.Counts.push_back(FacetCountVal);
          }

          std::sort(FacetRes.Counts.begin(), FacetRes.Counts.end(), [](const FacetCount &A, const FacetCount &B)
                    {
                         return A.Count > B.Count;
                    });

          if (MaxFacetValues > 0)
          {
               std::size_t MaxFacetSize = static_cast<std::size_t>(MaxFacetValues);

               if (FacetRes.Counts.size() > MaxFacetSize)
               {
                    FacetRes.Counts.resize(MaxFacetSize);
               }
          }

          Facets[FieldName] = FacetRes;
     }

     return Facets;
}

/* GenerateAggregations generates aggregations for search hits. */

std::map<std::string, AggregationResult> SearchAPI::GenerateAggregations(const std::vector<SearchHit> &Hits, const std::string &AggsConfigJson)
{
     std::map<std::string, AggregationResult> Aggregations;

     if (AggsConfigJson.empty())
     {
          return Aggregations;
     }

     try
     {
          nlohmann::json AggsConfig = nlohmann::json::parse(AggsConfigJson);

          auto ParseDouble = [](const std::string &Text, double &Value) -> bool
          {
               if (Text.empty())
               {
                    return false;
               }

               char *End = nullptr;
               const double Parsed = std::strtod(Text.c_str(), &End);

               if (End == Text.c_str() || (End && *End != '\0'))
               {
                    return false;
               }

               Value = Parsed;
               return true;
          };

          for (auto &[AggName, AggConfigVal] : AggsConfig.items())
          {
               AggregationResult AggResultVal;

               AggResultVal.Name = AggName;

               if (AggConfigVal.contains("terms"))
               {
                    AggResultVal.Type = "terms";

                    auto &TermsConfig = AggConfigVal["terms"];

                    std::string FieldVal = TermsConfig.value("field", "");

                    int SizeVal = TermsConfig.value("size", 10);

                    if (!FieldVal.empty())
                    {
                         std::map<std::string, int> BucketCounts;

                         for (const auto &HitObj : Hits)
                         {
                              if (HitObj.Document.count(FieldVal))
                              {
                                   std::string ValueVal = HitObj.Document.at(FieldVal);

                                   BucketCounts[ValueVal]++;
                              }
                         }

                         for (const auto &[KeyVal, CountVal] : BucketCounts)
                         {
                              AggregationBucket Bucket;

                              Bucket.Key = KeyVal;

                              Bucket.DocCount = CountVal;

                              AggResultVal.Buckets.push_back(Bucket);
                         }

                         std::sort(AggResultVal.Buckets.begin(), AggResultVal.Buckets.end(), [](const AggregationBucket &A, const AggregationBucket &B)
                                   {
                                        return A.DocCount > B.DocCount;
                                   });

                         if (SizeVal > 0)
                         {
                              std::size_t SizeValSize = static_cast<std::size_t>(SizeVal);

                              if (AggResultVal.Buckets.size() > SizeValSize)
                              {
                                   AggResultVal.Buckets.resize(SizeValSize);
                              }
                         }
                    }
               }
               else if (AggConfigVal.contains("count"))
               {
                    AggResultVal.Type = "count";

                    const auto &CountConfig = AggConfigVal["count"];
                    const std::string FieldVal = CountConfig.value("field", "");
                    double CountValue = 0.0;

                    if (FieldVal.empty())
                    {
                         CountValue = static_cast<double>(Hits.size());
                    }
                    else
                    {
                         for (const auto &HitObj : Hits)
                         {
                              auto FieldIt = HitObj.Document.find(FieldVal);

                              if (FieldIt != HitObj.Document.end() && !FieldIt->second.empty() && FieldIt->second != "null")
                              {
                                   CountValue += 1.0;
                              }
                         }
                    }

                    AggResultVal.Metrics["value"] = CountValue;
               }
               else
               {
                    static const std::vector<std::string> MetricTypes = {"avg", "sum", "min", "max", "stats"};

                    for (const auto &MetricType : MetricTypes)
                    {
                         if (!AggConfigVal.contains(MetricType))
                         {
                              continue;
                         }

                         AggResultVal.Type = MetricType;

                         const auto &MetricConfig = AggConfigVal[MetricType];
                         const std::string FieldVal = MetricConfig.value("field", "");

                         if (FieldVal.empty())
                         {
                              break;
                         }

                         double SumValue = 0.0;
                         double MinValue = 0.0;
                         double MaxValue = 0.0;
                         double CountValue = 0.0;
                         bool HasValue = false;

                         for (const auto &HitObj : Hits)
                         {
                              auto FieldIt = HitObj.Document.find(FieldVal);

                              if (FieldIt == HitObj.Document.end())
                              {
                                   continue;
                              }

                              double ParsedValue = 0.0;

                              if (!ParseDouble(FieldIt->second, ParsedValue))
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

                         if (MetricType == "avg")
                         {
                              AggResultVal.Metrics["value"] = CountValue > 0.0 ? (SumValue / CountValue) : 0.0;
                         }
                         else if (MetricType == "sum")
                         {
                              AggResultVal.Metrics["value"] = SumValue;
                         }
                         else if (MetricType == "min")
                         {
                              AggResultVal.Metrics["value"] = HasValue ? MinValue : 0.0;
                         }
                         else if (MetricType == "max")
                         {
                              AggResultVal.Metrics["value"] = HasValue ? MaxValue : 0.0;
                         }
                         else if (MetricType == "stats")
                         {
                              AggResultVal.Metrics["count"] = CountValue;
                              AggResultVal.Metrics["sum"] = SumValue;
                              AggResultVal.Metrics["min"] = HasValue ? MinValue : 0.0;
                              AggResultVal.Metrics["max"] = HasValue ? MaxValue : 0.0;
                              AggResultVal.Metrics["avg"] = CountValue > 0.0 ? (SumValue / CountValue) : 0.0;
                         }

                         break;
                    }
               }

               Aggregations[AggName] = AggResultVal;
          }
     }
     catch (...)
     {
     }

     return Aggregations;
}
