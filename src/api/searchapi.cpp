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
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pthread.h>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "api/apikeys.h"
#include "api/searchapi.h"
#include "api/userauth.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/hybrid_rank_fusion.h"
#include "search/document_collection_store.h"
#include "search/rocksdb_storage_engine.h"
#include "search/lexical_inverted_index.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Implements shared SearchAPI lifecycle and request helper behavior. */

static constexpr const char *kReplicationOutboxPrefix = "replication_outbox:";
static constexpr const char *kReplicationAppliedPrefix = "replication_applied:";
static constexpr const char *kReplicationResyncStateKey = "replication_resync:active";
static constexpr const char *kReplicationResyncCollectionsPrefix = "replication_resync:collections:";

/* Returns a lowercase copy of the input string. */

static std::string ToLowerCopy(const std::string &Value)
{
     std::string Result(Value);
     std::transform(Result.begin(), Result.end(), Result.begin(), [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Result;
}

/* Normalizes path for extraction values. */

static std::string NormalizePathForExtraction(const std::string &Path)
{
     std::string NormalizedPath = Path;
     const size_t QueryPos = NormalizedPath.find('?');

     if (QueryPos != std::string::npos)
     {
          NormalizedPath = NormalizedPath.substr(0, QueryPos);
     }

     if (NormalizedPath.size() > 1 && NormalizedPath.back() == '/')
     {
          NormalizedPath.pop_back();
     }

     return NormalizedPath;
}

/* Returns a request header value using case-insensitive lookup. */

static std::string GetHeaderValueInsensitive(const std::map<std::string, std::string> &Headers, const std::string &Name)
{
     for (const auto &Pair : Headers)
     {
          if (ToLowerCopy(Pair.first) == ToLowerCopy(Name))
          {
               return Pair.second;
          }
     }

     return "";
}

/* Implements the trim header value helper. */

static std::string TrimHeaderValue(const std::string &Value)
{
     const std::size_t Start = Value.find_first_not_of(" \t");
     if (Start == std::string::npos)
     {
          return "";
     }

     const std::size_t End = Value.find_last_not_of(" \t");
     return Value.substr(Start, End - Start + 1);
}

/* Returns query param value values. */

static std::string GetQueryParamValue(const HttpRequest &Request, const std::string &Key)
{
     const auto It = Request.QueryParams.find(Key);
     if (It == Request.QueryParams.end())
     {
          return "";
     }

     return TrimHeaderValue(It->second);
}

/* Extracts peer auth token values. */

static std::string ExtractPeerAuthToken(const HttpRequest &Request)
{
     std::string AuthHeader = TrimHeaderValue(GetHeaderValueInsensitive(Request.Headers, "Authorization"));
     if (AuthHeader.empty())
     {
          const std::string ApiKey = TrimHeaderValue(GetHeaderValueInsensitive(Request.Headers, "X-API-Key"));
          if (!ApiKey.empty())
          {
               AuthHeader = "Bearer " + ApiKey;
          }
     }

     if (AuthHeader.rfind("Bearer ", 0) == 0)
     {
          AuthHeader = AuthHeader.substr(7);
     }

     return TrimHeaderValue(AuthHeader);
}

/* Checks whether authorized replication hop applies. */

static bool IsAuthorizedReplicationHop(const HttpRequest &Request)
{
     const std::string HopHeader = ToLowerCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Replication-Hop"));
     if (HopHeader != "1" && HopHeader != "true")
     {
          return false;
     }

     if (!Instance || !Instance->Config)
     {
          return false;
     }

     const std::string AuthToken = ExtractPeerAuthToken(Request);
     if (AuthToken.empty())
     {
          return false;
     }

     auto HasMatchingToken = [&](const std::vector<std::string> &Endpoints,
                                 bool UseSlaveTokens) -> bool
     {
          for (const auto &Endpoint : Endpoints)
          {
               std::string PrimaryToken;
               std::string SecondaryToken;
               const bool FoundTokens = UseSlaveTokens
                                             ? Instance->Config->GetSlavePeerTokens(Endpoint, &PrimaryToken, &SecondaryToken)
                                             : Instance->Config->GetClusterPeerTokens(Endpoint, &PrimaryToken, &SecondaryToken);
               if (!FoundTokens)
               {
                    continue;
               }

               if ((!PrimaryToken.empty() && AuthToken == PrimaryToken) ||
                   (!SecondaryToken.empty() && AuthToken == SecondaryToken))
               {
                    return true;
               }
          }

          return false;
     };

     if (HasMatchingToken(Instance->Config->GetSlaveNodes(), true))
     {
          return true;
     }

     if (HasMatchingToken(Instance->Config->GetClusterNodes(), false))
     {
          return true;
     }

     return false;
}

/* Validates collection name value input. */

static bool ValidateCollectionNameValue(const std::string &Name, std::string *ErrorMessage)
{
     if (Name.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required";
          }

          return false;
     }

     if (Name.length() > 64)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name must be between 1 and 64 characters";
          }

          return false;
     }

     for (unsigned char C : Name)
     {
          if (!std::isalnum(C) && C != '_' && C != '-' && C != '.')
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Collection name can only contain alphanumeric characters, underscores, hyphens, and dots";
               }

               return false;
          }
     }

     if (!std::isalpha(static_cast<unsigned char>(Name[0])) && Name[0] != '_')
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name must start with a letter or underscore";
          }

          return false;
     }

     return true;
}

/* Builds replication outbox key data. */

static std::string BuildReplicationOutboxKey(const std::string &EntryID)
{
     return std::string(kReplicationOutboxPrefix) + EntryID;
}

/* Builds replication applied key data. */

static std::string BuildReplicationAppliedKey(const std::string &OperationID)
{
     return std::string(kReplicationAppliedPrefix) + OperationID;
}

/* Implements the serialize replication outbox record helper. */

static nlohmann::json SerializeReplicationOutboxRecord(const std::string &EntryID,
                                                       const std::string &State,
                                                       const std::string &OperationLabel,
                                                       uint64_t TimestampMS,
                                                       const HttpRequest &Request)
{
     HttpRequest JournalRequest = Request;
     if (JournalRequest.Headers.find("X-HLQ-Replication-Op") == JournalRequest.Headers.end())
     {
          JournalRequest.Headers["X-HLQ-Replication-Op"] = EntryID;
     }

     nlohmann::json Record;
     Record["id"] = EntryID;
     Record["state"] = State;
     Record["operation"] = OperationLabel;
     Record["timestamp_ms"] = TimestampMS;
     Record["method"] = JournalRequest.Method;
     Record["path"] = JournalRequest.Path;
     Record["version"] = JournalRequest.Version;
     Record["operation_id"] = GetHeaderValueInsensitive(JournalRequest.Headers, "X-HLQ-Replication-Op");
     Record["body"] = JournalRequest.Body;
     Record["body_truncated"] = false;
     Record["remote_address"] = JournalRequest.RemoteAddress;
     Record["remote_port"] = JournalRequest.RemotePort;
     Record["api_key_id"] = JournalRequest.APIKeyID;
     Record["authenticated"] = JournalRequest.Authenticated;
     Record["headers"] = JournalRequest.Headers;
     Record["query_params"] = JournalRequest.QueryParams;
     return Record;
}

/* Returns replication resync session header values. */

static std::string GetReplicationResyncSessionHeader(const HttpRequest &Request)
{
     return TrimHeaderValue(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Resync-Session"));
}

/* Returns replication resync stage header values. */

static std::string GetReplicationResyncStageHeader(const HttpRequest &Request)
{
     return ToLowerCopy(TrimHeaderValue(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Resync-Stage")));
}

/* Builds replication resync collections key data. */

static std::string BuildReplicationResyncCollectionsKey(const std::string &SessionID)
{
     return std::string(kReplicationResyncCollectionsPrefix) + SessionID;
}

/* Extracts resync collection from path values. */

static std::string ExtractResyncCollectionFromPath(const std::string &Path)
{
     const std::string Prefix = "/collections/";

     if (Path.rfind(Prefix, 0) != 0)
     {
          return "";
     }

     const std::string Remainder = Path.substr(Prefix.size());
     const std::size_t SlashPos = Remainder.find('/');

     if (SlashPos == std::string::npos)
     {
          return Remainder;
     }

     return Remainder.substr(0, SlashPos);
}

/* Implements the track replication resync collection helper. */

static void TrackReplicationResyncCollection(const std::string &SessionID,
                                             const std::string &CollectionName)
{
     if (!Instance || !Instance->Database || SessionID.empty() || CollectionName.empty())
     {
          return;
     }

     const std::string Key = BuildReplicationResyncCollectionsKey(SessionID);
     std::vector<std::string> Collections;
     std::unordered_set<std::string> Seen;
     const std::string Existing = Instance->Database->Get(Key);

     if (!Existing.empty())
     {
          try
          {
               const nlohmann::json Root = nlohmann::json::parse(Existing);

               if (Root.is_array())
               {
                    for (const auto &Value : Root)
                    {
                         if (!Value.is_string())
                         {
                              continue;
                         }

                         const std::string Name = Value.get<std::string>();

                         if (!Name.empty() && Seen.insert(Name).second)
                         {
                              Collections.push_back(Name);
                         }
                    }
               }
          }
          catch (...)
          {
          }
     }

     if (Seen.insert(CollectionName).second)
     {
          Collections.push_back(CollectionName);
     }

     nlohmann::json Root = nlohmann::json::array();

     for (const auto &Name : Collections)
     {
          Root.push_back(Name);
     }

     (void)Instance->Database->Set(Key, Root.dump());
}

/* Loads replication resync collections data. */

static std::vector<std::string> LoadReplicationResyncCollections(const std::string &SessionID)
{
     std::vector<std::string> Collections;

     if (!Instance || !Instance->Database || SessionID.empty())
     {
          return Collections;
     }

     const std::string Existing = Instance->Database->Get(BuildReplicationResyncCollectionsKey(SessionID));

     if (Existing.empty())
     {
          return Collections;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(Existing);
          std::unordered_set<std::string> Seen;

          if (Root.is_array())
          {
               for (const auto &Value : Root)
               {
                    if (!Value.is_string())
                    {
                         continue;
                    }

                    const std::string Name = Value.get<std::string>();

                    if (!Name.empty() && Seen.insert(Name).second)
                    {
                         Collections.push_back(Name);
                    }
               }
          }
     }
     catch (...)
     {
     }

     return Collections;
}

/* Implements the clear replication resync collections helper. */

static void ClearReplicationResyncCollections(const std::string &SessionID)
{
     if (!Instance || !Instance->Database || SessionID.empty())
     {
          return;
     }

     (void)Instance->Database->Del(BuildReplicationResyncCollectionsKey(SessionID));
}

/* Returns replication operation header values. */

static std::string GetReplicationOperationHeader(const HttpRequest &Request)
{
     return TrimHeaderValue(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Replication-Op"));
}

/* Implements the crash point matches helper. */

static bool CrashPointMatches(const std::string &ConfiguredPoints, const std::string &Point)
{
     std::stringstream Stream(ConfiguredPoints);
     std::string Token;
     while (std::getline(Stream, Token, ','))
     {
          Token = TrimHeaderValue(Token);
          if (Token == Point)
          {
               return true;
          }
     }

     return false;
}

/* Returns the singleton SearchAPI instance. */

SearchAPI &SearchAPI::GetInstance()
{
     static SearchAPI SInstance;

     return SInstance;
}

/* Releases SearchAPI resources during shutdown. */

SearchAPI::~SearchAPI()
{
     Shutdown();
}

/* SearchAPIInstance is a free function for backward compatibility. */

SearchAPI &SearchAPIInstance()
{
     return SearchAPI::GetInstance();
}

/* Initialize initializes SearchAPI. */

bool SearchAPI::Initialize()
{
     /* Initialize the hybrid storage manager. */

     if (!HybridStorageManagerInstance().Initialize())
     {
          return false;
     }

     ReplayCommittedReplicationOutbox();

     return true;
}

/* Start starts SearchAPI. */

bool SearchAPI::Start()
{
     /* Initialize SearchAPI and User Authentication Manager. */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "Starting SearchAPI.");
     }

     if (!Initialize())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hlquery", "Failed to initialize SearchAPI.");
          }

          return false;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hlquery", "SearchAPI initialized successfully.");
     }

     EnsureReplicationMonitorStarted();
     EnsureDistributedLinkMonitorStarted();

     /* Initialize User Authentication Manager. */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "Initializing UserAuthManager.");
     }

     if (Instance)
     {
          Instance->Users = std::make_unique<UserAuthManager>();

          if (!Instance->Users->Initialize())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hlquery", "Failed to initialize UserAuthManager.");
               }

               return false;
          }

          if (Instance->Logs)
          {
               Instance->Logs->Normal("hlquery", "UserAuthManager initialized successfully.");
          }

          /* Initialize APIKeyManager. */

          if (Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Initializing APIKeyManager.");
          }

          if (!APIKeyManager::Instance().Initialize())
          {
               if (Instance->Logs)
               {
                    Instance->Logs->Normal("hlquery", "Failed to initialize APIKeyManager.");
               }
          }
          else
          {
               if (Instance->Logs)
               {
                    Instance->Logs->Normal("hlquery", "APIKeyManager initialized successfully.");
               }
          }
     }

     return true;
}

void SearchAPI::MaybeTriggerCrashInjection(const std::string &Point) const
{
     const char *ConfiguredPoints = std::getenv("HLQ_TEST_CRASH_POINT");
     if (!ConfiguredPoints || Point.empty() || !CrashPointMatches(ConfiguredPoints, Point))
     {
          return;
     }

     const char *MarkerPath = std::getenv("HLQ_TEST_CRASH_MARKER");
     if (MarkerPath && *MarkerPath)
     {
          std::ofstream MarkerFile(MarkerPath, std::ios::app);
          if (MarkerFile)
          {
               MarkerFile << Point << "\n";
          }
     }

     std::raise(SIGABRT);
}

bool SearchAPI::PrepareReplicationOutboxRecord(const HttpRequest &Request,
                                               const std::string &OperationLabel,
                                               std::string *OutEntryID,
                                               std::string *OutError) const
{
     if (OutEntryID)
     {
          OutEntryID->clear();
     }

     if (!ShouldAttemptReplication(Request))
     {
          return true;
     }

     if (!Instance || !Instance->Database)
     {
          if (OutError)
          {
               *OutError = "Replication outbox unavailable.";
          }
          return false;
     }

     const uint64_t Sequence = ReplicationOutboxClock.fetch_add(1, std::memory_order_relaxed);
     const uint64_t TimestampMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     const std::string EntryID = std::to_string(TimestampMS) + "-" + std::to_string(Sequence);
     const std::string Key = BuildReplicationOutboxKey(EntryID);
     const nlohmann::json Record = SerializeReplicationOutboxRecord(EntryID, "prepared", OperationLabel, TimestampMS, Request);

     if (!Instance->Database->Set(Key, Record.dump()) || !Instance->Database->SyncWAL())
     {
          if (OutError)
          {
               *OutError = "Failed to persist replication outbox record.";

               const std::string WriteError = Instance->Database->GetLastWriteErrorMessage();
               if (!WriteError.empty())
               {
                    *OutError += " " + WriteError;
               }
          }
          return false;
     }

     if (OutEntryID)
     {
          *OutEntryID = EntryID;
     }
     return true;
}

bool SearchAPI::MarkReplicationOutboxCommitted(const std::string &EntryID,
                                               const HttpRequest &Request,
                                               const std::string &OperationLabel,
                                               std::string *OutError) const
{
     if (EntryID.empty())
     {
          return true;
     }

     if (!Instance || !Instance->Database)
     {
          if (OutError)
          {
               *OutError = "Replication outbox unavailable.";
          }
          return false;
     }

     const uint64_t TimestampMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     const std::string Key = BuildReplicationOutboxKey(EntryID);
     const nlohmann::json Record = SerializeReplicationOutboxRecord(EntryID, "committed", OperationLabel, TimestampMS, Request);

     if (!Instance->Database->Set(Key, Record.dump()) || !Instance->Database->SyncWAL())
     {
          if (OutError)
          {
               *OutError = "Failed to mark replication outbox record committed.";

               const std::string WriteError = Instance->Database->GetLastWriteErrorMessage();
               if (!WriteError.empty())
               {
                    *OutError += " " + WriteError;
               }
          }
          return false;
     }

     return true;
}

void SearchAPI::ClearReplicationOutboxRecord(const std::string &EntryID) const
{
     if (EntryID.empty() || !Instance || !Instance->Database)
     {
          return;
     }

     const std::string Key = BuildReplicationOutboxKey(EntryID);
     Instance->Database->Del(Key);
     Instance->Database->SyncWAL();
}

void SearchAPI::FinalizeReplicationOutboxRecord(const std::string &EntryID) const
{
     if (!IsAsyncReplicationMode())
     {
          ClearReplicationOutboxRecord(EntryID);
     }
}

bool SearchAPI::IsAsyncReplicationMode() const
{
     return Instance && Instance->Config && Instance->Config->GetReplicationMode() == "async";
}

void SearchAPI::ReplayCommittedReplicationOutbox() const
{
     if (!Instance || !Instance->Database || !Instance->Config || !Instance->Config->GetReplicationEnabled())
     {
          return;
     }

     const std::vector<std::string> Keys = Instance->Database->Keys(kReplicationOutboxPrefix + std::string("*"), true);
     for (const auto &Key : Keys)
     {
          const std::string RawRecord = Instance->Database->Get(Key);
          if (RawRecord.empty())
          {
               continue;
          }

          try
          {
               const nlohmann::json Record = nlohmann::json::parse(RawRecord);
               const std::string State = Record.value("state", std::string());
               if (State == "prepared")
               {
                    /* A crash may have happened before the commit marker. The local
                     * write is authoritative, so a full resync closes either outcome. */
                    for (const auto &Endpoint : Instance->Config->GetSlaveNodes())
                    {
                         MarkSlaveDirty(Endpoint);
                    }
                    const std::string PreparedEntryID = Record.value("id", std::string());
                    if (!PreparedEntryID.empty())
                    {
                         ClearReplicationOutboxRecord(PreparedEntryID);
                    }
                    continue;
               }
               if (State != "committed")
               {
                    continue;
               }

               const std::string EntryID = Record.value("id", std::string());
               if (EntryID.empty())
               {
                    continue;
               }

               if (Record.value("body_truncated", false))
               {
                    /* The local database is authoritative; force a full resync instead of replaying corrupt JSON. */
                    for (const auto &Endpoint : Instance->Config->GetSlaveNodes())
                    {
                         MarkSlaveDirty(Endpoint);
                    }
                    ClearReplicationOutboxRecord(EntryID);
                    continue;
               }

               HttpRequest Request;
               Request.Method = Record.value("method", std::string());
               Request.Path = Record.value("path", std::string());
               Request.Version = Record.value("version", std::string("HTTP/1.1"));
               Request.Body = Record.value("body", std::string());
               Request.RemoteAddress = Record.value("remote_address", std::string());
               Request.RemotePort = Record.value("remote_port", 0);
               Request.APIKeyID = Record.value("api_key_id", std::string());
               Request.Authenticated = Record.value("authenticated", false);
               if (Record.contains("headers") && Record["headers"].is_object())
               {
                    Request.Headers = Record["headers"].get<std::map<std::string, std::string>>();
               }
               if (Record.contains("query_params") && Record["query_params"].is_object())
               {
                    Request.QueryParams = Record["query_params"].get<std::map<std::string, std::string>>();
               }
               Request.Headers["X-HLQ-Replication-Op"] = Record.value("operation_id", EntryID);

               std::string Error;
               const bool Replayed = ReplicateWriteRequest(Request,
                                                            Record.value("operation", std::string("recovered")),
                                                            &Error,
                                                            EntryID);
               if (Replayed && !IsAsyncReplicationMode())
               {
                    bool AnyDirty = false;
                    for (const auto &Endpoint : Instance->Config->GetSlaveNodes())
                    {
                         bool Dirty = false;
                         bool Resync = false;
                         (void)GetReplicationNodeState(Endpoint, nullptr, nullptr, &Dirty, &Resync);
                         AnyDirty = AnyDirty || Dirty || Resync;
                    }
                    if (!AnyDirty)
                    {
                         ClearReplicationOutboxRecord(EntryID);
                    }
               }
          }
          catch (...)
          {
               RecordReplicationFailure("Invalid replication outbox record: " + Key);
          }
     }
}

HttpResponse SearchAPI::CheckReplicationOperationDedup(const HttpRequest &Request,
                                                       const std::string &Operation) const
{
     if (!Instance || !Instance->Database || !IsAuthorizedReplicationHop(Request))
     {
          return HttpResponse(0, "", "");
     }

     const std::string OperationID = GetReplicationOperationHeader(Request);
     if (OperationID.empty())
     {
          return HttpResponse(0, "", "");
     }

     const std::string MarkerValue = Instance->Database->Get(BuildReplicationAppliedKey(OperationID));
     if (MarkerValue.empty())
     {
          return HttpResponse(0, "", "");
     }

     try
     {
          const nlohmann::json Marker = nlohmann::json::parse(MarkerValue);
          const int StatusCode = Marker.value("status_code", Status::OK);
          const std::string StatusString = Marker.value("status_text", StatusText(StatusCode));
          const std::string ContentType = Marker.value("content_type", std::string("application/json"));

          HttpResponse Response(StatusCode, StatusString, ContentType);
          Response.Body = Marker.value("body", std::string(""));
          Response.Headers["X-HLQ-Replication-Dedup"] = "1";

          if (Marker.contains("headers") && Marker["headers"].is_object())
          {
               for (auto It = Marker["headers"].begin(); It != Marker["headers"].end(); ++It)
               {
                    if (It.value().is_string())
                    {
                         Response.Headers[It.key()] = It.value().get<std::string>();
                    }
               }
          }

          return Response;
     }
     catch (...)
     {
          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          nlohmann::json Body;
          Body["error"] = "Replication dedupe unavailable";
          Body["message"] = "Replica found a corrupt replication dedupe marker and refused to reapply the operation.";
          Body["operation"] = Operation;
          Body["operation_id"] = OperationID;
          Response.Body = Body.dump();
          return Response;
     }
}

void SearchAPI::FinalizeReplicationOperation(const HttpRequest &Request,
                                             const HttpResponse &Response) const
{
     if (!Instance || !Instance->Database || !IsAuthorizedReplicationHop(Request))
     {
          return;
     }

     if (Response.StatusCode < 200 || Response.StatusCode >= 300)
     {
          return;
     }

     const std::string OperationID = GetReplicationOperationHeader(Request);
     if (OperationID.empty())
     {
          return;
     }

     nlohmann::json Marker;
     Marker["operation_id"] = OperationID;
     Marker["applied_ms"] = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     Marker["method"] = Request.Method;
     Marker["path"] = Request.Path;
     Marker["status_code"] = Response.StatusCode;
     Marker["status_text"] = Response.StatusText;
     Marker["content_type"] = GetHeaderValueInsensitive(Response.Headers, "Content-Type");
     Marker["headers"] = Response.Headers;
     Marker["body"] = Response.Body;

     if (!Instance->Database->Set(BuildReplicationAppliedKey(OperationID), Marker.dump()))
     {
          return;
     }

     Instance->Database->SyncWAL();
}

HttpResponse SearchAPI::CheckReplicationResyncFence(const HttpRequest &Request,
                                                    const std::string &Operation) const
{
     if (!Instance || !Instance->Database || !IsAuthorizedReplicationHop(Request))
     {
          return HttpResponse(0, "", "");
     }

     const std::string SessionID = GetReplicationResyncSessionHeader(Request);
     const std::string Stage = GetReplicationResyncStageHeader(Request);

     if (Stage == "start")
     {
          if (SessionID.empty())
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               nlohmann::json Body;
               Body["error"] = "Invalid replication resync session";
               Body["message"] = "Missing X-HLQ-Resync-Session header for resync start.";
               Body["operation"] = Operation;
               Response.Body = Body.dump();
               return Response;
          }

          nlohmann::json Marker;
          Marker["session"] = SessionID;
          Marker["started_ms"] = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
          Marker["operation"] = Operation;
          if (!Instance->Database->Set(kReplicationResyncStateKey, Marker.dump()) || !Instance->Database->SyncWAL())
          {
               HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               nlohmann::json Body;
               Body["error"] = "Replication resync unavailable";
               Body["message"] = "Failed to persist replica resync fence state.";
               Body["operation"] = Operation;
               Response.Body = Body.dump();
               return Response;
          }
     }

     const std::string ActiveState = Instance->Database->Get(kReplicationResyncStateKey);
     if (ActiveState.empty())
     {
          return HttpResponse(0, "", "");
     }

     std::string ActiveSession;
     try
     {
          nlohmann::json Marker = nlohmann::json::parse(ActiveState);
          ActiveSession = Marker.value("session", "");
     }
     catch (...)
     {
          ActiveSession = "";
     }

     if (ActiveSession.empty() || ActiveSession == SessionID)
     {
          return HttpResponse(0, "", "");
     }

     HttpResponse Response(Status::CONFLICT, StatusText(Status::CONFLICT), "application/json");
     nlohmann::json Body;
     Body["error"] = "Replica resync in progress";
     Body["message"] = "Replica is fenced for a full resync session and rejected a conflicting replication write.";
     Body["active_session"] = ActiveSession;
     Body["operation"] = Operation;
     Response.Body = Body.dump();
     return Response;
}

void SearchAPI::FinalizeReplicationResyncRequest(const HttpRequest &Request,
                                                 const HttpResponse &Response) const
{
     if (!Instance || !Instance->Database || !IsAuthorizedReplicationHop(Request))
     {
          return;
     }

     if (Response.StatusCode < 200 || Response.StatusCode >= 300)
     {
          return;
     }

     if (GetReplicationResyncStageHeader(Request) != "complete")
     {
          return;
     }

     const std::string SessionID = GetReplicationResyncSessionHeader(Request);
     if (SessionID.empty())
     {
          return;
     }

     const std::string CollectionName = ExtractResyncCollectionFromPath(Request.Path);
     const bool IsBulkImportRequest =
          (Request.Method == "POST" &&
           Request.Path.find("/documents/import") != std::string::npos);

     if (IsBulkImportRequest && !CollectionName.empty())
     {
          TrackReplicationResyncCollection(SessionID, CollectionName);
     }

     if (GetReplicationResyncStageHeader(Request) != "complete")
     {
          return;
     }

     const std::string ActiveState = Instance->Database->Get(kReplicationResyncStateKey);
     if (ActiveState.empty())
     {
          ClearReplicationResyncCollections(SessionID);
          return;
     }

     std::string ActiveSession;
     try
     {
          nlohmann::json Marker = nlohmann::json::parse(ActiveState);
          ActiveSession = Marker.value("session", "");
     }
     catch (...)
     {
          return;
     }

     if (ActiveSession != SessionID)
     {
          return;
     }

     const std::vector<std::string> ResyncedCollections = LoadReplicationResyncCollections(SessionID);
     ClearReplicationResyncCollections(SessionID);
     Instance->Database->Del(kReplicationResyncStateKey);
     Instance->Database->SyncWAL();
}

void SearchAPI::CleanupFinishedAsyncReplicationTasks() const
{
     std::lock_guard<std::mutex> lock(AsyncReplicationTasksMutex);

     auto It = AsyncReplicationTasks.begin();
     while (It != AsyncReplicationTasks.end())
     {
          if (It->valid() && It->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
          {
               It->get();
               It = AsyncReplicationTasks.erase(It);
               continue;
          }

          ++It;
     }
}

void SearchAPI::EnqueueAsyncReplicationTask(std::function<void()> Task) const
{
     CleanupFinishedAsyncReplicationTasks();

     std::lock_guard<std::mutex> lock(AsyncReplicationTasksMutex);
     AsyncReplicationTasks.push_back(std::async(std::launch::async, [Task = std::move(Task)]() mutable
                                                {
                                                     Task();
                                                }));
}

/* Implements the shutdown helper. */

void SearchAPI::Shutdown()
{
     DistributedLinkMonitorStop.store(true, std::memory_order_relaxed);
     DistributedLinkMonitorCV.notify_all();

     if (DistributedLinkMonitorThread.joinable())
     {
          DistributedLinkMonitorThread.join();
     }

     ReplicationMonitorStop.store(true, std::memory_order_relaxed);
     ReplicationMonitorCV.notify_all();

     if (ReplicationMonitorThread.joinable())
     {
          ReplicationMonitorThread.join();
     }

     std::vector<std::future<void>> PendingTasks;
     {
          std::lock_guard<std::mutex> lock(AsyncReplicationTasksMutex);
          PendingTasks.swap(AsyncReplicationTasks);
     }

     if (!PendingTasks.empty())
     {
          MaybeTriggerCrashInjection("replication_shutdown_async");
     }

     for (auto &Task : PendingTasks)
     {
          if (Task.valid())
          {
               Task.get();
          }
     }
}

/* IsInitialized checks if SearchAPI is initialized. */

bool SearchAPI::IsInitialized() const
{
     return HybridStorageManagerInstance().IsInitialized();
}

/* Implements the attach search response meta helper. */

void SearchAPI::AttachSearchResponseMeta(HttpResponse &Response,
                                         const ComprehensiveSearchQuery &Query,
                                         const HttpRequest &Request,
                                         const std::string &CollectionName)
{
     try
     {
          nlohmann::json Root = nlohmann::json::parse(Response.Body);
          nlohmann::json Meta = nlohmann::json::object();

          Meta["query"] = Query.Q;
          Meta["exact_match"] = Query.PrioritizeExactMatch;
          Meta["highlight"] = Query.Highlight;
          Meta["distributed"] = ShouldAttemptDistributedSearch(Request);

          const std::string Route = GetQueryParamValue(Request, "route");
          if (!Route.empty())
          {
               Meta["route"] = Route;
          }

          if (!CollectionName.empty())
          {
               Meta["collection"] = CollectionName;
          }

          const std::string ExecutionMode = TrimHeaderValue(GetHeaderValueInsensitive(Response.Headers, "X-HLQ-Execution-Mode"));
          if (!ExecutionMode.empty())
          {
               Meta["execution_mode"] = ExecutionMode;
          }

          Root["meta"] = Meta;
          Response.Body = Root.dump();
     }
     catch (...)
     {
     }
}

uint64_t SearchAPI::GetCollectionMutationVersion(const std::string &Collection) const
{
     std::lock_guard<std::mutex> lock(CollectionMutationMutex);

     uint64_t collection_version = 0;
     auto it = CollectionMutationVersions.find(Collection);
     if (it != CollectionMutationVersions.end())
     {
          collection_version = it->second;
     }

     uint64_t global_version = 0;
     auto global_it = CollectionMutationVersions.find("*");
     if (global_it != CollectionMutationVersions.end())
     {
          global_version = global_it->second;
     }

     return std::max(collection_version, global_version);
}

/* Implements the bump collection mutation version helper. */

uint64_t SearchAPI::BumpCollectionMutationVersion(const std::string &Collection)
{
     const uint64_t next_version = CollectionMutationClock.fetch_add(1, std::memory_order_relaxed) + 1;

     {
          std::lock_guard<std::mutex> lock(CollectionMutationMutex);
          CollectionMutationVersions[Collection] = next_version;
     }

     return next_version;
}

/* Implements the reset collection mutation versions helper. */

void SearchAPI::ResetCollectionMutationVersions()
{
     std::lock_guard<std::mutex> lock(CollectionMutationMutex);
     CollectionMutationVersions.clear();
}

ReplicationStatusSnapshot SearchAPI::GetReplicationStatusSnapshot() const
{
     ReplicationStatusSnapshot Snapshot;
     Snapshot.RequestsAttempted = ReplicationRequestsAttempted.load(std::memory_order_relaxed);
     Snapshot.RequestsSucceeded = ReplicationRequestsSucceeded.load(std::memory_order_relaxed);
     Snapshot.RequestsFailed = ReplicationRequestsFailed.load(std::memory_order_relaxed);
     Snapshot.ReplicaAcks = ReplicationReplicaAcks.load(std::memory_order_relaxed);

     std::lock_guard<std::mutex> lock(ReplicationStatusMutex);
     Snapshot.LastError = LastReplicationError;
     Snapshot.LastErrorTimestampMS = LastReplicationErrorTimestampMS;
     return Snapshot;
}

/* CheckReadOnlyMode checks if read-only mode is active. */

HttpResponse SearchAPI::CheckReadOnlyMode(const HttpRequest &Request, const std::string &Operation)
{
     if (!Instance || !Instance->Config)
     {
          return HttpResponse(0, "", "");
     }

     if (!Instance->Config->GetReplicaModeEnabled() || Instance->Config->GetReplicaAllowWrites())
     {
          return HttpResponse(0, "", "");
     }

     if (IsAuthorizedReplicationHop(Request))
     {
          HttpResponse FenceResponse = CheckReplicationResyncFence(Request, Operation);
          if (FenceResponse.StatusCode != 0)
          {
               return FenceResponse;
          }
          return HttpResponse(0, "", "");
     }

     HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
     Response.Body = "{\"error\":\"Read-only replica\",\"message\":\"Writes are disabled on this replica. Replication traffic is still allowed.\",\"operation\":\"" + EscapeJSONString(Operation) + "\"}";
     return Response;
}

/* ResolveCollectionName resolves collection name. */

std::string SearchAPI::ResolveCollectionName(const std::string &Name)
{
     if (Name.empty())
     {
          return "";
     }

     /* 1. Check if name is a real collection. */

     if (HybridStorageManagerInstance().CollectionExists(Name))
     {
          return Name;
     }

     /* 2. Check if name is an alias. */

     std::string AliasKey = "alias:" + Name;
     std::string AliasJSON = HybridStorageManagerInstance().Get(AliasKey);

     if (!AliasJSON.empty())
     {
          try
          {
               nlohmann::json Data = nlohmann::json::parse(AliasJSON);

               if (Data.contains("collection_name") && Data["collection_name"].is_string())
               {
                    return Data["collection_name"].get<std::string>();
               }
               else if (Data.contains("collection") && Data["collection"].is_string())
               {
                    return Data["collection"].get<std::string>();
               }
          }
          catch (...)
          {
               /* Ignore parse error, return original name. */
          }
     }

     return Name;
}

/* ExtractCollectionFromPath extracts collection name from path. */

std::string SearchAPI::ExtractCollectionFromPath(const std::string &Path)
{
     /* Extract collection name from paths like /collections/{name}/documents. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex CollectionRegex(R"(^/collections/([^/]+)(?:/|$))");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, CollectionRegex))
     {
          std::string CollectionName = Match[1].str();

          /* Validate collection name doesn't contain path traversal attempts. */

          if (CollectionName.find("..") != std::string::npos || CollectionName.find("/") != std::string::npos || CollectionName.find("\\") != std::string::npos)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "SECURITY: Path traversal attempt detected in collection name: " + CollectionName + ".");
               }

               return "";
          }

          /* Full format validation: length, characters, starting character (matches ValidateCollectionSchema). */

          std::string ValidationError;
          if (!ValidateCollectionNameValue(CollectionName, &ValidationError))
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "Invalid collection name '" + CollectionName + "': " + ValidationError + ".");
               }

               return "";
          }

          /* Resolve alias if it exists. */

          return ResolveCollectionName(CollectionName);
     }

     return "";
}

/* ExtractDocumentIdFromPath extracts document ID from path. */

std::string SearchAPI::ExtractDocumentIdFromPath(const std::string &Path)
{
     /* Extract document ID from paths like /collections/{name}/documents/{id}. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex DocumentRegex(R"(^/collections/[^/]+/documents/([^/]+)(?:/context)?$)");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, DocumentRegex))
     {
          std::string DocID = Match[1].str();

          /* Validate document ID doesn't contain path traversal attempts. */

          if (DocID.find("..") != std::string::npos || DocID.find("/") != std::string::npos || DocID.find("\\") != std::string::npos)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "SECURITY: Path traversal attempt detected in document ID: " + DocID + ".");
               }

               return "";
          }

          /*
     * Full format validation: length (1-256 characters) and character set
     * (alphanumeric, underscores, hyphens, dots).
     * This ensures consistent validation across all endpoints that use this function.
     */

          if (DocID.empty())
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "Document ID is empty.");
               }

               return "";
          }

          if (DocID.size() > 256)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "Document ID too long: " + DocID + " (length: " + std::to_string(DocID.length()) + ").");
               }

               return "";
          }

          /* Check for valid characters (alphanumeric, underscore, hyphen). */

          for (unsigned char C : DocID)
          {
               if (!std::isalnum(C) && C != '_' && C != '-' && C != '.')
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("search_api", "Document ID contains invalid character: " + std::string(1, static_cast<char>(C)) + " in ID: " + DocID + ".");
                    }

                    return "";
               }
          }

          return DocID;
     }

     return "";
}

/* ExtractSynonymIdFromPath extracts synonym ID from path. */

std::string SearchAPI::ExtractSynonymIdFromPath(const std::string &Path)
{
     /* Extract synonym ID from paths like /collections/{name}/synonyms/{id}. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex SynonymRegex(R"(^/collections/[^/]+/(?:synonyms|synonym_sets)/([^/]+)$)");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, SynonymRegex))
     {
          return Match[1].str();
     }

     std::regex GlobalSynonymSetRegex(R"(^/synonym_sets/global/(?:items/)?([^/]+)$)");

     if (std::regex_search(NormalizedPath, Match, GlobalSynonymSetRegex))
     {
          return Match[1].str();
     }

     return "";
}

/* ExtractStopwordFromPath extracts stopword from path. */

std::string SearchAPI::ExtractStopwordFromPath(const std::string &Path)
{
     /* Extract stopword from paths like /collections/{name}/stopwords/{word}. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex StopwordRegex(R"(^/collections/[^/]+/(?:stopwords|stopword_sets)/([^/]+)$)");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, StopwordRegex))
     {
          return Match[1].str();
     }

     /* Extract stopword from global paths like /stopwords/global/{word}. */

     std::regex GlobalStopwordRegex(R"(^/stopwords/global/([^/]+)$)");
     if (std::regex_search(NormalizedPath, Match, GlobalStopwordRegex))
     {
          return Match[1].str();
     }

     std::regex GlobalStopwordSetRegex(R"(^/stopword_sets/global/(?:items/)?([^/]+)$)");
     if (std::regex_search(NormalizedPath, Match, GlobalStopwordSetRegex))
     {
          return Match[1].str();
     }

     return "";
}

/* ExtractOverrideIdFromPath extracts override ID from path. */

std::string SearchAPI::ExtractOverrideIdFromPath(const std::string &Path)
{
     /* Extract override ID from paths like /collections/{name}/overrides/{id}. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex OverrideRegex(R"(^/collections/[^/]+/(?:overrides|curations|curation_sets)/([^/]+)$)");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, OverrideRegex))
     {
          return Match[1].str();
     }

     return "";
}

/* ExtractAliasNameFromPath extracts alias name from path. */

std::string SearchAPI::ExtractAliasNameFromPath(const std::string &Path)
{
     /* Extract alias name from paths like /aliases/{name}. */

     const std::string NormalizedPath = NormalizePathForExtraction(Path);
     std::regex AliasRegex(R"(^/aliases/([^/]+)$)");
     std::smatch Match;

     if (std::regex_search(NormalizedPath, Match, AliasRegex))
     {
          std::string AliasName = Match[1].str();

          if (AliasName.find("..") != std::string::npos || AliasName.find("/") != std::string::npos || AliasName.find("\\") != std::string::npos)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "SECURITY: Path traversal attempt detected in alias name: " + AliasName + ".");
               }

               return "";
          }

          if (AliasName.empty())
          {
               return "";
          }

          if (AliasName.length() > 64)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "Alias name too long: " + AliasName + " (length: " + std::to_string(AliasName.length()) + ").");
               }

               return "";
          }

          for (unsigned char C : AliasName)
          {
               if (!std::isalnum(C) && C != '_' && C != '-' && C != '.')
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("search_api", "Alias name contains invalid character: " + std::string(1, static_cast<char>(C)) + " in name: " + AliasName + ".");
                    }

                    return "";
               }
          }

          if (!std::isalpha(static_cast<unsigned char>(AliasName[0])) && AliasName[0] != '_')
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "Alias name must start with letter or underscore: " + AliasName + ".");
               }

               return "";
          }

          return AliasName;
     }

     return "";
}

/* GenerateJSONResponse generates JSON response from results. */

std::string SearchAPI::GenerateJSONResponse(const std::vector<SearchResult> &Results)
{
     std::ostringstream JSON;

     JSON << "{";
     JSON << "\"found\":" << Results.size() << ",";
     JSON << "\"out_of\":" << Results.size() << ",";
     JSON << "\"page\":" << 1 << ",";
     JSON << "\"search_time_ms\":" << 0 << ",";
     JSON << "\"hits\":[";

     for (size_t I = 0; I < Results.size(); ++I)
     {
          if (I > 0)
          {
               JSON << ",";
          }

          const SearchResult &Result = Results[I];

          JSON << "{";
          JSON << "\"document\":{";
          JSON << "\"id\":\"" << EscapeJSONString(Result.ID) << "\",";

          /* SearchResult doesn't have title - would need to extract from document. */

          JSON << "\"title\":\"\"";

          if (!Result.Document.empty())
          {
               JSON << ",";

               bool First = true;

               /* SearchResult has document map, not fields. */

               for (const auto &FieldPair : Result.Document)
               {
                    if (!First)
                    {
                         JSON << ",";
                    }

                    JSON << "\"" << EscapeJSONString(FieldPair.first) << "\":\"" << EscapeJSONString(FieldPair.second) << "\"";

                    First = false;
               }
          }

          JSON << "},";
          JSON << "\"text_match\":" << Result.Score << ",";

          /* SearchResult doesn't have snippet. */

          JSON << "\"text_match_info\":\"\"";
          JSON << "}";
     }

     JSON << "]}";

     return JSON.str();
}

/* GenerateErrorResponse generates error response. */

std::string SearchAPI::GenerateErrorResponse(const std::string &Error, int StatusCode)
{
     return "{\"error\":\"" + EscapeJSONString(Error) + "\",\"status\":" + std::to_string(StatusCode) + "}";
}

/* ValidateFieldName validates field name. */

bool SearchAPI::ValidateFieldName(const std::string &FieldName, std::string *ErrorMsg)
{
     if (FieldName.empty())
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Field name cannot be empty";
          }

          return false;
     }

     if (FieldName.length() > 64)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Field name cannot exceed 64 characters";
          }

          return false;
     }

     /* Check for invalid characters: commas and other non-alphanumeric (except underscore and hyphen). */

     for (char C : FieldName)
     {
          if (C == ',')
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = "Field name cannot contain commas";
               }

               return false;
          }

          if (!std::isalnum(static_cast<unsigned char>(C)) && C != '_' && C != '-')
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = "Field name can only contain alphanumeric characters, underscores, and hyphens (no commas)";
               }

               return false;
          }
     }

     /* Check if starts with letter or underscore. */

     if (!std::isalpha(static_cast<unsigned char>(FieldName[0])) && FieldName[0] != '_')
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Field name must start with a letter or underscore";
          }

          return false;
     }

     /* Check reserved fields. */

     const std::unordered_set<std::string> ReservedFields = {"id", "score", "timestamp", "_id", "_score", "_timestamp"};

     if (ReservedFields.find(FieldName) != ReservedFields.end())
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Field name '" + FieldName + "' is reserved and cannot be used";
          }

          return false;
     }

     return true;
}

/* ValidateFieldValue validates field value. */

bool SearchAPI::ValidateFieldValue(const std::string &FieldValue, std::string *ErrorMsg, const std::string &FieldName)
{
     /* Check for other problematic control characters (but allow normal text). */

     for (char C : FieldValue)
     {
          /* Allow printable characters, newlines, tabs, but reject other control chars. */

          if (static_cast<unsigned char>(C) < 32 && C != '\n' && C != '\r' && C != '\t')
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = "Field values cannot contain control characters";
               }

               return false;
          }
     }

     return true;
}

/* ValidateQueryInput validates query and filter inputs to reduce injection risk. */

bool SearchAPI::ValidateQueryInput(const std::string &Value, std::string *ErrorMsg, size_t MaxBytes, const std::string &FieldName, bool AllowNewlines)
{
     if (Value.empty())
     {
          return true;
     }

     if (Value.size() > MaxBytes)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Parameter '" + FieldName + "' exceeds maximum size of " + std::to_string(MaxBytes) + " bytes";
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("search_api", "SECURITY: Oversized input for '" + FieldName + "' (" + std::to_string(Value.size()) + " bytes) - rejected.");
          }

          return false;
     }

     for (unsigned char C : Value)
     {
          if (C == 0)
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = "Parameter '" + FieldName + "' contains a null byte";
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("search_api", "SECURITY: Null byte detected in '" + FieldName + "' - rejected.");
               }

               return false;
          }

          if (C < 32 || C == 127)
          {
               if (AllowNewlines && (C == '\n' || C == '\r' || C == '\t'))
               {
                    continue;
               }

               if (ErrorMsg)
               {
                    *ErrorMsg = "Parameter '" + FieldName + "' contains control characters";
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("search_api", "SECURITY: Control character detected in '" + FieldName + "' - rejected.");
               }

               return false;
          }
     }

     return true;
}

/* ParseDocumentFromJSON parses document from JSON. */

bool SearchAPI::ParseDocumentFromJSON(const std::string &Json, Document &DocumentObj, std::string *ErrorMsg)
{
     try
     {
          nlohmann::json DocJSON;

          try
          {
               /* Parse with explicit depth limit (default is 32, but be explicit). */

               DocJSON = nlohmann::json::parse(Json, nullptr, true, true);
          }
          catch (const nlohmann::json::parse_error &E)
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = std::string("JSON parse error: ") + E.what();
               }

               return false;
          }

          return ParseDocumentFromJSON(DocJSON, DocumentObj, ErrorMsg);
     }
     catch (const std::exception &E)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = std::string("Document parsing failed: ") + E.what();
          }

          return false;
     }
}

/* Parses document from JSON input. */

bool SearchAPI::ParseDocumentFromJSON(const nlohmann::json &DocJSON, Document &DocumentObj, std::string *ErrorMsg)
{
     try
     {
          /* Validate that DocJSON is an object. */

          if (!DocJSON.is_object())
          {
               if (ErrorMsg)
               {
                    *ErrorMsg = "Document must be a JSON object";
               }

               return false;
          }

          /* Extract basic fields with NULL validation. */

          if (DocJSON.contains("id"))
          {
               if (DocJSON["id"].is_null())
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Field 'id' cannot be null";
                    }

                    return false;
               }

               if (DocJSON["id"].is_string())
               {
                    DocumentObj.ID = DocJSON["id"].get<std::string>();
               }
               else
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Field 'id' must be a string";
                    }

                    return false;
               }
          }

          /* Handle title field - check both "title" and "doc_title". */

          if (DocJSON.contains("title"))
          {
               if (DocJSON["title"].is_null())
               {
                    DocumentObj.Title = "";
               }
               else if (DocJSON["title"].is_string())
               {
                    DocumentObj.Title = DocJSON["title"].get<std::string>();

                    std::string TitleError;

                    if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid title value: " + TitleError;
                         }

                         return false;
                    }
               }
               else
               {
                    DocumentObj.Title = DocJSON["title"].dump();

                    std::string TitleError;

                    if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid title value: " + TitleError;
                         }

                         return false;
                    }
               }
          }
          else if (DocJSON.contains("doc_title"))
          {
               if (DocJSON["doc_title"].is_null())
               {
                    DocumentObj.Title = "";
               }
               else if (DocJSON["doc_title"].is_string())
               {
                    DocumentObj.Title = DocJSON["doc_title"].get<std::string>();

                    std::string TitleError;

                    if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid title value: " + TitleError;
                         }

                         return false;
                    }

                    DocumentObj.Fields["doc_title"] = DocumentObj.Title;
               }
               else
               {
                    DocumentObj.Title = DocJSON["doc_title"].dump();

                    std::string TitleError;

                    if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid title value: " + TitleError;
                         }

                         return false;
                    }

                    DocumentObj.Fields["doc_title"] = DocumentObj.Title;
               }
          }

          /* Handle content field - check both "content" and "doc_content". */

          if (DocJSON.contains("content"))
          {
               if (DocJSON["content"].is_null())
               {
                    DocumentObj.Content = "";
               }
               else if (DocJSON["content"].is_string())
               {
                    DocumentObj.Content = DocJSON["content"].get<std::string>();

                    std::string ContentError;

                    if (!ValidateFieldValue(DocumentObj.Content, &ContentError))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid content value: " + ContentError;
                         }

                         return false;
                    }
               }
               else
               {
                    DocumentObj.Content = DocJSON["content"].dump();

                    std::string ContentError;

                    if (!ValidateFieldValue(DocumentObj.Content, &ContentError))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid content value: " + ContentError;
                         }

                         return false;
                    }
               }
          }
          else if (DocJSON.contains("doc_content"))
          {
               if (DocJSON["doc_content"].is_null())
               {
                    DocumentObj.Content = "";
               }
               else if (DocJSON["doc_content"].is_string())
               {
                    DocumentObj.Content = DocJSON["doc_content"].get<std::string>();

                    std::string ContentError;

                    if (!ValidateFieldValue(DocumentObj.Content, &ContentError))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid content value: " + ContentError;
                         }

                         return false;
                    }
               }
               else
               {
                    DocumentObj.Content = DocJSON["doc_content"].dump();

                    std::string ContentError;

                    if (!ValidateFieldValue(DocumentObj.Content, &ContentError))
                    {
                         if (ErrorMsg)
                         {
                              *ErrorMsg = "Invalid content value: " + ContentError;
                         }

                         return false;
                    }
               }
          }

          if (DocJSON.contains("score"))
          {
               if (DocJSON["score"].is_null())
               {
                    DocumentObj.Score = 0.0;
               }
               else if (DocJSON["score"].is_number())
               {
                    DocumentObj.Score = DocJSON["score"].get<double>();
               }
               else
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Field 'score' must be numeric";
                    }

                    return false;
               }
          }

          if (DocJSON.contains("timestamp"))
          {
               if (DocJSON["timestamp"].is_null())
               {
                    DocumentObj.Timestamp = 0;
               }
               else if (DocJSON["timestamp"].is_number_integer() || DocJSON["timestamp"].is_number_unsigned())
               {
                    DocumentObj.Timestamp = DocJSON["timestamp"].get<uint64_t>();
               }
               else
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Field 'timestamp' must be an integer";
                    }

                    return false;
               }
          }

          /* Extract all other fields (excluding metadata fields). */

          for (const auto &[Key, Value] : DocJSON.items())
          {
               if (Key == "id" || Key == "title" || Key == "content" || Key == "doc_title" || Key == "doc_content" || Key == "score" || Key == "timestamp")
               {
                    continue;
               }

               if (Key == "fields" && Value.is_object())
               {
                    for (const auto &[NestedKey, NestedValue] : Value.items())
                    {
                         std::string NestedFieldNameError;

                         if (!ValidateFieldName(NestedKey, &NestedFieldNameError))
                         {
                              if (ErrorMsg)
                              {
                                   *ErrorMsg = "Invalid field name '" + NestedKey + "': " + NestedFieldNameError;
                              }

                              return false;
                         }

                         if (NestedValue.is_null())
                         {
                              continue;
                         }

                         std::string NestedFieldValue;

                         if (NestedValue.is_string())
                         {
                              NestedFieldValue = NestedValue.get<std::string>();
                         }
                         else if (NestedValue.is_number_integer())
                         {
                              NestedFieldValue = std::to_string(NestedValue.get<int64_t>());
                         }
                         else if (NestedValue.is_number_float())
                         {
                              NestedFieldValue = std::to_string(NestedValue.get<double>());
                         }
                         else if (NestedValue.is_boolean())
                         {
                              NestedFieldValue = NestedValue.get<bool>() ? "true" : "false";
                         }
                         else
                         {
                              NestedFieldValue = NestedValue.dump();
                         }

                         std::string NestedFieldValueError;

                         if (!ValidateFieldValue(NestedFieldValue, &NestedFieldValueError, NestedKey))
                         {
                              if (ErrorMsg)
                              {
                                   *ErrorMsg = "Invalid field value for '" + NestedKey + "': " + NestedFieldValueError;
                              }

                              return false;
                         }

                         DocumentObj.Fields[NestedKey] = std::move(NestedFieldValue);
                    }

                    continue;
               }

               std::string FieldNameError;

               if (!ValidateFieldName(Key, &FieldNameError))
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Invalid field name '" + Key + "': " + FieldNameError;
                    }

                    return false;
               }

               if (Value.is_null())
               {
                    continue;
               }

               std::string FieldValue;

               if (Value.is_string())
               {
                    FieldValue = Value.get<std::string>();
               }
               else if (Value.is_number_integer())
               {
                    FieldValue = std::to_string(Value.get<int64_t>());
               }
               else if (Value.is_number_float())
               {
                    FieldValue = std::to_string(Value.get<double>());
               }
               else if (Value.is_boolean())
               {
                    FieldValue = Value.get<bool>() ? "true" : "false";
               }
               else
               {
                    FieldValue = Value.dump();
               }

               std::string FieldValueError;

               if (!ValidateFieldValue(FieldValue, &FieldValueError, Key))
               {
                    if (ErrorMsg)
                    {
                         *ErrorMsg = "Invalid field value for '" + Key + "': " + FieldValueError;
                    }

                    return false;
               }

               DocumentObj.Fields[Key] = std::move(FieldValue);
          }

          return true;
     }
     catch (const std::exception &E)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = std::string("Document parsing failed: ") + E.what();
          }

          return false;
     }
}

/* ParseCollectionConfigFromJSON parses collection configuration from JSON. */

bool SearchAPI::ParseCollectionConfigFromJSON(const std::string &Json, CollectionConfig &Config)
{
     try
     {
          nlohmann::json Parsed = nlohmann::json::parse(Json);

          if (Parsed.contains("name") && Parsed["name"].is_string())
          {
               Config.Name = Parsed["name"].get<std::string>();
          }
          else
          {
               Config.Name = ExtractJSONValue(Json, "name");
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "Extracted name: '" + Config.Name + "'.");
          }

          if (Parsed.contains("fields") && Parsed["fields"].is_array())
          {
               for (const auto &Field : Parsed["fields"])
               {
                    if (Field.is_object() && Field.contains("name") && Field["name"].is_string())
                    {
                         std::string FieldName = Field["name"].get<std::string>();
                         std::string FieldType = "string";

                         if (Field.contains("type") && Field["type"].is_string())
                         {
                              FieldType = Field["type"].get<std::string>();
                         }

                         Config.Fields[FieldName] = FieldType;
                    }
               }
          }
          else if (Parsed.contains("fields") && Parsed["fields"].is_object())
          {
               for (auto It = Parsed["fields"].begin(); It != Parsed["fields"].end(); ++It)
               {
                    if (It.value().is_string())
                    {
                         Config.Fields[It.key()] = It.value().get<std::string>();
                    }
                    else if (!It.value().is_null())
                    {
                         Config.Fields[It.key()] = It.value().dump();
                    }
               }
          }
          else if (Parsed.contains("searchable_fields") && Parsed["searchable_fields"].is_array())
          {
               for (const auto &Field : Parsed["searchable_fields"])
               {
                    if (Field.is_string())
                    {
                         Config.Fields[Field.get<std::string>()] = "string";
                    }
               }
          }

          if (Parsed.contains("metadata") && Parsed["metadata"].is_object())
          {
               for (auto It = Parsed["metadata"].begin(); It != Parsed["metadata"].end(); ++It)
               {
                    if (It.value().is_string())
                    {
                         Config.Metadata[It.key()] = It.value().get<std::string>();
                    }
                    else if (!It.value().is_null())
                    {
                         Config.Metadata[It.key()] = It.value().dump();
                    }
                    else
                    {
                         Config.Metadata.erase(It.key());
                    }
               }
          }

          if (Parsed.contains("default_sorting_field") && Parsed["default_sorting_field"].is_string())
          {
               const std::string DefaultSortingField = Parsed["default_sorting_field"].get<std::string>();

               if (!DefaultSortingField.empty())
               {
                    Config.Metadata["_default_sorting_field"] = DefaultSortingField;
               }
          }

          if (Parsed.contains("language") && Parsed["language"].is_string())
          {
               const std::string Language = Parsed["language"].get<std::string>();

               if (!Language.empty() && Language != "auto")
               {
                    Config.Metadata["_lang"] = Language;
               }
          }

          if (Parsed.contains("lang") && Parsed["lang"].is_string())
          {
               const std::string Language = Parsed["lang"].get<std::string>();

               if (!Language.empty() && Language != "auto")
               {
                    Config.Metadata["_lang"] = Language;
               }
          }

          if (Parsed.is_object())
          {
               for (auto It = Parsed.begin(); It != Parsed.end(); ++It)
               {
                    if (!It.key().empty() && It.key()[0] == '_')
                    {
                         if (It.value().is_string())
                         {
                              Config.Metadata[It.key()] = It.value().get<std::string>();
                         }
                         else if (!It.value().is_null())
                         {
                              Config.Metadata[It.key()] = It.value().dump();
                         }
                         else
                         {
                              Config.Metadata.erase(It.key());
                         }
                    }
               }
          }
     }
     catch (const nlohmann::json::exception &E)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "JSON parse failed, using fallback: " + std::string(E.what()) + ".");
          }

          Config.Name = ExtractJSONValue(Json, "name");
     }

     if (Config.Name.empty())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "ParseCollectionConfigFromJSON: name is empty.");
          }

          return false;
     }

     return true;
}

/* ValidateCollectionSchema validates collection schema. */

bool SearchAPI::ValidateCollectionSchema(const CollectionConfig &Config, std::string &ErrorMessage)
{
     ErrorMessage.clear();
     if (!ValidateCollectionNameValue(Config.Name, &ErrorMessage))
     {
          return false;
     }

     if (Config.Fields.empty())
     {
          ErrorMessage = "Collection schema must include at least one field.";
          return false;
     }

     static const std::unordered_set<std::string> AllowedTypes = {
          "string",
          "string[]",
          "keyword",
          "int",
          "int32",
          "int64",
          "float",
          "float[]",
          "double",
          "bool",
          "boolean",
          "geo",
          "geopoint",
          "geo_point",
          "latlon",
          "object",
          "json",
          "vector"};

     for (const auto &FieldEntry : Config.Fields)
     {
          if (!ValidateFieldName(FieldEntry.first, &ErrorMessage))
          {
               return false;
          }

          if (FieldEntry.second.empty())
          {
               ErrorMessage = "Field '" + FieldEntry.first + "' must declare a type.";
               return false;
          }

          std::string NormalizedType = FieldEntry.second;
          std::transform(NormalizedType.begin(), NormalizedType.end(), NormalizedType.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });

          if (AllowedTypes.find(NormalizedType) == AllowedTypes.end())
          {
               ErrorMessage = "Field '" + FieldEntry.first + "' has unsupported type '" + FieldEntry.second + "'.";
               return false;
          }
     }

     return true;
}

/* EscapeJSONString escapes string for JSON inclusion. */

std::string SearchAPI::EscapeJSONString(const std::string &Str)
{
     static const size_t MaxEscapeInputSize = 10 * 1024 * 1024;

     std::string StrToEscape = Str;

     bool Truncated = false;

     if (Str.size() > MaxEscapeInputSize)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "EscapeJSONString: Input too large (" + std::to_string(Str.size()) + " bytes), truncating to prevent ReDoS.");
          }

          StrToEscape = Str.substr(0, MaxEscapeInputSize);

          Truncated = true;
     }

     std::string Escaped;

     Escaped.reserve(StrToEscape.length() * 2);

     for (char C : StrToEscape)
     {
          switch (C)
          {
               case '"':
                    Escaped += "\\\"";
                    break;
               case '\\':
                    Escaped += "\\\\";
                    break;
               case '\b':
                    Escaped += "\\b";
                    break;
               case '\f':
                    Escaped += "\\f";
                    break;
               case '\n':
                    Escaped += "\\n";
                    break;
               case '\r':
                    Escaped += "\\r";
                    break;
               case '\t':
                    Escaped += "\\t";
                    break;
               default:
                    Escaped += C;
                    break;
          }
     }

     if (Truncated)
     {
          Escaped += "...[truncated]";
     }

     return Escaped;
}

/* EscapeHTMLString escapes string for HTML inclusion. */

std::string SearchAPI::EscapeHTMLString(const std::string &Str)
{
     std::string Escaped;

     Escaped.reserve(Str.length() * 2);

     for (char C : Str)
     {
          switch (C)
          {
               case '<':
                    Escaped += "&lt;";
                    break;
               case '>':
                    Escaped += "&gt;";
                    break;
               case '&':
                    Escaped += "&amp;";
                    break;
               case '"':
                    Escaped += "&quot;";
                    break;
               case '\'':
                    Escaped += "&#39;";
                    break;
               default:
                    Escaped += C;
                    break;
          }
     }

     return Escaped;
}

/* ExtractJSONValue extracts value from JSON by key. */

std::string SearchAPI::ExtractJSONValue(const std::string &Json, const std::string &Key)
{
     std::string Pattern = "\"" + Key + "\"\\s*:\\s*\"([^\"]+)\"";
     std::regex Regex(Pattern);
     std::smatch Match;

     if (std::regex_search(Json, Match, Regex))
     {
          return Match[1].str();
     }

     Pattern = "\"" + Key + "\"\\s*:\\s*(true|false)";
     Regex = std::regex(Pattern);

     if (std::regex_search(Json, Match, Regex))
     {
          return Match[1].str();
     }

     Pattern = "\"" + Key + "\"\\s*:\\s*(\\d+)";
     Regex = std::regex(Pattern);

     if (std::regex_search(Json, Match, Regex))
     {
          return Match[1].str();
     }

     return "";
}

/* ExtractJSONArray extracts array from JSON by key. */

std::vector<std::string> SearchAPI::ExtractJSONArray(const std::string &Json, const std::string &Key)
{
     std::vector<std::string> Result;
     std::string Pattern = "\"" + Key + "\"\\s*:\\s*\\[([^\\]]+)\\]";
     std::regex Regex(Pattern);
     std::smatch Match;

     if (std::regex_search(Json, Match, Regex))
     {
          std::string ArrayContent = Match[1].str();
          std::istringstream Iss(ArrayContent);
          std::string Item;

          while (std::getline(Iss, Item, ','))
          {
               Item.erase(std::remove(Item.begin(), Item.end(), '"'), Item.end());
               Item.erase(std::remove(Item.begin(), Item.end(), ' '), Item.end());

               if (!Item.empty())
               {
                    Result.push_back(Item);
               }
          }
     }

     return Result;
}

/* GetCurrentTimestamp gets current timestamp as ISO 8601 string. */

std::string SearchAPI::GetCurrentTimestamp()
{
     long long MSSinceEpoch;

     if (Instance)
     {
          MSSinceEpoch = Instance->NowMs();
     }
     else
     {
          MSSinceEpoch = NowMs();
     }

     time_t TimeTVal = static_cast<time_t>(MSSinceEpoch / 1000);
     long long MSVal = MSSinceEpoch % 1000;

     struct tm TMBuf;
     struct tm *TM = gmtime_r(&TimeTVal, &TMBuf);

     if (!TM)
     {
          return std::to_string(TimeTVal) + ".000Z";
     }

     std::ostringstream OSS;

     OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
     OSS << '.' << std::setfill('0') << std::setw(3) << MSVal << 'Z';

     return OSS.str();
}

/* GetCollectionCreatedAt gets collection creation timestamp. */

std::string SearchAPI::GetCollectionCreatedAt(const std::string &CollectionName)
{
     const time_t TimeVal = HybridStorageManagerInstance().GetCollectionCreatedAt(CollectionName);

     if (TimeVal > 0)
     {
          struct tm TMBuf;
          struct tm *TM = gmtime_r(&TimeVal, &TMBuf);

          if (TM)
          {
               std::ostringstream OSS;
               OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
               OSS << ".000Z";
               return OSS.str();
          }
     }

     return GetCurrentTimestamp();
}
