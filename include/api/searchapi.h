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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vendor/json/json.hpp"
#include "api/httpserver.h"
#include "search/cstore.h"
#include "search/lindex.h"

struct VectorQuery
{
     std::string Field;

     /* FieldName is an alias for Field. */

     std::string FieldName;
     std::vector<float> Vector;
     int K = 10;

     /* TopK is an alias for K. */

     int TopK = 10;
     float Threshold = 0.0f;
     bool Normalize = false;
};

struct SearchResult
{
     std::string ID;
     double Score = 0.0;
     std::unordered_map<std::string, std::string> Document;
};

struct FilterCondition
{
     std::string Field;
     std::string Op;
     std::string Value;
     std::string LogicalConnector;
};

struct SearchQuery
{
     std::string Q;
     std::vector<std::string> QueryBy;
     std::string FilterBy;
     int PerPage = 10;
     int Page = 1;
};

class DocumentStorage
{
   public:

     /* AddDocument stores a document for the collection. */

     bool AddDocument(const std::string& Collection, const Document& Doc)
     {
          return true;
     }

     /* GetDocument fetches a document by collection and ID. */

     Document GetDocument(const std::string& Collection, const std::string& ID)
     {
          return Document();
     }

     /* ListDocuments returns a slice of documents for a collection. */

     std::vector<Document> ListDocuments(const std::string& Collection, int Limit, int Offset)
     {
          return {};
     }

     /* DeleteDocument removes a document from the collection. */

     bool DeleteDocument(const std::string& Collection, const std::string& ID)
     {
          return true;
     }
};

struct GeoPoint
{
     double Latitude = 0.0;
     double Longitude = 0.0;
};

struct GeoRadius
{
     std::string Field;
     double Lat = 0.0;
     double Lon = 0.0;
     double RadiusKM = 0.0;
     bool Enabled = false;
};

struct GeoBox
{
     std::string Field;
     double TopLeftLat = 0.0;
     double TopLeftLon = 0.0;
     double BottomRightLat = 0.0;
     double BottomRightLon = 0.0;
     bool Enabled = false;
};

struct FacetQuery
{
     std::string Field;
     std::string Query;
};

struct SearchOverride
{
     std::string Rule;
     std::vector<std::string> PinDocuments;
     std::map<std::string, float> BoostDocuments;
     std::map<std::string, float> DemoteDocuments;
     std::string Filter;
};

struct ComprehensiveSearchQuery
{
     /*
     * Basic search query string.
     *
     * Supports advanced query syntax:
     *   - NOT: !term or NOT term
     *   - FIELD: title:laptop or price:100
     *   - RANGE: price:[100 TO 500] or price:{100 TO 500}
     *   - WILDCARD: laptop*, *laptop, lap*top
     *   - REGEX: title:/pattern/
     *   - FUZZY: laptop~ or laptop~2
     *   - BOOST: laptop^2.0 or laptop^2
     *   - Boolean: term1 AND term2, term1 OR term2, +term, -term
     *   - Phrase: "exact phrase"
     */

     std::string Q;
     std::vector<std::string> QueryBy;
     std::string FilterBy;
     std::vector<std::string> FacetBy;
     std::map<std::string, std::string> FacetQuery;
     int MaxFacetValues = 10;

     /* Pagination. */

     int PerPage = 10;
     int Page = 1;
     int Offset = 0;
     std::string PageToken;
     std::string NextPageToken;

     /* Sorting. */

     std::vector<std::string> SortBy;

     /* Highlighting. */

     bool Highlight = false;
     std::vector<std::string> HighlightFields;
     std::string HighlightFullFields;

     /* Field selection. */

     std::vector<std::string> IncludeFields;
     std::vector<std::string> ExcludeFields;

     /* Typo tolerance. */

     int NumTypos = 2;
     bool NumTyposExplicit = false;
     int DropTokensThreshold = 0;
     int TypoTokensThreshold = 2;
     bool Prefix = false;
     bool InlineFuzzy = false;

     /* Grouping. */

     std::vector<std::string> GroupBy;
     int GroupLimit = 3;

     /* Search behavior. */

     bool PrioritizeExactMatch = true;
     bool ExhaustiveSearch = false;
     bool AllowScanFallback = true;
     bool CaseSensitive = false;
     std::map<std::string, double> TermBoosts;

     /* Vector search. */

     VectorQuery VectorQueryVal;
     float HybridAlpha = 0.5f;

     /* Vector search parameters. */

     std::string VectorQueryStr;
     std::string Embedding;
     bool UseRemoteEmbedding = false;
     std::string RemoteEmbeddingURL;
     std::string RemoteEmbeddingAPIKey;
     std::string RemoteEmbeddingModel;
     std::string RemoteEmbeddingField;

     /* Geo search. */

     GeoRadius GeoRadiusVal;
     GeoBox GeoBoxVal;
     std::string GeoSortBy;

     /* Overrides. */

     bool EnableOverrides = false;
     std::vector<SearchOverride> Overrides;

     /* Synonyms and stopwords. */

     bool EnableSynonyms = true;
     bool EnableStopwords = true;

     /* Analytics. */

     std::string AnalyticsTag;
     bool EnableAnalytics = true;

     /* Aggregations. */

     /* Aggregations stores the JSON string with aggregation config. */

     std::string Aggregations;

     /* PreserveMatchedHits keeps the full filtered/sorted hit set for callers
      * that need analytics rows before grouping and pagination are applied.
      */

     bool PreserveMatchedHits = false;

     /* Include created_at field in response. */

     /* IncludeCreatedAt defaults to false and only includes the field when requested. */

     bool IncludeCreatedAt = false;

     /* IncludeVectorDistance controls whether `_vector_distance` is emitted per hit. */

     bool IncludeVectorDistance = false;
};

struct SearchHit
{
     std::map<std::string, std::string> Document;
     float TextMatch = 0.0f;
     float VectorScore = 0.0f;
     float HybridScore = 0.0f;

     /* Weight is used for sorting adjustments. */

     float Weight = 1.0f;
     std::map<std::string, std::string> Highlights;
     std::string GroupKey;
     int GroupCount = 0;
};

struct FacetCount
{
     std::string Value;
     int Count;
     std::string Highlighted;
};

struct FacetResult
{
     std::string FieldName;
     std::vector<FacetCount> Counts;
     std::string Stats;
};

/* Aggregation structures. */

struct AggregationBucket
{
     std::string Key;
     int DocCount;

     /* AggregationsJSON stores nested aggregations as JSON strings. */

     std::map<std::string, std::string> AggregationsJSON;
};

struct AggregationResult
{
     std::string Name;

     /* Type defines the aggregation kind: "terms", "stats", "sum", "avg", "min", "max". */

     std::string Type;

     /* Buckets stores buckets for terms aggregations. */

     std::vector<AggregationBucket> Buckets;

     /* Metrics stores metric aggregation values. */

     std::map<std::string, double> Metrics;
};

struct ComprehensiveSearchResult
{
     std::vector<SearchHit> Hits;
     int Found = 0;
     int OutOf = 0;
     int Page = 1;
     int PerPage = 10;
     std::string NextPageToken;
     std::map<std::string, FacetResult> Facets;
     std::map<std::string, AggregationResult> Aggregations;
     std::vector<SearchHit> MatchedHits;
     std::map<std::string, std::string> RequestParams;
     float SearchTimeMS = 0.0f;
     std::vector<std::string> GroupByHits;
     bool IndexingInProgress = false;
     bool PartialResults = false;
     std::string PartialReason;
     /* Error contains the error message when query execution fails. */

     std::string Error;
     std::vector<std::map<std::string, std::string>> DistributedDiagnostics;
};

struct ReplicationStatusSnapshot
{
     uint64_t RequestsAttempted = 0;
     uint64_t RequestsSucceeded = 0;
     uint64_t RequestsFailed = 0;
     uint64_t ReplicaAcks = 0;
     std::string LastError;
     uint64_t LastErrorTimestampMS = 0;
};

struct PeerReconnectDiagnostics
{
     bool HasState = false;
     int ConsecutiveFailures = 0;
     int64_t NextRetryMS = 0;
     uint64_t LastFailureMS = 0;
     uint64_t LastSuccessMS = 0;
     std::string LastError;
};

/* Search API handler for HTTP server. */

class SearchAPI
{
   private:

     /* Prevent external construction and enforce singleton access. */

     SearchAPI() = default;
     ~SearchAPI();

     /* DocumentStoragePtr tracks the active document storage adapter. */

     DocumentStorage* DocumentStoragePtr;
     mutable std::mutex CollectionMutationMutex;
     std::unordered_map<std::string, uint64_t> CollectionMutationVersions;
     std::atomic<uint64_t> CollectionMutationClock{1};
     mutable std::atomic<uint64_t> ReplicationOutboxClock{1};
     mutable std::mutex ReplicationStatusMutex;
     mutable std::atomic<uint64_t> ReplicationRequestsAttempted{0};
     mutable std::atomic<uint64_t> ReplicationRequestsSucceeded{0};
     mutable std::atomic<uint64_t> ReplicationRequestsFailed{0};
     mutable std::atomic<uint64_t> ReplicationReplicaAcks{0};
     mutable std::string LastReplicationError;
     mutable uint64_t LastReplicationErrorTimestampMS = 0;
     mutable std::mutex ReplicationSlaveStateMutex;
     mutable std::unordered_set<std::string> ReplicationDirtySlaves;
     mutable std::unordered_set<std::string> ReplicationResyncInProgress;
     mutable std::unordered_map<std::string, uint64_t> ReplicationLastReachableTimestampMS;
     mutable std::unordered_map<std::string, uint64_t> ReplicationLastResyncTimestampMS;
     mutable std::atomic<bool> ReplicationMonitorRunning{false};
     mutable std::atomic<bool> ReplicationMonitorStop{false};
     mutable std::condition_variable ReplicationMonitorCV;
     mutable std::mutex ReplicationMonitorCVMutex;
     mutable std::thread ReplicationMonitorThread;
     mutable std::atomic<bool> DistributedLinkMonitorRunning{false};
     mutable std::atomic<bool> DistributedLinkMonitorStop{false};
     mutable std::condition_variable DistributedLinkMonitorCV;
     mutable std::mutex DistributedLinkMonitorCVMutex;
     mutable std::thread DistributedLinkMonitorThread;
     mutable std::mutex AsyncReplicationTasksMutex;
     mutable std::vector<std::future<void>> AsyncReplicationTasks;

     /* ParseDocumentFromJSON parses a document payload into a Document. */

     bool ParseDocumentFromJSON(const std::string& Json, Document& DocumentObj, std::string* ErrorMsg = nullptr);
     bool ParseDocumentFromJSON(const nlohmann::json& DocJSON, Document& DocumentObj, std::string* ErrorMsg = nullptr);

     /* ParseCollectionConfigFromJSON parses collection configuration JSON. */

     bool ParseCollectionConfigFromJSON(const std::string& Json, CollectionConfig& Config);

     /* ValidateCollectionSchema validates collection field definitions. */

     bool ValidateCollectionSchema(const CollectionConfig& Config, std::string& ErrorMessage);

     /* ValidateFieldName validates a field name for schema rules. */

     bool ValidateFieldName(const std::string& FieldName, std::string* ErrorMsg = nullptr);

     /* ValidateFieldValue validates a field value for ingestion. */

     bool ValidateFieldValue(const std::string& FieldValue, std::string* ErrorMsg = nullptr, const std::string& FieldName = "");

     /* ValidateQueryInput validates search query inputs for safety limits. */

     bool ValidateQueryInput(const std::string& Value, std::string* ErrorMsg, size_t MaxBytes, const std::string& FieldName, bool AllowNewlines = false);

     /* ExtractJSONValue fetches a simple string value for a JSON key. */

     std::string ExtractJSONValue(const std::string& Json, const std::string& Key);

     /* ExtractJSONArray fetches a JSON array as a vector of strings. */

     std::vector<std::string> ExtractJSONArray(const std::string& Json, const std::string& Key);

     /* ParseSearchParamsFromJSON converts JSON body params to a map. */

     std::unordered_map<std::string, std::string> ParseSearchParamsFromJSON(const std::string& Json);

     /* ParseComprehensiveSearchQuery builds a full search query object. */

     ComprehensiveSearchQuery ParseComprehensiveSearchQuery(const std::unordered_map<std::string, std::string>& Params);

     /* PerformComprehensiveSearch runs lexical, vector, or hybrid search. */

     ComprehensiveSearchResult PerformComprehensiveSearch(const std::string& Collection, const ComprehensiveSearchQuery& Query);

     /* GenerateComprehensiveSearchResponse builds the JSON response payload. */

     std::string GenerateComprehensiveSearchResponse(const ComprehensiveSearchResult& Result, const ComprehensiveSearchQuery& Query);

     /* GenerateHTMLSearchResponse builds the HTML search response payload. */

     std::string GenerateHTMLSearchResponse(const ComprehensiveSearchResult& Result, const ComprehensiveSearchQuery& Query, const std::string& CollectionName);

     /* ProcessLexicalSearch runs the keyword search pipeline. */

     std::vector<SearchHit> ProcessLexicalSearch(const std::string& Collection, const ComprehensiveSearchQuery& Query);

     /* ProcessVectorSearch runs the vector search pipeline. */

     std::vector<SearchHit> ProcessVectorSearch(const std::string& Collection, const ComprehensiveSearchQuery& Query);

     /* ValidateVectorQueryPayload validates vector JSON before execution. */

     bool ValidateVectorQueryPayload(const std::string& Payload, int DefaultTopK, std::string* Error);

     /* ProcessHybridSearch merges lexical and vector results. */

     std::vector<SearchHit> ProcessHybridSearch(const std::string& Collection, const ComprehensiveSearchQuery& Query);

     /* ApplyFilters filters hits using the filter_by expression. */

     std::vector<SearchHit> ApplyFilters(const std::vector<SearchHit>& Hits, const std::string& FilterBy);

     /* EvaluateFilterCondition evaluates a single filter condition. */

     bool EvaluateFilterCondition(const std::map<std::string, std::string>& Document, const FilterCondition& Condition);

     /* SortField stores a sort field name and direction. */

     struct SortField
     {
          std::string Field;

          /* Direction is either "asc" or "desc". */

          std::string Direction;
     };

     /* ApplySorting sorts hits according to the sort_by criteria. */

     std::vector<SearchHit> ApplySorting(const std::vector<SearchHit>& Hits, const std::vector<std::string>& SortBy);

     /* ResolveDefaultCollectionSortBy returns collection-level default sort fields. */

     std::vector<std::string> ResolveDefaultCollectionSortBy(const std::string& Collection);

     /* ApplyModuleWeights lets runtime modules adjust hit weights before final ranking. */

     void ApplyModuleWeights(std::vector<SearchHit>& Hits, const std::string& Collection, const ComprehensiveSearchQuery& Query, const std::string& RankingMode);

     /* ApplyCollectionRankWeights adjusts hit weights using collection-level _rank_* metadata. */

     void ApplyCollectionRankWeights(std::vector<SearchHit>& Hits, const std::string& Collection);

     /* GetEffectiveScore returns the score after hit-specific weighting is applied. */

     float GetEffectiveScore(const SearchHit& Hit) const;

     /* ApplyPagination slices the hit list for the requested page. */

     std::vector<SearchHit> ApplyPagination(const std::vector<SearchHit>& Hits, int Page, int PerPage);

     /* ApplyGrouping groups hits by requested fields. */

     std::vector<SearchHit> ApplyGrouping(const std::vector<SearchHit>& Hits, const std::vector<std::string>& GroupBy, int GroupLimit);

     /* CalculateWeight computes a final weighting score for ranking. */

     float CalculateWeight(const SearchHit& Hit);

     /* CalculateRecencyDecay applies recency decay to ranking weight. */

     float CalculateRecencyDecay(const SearchHit& Hit, const std::string& Query = "");

     /* ApplyFieldSelection limits fields based on include/exclude lists. */

     std::map<std::string, std::string> ApplyFieldSelection(const std::map<std::string, std::string>& Document,
                                                            const std::vector<std::string>& IncludeFields,
                                                            const std::vector<std::string>& ExcludeFields);

     /* GenerateHighlights builds highlighted field values for hits. */

     std::map<std::string, std::string> GenerateHighlights(const std::map<std::string, std::string>& Document,
                                                           const std::string& Query,
                                                           const std::vector<std::string>& HighlightFields);

     /* GenerateFacets builds facet results from hits. */

     std::map<std::string, FacetResult> GenerateFacets(const std::vector<SearchHit>& Hits,
                                                       const std::vector<std::string>& FacetBy,
                                                       const std::map<std::string, std::string>& FacetQuery,
                                                       int MaxFacetValues);

     /* GenerateAggregations builds aggregation results from hits. */

     std::map<std::string, AggregationResult> GenerateAggregations(const std::vector<SearchHit>& Hits, const std::string& AggsConfigJSON);

     /* IsWithinGeoRadius tests whether a point is within the radius. */

     bool IsWithinGeoRadius(const GeoPoint& Point, const GeoRadius& Radius);

     /* IsWithinGeoBox tests whether a point is inside the geo box. */

     bool IsWithinGeoBox(const GeoPoint& Point, const GeoBox& Box);

     /* CalculateGeoDistance computes distance between two coordinates. */

     double CalculateGeoDistance(const GeoPoint& P1, const GeoPoint& P2);

     /* IsTypoTolerant checks fuzzy match tolerance for a pattern. */

     bool IsTypoTolerant(const std::string& Text, const std::string& Pattern, int MaxTypos);

     /* GenerateTypoVariations generates candidate typo variants. */

     std::vector<std::string> GenerateTypoVariations(const std::string& Text, int MaxTypos);

     /* ApplyOverrides applies override rules to the hit list. */

     std::vector<SearchHit> ApplyOverrides(const std::vector<SearchHit>& Hits, const std::vector<SearchOverride>& Overrides, const ComprehensiveSearchQuery& Query);

     /* Distributed query routing helpers. */

     bool ShouldAttemptDistributedIngest(const HttpRequest& Request) const;

     bool ProxyDistributedRequest(const HttpRequest& Request,
                                  const std::string& Host,
                                  int Port,
                                  HttpResponse* OutResponse,
                                  std::string* OutError) const;

     bool ResolveDistributedRoute(const std::string& Route,
                                  std::string* OutHost,
                                  int* OutPort,
                                  bool* OutIsLocal) const;

     bool ShouldAttemptReplication(const HttpRequest& Request) const;

     bool ProxyReplicationRequest(const HttpRequest& Request,
                                  const std::string& Host,
                                  int Port,
                                  HttpResponse* OutResponse,
                                  std::string* OutError) const;

     bool PingReplicationSlave(const std::string& Host,
                               int Port,
                               std::string* OutError) const;

     bool ResyncSlaveFromScratch(const std::string& Host,
                                 int Port,
                                 std::string* OutError) const;

     bool PrepareReplicationOutboxRecord(const HttpRequest& Request,
                                         const std::string& OperationLabel,
                                         std::string* OutEntryID,
                                         std::string* OutError) const;

     bool MarkReplicationOutboxCommitted(const std::string& EntryID,
                                         const HttpRequest& Request,
                                         const std::string& OperationLabel,
                                         std::string* OutError) const;

     void ClearReplicationOutboxRecord(const std::string& EntryID) const;

     void PersistReplicationSlaveState(const std::string& Endpoint) const;

     bool RestoreReplicationSlaveState(const std::string& Endpoint) const;

     bool QueuePendingReplication(const std::string& Endpoint,
                                  const HttpRequest& Request,
                                  bool AllowOverflow,
                                  std::string* OutError = nullptr) const;
     std::vector<HttpRequest> TakePendingReplications(const std::string& Endpoint) const;
     bool ReplayPendingReplications(const std::string& Endpoint, const std::string& Host, int Port) const;

     void EnsureReplicationMonitorStarted() const;

     void ReplicationMonitorLoop() const;

     void EnsureDistributedLinkMonitorStarted() const;

     void DistributedLinkMonitorLoop() const;

     void CleanupFinishedAsyncReplicationTasks() const;

     void EnqueueAsyncReplicationTask(std::function<void()> Task) const;

     void MarkSlaveDirty(const std::string& Endpoint) const;

     void MarkSlaveReachable(const std::string& Endpoint) const;

     void MarkSlaveResynced(const std::string& Endpoint) const;

     bool IsSlaveResyncActive(const std::string& Endpoint) const;

     bool ReplicateWriteRequest(const HttpRequest& Request,
                                const std::string& OperationLabel,
                                std::string* OutError) const;

     void RecordReplicationFailure(const std::string& ErrorMessage) const;

     void RecordReplicationSuccess(size_t AckCount) const;

     bool SelectDistributedNodeForKey(const std::string& Key,
                                      std::string* OutHost,
                                      int* OutPort,
                                      bool* OutIsLocal) const;

     /* CalculateTextMatchScore scores the lexical match. */

     float CalculateTextMatchScore(const Document& Doc, const std::string& Query, const std::vector<std::string>& QueryBy);

     /* CalculateVectorSimilarity computes vector similarity score. */

     float CalculateVectorSimilarity(const Document& Doc, const VectorQuery& VectorQueryVal);

     /* GenerateFieldHighlight builds highlights for one field. */

     std::string GenerateFieldHighlight(const std::string& FieldValue, const std::string& Query);

     /* CalculateLevenshteinDistance computes edit distance. */

     int CalculateLevenshteinDistance(const std::string& S1, const std::string& S2);

     /* IsExactMatch checks whether a hit is an exact query match. */

     bool IsExactMatch(const SearchHit& Hit, const std::string& Query);

     /* ParseMultiSearchRequest parses multi-search JSON payloads. */

     std::vector<std::pair<std::string, ComprehensiveSearchQuery>> ParseMultiSearchRequest(const std::string& JSONBody);

     /* ShouldAttemptDistributedSearch checks if distributed routing should be attempted. */

     bool ShouldAttemptDistributedSearch(const HttpRequest& Request) const;

     /* IsStrictDistributedMode checks whether remote execution is mandatory. */

     bool IsStrictDistributedMode() const;

     /* TryDistributedSearch executes distributed query orchestration when available. */

     bool TryDistributedSearch(const HttpRequest& Request,
                              const std::string& Collection,
                              const ComprehensiveSearchQuery& Query,
                              ComprehensiveSearchResult* OutResult,
                              std::string* OutError);

     /* GetReplicationNodeState snapshots replication freshness state for a node endpoint. */

     bool GetReplicationNodeState(const std::string& Endpoint,
                                  uint64_t* OutLastReachableMS,
                                  uint64_t* OutLastResyncMS,
                                  bool* OutDirty,
                                  bool* OutResyncInProgress) const;

     /* ParseCommaSeparated splits a comma-delimited string. */

     std::vector<std::string> ParseCommaSeparated(const std::string& Input);

     /* ParseFilters parses filter expressions into conditions. */

     std::vector<FilterCondition> ParseFilters(const std::string& FilterString);

     /* ParseSortFields parses sort-by expressions into fields. */

     std::vector<SortField> ParseSortFields(const std::string& SortString);

   public:

     std::vector<SearchHit> ApplyFiltersForSQL(const std::vector<SearchHit>& Hits, const std::string& FilterBy)
     {
          return ApplyFilters(Hits, FilterBy);
     }

     std::vector<SearchHit> ApplyPaginationForSQL(const std::vector<SearchHit>& Hits, int Page, int PerPage, int Offset)
     {
          if (Hits.empty())
          {
               return {};
          }

          if (PerPage <= 0 || Offset < 0)
          {
               return {};
          }

          const std::size_t StartPosSize = static_cast<std::size_t>(Offset);
          const std::size_t PerPageSize = static_cast<std::size_t>(PerPage);

          if (StartPosSize >= Hits.size())
          {
               return {};
          }

          const std::size_t EndPosSize = std::min(StartPosSize + PerPageSize, Hits.size());

          return std::vector<SearchHit>(
               Hits.begin() + static_cast<std::vector<SearchHit>::difference_type>(StartPosSize),
               Hits.begin() + static_cast<std::vector<SearchHit>::difference_type>(EndPosSize));
     }

     std::string GenerateSearchResponseForSQL(const ComprehensiveSearchResult& Result, const ComprehensiveSearchQuery& Query)
     {
          return GenerateComprehensiveSearchResponse(Result, Query);
     }

     void MaybeTriggerCrashInjection(const std::string& Point) const;

     HttpResponse CheckReplicationOperationDedup(const HttpRequest& Request,
                                                 const std::string& Operation) const;

     void FinalizeReplicationOperation(const HttpRequest& Request,
                                       const HttpResponse& Response) const;

     HttpResponse CheckReplicationResyncFence(const HttpRequest& Request,
                                              const std::string& Operation) const;

     void FinalizeReplicationResyncRequest(const HttpRequest& Request,
                                          const HttpResponse& Response) const;

     /* GetInstance returns the singleton SearchAPI instance. */

     static SearchAPI& GetInstance();

     /* Initialize prepares the search API and subsystems. */

     bool Initialize();

     /* Start initializes the search API and dependencies. */

     bool Start();

     /* Shutdown stops background replication/peer activity before process teardown. */

     void Shutdown();

     /* IsInitialized reports whether the API is ready. */

     bool IsInitialized() const;

     PeerReconnectDiagnostics GetPeerReconnectDiagnostics(const std::string& Endpoint) const;

     void ClearPeerReconnectDiagnostics(const std::string& Endpoint) const;

     /*
     * HTTP route handlers.
     *
     * All search endpoints support advanced query syntax in the 'q' parameter:
     *   - NOT: !term or NOT term
     *   - FIELD: title:laptop or price:100
     *   - RANGE: price:[100 TO 500] or price:{100 TO 500}
     *   - WILDCARD: laptop*, *laptop, lap*top
     *   - REGEX: title:/pattern/
     *   - FUZZY: laptop~ or laptop~2
     *   - BOOST: laptop^2.0 or laptop^2
     *   - Boolean: term1 AND term2, term1 OR term2, +term, -term
     *   - Phrase: "exact phrase"
     */

     /* HandleCreateCollection handles collection creation requests. */

     HttpResponse HandleCreateCollection(const HttpRequest& Request);

     /* HandleDeleteCollection handles collection deletion requests. */

     HttpResponse HandleDeleteCollection(const HttpRequest& Request);

     /* HandleFlush handles flush requests for all collections. */

     HttpResponse HandleFlush(const HttpRequest& Request);

     /* HandleListCollections handles collection listing requests. */

     HttpResponse HandleListCollections(const HttpRequest& Request);

     /* HandleListCollectionsDistributed handles distributed collection listing. */

     HttpResponse HandleListCollectionsDistributed(const HttpRequest& Request);

     /* HandleListDocuments handles document listing requests. */

     HttpResponse HandleListDocuments(const HttpRequest& Request);

     /* HandleGetCollection handles collection detail requests. */

     HttpResponse HandleGetCollection(const HttpRequest& Request);

     /* HandleGetCollectionLanguage returns language info for a collection. */

     HttpResponse HandleGetCollectionLanguage(const HttpRequest& Request);

     /* HandleUpdateCollection handles collection update requests. */

     HttpResponse HandleUpdateCollection(const HttpRequest& Request);

     /* HandleAddDocument handles document ingestion requests. */

     HttpResponse HandleAddDocument(const HttpRequest& Request);

     /* HandleUpdateDocument handles document update requests. */

     HttpResponse HandleUpdateDocument(const HttpRequest& Request);

     /* HandleDeleteDocument handles document deletion requests. */

     HttpResponse HandleDeleteDocument(const HttpRequest& Request);

     /* HandleGetDocument handles document retrieval requests. */

     HttpResponse HandleGetDocument(const HttpRequest& Request);

     /* HandleGetDocumentContext returns alternate contextual phrases for one document. */

     HttpResponse HandleGetDocumentContext(const HttpRequest& Request);
     /* HandleBulkImportDocuments handles bulk document imports. */

     HttpResponse HandleBulkImportDocuments(const HttpRequest& Request);

     /* HandleDeleteDocumentsByFilter handles filtered deletions. */

     HttpResponse HandleDeleteDocumentsByFilter(const HttpRequest& Request);

     /* HandleUpdateByQuery handles update-by-query requests. */

     HttpResponse HandleUpdateByQuery(const HttpRequest& Request);

     /* HandleDeleteByQuery handles delete-by-query requests. */

     HttpResponse HandleDeleteByQuery(const HttpRequest& Request);

     /* HandleSearch handles search requests. */

     HttpResponse HandleSearch(const HttpRequest& Request);

     /* HandleGlobalSearch handles merged cross-collection search requests. */

     HttpResponse HandleGlobalSearch(const HttpRequest& Request);

     /* HandleVectorSearch handles vector search requests. */

     HttpResponse HandleVectorSearch(const HttpRequest& Request);

     /* HandleMultiSearch handles multi-search requests. */

     HttpResponse HandleMultiSearch(const HttpRequest& Request);

     /* HandleFacetCounts handles facet-only requests. */

     HttpResponse HandleFacetCounts(const HttpRequest& Request);

     /* HandleExportDocuments handles search export requests. */

     HttpResponse HandleExportDocuments(const HttpRequest& Request);

     /* HandleMaybe returns suggestion candidates for a query in a collection. */

     HttpResponse HandleMaybe(const HttpRequest& Request);

     /* HandleAutocomplete handles autocomplete requests. */

     HttpResponse HandleAutocomplete(const HttpRequest& Request);

     /* HandleSpellcheck handles spellcheck requests. */

     HttpResponse HandleSpellcheck(const HttpRequest& Request);

     /* HandleListUsers handles user listing requests. */

     HttpResponse HandleListUsers(const HttpRequest& Request);

     /* HandleCreateUser handles user creation requests. */

     HttpResponse HandleCreateUser(const HttpRequest& Request);

     /* HandleGetUser handles user retrieval requests. */

     HttpResponse HandleGetUser(const HttpRequest& Request);

     /* HandleDeleteUser handles user deletion requests. */

     HttpResponse HandleDeleteUser(const HttpRequest& Request);

     /* HandleUpdateUser handles user update requests. */

     HttpResponse HandleUpdateUser(const HttpRequest& Request);

     /* HandleListKeys handles API key listing requests. */

     HttpResponse HandleListKeys(const HttpRequest& Request);

     /* HandleCreateKey handles API key creation requests. */

     HttpResponse HandleCreateKey(const HttpRequest& Request);

     /* HandleGetKey handles API key retrieval requests. */

     HttpResponse HandleGetKey(const HttpRequest& Request);

     /* HandleDeleteKey handles API key deletion requests. */

     HttpResponse HandleDeleteKey(const HttpRequest& Request);

     /* HandleUpdateKey handles API key update requests. */

     HttpResponse HandleUpdateKey(const HttpRequest& Request);

     /* HandleListPresets handles search preset listing requests. */

     HttpResponse HandleListPresets(const HttpRequest& Request);

     /* HandleCreateOrUpdatePreset handles search preset upserts. */

     HttpResponse HandleCreateOrUpdatePreset(const HttpRequest& Request);

     /* HandleGetPreset handles search preset retrieval requests. */

     HttpResponse HandleGetPreset(const HttpRequest& Request);

     /* HandleDeletePreset handles search preset deletion requests. */

     HttpResponse HandleDeletePreset(const HttpRequest& Request);

     /* HandleAnalyticsClick handles click analytics requests. */

     HttpResponse HandleAnalyticsClick(const HttpRequest& Request);

     /* HandleListSynonyms handles collection synonym listing. */

     HttpResponse HandleListSynonyms(const HttpRequest& Request);

     /* HandleListAllSynonyms handles global synonym listing. */

     HttpResponse HandleListAllSynonyms(const HttpRequest& Request);

     /* HandleListGlobalSynonyms handles global-scope synonym listing. */

     HttpResponse HandleListGlobalSynonyms(const HttpRequest& Request);

     /* HandleCreateOrUpdateSynonym handles synonym upsert requests. */

     HttpResponse HandleCreateOrUpdateSynonym(const HttpRequest& Request);

     /* HandleGetSynonym handles synonym retrieval requests. */

     HttpResponse HandleGetSynonym(const HttpRequest& Request);

     /* HandleDeleteSynonym handles synonym deletion requests. */

     HttpResponse HandleDeleteSynonym(const HttpRequest& Request);

     /* HandleCreateOrUpdateGlobalSynonym handles global synonym upsert requests. */

     HttpResponse HandleCreateOrUpdateGlobalSynonym(const HttpRequest& Request);

     /* HandleGetGlobalSynonym handles global synonym retrieval requests. */

     HttpResponse HandleGetGlobalSynonym(const HttpRequest& Request);

     /* HandleDeleteGlobalSynonym handles global synonym deletion requests. */

     HttpResponse HandleDeleteGlobalSynonym(const HttpRequest& Request);

     /* HandleListAllStopwords handles global stopword listing. */

     HttpResponse HandleListAllStopwords(const HttpRequest& Request);

     /* HandleListStopwords handles collection stopword listing. */

     HttpResponse HandleListStopwords(const HttpRequest& Request);

     /* HandleListGlobalStopwords handles global stopword listing. */

     HttpResponse HandleListGlobalStopwords(const HttpRequest& Request);

     /* HandleCreateStopword handles stopword creation requests. */

     HttpResponse HandleCreateStopword(const HttpRequest& Request);

     /* HandleCreateGlobalStopword handles global stopword creation requests. */

     HttpResponse HandleCreateGlobalStopword(const HttpRequest& Request);

     /* HandleDeleteStopword handles stopword deletion requests. */

     HttpResponse HandleDeleteStopword(const HttpRequest& Request);

     /* HandleDeleteGlobalStopword handles global stopword deletion requests. */

     HttpResponse HandleDeleteGlobalStopword(const HttpRequest& Request);

     /* HandleListOverrides handles override listing requests. */

     HttpResponse HandleListOverrides(const HttpRequest& Request);

     /* HandleCreateOrUpdateOverride handles override upserts. */

     HttpResponse HandleCreateOrUpdateOverride(const HttpRequest& Request);

     /* HandleGetOverride handles override retrieval requests. */

     HttpResponse HandleGetOverride(const HttpRequest& Request);

     /* HandleDeleteOverride handles override deletion requests. */

     HttpResponse HandleDeleteOverride(const HttpRequest& Request);

     /* HandleListAliases handles alias listing requests. */

     HttpResponse HandleListAliases(const HttpRequest& Request);

     /* HandleCreateOrUpdateAlias handles alias upserts. */

     HttpResponse HandleCreateOrUpdateAlias(const HttpRequest& Request);

     /* HandleGetAlias handles alias retrieval requests. */

     HttpResponse HandleGetAlias(const HttpRequest& Request);

     /* HandleDeleteAlias handles alias deletion requests. */

     HttpResponse HandleDeleteAlias(const HttpRequest& Request);

     /* HandleListModules handles module discovery requests. */

     HttpResponse HandleListModules(const HttpRequest& Request);

     /* HandleModuleSyntax returns syntax and parameter metadata for a module. */

     HttpResponse HandleModuleSyntax(const HttpRequest& Request);

     /* HandleModuleAPI dispatches an API request to a loaded module. */

     HttpResponse HandleModuleAPI(const HttpRequest& Request);

     /* HandleModuleLoad loads one module into the live runtime registry. */

     HttpResponse HandleModuleLoad(const HttpRequest& Request);

     /* HandleModuleUnload unloads one module from the live runtime registry. */

     HttpResponse HandleModuleUnload(const HttpRequest& Request);

     /* HandleHealth handles health check requests. */

     HttpResponse HandleHealth(const HttpRequest& Request);

     /* HandleReady handles lightweight readiness checks. */

     HttpResponse HandleReady(const HttpRequest& Request);

     /* AttachSearchResponseMeta injects a compact meta block into search JSON responses. */

     void AttachSearchResponseMeta(HttpResponse& Response,
                                   const ComprehensiveSearchQuery& Query,
                                   const HttpRequest& Request,
                                   const std::string& CollectionName = "");

     /* HandlePing handles ping requests. */

     HttpResponse HandlePing(const HttpRequest& Request);

     /* HandleMetrics handles metrics summary requests. */

     HttpResponse HandleMetrics(const HttpRequest& Request);

     /* HandleStats handles stats requests. */

     HttpResponse HandleStats(const HttpRequest& Request);

     /* HandleCache returns cache statistics. */

     HttpResponse HandleCache(const HttpRequest& Request);

     /* HandleMetricsHistory handles metrics history requests. */

     HttpResponse HandleMetricsHistory(const HttpRequest& Request);

     /* HandleEtc handles diagnostic etc requests. */

     HttpResponse HandleEtc(const HttpRequest& Request);

     /* HandleConnections handles active connection requests. */

     HttpResponse HandleConnections(const HttpRequest& Request);

     /* HandleRocksDB handles RocksDB status requests. */

     HttpResponse HandleRocksDB(const HttpRequest& Request);

     /* HandleStatus handles status requests. */

     HttpResponse HandleStatus(const HttpRequest& Request);

     /* HandleSearchConfig returns active search configuration values. */

     HttpResponse HandleSearchConfig(const HttpRequest& Request);

     /* HandleConfigFiles returns active configuration files and included files. */

     HttpResponse HandleConfigFiles(const HttpRequest& Request);

     /* HandleLinksList handles listing cluster links. */

     HttpResponse HandleLinksList(const HttpRequest& Request);

     /* HandleLinksPing handles pinging cluster links. */

     HttpResponse HandleLinksPing(const HttpRequest& Request);

     /* HandleLinksConnect handles adding a cluster link. */

     HttpResponse HandleLinksConnect(const HttpRequest& Request);

     /* HandleLinksDisconnect handles removing a cluster link. */

     HttpResponse HandleLinksDisconnect(const HttpRequest& Request);

     /* HandleStartup handles startup status requests. */

     HttpResponse HandleStartup(const HttpRequest& Request);

     /* HandleDocTotal handles total document count requests. */

     HttpResponse HandleDocTotal(const HttpRequest& Request);

     /* HandleUpdateCounters handles counter update requests. */

     HttpResponse HandleUpdateCounters(const HttpRequest& Request);

     /* HandleDebugCounters handles debug counter requests. */

     HttpResponse HandleDebugCounters(const HttpRequest& Request);

     /* HandleRepair handles repair requests. */

     HttpResponse HandleRepair(const HttpRequest& Request);

     /* HandleIntegrity handles integrity check requests. */

     HttpResponse HandleIntegrity(const HttpRequest& Request);

     /* HandleSelfCheck handles self-check requests. */

     HttpResponse HandleSelfCheck(const HttpRequest& Request);

     /* HandleStorageStatus handles storage status requests. */

     HttpResponse HandleStorageStatus(const HttpRequest& Request);

     /* ResolveCollectionName resolves aliases to collection names. */

     std::string ResolveCollectionName(const std::string& Name);

     /* GetCollectionMutationVersion returns the current mutation version for cache invalidation. */

     uint64_t GetCollectionMutationVersion(const std::string& Collection) const;

     /* BumpCollectionMutationVersion invalidates cached search results for a collection. */

     uint64_t BumpCollectionMutationVersion(const std::string& Collection);

     /* ResetCollectionMutationVersions clears all mutation versions. */

     void ResetCollectionMutationVersions();


     ReplicationStatusSnapshot GetReplicationStatusSnapshot() const;

     /* ExtractCollectionFromPath extracts collection names from paths. */

     std::string ExtractCollectionFromPath(const std::string& Path);

     /* ExtractDocumentIdFromPath extracts document IDs from paths. */

     std::string ExtractDocumentIdFromPath(const std::string& Path);

     /* ExtractSynonymIdFromPath extracts synonym IDs from paths. */

     std::string ExtractSynonymIdFromPath(const std::string& Path);

     /* ExtractStopwordFromPath extracts stopwords from paths. */

     std::string ExtractStopwordFromPath(const std::string& Path);

     /* ExtractOverrideIdFromPath extracts override IDs from paths. */

     std::string ExtractOverrideIdFromPath(const std::string& Path);

     /* ExtractAliasNameFromPath extracts alias names from paths. */

     std::string ExtractAliasNameFromPath(const std::string& Path);

     /* ExtractKeyIDFromPath extracts API key IDs from paths. */

     std::string ExtractKeyIDFromPath(const std::string& Path);

     /* ParseQueryParams parses a query string into key/value pairs. */

     std::unordered_map<std::string, std::string> ParseQueryParams(const std::string& QueryString);

     /* GenerateJSONResponse builds a JSON array response. */

     std::string GenerateJSONResponse(const std::vector<SearchResult>& Results);

     /* GenerateErrorResponse builds a JSON error payload. */

     std::string GenerateErrorResponse(const std::string& Error, int StatusCode = 400);

     /* GetCurrentTimestamp returns the current timestamp string. */

     std::string GetCurrentTimestamp();

     /* GetCollectionCreatedAt returns collection creation time. */

     std::string GetCollectionCreatedAt(const std::string& CollectionName);

     /* EscapeJSONString escapes values for JSON output. */

     std::string EscapeJSONString(const std::string& Str);

     /* EscapeHTMLString escapes values for HTML output. */

     std::string EscapeHTMLString(const std::string& Str);

     /* CheckReadOnlyMode rejects writes in read-only mode, while letting replication traffic through. */

     HttpResponse CheckReadOnlyMode(const HttpRequest& Request, const std::string& Operation);
};
