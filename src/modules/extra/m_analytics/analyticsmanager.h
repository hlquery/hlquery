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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "api/httpserver.h"
#include "core/modules.h"

struct AnalyticsBucketKey
{
     /* Aggregated action name. */

     std::string Action;

     /* Aggregated collection name. */

     std::string Collection;

     /* Aggregated requester IP. */

     std::string RequesterIP;

     /* Aggregated requester identity. */

     std::string RequesterUser;

     /* Compares bucket keys for hash table equality. */

     bool operator==(const AnalyticsBucketKey &Other) const
     {
          return Action == Other.Action && Collection == Other.Collection && RequesterIP == Other.RequesterIP && RequesterUser == Other.RequesterUser;
     }
};

struct AnalyticsBucketKeyHash
{
     /* Hashes a bucket key for unordered containers. */

     size_t operator()(const AnalyticsBucketKey &Key) const
     {
          size_t Result = std::hash<std::string>{}(Key.Action) ^ (std::hash<std::string>{}(Key.Collection) << 1U);
          Result ^= (std::hash<std::string>{}(Key.RequesterIP) << 2U);
          Result ^= (std::hash<std::string>{}(Key.RequesterUser) << 3U);
          return Result;
     }
};

struct AnalyticsBucket
{
     /* Total request count for this bucket. */

     uint64_t Count = 0;

     /* Requests carrying an auth token or API key. */

     uint64_t AuthenticatedCount = 0;

     /* Total request bytes received for this bucket. */

     uint64_t RequestBytes = 0;

     /* Total response bytes sent for this bucket. */

     uint64_t ResponseBytes = 0;

     /* Successful request count. */

     uint64_t Status2xx = 0;

     /* Client error count. */

     uint64_t Status4xx = 0;

     /* Server error count. */

     uint64_t Status5xx = 0;

     /* Successful search events aggregated into this bucket. */

     uint64_t SearchCount = 0;

     /* Successful click events aggregated into this bucket. */

     uint64_t ClickCount = 0;

     /* Sum of total matches reported by search results. */

     uint64_t SearchResultsFound = 0;

     /* Sum of hits actually returned in responses. */

     uint64_t SearchResultsReturned = 0;

     /* Sum of search execution time in milliseconds. */

     uint64_t SearchTimeMS = 0;

     /* Sum of click ranks when provided. */

     uint64_t ClickRankSum = 0;
};

/* Aggregates and flushes module analytics events to a remote endpoint. */

class AnalyticsManager
{
   private:


     struct ParsedEndpoint
     {
          /* URL scheme. */

          std::string Scheme;

          /* Remote host name. */

          std::string Host;

          /* Remote port. */

          uint16_t Port = 0;

          /* Remote path. */

          std::string Path;

          /* Whether parsing succeeded. */

          bool Valid = false;
     };

     /* Worker loop for periodic flushes. */

     void WorkerLoop();

     /* Flushes aggregated usage once. */

     void FlushOnce();

     /* Returns an approximate in-memory size of buffered analytics state. */

     size_t EstimateBufferedBytesLocked() const;

     /* Sends the startup event if still pending. */

     bool FlushStartupEvent();

     /* Merges a failed snapshot back into the in-memory buckets. */

     void MergeBucketsLocked(const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> &Snapshot);

     /* Normalizes a request into a bucket key. */

     AnalyticsBucketKey NormalizeKey(const HttpRequest &Request, RouteAction ActionVal) const;

     /* Extracts the collection name from a request path. */

     std::string ExtractCollectionName(const HttpRequest &Request) const;

     /* Estimates the serialized request size in bytes. */

     uint64_t EstimateRequestBytes(const HttpRequest &Request) const;

     /* Estimates the serialized response size in bytes. */

     uint64_t EstimateResponseBytes(const HttpResponse &Response) const;

     /* Returns whether the request carried a token header. */

     bool HasAuthToken(const HttpRequest &Request) const;

     /* Parses the configured analytics endpoint. */

     ParsedEndpoint ParseEndpoint(const std::string &URLValue) const;

     /* Sends one payload to the remote endpoint. */

     bool PostPayload(const std::string &Body);

     /* Builds the regular usage payload. */

     std::string BuildPayload(const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> &Snapshot,
                              uint64_t WindowStartMS,
                              uint64_t WindowEndMS);

     /* Builds the startup payload. */

     std::string BuildStartupPayload() const;

     /* Returns milliseconds since the Unix epoch. */

     uint64_t NowMS() const;

     /* Local server name. */

     std::string ServerName;

     /* Local server ID. */

     std::string ServerID;

     /* Configured database engine in use (for analytics attribution). */

     std::string UsedDB;

     /* Configured remote endpoint URL. */

     std::string EndpointURL;

     /* Optional token sent as outbound analytics request header. */

     std::string APIToken;

     /* Flush interval in seconds. */

     int FlushIntervalSeconds = 300;

     /* Connect and I/O timeout in milliseconds. */

     int ConnectTimeoutMS = 5000;

     /* Whether read routes should be tracked. */

     bool TrackReads = true;

     /* Whether write routes should be tracked. */

     bool TrackWrites = true;

     /* Parsed endpoint metadata. */

     ParsedEndpoint Endpoint;

     /* Whether analytics are enabled. */

     std::atomic<bool> Enabled{false};

     /* Whether the worker should keep running. */

     std::atomic<bool> Running{false};

     /* Whether Start has already been called. */

     std::atomic<bool> Started{false};

     /* Protects access to aggregate buckets. */

     mutable std::mutex BucketsMutex;

     /* Wakes the worker when shutting down. */

     std::condition_variable WakeCondition;

     /* Background flush task running on the management pool. */

     std::future<void> WorkerFuture;

     /* Aggregated usage buckets. */

     std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> Buckets;

     /* Start time of the current usage window. */

     uint64_t WindowStartMS = 0;

     /* Startup timestamp for this process. */

     uint64_t StartupTimeMS = 0;

     /* Whether the startup event still needs to be sent. */

     std::atomic<bool> StartupEventPending{false};

     /* Whether the worker should flush immediately instead of waiting for the interval. */

     std::atomic<bool> FlushRequested{false};

     /* Count of request events skipped due to contention. */

     std::atomic<uint64_t> DroppedEvents{0};

     /* Count of failed outbound posts. */

     std::atomic<uint64_t> FailedPosts{0};

     /* Hard cap for unique in-memory buckets. */

     static constexpr size_t MaxBuckets = 512;

     /* Approximate buffered analytics size threshold for immediate flush. */

     static constexpr size_t MaxBufferedBytes = 10 * 1024 * 1024;

   public:

     /* Initialize the analytics manager with endpoint and tracking settings. */


     AnalyticsManager(const std::string &ServerNameValue,
                      const std::string &ServerIDValue,
                      const std::string &UsedDBValue,
                      const std::string &EndpointURLValue,
                      const std::string &APITokenValue,
                      int FlushIntervalSecondsValue,
                      int ConnectTimeoutMSValue,
                      bool TrackReadsValue,
                      bool TrackWritesValue);

     /* Stop the worker and release analytics resources. */

     ~AnalyticsManager();

     /* Returns whether analytics are enabled. */

     bool IsEnabled() const
     {
          return Enabled.load(std::memory_order_acquire);
     }

     /* Starts the analytics worker. */

     void Start();

     /* Stops the analytics worker. */

     void Stop();

     /* Forces an immediate usage flush. */

     void FlushNow();

     /* Requests an immediate usage flush on the analytics worker. */

     bool RequestFlushAsync();

     /* Records a single request into the in-memory aggregate. */

     void RecordRequest(const HttpRequest &Request, const HttpResponse &Response, RouteAction ActionVal);

     /* Records a successful search analytics event. */

     void RecordSearchEvent(const std::string &Action,
                            const std::string &Collection,
                            uint64_t SearchTimeMS,
                            uint64_t Found,
                            uint64_t Returned,
                            const std::string &RequesterIP,
                            const std::string &RequesterUser,
                            bool Authenticated);

     /* Records a successful click analytics event. */

     void RecordClickEvent(const std::string &Collection, int Rank, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated);

     /* Records a named collection-level event. */

     void RecordCollectionEvent(const std::string &Action, const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated);

     /* Records a named document-level event. */

     void RecordDocumentEvent(const std::string &Action, const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated);

     /* Records a named event with an explicit occurrence count. */

     void RecordCountedEvent(const std::string &Action,
                             const std::string &Collection,
                             uint64_t Count,
                             const std::string &RequesterIP,
                             const std::string &RequesterUser,
                             bool Authenticated);
};
