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
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "search/rfusion.h"
#include "vendor/json/json.hpp"

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

namespace
{
enum class VectorMetric
{
     Cosine,
     Dot,
     L2,
     L1
};

struct VectorPayload
{
     std::vector<float> Dense;
     std::unordered_map<int, float> Sparse;
     int MaxSparseIndex = -1;

     void Reset()
     {
          Dense.clear();
          Sparse.clear();
          MaxSparseIndex = -1;
     }

     bool Empty() const
     {
          return Dense.empty() && Sparse.empty();
     }

     bool IsSparse() const
     {
          return !Sparse.empty() && Dense.empty();
     }
};

struct ParsedVectorQuery
{
     std::string FieldName = "embedding";
     VectorPayload Payload;
     float Weight = 1.0f;
     VectorMetric Metric = VectorMetric::Cosine;
     float Threshold = -std::numeric_limits<float>::infinity();
     bool Normalize = false;
     float MaxDistance = std::numeric_limits<float>::infinity();
     float MinDistance = -std::numeric_limits<float>::infinity();
     bool HasMaxDistance = false;
     bool HasMinDistance = false;
};

struct ParsedVectorRequest
{
     std::vector<ParsedVectorQuery> Queries;
     std::string FusionMethod = "weighted_sum";
     int RrfK = 60;
     int TopK = 10;
     int HnswEfSearch = 0;
     int IvfNProbe = 0;
     bool IsLinear = false;
     bool IsUsingRefiner = false;
};

struct ScoreStats
{
     float Min = 0.0f;
     float Max = 0.0f;
     float Mean = 0.0f;
     float StdDev = 0.0f;
     bool HasValues = false;
};

static std::string NormalizeLowerCopy(const std::string &Value)
{
     std::string Lower = Value;
     std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Lower;
}

static ScoreStats CalculateScoreStats(const std::vector<float> &Values)
{
     ScoreStats StatsObj;
     if (Values.empty())
     {
          return StatsObj;
     }

     StatsObj.HasValues = true;
     StatsObj.Min = *std::min_element(Values.begin(), Values.end());
     StatsObj.Max = *std::max_element(Values.begin(), Values.end());
     float Sum = std::accumulate(Values.begin(), Values.end(), 0.0f);
     StatsObj.Mean = Sum / static_cast<float>(Values.size());

     float Variance = 0.0f;
     for (float Value : Values)
     {
          float Delta = Value - StatsObj.Mean;
          Variance += Delta * Delta;
     }

     Variance /= static_cast<float>(Values.size());
     StatsObj.StdDev = std::sqrt(std::max(0.0f, Variance));
     return StatsObj;
}

static VectorMetric ParseVectorMetric(const std::string &Raw)
{
     const std::string Metric = NormalizeLowerCopy(Raw);
     if (Metric == "dot" || Metric == "dot_product" || Metric == "inner_product" || Metric == "ip")
     {
          return VectorMetric::Dot;
     }
     if (Metric == "l2" || Metric == "euclidean")
     {
          return VectorMetric::L2;
     }
     if (Metric == "l1" || Metric == "manhattan")
     {
          return VectorMetric::L1;
     }
     return VectorMetric::Cosine;
}

static float GetVectorValue(const VectorPayload &Payload, int Index)
{
     if (Index >= 0 && Index < static_cast<int>(Payload.Dense.size()))
     {
          return Payload.Dense[Index];
     }

     auto It = Payload.Sparse.find(Index);
     if (It != Payload.Sparse.end())
     {
          return It->second;
     }

     return 0.0f;
}

static bool ParseVectorPayloadFromJsonValue(const nlohmann::json &Value, VectorPayload *Out)
{
     if (!Out)
     {
          return false;
     }

     Out->Reset();

     if (Value.is_object())
     {
          int MaxIndex = -1;
          for (auto It = Value.begin(); It != Value.end(); ++It)
          {
               if (!It.value().is_number())
               {
                    return false;
               }

               int Index = -1;
               try
               {
                    size_t ParsedCount = 0;
                    Index = std::stoi(It.key(), &ParsedCount);
                    if (ParsedCount != It.key().size())
                    {
                         return false;
                    }
               }
               catch (...)
               {
                    return false;
               }

               if (Index < 0 || Index > 65535)
               {
                    return false;
               }

               float ValueFloat = It.value().get<float>();
               Out->Sparse[Index] = ValueFloat;
               MaxIndex = std::max(MaxIndex, Index);
          }

          if (Out->Sparse.empty())
          {
               return false;
          }

          Out->MaxSparseIndex = MaxIndex;
          return true;
     }

     if (Value.is_array())
     {
          Out->Dense.reserve(Value.size());
          for (const auto &Elem : Value)
          {
               if (!Elem.is_number())
               {
                    return false;
               }

               Out->Dense.push_back(Elem.get<float>());
          }

          return !Out->Dense.empty();
     }

     return false;
}

static bool ParseSingleVectorQuery(const nlohmann::json &Obj, ParsedVectorQuery *OutQuery)
{
     if (!OutQuery || !Obj.is_object())
     {
          return false;
     }

     ParsedVectorQuery Query;

     if (Obj.contains("field_name") && Obj["field_name"].is_string())
     {
          Query.FieldName = Obj["field_name"].get<std::string>();
     }
     else if (Obj.contains("fieldName") && Obj["fieldName"].is_string())
     {
          Query.FieldName = Obj["fieldName"].get<std::string>();
     }
     else if (Obj.contains("field") && Obj["field"].is_string())
     {
          Query.FieldName = Obj["field"].get<std::string>();
     }

     if (Obj.contains("metric_type") && Obj["metric_type"].is_string())
     {
          Query.Metric = ParseVectorMetric(Obj["metric_type"].get<std::string>());
     }
     else if (Obj.contains("metricType") && Obj["metricType"].is_string())
     {
          Query.Metric = ParseVectorMetric(Obj["metricType"].get<std::string>());
     }
     else if (Obj.contains("metric") && Obj["metric"].is_string())
     {
          Query.Metric = ParseVectorMetric(Obj["metric"].get<std::string>());
     }
     else if (Obj.contains("distance_metric") && Obj["distance_metric"].is_string())
     {
          Query.Metric = ParseVectorMetric(Obj["distance_metric"].get<std::string>());
     }
     else if (Obj.contains("distanceMetric") && Obj["distanceMetric"].is_string())
     {
          Query.Metric = ParseVectorMetric(Obj["distanceMetric"].get<std::string>());
     }

     if (Obj.contains("normalize") && Obj["normalize"].is_boolean())
     {
          Query.Normalize = Obj["normalize"].get<bool>();
     }

     if (Obj.contains("weight") && Obj["weight"].is_number())
     {
          Query.Weight = Obj["weight"].get<float>();
          if (Query.Weight <= 0.0f)
          {
               Query.Weight = 1.0f;
          }
     }

     if (Obj.contains("threshold") && Obj["threshold"].is_number())
     {
          Query.Threshold = Obj["threshold"].get<float>();
     }
     else if (Obj.contains("score_threshold") && Obj["score_threshold"].is_number())
     {
          Query.Threshold = Obj["score_threshold"].get<float>();
     }
     else if (Obj.contains("min_score") && Obj["min_score"].is_number())
     {
          Query.Threshold = Obj["min_score"].get<float>();
     }
     else if (Obj.contains("scoreThreshold") && Obj["scoreThreshold"].is_number())
     {
          Query.Threshold = Obj["scoreThreshold"].get<float>();
     }

     if (Obj.contains("radius") && Obj["radius"].is_number())
     {
          Query.MaxDistance = std::max(0.0f, Obj["radius"].get<float>());
          Query.HasMaxDistance = true;
     }
     else if (Obj.contains("max_distance") && Obj["max_distance"].is_number())
     {
          Query.MaxDistance = std::max(0.0f, Obj["max_distance"].get<float>());
          Query.HasMaxDistance = true;
     }
     else if (Obj.contains("maxDistance") && Obj["maxDistance"].is_number())
     {
          Query.MaxDistance = std::max(0.0f, Obj["maxDistance"].get<float>());
          Query.HasMaxDistance = true;
     }

     if (Obj.contains("range_filter") && Obj["range_filter"].is_number())
     {
          Query.MinDistance = Obj["range_filter"].get<float>();
          Query.HasMinDistance = true;
     }
     else if (Obj.contains("min_distance") && Obj["min_distance"].is_number())
     {
          Query.MinDistance = Obj["min_distance"].get<float>();
          Query.HasMinDistance = true;
     }
     else if (Obj.contains("rangeFilter") && Obj["rangeFilter"].is_number())
     {
          Query.MinDistance = Obj["rangeFilter"].get<float>();
          Query.HasMinDistance = true;
     }
     else if (Obj.contains("minDistance") && Obj["minDistance"].is_number())
     {
          Query.MinDistance = Obj["minDistance"].get<float>();
          Query.HasMinDistance = true;
     }

     bool HasVector = false;
     if (Obj.contains("vector"))
     {
          HasVector = ParseVectorPayloadFromJsonValue(Obj["vector"], &Query.Payload);
     }
     else if (Obj.contains("vectorQuery"))
     {
          HasVector = ParseVectorPayloadFromJsonValue(Obj["vectorQuery"], &Query.Payload);
     }
     else if (Obj.contains("embedding"))
     {
          HasVector = ParseVectorPayloadFromJsonValue(Obj["embedding"], &Query.Payload);
     }

     if (!HasVector || Query.Payload.Empty())
     {
          return false;
     }

     *OutQuery = std::move(Query);
     return true;
}

static ParsedVectorRequest ParseVectorRequest(const nlohmann::json &Root, int DefaultTopK)
{
     ParsedVectorRequest Parsed;
     Parsed.TopK = std::max(1, DefaultTopK);
     bool HasGlobalRadius = false;
     bool HasGlobalRangeFilter = false;
     float GlobalRadius = std::numeric_limits<float>::infinity();
     float GlobalRangeFilter = -std::numeric_limits<float>::infinity();

     if (Root.is_array())
     {
          ParsedVectorQuery Query;
          if (ParseVectorPayloadFromJsonValue(Root, &Query.Payload))
          {
               Parsed.Queries.push_back(std::move(Query));
          }
          return Parsed;
     }

     if (!Root.is_object())
     {
          return Parsed;
     }

     auto ParseQueryParamsObject = [&](const nlohmann::json &ParamsObj)
     {
          if (!ParamsObj.is_object())
          {
               return;
          }

          if (ParamsObj.contains("hnsw_ef_search") && ParamsObj["hnsw_ef_search"].is_number_integer())
          {
               Parsed.HnswEfSearch = std::max(1, ParamsObj["hnsw_ef_search"].get<int>());
          }
          else if (ParamsObj.contains("ef_search") && ParamsObj["ef_search"].is_number_integer())
          {
               Parsed.HnswEfSearch = std::max(1, ParamsObj["ef_search"].get<int>());
          }
          else if (ParamsObj.contains("ef") && ParamsObj["ef"].is_number_integer())
          {
               Parsed.HnswEfSearch = std::max(1, ParamsObj["ef"].get<int>());
          }

          if (ParamsObj.contains("ivf_nprobe") && ParamsObj["ivf_nprobe"].is_number_integer())
          {
               Parsed.IvfNProbe = std::max(1, ParamsObj["ivf_nprobe"].get<int>());
          }
          else if (ParamsObj.contains("nprobe") && ParamsObj["nprobe"].is_number_integer())
          {
               Parsed.IvfNProbe = std::max(1, ParamsObj["nprobe"].get<int>());
          }

          if (ParamsObj.contains("is_linear") && ParamsObj["is_linear"].is_boolean())
          {
               Parsed.IsLinear = ParamsObj["is_linear"].get<bool>();
          }

          if (ParamsObj.contains("is_using_refiner") && ParamsObj["is_using_refiner"].is_boolean())
          {
               Parsed.IsUsingRefiner = ParamsObj["is_using_refiner"].get<bool>();
          }

          if (ParamsObj.contains("radius") && ParamsObj["radius"].is_number())
          {
               HasGlobalRadius = true;
               GlobalRadius = std::max(0.0f, ParamsObj["radius"].get<float>());
          }
          else if (ParamsObj.contains("max_distance") && ParamsObj["max_distance"].is_number())
          {
               HasGlobalRadius = true;
               GlobalRadius = std::max(0.0f, ParamsObj["max_distance"].get<float>());
          }

          if (ParamsObj.contains("range_filter") && ParamsObj["range_filter"].is_number())
          {
               HasGlobalRangeFilter = true;
               GlobalRangeFilter = ParamsObj["range_filter"].get<float>();
          }
          else if (ParamsObj.contains("min_distance") && ParamsObj["min_distance"].is_number())
          {
               HasGlobalRangeFilter = true;
               GlobalRangeFilter = ParamsObj["min_distance"].get<float>();
          }
     };

     if (Root.contains("topk") && Root["topk"].is_number_integer())
     {
          Parsed.TopK = std::max(1, Root["topk"].get<int>());
     }
     else if (Root.contains("topK") && Root["topK"].is_number_integer())
     {
          Parsed.TopK = std::max(1, Root["topK"].get<int>());
     }
     else if (Root.contains("top_k") && Root["top_k"].is_number_integer())
     {
          Parsed.TopK = std::max(1, Root["top_k"].get<int>());
     }
     else if (Root.contains("k") && Root["k"].is_number_integer())
     {
          Parsed.TopK = std::max(1, Root["k"].get<int>());
     }
     else if (Root.contains("limit") && Root["limit"].is_number_integer())
     {
          Parsed.TopK = std::max(1, Root["limit"].get<int>());
     }

     if (Root.contains("hnsw_ef_search") && Root["hnsw_ef_search"].is_number_integer())
     {
          Parsed.HnswEfSearch = std::max(1, Root["hnsw_ef_search"].get<int>());
     }
     else if (Root.contains("ef_search") && Root["ef_search"].is_number_integer())
     {
          Parsed.HnswEfSearch = std::max(1, Root["ef_search"].get<int>());
     }
     else if (Root.contains("ef") && Root["ef"].is_number_integer())
     {
          Parsed.HnswEfSearch = std::max(1, Root["ef"].get<int>());
     }

     if (Root.contains("ivf_nprobe") && Root["ivf_nprobe"].is_number_integer())
     {
          Parsed.IvfNProbe = std::max(1, Root["ivf_nprobe"].get<int>());
     }
     else if (Root.contains("nprobe") && Root["nprobe"].is_number_integer())
     {
          Parsed.IvfNProbe = std::max(1, Root["nprobe"].get<int>());
     }

     if (Root.contains("is_linear") && Root["is_linear"].is_boolean())
     {
          Parsed.IsLinear = Root["is_linear"].get<bool>();
     }

     if (Root.contains("is_using_refiner") && Root["is_using_refiner"].is_boolean())
     {
          Parsed.IsUsingRefiner = Root["is_using_refiner"].get<bool>();
     }

     if (Root.contains("radius") && Root["radius"].is_number())
     {
          HasGlobalRadius = true;
          GlobalRadius = std::max(0.0f, Root["radius"].get<float>());
     }
     else if (Root.contains("max_distance") && Root["max_distance"].is_number())
     {
          HasGlobalRadius = true;
          GlobalRadius = std::max(0.0f, Root["max_distance"].get<float>());
     }

     if (Root.contains("range_filter") && Root["range_filter"].is_number())
     {
          HasGlobalRangeFilter = true;
          GlobalRangeFilter = Root["range_filter"].get<float>();
     }
     else if (Root.contains("min_distance") && Root["min_distance"].is_number())
     {
          HasGlobalRangeFilter = true;
          GlobalRangeFilter = Root["min_distance"].get<float>();
     }

     if (Root.contains("query_params"))
     {
          ParseQueryParamsObject(Root["query_params"]);
     }
     if (Root.contains("queryParams"))
     {
          ParseQueryParamsObject(Root["queryParams"]);
     }
     if (Root.contains("params"))
     {
          ParseQueryParamsObject(Root["params"]);
     }

     if (Root.contains("fusion"))
     {
          if (Root["fusion"].is_string())
          {
               Parsed.FusionMethod = NormalizeLowerCopy(Root["fusion"].get<std::string>());
          }
          else if (Root["fusion"].is_object())
          {
               const auto &Fusion = Root["fusion"];
               if (Fusion.contains("method") && Fusion["method"].is_string())
               {
                    Parsed.FusionMethod = NormalizeLowerCopy(Fusion["method"].get<std::string>());
               }
               if (Fusion.contains("rrf_k") && Fusion["rrf_k"].is_number_integer())
               {
                    Parsed.RrfK = std::max(1, Fusion["rrf_k"].get<int>());
               }
          }
     }

     if (Root.contains("vector_queries") && Root["vector_queries"].is_array())
     {
          for (const auto &Entry : Root["vector_queries"])
          {
               ParsedVectorQuery Query;
               if (ParseSingleVectorQuery(Entry, &Query))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
     }
     else if (Root.contains("vectorQueries") && Root["vectorQueries"].is_array())
     {
          for (const auto &Entry : Root["vectorQueries"])
          {
               ParsedVectorQuery Query;
               if (ParseSingleVectorQuery(Entry, &Query))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
     }
     else if (Root.contains("vectors") && Root["vectors"].is_array())
     {
          for (const auto &Entry : Root["vectors"])
          {
               ParsedVectorQuery Query;
               if (ParseSingleVectorQuery(Entry, &Query))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
     }
     else if (Root.contains("vector_query"))
     {
          if (Root["vector_query"].is_object())
          {
               ParsedVectorQuery Query;
               if (ParseSingleVectorQuery(Root["vector_query"], &Query))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
          else if (Root["vector_query"].is_array())
          {
               ParsedVectorQuery Query;
               if (ParseVectorPayloadFromJsonValue(Root["vector_query"], &Query.Payload))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
     }
     else if (Root.contains("vectorQuery"))
     {
          if (Root["vectorQuery"].is_object())
          {
               ParsedVectorQuery Query;
               if (ParseSingleVectorQuery(Root["vectorQuery"], &Query))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
          else if (Root["vectorQuery"].is_array())
          {
               ParsedVectorQuery Query;
               if (ParseVectorPayloadFromJsonValue(Root["vectorQuery"], &Query.Payload))
               {
                    Parsed.Queries.push_back(std::move(Query));
               }
          }
     }
     else
     {
          ParsedVectorQuery Query;
          if (ParseSingleVectorQuery(Root, &Query))
          {
               Parsed.Queries.push_back(std::move(Query));
          }
     }

     if (HasGlobalRadius || HasGlobalRangeFilter)
     {
          for (auto &QueryObj : Parsed.Queries)
          {
               if (HasGlobalRadius && !QueryObj.HasMaxDistance)
               {
                    QueryObj.MaxDistance = GlobalRadius;
                    QueryObj.HasMaxDistance = true;
               }
               if (HasGlobalRangeFilter && !QueryObj.HasMinDistance)
               {
                    QueryObj.MinDistance = GlobalRangeFilter;
                    QueryObj.HasMinDistance = true;
               }
          }
     }

     return Parsed;
}

static float NormalizeScore(float RawValue, const ScoreStats &StatsObj, const std::string &Method, bool Enabled)
{
     if (!Enabled || !StatsObj.HasValues)
     {
          return RawValue;
     }

     if (Method == "zscore")
     {
          if (StatsObj.StdDev <= 1e-9f)
          {
               return 0.5f;
          }

          float Z = (RawValue - StatsObj.Mean) / StatsObj.StdDev;
          return 1.0f / (1.0f + std::exp(-Z));
     }

     float Range = StatsObj.Max - StatsObj.Min;
     if (Range <= 1e-9f)
     {
          return 1.0f;
     }

     float Value = (RawValue - StatsObj.Min) / Range;
     if (Value < 0.0f)
     {
          return 0.0f;
     }
     if (Value > 1.0f)
     {
          return 1.0f;
     }
     return Value;
}

static int CountQueryTerms(const std::string &Query)
{
     int Count = 0;
     bool InToken = false;
     bool InQuote = false;

     for (char C : Query)
     {
          unsigned char UC = static_cast<unsigned char>(C);
          if (C == '"')
          {
               if (InToken)
               {
                    Count++;
                    InToken = false;
               }
               InQuote = !InQuote;
               continue;
          }

          if (std::isspace(UC) != 0 && !InQuote)
          {
               if (InToken)
               {
                    Count++;
                    InToken = false;
               }
               continue;
          }

          InToken = true;
     }

     if (InToken)
     {
          Count++;
     }

     return std::max(0, Count);
}

static float ComputeVectorMagnitude(const VectorPayload &Payload)
{
     if (Payload.Empty())
     {
          return 0.0f;
     }

     float Sum = 0.0f;
     if (Payload.IsSparse())
     {
          for (const auto &Pair : Payload.Sparse)
          {
               Sum += Pair.second * Pair.second;
          }
     }
     else
     {
          for (float Value : Payload.Dense)
          {
               Sum += Value * Value;
          }
     }

     return std::sqrt(std::max(0.0f, Sum));
}

static void NormalizeVectorPayload(VectorPayload *Payload)
{
     if (!Payload || Payload->Empty())
     {
          return;
     }

     float Norm = ComputeVectorMagnitude(*Payload);
     if (Norm <= 1e-12f)
     {
          return;
     }

     if (Payload->IsSparse())
     {
          for (auto &Pair : Payload->Sparse)
          {
               Pair.second /= Norm;
          }
     }
     else
     {
          for (float &Value : Payload->Dense)
          {
               Value /= Norm;
          }
     }
}

static void VisitVectorDimensions(const VectorPayload &Query,
                                  const VectorPayload &Doc,
                                  const std::function<void(int, float, float)> &Visitor)
{
     int MaxDense = static_cast<int>(std::max(Query.Dense.size(), Doc.Dense.size()));

     for (int Index = 0; Index < MaxDense; ++Index)
     {
          Visitor(Index, GetVectorValue(Query, Index), GetVectorValue(Doc, Index));
     }

     std::unordered_set<int> ExtraIndices;
     auto CollectIndices = [&](const std::unordered_map<int, float> &Map)
     {
          for (const auto &Pair : Map)
          {
               if (Pair.first >= MaxDense)
               {
                    ExtraIndices.insert(Pair.first);
               }
          }
     };

     CollectIndices(Query.Sparse);
     CollectIndices(Doc.Sparse);

     for (int Index : ExtraIndices)
     {
          Visitor(Index, GetVectorValue(Query, Index), GetVectorValue(Doc, Index));
     }
}

static float ComputeDotProduct(const VectorPayload &QueryVector, const VectorPayload &DocVector)
{
     float Sum = 0.0f;
     VisitVectorDimensions(QueryVector, DocVector, [&](int, float Q, float D)
                           {
                                Sum += Q * D;
                           });
     return Sum;
}

static float ComputeL2Distance(const VectorPayload &QueryVector, const VectorPayload &DocVector)
{
     float Sum = 0.0f;
     VisitVectorDimensions(QueryVector, DocVector, [&](int, float Q, float D)
                           {
                                float Delta = Q - D;
                                Sum += Delta * Delta;
                           });
     return std::sqrt(std::max(0.0f, Sum));
}

static float ComputeL1Distance(const VectorPayload &QueryVector, const VectorPayload &DocVector)
{
     float Sum = 0.0f;
     VisitVectorDimensions(QueryVector, DocVector, [&](int, float Q, float D)
                           {
                                Sum += std::fabs(Q - D);
                           });
     return Sum;
}

static float ComputeSimilarityByMetric(const VectorPayload &QueryVector,
                                       const VectorPayload &DocVector,
                                       VectorMetric Metric)
{
     if (QueryVector.Empty() || DocVector.Empty())
     {
          return 0.0f;
     }

     if (Metric == VectorMetric::Dot)
     {
          return ComputeDotProduct(QueryVector, DocVector);
     }

     if (Metric == VectorMetric::L2)
     {
          float Distance = ComputeL2Distance(QueryVector, DocVector);
          return 1.0f / (1.0f + Distance);
     }

     if (Metric == VectorMetric::L1)
     {
          float Distance = ComputeL1Distance(QueryVector, DocVector);
          return 1.0f / (1.0f + Distance);
     }

     float NormQ = ComputeVectorMagnitude(QueryVector);
     float NormD = ComputeVectorMagnitude(DocVector);
     if (NormQ <= 1e-12f || NormD <= 1e-12f)
     {
          return 0.0f;
     }

     return ComputeDotProduct(QueryVector, DocVector) / (NormQ * NormD);
}

static float ComputeDistanceByMetric(const VectorPayload &QueryVector,
                                     const VectorPayload &DocVector,
                                     VectorMetric Metric)
{
     if (QueryVector.Empty() || DocVector.Empty())
     {
          return std::numeric_limits<float>::infinity();
     }

     if (Metric == VectorMetric::L2)
     {
          return ComputeL2Distance(QueryVector, DocVector);
     }

     if (Metric == VectorMetric::L1)
     {
          return ComputeL1Distance(QueryVector, DocVector);
     }

     if (Metric == VectorMetric::Dot)
     {
          return -ComputeDotProduct(QueryVector, DocVector);
     }

     float Similarity = ComputeSimilarityByMetric(QueryVector, DocVector, VectorMetric::Cosine);
     return 1.0f - Similarity;
}
}
/* HandleVectorSearch top-level vector search handler. */

/*
 * SearchAPI::HandleVectorSearch implementation.
 */

HttpResponse SearchAPI::HandleVectorSearch(const HttpRequest &Request)
{
     if (Request.Method != "POST" && Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::COLLECTION_INVALID_NAME, "Invalid collection name.");
     }

     ComprehensiveSearchQuery SearchQueryObj;

     try
     {
          nlohmann::json JsonBodyObj;

          if (Request.Method == "POST")
          {
               JsonBodyObj = nlohmann::json::parse(Request.Body);
          }
          else
          {
               JsonBodyObj = nlohmann::json::object();
               for (const auto &ParamPair : Request.QueryParams)
               {
                    const std::string &Key = ParamPair.first;
                    const std::string &RawValue = ParamPair.second;
                    if (RawValue.empty())
                    {
                         JsonBodyObj[Key] = "";
                         continue;
                    }

                    try
                    {
                         JsonBodyObj[Key] = nlohmann::json::parse(RawValue);
                    }
                    catch (...)
                    {
                         JsonBodyObj[Key] = RawValue;
                    }
               }
          }

          SearchQueryObj.VectorQueryStr = JsonBodyObj.dump();

          if (JsonBodyObj.is_object())
          {
               if (JsonBodyObj.contains("q") && JsonBodyObj["q"].is_string())
               {
                    SearchQueryObj.Q = JsonBodyObj["q"].get<std::string>();
               }

               if (JsonBodyObj.contains("query_by"))
               {
                    if (JsonBodyObj["query_by"].is_string())
                    {
                         SearchQueryObj.QueryBy = ParseCommaSeparated(JsonBodyObj["query_by"].get<std::string>());
                    }
                    else if (JsonBodyObj["query_by"].is_array())
                    {
                         for (const auto &FieldVal : JsonBodyObj["query_by"])
                         {
                              if (FieldVal.is_string())
                              {
                                   SearchQueryObj.QueryBy.push_back(FieldVal.get<std::string>());
                              }
                         }
                    }
               }
               else if (JsonBodyObj.contains("queryBy"))
               {
                    if (JsonBodyObj["queryBy"].is_string())
                    {
                         SearchQueryObj.QueryBy = ParseCommaSeparated(JsonBodyObj["queryBy"].get<std::string>());
                    }
                    else if (JsonBodyObj["queryBy"].is_array())
                    {
                         for (const auto &FieldVal : JsonBodyObj["queryBy"])
                         {
                              if (FieldVal.is_string())
                              {
                                   SearchQueryObj.QueryBy.push_back(FieldVal.get<std::string>());
                              }
                         }
                    }
               }

               if (JsonBodyObj.contains("topk") && JsonBodyObj["topk"].is_number_integer())
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["topk"].get<int>());
               }
               else if (JsonBodyObj.contains("topK") && JsonBodyObj["topK"].is_number_integer())
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["topK"].get<int>());
               }
               else if (JsonBodyObj.contains("top_k") && JsonBodyObj["top_k"].is_number_integer())
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["top_k"].get<int>());
               }
               else if (JsonBodyObj.contains("k") && JsonBodyObj["k"].is_number_integer())
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["k"].get<int>());
               }
               else if (JsonBodyObj.contains("limit") && JsonBodyObj["limit"].is_number_integer())
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["limit"].get<int>());
               }

               if (JsonBodyObj.contains("per_page"))
               {
                    SearchQueryObj.PerPage = std::max(1, JsonBodyObj["per_page"].get<int>());
               }

               if (JsonBodyObj.contains("offset") && JsonBodyObj["offset"].is_number_integer())
               {
                    int Offset = std::max(0, JsonBodyObj["offset"].get<int>());
                    SearchQueryObj.Page = (Offset / std::max(1, SearchQueryObj.PerPage)) + 1;
               }

               if (JsonBodyObj.contains("page"))
               {
                    SearchQueryObj.Page = JsonBodyObj["page"].get<int>();
               }

               if (JsonBodyObj.contains("filter_by"))
               {
                    SearchQueryObj.FilterBy = JsonBodyObj["filter_by"].get<std::string>();
               }
               else if (JsonBodyObj.contains("filterBy") && JsonBodyObj["filterBy"].is_string())
               {
                    SearchQueryObj.FilterBy = JsonBodyObj["filterBy"].get<std::string>();
               }
               else if (JsonBodyObj.contains("filter") && JsonBodyObj["filter"].is_string())
               {
                    SearchQueryObj.FilterBy = JsonBodyObj["filter"].get<std::string>();
               }

               if (JsonBodyObj.contains("hybrid_alpha") && JsonBodyObj["hybrid_alpha"].is_number())
               {
                    SearchQueryObj.HybridAlpha = std::max(0.0f, std::min(1.0f, JsonBodyObj["hybrid_alpha"].get<float>()));
               }
               else if (JsonBodyObj.contains("hybridAlpha") && JsonBodyObj["hybridAlpha"].is_number())
               {
                    SearchQueryObj.HybridAlpha = std::max(0.0f, std::min(1.0f, JsonBodyObj["hybridAlpha"].get<float>()));
               }

               if (JsonBodyObj.contains("output_fields"))
               {
                    if (JsonBodyObj["output_fields"].is_string())
                    {
                         SearchQueryObj.IncludeFields = ParseCommaSeparated(JsonBodyObj["output_fields"].get<std::string>());
                    }
                    else if (JsonBodyObj["output_fields"].is_array())
                    {
                         for (const auto &FieldVal : JsonBodyObj["output_fields"])
                         {
                              if (FieldVal.is_string())
                              {
                                   SearchQueryObj.IncludeFields.push_back(FieldVal.get<std::string>());
                              }
                         }
                    }

                    if (!SearchQueryObj.IncludeFields.empty())
                    {
                         bool HasID = false;
                         for (const auto &FieldName : SearchQueryObj.IncludeFields)
                         {
                              if (FieldName == "id")
                              {
                                   HasID = true;
                                   break;
                              }
                         }
                         if (!HasID)
                         {
                              SearchQueryObj.IncludeFields.push_back("id");
                         }
                    }
               }
               else if (JsonBodyObj.contains("outputFields"))
               {
                    if (JsonBodyObj["outputFields"].is_string())
                    {
                         SearchQueryObj.IncludeFields = ParseCommaSeparated(JsonBodyObj["outputFields"].get<std::string>());
                    }
                    else if (JsonBodyObj["outputFields"].is_array())
                    {
                         for (const auto &FieldVal : JsonBodyObj["outputFields"])
                         {
                              if (FieldVal.is_string())
                              {
                                   SearchQueryObj.IncludeFields.push_back(FieldVal.get<std::string>());
                              }
                         }
                    }

                    if (!SearchQueryObj.IncludeFields.empty())
                    {
                         bool HasID = false;
                         for (const auto &FieldName : SearchQueryObj.IncludeFields)
                         {
                              if (FieldName == "id")
                              {
                                   HasID = true;
                                   break;
                              }
                         }
                         if (!HasID)
                         {
                              SearchQueryObj.IncludeFields.push_back("id");
                         }
                    }
               }

               bool IncludeVector = true;
               if (JsonBodyObj.contains("include_vector"))
               {
                    if (JsonBodyObj["include_vector"].is_boolean())
                    {
                         IncludeVector = JsonBodyObj["include_vector"].get<bool>();
                    }
                    else if (JsonBodyObj["include_vector"].is_number_integer())
                    {
                         IncludeVector = JsonBodyObj["include_vector"].get<int>() != 0;
                    }
               }
               else if (JsonBodyObj.contains("includeVector"))
               {
                    if (JsonBodyObj["includeVector"].is_boolean())
                    {
                         IncludeVector = JsonBodyObj["includeVector"].get<bool>();
                    }
                    else if (JsonBodyObj["includeVector"].is_number_integer())
                    {
                         IncludeVector = JsonBodyObj["includeVector"].get<int>() != 0;
                    }
               }

               if (JsonBodyObj.contains("include_distance"))
               {
                    if (JsonBodyObj["include_distance"].is_boolean())
                    {
                         SearchQueryObj.IncludeVectorDistance = JsonBodyObj["include_distance"].get<bool>();
                    }
                    else if (JsonBodyObj["include_distance"].is_number_integer())
                    {
                         SearchQueryObj.IncludeVectorDistance = JsonBodyObj["include_distance"].get<int>() != 0;
                    }
               }
               else if (JsonBodyObj.contains("includeDistance"))
               {
                    if (JsonBodyObj["includeDistance"].is_boolean())
                    {
                         SearchQueryObj.IncludeVectorDistance = JsonBodyObj["includeDistance"].get<bool>();
                    }
                    else if (JsonBodyObj["includeDistance"].is_number_integer())
                    {
                         SearchQueryObj.IncludeVectorDistance = JsonBodyObj["includeDistance"].get<int>() != 0;
                    }
               }

               if (!IncludeVector)
               {
                    ParsedVectorRequest ParsedRequest = ParseVectorRequest(JsonBodyObj, SearchQueryObj.PerPage);
                    for (const auto &VQ : ParsedRequest.Queries)
                    {
                         SearchQueryObj.ExcludeFields.push_back(VQ.FieldName);
                    }
               }
          }
     }
     catch (...)
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid JSON body.");
     }

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

     const size_t MaxFilterBytes = 8192;

     std::string ValidationError;

     if (!ValidateQueryInput(SearchQueryObj.FilterBy, &ValidationError, MaxFilterBytes, "filter_by"))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid search parameter.", ValidationError);
     }

     ComprehensiveSearchResult SearchResultObj = PerformComprehensiveSearch(CollectionName, SearchQueryObj);

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = GenerateComprehensiveSearchResponse(SearchResultObj, SearchQueryObj);

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

     return Response;
}

/* CalculateVectorSimilarity calculates cosine similarity between vectors. */

/*
 * SearchAPI::CalculateVectorSimilarity implementation.
 */

float SearchAPI::CalculateVectorSimilarity(const Document &Doc, const VectorQuery &VectorQueryVal)
{
     if (VectorQueryVal.Vector.empty())
     {
          return 0.0f;
     }

     std::string FieldName = VectorQueryVal.FieldName.empty() ? "embedding" : VectorQueryVal.FieldName;

     if (Doc.Fields.find(FieldName) == Doc.Fields.end())
     {
          return 0.0f;
     }

     std::vector<float> DocVector;

     try
     {
          auto FieldIt = Doc.Fields.find(FieldName);

          if (FieldIt == Doc.Fields.end())
          {
               return 0.0f;
          }

          nlohmann::json EmbeddingJSON = nlohmann::json::parse(FieldIt->second);

          if (EmbeddingJSON.is_array())
          {
               for (const auto &Val : EmbeddingJSON)
               {
                    float FVal = Val.get<float>();

                    DocVector.push_back(FVal);
               }
          }
     }
     catch (...)
     {
          return 0.0f;
     }

     if (DocVector.empty() || DocVector.size() != VectorQueryVal.Vector.size())
     {
          return 0.0f;
     }

     float DotProduct = 0.0f;
     float QueryNormSq = 0.0f;
     float DocNormSq = 0.0f;

     for (size_t I = 0; I < DocVector.size(); ++I)
     {
          DotProduct += VectorQueryVal.Vector[I] * DocVector[I];
          QueryNormSq += VectorQueryVal.Vector[I] * VectorQueryVal.Vector[I];
          DocNormSq += DocVector[I] * DocVector[I];
     }

     float QueryNorm = std::sqrt(QueryNormSq);
     float DocNorm = std::sqrt(DocNormSq);

     if (QueryNorm == 0.0f || DocNorm == 0.0f)
     {
          return 0.0f;
     }

     return DotProduct / (QueryNorm * DocNorm);
}

/* ProcessVectorSearch performs a vector-based similarity search. */

/*
 * SearchAPI::ProcessVectorSearch implementation.
 */

std::vector<SearchHit> SearchAPI::ProcessVectorSearch(const std::string &Collection, const ComprehensiveSearchQuery &Query)
{
     std::vector<SearchHit> Hits;

     if (Query.VectorQueryStr.empty() && Query.Embedding.empty())
     {
          return Hits;
     }

     int CandidateLimit = 1000;

     if (Instance && Instance->Config)
     {
          CandidateLimit = std::max(1, Instance->Config->GetHybridVectorCandidateLimit());
     }

     ParsedVectorRequest ParsedRequest;
     try
     {
          const std::string JsonStr = !Query.VectorQueryStr.empty() ? Query.VectorQueryStr : Query.Embedding;
          nlohmann::json VectorJSON = nlohmann::json::parse(JsonStr);
          ParsedRequest = ParseVectorRequest(VectorJSON, Query.PerPage);
     }
     catch (...)
     {
          return Hits;
     }

     if (ParsedRequest.Queries.empty())
     {
          return Hits;
     }

     int RequestedTopK = std::max(Query.PerPage, ParsedRequest.TopK);
     CandidateLimit = std::max(CandidateLimit, RequestedTopK);
     CandidateLimit = std::max(CandidateLimit, static_cast<int>(ParsedRequest.Queries.size() * RequestedTopK));

     if (ParsedRequest.HnswEfSearch > 0)
     {
          CandidateLimit = std::max(CandidateLimit, ParsedRequest.HnswEfSearch);
     }

     if (ParsedRequest.IvfNProbe > 0)
     {
          CandidateLimit = std::max(CandidateLimit, ParsedRequest.IvfNProbe * RequestedTopK);
     }

     if (ParsedRequest.IsUsingRefiner)
     {
          CandidateLimit = std::max(CandidateLimit, RequestedTopK * 4);
     }

     /*
           * There is no ANN/vector index backing this path yet, so restricting the
           * candidate set by storage order can silently miss the nearest neighbors.
           * For correctness, score the full collection.
           */
     size_t CollectionDocCount = HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);
     int DocumentScanLimit = CandidateLimit;

     if (CollectionDocCount > 0)
     {
          const size_t MaxListLimit = static_cast<size_t>(std::numeric_limits<int>::max());
          DocumentScanLimit = static_cast<int>(std::min(CollectionDocCount, MaxListLimit));
     }

     std::vector<Document> AllDocuments = HybridStorageManager::GetInstance().ListDocuments(Collection, DocumentScanLimit);

     std::unordered_map<std::string, Document> DocByID;
     std::vector<std::unordered_map<std::string, float>> QueryScores(ParsedRequest.Queries.size());
     std::vector<std::unordered_map<std::string, float>> QueryDistances(ParsedRequest.Queries.size());
     DocByID.reserve(AllDocuments.size());

     std::vector<VectorPayload> PreparedQueryVectors;
     PreparedQueryVectors.reserve(ParsedRequest.Queries.size());

     for (const auto &QueryObj : ParsedRequest.Queries)
     {
          VectorPayload Payload = QueryObj.Payload;
          if (QueryObj.Normalize)
          {
               NormalizeVectorPayload(&Payload);
          }
          PreparedQueryVectors.push_back(std::move(Payload));
     }

     for (const auto &StorageDoc : AllDocuments)
     {
          bool DocMatched = false;
          std::unordered_map<std::string, VectorPayload> ParsedDocVectors;

          for (size_t QI = 0; QI < ParsedRequest.Queries.size(); ++QI)
          {
               const ParsedVectorQuery &QueryObj = ParsedRequest.Queries[QI];

               auto CacheIt = ParsedDocVectors.find(QueryObj.FieldName);
               if (CacheIt == ParsedDocVectors.end())
               {
                    auto FieldIt = StorageDoc.Fields.find(QueryObj.FieldName);
                    if (FieldIt == StorageDoc.Fields.end())
                    {
                         continue;
                    }

                    VectorPayload ParsedVector;
                    try
                    {
                         nlohmann::json EmbeddingJSON = nlohmann::json::parse(FieldIt->second);
                         if (!ParseVectorPayloadFromJsonValue(EmbeddingJSON, &ParsedVector))
                         {
                              continue;
                         }
                    }
                    catch (...)
                    {
                         continue;
                    }

                    CacheIt = ParsedDocVectors.emplace(QueryObj.FieldName, std::move(ParsedVector)).first;
               }

               VectorPayload DocVector = CacheIt->second;
               if (QueryObj.Normalize)
               {
                    NormalizeVectorPayload(&DocVector);
               }

               float Distance = ComputeDistanceByMetric(PreparedQueryVectors[QI], DocVector, QueryObj.Metric);
               if (QueryObj.HasMaxDistance && Distance > QueryObj.MaxDistance)
               {
                    continue;
               }
               if (QueryObj.HasMinDistance && Distance < QueryObj.MinDistance)
               {
                    continue;
               }

               float Score = ComputeSimilarityByMetric(PreparedQueryVectors[QI], DocVector, QueryObj.Metric);
               if (Score < QueryObj.Threshold)
               {
                    continue;
               }

               QueryScores[QI][StorageDoc.ID] = Score;
               QueryDistances[QI][StorageDoc.ID] = Distance;
               DocMatched = true;
          }

          if (DocMatched)
          {
               DocByID[StorageDoc.ID] = StorageDoc;
          }
     }

     std::unordered_map<std::string, float> FinalScores;
     FinalScores.reserve(DocByID.size());
     std::unordered_map<std::string, float> FinalDistances;
     FinalDistances.reserve(DocByID.size());

     const std::string FusionMethod = NormalizeLowerCopy(ParsedRequest.FusionMethod);

     if (FusionMethod == "rrf")
     {
          for (size_t QI = 0; QI < ParsedRequest.Queries.size(); ++QI)
          {
               std::vector<std::pair<std::string, float>> Ranked(QueryScores[QI].begin(), QueryScores[QI].end());
               std::sort(Ranked.begin(), Ranked.end(), [](const auto &A, const auto &B)
                         {
                              return A.second > B.second;
                         });

               for (size_t Rank = 0; Rank < Ranked.size(); ++Rank)
               {
                    const float Score = 1.0f / static_cast<float>(ParsedRequest.RrfK + static_cast<int>(Rank) + 1);
                    const std::string &DocID = Ranked[Rank].first;
                    FinalScores[DocID] += ParsedRequest.Queries[QI].Weight * Score;
                    auto DistIt = QueryDistances[QI].find(DocID);
                    if (DistIt != QueryDistances[QI].end())
                    {
                         auto CurrentIt = FinalDistances.find(DocID);
                         if (CurrentIt == FinalDistances.end())
                         {
                              FinalDistances[DocID] = DistIt->second;
                         }
                         else
                         {
                              CurrentIt->second = std::min(CurrentIt->second, DistIt->second);
                         }
                    }
               }
          }
     }
     else
     {
          for (const auto &Pair : DocByID)
          {
               const std::string &DocID = Pair.first;
               float Sum = 0.0f;
               float WeightSum = 0.0f;
               float MaxScore = -std::numeric_limits<float>::infinity();
               float MinDistance = std::numeric_limits<float>::infinity();

               for (size_t QI = 0; QI < ParsedRequest.Queries.size(); ++QI)
               {
                    auto ScoreIt = QueryScores[QI].find(DocID);
                    if (ScoreIt == QueryScores[QI].end())
                    {
                         continue;
                    }

                    const float WeightedScore = ParsedRequest.Queries[QI].Weight * ScoreIt->second;
                    Sum += WeightedScore;
                    WeightSum += ParsedRequest.Queries[QI].Weight;
                    MaxScore = std::max(MaxScore, WeightedScore);

                    auto DistIt = QueryDistances[QI].find(DocID);
                    if (DistIt != QueryDistances[QI].end())
                    {
                         MinDistance = std::min(MinDistance, DistIt->second);
                    }
               }

               if (FusionMethod == "max")
               {
                    if (MaxScore > -std::numeric_limits<float>::infinity())
                    {
                         FinalScores[DocID] = MaxScore;
                    }
               }
               else if (FusionMethod == "average" || FusionMethod == "avg")
               {
                    if (WeightSum > 0.0f)
                    {
                         FinalScores[DocID] = Sum / WeightSum;
                    }
               }
               else
               {
                    FinalScores[DocID] = Sum;
               }

               if (MinDistance < std::numeric_limits<float>::infinity())
               {
                    FinalDistances[DocID] = MinDistance;
               }
          }
     }

     for (const auto &Pair : FinalScores)
     {
          auto DocIt = DocByID.find(Pair.first);
          if (DocIt == DocByID.end())
          {
               continue;
          }

          const auto &StorageDoc = DocIt->second;

          SearchHit HitObj;
          HitObj.Document["id"] = StorageDoc.ID;
          HitObj.Document["title"] = StorageDoc.Title;
          HitObj.Document["content"] = StorageDoc.Content;
          HitObj.Document["score"] = std::to_string(StorageDoc.Score);
          HitObj.Document["timestamp"] = std::to_string(StorageDoc.Timestamp);
          for (const auto &Field : StorageDoc.Fields)
          {
               HitObj.Document[Field.first] = Field.second;
          }

          HitObj.VectorScore = Pair.second;
          HitObj.TextMatch = 0.0f;
          HitObj.Weight = CalculateWeight(HitObj);

          if (Query.IncludeVectorDistance)
          {
               auto DistIt = FinalDistances.find(Pair.first);
               if (DistIt != FinalDistances.end())
               {
                    HitObj.Document["_vector_distance"] = std::to_string(DistIt->second);
               }
          }
          Hits.push_back(std::move(HitObj));
     }

     std::sort(Hits.begin(), Hits.end(), [](const SearchHit &A, const SearchHit &B)
               {
                    return A.VectorScore > B.VectorScore;
               });

     return Hits;
}

/* ProcessHybridSearch combines lexical and vector search. */

/*
 * SearchAPI::ProcessHybridSearch implementation.
 */

std::vector<SearchHit> SearchAPI::ProcessHybridSearch(const std::string &Collection, const ComprehensiveSearchQuery &Query)
{
     std::vector<SearchHit> LexicalHits = ProcessLexicalSearch(Collection, Query);
     std::vector<SearchHit> VectorHits = ProcessVectorSearch(Collection, Query);

     std::string MergeMethod = "linear";
     std::string NormalizationMethod = "minmax";
     bool NormalizeComponentScores = false;
     bool DynamicAlphaEnabled = false;
     int RrfK = 60;
     int ShortQueryTerms = 2;
     int LongQueryTerms = 6;
     float AlphaShort = 0.35f;
     float AlphaMedium = 0.50f;
     float AlphaLong = 0.70f;
     bool RerankEnabled = false;
     int RerankTopK = 100;
     float RerankLexicalWeight = 0.55f;
     float RerankVectorWeight = 0.45f;
     float RerankCoverageBoost = 0.10f;

     if (Instance && Instance->Config)
     {
          MergeMethod = NormalizeLowerCopy(Instance->Config->GetHybridMergeMethod());
          NormalizationMethod = NormalizeLowerCopy(Instance->Config->GetHybridNormalizationMethod());
          NormalizeComponentScores = Instance->Config->GetHybridNormalizeComponentScores();
          RrfK = std::max(1, Instance->Config->GetHybridRrfK());
          DynamicAlphaEnabled = Instance->Config->GetHybridDynamicAlphaEnabled();
          ShortQueryTerms = std::max(1, Instance->Config->GetHybridShortQueryTerms());
          LongQueryTerms = std::max(ShortQueryTerms, Instance->Config->GetHybridLongQueryTerms());
          AlphaShort = static_cast<float>(std::max(0.0, std::min(1.0, Instance->Config->GetHybridAlphaShort())));
          AlphaMedium = static_cast<float>(std::max(0.0, std::min(1.0, Instance->Config->GetHybridAlphaMedium())));
          AlphaLong = static_cast<float>(std::max(0.0, std::min(1.0, Instance->Config->GetHybridAlphaLong())));
          RerankEnabled = Instance->Config->GetHybridRerankEnabled();
          RerankTopK = std::max(1, Instance->Config->GetHybridRerankTopK());
          RerankLexicalWeight = static_cast<float>(Instance->Config->GetHybridRerankLexicalWeight());
          RerankVectorWeight = static_cast<float>(Instance->Config->GetHybridRerankVectorWeight());
          RerankCoverageBoost = static_cast<float>(Instance->Config->GetHybridRerankCoverageBoost());
     }

     float Alpha = Query.HybridAlpha;
     if (DynamicAlphaEnabled && !Query.Q.empty())
     {
          int TermCount = CountQueryTerms(Query.Q);
          if (TermCount <= ShortQueryTerms)
          {
               Alpha = AlphaShort;
          }
          else if (TermCount >= LongQueryTerms)
          {
               Alpha = AlphaLong;
          }
          else
          {
               Alpha = AlphaMedium;
          }
     }

     std::unordered_map<std::string, SearchHit> LexicalHitByID;
     std::unordered_map<std::string, SearchHit> VectorHitByID;
     std::unordered_map<std::string, int> LexicalRankByID;
     std::unordered_map<std::string, int> VectorRankByID;
     std::unordered_map<std::string, float> LexicalScoreByID;
     std::unordered_map<std::string, float> VectorScoreByID;
     std::vector<float> LexicalScores;
     std::vector<float> VectorScores;

     for (size_t I = 0; I < LexicalHits.size(); ++I)
     {
          auto It = LexicalHits[I].Document.find("id");
          if (It == LexicalHits[I].Document.end())
          {
               continue;
          }

          const std::string &ID = It->second;
          LexicalHitByID[ID] = LexicalHits[I];
          LexicalRankByID[ID] = static_cast<int>(I) + 1;
          LexicalScoreByID[ID] = LexicalHits[I].TextMatch;
          LexicalScores.push_back(LexicalHits[I].TextMatch);
     }

     for (size_t I = 0; I < VectorHits.size(); ++I)
     {
          auto It = VectorHits[I].Document.find("id");
          if (It == VectorHits[I].Document.end())
          {
               continue;
          }

          const std::string &ID = It->second;
          VectorHitByID[ID] = VectorHits[I];
          VectorRankByID[ID] = static_cast<int>(I) + 1;
          VectorScoreByID[ID] = VectorHits[I].VectorScore;
          VectorScores.push_back(VectorHits[I].VectorScore);
     }

     ScoreStats LexicalStats = CalculateScoreStats(LexicalScores);
     ScoreStats VectorStats = CalculateScoreStats(VectorScores);

     std::unordered_map<std::string, SearchHit> CombinedMap;

     for (const auto &Pair : LexicalHitByID)
     {
          CombinedMap[Pair.first] = Pair.second;
     }
     for (const auto &Pair : VectorHitByID)
     {
          if (CombinedMap.find(Pair.first) == CombinedMap.end())
          {
               CombinedMap[Pair.first] = Pair.second;
          }
     }

     for (auto &Pair : CombinedMap)
     {
          const std::string &ID = Pair.first;
          SearchHit &HitObj = Pair.second;

          float LexicalRaw = 0.0f;
          float VectorRaw = 0.0f;
          int LexicalRank = 0;
          int VectorRank = 0;

          auto LexicalScoreIt = LexicalScoreByID.find(ID);
          if (LexicalScoreIt != LexicalScoreByID.end())
          {
               LexicalRaw = LexicalScoreIt->second;
               HitObj.TextMatch = LexicalRaw;
          }

          auto VectorScoreIt = VectorScoreByID.find(ID);
          if (VectorScoreIt != VectorScoreByID.end())
          {
               VectorRaw = VectorScoreIt->second;
               HitObj.VectorScore = VectorRaw;
          }

          auto LexicalRankIt = LexicalRankByID.find(ID);
          if (LexicalRankIt != LexicalRankByID.end())
          {
               LexicalRank = LexicalRankIt->second;
          }

          auto VectorRankIt = VectorRankByID.find(ID);
          if (VectorRankIt != VectorRankByID.end())
          {
               VectorRank = VectorRankIt->second;
          }

          if (MergeMethod == "rrf")
          {
               HitObj.HybridScore = HybridSearchEngine::ReciprocalRankFusion(LexicalRank, VectorRank, RrfK);
          }
          else
          {
               float LexicalComponent = NormalizeScore(LexicalRaw, LexicalStats, NormalizationMethod, NormalizeComponentScores);
               float VectorComponent = NormalizeScore(VectorRaw, VectorStats, NormalizationMethod, NormalizeComponentScores);
               HitObj.HybridScore = HybridSearchEngine::CombineScores(LexicalComponent, VectorComponent, Alpha);
          }
     }

     std::vector<SearchHit> Result;
     Result.reserve(CombinedMap.size());

     for (auto &Pair : CombinedMap)
     {
          Result.push_back(std::move(Pair.second));
     }

     std::sort(Result.begin(), Result.end(), [](const SearchHit &A, const SearchHit &B)
               {
                    return A.HybridScore > B.HybridScore;
               });

     if (RerankEnabled && !Result.empty())
     {
          int EffectiveTopK = std::min(RerankTopK, static_cast<int>(Result.size()));
          std::vector<float> TopKLexicalScores;
          std::vector<float> TopKVectorScores;
          TopKLexicalScores.reserve(static_cast<size_t>(EffectiveTopK));
          TopKVectorScores.reserve(static_cast<size_t>(EffectiveTopK));

          for (int I = 0; I < EffectiveTopK; ++I)
          {
               TopKLexicalScores.push_back(Result[static_cast<size_t>(I)].TextMatch);
               TopKVectorScores.push_back(Result[static_cast<size_t>(I)].VectorScore);
          }

          ScoreStats TopKLexicalStats = CalculateScoreStats(TopKLexicalScores);
          ScoreStats TopKVectorStats = CalculateScoreStats(TopKVectorScores);

          for (int I = 0; I < EffectiveTopK; ++I)
          {
               SearchHit &HitObj = Result[static_cast<size_t>(I)];
               float LexicalComponent = NormalizeScore(HitObj.TextMatch, TopKLexicalStats, NormalizationMethod, true);
               float VectorComponent = NormalizeScore(HitObj.VectorScore, TopKVectorStats, NormalizationMethod, true);
               float Coverage = (HitObj.TextMatch > 0.0f && HitObj.VectorScore > 0.0f) ? 1.0f : 0.0f;
               HitObj.HybridScore = (RerankLexicalWeight * LexicalComponent) + (RerankVectorWeight * VectorComponent) + (RerankCoverageBoost * Coverage);
          }

          std::stable_sort(Result.begin(), Result.begin() + EffectiveTopK, [](const SearchHit &A, const SearchHit &B)
                           {
                                return A.HybridScore > B.HybridScore;
                           });
     }

     return Result;
}
