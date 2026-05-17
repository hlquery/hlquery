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
#include <cstdint>
#include <cstring>
#include <map>
#include <netdb.h>
#ifdef HLQUERY_HAS_OPENSSL
/// $CompilerFlags: find_compiler_flags("openssl")
/// $LinkerFlags: find_linker_flags("openssl")
/// $PackageInfo: require_system("alpine") openssl-dev
/// $PackageInfo: require_system("arch") openssl
/// $PackageInfo: require_system("darwin") pkg-config openssl
/// $PackageInfo: require_system("debian~") libssl-dev pkg-config
/// $PackageInfo: require_system("rhel~") openssl-devel pkgconfig
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#endif
#include <sstream>
#include <string>
#include <tuple>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#include "common/searchpool.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "modules/extra/m_analytics/analyticsmanager.h"
#include "vendor/json/json.hpp"

namespace
{
constexpr const char *kSystemCollection = "*";
constexpr const char *kOverflowCollection = "__other__";

/* Return whether the route should count as a read operation. */

bool IsReadAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::Status:
          case RouteAction::Health:
          case RouteAction::Ping:
          case RouteAction::Stats:
          case RouteAction::Metrics:
          case RouteAction::MetricsHistory:
          case RouteAction::Connections:
          case RouteAction::RocksDB:
          case RouteAction::DocTotal:
          case RouteAction::Integrity:
          case RouteAction::SelfCheck:
          case RouteAction::StorageStatus:
          case RouteAction::Etc:
          case RouteAction::Root:
          case RouteAction::ListCollections:
          case RouteAction::ListCollectionsDistributed:
          case RouteAction::GetCollection:
          case RouteAction::DocumentSearch:
          case RouteAction::VectorSearch:
          case RouteAction::MultiSearch:
          case RouteAction::GlobalSearch:
          case RouteAction::ListDocuments:
          case RouteAction::GetDocument:
          case RouteAction::FacetCounts:
          case RouteAction::ExportDocuments:
          case RouteAction::ListSynonyms:
          case RouteAction::ListAllSynonyms:
          case RouteAction::GetSynonym:
          case RouteAction::ListGlobalSynonyms:
          case RouteAction::GetGlobalSynonym:
          case RouteAction::ListStopwords:
          case RouteAction::ListAllStopwords:
          case RouteAction::ListGlobalStopwords:
          case RouteAction::ListOverrides:
          case RouteAction::GetOverride:
          case RouteAction::ListAliases:
          case RouteAction::GetAlias:
          case RouteAction::LinksList:
          case RouteAction::LinksPing:
          case RouteAction::ListUsers:
          case RouteAction::GetUser:
          case RouteAction::ListKeys:
          case RouteAction::GetKey:
               return true;
          default:
               return false;
     }
}

/* Return whether the route should count as a write operation. */

bool IsWriteAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::Flush:
          case RouteAction::UpdateCounters:
          case RouteAction::DebugCounters:
          case RouteAction::Repair:
          case RouteAction::CreateCollection:
          case RouteAction::UpdateCollection:
          case RouteAction::DeleteCollection:
          case RouteAction::AddDocument:
          case RouteAction::BulkImportDocuments:
          case RouteAction::UpdateDocument:
          case RouteAction::DeleteDocument:
          case RouteAction::DeleteDocumentsByFilter:
          case RouteAction::UpdateByQuery:
          case RouteAction::DeleteByQuery:
          case RouteAction::UpsertSynonym:
          case RouteAction::DeleteSynonym:
          case RouteAction::UpsertGlobalSynonym:
          case RouteAction::DeleteGlobalSynonym:
          case RouteAction::CreateStopword:
          case RouteAction::DeleteStopword:
          case RouteAction::CreateGlobalStopword:
          case RouteAction::DeleteGlobalStopword:
          case RouteAction::UpsertOverride:
          case RouteAction::DeleteOverride:
          case RouteAction::UpsertAlias:
          case RouteAction::DeleteAlias:
          case RouteAction::LinksConnect:
          case RouteAction::LinksDisconnect:
          case RouteAction::CreateUser:
          case RouteAction::UpdateUser:
          case RouteAction::DeleteUser:
          case RouteAction::CreateKey:
          case RouteAction::UpdateKey:
          case RouteAction::DeleteKey:
               return true;
          default:
               return false;
     }
}

/* Remove the query string from a request path. */

std::string StripQueryString(const std::string &PathValue)
{
     size_t QueryPos = PathValue.find('?');

     if (QueryPos == std::string::npos)
     {
          return PathValue;
     }

     return PathValue.substr(0, QueryPos);
}
}

/* Initialize the analytics manager runtime state. */

AnalyticsManager::AnalyticsManager(const std::string &ServerNameValue,
                                   const std::string &ServerIDValue,
                                   const std::string &UsedDBValue,
                                   const std::string &EndpointURLValue,
                                   const std::string &APITokenValue,
                                   int FlushIntervalSecondsValue,
                                   int ConnectTimeoutMSValue,
                                   bool TrackReadsValue,
                                   bool TrackWritesValue)
    : ServerName(ServerNameValue),
      ServerID(ServerIDValue),
      UsedDB(UsedDBValue),
      EndpointURL(EndpointURLValue),
      APIToken(APITokenValue),
      FlushIntervalSeconds(std::max(10, FlushIntervalSecondsValue)),
      ConnectTimeoutMS(std::max(250, ConnectTimeoutMSValue)),
      TrackReads(TrackReadsValue),
      TrackWrites(TrackWritesValue),
      Endpoint(ParseEndpoint(EndpointURLValue))
{
     Enabled.store(Endpoint.Valid, std::memory_order_release);
}

/* Stop the analytics manager on destruction. */

AnalyticsManager::~AnalyticsManager()
{
     Stop();
}

/* Start the background analytics worker. */

void AnalyticsManager::Start()
{
     if (!IsEnabled() || Started.exchange(true, std::memory_order_acq_rel))
     {
          return;
     }

     SearchThreadPool::PoolStats ManagementPoolStats = ThreadPoolManager::GetInstance().GetManagementPool().GetStats();

     if (ManagementPoolStats.TotalThreads == 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics disabled: management pool has 0 worker threads.");
          }

          Enabled.store(false, std::memory_order_release);
          Running.store(false, std::memory_order_release);
          Started.store(false, std::memory_order_release);
          return;
     }

     Running.store(true, std::memory_order_release);

     {
          std::lock_guard<std::mutex> Lock(BucketsMutex);
          WindowStartMS = NowMS();
          StartupTimeMS = WindowStartMS;
     }

     StartupEventPending.store(true, std::memory_order_release);

     WorkerFuture = ThreadPoolManager::GetInstance().GetManagementPool().Submit([this]()
                                                                                {
                                                                                     WorkerLoop();
                                                                                });

     if (!WorkerFuture.valid())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics disabled: management pool queue rejected analytics worker task.");
          }

          Enabled.store(false, std::memory_order_release);
          Running.store(false, std::memory_order_release);
          Started.store(false, std::memory_order_release);
          return;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("analytics", "Analytics manager started on management pool (endpoint: " + EndpointURL +
                                                   ", flush_interval=" + std::to_string(FlushIntervalSeconds) + "s).");
     }
}

/* Stop the background analytics worker. */

void AnalyticsManager::Stop()
{
     if (!Started.exchange(false, std::memory_order_acq_rel))
     {
          return;
     }

     Running.store(false, std::memory_order_release);
     WakeCondition.notify_all();

     if (WorkerFuture.valid())
     {
          if (WorkerFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("analytics", "Analytics worker did not stop within 2 seconds; continuing shutdown.");
               }
          }
     }
}

/* Flush analytics immediately on demand. */

void AnalyticsManager::FlushNow()
{
     FlushOnce();
}

bool AnalyticsManager::RequestFlushAsync()
{
     if (!IsEnabled())
     {
          return false;
     }

     Start();

     if (!Started.load(std::memory_order_acquire))
     {
          return false;
     }

     FlushRequested.store(true, std::memory_order_release);
     WakeCondition.notify_one();
     return true;
}

/* Record one HTTP request and response pair. */

void AnalyticsManager::RecordRequest(const HttpRequest &Request, const HttpResponse &Response, RouteAction ActionVal)
{
     if (!IsEnabled())
     {
          return;
     }

     const bool ReadAction = IsReadAction(ActionVal);
     const bool WriteAction = IsWriteAction(ActionVal);

     if ((ReadAction && !TrackReads) ||
         (WriteAction && !TrackWrites) ||
         (!ReadAction && !WriteAction))
     {
          return;
     }

     AnalyticsBucketKey Key = NormalizeKey(Request, ActionVal);
     std::unique_lock<std::mutex> Lock(BucketsMutex, std::try_to_lock);
     const int StatusCode = Response.StatusCode;
     const uint64_t RequestBytes = EstimateRequestBytes(Request);
     const uint64_t ResponseBytes = EstimateResponseBytes(Response);
     const bool HasAuth = HasAuthToken(Request);

     if (!Lock.owns_lock())
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     if (WindowStartMS == 0)
     {
          WindowStartMS = NowMS();
     }

     if (Buckets.size() >= MaxBuckets && Buckets.find(Key) == Buckets.end())
     {
          Key.Collection = kOverflowCollection;
     }

     AnalyticsBucket &Bucket = Buckets[Key];
     Bucket.Count++;
     Bucket.AuthenticatedCount += HasAuth ? 1ULL : 0ULL;
     Bucket.RequestBytes += RequestBytes;
     Bucket.ResponseBytes += ResponseBytes;

     if (StatusCode >= 200 && StatusCode < 300)
     {
          Bucket.Status2xx++;
     }
     else if (StatusCode >= 400 && StatusCode < 500)
     {
          Bucket.Status4xx++;
     }
     else if (StatusCode >= 500)
     {
          Bucket.Status5xx++;
     }

     if (EstimateBufferedBytesLocked() >= MaxBufferedBytes)
     {
          FlushRequested.store(true, std::memory_order_release);
          WakeCondition.notify_one();
     }
}

/* Record one search analytics event. */

void AnalyticsManager::RecordSearchEvent(const std::string &Action,
                                         const std::string &Collection,
                                         uint64_t SearchTimeMS,
                                         uint64_t Found,
                                         uint64_t Returned,
                                         const std::string &RequesterIP,
                                         const std::string &RequesterUser,
                                         bool Authenticated)
{
     if (!IsEnabled())
     {
          return;
     }

     AnalyticsBucketKey Key;
     Key.Action = Action.empty() ? "Search" : Action;
     Key.Collection = Collection.empty() ? kSystemCollection : Collection;
     Key.RequesterIP = RequesterIP;
     Key.RequesterUser = RequesterUser;

     std::unique_lock<std::mutex> Lock(BucketsMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     if (WindowStartMS == 0)
     {
          WindowStartMS = NowMS();
     }

     if (Buckets.size() >= MaxBuckets && Buckets.find(Key) == Buckets.end())
     {
          Key.Collection = kOverflowCollection;
     }

     AnalyticsBucket &Bucket = Buckets[Key];
     Bucket.SearchCount++;
     Bucket.SearchResultsFound += Found;
     Bucket.SearchResultsReturned += Returned;
     Bucket.SearchTimeMS += SearchTimeMS;
     Bucket.AuthenticatedCount += Authenticated ? 1ULL : 0ULL;

     if (EstimateBufferedBytesLocked() >= MaxBufferedBytes)
     {
          FlushRequested.store(true, std::memory_order_release);
          WakeCondition.notify_one();
     }
}

void AnalyticsManager::RecordQueryEvent(const AnalyticsQueryEvent &Event)
{
     if (!IsEnabled())
     {
          return;
     }

     std::unique_lock<std::mutex> Lock(BucketsMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     if (WindowStartMS == 0)
     {
          WindowStartMS = NowMS();
     }

     if (QueryEvents.size() >= MaxQueryEvents)
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     AnalyticsQueryEvent Copy = Event;

     if (Copy.Action.empty())
     {
          Copy.Action = "SearchQuery";
     }

     if (Copy.Collection.empty())
     {
          Copy.Collection = kSystemCollection;
     }

     if (Copy.Query.size() > MaxQueryLength)
     {
          Copy.Query.resize(MaxQueryLength);
     }

     QueryEvents.push_back(std::move(Copy));

     if (EstimateBufferedBytesLocked() >= MaxBufferedBytes)
     {
          FlushRequested.store(true, std::memory_order_release);
          WakeCondition.notify_one();
     }
}

/* Record one analytics click event. */

void AnalyticsManager::RecordClickEvent(const std::string &Collection, int Rank, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
{
     if (!IsEnabled())
     {
          return;
     }

     AnalyticsBucketKey Key;
     Key.Action = "AnalyticsClick";
     Key.Collection = Collection.empty() ? kSystemCollection : Collection;
     Key.RequesterIP = RequesterIP;
     Key.RequesterUser = RequesterUser;

     std::unique_lock<std::mutex> Lock(BucketsMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     if (WindowStartMS == 0)
     {
          WindowStartMS = NowMS();
     }

     if (Buckets.size() >= MaxBuckets && Buckets.find(Key) == Buckets.end())
     {
          Key.Collection = kOverflowCollection;
     }

     AnalyticsBucket &Bucket = Buckets[Key];
     Bucket.ClickCount++;
     Bucket.AuthenticatedCount += Authenticated ? 1ULL : 0ULL;

     if (Rank >= 0)
     {
          Bucket.ClickRankSum += static_cast<uint64_t>(Rank);
     }

     if (EstimateBufferedBytesLocked() >= MaxBufferedBytes)
     {
          FlushRequested.store(true, std::memory_order_release);
          WakeCondition.notify_one();
     }
}

/* Record one collection-scoped analytics event. */

void AnalyticsManager::RecordCollectionEvent(const std::string &Action, const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
{
     RecordCountedEvent(Action.empty() ? "CollectionEvent" : Action,
                        Collection.empty() ? kSystemCollection : Collection,
                        1,
                        RequesterIP,
                        RequesterUser,
                        Authenticated);
}

/* Record one document-scoped analytics event. */

void AnalyticsManager::RecordDocumentEvent(const std::string &Action, const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
{
     RecordCountedEvent(Action.empty() ? "DocumentEvent" : Action,
                        Collection.empty() ? kSystemCollection : Collection,
                        1,
                        RequesterIP,
                        RequesterUser,
                        Authenticated);
}

/* Record one counted analytics event. */

void AnalyticsManager::RecordCountedEvent(const std::string &Action,
                                          const std::string &Collection,
                                          uint64_t Count,
                                          const std::string &RequesterIP,
                                          const std::string &RequesterUser,
                                          bool Authenticated)
{
     if (!IsEnabled())
     {
          return;
     }

     AnalyticsBucketKey Key;
     Key.Action = Action.empty() ? "CollectionEvent" : Action;
     Key.Collection = Collection.empty() ? kSystemCollection : Collection;
     Key.RequesterIP = RequesterIP;
     Key.RequesterUser = RequesterUser;

     std::unique_lock<std::mutex> Lock(BucketsMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          DroppedEvents.fetch_add(1, std::memory_order_relaxed);
          return;
     }

     if (WindowStartMS == 0)
     {
          WindowStartMS = NowMS();
     }

     if (Buckets.size() >= MaxBuckets && Buckets.find(Key) == Buckets.end())
     {
          Key.Collection = kOverflowCollection;
     }

     AnalyticsBucket &Bucket = Buckets[Key];
     Bucket.Count += Count;
     Bucket.AuthenticatedCount += Authenticated ? Count : 0ULL;

     if (EstimateBufferedBytesLocked() >= MaxBufferedBytes)
     {
          FlushRequested.store(true, std::memory_order_release);
          WakeCondition.notify_one();
     }
}

/* Run the periodic flush loop on the management thread pool. */

void AnalyticsManager::WorkerLoop()
{
     try
     {
          (void)FlushStartupEvent();

          while (true)
          {
               std::unique_lock<std::mutex> Lock(BucketsMutex);

               WakeCondition.wait_for(
                    Lock,
                    std::chrono::seconds(FlushIntervalSeconds),
                    [this]()
                    {
                         return !Running.load(std::memory_order_acquire) || FlushRequested.load(std::memory_order_acquire);
                    });

               bool ShouldExit = !Running.load(std::memory_order_acquire);
               bool FlushNowRequested = FlushRequested.exchange(false, std::memory_order_acq_rel);
               Lock.unlock();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("analytics", "Analytics worker wakeup: exit=" + std::string(ShouldExit ? "true" : "false") +
                                                           ", flush_now=" + std::string(FlushNowRequested ? "true" : "false") +
                                                           ", flush_interval=" + std::to_string(FlushIntervalSeconds) + "s.");
               }

               (void)FlushStartupEvent();

               FlushOnce();

               if (ShouldExit)
               {
                    break;
               }
          }
     }
     catch (const std::exception &Error)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("analytics", "Analytics worker crashed: " + std::string(Error.what()) + ".");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("analytics", "Analytics worker crashed with unknown exception.");
          }
     }
}

/* Flush the one-time startup analytics event. */

bool AnalyticsManager::FlushStartupEvent()
{
     if (!StartupEventPending.load(std::memory_order_acquire))
     {
          return true;
     }

     const std::string StartupPayload = BuildStartupPayload();

     if (StartupPayload.empty())
     {
          return false;
     }

     if (!PostPayload(StartupPayload))
     {
          return false;
     }

     StartupEventPending.store(false, std::memory_order_release);
     return true;
}

/* Flush the current in-memory analytics snapshot. */

void AnalyticsManager::FlushOnce()
{
     if (!IsEnabled())
     {
          return;
     }

     std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> Snapshot;
     std::vector<AnalyticsQueryEvent> QuerySnapshot;
     uint64_t SnapshotStartMS = 0;
     uint64_t SnapshotEndMS = NowMS();
     uint64_t TotalRequests = 0;

     {
          std::lock_guard<std::mutex> Lock(BucketsMutex);

          if (Buckets.empty())
          {
               QuerySnapshot.swap(QueryEvents);
               if (WindowStartMS == 0)
               {
                    WindowStartMS = SnapshotEndMS;
               }
               SnapshotStartMS = WindowStartMS;
               WindowStartMS = SnapshotEndMS;
          }
          else
          {
               Snapshot.swap(Buckets);
               QuerySnapshot.swap(QueryEvents);
               SnapshotStartMS = WindowStartMS;
               WindowStartMS = SnapshotEndMS;
          }
     }

     for (const auto &Entry : Snapshot)
     {
          TotalRequests += Entry.second.Count;
     }

     std::string Payload = BuildPayload(Snapshot, QuerySnapshot, SnapshotStartMS, SnapshotEndMS);

     if (Payload.empty())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("analytics", "Analytics usage flush skipped: payload generation returned empty.");
          }

          return;
     }

     if (!PostPayload(Payload))
     {
          std::lock_guard<std::mutex> Lock(BucketsMutex);

          if (WindowStartMS == 0 || SnapshotStartMS < WindowStartMS)
          {
               WindowStartMS = SnapshotStartMS;
          }

          MergeBucketsLocked(Snapshot);
          if (!QuerySnapshot.empty() && QueryEvents.size() < MaxQueryEvents)
          {
               const size_t SpaceLeft = MaxQueryEvents - QueryEvents.size();
               const size_t CopyCount = std::min(SpaceLeft, QuerySnapshot.size());
               QueryEvents.insert(QueryEvents.end(), QuerySnapshot.begin(), QuerySnapshot.begin() + static_cast<std::ptrdiff_t>(CopyCount));
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("analytics", "Analytics usage flush sent: buckets=" + std::to_string(Snapshot.size()) +
                                                  ", total_requests=" + std::to_string(TotalRequests) + ".");
     }
}

/* Estimate buffered analytics memory usage while locked. */

size_t AnalyticsManager::EstimateBufferedBytesLocked() const
{
     size_t Total = 0;

     for (const auto &Entry : Buckets)
     {
          Total += sizeof(Entry);
          Total += Entry.first.Action.capacity();
          Total += Entry.first.Collection.capacity();
          Total += Entry.first.RequesterIP.capacity();
          Total += Entry.first.RequesterUser.capacity();
     }

     for (const auto &Entry : QueryEvents)
     {
          Total += sizeof(Entry);
          Total += Entry.Action.capacity();
          Total += Entry.Collection.capacity();
          Total += Entry.Query.capacity();
          Total += Entry.DocumentID.capacity();
          Total += Entry.RequesterIP.capacity();
          Total += Entry.RequesterUser.capacity();
     }

     return Total;
}

/* Merge one failed flush snapshot back into the active buckets. */

void AnalyticsManager::MergeBucketsLocked(const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> &Snapshot)
{
     for (const auto &Entry : Snapshot)
     {
          AnalyticsBucketKey MergeKey = Entry.first;

          if (Buckets.size() >= MaxBuckets && Buckets.find(MergeKey) == Buckets.end())
          {
               MergeKey.Collection = kOverflowCollection;
          }

          AnalyticsBucket &Target = Buckets[MergeKey];
          Target.Count += Entry.second.Count;
          Target.AuthenticatedCount += Entry.second.AuthenticatedCount;
          Target.RequestBytes += Entry.second.RequestBytes;
          Target.ResponseBytes += Entry.second.ResponseBytes;
          Target.Status2xx += Entry.second.Status2xx;
          Target.Status4xx += Entry.second.Status4xx;
          Target.Status5xx += Entry.second.Status5xx;
          Target.SearchCount += Entry.second.SearchCount;
          Target.ClickCount += Entry.second.ClickCount;
          Target.SearchResultsFound += Entry.second.SearchResultsFound;
          Target.SearchResultsReturned += Entry.second.SearchResultsReturned;
          Target.SearchTimeMS += Entry.second.SearchTimeMS;
          Target.ClickRankSum += Entry.second.ClickRankSum;
     }
}

/* Normalize one request into an analytics bucket key. */

AnalyticsBucketKey AnalyticsManager::NormalizeKey(const HttpRequest &Request, RouteAction ActionVal) const
{
     AnalyticsBucketKey Key;
     Key.Action = RouteActionName(ActionVal);
     Key.Collection = ExtractCollectionName(Request);
     Key.RequesterIP = Request.RemoteAddress;
     Key.RequesterUser = Request.APIKeyID;

     if (Key.Collection.empty())
     {
          Key.Collection = kSystemCollection;
     }

     return Key;
}

/* Extract the collection name associated with one request. */

std::string AnalyticsManager::ExtractCollectionName(const HttpRequest &Request) const
{
     std::string PathValue = StripQueryString(Request.Path);
     const std::string Prefix = "/collections/";

     if (PathValue.rfind(Prefix, 0) != 0)
     {
          return kSystemCollection;
     }

     size_t StartPos = Prefix.size();
     size_t EndPos = PathValue.find('/', StartPos);

     if (EndPos == std::string::npos)
     {
          return PathValue.substr(StartPos);
     }

     return PathValue.substr(StartPos, EndPos - StartPos);
}

/* Estimate serialized request bytes for one analytics sample. */

uint64_t AnalyticsManager::EstimateRequestBytes(const HttpRequest &Request) const
{
     uint64_t Total = 0;

     Total += Request.Method.size() + 1;
     Total += Request.Path.size() + 1;
     Total += Request.Version.size() + 2;

     for (const auto &Header : Request.Headers)
     {
          Total += Header.first.size() + 2 + Header.second.size() + 2;
     }

     Total += 2;
     Total += Request.Body.size();
     return Total;
}

/* Estimate serialized response bytes for one analytics sample. */

uint64_t AnalyticsManager::EstimateResponseBytes(const HttpResponse &Response) const
{
     uint64_t Total = 0;
     std::string StatusCodeStr = std::to_string(Response.StatusCode);

     Total += 8 + 1;
     Total += StatusCodeStr.size() + 1;
     Total += Response.StatusText.size() + 2;

     for (const auto &Header : Response.Headers)
     {
          Total += Header.first.size() + 2 + Header.second.size() + 2;
     }

     Total += std::string("Content-Length").size() + 2 + std::to_string(Response.Body.size()).size() + 2;
     Total += std::string("Access-Control-Allow-Origin").size() + 2 + 1 + 2;
     Total += std::string("Access-Control-Allow-Methods").size() + 2 + std::string("GET, POST, PUT, DELETE, OPTIONS").size() + 2;
     Total += std::string("Access-Control-Allow-Headers").size() + 2 + std::string("Content-Type, Authorization, Accept, X-Requested-With, X-API-Key").size() + 2;
     Total += 2;
     Total += Response.Body.size();
     return Total;
}

/* Detect whether one request included authentication credentials. */

bool AnalyticsManager::HasAuthToken(const HttpRequest &Request) const
{
     auto HasNonEmptyHeader = [&Request](const char *Key) -> bool
     {
          auto It = Request.Headers.find(Key);
          return It != Request.Headers.end() && !It->second.empty();
     };

     return HasNonEmptyHeader("Authorization") ||
            HasNonEmptyHeader("authorization") ||
            HasNonEmptyHeader("X-API-Key") ||
            HasNonEmptyHeader("x-api-key");
}

/* Parse the configured remote analytics endpoint. */

AnalyticsManager::ParsedEndpoint AnalyticsManager::ParseEndpoint(const std::string &URLValue) const
{
     ParsedEndpoint Result;

     if (URLValue.empty())
     {
          return Result;
     }

     size_t SchemePos = URLValue.find("://");

     if (SchemePos == std::string::npos)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics disabled: analytics endpoint must include an explicit scheme.");
          }

          return Result;
     }

     Result.Scheme = URLValue.substr(0, SchemePos);

     std::transform(Result.Scheme.begin(), Result.Scheme.end(), Result.Scheme.begin(),
                    [](unsigned char Character)
                    {
                         return static_cast<char>(std::tolower(Character));
                    });

     if (Result.Scheme != "http" && Result.Scheme != "https")
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics disabled: only http:// and https:// analytics endpoints are supported.");
          }

          return Result;
     }

     size_t AuthorityStart = SchemePos + 3;
     size_t PathStart = URLValue.find('/', AuthorityStart);
     std::string Authority = (PathStart == std::string::npos) ? URLValue.substr(AuthorityStart)
                                                              : URLValue.substr(AuthorityStart, PathStart - AuthorityStart);

     if (Authority.empty())
     {
          return Result;
     }

     size_t PortPos = Authority.rfind(':');

     if (PortPos != std::string::npos && Authority.find(']') == std::string::npos)
     {
          Result.Host = Authority.substr(0, PortPos);

          try
          {
               int ParsedPort = std::stoi(Authority.substr(PortPos + 1));

               if (ParsedPort <= 0 || ParsedPort > 65535)
               {
                    return Result;
               }

               Result.Port = static_cast<uint16_t>(ParsedPort);
          }
          catch (...)
          {
               return Result;
          }
     }
     else
     {
          Result.Host = Authority;
          Result.Port = (Result.Scheme == "https") ? 443 : 80;
     }

     Result.Path = (PathStart == std::string::npos) ? "/" : URLValue.substr(PathStart);
     Result.Valid = !Result.Host.empty() && !Result.Path.empty();

     return Result;
}

/* Send one analytics payload to the remote endpoint. */

bool AnalyticsManager::PostPayload(const std::string &Body)
{
     if (!Endpoint.Valid)
     {
          return false;
     }

     struct addrinfo Hints;
     std::memset(&Hints, 0, sizeof(Hints));
     Hints.ai_family = AF_UNSPEC;
     Hints.ai_socktype = SOCK_STREAM;

     struct addrinfo *Addresses = nullptr;
     std::string PortString = std::to_string(Endpoint.Port);

     int ResolveResult = getaddrinfo(Endpoint.Host.c_str(), PortString.c_str(), &Hints, &Addresses);

     if (ResolveResult != 0)
     {
          FailedPosts.fetch_add(1, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics flush failed: DNS resolution failed for '" + Endpoint.Host + "'.");
          }

          return false;
     }

     int SocketFD = -1;

     for (struct addrinfo *Entry = Addresses; Entry != nullptr; Entry = Entry->ai_next)
     {
          SocketFD = socket(Entry->ai_family, Entry->ai_socktype, Entry->ai_protocol);

          if (SocketFD < 0)
          {
               continue;
          }

          struct timeval TimeoutValue;
          TimeoutValue.tv_sec = ConnectTimeoutMS / 1000;
          TimeoutValue.tv_usec = (ConnectTimeoutMS % 1000) * 1000;

          setsockopt(SocketFD, SOL_SOCKET, SO_RCVTIMEO, &TimeoutValue, sizeof(TimeoutValue));
          setsockopt(SocketFD, SOL_SOCKET, SO_SNDTIMEO, &TimeoutValue, sizeof(TimeoutValue));

          if (connect(SocketFD, Entry->ai_addr, Entry->ai_addrlen) == 0)
          {
               break;
          }

          close(SocketFD);
          SocketFD = -1;
     }

     freeaddrinfo(Addresses);

     if (SocketFD < 0)
     {
          FailedPosts.fetch_add(1, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics flush failed: could not connect to '" + EndpointURL + "'.");
          }

          return false;
     }

     std::ostringstream RequestStream;
     RequestStream << "POST " << Endpoint.Path << " HTTP/1.1\r\n"
                   << "Host: " << Endpoint.Host << "\r\n"
                   << "Content-Type: application/json\r\n";

     if (!APIToken.empty())
     {
          RequestStream << "X-API-Key: " << APIToken << "\r\n";
     }

     RequestStream << "Content-Length: " << Body.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << Body;

     std::string RequestData = RequestStream.str();

     std::string ResponseData;
     bool UseTLS = (Endpoint.Scheme == "https");

     if (UseTLS)
     {
#ifdef HLQUERY_HAS_OPENSSL
          SSL_CTX *Context = SSL_CTX_new(TLS_client_method());

          if (!Context)
          {
               FailedPosts.fetch_add(1, std::memory_order_relaxed);
               close(SocketFD);
               return false;
          }

          SSL_CTX_set_verify(Context, SSL_VERIFY_PEER, nullptr);

          if (SSL_CTX_set_default_verify_paths(Context) != 1)
          {
               SSL_CTX_free(Context);
               FailedPosts.fetch_add(1, std::memory_order_relaxed);
               close(SocketFD);
               return false;
          }

          SSL *TLSHandle = SSL_new(Context);

          if (!TLSHandle)
          {
               SSL_CTX_free(Context);
               FailedPosts.fetch_add(1, std::memory_order_relaxed);
               close(SocketFD);
               return false;
          }

          X509_VERIFY_PARAM *VerifyParams = SSL_get0_param(TLSHandle);

          if (X509_VERIFY_PARAM_set1_host(VerifyParams, Endpoint.Host.c_str(), 0) != 1)
          {
               SSL_free(TLSHandle);
               SSL_CTX_free(Context);
               FailedPosts.fetch_add(1, std::memory_order_relaxed);
               close(SocketFD);
               return false;
          }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
          SSL_set_tlsext_host_name(TLSHandle, Endpoint.Host.c_str());
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
          SSL_set_fd(TLSHandle, SocketFD);

          if (SSL_connect(TLSHandle) != 1)
          {
               SSL_free(TLSHandle);
               SSL_CTX_free(Context);
               FailedPosts.fetch_add(1, std::memory_order_relaxed);
               close(SocketFD);
               return false;
          }

          size_t TotalSent = 0;

          while (TotalSent < RequestData.size())
          {
               int Sent = SSL_write(TLSHandle, RequestData.data() + TotalSent, static_cast<int>(RequestData.size() - TotalSent));

               if (Sent <= 0)
               {
                    SSL_shutdown(TLSHandle);
                    SSL_free(TLSHandle);
                    SSL_CTX_free(Context);
                    FailedPosts.fetch_add(1, std::memory_order_relaxed);
                    close(SocketFD);
                    return false;
               }

               TotalSent += static_cast<size_t>(Sent);
          }

          char Buffer[4096];

          while (true)
          {
               int Received = SSL_read(TLSHandle, Buffer, static_cast<int>(sizeof(Buffer)));

               if (Received <= 0)
               {
                    break;
               }

               ResponseData.append(Buffer, static_cast<size_t>(Received));
          }

          SSL_shutdown(TLSHandle);
          SSL_free(TLSHandle);
          SSL_CTX_free(Context);
#else
          FailedPosts.fetch_add(1, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("analytics", "Analytics flush failed: HTTPS endpoint '" + EndpointURL + "' requires OpenSSL support in this build.");
          }

          close(SocketFD);
          return false;
#endif
     }
     else
     {
          size_t TotalSent = 0;

          while (TotalSent < RequestData.size())
          {
               ssize_t Sent = send(SocketFD, RequestData.data() + TotalSent, RequestData.size() - TotalSent, MSG_NOSIGNAL);

               if (Sent <= 0)
               {
                    FailedPosts.fetch_add(1, std::memory_order_relaxed);
                    close(SocketFD);
                    return false;
               }

               TotalSent += static_cast<size_t>(Sent);
          }

          char Buffer[4096];

          while (true)
          {
               ssize_t Received = recv(SocketFD, Buffer, sizeof(Buffer), 0);

               if (Received <= 0)
               {
                    break;
               }

               ResponseData.append(Buffer, static_cast<size_t>(Received));
          }
     }

     close(SocketFD);

     size_t FirstLineEnd = ResponseData.find("\r\n");
     std::string StatusLine = (FirstLineEnd == std::string::npos) ? ResponseData : ResponseData.substr(0, FirstLineEnd);

     std::istringstream StatusStream(StatusLine);
     std::string HTTPVersion;
     int StatusCode = 0;
     StatusStream >> HTTPVersion >> StatusCode;

     bool Success = (StatusCode >= 200 && StatusCode < 300);

     if (!Success && Instance && Instance->Logs)
     {
          Instance->Logs->Normal("analytics", "Analytics flush failed: remote endpoint returned status " +
                                                  std::to_string(StatusCode) + ".");
     }

     if (!Success)
     {
          FailedPosts.fetch_add(1, std::memory_order_relaxed);
     }

     return Success;
}

/* Build the regular analytics usage payload body. */

std::string AnalyticsManager::BuildPayload(
     const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> &Snapshot,
     const std::vector<AnalyticsQueryEvent> &QuerySnapshot,
     uint64_t SnapshotWindowStartMS,
     uint64_t SnapshotWindowEndMS)
{
     nlohmann::json Payload;
     Payload["type"] = "usage";
     Payload["used_db"] = UsedDB;
     Payload["window_start_ms"] = SnapshotWindowStartMS;
     Payload["window_end_ms"] = SnapshotWindowEndMS;
     Payload["bucket_count"] = Snapshot.size();

     uint64_t TotalRequests = 0;
     uint64_t TotalSearches = 0;
     uint64_t TotalClicks = 0;
     uint64_t TotalAuthenticatedRequests = 0;
     uint64_t TotalRequestBytes = 0;
     uint64_t TotalResponseBytes = 0;
     nlohmann::json Events = nlohmann::json::array();

     std::map<std::tuple<std::string, std::string, std::string, std::string>, AnalyticsBucket> OrderedBuckets;

     for (const auto &Entry : Snapshot)
     {
          OrderedBuckets[{Entry.first.Action, Entry.first.Collection, Entry.first.RequesterIP, Entry.first.RequesterUser}] = Entry.second;
     }

     for (const auto &Entry : OrderedBuckets)
     {
          const AnalyticsBucket &Bucket = Entry.second;
          TotalRequests += Bucket.Count;
          TotalSearches += Bucket.SearchCount;
          TotalClicks += Bucket.ClickCount;
          TotalAuthenticatedRequests += Bucket.AuthenticatedCount;
          TotalRequestBytes += Bucket.RequestBytes;
          TotalResponseBytes += Bucket.ResponseBytes;

          Events.push_back({
               {"action", std::get<0>(Entry.first)},
               {"collection", std::get<1>(Entry.first)},
               {"requester_ip", std::get<2>(Entry.first)},
               {"requester_user", std::get<3>(Entry.first)},
               {"count", Bucket.Count},
               {"authenticated_count", Bucket.AuthenticatedCount},
               {"request_bytes", Bucket.RequestBytes},
               {"response_bytes", Bucket.ResponseBytes},
               {"transferred_bytes", Bucket.RequestBytes + Bucket.ResponseBytes},
               {"status_2xx", Bucket.Status2xx},
               {"status_4xx", Bucket.Status4xx},
               {"status_5xx", Bucket.Status5xx},
               {"search_count", Bucket.SearchCount},
               {"click_count", Bucket.ClickCount},
               {"search_results_found", Bucket.SearchResultsFound},
               {"search_results_returned", Bucket.SearchResultsReturned},
               {"search_time_ms", Bucket.SearchTimeMS},
               {"click_rank_sum", Bucket.ClickRankSum},
          });
     }

     Payload["total_requests"] = TotalRequests;
     Payload["total_searches"] = TotalSearches;
     Payload["total_clicks"] = TotalClicks;
     Payload["processed_requests"] = TotalRequests;
     Payload["authenticated_requests"] = TotalAuthenticatedRequests;
     Payload["anonymous_requests"] = TotalRequests >= TotalAuthenticatedRequests ? (TotalRequests - TotalAuthenticatedRequests) : 0;
     Payload["request_bytes_total"] = TotalRequestBytes;
     Payload["response_bytes_total"] = TotalResponseBytes;
     Payload["transferred_bytes_total"] = TotalRequestBytes + TotalResponseBytes;
     Payload["request_megabytes_total"] = static_cast<double>(TotalRequestBytes) / (1024.0 * 1024.0);
     Payload["response_megabytes_total"] = static_cast<double>(TotalResponseBytes) / (1024.0 * 1024.0);
     Payload["transferred_megabytes_total"] = static_cast<double>(TotalRequestBytes + TotalResponseBytes) / (1024.0 * 1024.0);
     Payload["dropped_events"] = DroppedEvents.exchange(0, std::memory_order_acq_rel);
     Payload["failed_posts_total"] = FailedPosts.load(std::memory_order_relaxed);
     Payload["events"] = std::move(Events);

     if (!QuerySnapshot.empty())
     {
          nlohmann::json Searches = nlohmann::json::array();

          for (const auto &Entry : QuerySnapshot)
          {
               Searches.push_back({
                    {"action", Entry.Action},
                    {"collection", Entry.Collection.empty() ? "*" : Entry.Collection},
                    {"query", Entry.Query},
                    {"document_id", Entry.DocumentID.empty() ? nullptr : nlohmann::json(Entry.DocumentID)},
                    {"requester_ip", Entry.RequesterIP.empty() ? nullptr : nlohmann::json(Entry.RequesterIP)},
                    {"requester_user", Entry.RequesterUser.empty() ? nullptr : nlohmann::json(Entry.RequesterUser)},
                    {"authenticated", Entry.Authenticated},
                    {"search_time_ms", Entry.SearchTimeMS},
                    {"found", Entry.Found},
                    {"returned", Entry.Returned},
                    {"document_count", Entry.DocumentCount},
               });
          }

          Payload["searches"] = std::move(Searches);
     }

     return Payload.dump();
}

/* Build the startup analytics payload body. */

std::string AnalyticsManager::BuildStartupPayload() const
{
     nlohmann::json Payload;
     Payload["type"] = "startup";
     Payload["used_db"] = UsedDB;
     Payload["server"]["name"] = ServerName;
     Payload["server"]["id"] = ServerID;
     Payload["server"]["version"] = HLQUERY_VERSION;
     Payload["server"]["system"] = HLQUERY_SYSTEM;
     Payload["startup_time_ms"] = StartupTimeMS;
     Payload["process_id"] = static_cast<int>(getpid());
     Payload["analytics"]["flush_interval"] = FlushIntervalSeconds;
     Payload["analytics"]["connect_timeout_ms"] = ConnectTimeoutMS;
     Payload["analytics"]["track_reads"] = TrackReads;
     Payload["analytics"]["track_writes"] = TrackWrites;
     Payload["analytics"]["failed_posts_total"] = FailedPosts.load(std::memory_order_relaxed);
     Payload["analytics"]["dropped_events_total"] = DroppedEvents.load(std::memory_order_relaxed);
     Payload["events"] = nlohmann::json::array({{
          {"action", "Startup"},
          {"collection", "*"},
          {"count", 1},
          {"status_2xx", 1},
          {"status_4xx", 0},
          {"status_5xx", 0},
     }});

     return Payload.dump();
}

/* Return the current Unix time in milliseconds. */

uint64_t AnalyticsManager::NowMS() const
{
     auto NowValue = std::chrono::system_clock::now().time_since_epoch();
     return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(NowValue).count());
}
