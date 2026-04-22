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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include <vendor/json/json.hpp>

#if defined(__has_include)
# if __has_include(<openssl/err.h>) && __has_include(<openssl/ssl.h>)
#  define HLQUERY_BENCH_HAS_OPENSSL 1
# endif
#endif

#ifdef HLQUERY_BENCH_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

/* HTTPResponse struct represents a response from the server. */

struct HTTPResponse
{
     int StatusCode;

     std::string Body;

     std::string ErrorMessage;
};

/* BenchmarkClient class for performance testing. */

class BenchmarkClient
{
   private:

     std::string Host;

     int Port;

     bool UseSSL;

     std::string AuthToken;

     int SocketFD;

     bool SocketConnected;

     std::mutex SocketMutex;

     std::atomic<int> RequestCount;

     bool ReuseCollections;
     bool SSLAuthMode = false;

     static const int MAX_REQUESTS_PER_CONNECTION = 100;

#ifdef HLQUERY_HAS_OPENSSL
     SSL_CTX *SSLCtx = nullptr;

     SSL *SSLObj = nullptr;
#endif

     /* Connects a socket with retry. */

     bool ConnectSocket(int &sock, int max_retries = 3, int connect_timeout_sec = 5);

     /* Closes the socket. */

     void CloseSocket();

     /* Gets a connection. */

     bool GetConnection(int &sock);

     /* Bulk insert helpers for adaptive retries on constrained systems. */

     int InsertDocumentsBulkInternal(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs, int split_depth);
     int InsertDocumentsBulkRequest(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs, HTTPResponse &response);
     bool IsRetryableBulkInsertResponse(const HTTPResponse &response) const;
     void SleepBeforeBulkRetry(int attempt, int split_depth) const;

   public:

     BenchmarkClient(const std::string &base_url, const std::string &token = "", bool reuse_collections = false);

     ~BenchmarkClient();

     /* Resets connection state. */

     void Reset();

     /* Resets the connection. */

     void ResetConnection();

     /* Makes a request. */

     HTTPResponse MakeRequest(const std::string &method, const std::string &path, const std::string &body = "", int max_retries = 3, bool use_keep_alive = true, int timeout_ms = 10000);

     /* Parses the URL. */

     void ParseURL(const std::string &url);

     /* Tests connection. */

     std::string TestConnection();

     /* Deletes a collection. */

     bool DeleteCollection(const std::string &name);

     /* Creates a collection. */

     bool CreateCollection(const std::string &name);
     bool CreateCollection(const std::string &name, int timeout_ms);

     bool CreateCollectionLocal(const std::string &name);
     bool CreateCollectionLocal(const std::string &name, int timeout_ms);

     /* Creates a collection with custom schema. */

     bool CreateCollectionWithSchema(const std::string &name, const nlohmann::json &fields, const std::string &default_sorting_field = "");

     bool CreateCollectionWithSchemaLocal(const std::string &name, const nlohmann::json &fields, const std::string &default_sorting_field = "");

     /* Inserts a document. */

     bool InsertDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content);

     /* Upserts a document with fields. */

     bool UpsertDocumentWithFields(const std::string &collection, const nlohmann::json &doc);

     bool UpsertDocumentWithFieldsLocal(const std::string &collection, const nlohmann::json &doc);

     /* Updates a document. */

     bool UpdateDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content);

     /* Upserts a document. */

     bool UpsertDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content);

     /* Adds a synonym. */

     bool AddSynonym(const std::string &collection, const std::string &synonym_id, const std::string &root_term, const std::vector<std::string> &synonyms);

     /* Adds a stopword. */

     bool AddStopword(const std::string &collection, const std::string &word);

     /* Inserts documents in bulk. */

     int InsertDocumentsBulk(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs);

     int InsertDocumentsBulkLocal(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs);

     /* Searches in a collection. */

     HTTPResponse Search(const std::string &collection, const std::string &query, const std::map<std::string, std::string> &params = {});

     /* Searches in a collection using POST. */

     HTTPResponse SearchPost(const std::string &collection, const nlohmann::json &Search_params);

     /* Performs a multi-search. */

     HTTPResponse MultiSearch(const nlohmann::json &multi_Search_params);

     /* Gets documents from a collection. */

     HTTPResponse GetCollectionDocuments(const std::string &collection, int offset = 0, int limit = 1000);

     /* Gets a document. */

     HTTPResponse GetDocument(const std::string &collection, const std::string &doc_id);

     /* Lists collections. */

     std::vector<std::string> ListCollections();

     /* Gets collection statistics. */

     HTTPResponse GetCollectionStats(const std::string &collection_name);

     /* Gets a collection. */

     HTTPResponse GetCollection(const std::string &collection_name);

     /* Gets server statistics. */

     HTTPResponse GetStats();

     /* Gets total document count. */

     HTTPResponse GetDocTotal();

     /* Gets health status. */

     HTTPResponse GetHealth();

     /* Gets metrics. */

     HTTPResponse GetMetrics();

     /* Updates counters. */

     HTTPResponse UpdateCounters();

     /* Flushes and syncs. */

     HTTPResponse FlushSync();

     /* Enables or disables global SSL dual-auth header mode for all benchmark clients. */

     static void SetGlobalSSLAuthMode(bool enabled);

   private:

     /* Encodes a URL. */

     std::string UrlEncode(const std::string &value);
};

/* Forward declarations for logging and reporting. */

void LogError(const std::string &message);

void LogOutput(const std::string &message);

/* OperationMetrics struct represents metrics for an operation. */

struct OperationMetrics
{
     std::string OperationType;

     std::string OperationSubtype;

     int64_t DurationMS;

     bool Success;

     int ResultCount;

     std::string CollectionName;

     std::map<std::string, std::string> Metadata;
};

/* AdvancedMetrics struct represents advanced metrics for the benchmark. */

struct AdvancedMetrics
{
     std::string ConfigURL;

     std::string ConfigAuthToken;

     int ConfigCollections;

     int ConfigDocuments;

     int ConfigThreads;

     int ConfigBatchSize;

     std::string RunID;

     std::string RunSeed;

     int64_t Phase1StartMS;

     int64_t Phase1EndMS;

     int64_t Phase1DurationMS;

     int64_t Phase2StartMS;

     int64_t Phase2EndMS;

     int64_t Phase2DurationMS;

     int64_t TotalEndMS;

     int64_t IngestStartMS;

     int64_t IngestEndMS;

     int64_t IngestDurationMS;

     int64_t CommitStartMS;

     int64_t CommitEndMS;

     int64_t CommitDurationMS;

     int CommitStatusCode = 0;

     std::string DurabilityConfigPath;

     std::string WalSyncMode;

     std::string WalBytesPerSync;

     std::string ManualWalFlush;

     std::vector<int64_t> CollectionTimings;

     std::vector<int64_t> BatchTimings;

     std::vector<int> BatchSizes;

     std::vector<std::string> BatchCollections;

     int Phase1CollectionsCreated;

     int Phase1CollectionsSkipped;

     int Phase2DocumentsInserted;

     int Phase2DocumentsSkipped;

     double Phase1ThroughputCollectionsPerSec;

     double Phase2ThroughputDocsPerSec;

     double TotalThroughputDocsPerSec;

     int FinalCollectionsCount = 0;

     int FinalDocumentsCount = 0;

     std::map<std::string, int> FinalPerCollectionCounts;

     std::vector<std::string> FinalCollectionNames;

     std::vector<int64_t> LatencyPercentiles;

     std::vector<OperationMetrics> DetailedOperations;

     std::vector<int64_t> SearchTimings;

     std::vector<int64_t> MultiSearchTimings;

     std::vector<int64_t> DocumentGetTimings;

     std::vector<int64_t> DocumentUpdateTimings;

     std::vector<int64_t> DocumentDeleteTimings;

     std::vector<int64_t> CollectionListTimings;

     std::vector<int64_t> CollectionGetTimings;

     std::vector<int64_t> CollectionUpdateTimings;

     std::vector<int64_t> CollectionDeleteTimings;

     std::vector<int64_t> SynonymOperationTimings;

     std::vector<int64_t> StopwordOperationTimings;

     std::vector<int64_t> OverrideOperationTimings;

     std::map<std::string, std::vector<int64_t>> QueryTypeTimings;

     std::map<std::string, std::vector<int64_t>> FilterTypeTimings;

     std::map<std::string, std::vector<int64_t>> SortTypeTimings;

     std::map<int, std::vector<int64_t>> DocumentSizeTimings;

     std::map<int, std::vector<int64_t>> BatchSizeTimings;

     std::map<int, double> ThreadCountThroughput;

     int64_t FinalTotalOperations = 0;

     int64_t FinalSuccessfulOperations = 0;

     double FinalSuccessRate = 0.0;

     double FinalAvgOperationTime = 0.0;
};

extern std::vector<std::string> FAKE_STOPWORDS;

extern AdvancedMetrics advanced_metrics;

extern std::mutex advanced_metrics_mutex;

extern std::atomic<bool> g_benchmark_should_stop;

extern std::atomic<bool> g_flood_should_stop;

extern std::string g_collection_prefix;

extern bool verbose_mode;

extern std::atomic<int> spinner_index;

extern std::atomic<int> last_printed_percent;

extern std::mutex progress_bar_mutex;

extern std::mutex console_mutex;

extern std::ofstream *log_file_stream;

extern std::mutex log_mutex;

extern std::atomic<int> collections_created;

extern std::atomic<int> documents_inserted;

extern std::atomic<int> additional_documents_inserted;

extern std::atomic<int> collections_skipped;

extern std::atomic<int> documents_skipped;

extern std::atomic<int> additional_documents_skipped;
