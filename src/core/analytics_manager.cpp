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

#include "core/analytics_manager.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#include "core/config.h"
#include "core/logmanager.h"
#include "core/thread_limit.h"
#include "vendor/json/json.hpp"

namespace
{
constexpr const char* kSystemCollection = "*";
constexpr const char* kOverflowCollection = "__other__";

bool IsReadAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::GetCollection:
          case RouteAction::DocumentSearch:
          case RouteAction::VectorSearch:
          case RouteAction::MultiSearch:
          case RouteAction::GlobalSearch:
          case RouteAction::ListDocuments:
          case RouteAction::GetDocument:
          case RouteAction::FacetCounts:
          case RouteAction::ExportDocuments:
               return true;
          default:
               return false;
     }
}

bool IsWriteAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
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
               return true;
          default:
               return false;
     }
}

std::string StripQueryString(const std::string& PathValue)
{
     size_t QueryPos = PathValue.find('?');

     if (QueryPos == std::string::npos)
     {
          return PathValue;
     }

     return PathValue.substr(0, QueryPos);
}
}

AnalyticsManager::AnalyticsManager(const std::string& ServerNameValue,
                                   const std::string& ServerIDValue,
                                   const std::string& EndpointURLValue,
                                   int FlushIntervalSecondsValue,
                                   int ConnectTimeoutMSValue,
                                   bool TrackReadsValue,
                                   bool TrackWritesValue,
                                   LogManager* LoggerValue)
    : ServerName(ServerNameValue),
      ServerID(ServerIDValue),
      EndpointURL(EndpointURLValue),
      FlushIntervalSeconds(std::max(30, FlushIntervalSecondsValue)),
      ConnectTimeoutMS(std::max(250, ConnectTimeoutMSValue)),
      TrackReads(TrackReadsValue),
      TrackWrites(TrackWritesValue),
      Logger(LoggerValue),
      Endpoint(ParseEndpoint(EndpointURLValue))
{
     Enabled.store(Endpoint.Valid, std::memory_order_release);
}

AnalyticsManager::~AnalyticsManager()
{
     Stop();
}

void AnalyticsManager::Start()
{
     if (!IsEnabled() || Started.exchange(true, std::memory_order_acq_rel))
     {
          return;
     }

     if (!ThreadLimit::TryAcquireThreadSlot())
     {
          if (Logger)
          {
               Logger->Normal("analytics", "Analytics disabled: no thread slots available for analytics worker.");
          }

          Enabled.store(false, std::memory_order_release);
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

     WorkerThread = std::thread(&AnalyticsManager::WorkerLoop, this);

     if (Logger)
     {
          Logger->Normal("analytics", "Analytics manager started (endpoint: " + EndpointURL +
                                           ", flush_interval=" + std::to_string(FlushIntervalSeconds) + "s).");
     }
}

void AnalyticsManager::Stop()
{
     if (!Started.exchange(false, std::memory_order_acq_rel))
     {
          return;
     }

     Running.store(false, std::memory_order_release);
     WakeCondition.notify_all();

     if (WorkerThread.joinable())
     {
          WorkerThread.join();
     }
}

void AnalyticsManager::FlushNow()
{
     FlushOnce();
}

void AnalyticsManager::RecordRequest(const HttpRequest& Request, RouteAction ActionVal, int StatusCode)
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

     AnalyticsBucket& Bucket = Buckets[Key];
     Bucket.Count++;

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
}

void AnalyticsManager::WorkerLoop()
{
     ThreadLimit::SetThreadName("hlq-analytics");

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
                         return !Running.load(std::memory_order_acquire);
                    });

               bool ShouldExit = !Running.load(std::memory_order_acquire);
               Lock.unlock();

               (void)FlushStartupEvent();

               FlushOnce();

               if (ShouldExit)
               {
                    break;
               }
          }
     }
     catch (const std::exception& Error)
     {
          if (Logger)
          {
               Logger->Critical("analytics", "Analytics worker crashed: " + std::string(Error.what()) + ".");
          }
     }
     catch (...)
     {
          if (Logger)
          {
               Logger->Critical("analytics", "Analytics worker crashed with unknown exception.");
          }
     }

     ThreadLimit::DecrementThreadCount();
}

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

void AnalyticsManager::FlushOnce()
{
     if (!IsEnabled())
     {
          return;
     }

     std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash> Snapshot;
     uint64_t SnapshotStartMS = 0;
     uint64_t SnapshotEndMS = NowMS();

     {
          std::lock_guard<std::mutex> Lock(BucketsMutex);

          if (Buckets.empty())
          {
               if (WindowStartMS == 0)
               {
                    WindowStartMS = SnapshotEndMS;
               }

               return;
          }

          Snapshot.swap(Buckets);
          SnapshotStartMS = WindowStartMS;
          WindowStartMS = SnapshotEndMS;
     }

     std::string Payload = BuildPayload(Snapshot, SnapshotStartMS, SnapshotEndMS);

     if (Payload.empty())
     {
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
     }
}

void AnalyticsManager::MergeBucketsLocked(const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash>& Snapshot)
{
     for (const auto& Entry : Snapshot)
     {
          AnalyticsBucketKey MergeKey = Entry.first;

          if (Buckets.size() >= MaxBuckets && Buckets.find(MergeKey) == Buckets.end())
          {
               MergeKey.Collection = kOverflowCollection;
          }

          AnalyticsBucket& Target = Buckets[MergeKey];
          Target.Count += Entry.second.Count;
          Target.Status2xx += Entry.second.Status2xx;
          Target.Status4xx += Entry.second.Status4xx;
          Target.Status5xx += Entry.second.Status5xx;
     }
}

AnalyticsBucketKey AnalyticsManager::NormalizeKey(const HttpRequest& Request, RouteAction ActionVal) const
{
     AnalyticsBucketKey Key;
     Key.Action = RouteActionName(ActionVal);
     Key.Collection = ExtractCollectionName(Request);

     if (Key.Collection.empty())
     {
          Key.Collection = kSystemCollection;
     }

     return Key;
}

std::string AnalyticsManager::ExtractCollectionName(const HttpRequest& Request) const
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

AnalyticsManager::ParsedEndpoint AnalyticsManager::ParseEndpoint(const std::string& URLValue) const
{
     ParsedEndpoint Result;

     if (URLValue.empty())
     {
          return Result;
     }

     size_t SchemePos = URLValue.find("://");

     if (SchemePos == std::string::npos)
     {
          if (Logger)
          {
               Logger->Normal("analytics", "Analytics disabled: analytics endpoint must include an explicit scheme.");
          }

          return Result;
     }

     Result.Scheme = URLValue.substr(0, SchemePos);

     std::transform(Result.Scheme.begin(), Result.Scheme.end(), Result.Scheme.begin(),
                    [](unsigned char Character)
                    {
                         return static_cast<char>(std::tolower(Character));
                    });

     if (Result.Scheme != "http")
     {
          if (Logger)
          {
               Logger->Normal("analytics", "Analytics disabled: only http:// analytics endpoints are currently supported.");
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
          Result.Port = 80;
     }

     Result.Path = (PathStart == std::string::npos) ? "/" : URLValue.substr(PathStart);
     Result.Valid = !Result.Host.empty() && !Result.Path.empty();

     return Result;
}

bool AnalyticsManager::PostPayload(const std::string& Body)
{
     if (!Endpoint.Valid)
     {
          return false;
     }

     struct addrinfo Hints;
     std::memset(&Hints, 0, sizeof(Hints));
     Hints.ai_family = AF_UNSPEC;
     Hints.ai_socktype = SOCK_STREAM;

     struct addrinfo* Addresses = nullptr;
     std::string PortString = std::to_string(Endpoint.Port);

     int ResolveResult = getaddrinfo(Endpoint.Host.c_str(), PortString.c_str(), &Hints, &Addresses);

     if (ResolveResult != 0)
     {
          FailedPosts.fetch_add(1, std::memory_order_relaxed);

          if (Logger)
          {
               Logger->Normal("analytics", "Analytics flush failed: DNS resolution failed for '" + Endpoint.Host + "'.");
          }

          return false;
     }

     int SocketFD = -1;

     for (struct addrinfo* Entry = Addresses; Entry != nullptr; Entry = Entry->ai_next)
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

          if (Logger)
          {
               Logger->Normal("analytics", "Analytics flush failed: could not connect to '" + EndpointURL + "'.");
          }

          return false;
     }

     std::ostringstream RequestStream;
     RequestStream << "POST " << Endpoint.Path << " HTTP/1.1\r\n"
                   << "Host: " << Endpoint.Host << "\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << Body.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << Body;

     std::string RequestData = RequestStream.str();

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

     std::string ResponseData;
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

     close(SocketFD);

     size_t FirstLineEnd = ResponseData.find("\r\n");
     std::string StatusLine = (FirstLineEnd == std::string::npos) ? ResponseData : ResponseData.substr(0, FirstLineEnd);

     std::istringstream StatusStream(StatusLine);
     std::string HTTPVersion;
     int StatusCode = 0;
     StatusStream >> HTTPVersion >> StatusCode;

     bool Success = (StatusCode >= 200 && StatusCode < 300);

     if (!Success && Logger)
     {
          Logger->Normal("analytics", "Analytics flush failed: remote endpoint returned status " +
                                           std::to_string(StatusCode) + ".");
     }

     if (!Success)
     {
          FailedPosts.fetch_add(1, std::memory_order_relaxed);
     }

     return Success;
}

std::string AnalyticsManager::BuildPayload(
    const std::unordered_map<AnalyticsBucketKey, AnalyticsBucket, AnalyticsBucketKeyHash>& Snapshot,
    uint64_t SnapshotWindowStartMS,
    uint64_t SnapshotWindowEndMS)
{
     if (Snapshot.empty())
     {
          return "";
     }

     nlohmann::json Payload;
     Payload["type"] = "usage";
     Payload["server"]["name"] = ServerName;
     Payload["server"]["id"] = ServerID;
     Payload["server"]["version"] = HLQUERY_VERSION;
     Payload["window_start_ms"] = SnapshotWindowStartMS;
     Payload["window_end_ms"] = SnapshotWindowEndMS;
     Payload["bucket_count"] = Snapshot.size();

     uint64_t TotalRequests = 0;
     nlohmann::json Events = nlohmann::json::array();

     std::map<std::pair<std::string, std::string>, AnalyticsBucket> OrderedBuckets;

     for (const auto& Entry : Snapshot)
     {
          OrderedBuckets[{Entry.first.Action, Entry.first.Collection}] = Entry.second;
     }

     for (const auto& Entry : OrderedBuckets)
     {
          const AnalyticsBucket& Bucket = Entry.second;
          TotalRequests += Bucket.Count;

          Events.push_back({
              {"action", Entry.first.first},
              {"collection", Entry.first.second},
              {"count", Bucket.Count},
              {"status_2xx", Bucket.Status2xx},
              {"status_4xx", Bucket.Status4xx},
              {"status_5xx", Bucket.Status5xx},
          });
     }

     Payload["total_requests"] = TotalRequests;
     Payload["dropped_events"] = DroppedEvents.exchange(0, std::memory_order_acq_rel);
     Payload["failed_posts_total"] = FailedPosts.load(std::memory_order_relaxed);
     Payload["events"] = std::move(Events);

     return Payload.dump();
}

std::string AnalyticsManager::BuildStartupPayload() const
{
     nlohmann::json Payload;
     Payload["type"] = "startup";
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
     Payload["events"] = nlohmann::json::array({
         {
             {"action", "Startup"},
             {"collection", "*"},
             {"count", 1},
             {"status_2xx", 1},
             {"status_4xx", 0},
             {"status_5xx", 0},
         }});

     return Payload.dump();
}

uint64_t AnalyticsManager::NowMS() const
{
     auto NowValue = std::chrono::system_clock::now().time_since_epoch();
     return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(NowValue).count());
}
