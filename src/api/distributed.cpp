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
#include <ctime>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <string>
#include <thread>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifdef HLQUERY_HAS_OPENSSL

#include <openssl/err.h>
#include <openssl/ssl.h>

#else

struct ssl_ctx_st;
struct ssl_st;
using SSL_CTX = ssl_ctx_st;
using SSL = ssl_st;

#endif

#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

/* Provides distributed search and replication API handlers. */

static std::string ToLowerCopy(const std::string &Value)
{
     std::string Out = Value;
     std::transform(Out.begin(), Out.end(), Out.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Out;
}

/* Returns a request header value using case-insensitive lookup. */

static std::string GetHeaderValueInsensitive(const std::map<std::string, std::string> &Headers, const std::string &Name)
{
     auto It = Headers.find(Name);

     if (It != Headers.end())
     {
          return It->second;
     }

     std::string LowerName = ToLowerCopy(Name);

     for (const auto &Pair : Headers)
     {
          if (ToLowerCopy(Pair.first) == LowerName)
          {
               return Pair.second;
          }
     }

     return "";
}

/* Formats a steady-clock duration as milliseconds. */

static std::string DurationToMillisecondsString(const std::chrono::steady_clock::duration &Duration)
{
     return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Duration).count());
}

/* Returns a trimmed copy of the input string. */

static std::string TrimCopy(const std::string &Value)
{
     size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

/* Builds a unique replication operation id. */

static std::string BuildReplicationOperationID()
{
     const uint64_t TimestampMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     static std::atomic<uint64_t> Sequence{1};
     return std::to_string(TimestampMS) + "-" + std::to_string(Sequence.fetch_add(1, std::memory_order_relaxed));
}

/* Ensures replication requests have a stable operation id. */

static void EnsureReplicationOperationID(HttpRequest &Request)
{
     if (!TrimCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Replication-Op")).empty())
     {
          return;
     }

     Request.Headers["X-HLQ-Replication-Op"] = BuildReplicationOperationID();
}

/* Parses node endpoint input. */

static bool ParseNodeEndpoint(const std::string &Raw, std::string &HostOut, int &PortOut, bool &UseSSLOut)
{
     std::string Scheme;
     if (!ParseSharedNodeEndpoint(Raw, HostOut, PortOut, &Scheme))
     {
          return false;
     }

     if (!Scheme.empty() && Scheme != "http")
     {
          if (Scheme != "https")
          {
               return false;
          }
     }

     UseSSLOut = (Scheme == "https");
     return true;
}

/* Checks whether a host name refers to the local node. */

static bool IsLocalHostName(const std::string &Host)
{
     std::string Lower = ToLowerCopy(Host);
     return (Lower == "localhost" || Lower == "127.0.0.1" || Lower == "::1" || Lower == "0.0.0.0");
}

struct NodeEndpoint
{
     std::string Endpoint;
     std::string Host;
     int Port = 0;
     bool UseSSL = false;
     bool IsLocal = false;
};

/* Builds a readable label for a distributed node. */

static std::string BuildDistributedNodeLabel(const NodeEndpoint &Node)
{
     if (Node.IsLocal)
     {
          return "local";
     }

     std::ostringstream Label;
     Label << Node.Host << ":" << Node.Port;

     if (!Node.Endpoint.empty())
     {
          Label << "/" << Node.Endpoint;
     }

     return Label.str();
}

static constexpr uint64_t kReplicaFreshReachableWindowMS = 15000;
static constexpr float kReplicaFreshTrustWeight = 1.0f;
static constexpr float kReplicaUnknownTrustWeight = 0.97f;
static constexpr float kReplicaStaleTrustWeight = 0.80f;

static constexpr size_t kMaxPendingReplicationRequests = 512;
static constexpr int kPeerReconnectBackoffMaxMS = 5000;
static std::mutex PendingReplicationMutex;
static std::unordered_map<std::string, std::vector<HttpRequest>> PendingReplicationRequests;

struct PersistentPeerSocket
{
     int Fd = -1;
#ifdef HLQUERY_HAS_OPENSSL
     SSL_CTX *SSLCtx = nullptr;
     SSL *SSLObj = nullptr;
#endif
     bool UseSSL = false;
     int RequestsServed = 0;
     uint64_t LastUsedMS = 0;
     bool InUse = false;
};

struct PersistentPeerPool
{
     std::vector<std::shared_ptr<PersistentPeerSocket>> Sockets;
     int ConsecutiveFailures = 0;
     std::chrono::steady_clock::time_point NextReconnectAt = std::chrono::steady_clock::time_point::min();
     uint64_t LastFailureMS = 0;
     uint64_t LastSuccessMS = 0;
     std::string LastError;
     std::mutex Mutex;
};

static std::mutex PersistentPeerSocketMutex;
static std::unordered_map<std::string, std::shared_ptr<PersistentPeerPool>> PersistentPeerSocketPool;

/* Clamps peer reconnect delays to supported bounds. */

static int ClampPeerReconnectMS(int ReconnectMS)
{
     if (ReconnectMS < 100)
     {
          return 100;
     }

     return std::min(ReconnectMS, kPeerReconnectBackoffMaxMS);
}

/* Computes the reconnect delay for a peer after failures. */

static int ComputePeerReconnectDelayMS(int BaseReconnectMS, int ConsecutiveFailures, const std::string &Key)
{
     int DelayMS = ClampPeerReconnectMS(BaseReconnectMS);
     const int ExtraFailures = std::max(0, ConsecutiveFailures - 1);

     for (int I = 0; I < ExtraFailures && DelayMS < kPeerReconnectBackoffMaxMS; ++I)
     {
          DelayMS = std::min(DelayMS * 2, kPeerReconnectBackoffMaxMS);
     }

     const int JitterWindowMS = std::max(25, std::min(250, DelayMS / 4));
     const size_t Jitter = std::hash<std::string>{}(Key + ":" + std::to_string(ConsecutiveFailures)) % static_cast<size_t>(JitterWindowMS);
     DelayMS = std::min(DelayMS + static_cast<int>(Jitter), kPeerReconnectBackoffMaxMS);

     return DelayMS;
}

/* Clears reconnect state for one peer. */

static void ResetPeerReconnectState(const std::string &Key)
{
     std::lock_guard<std::mutex> Guard(PersistentPeerSocketMutex);
     auto It = PersistentPeerSocketPool.find(Key);
     if (It == PersistentPeerSocketPool.end())
     {
          return;
     }

     std::lock_guard<std::mutex> PoolGuard(It->second->Mutex);
     It->second->ConsecutiveFailures = 0;
     It->second->NextReconnectAt = std::chrono::steady_clock::time_point::min();
     It->second->LastSuccessMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     It->second->LastError.clear();
}

/* Clears reconnect state for every peer. */

static void ResetAllPeerReconnectState()
{
     std::lock_guard<std::mutex> Guard(PersistentPeerSocketMutex);
     for (auto &Pair : PersistentPeerSocketPool)
     {
          std::lock_guard<std::mutex> PoolGuard(Pair.second->Mutex);
          Pair.second->ConsecutiveFailures = 0;
          Pair.second->NextReconnectAt = std::chrono::steady_clock::time_point::min();
          Pair.second->LastError.clear();
     }
}

/* Captures reconnect diagnostics for one peer. */

static PeerReconnectDiagnostics SnapshotPeerReconnectState(const std::string &Key)
{
     PeerReconnectDiagnostics Diagnostics;
     std::lock_guard<std::mutex> Guard(PersistentPeerSocketMutex);
     auto It = PersistentPeerSocketPool.find(Key);
     if (It == PersistentPeerSocketPool.end())
     {
          return Diagnostics;
     }

     auto NowTime = ::Now();
     std::lock_guard<std::mutex> PoolGuard(It->second->Mutex);
     Diagnostics.HasState = true;
     Diagnostics.ConsecutiveFailures = It->second->ConsecutiveFailures;
     Diagnostics.LastFailureMS = It->second->LastFailureMS;
     Diagnostics.LastSuccessMS = It->second->LastSuccessMS;
     Diagnostics.LastError = It->second->LastError;
     if (It->second->NextReconnectAt > NowTime)
     {
          Diagnostics.NextRetryMS = std::chrono::duration_cast<std::chrono::milliseconds>(It->second->NextReconnectAt - NowTime).count();
     }

     return Diagnostics;
}

/* Acquires the shared persistent socket pool for a peer. */

static std::shared_ptr<PersistentPeerPool> AcquirePersistentPeerPool(const std::string &Key, int MaxConnections)
{
     if (MaxConnections < 1)
     {
          MaxConnections = 1;
     }

     std::lock_guard<std::mutex> Guard(PersistentPeerSocketMutex);
     auto It = PersistentPeerSocketPool.find(Key);
     if (It != PersistentPeerSocketPool.end())
     {
          std::lock_guard<std::mutex> PoolGuard(It->second->Mutex);
          while (static_cast<int>(It->second->Sockets.size()) < MaxConnections)
          {
               It->second->Sockets.push_back(std::make_shared<PersistentPeerSocket>());
          }
          return It->second;
     }
     auto Pool = std::make_shared<PersistentPeerPool>();
     for (int I = 0; I < MaxConnections; ++I)
     {
          Pool->Sockets.push_back(std::make_shared<PersistentPeerSocket>());
     }
     PersistentPeerSocketPool[Key] = Pool;
     return Pool;
}

/* Closes a persistent peer socket and clears its TLS state. */

static void ClosePersistentPeerSocket(PersistentPeerSocket &Socket)
{
     if (Socket.Fd >= 0)
     {
          close(Socket.Fd);
          Socket.Fd = -1;
     }
#ifdef HLQUERY_HAS_OPENSSL
     if (Socket.SSLObj)
     {
          SSL_shutdown(Socket.SSLObj);
          SSL_free(Socket.SSLObj);
          Socket.SSLObj = nullptr;
     }
     if (Socket.SSLCtx)
     {
          SSL_CTX_free(Socket.SSLCtx);
          Socket.SSLCtx = nullptr;
     }
#endif
     Socket.UseSSL = false;
     Socket.RequestsServed = 0;
     Socket.LastUsedMS = 0;
}

enum class PeerTokenSource
{
     None,
     Cluster,
     Slave
};

struct PeerRequestResult
{
     bool Delivered = false;
     int StatusCode = 0;
     std::string Body;
     std::string Error;
     bool UsedSecondaryToken = false;
     bool UsedNoAuthFallback = false;
     int Attempts = 0;
};

/* Checks whether secondary peer token exists. */

static bool HasSecondaryPeerToken(PeerTokenSource TokenSource,
                                  const std::string &Endpoint,
                                  std::string *OutPrimary,
                                  std::string *OutSecondary)
{
     if (!Instance || !Instance->Config || Endpoint.empty())
     {
          return false;
     }

     std::string PrimaryToken;
     std::string SecondaryToken;
     bool FoundTokens = false;

     if (TokenSource == PeerTokenSource::Cluster)
     {
          FoundTokens = Instance->Config->GetClusterPeerTokens(Endpoint, &PrimaryToken, &SecondaryToken);
     }
     else if (TokenSource == PeerTokenSource::Slave)
     {
          FoundTokens = Instance->Config->GetSlavePeerTokens(Endpoint, &PrimaryToken, &SecondaryToken);
     }

     if (OutPrimary)
     {
          *OutPrimary = PrimaryToken;
     }
     if (OutSecondary)
     {
          *OutSecondary = SecondaryToken;
     }

     return FoundTokens && !SecondaryToken.empty();
}

/* Builds configured endpoints data. */

static bool BuildConfiguredEndpoints(const std::vector<std::string> &ConfiguredNodes, std::vector<NodeEndpoint> &OutNodes)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     if (ConfiguredNodes.empty())
     {
          return false;
     }

     std::vector<int> LocalPorts;
     for (const auto &Bind : Instance->Config->GetBindConfigs())
     {
          LocalPorts.push_back(Bind.port);
     }

     for (const auto &Raw : ConfiguredNodes)
     {
          std::string Host;
          int Port = 0;
          bool UseSSL = false;
          if (!ParseNodeEndpoint(Raw, Host, Port, UseSSL))
          {
               continue;
          }

          bool Local = false;

          for (int BindPort : LocalPorts)
          {
               if (Port == BindPort)
               {
                    if (IsLocalHostName(Host))
                    {
                         Local = true;
                    }
                    else
                    {
                         std::string BindAddr = Instance->Config->GetBindAddress();
                         if (!BindAddr.empty() && BindAddr != "0.0.0.0" && ToLowerCopy(BindAddr) == ToLowerCopy(Host))
                         {
                              Local = true;
                         }
                    }
               }
          }

          NodeEndpoint Endpoint;
          Endpoint.Endpoint = TrimCopy(Raw);
          Endpoint.Host = Host;
          Endpoint.Port = Port;
          Endpoint.UseSSL = UseSSL;
          Endpoint.IsLocal = Local;
          OutNodes.push_back(Endpoint);
     }

     return !OutNodes.empty();
}

/* Builds cluster endpoints data. */

static bool BuildClusterEndpoints(std::vector<NodeEndpoint> &OutNodes)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     return BuildConfiguredEndpoints(Instance->Config->GetClusterNodes(), OutNodes);
}

/* Builds slave endpoints data. */

static bool BuildSlaveEndpoints(std::vector<NodeEndpoint> &OutNodes)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     return BuildConfiguredEndpoints(Instance->Config->GetSlaveNodes(), OutNodes);
}

/* Implements the snapshot collection document ids helper. */

static std::vector<std::string> SnapshotCollectionDocumentIDs(const std::string &Collection)
{
     std::vector<std::string> IDs;

     if (!Instance || !Instance->Database || Collection.empty())
     {
          return IDs;
     }

     const std::string Pattern = "doc:" + Collection + ":*";
     const std::vector<std::string> Keys = Instance->Database->Keys(Pattern);
     IDs.reserve(Keys.size());

     for (const auto &Key : Keys)
     {
          const std::size_t LastColon = Key.find_last_of(':');
          if (LastColon == std::string::npos || LastColon + 1 >= Key.size())
          {
               continue;
          }

          IDs.push_back(Key.substr(LastColon + 1));
     }

     return IDs;
}

/* Checks whether replication replica endpoint applies. */

static bool IsReplicationReplicaEndpoint(const std::string &Endpoint)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     const auto SlaveNodes = Instance->Config->GetSlaveNodes();
     return std::find(SlaveNodes.begin(), SlaveNodes.end(), TrimCopy(Endpoint)) != SlaveNodes.end();
}

/* Implements the send HTTP request helper. */

static bool SendHttpRequest(const std::string &Host,
                            int Port,
                            const std::string &Endpoint,
                            bool UseSSL,
                            const HttpRequest &Request,
                            int TimeoutMS,
                            bool UsePersistentTransport,
                            int PersistentBurst,
                            int ConnectionsPerPeer,
                            int IdleMS,
                            bool AutoReconnect,
                            int ReconnectMS,
                            PeerTokenSource TokenSource,
                            bool UseSecondaryPeerToken,
                            int *OutStatus,
                            std::string *OutBody,
                            std::string *OutError)
{
     if (OutStatus)
     {
          *OutStatus = 0;
     }
     if (OutBody)
     {
          OutBody->clear();
     }

     auto ConfigureSocketTimeouts = [&](int Sock)
     {
          struct timeval TV;
          TV.tv_sec = TimeoutMS / 1000;
          TV.tv_usec = (TimeoutMS % 1000) * 1000;
          setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, &TV, sizeof(TV));
          setsockopt(Sock, SOL_SOCKET, SO_SNDTIMEO, &TV, sizeof(TV));
     };

     auto ConfigureSocketPerformance = [&](int Sock)
     {
          int Enabled = 1;
          setsockopt(Sock, SOL_SOCKET, SO_KEEPALIVE, &Enabled, sizeof(Enabled));
          setsockopt(Sock, IPPROTO_TCP, TCP_NODELAY, &Enabled, sizeof(Enabled));
     };

     auto OpenSocket = [&](int *OutSock) -> bool
     {
          if (!OutSock)
          {
               return false;
          }

          *OutSock = -1;
          addrinfo Hints{};
          Hints.ai_family = AF_UNSPEC;
          Hints.ai_socktype = SOCK_STREAM;
          Hints.ai_protocol = IPPROTO_TCP;

          addrinfo *Res = nullptr;
          std::string PortStr = std::to_string(Port);
          if (getaddrinfo(Host.c_str(), PortStr.c_str(), &Hints, &Res) != 0 || !Res)
          {
               if (OutError)
               {
                    *OutError = "Failed to resolve host: " + Host;
               }
               return false;
          }

          for (addrinfo *P = Res; P != nullptr; P = P->ai_next)
          {
               int Sock = socket(P->ai_family, P->ai_socktype, P->ai_protocol);
               if (Sock < 0)
               {
                    continue;
               }

               ConfigureSocketTimeouts(Sock);
               ConfigureSocketPerformance(Sock);

               if (connect(Sock, P->ai_addr, P->ai_addrlen) == 0)
               {
                    *OutSock = Sock;
                    break;
               }

               close(Sock);
          }

          freeaddrinfo(Res);

          if (*OutSock < 0)
          {
               if (OutError)
               {
                    *OutError = "Failed to connect to " + Host + ":" + PortStr;
               }
               return false;
          }

          return true;
     };

#ifdef HLQUERY_HAS_OPENSSL
     auto OpenTLSSocket = [&](int Sock, SSL_CTX **OutCtx, SSL **OutSSL) -> bool
     {
          if (OutCtx)
          {
               *OutCtx = nullptr;
          }
          if (OutSSL)
          {
               *OutSSL = nullptr;
          }

          static std::once_flag SSLInitOnce;
          std::call_once(SSLInitOnce, []()
                         {
                              SSL_library_init();
                              SSL_load_error_strings();
                              OpenSSL_add_ssl_algorithms();
                         });

          SSL_CTX *Ctx = SSL_CTX_new(TLS_client_method());
          if (!Ctx)
          {
               if (OutError)
               {
                    *OutError = "Failed to create TLS client context for " + Host + ":" + std::to_string(Port);
               }
               return false;
          }
          SSL_CTX_set_verify(Ctx, SSL_VERIFY_NONE, nullptr);

          SSL *SslObj = SSL_new(Ctx);
          if (!SslObj)
          {
               SSL_CTX_free(Ctx);
               if (OutError)
               {
                    *OutError = "Failed to create TLS session for " + Host + ":" + std::to_string(Port);
               }
               return false;
          }

          SSL_set_fd(SslObj, Sock);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
          SSL_set_tlsext_host_name(SslObj, Host.c_str());
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

          if (SSL_connect(SslObj) != 1)
          {
               SSL_free(SslObj);
               SSL_CTX_free(Ctx);
               if (OutError)
               {
                    *OutError = "TLS handshake failed for " + Host + ":" + std::to_string(Port);
               }
               return false;
          }

          if (OutCtx)
          {
               *OutCtx = Ctx;
          }
          if (OutSSL)
          {
               *OutSSL = SslObj;
          }
          return true;
     };
#endif

     auto BuildAuthHeaderValues = [&](std::string *OutApiKey, std::string *OutAuthorization)
     {
          std::string ApiKey = GetHeaderValueInsensitive(Request.Headers, "X-API-Key");
          std::string Auth = GetHeaderValueInsensitive(Request.Headers, "Authorization");

          if (Instance && Instance->Config && !Endpoint.empty())
          {
               std::string PrimaryToken;
               std::string SecondaryToken;
               bool FoundTokens = false;
               if (TokenSource == PeerTokenSource::Cluster)
               {
                    FoundTokens = Instance->Config->GetClusterPeerTokens(Endpoint, &PrimaryToken, &SecondaryToken);
               }
               else if (TokenSource == PeerTokenSource::Slave)
               {
                    FoundTokens = Instance->Config->GetSlavePeerTokens(Endpoint, &PrimaryToken, &SecondaryToken);
               }

               if (FoundTokens)
               {
                    std::string SelectedToken = UseSecondaryPeerToken ? SecondaryToken : PrimaryToken;
                    if (!SelectedToken.empty())
                    {
                         ApiKey = SelectedToken;
                         Auth = "Bearer " + SelectedToken;
                    }
               }
          }

          if (OutApiKey)
          {
               *OutApiKey = ApiKey;
          }
          if (OutAuthorization)
          {
               *OutAuthorization = Auth;
          }
     };

     auto UrlEncode = [](const std::string &Value) -> std::string
     {
          std::ostringstream Encoded;
          Encoded.fill('0');
          Encoded << std::uppercase << std::hex;
          for (unsigned char C : Value)
          {
               const bool Unreserved =
                    (C >= 'A' && C <= 'Z') ||
                    (C >= 'a' && C <= 'z') ||
                    (C >= '0' && C <= '9') ||
                    C == '-' || C == '_' || C == '.' || C == '~';
               if (Unreserved)
               {
                    Encoded << static_cast<char>(C);
               }
               else
               {
                    Encoded << '%' << std::setw(2) << static_cast<int>(C);
               }
          }
          return Encoded.str();
     };

     std::string Path = Request.Path.empty() ? "/" : Request.Path;
     if (!Request.QueryParams.empty())
     {
          std::string QueryString;
          bool First = true;
          for (const auto &Pair : Request.QueryParams)
          {
               if (!First)
               {
                    QueryString += "&";
               }
               First = false;
               QueryString += UrlEncode(Pair.first);
               QueryString += "=";
               QueryString += UrlEncode(Pair.second);
          }
          if (!QueryString.empty())
          {
               Path += "?";
               Path += QueryString;
          }
     }

     std::ostringstream Req;
     Req << Request.Method << " " << Path << " HTTP/1.1\r\n";
     Req << "Host: " << Host << ":" << Port << "\r\n";
     Req << "Connection: " << (UsePersistentTransport ? "keep-alive" : "close") << "\r\n";
     Req << "Accept: application/json\r\n";
     Req << "X-HLQ-Distributed-Hop: 1\r\n";

     auto AppendHeaderIfPresent = [&](const std::string &HeaderName)
     {
          const std::string HeaderValue = GetHeaderValueInsensitive(Request.Headers, HeaderName);
          if (!HeaderValue.empty())
          {
               Req << HeaderName << ": " << HeaderValue << "\r\n";
          }
     };

     AppendHeaderIfPresent("X-HLQ-Replication-Hop");
     AppendHeaderIfPresent("X-HLQ-Link-Ping");

     std::string ApiKey;
     std::string Auth;
     BuildAuthHeaderValues(&ApiKey, &Auth);

     if (!ApiKey.empty())
     {
          Req << "X-API-Key: " << ApiKey << "\r\n";
     }

     if (!Auth.empty())
     {
          Req << "Authorization: " << Auth << "\r\n";
     }

     auto ContentType = GetHeaderValueInsensitive(Request.Headers, "Content-Type");
     if (!Request.Body.empty())
     {
          if (ContentType.empty())
          {
               ContentType = "application/json";
          }
          Req << "Content-Type: " << ContentType << "\r\n";
          Req << "Content-Length: " << Request.Body.size() << "\r\n";
     }
     else
     {
          Req << "Content-Length: 0\r\n";
     }

     Req << "\r\n";
     if (!Request.Body.empty())
     {
          Req << Request.Body;
     }

     std::string ReqStr = Req.str();
     const std::string PoolKey = Host + ":" + std::to_string(Port);
     const bool IsReadinessProbe = Request.Method == "GET" && Request.Path == "/health";
     int Sock = -1;
#ifdef HLQUERY_HAS_OPENSSL
     SSL_CTX *ActiveSSLCtx = nullptr;
     SSL *ActiveSSLObj = nullptr;
#endif
     std::shared_ptr<PersistentPeerPool> PoolEntry;
     std::shared_ptr<PersistentPeerSocket> PoolSocket;
     bool PoolReserved = false;

     enum class PoolAcquireStatus
     {
          Success,
          Busy,
          Backoff
     };

     auto AcquirePoolSlot = [&]() -> PoolAcquireStatus
     {
          if (!UsePersistentTransport && !AutoReconnect)
          {
               return PoolAcquireStatus::Busy;
          }
          PoolEntry = AcquirePersistentPeerPool(PoolKey, UsePersistentTransport ? ConnectionsPerPeer : 1);
          auto Now = ::Now();
          const uint64_t NowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
          std::lock_guard<std::mutex> Guard(PoolEntry->Mutex);
          /*
           * Failed peer links are only reopened by health probes. Normal
           * distributed writes/searches must not use a peer that the cluster
           * currently knows as down, even after the retry timer has elapsed.
           */
          if (AutoReconnect && PoolEntry->ConsecutiveFailures > 0 && !IsReadinessProbe)
          {
               if (OutError)
               {
                    *OutError = "Peer link is down for " + PoolKey + "; waiting for health probe reconnect.";
               }
               return PoolAcquireStatus::Backoff;
          }
          if (AutoReconnect && PoolEntry->NextReconnectAt > Now && !IsReadinessProbe)
          {
               if (OutError)
               {
                    *OutError = "Peer reconnect backoff active for " + PoolKey;
               }
               return PoolAcquireStatus::Backoff;
          }

          const uint64_t IdleLimitMS = IdleMS > 0 ? static_cast<uint64_t>(IdleMS) : 30000;
          for (const auto &Socket : PoolEntry->Sockets)
          {
               if (!Socket || Socket->InUse || Socket->Fd < 0 || Socket->LastUsedMS == 0)
               {
                    continue;
               }

               if (NowMS > Socket->LastUsedMS && NowMS - Socket->LastUsedMS > IdleLimitMS)
               {
                    ClosePersistentPeerSocket(*Socket);
               }
          }

          auto PickReusableSocket = [&]() -> std::shared_ptr<PersistentPeerSocket>
          {
               for (const auto &Socket : PoolEntry->Sockets)
               {
                    if (Socket && !Socket->InUse && Socket->Fd >= 0 && Socket->UseSSL == UseSSL)
                    {
                         return Socket;
                    }
               }

               return nullptr;
          };

          auto PickEmptySocket = [&]() -> std::shared_ptr<PersistentPeerSocket>
          {
               for (const auto &Socket : PoolEntry->Sockets)
               {
                    if (Socket && !Socket->InUse && Socket->Fd < 0)
                    {
                         return Socket;
                    }
               }

               return nullptr;
          };

          /*
           * Health probes double as pool warmers. They prefer an empty slot
           * until the pool reaches configured capacity, then normal reuse wins.
           */

          PoolSocket = (IsReadinessProbe && UsePersistentTransport) ? PickEmptySocket() : PickReusableSocket();

          if (!PoolSocket)
          {
               PoolSocket = PickReusableSocket();
          }
          if (!PoolSocket)
          {
               PoolSocket = PickEmptySocket();
          }
          if (!PoolSocket)
          {
               return PoolAcquireStatus::Busy;
          }

          PoolSocket->InUse = true;
          PoolReserved = true;
          if (PoolSocket->Fd >= 0 && PoolSocket->UseSSL == UseSSL)
          {
               Sock = PoolSocket->Fd;
#ifdef HLQUERY_HAS_OPENSSL
               ActiveSSLCtx = PoolSocket->SSLCtx;
               ActiveSSLObj = PoolSocket->SSLObj;
#endif
               return PoolAcquireStatus::Success;
          }
          if (PoolSocket->Fd >= 0)
          {
               ClosePersistentPeerSocket(*PoolSocket);
          }
          return PoolAcquireStatus::Success;
     };

     auto AbortPoolSlot = [&]()
     {
          if (!PoolReserved || !PoolEntry)
          {
               return;
          }

          std::lock_guard<std::mutex> Guard(PoolEntry->Mutex);
          PoolEntry->ConsecutiveFailures++;
          PoolEntry->LastFailureMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
          PoolEntry->LastError = OutError ? *OutError : std::string("Peer request failed");

          if (ReconnectMS > 0)
          {
               PoolEntry->NextReconnectAt = ::Now() + std::chrono::milliseconds(ComputePeerReconnectDelayMS(ReconnectMS, PoolEntry->ConsecutiveFailures, PoolKey));
          }

          const bool ClosedActivePoolFD = PoolSocket && PoolSocket->Fd >= 0 && PoolSocket->Fd == Sock;
          if (PoolSocket)
          {
               ClosePersistentPeerSocket(*PoolSocket);
               PoolSocket->InUse = false;
          }
          if (ClosedActivePoolFD)
          {
               Sock = -1;
#ifdef HLQUERY_HAS_OPENSSL
               ActiveSSLCtx = nullptr;
               ActiveSSLObj = nullptr;
#endif
          }
          PoolSocket.reset();
          PoolEntry.reset();
          PoolReserved = false;
     };

     auto FinalizePoolSlot = [&](bool KeepAlive)
     {
          if (!PoolReserved || !PoolEntry)
          {
               return;
          }
          std::lock_guard<std::mutex> Guard(PoolEntry->Mutex);

          if (!KeepAlive)
          {
               if (PoolSocket)
               {
                    ClosePersistentPeerSocket(*PoolSocket);
                    PoolSocket->InUse = false;
               }
               PoolEntry->ConsecutiveFailures = 0;
               PoolEntry->NextReconnectAt = std::chrono::steady_clock::time_point::min();
               PoolEntry->LastSuccessMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
               PoolEntry->LastError.clear();
               PoolSocket.reset();
               PoolEntry.reset();
               PoolReserved = false;
               return;
          }
          PoolEntry->ConsecutiveFailures = 0;
          PoolEntry->NextReconnectAt = std::chrono::steady_clock::time_point::min();
          PoolEntry->LastSuccessMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
          PoolEntry->LastError.clear();
          if (!PoolSocket)
          {
               PoolEntry.reset();
               PoolReserved = false;
               return;
          }
          PoolSocket->RequestsServed++;
          PoolSocket->Fd = Sock;
          PoolSocket->UseSSL = UseSSL;
          PoolSocket->LastUsedMS = PoolEntry->LastSuccessMS;
#ifdef HLQUERY_HAS_OPENSSL
          PoolSocket->SSLCtx = ActiveSSLCtx;
          PoolSocket->SSLObj = ActiveSSLObj;
#endif
          if (PersistentBurst < 1)
          {
               PersistentBurst = 1;
          }

          if (PoolSocket->RequestsServed >= PersistentBurst)
          {
               ClosePersistentPeerSocket(*PoolSocket);
          }
          PoolSocket->InUse = false;
          PoolSocket.reset();
          PoolEntry.reset();
          PoolReserved = false;
     };

     auto CloseEphemeralConnection = [&]()
     {
#ifdef HLQUERY_HAS_OPENSSL
          if (UseSSL && ActiveSSLObj)
          {
               SSL_shutdown(ActiveSSLObj);
               SSL_free(ActiveSSLObj);
               ActiveSSLObj = nullptr;
          }

          if (UseSSL && ActiveSSLCtx)
          {
               SSL_CTX_free(ActiveSSLCtx);
               ActiveSSLCtx = nullptr;
          }
#endif
          if (Sock >= 0)
          {
               close(Sock);
               Sock = -1;
          }
     };

     auto AcquireResult = AcquirePoolSlot();
     if (AcquireResult == PoolAcquireStatus::Backoff)
     {
          return false;
     }

     if (Sock < 0)
     {
          int Attempts = (AutoReconnect ? 2 : 1);
          bool Connected = false;

          for (int Attempt = 0; Attempt < Attempts; ++Attempt)
          {
               if (OpenSocket(&Sock))
               {
#ifdef HLQUERY_HAS_OPENSSL
                    if (UseSSL && !OpenTLSSocket(Sock, &ActiveSSLCtx, &ActiveSSLObj))
                    {
                         close(Sock);
                         Sock = -1;
                         continue;
                    }
#else
                    if (UseSSL)
                    {
                         close(Sock);
                         Sock = -1;
                         continue;
                    }
#endif
                    Connected = true;
                    break;
               }

               if (Attempt + 1 < Attempts && ReconnectMS > 0)
               {
                    usleep(static_cast<useconds_t>(ReconnectMS) * 1000);
               }
          }

          if (!Connected)
          {
               AbortPoolSlot();
               return false;
          }
     }

     auto SendAll = [&](int FD) -> bool
     {
          ssize_t Sent = 0;
          while (Sent < static_cast<ssize_t>(ReqStr.size()))
          {
#ifdef HLQUERY_HAS_OPENSSL
               ssize_t WriteCount = UseSSL && ActiveSSLObj
                                         ? SSL_write(ActiveSSLObj, ReqStr.data() + Sent, static_cast<int>(ReqStr.size() - static_cast<size_t>(Sent)))
                                         : send(FD, ReqStr.data() + Sent, ReqStr.size() - static_cast<size_t>(Sent), 0);
#else
               ssize_t WriteCount = send(FD, ReqStr.data() + Sent, ReqStr.size() - static_cast<size_t>(Sent), 0);
#endif
               if (WriteCount <= 0)
               {
                    return false;
               }
               Sent += WriteCount;
          }
          return true;
     };

     if (!SendAll(Sock))
     {
          if (OutError)
          {
               *OutError = "Failed to send request to " + Host + ":" + std::to_string(Port);
          }
          AbortPoolSlot();
          CloseEphemeralConnection();
          return false;
     }

     auto ReceiveHttpResponse = [&](int FD, int *StatusCodeOut, std::string *BodyOut, bool *KeepAliveOut) -> bool
     {
          if (StatusCodeOut)
          {
               *StatusCodeOut = 0;
          }
          if (BodyOut)
          {
               BodyOut->clear();
          }
          if (KeepAliveOut)
          {
               *KeepAliveOut = false;
          }

          std::string RawResponse;
          RawResponse.reserve(4096);

          bool HeadersParsed = false;
          size_t HeaderEnd = std::string::npos;
          size_t ExpectedBodyLength = 0;
          bool HasContentLength = false;

          while (true)
          {
               if (HeadersParsed)
               {
                    size_t CurrentBodyLength = RawResponse.size() - (HeaderEnd + 4);
                    if (HasContentLength && CurrentBodyLength >= ExpectedBodyLength)
                    {
                         break;
                    }
               }

               char Buffer[4096];
#ifdef HLQUERY_HAS_OPENSSL
               ssize_t ReadCount = UseSSL && ActiveSSLObj
                                        ? SSL_read(ActiveSSLObj, Buffer, static_cast<int>(sizeof(Buffer)))
                                        : recv(FD, Buffer, sizeof(Buffer), 0);
#else
               ssize_t ReadCount = recv(FD, Buffer, sizeof(Buffer), 0);
#endif
               if (ReadCount <= 0)
               {
                    break;
               }
               RawResponse.append(Buffer, static_cast<size_t>(ReadCount));

               if (!HeadersParsed)
               {
                    HeaderEnd = RawResponse.find("\r\n\r\n");
                    if (HeaderEnd != std::string::npos)
                    {
                         HeadersParsed = true;
                         std::string HeaderStr = RawResponse.substr(0, HeaderEnd);
                         std::istringstream HeaderStream(HeaderStr);
                         std::string StatusLine;
                         std::getline(HeaderStream, StatusLine);
                         int ParsedStatusCode = 0;
                         if (!StatusLine.empty())
                         {
                              std::istringstream StatusSS(StatusLine);
                              std::string HttpVersion;
                              StatusSS >> HttpVersion >> ParsedStatusCode;
                         }
                         if (StatusCodeOut)
                         {
                              *StatusCodeOut = ParsedStatusCode;
                         }

                         std::string HeaderLine;
                         bool ServerKeepAlive = true;
                         while (std::getline(HeaderStream, HeaderLine))
                         {
                              if (!HeaderLine.empty() && HeaderLine.back() == '\r')
                              {
                                   HeaderLine.pop_back();
                              }

                              size_t ColonPos = HeaderLine.find(':');
                              if (ColonPos == std::string::npos)
                              {
                                   continue;
                              }
                              std::string Name = ToLowerCopy(TrimCopy(HeaderLine.substr(0, ColonPos)));
                              std::string Value = TrimCopy(HeaderLine.substr(ColonPos + 1));
                              if (Name == "content-length")
                              {
                                   try
                                   {
                                        long long ParsedLength = std::stoll(Value);
                                        if (ParsedLength >= 0)
                                        {
                                             ExpectedBodyLength = static_cast<size_t>(ParsedLength);
                                             HasContentLength = true;
                                        }
                                   }
                                   catch (...)
                                   {
                                        HasContentLength = false;
                                   }
                              }
                              else if (Name == "connection" && ToLowerCopy(Value) == "close")
                              {
                                   ServerKeepAlive = false;
                              }
                         }

                         if (KeepAliveOut)
                         {
                              *KeepAliveOut = ServerKeepAlive;
                         }

                         if (HasContentLength)
                         {
                              size_t CurrentBodyLength = RawResponse.size() - (HeaderEnd + 4);
                              if (CurrentBodyLength >= ExpectedBodyLength)
                              {
                                   break;
                              }
                         }
                    }
               }
          }

          if (!HeadersParsed || HeaderEnd == std::string::npos)
          {
               if (OutError)
               {
                    *OutError = "Invalid HTTP response from " + Host + ":" + std::to_string(Port);
               }
               return false;
          }

          size_t BodyOffset = HeaderEnd + 4;
          size_t AvailableBody = (RawResponse.size() >= BodyOffset) ? (RawResponse.size() - BodyOffset) : 0;
          std::string BodyStr;
          if (HasContentLength)
          {
               if (AvailableBody < ExpectedBodyLength)
               {
                    if (OutError)
                    {
                         *OutError = "Incomplete HTTP response body from " + Host + ":" + std::to_string(Port);
                    }
                    return false;
               }
               BodyStr = RawResponse.substr(BodyOffset, ExpectedBodyLength);
          }
          else
          {
               BodyStr = RawResponse.substr(BodyOffset);
          }

          if (BodyOut)
          {
               *BodyOut = BodyStr;
          }
          return true;
     };

     int StatusCode = 0;
     std::string BodyStr;
     bool ServerKeepAlive = false;
     bool ReadOK = ReceiveHttpResponse(Sock, &StatusCode, &BodyStr, &ServerKeepAlive);
     if (!ReadOK)
     {
          AbortPoolSlot();
          CloseEphemeralConnection();
          return false;
     }

     if (OutStatus)
     {
          *OutStatus = StatusCode;
     }
     if (OutBody)
     {
          *OutBody = BodyStr;
     }

     bool KeepAlive = UsePersistentTransport && ServerKeepAlive;
     if (PoolReserved)
     {
          FinalizePoolSlot(KeepAlive);
     }
     else
     {
          CloseEphemeralConnection();
     }

     return true;
}

/* Implements the execute peer request with fallback helper. */

static bool ExecutePeerRequestWithFallback(const std::string &Host,
                                           int Port,
                                           const std::string &Endpoint,
                                           bool UseSSL,
                                           const HttpRequest &Request,
                                           int TimeoutMS,
                                           bool UsePersistentTransport,
                                           int PersistentBurst,
                                           int ConnectionsPerPeer,
                                           int IdleMS,
                                           bool AutoReconnect,
                                           int ReconnectMS,
                                           PeerTokenSource TokenSource,
                                           PeerRequestResult *OutResult)
{
     if (!OutResult)
     {
          return false;
     }

     *OutResult = PeerRequestResult();

     auto IssueAttempt = [&](const std::string &EndpointValue,
                             const HttpRequest &RequestValue,
                             PeerTokenSource AttemptTokenSource,
                             bool UseSecondaryToken) -> bool
     {
          OutResult->Attempts++;
          std::string Body;
          std::string Error;
          int StatusCode = 0;
          if (!SendHttpRequest(Host,
                               Port,
                               EndpointValue,
                               UseSSL,
                               RequestValue,
                               TimeoutMS,
                               UsePersistentTransport,
                               PersistentBurst,
                               ConnectionsPerPeer,
                               IdleMS,
                               AutoReconnect,
                               ReconnectMS,
                               AttemptTokenSource,
                               UseSecondaryToken,
                               &StatusCode,
                               &Body,
                               &Error))
          {
               OutResult->Error = Error;
               return false;
          }

          OutResult->Delivered = true;
          OutResult->StatusCode = StatusCode;
          OutResult->Body = std::move(Body);
          OutResult->Error.clear();
          return true;
     };

     if (!IssueAttempt(Endpoint, Request, TokenSource, false))
     {
          return false;
     }

     if ((OutResult->StatusCode == 401 || OutResult->StatusCode == 403) && TokenSource != PeerTokenSource::None)
     {
          std::string PrimaryToken;
          std::string SecondaryToken;
          if (HasSecondaryPeerToken(TokenSource, Endpoint, &PrimaryToken, &SecondaryToken))
          {
               if (IssueAttempt(Endpoint, Request, TokenSource, true))
               {
                    OutResult->UsedSecondaryToken = true;
               }
          }
     }

     if (OutResult->StatusCode == 403)
     {
          const std::string LowerBody = ToLowerCopy(OutResult->Body);
          if (LowerBody.find("authentication is disabled") != std::string::npos ||
              LowerBody.find("tokens are not accepted") != std::string::npos)
          {
               HttpRequest NoAuthRequest = Request;
               NoAuthRequest.Headers.erase("Authorization");
               NoAuthRequest.Headers.erase("authorization");
               NoAuthRequest.Headers.erase("X-API-Key");
               NoAuthRequest.Headers.erase("x-api-key");

               if (IssueAttempt("", NoAuthRequest, PeerTokenSource::None, false))
               {
                    OutResult->UsedNoAuthFallback = true;
               }
          }
     }

     return true;
}

/* Implements the serialize collection config for replication helper. */

static std::string SerializeCollectionConfigForReplication(const CollectionConfig &Config)
{
     nlohmann::json Body;
     Body["name"] = Config.Name;
     Body["fields"] = nlohmann::json::array();

     for (const auto &Field : Config.Fields)
     {
          nlohmann::json Entry;
          Entry["name"] = Field.first;
          Entry["type"] = Field.second.empty() ? "string" : Field.second;
          Body["fields"].push_back(Entry);
     }

     for (const auto &Meta : Config.Metadata)
     {
          Body[Meta.first] = Meta.second;
     }

     return Body.dump();
}

/* Implements the serialize document batch for replication helper. */

static std::string SerializeDocumentBatchForReplication(const std::vector<Document> &Docs)
{
     nlohmann::json Body;
     Body["documents"] = nlohmann::json::array();

     for (const auto &Doc : Docs)
     {
          nlohmann::json Entry;
          Entry["id"] = Doc.ID;
          Entry["title"] = Doc.Title;
          Entry["content"] = Doc.Content;
          Entry["timestamp"] = Doc.Timestamp;

          for (const auto &Field : Doc.Fields)
          {
               Entry[Field.first] = Field.second;
          }

          Body["documents"].push_back(Entry);
     }

     return Body.dump();
}

/* Implements the score for hit helper. */

static float ScoreForHit(const SearchHit &Hit)
{
     if (Hit.HybridScore > 0.0f)
     {
          return Hit.HybridScore;
     }
     if (Hit.VectorScore > 0.0f)
     {
          return Hit.VectorScore;
     }
     return Hit.TextMatch;
}

/* Implements the merge facet counts helper. */

static void MergeFacetCounts(std::map<std::string, std::map<std::string, int>> &FacetCounts,
                             const nlohmann::json &FacetsJSON)
{
     if (!FacetsJSON.is_object())
     {
          return;
     }

     for (auto It = FacetsJSON.begin(); It != FacetsJSON.end(); ++It)
     {
          const std::string FacetName = It.key();
          if (!It.value().is_array())
          {
               continue;
          }

          for (const auto &Entry : It.value())
          {
               if (!Entry.is_object())
               {
                    continue;
               }
               std::string Value = Entry.value("value", "");
               int Count = Entry.value("count", 0);
               if (!Value.empty() && Count > 0)
               {
                    FacetCounts[FacetName][Value] += Count;
               }
          }
     }
}

/* Implements the append hits from JSON helper. */

static void AppendHitsFromJSON(const nlohmann::json &JSONObj,
                               std::vector<SearchHit> &OutHits,
                               const std::string &SourceNode = "",
                               const std::string &SourceReplicationState = "")
{
     if (!JSONObj.contains("hits") || !JSONObj["hits"].is_array())
     {
          return;
     }

     for (const auto &HitVal : JSONObj["hits"])
     {
          if (!HitVal.is_object())
          {
               continue;
          }

          SearchHit Hit;

          if (HitVal.contains("document") && HitVal["document"].is_object())
          {
               for (auto DocIt = HitVal["document"].begin(); DocIt != HitVal["document"].end(); ++DocIt)
               {
                    if (DocIt.value().is_string())
                    {
                         Hit.Document[DocIt.key()] = DocIt.value().get<std::string>();
                    }
                    else
                    {
                         Hit.Document[DocIt.key()] = DocIt.value().dump();
                    }
               }
          }

          if (!SourceNode.empty())
          {
               Hit.Document["_source_node"] = SourceNode;
          }

          if (!SourceReplicationState.empty())
          {
               Hit.Document["_source_replication_state"] = SourceReplicationState;
          }

          Hit.TextMatch = HitVal.value("text_match", HitVal.value("_text_match", 0.0f));
          Hit.VectorScore = HitVal.value("vector_score", 0.0f);
          Hit.HybridScore = HitVal.value("hybrid_score", 0.0f);

          if (HitVal.contains("highlights") && HitVal["highlights"].is_object())
          {
               for (auto HIt = HitVal["highlights"].begin(); HIt != HitVal["highlights"].end(); ++HIt)
               {
                    if (HIt.value().is_string())
                    {
                         Hit.Highlights[HIt.key()] = HIt.value().get<std::string>();
                    }
                    else
                    {
                         Hit.Highlights[HIt.key()] = HIt.value().dump();
                    }
               }
          }

          OutHits.push_back(std::move(Hit));
     }
}
bool SearchAPI::IsStrictDistributedMode() const
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     return Instance->Config->GetDistributedSearchMode() == "strict_remote";
}

bool SearchAPI::ShouldAttemptDistributedSearch(const HttpRequest &Request) const
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     if (!Instance->Config->GetDistributedSearchEnabled())
     {
          return false;
     }

     if (Instance->Config->GetClusterNodes().empty())
     {
          return false;
     }

     const std::string Mode = Instance->Config->GetDistributedSearchMode();
     if (Mode == "disabled")
     {
          return false;
     }

     /* Avoid accidental forward loops if/when remote fanout is implemented. */

     const std::string HopHeader = ToLowerCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Distributed-Hop"));
     if (HopHeader == "1" || HopHeader == "true")
     {
          return false;
     }

     /* Per-request override for integration testing and rollout control. */

     auto OverrideIt = Request.QueryParams.find("distributed");
     if (OverrideIt != Request.QueryParams.end())
     {
          const std::string Override = ToLowerCopy(OverrideIt->second);
          if (Override == "off" || Override == "false" || Override == "0" || Override == "local")
          {
               return false;
          }
          if (Override == "on" || Override == "true" || Override == "1" || Override == "force" || Override == "remote")
          {
               return true;
          }
     }

     return (Mode == "local_first" || Mode == "remote_only" || Mode == "strict_remote");
}

bool SearchAPI::GetReplicationNodeState(const std::string &Endpoint,
                                        uint64_t *OutLastReachableMS,
                                        uint64_t *OutLastResyncMS,
                                        bool *OutDirty,
                                        bool *OutResyncInProgress) const
{
     if (OutLastReachableMS)
     {
          *OutLastReachableMS = 0;
     }
     if (OutLastResyncMS)
     {
          *OutLastResyncMS = 0;
     }
     if (OutDirty)
     {
          *OutDirty = false;
     }
     if (OutResyncInProgress)
     {
          *OutResyncInProgress = false;
     }

     std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);

     bool HasState = false;

     auto ReachableIt = ReplicationLastReachableTimestampMS.find(Endpoint);
     if (ReachableIt != ReplicationLastReachableTimestampMS.end())
     {
          if (OutLastReachableMS)
          {
               *OutLastReachableMS = ReachableIt->second;
          }
          HasState = true;
     }

     auto ResyncIt = ReplicationLastResyncTimestampMS.find(Endpoint);
     if (ResyncIt != ReplicationLastResyncTimestampMS.end())
     {
          if (OutLastResyncMS)
          {
               *OutLastResyncMS = ResyncIt->second;
          }
          HasState = true;
     }

     auto DirtyIt = ReplicationDirtySlaves.find(Endpoint);
     if (DirtyIt != ReplicationDirtySlaves.end())
     {
          if (OutDirty)
          {
               *OutDirty = true;
          }
          HasState = true;
     }

     auto ResyncInProgressIt = ReplicationResyncInProgress.find(Endpoint);
     if (ResyncInProgressIt != ReplicationResyncInProgress.end())
     {
          if (OutResyncInProgress)
          {
               *OutResyncInProgress = true;
          }
          HasState = true;
     }

     return HasState;
}

/* Implements the try distributed search helper. */

bool SearchAPI::TryDistributedSearch(const HttpRequest &Request,
                                     const std::string &Collection,
                                     const ComprehensiveSearchQuery &Query,
                                     ComprehensiveSearchResult *OutResult,
                                     std::string *OutError)
{
     if (!ShouldAttemptDistributedSearch(Request))
     {
          return false;
     }

     if (!OutResult)
     {
          if (OutError)
          {
               *OutError = "Invalid distributed search state.";
          }
          return false;
     }

     std::vector<NodeEndpoint> Nodes;
     if (!BuildClusterEndpoints(Nodes))
     {
          if (OutError)
          {
               *OutError = "No cluster nodes configured.";
          }
          return false;
     }

     Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
                                [](const NodeEndpoint &Node)
                                {
                                     return !Node.IsLocal && IsReplicationReplicaEndpoint(Node.Endpoint);
                                }),
                 Nodes.end());

     bool RouteSpecified = false;
     bool RouteIsLocal = false;
     std::string RoutedHost;
     int RoutedPort = 0;
     auto RouteIt = Request.QueryParams.find("route");
     if (RouteIt != Request.QueryParams.end() && !TrimCopy(RouteIt->second).empty())
     {
          RouteSpecified = true;
          if (!ResolveDistributedRoute(RouteIt->second, &RoutedHost, &RoutedPort, &RouteIsLocal))
          {
               if (OutError)
               {
                    *OutError = "Invalid distributed route target.";
               }
               return false;
          }

          if (RouteIsLocal)
          {
               Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
                                          [](const NodeEndpoint &Node)
                                          {
                                               return !Node.IsLocal;
                                          }),
                           Nodes.end());
          }
          else
          {
               Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
                                          [&](const NodeEndpoint &Node)
                                          {
                                               return Node.IsLocal || ToLowerCopy(Node.Host) != ToLowerCopy(RoutedHost) || Node.Port != RoutedPort;
                                          }),
                           Nodes.end());
          }
     }

     if (Nodes.empty())
     {
          if (RouteSpecified && RouteIsLocal)
          {
               /* Route-to-local is valid even when self is not listed in links.conf. */
          }
          else if (OutError)
          {
               *OutError = "No distributed search nodes available after excluding replication replicas.";
          }
          if (!RouteSpecified || !RouteIsLocal)
          {
               return false;
          }
     }

     struct NodeRoutingState
     {
          NodeEndpoint Node;
          bool HasReplicationState = false;
          bool Dirty = false;
          bool ResyncInProgress = false;
          uint64_t LastReachableMS = 0;
          uint64_t LastResyncMS = 0;
          bool Stale = false;
          float TrustWeight = kReplicaUnknownTrustWeight;
          int RoutingPriority = 2;
     };

     const uint64_t NowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : 0;
     std::vector<NodeRoutingState> RoutedNodes;
     RoutedNodes.reserve(Nodes.size());
     for (const auto &Node : Nodes)
     {
          NodeRoutingState State;
          State.Node = Node;
          if (Node.IsLocal)
          {
               State.TrustWeight = kReplicaFreshTrustWeight;
               State.RoutingPriority = 0;
          }
          else
          {
               State.HasReplicationState = GetReplicationNodeState(Node.Endpoint,
                                                                   &State.LastReachableMS,
                                                                   &State.LastResyncMS,
                                                                   &State.Dirty,
                                                                   &State.ResyncInProgress);
               const bool ReachableFresh = (State.LastReachableMS > 0 &&
                                            NowMS >= State.LastReachableMS &&
                                            (NowMS - State.LastReachableMS) <= kReplicaFreshReachableWindowMS);
               State.Stale = State.Dirty || State.ResyncInProgress || (State.HasReplicationState && !ReachableFresh);
               if (State.Stale)
               {
                    State.TrustWeight = kReplicaStaleTrustWeight;
                    State.RoutingPriority = 3;
               }
               else if (State.HasReplicationState)
               {
                    State.TrustWeight = kReplicaFreshTrustWeight;
                    State.RoutingPriority = 1;
               }
          }

          RoutedNodes.push_back(std::move(State));
     }

     std::stable_sort(RoutedNodes.begin(), RoutedNodes.end(),
                      [&](const NodeRoutingState &A, const NodeRoutingState &B)
                      {
                           if (A.RoutingPriority != B.RoutingPriority)
                           {
                                return A.RoutingPriority < B.RoutingPriority;
                           }
                           if (A.LastReachableMS != B.LastReachableMS)
                           {
                                return A.LastReachableMS > B.LastReachableMS;
                           }
                           if (A.LastResyncMS != B.LastResyncMS)
                           {
                                return A.LastResyncMS > B.LastResyncMS;
                           }
                           return BuildDistributedNodeLabel(A.Node) < BuildDistributedNodeLabel(B.Node);
                      });

     const std::string Mode = Instance && Instance->Config ? Instance->Config->GetDistributedSearchMode() : "disabled";
     const bool PreferLocal = Instance && Instance->Config ? Instance->Config->GetDistributedSearchPreferLocal() : true;
     const int TimeoutMS = Instance && Instance->Config ? Instance->Config->GetDistributedSearchTimeoutMS() : 2000;
     const bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
     const int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
     const bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
     const int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;
     const int Page = Query.Page < 1 ? 1 : Query.Page;
     const int PerPage = Query.PerPage < 1 ? 100 : Query.PerPage;
     const std::size_t PageSize = static_cast<std::size_t>(Page);
     const std::size_t PerPageSize = static_cast<std::size_t>(PerPage);
     const std::size_t RequestedStart = Query.Offset > 0
                                             ? static_cast<std::size_t>(Query.Offset)
                                             : ((PageSize - 1) > (std::numeric_limits<std::size_t>::max() / PerPageSize)
                                                     ? std::numeric_limits<std::size_t>::max()
                                                     : (PageSize - 1) * PerPageSize);
     const std::size_t FanoutPerPageSize = RequestedStart > (std::numeric_limits<std::size_t>::max() - PerPageSize)
                                                ? std::numeric_limits<std::size_t>::max()
                                                : std::max(PerPageSize, RequestedStart + PerPageSize);
     const int FanoutPerPage = (FanoutPerPageSize > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                                    ? std::numeric_limits<int>::max()
                                    : static_cast<int>(FanoutPerPageSize);
     ComprehensiveSearchQuery FanoutQuery = Query;
     FanoutQuery.Page = 1;
     FanoutQuery.PerPage = FanoutPerPage;
     FanoutQuery.Offset = 0;

     std::vector<SearchHit> AggregatedHits;
     std::map<std::string, std::map<std::string, int>> FacetCounts;
     std::vector<std::map<std::string, std::string>> NodeDiagnostics;
     int FoundTotal = 0;
     int OutOfTotal = 0;
     bool IndexingInProgress = false;
     bool PartialResults = false;
     float MaxSearchTime = 0.0f;

     auto AddLocal = [&](const ComprehensiveSearchResult &LocalRes)
     {
          std::map<std::string, std::string> Diagnostic;
          Diagnostic["node"] = "local";
          Diagnostic["status"] = LocalRes.Error.empty() ? "ok" : "error";
          Diagnostic["merge_reason"] = "local_result";
          Diagnostic["replication_state"] = "local";
          Diagnostic["replication_trust_weight"] = std::to_string(kReplicaFreshTrustWeight);
          Diagnostic["hits_added"] = std::to_string(LocalRes.Hits.size());
          Diagnostic["found"] = std::to_string(LocalRes.Found);
          Diagnostic["out_of"] = std::to_string(LocalRes.OutOf);
          Diagnostic["search_time_ms"] = std::to_string(LocalRes.SearchTimeMS);
          if (!LocalRes.Error.empty())
          {
               Diagnostic["error"] = LocalRes.Error;
          }
          NodeDiagnostics.push_back(std::move(Diagnostic));

          for (auto Hit : LocalRes.Hits)
          {
               Hit.Document["_source_node"] = "local";
               Hit.Document["_source_replication_state"] = "local";
               AggregatedHits.push_back(std::move(Hit));
          }
          FoundTotal += LocalRes.Found;
          OutOfTotal += LocalRes.OutOf;
          IndexingInProgress = IndexingInProgress || LocalRes.IndexingInProgress;
          PartialResults = PartialResults || LocalRes.PartialResults;
          MaxSearchTime = std::max(MaxSearchTime, LocalRes.SearchTimeMS);
          for (const auto &FacetPair : LocalRes.Facets)
          {
               for (const auto &Count : FacetPair.second.Counts)
               {
                    if (!Count.Value.empty() && Count.Count > 0)
                    {
                         FacetCounts[FacetPair.first][Count.Value] += Count.Count;
                    }
               }
          }
     };

     const bool AllowLocal = RouteSpecified ? RouteIsLocal : (Mode == "local_first" || (Mode == "remote_only" ? false : PreferLocal));

     if (AllowLocal)
     {
          ComprehensiveSearchResult LocalRes = PerformComprehensiveSearch(Collection, FanoutQuery);
          AddLocal(LocalRes);
     }

     int RemoteResponses = 0;

     for (const auto &NodeState : RoutedNodes)
     {
          const NodeEndpoint &Node = NodeState.Node;
          if (Node.IsLocal)
          {
               if (Mode == "remote_only" || Mode == "strict_remote")
               {
                    continue;
               }
               if (!AllowLocal)
               {
                    ComprehensiveSearchResult LocalRes = PerformComprehensiveSearch(Collection, FanoutQuery);
                    AddLocal(LocalRes);
               }
               continue;
          }

          const auto RequestStart = ::Now();
          HttpRequest FanoutRequest = Request;
          FanoutRequest.QueryParams.erase("offset");
          FanoutRequest.QueryParams.erase("from");
          FanoutRequest.QueryParams.erase("limit");
          FanoutRequest.QueryParams.erase("size");
          FanoutRequest.QueryParams["page"] = "1";
          FanoutRequest.QueryParams["per_page"] = std::to_string(FanoutPerPage);

          if (FanoutRequest.Method == "POST" && !FanoutRequest.Body.empty())
          {
               try
               {
                    nlohmann::json FanoutBody = nlohmann::json::parse(FanoutRequest.Body);
                    if (FanoutBody.is_object())
                    {
                         FanoutBody.erase("offset");
                         FanoutBody.erase("from");
                         FanoutBody.erase("limit");
                         FanoutBody.erase("size");
                         FanoutBody["page"] = 1;
                         FanoutBody["per_page"] = FanoutPerPage;
                         FanoutRequest.Body = FanoutBody.dump();
                    }
               }
               catch (...)
               {
                    /* The caller already validated the body; keep it unchanged on failure. */
               }
          }

          PeerRequestResult PeerResult;
          int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
          int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
          if (!ExecutePeerRequestWithFallback(Node.Host, Node.Port, Node.Endpoint, Node.UseSSL, FanoutRequest, TimeoutMS, UsePersistentTransport, PersistentBurst, ConnectionsPerPeer, IdleMS, AutoReconnect, ReconnectMS, PeerTokenSource::Cluster, &PeerResult))
          {
               std::map<std::string, std::string> Diagnostic;
               Diagnostic["node"] = BuildDistributedNodeLabel(Node);
               Diagnostic["status"] = "transport_error";
               Diagnostic["merge_reason"] = "request_failed";
               Diagnostic["latency_ms"] = DurationToMillisecondsString(::Now() - RequestStart);
               Diagnostic["retry_policy"] = "standard_peer_fallback";
               Diagnostic["attempts"] = std::to_string(PeerResult.Attempts);
               Diagnostic["used_secondary_token"] = PeerResult.UsedSecondaryToken ? "true" : "false";
               Diagnostic["used_no_auth_fallback"] = PeerResult.UsedNoAuthFallback ? "true" : "false";
               Diagnostic["replication_state"] = NodeState.Stale ? "stale" : (NodeState.HasReplicationState ? "fresh" : "unknown");
               Diagnostic["replication_trust_weight"] = std::to_string(NodeState.TrustWeight);
               Diagnostic["replication_dirty"] = NodeState.Dirty ? "true" : "false";
               Diagnostic["replication_resync_in_progress"] = NodeState.ResyncInProgress ? "true" : "false";
               Diagnostic["replication_last_reachable_ms"] = std::to_string(NodeState.LastReachableMS);
               Diagnostic["replication_last_resync_ms"] = std::to_string(NodeState.LastResyncMS);
               if (!PeerResult.Error.empty())
               {
                    Diagnostic["error"] = PeerResult.Error;
               }
               NodeDiagnostics.push_back(std::move(Diagnostic));

               if (OutError && OutError->empty())
               {
                    *OutError = PeerResult.Error;
               }
               continue;
          }

          if (PeerResult.StatusCode < 200 || PeerResult.StatusCode >= 300)
          {
               std::map<std::string, std::string> Diagnostic;
               Diagnostic["node"] = BuildDistributedNodeLabel(Node);
               Diagnostic["status"] = "http_error";
               Diagnostic["merge_reason"] = "status_rejected";
               Diagnostic["latency_ms"] = DurationToMillisecondsString(::Now() - RequestStart);
               Diagnostic["status_code"] = std::to_string(PeerResult.StatusCode);
               Diagnostic["retry_policy"] = "standard_peer_fallback";
               Diagnostic["attempts"] = std::to_string(PeerResult.Attempts);
               Diagnostic["used_secondary_token"] = PeerResult.UsedSecondaryToken ? "true" : "false";
               Diagnostic["used_no_auth_fallback"] = PeerResult.UsedNoAuthFallback ? "true" : "false";
               Diagnostic["replication_state"] = NodeState.Stale ? "stale" : (NodeState.HasReplicationState ? "fresh" : "unknown");
               Diagnostic["replication_trust_weight"] = std::to_string(NodeState.TrustWeight);
               Diagnostic["replication_dirty"] = NodeState.Dirty ? "true" : "false";
               Diagnostic["replication_resync_in_progress"] = NodeState.ResyncInProgress ? "true" : "false";
               Diagnostic["replication_last_reachable_ms"] = std::to_string(NodeState.LastReachableMS);
               Diagnostic["replication_last_resync_ms"] = std::to_string(NodeState.LastResyncMS);
               NodeDiagnostics.push_back(std::move(Diagnostic));

               if (OutError && OutError->empty())
               {
                    *OutError = "Remote node returned status " + std::to_string(PeerResult.StatusCode);
               }
               continue;
          }

          nlohmann::json ResponseJSON;
          try
          {
               ResponseJSON = nlohmann::json::parse(PeerResult.Body);
          }
          catch (...)
          {
               std::map<std::string, std::string> Diagnostic;
               Diagnostic["node"] = BuildDistributedNodeLabel(Node);
               Diagnostic["status"] = "invalid_json";
               Diagnostic["merge_reason"] = "parse_failed";
               Diagnostic["latency_ms"] = DurationToMillisecondsString(::Now() - RequestStart);
               Diagnostic["status_code"] = std::to_string(PeerResult.StatusCode);
               Diagnostic["retry_policy"] = "standard_peer_fallback";
               Diagnostic["attempts"] = std::to_string(PeerResult.Attempts);
               Diagnostic["used_secondary_token"] = PeerResult.UsedSecondaryToken ? "true" : "false";
               Diagnostic["used_no_auth_fallback"] = PeerResult.UsedNoAuthFallback ? "true" : "false";
               Diagnostic["replication_state"] = NodeState.Stale ? "stale" : (NodeState.HasReplicationState ? "fresh" : "unknown");
               Diagnostic["replication_trust_weight"] = std::to_string(NodeState.TrustWeight);
               Diagnostic["replication_dirty"] = NodeState.Dirty ? "true" : "false";
               Diagnostic["replication_resync_in_progress"] = NodeState.ResyncInProgress ? "true" : "false";
               Diagnostic["replication_last_reachable_ms"] = std::to_string(NodeState.LastReachableMS);
               Diagnostic["replication_last_resync_ms"] = std::to_string(NodeState.LastResyncMS);
               NodeDiagnostics.push_back(std::move(Diagnostic));

               if (OutError && OutError->empty())
               {
                    *OutError = "Failed to parse remote response JSON.";
               }
               continue;
          }

          const std::size_t HitsBeforeMerge = AggregatedHits.size();
          AppendHitsFromJSON(ResponseJSON,
                             AggregatedHits,
                             BuildDistributedNodeLabel(Node),
                             NodeState.Stale ? "stale" : (NodeState.HasReplicationState ? "fresh" : "unknown"));
          if (NodeState.TrustWeight < 1.0f)
          {
               for (std::size_t I = HitsBeforeMerge; I < AggregatedHits.size(); ++I)
               {
                    AggregatedHits[I].Weight *= NodeState.TrustWeight;
               }
          }
          FoundTotal += ResponseJSON.value("found", 0);
          OutOfTotal += ResponseJSON.value("out_of", 0);
          IndexingInProgress = IndexingInProgress || ResponseJSON.value("indexing_in_progress", false);
          PartialResults = PartialResults || ResponseJSON.value("partial_results", false);
          MaxSearchTime = std::max(MaxSearchTime, ResponseJSON.value("search_time_ms", 0.0f));
          if (ResponseJSON.contains("facets"))
          {
               MergeFacetCounts(FacetCounts, ResponseJSON["facets"]);
          }

          std::map<std::string, std::string> Diagnostic;
          Diagnostic["node"] = BuildDistributedNodeLabel(Node);
          Diagnostic["status"] = "ok";
          Diagnostic["merge_reason"] = "merged";
          Diagnostic["latency_ms"] = DurationToMillisecondsString(::Now() - RequestStart);
          Diagnostic["status_code"] = std::to_string(PeerResult.StatusCode);
          Diagnostic["hits_added"] = std::to_string(ResponseJSON.value("hits", nlohmann::json::array()).size());
          Diagnostic["found"] = std::to_string(ResponseJSON.value("found", 0));
          Diagnostic["out_of"] = std::to_string(ResponseJSON.value("out_of", 0));
          Diagnostic["search_time_ms"] = std::to_string(ResponseJSON.value("search_time_ms", 0.0f));
          Diagnostic["retry_policy"] = "standard_peer_fallback";
          Diagnostic["attempts"] = std::to_string(PeerResult.Attempts);
          Diagnostic["used_secondary_token"] = PeerResult.UsedSecondaryToken ? "true" : "false";
          Diagnostic["used_no_auth_fallback"] = PeerResult.UsedNoAuthFallback ? "true" : "false";
          Diagnostic["replication_state"] = NodeState.Stale ? "stale" : (NodeState.HasReplicationState ? "fresh" : "unknown");
          Diagnostic["replication_trust_weight"] = std::to_string(NodeState.TrustWeight);
          Diagnostic["replication_dirty"] = NodeState.Dirty ? "true" : "false";
          Diagnostic["replication_resync_in_progress"] = NodeState.ResyncInProgress ? "true" : "false";
          Diagnostic["replication_last_reachable_ms"] = std::to_string(NodeState.LastReachableMS);
          Diagnostic["replication_last_resync_ms"] = std::to_string(NodeState.LastResyncMS);
          NodeDiagnostics.push_back(std::move(Diagnostic));

          RemoteResponses++;
     }

     if (RemoteResponses == 0 && AggregatedHits.empty())
     {
          if (OutError && OutError->empty())
          {
               *OutError = "No distributed nodes available to execute query.";
          }
          return false;
     }

     int DuplicateCount = 0;
     std::vector<std::map<std::string, std::string>> DuplicateDocuments;

     if (!AggregatedHits.empty())
     {
          std::unordered_map<std::string, std::size_t> SeenById;
          std::vector<SearchHit> Deduped;
          Deduped.reserve(AggregatedHits.size());

          for (auto &Hit : AggregatedHits)
          {
               auto It = Hit.Document.find("id");
               if (It == Hit.Document.end() || It->second.empty())
               {
                    Deduped.push_back(std::move(Hit));
                    continue;
               }

               const std::string &Id = It->second;
               auto Found = SeenById.find(Id);
               if (Found == SeenById.end())
               {
                    SeenById[Id] = Deduped.size();
                    Deduped.push_back(std::move(Hit));
               }
               else
               {
                    DuplicateDocuments.emplace_back(Hit.Document);
                    DuplicateCount++;
                    std::size_t Index = Found->second;
                    if (ScoreForHit(Hit) > ScoreForHit(Deduped[Index]))
                    {
                         Deduped[Index] = std::move(Hit);
                    }
               }
          }

          AggregatedHits.swap(Deduped);
     }

     if (DuplicateCount > 0)
     {
          FoundTotal = std::max(0, FoundTotal - DuplicateCount);
          OutOfTotal = std::max(0, OutOfTotal - DuplicateCount);
     }
     if (!NodeDiagnostics.empty())
     {
          std::map<std::string, std::string> MergeDiagnostic;
          MergeDiagnostic["node"] = "merge";
          MergeDiagnostic["status"] = "ok";
          MergeDiagnostic["merge_reason"] = "post_merge";
          MergeDiagnostic["duplicates_dropped"] = std::to_string(DuplicateCount);
          MergeDiagnostic["final_hits"] = std::to_string(AggregatedHits.size());
          MergeDiagnostic["final_found"] = std::to_string(FoundTotal);
          MergeDiagnostic["final_out_of"] = std::to_string(OutOfTotal);
          NodeDiagnostics.push_back(std::move(MergeDiagnostic));
     }
     if (!DuplicateDocuments.empty() && !Query.FacetBy.empty())
     {
          for (const auto &Doc : DuplicateDocuments)
          {
               for (const auto &FacetField : Query.FacetBy)
               {
                    auto FieldIt = Doc.find(FacetField);
                    if (FieldIt == Doc.end() || FieldIt->second.empty())
                    {
                         continue;
                    }

                    auto FacetIt = FacetCounts.find(FacetField);
                    if (FacetIt == FacetCounts.end())
                    {
                         continue;
                    }

                    auto CountIt = FacetIt->second.find(FieldIt->second);
                    if (CountIt == FacetIt->second.end())
                    {
                         continue;
                    }

                    CountIt->second = std::max(0, CountIt->second - 1);
               }
          }
     }

     if (!Query.SortBy.empty())
     {
          AggregatedHits = ApplySorting(AggregatedHits, Query.SortBy);
     }
     else
     {
          std::stable_sort(AggregatedHits.begin(), AggregatedHits.end(),
                           [](const SearchHit &A, const SearchHit &B)
                           {
                                return ScoreForHit(A) > ScoreForHit(B);
                           });
     }

     OutResult->Hits.clear();
     const std::size_t Start = RequestedStart;
     if (Start < AggregatedHits.size())
     {
          const std::size_t End = std::min(AggregatedHits.size(), Start + PerPageSize);
          OutResult->Hits.insert(OutResult->Hits.end(),
                                 AggregatedHits.begin() + static_cast<std::vector<SearchHit>::difference_type>(Start),
                                 AggregatedHits.begin() + static_cast<std::vector<SearchHit>::difference_type>(End));
     }

     OutResult->Found = FoundTotal;
     OutResult->OutOf = OutOfTotal;
     OutResult->Page = Page;
     OutResult->PerPage = PerPage;
     OutResult->SearchTimeMS = MaxSearchTime;
     OutResult->IndexingInProgress = IndexingInProgress;
     OutResult->PartialResults = PartialResults;
     if (PartialResults)
     {
          OutResult->PartialReason = "one_or_more_nodes_partial";
     }
     OutResult->DistributedDiagnostics = std::move(NodeDiagnostics);

     OutResult->Facets.clear();
     for (const auto &FacetPair : FacetCounts)
     {
          FacetResult Res;
          Res.FieldName = FacetPair.first;
          for (const auto &CountPair : FacetPair.second)
          {
               FacetCount Count;
               Count.Value = CountPair.first;
               Count.Count = CountPair.second;
               Res.Counts.push_back(Count);
          }
          OutResult->Facets[FacetPair.first] = Res;
     }

     return true;
}

bool SearchAPI::ShouldAttemptDistributedIngest(const HttpRequest &Request) const
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     if (!Instance->Config->GetDistributedSearchEnabled())
     {
          return false;
     }

     if (Instance->Config->GetClusterNodes().empty())
     {
          return false;
     }

     const std::string HopHeader = ToLowerCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Distributed-Hop"));
     const std::string ReplicationHopHeader = ToLowerCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Replication-Hop"));
     if (HopHeader == "1" || HopHeader == "true" || ReplicationHopHeader == "1" || ReplicationHopHeader == "true")
     {
          return false;
     }

     auto RouteIt = Request.QueryParams.find("route");
     const bool HasRoute = (RouteIt != Request.QueryParams.end() && !TrimCopy(RouteIt->second).empty());

     auto OverrideIt = Request.QueryParams.find("distributed");
     if (OverrideIt != Request.QueryParams.end())
     {
          const std::string Override = ToLowerCopy(OverrideIt->second);
          if (Override == "off" || Override == "false" || Override == "0" || Override == "local")
          {
               return false;
          }
          if (Override == "on" || Override == "true" || Override == "1" || Override == "force" || Override == "remote")
          {
               return true;
          }
     }

     /*
      * Writes must remain local unless the caller explicitly opts into
      * distributed routing. This keeps single-node and mixed deployments
      * working even when cluster links are configured but temporarily down.
      */
     return HasRoute;
}

bool SearchAPI::ProxyDistributedRequest(const HttpRequest &Request,
                                        const std::string &Host,
                                        int Port,
                                        HttpResponse *OutResponse,
                                        std::string *OutError) const
{
     if (!OutResponse)
     {
          if (OutError)
          {
               *OutError = "Invalid proxy response handle.";
          }
          return false;
     }

     int TimeoutMS = Instance && Instance->Config ? Instance->Config->GetDistributedSearchTimeoutMS() : 2000;
     bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
     int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
     int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
     int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
     bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
     int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;
     std::string Endpoint = Host + ":" + std::to_string(Port);
     bool UseSSLForNode = false;
     std::vector<NodeEndpoint> KnownNodes;
     if (BuildClusterEndpoints(KnownNodes))
     {
          for (const auto &Node : KnownNodes)
          {
               if (ToLowerCopy(Node.Host) == ToLowerCopy(Host) && Node.Port == Port)
               {
                    Endpoint = Node.Endpoint;
                    UseSSLForNode = Node.UseSSL;
                    break;
               }
          }
     }

     PeerRequestResult PeerResult;
     if (!ExecutePeerRequestWithFallback(Host, Port, Endpoint, UseSSLForNode, Request, TimeoutMS, UsePersistentTransport, PersistentBurst, ConnectionsPerPeer, IdleMS, AutoReconnect, ReconnectMS, PeerTokenSource::Cluster, &PeerResult))
     {
          if (OutError)
          {
               *OutError = PeerResult.Error;
          }
          return false;
     }

     if (PeerResult.StatusCode == 0)
     {
          if (OutError)
          {
               *OutError = "Remote node returned invalid status.";
          }
          return false;
     }

     *OutResponse = HttpResponse(PeerResult.StatusCode, StatusText(PeerResult.StatusCode), "application/json");
     OutResponse->Body = PeerResult.Body;
     return true;
}

bool SearchAPI::SelectDistributedNodeForKey(const std::string &Key,
                                            std::string *OutHost,
                                            int *OutPort,
                                            bool *OutIsLocal) const
{
     std::vector<NodeEndpoint> Nodes;
     if (!BuildClusterEndpoints(Nodes))
     {
          return false;
     }

     if (Key.empty())
     {
          return false;
     }

     std::hash<std::string> Hasher;
     size_t Index = Hasher(Key) % Nodes.size();

     if (OutHost)
     {
          *OutHost = Nodes[Index].Host;
     }
     if (OutPort)
     {
          *OutPort = Nodes[Index].Port;
     }
     if (OutIsLocal)
     {
          *OutIsLocal = Nodes[Index].IsLocal;
     }
     return true;
}

bool SearchAPI::ResolveDistributedRoute(const std::string &Route,
                                        std::string *OutHost,
                                        int *OutPort,
                                        bool *OutIsLocal) const
{
     const std::string NormalizedRoute = ToLowerCopy(TrimCopy(Route));
     if (NormalizedRoute.empty())
     {
          return false;
     }

     if (NormalizedRoute == "local")
     {
          if (OutHost)
          {
               OutHost->clear();
          }
          if (OutPort)
          {
               *OutPort = 0;
          }
          if (OutIsLocal)
          {
               *OutIsLocal = true;
          }
          return true;
     }

     if (Instance && Instance->Config)
     {
          std::string RouteHost;
          int RoutePort = 0;
          bool RouteUseSSL = false;
          if (ParseNodeEndpoint(Route, RouteHost, RoutePort, RouteUseSSL))
          {
               const std::string RouteHostLower = ToLowerCopy(RouteHost);
               for (const auto &Bind : Instance->Config->GetBindConfigs())
               {
                    if (Bind.port != RoutePort)
                    {
                         continue;
                    }

                    const std::string BindAddrLower = ToLowerCopy(Bind.address);
                    const bool RouteLooksLocal = IsLocalHostName(RouteHost);
                    const bool BindLooksLocal = Bind.address.empty() || Bind.address == "0.0.0.0" || IsLocalHostName(Bind.address);
                    const bool HostMatchesBind = (RouteHostLower == BindAddrLower) || (RouteLooksLocal && BindLooksLocal);

                    if (!HostMatchesBind)
                    {
                         continue;
                    }

                    if (OutHost)
                    {
                         OutHost->clear();
                    }
                    if (OutPort)
                    {
                         *OutPort = Bind.port;
                    }
                    if (OutIsLocal)
                    {
                         *OutIsLocal = true;
                    }
                    return true;
               }
          }
     }

     std::vector<NodeEndpoint> Nodes;
     if (!BuildClusterEndpoints(Nodes))
     {
          return false;
     }

     std::vector<NodeEndpoint> Matches;
     for (const auto &Node : Nodes)
     {
          const std::string EndpointLower = ToLowerCopy(Node.Endpoint);
          const std::string HostPortLower = ToLowerCopy(Node.Host + ":" + std::to_string(Node.Port));
          const std::string HostLower = ToLowerCopy(Node.Host);

          if (NormalizedRoute == EndpointLower || NormalizedRoute == HostPortLower || NormalizedRoute == HostLower)
          {
               Matches.push_back(Node);
          }
     }

     if (Matches.size() != 1)
     {
          return false;
     }

     if (OutHost)
     {
          *OutHost = Matches[0].Host;
     }
     if (OutPort)
     {
          *OutPort = Matches[0].Port;
     }
     if (OutIsLocal)
     {
          *OutIsLocal = Matches[0].IsLocal;
     }

     return true;
}

bool SearchAPI::ShouldAttemptReplication(const HttpRequest &Request) const
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     if (!Instance->Config->GetReplicationEnabled())
     {
          return false;
     }

     if (Instance->Config->GetSlaveNodes().empty())
     {
          return false;
     }

     const std::string HopHeader = ToLowerCopy(GetHeaderValueInsensitive(Request.Headers, "X-HLQ-Replication-Hop"));
     if (HopHeader == "1" || HopHeader == "true")
     {
          return false;
     }

     return true;
}

/* Builds replication slave state key data. */

static std::string BuildReplicationSlaveStateKey(const std::string &Endpoint)
{
     return "replication_slave_state:" + Endpoint;
}

void SearchAPI::PersistReplicationSlaveState(const std::string &Endpoint) const
{
     if (Endpoint.empty() || !Instance || !Instance->Database)
     {
          return;
     }

     nlohmann::json State;
     {
          std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
          State["endpoint"] = Endpoint;
          State["dirty"] = (ReplicationDirtySlaves.find(Endpoint) != ReplicationDirtySlaves.end());
          State["resync_in_progress"] = (ReplicationResyncInProgress.find(Endpoint) != ReplicationResyncInProgress.end());
          State["last_reachable_ms"] = ReplicationLastReachableTimestampMS.count(Endpoint) ? ReplicationLastReachableTimestampMS.at(Endpoint) : 0;
          State["last_resync_ms"] = ReplicationLastResyncTimestampMS.count(Endpoint) ? ReplicationLastResyncTimestampMS.at(Endpoint) : 0;
          State["updated_ms"] = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     }

     Instance->Database->Set(BuildReplicationSlaveStateKey(Endpoint), State.dump());
     Instance->Database->SyncWAL();
}

bool SearchAPI::RestoreReplicationSlaveState(const std::string &Endpoint) const
{
     if (Endpoint.empty() || !Instance || !Instance->Database)
     {
          return false;
     }

     const std::string RawState = Instance->Database->Get(BuildReplicationSlaveStateKey(Endpoint));
     if (RawState.empty())
     {
          return false;
     }

     try
     {
          nlohmann::json State = nlohmann::json::parse(RawState);
          const bool Dirty = State.value("dirty", false);
          const bool ResyncInProgress = State.value("resync_in_progress", false);
          const uint64_t LastReachableMS = State.value("last_reachable_ms", static_cast<uint64_t>(0));
          const uint64_t LastResyncMS = State.value("last_resync_ms", static_cast<uint64_t>(0));

          std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
          if (Dirty || ResyncInProgress)
          {
               ReplicationDirtySlaves.insert(Endpoint);
          }
          else
          {
               ReplicationDirtySlaves.erase(Endpoint);
          }

          if (LastReachableMS > 0)
          {
               ReplicationLastReachableTimestampMS[Endpoint] = LastReachableMS;
          }

          if (LastResyncMS > 0)
          {
               ReplicationLastResyncTimestampMS[Endpoint] = LastResyncMS;
          }

          /*
           * A process restart means any previously "in progress" resync session is gone.
           * Restore it as dirty instead so the monitor schedules a fresh full resync.
           */
          ReplicationResyncInProgress.erase(Endpoint);
          return true;
     }
     catch (...)
     {
          return false;
     }
}

void SearchAPI::EnsureReplicationMonitorStarted() const
{
     if (ReplicationMonitorRunning.exchange(true, std::memory_order_acq_rel))
     {
          return;
     }

     ResetAllPeerReconnectState();

     if (Instance && Instance->Config && Instance->Config->GetReplicationEnabled())
     {
          for (const auto &Endpoint : Instance->Config->GetSlaveNodes())
          {
               if (!RestoreReplicationSlaveState(Endpoint))
               {
                    MarkSlaveDirty(Endpoint);
               }
          }
     }

     ReplicationMonitorStop.store(false, std::memory_order_relaxed);
     ReplicationMonitorThread = std::thread([this]()
                                            {
                                                 ReplicationMonitorLoop();
                                            });
}

void SearchAPI::EnsureDistributedLinkMonitorStarted() const
{
     if (DistributedLinkMonitorRunning.exchange(true, std::memory_order_acq_rel))
     {
          return;
     }

     ResetAllPeerReconnectState();
     DistributedLinkMonitorStop.store(false, std::memory_order_relaxed);
     DistributedLinkMonitorThread = std::thread([this]()
                                                {
                                                     DistributedLinkMonitorLoop();
                                                });
}

void SearchAPI::MarkSlaveDirty(const std::string &Endpoint) const
{
     const uint64_t NowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : 0;
     {
          std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
          ReplicationDirtySlaves.insert(Endpoint);
          LastReplicationErrorTimestampMS = NowMS;
     }
     PersistReplicationSlaveState(Endpoint);
     ReplicationMonitorCV.notify_one();
}

void SearchAPI::MarkSlaveReachable(const std::string &Endpoint) const
{
     const uint64_t NowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : 0;
     std::string Host;
     int Port = 0;
     bool UseSSL = false;
     if (ParseNodeEndpoint(Endpoint, Host, Port, UseSSL))
     {
          (void)UseSSL;
          ResetPeerReconnectState(Host + ":" + std::to_string(Port));
     }

     {
          std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
          ReplicationLastReachableTimestampMS[Endpoint] = NowMS;
     }
     PersistReplicationSlaveState(Endpoint);
     ReplicationMonitorCV.notify_one();
}

void SearchAPI::MarkSlaveResynced(const std::string &Endpoint) const
{
     const uint64_t NowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : 0;
     {
          std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
          ReplicationDirtySlaves.erase(Endpoint);
          ReplicationResyncInProgress.erase(Endpoint);
          ReplicationLastResyncTimestampMS[Endpoint] = NowMS;
     }
     PersistReplicationSlaveState(Endpoint);
     ReplicationMonitorCV.notify_one();
}

PeerReconnectDiagnostics SearchAPI::GetPeerReconnectDiagnostics(const std::string &Endpoint) const
{
     std::string Host;
     int Port = 0;
     bool UseSSL = false;
     if (!ParseNodeEndpoint(Endpoint, Host, Port, UseSSL))
     {
          return PeerReconnectDiagnostics();
     }

     (void)UseSSL;
     return SnapshotPeerReconnectState(Host + ":" + std::to_string(Port));
}

void SearchAPI::ClearPeerReconnectDiagnostics(const std::string &Endpoint) const
{
     std::string Host;
     int Port = 0;
     bool UseSSL = false;
     if (!ParseNodeEndpoint(Endpoint, Host, Port, UseSSL))
     {
          return;
     }

     (void)UseSSL;
     ResetPeerReconnectState(Host + ":" + std::to_string(Port));
}

bool SearchAPI::IsSlaveResyncActive(const std::string &Endpoint) const
{
     std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
     return ReplicationResyncInProgress.find(Endpoint) != ReplicationResyncInProgress.end();
}

bool SearchAPI::PingReplicationSlave(const std::string &Host,
                                     int Port,
                                     std::string *OutError) const
{
     int TimeoutMS = Instance && Instance->Config ? Instance->Config->GetReplicationTimeoutMS() : 2000;
     bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
     int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
     bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
     int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;
     std::string Endpoint = Host + ":" + std::to_string(Port);
     bool UseSSLForNode = false;
     std::vector<NodeEndpoint> KnownNodes;
     if (BuildSlaveEndpoints(KnownNodes))
     {
          for (const auto &Node : KnownNodes)
          {
               if (ToLowerCopy(Node.Host) == ToLowerCopy(Host) && Node.Port == Port)
               {
                    Endpoint = Node.Endpoint;
                    UseSSLForNode = Node.UseSSL;
                    break;
               }
          }
     }

     HttpRequest PingRequest;
     PingRequest.Method = "GET";
     PingRequest.Path = "/health";
     PingRequest.Headers["X-HLQ-Replication-Hop"] = "1";

     PeerRequestResult PeerResult;
     int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
     int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
     if (!ExecutePeerRequestWithFallback(Host, Port, Endpoint, UseSSLForNode, PingRequest, TimeoutMS, UsePersistentTransport, PersistentBurst, ConnectionsPerPeer, IdleMS, AutoReconnect, ReconnectMS, PeerTokenSource::Slave, &PeerResult))
     {
          if (OutError)
          {
               *OutError = PeerResult.Error;
          }
          return false;
     }

     if (PeerResult.StatusCode >= 200 && PeerResult.StatusCode < 300)
     {
          return true;
     }

     if (OutError)
     {
          *OutError = "health returned status " + std::to_string(PeerResult.StatusCode);
     }

     return false;
}

bool SearchAPI::ProxyReplicationRequest(const HttpRequest &Request,
                                        const std::string &Host,
                                        int Port,
                                        HttpResponse *OutResponse,
                                        std::string *OutError) const
{
     if (!OutResponse)
     {
          if (OutError)
          {
               *OutError = "Invalid replication response handle.";
          }
          return false;
     }

     int TimeoutMS = Instance && Instance->Config ? Instance->Config->GetReplicationTimeoutMS() : 2000;
     bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
     int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
     int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
     int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
     bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
     int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;
     std::string Endpoint = Host + ":" + std::to_string(Port);
     bool UseSSLForNode = false;
     std::vector<NodeEndpoint> KnownNodes;
     if (BuildSlaveEndpoints(KnownNodes))
     {
          for (const auto &Node : KnownNodes)
          {
               if (ToLowerCopy(Node.Host) == ToLowerCopy(Host) && Node.Port == Port)
               {
                    Endpoint = Node.Endpoint;
                    UseSSLForNode = Node.UseSSL;
                    break;
               }
          }
     }

     HttpRequest ReplicationRequest = Request;
     ReplicationRequest.Headers["X-HLQ-Replication-Hop"] = "1";

     PeerRequestResult PeerResult;
     if (!ExecutePeerRequestWithFallback(Host, Port, Endpoint, UseSSLForNode, ReplicationRequest, TimeoutMS, UsePersistentTransport, PersistentBurst, ConnectionsPerPeer, IdleMS, AutoReconnect, ReconnectMS, PeerTokenSource::Slave, &PeerResult))
     {
          if (OutError)
          {
               *OutError = PeerResult.Error;
          }
          return false;
     }

     if (PeerResult.StatusCode == 0)
     {
          if (OutError)
          {
               *OutError = "Replica node returned invalid status.";
          }
          return false;
     }

     *OutResponse = HttpResponse(PeerResult.StatusCode, StatusText(PeerResult.StatusCode), "application/json");
     OutResponse->Body = PeerResult.Body;
     return true;
}

bool SearchAPI::QueuePendingReplication(const std::string &Endpoint,
                                        const HttpRequest &Request,
                                        bool AllowOverflow,
                                        std::string *OutError) const
{
     const std::string QueueEndpoint = TrimCopy(Endpoint);
     if (QueueEndpoint.empty())
     {
          if (OutError)
          {
               *OutError = "Pending replication queue requires a non-empty endpoint.";
          }
          return false;
     }

     std::lock_guard<std::mutex> lock(PendingReplicationMutex);
     auto &Queue = PendingReplicationRequests[QueueEndpoint];
     if (!AllowOverflow && Queue.size() >= kMaxPendingReplicationRequests)
     {
          if (OutError)
          {
               *OutError = "Pending replication queue is full for " + QueueEndpoint + " (" + std::to_string(kMaxPendingReplicationRequests) + " requests queued).";
          }
          return false;
     }
     Queue.push_back(Request);
     MaybeTriggerCrashInjection("replication_queue_append");
     return true;
}

std::vector<HttpRequest> SearchAPI::TakePendingReplications(const std::string &Endpoint) const
{
     const std::string QueueEndpoint = TrimCopy(Endpoint);
     if (QueueEndpoint.empty())
     {
          return {};
     }

     std::lock_guard<std::mutex> lock(PendingReplicationMutex);
     auto It = PendingReplicationRequests.find(QueueEndpoint);
     if (It == PendingReplicationRequests.end())
     {
          return {};
     }

     std::vector<HttpRequest> Pending;
     Pending.swap(It->second);
     PendingReplicationRequests.erase(It);
     return Pending;
}

bool SearchAPI::ReplayPendingReplications(const std::string &Endpoint, const std::string &Host, int Port) const
{
     auto Pending = TakePendingReplications(Endpoint);
     if (Pending.empty())
     {
          return true;
     }

     std::vector<HttpRequest> Failed;
     for (const auto &Request : Pending)
     {
          MaybeTriggerCrashInjection("replication_replay_send");

          HttpResponse ReplicaResponse;
          std::string ReplicaError;
          if (ProxyReplicationRequest(Request, Host, Port, &ReplicaResponse, &ReplicaError))
          {
               if (ReplicaResponse.StatusCode >= 200 && ReplicaResponse.StatusCode < 300)
               {
                    continue;
               }
          }

          Failed.push_back(Request);
     }

     if (Failed.empty())
     {
          return true;
     }

     std::lock_guard<std::mutex> lock(PendingReplicationMutex);
     auto &Queue = PendingReplicationRequests[Endpoint];
     for (const auto &Request : Failed)
     {
          Queue.push_back(Request);
     }

     if (Queue.size() > kMaxPendingReplicationRequests)
     {
          RecordReplicationFailure("Pending replication queue for " + Endpoint + " exceeded nominal limit during replay; preserving " + std::to_string(Queue.size()) + " queued requests to avoid data loss.");
     }

     return false;
}

void SearchAPI::RecordReplicationFailure(const std::string &ErrorMessage) const
{
     ReplicationRequestsFailed.fetch_add(1, std::memory_order_relaxed);
     std::lock_guard<std::mutex> lock(ReplicationStatusMutex);
     LastReplicationError = ErrorMessage;
     LastReplicationErrorTimestampMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : 0;
}

void SearchAPI::RecordReplicationSuccess(size_t AckCount) const
{
     ReplicationRequestsSucceeded.fetch_add(1, std::memory_order_relaxed);
     ReplicationReplicaAcks.fetch_add(static_cast<uint64_t>(AckCount), std::memory_order_relaxed);
}

bool SearchAPI::ReplicateWriteRequest(const HttpRequest &Request,
                                      const std::string &OperationLabel,
                                      std::string *OutError) const
{
     EnsureReplicationMonitorStarted();

     if (!ShouldAttemptReplication(Request))
     {
          return true;
     }

     std::vector<NodeEndpoint> SlaveNodes;
     if (!BuildSlaveEndpoints(SlaveNodes))
     {
          return true;
     }

     std::vector<NodeEndpoint> RemoteSlaves;
     RemoteSlaves.reserve(SlaveNodes.size());
     for (const auto &Node : SlaveNodes)
     {
          if (!Node.IsLocal)
          {
               RemoteSlaves.push_back(Node);
          }
     }

     if (RemoteSlaves.empty())
     {
          return true;
     }

     ReplicationRequestsAttempted.fetch_add(1, std::memory_order_relaxed);

     const std::string Mode = Instance && Instance->Config ? ToLowerCopy(Instance->Config->GetReplicationMode()) : std::string("sync_one");
     HttpRequest ReplicationRequest = Request;
     EnsureReplicationOperationID(ReplicationRequest);

     size_t RequiredAcks = RemoteSlaves.size();
     if (Mode == "sync_one")
     {
          RequiredAcks = 1;
     }
     else if (Mode == "quorum")
     {
          RequiredAcks = (RemoteSlaves.size() / 2) + 1;
     }

     auto ExecuteReplication = [this, ReplicationRequest, OperationLabel, RemoteSlaves, RequiredAcks]() -> std::pair<bool, std::string>
     {
          size_t Acked = 0;
          std::vector<std::string> Errors;

          for (const auto &Node : RemoteSlaves)
          {
               uint64_t LastReachableMS = 0;
               bool IsDirty = false;
               bool ResyncInProgress = false;
               (void)GetReplicationNodeState(Node.Endpoint, &LastReachableMS, nullptr, &IsDirty, &ResyncInProgress);

               if (IsDirty || ResyncInProgress)
               {
                    std::string QueueError;
                    if (!QueuePendingReplication(Node.Endpoint, ReplicationRequest, false, &QueueError))
                    {
                         Errors.push_back(Node.Endpoint + ": " + QueueError);
                    }
                    else
                    {
                         if (ResyncInProgress)
                         {
                              Errors.push_back(Node.Endpoint + ": deferred while full resync is in progress");
                         }
                         else if (LastReachableMS > 0)
                         {
                              Errors.push_back(Node.Endpoint + ": deferred while replica is marked dirty/offline until monitor reconnects and resyncs it");
                         }
                         else
                         {
                              Errors.push_back(Node.Endpoint + ": deferred while replica is marked unavailable and awaiting first successful monitor probe");
                         }
                    }
                    ReplicationMonitorCV.notify_one();
                    continue;
               }

               HttpResponse ReplicaResponse;
               std::string ReplicaError;
               if (!ProxyReplicationRequest(ReplicationRequest, Node.Host, Node.Port, &ReplicaResponse, &ReplicaError))
               {
                    MarkSlaveDirty(Node.Endpoint);
                    std::string QueueError;
                    if (!QueuePendingReplication(Node.Endpoint, ReplicationRequest, false, &QueueError))
                    {
                         Errors.push_back(Node.Endpoint + ": " + (ReplicaError.empty() ? std::string("request failed") : ReplicaError) + "; " + QueueError);
                    }
                    else
                    {
                         Errors.push_back(Node.Endpoint + ": " + (ReplicaError.empty() ? std::string("request failed") : ReplicaError));
                    }
                    continue;
               }

               if (ReplicaResponse.StatusCode >= 200 && ReplicaResponse.StatusCode < 300)
               {
                    if (OperationLabel == "flush")
                    {
                         bool ReplicaFlushSynced = false;

                         try
                         {
                              const nlohmann::json ReplicaBody = nlohmann::json::parse(ReplicaResponse.Body);
                              ReplicaFlushSynced =
                                   ReplicaBody.value("success", false) &&
                                   ReplicaBody.value("database_synced", false) &&
                                   ReplicaBody.value("replica_flush_synced", false);
                         }
                         catch (...)
                         {
                              ReplicaFlushSynced = false;
                         }

                         if (!ReplicaFlushSynced)
                         {
                              MarkSlaveDirty(Node.Endpoint);
                              std::string QueueError;
                              const bool Queued = QueuePendingReplication(Node.Endpoint, ReplicationRequest, false, &QueueError);
                              Errors.push_back(Node.Endpoint + ": flush applied but replica did not confirm durable database sync" +
                                               (Queued ? std::string("") : "; " + QueueError));
                              continue;
                         }
                    }

                    Acked++;
                    MarkSlaveReachable(Node.Endpoint);
               }
               else
               {
                    const bool ResyncConflict = (ReplicaResponse.StatusCode == 409 &&
                                                 ReplicaResponse.Body.find("Replica resync in progress") != std::string::npos);
                    if (ResyncConflict)
                    {
                         {
                              std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
                              ReplicationResyncInProgress.insert(Node.Endpoint);
                         }
                         PersistReplicationSlaveState(Node.Endpoint);
                    }
                    MarkSlaveDirty(Node.Endpoint);
                    std::string QueueError;
                    const bool Queued = QueuePendingReplication(Node.Endpoint, ReplicationRequest, false, &QueueError);
                    if (ResyncConflict)
                    {
                         Errors.push_back(Node.Endpoint + ": " + (Queued ? std::string("deferred while replica resync fence is active") : QueueError));
                    }
                    else
                    {
                         std::string Message = "HTTP " + std::to_string(ReplicaResponse.StatusCode);
                         if (!Queued)
                         {
                              Message += "; " + QueueError;
                         }
                         Errors.push_back(Node.Endpoint + ": " + Message);
                    }
               }
          }

          if (Acked >= RequiredAcks)
          {
               RecordReplicationSuccess(Acked);
               return {true, ""};
          }

          std::ostringstream ErrorStream;
          ErrorStream << "Replication " << OperationLabel << " degraded: acked " << Acked
                      << "/" << RemoteSlaves.size() << " replicas, required " << RequiredAcks;
          if (!Errors.empty())
          {
               ErrorStream << " (" << Errors.front();
               for (size_t I = 1; I < Errors.size(); ++I)
               {
                    ErrorStream << "; " << Errors[I];
               }
               ErrorStream << ")";
          }

          const std::string ErrorText = ErrorStream.str();
          RecordReplicationFailure(ErrorText);
          return {false, ErrorText};
     };

     if (Mode == "async")
     {
          EnqueueAsyncReplicationTask([ExecuteReplication]()
                                      {
                                           ExecuteReplication();
                                      });
          return true;
     }

     auto Result = ExecuteReplication();
     if (!Result.first)
     {
          bool FailOnError = Instance && Instance->Config ? Instance->Config->GetReplicationFailOnError() : true;
          if (FailOnError)
          {
               if (OutError)
               {
                    *OutError = Result.second;
               }
               return false;
          }

          return true;
     }

     return true;
}

bool SearchAPI::ResyncSlaveFromScratch(const std::string &Host,
                                       int Port,
                                       std::string *OutError) const
{
     const uint64_t SessionClock = ReplicationOutboxClock.fetch_add(1, std::memory_order_relaxed);
     const uint64_t SessionNowMS = Instance ? static_cast<uint64_t>(Instance->NowMs()) : static_cast<uint64_t>(time(nullptr) * 1000);
     const std::string ResyncSessionID = Host + ":" + std::to_string(Port) + "-" + std::to_string(SessionNowMS) + "-" + std::to_string(SessionClock);

     std::string Endpoint = Host + ":" + std::to_string(Port);
     bool UseSSLForNode = false;
     std::vector<NodeEndpoint> KnownNodes;
     if (BuildSlaveEndpoints(KnownNodes))
     {
          for (const auto &Node : KnownNodes)
          {
               if (ToLowerCopy(Node.Host) == ToLowerCopy(Host) && Node.Port == Port)
               {
                    Endpoint = Node.Endpoint;
                    UseSSLForNode = Node.UseSSL;
                    break;
               }
          }
     }

     auto SendToSlave = [&](const std::string &Method,
                            const std::string &Path,
                            const std::string &Body,
                            const std::string &ResyncStage,
                            int *OutStatus,
                            std::string *OutBody,
                            std::string *OutSendError) -> bool
     {
          HttpRequest Req;
          Req.Method = Method;
          Req.Path = Path;
          Req.Body = Body;
          Req.Headers["X-HLQ-Replication-Hop"] = "1";
          Req.Headers["X-HLQ-Resync-Session"] = ResyncSessionID;
          if (!ResyncStage.empty())
          {
               Req.Headers["X-HLQ-Resync-Stage"] = ResyncStage;
          }
          if (!Body.empty())
          {
               Req.Headers["Content-Type"] = "application/json";
          }

          bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
          int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
          int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
          int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
          bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
          int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;
          int TimeoutMS = Instance && Instance->Config ? std::max(5000, Instance->Config->GetReplicationTimeoutMS()) : 5000;
          PeerRequestResult PeerResult;
          if (!ExecutePeerRequestWithFallback(Host, Port, Endpoint, UseSSLForNode, Req, TimeoutMS, UsePersistentTransport, PersistentBurst, ConnectionsPerPeer, IdleMS, AutoReconnect, ReconnectMS, PeerTokenSource::Slave, &PeerResult))
          {
               if (OutSendError)
               {
                    *OutSendError = PeerResult.Error;
               }
               return false;
          }

          if (OutStatus)
          {
               *OutStatus = PeerResult.StatusCode;
          }
          if (OutBody)
          {
               *OutBody = PeerResult.Body;
          }
          return true;
     };

     std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();

     int StatusCode = 0;
     std::string ResponseBody;
     std::string SendError;
     if (!SendToSlave("POST", "/flush", "", Collections.empty() ? "complete" : "start", &StatusCode, &ResponseBody, &SendError))
     {
          if (OutError)
          {
               *OutError = "flush failed: " + SendError;
          }
          return false;
     }

     if (StatusCode < 200 || StatusCode >= 300)
     {
          if (OutError)
          {
               *OutError = "flush returned HTTP " + std::to_string(StatusCode);
          }
          return false;
     }

     for (size_t CollectionIndex = 0; CollectionIndex < Collections.size(); ++CollectionIndex)
     {
          const std::string &Collection = Collections[CollectionIndex];
          CollectionConfig Config;
          (void)HybridStorageManagerInstance().GetCollectionConfig(Collection, Config);
          Config.Name = Collection;
          const bool IsLastCollection = (CollectionIndex + 1 == Collections.size());
          const std::vector<std::string> SnapshotDocIDs = SnapshotCollectionDocumentIDs(Collection);

          const std::string CollectionBody = SerializeCollectionConfigForReplication(Config);
          const bool HasDocuments = !SnapshotDocIDs.empty();
          const std::string CollectionStage = (IsLastCollection && !HasDocuments) ? "complete" : std::string();
          if (!SendToSlave("POST", "/collections", CollectionBody, CollectionStage, &StatusCode, &ResponseBody, &SendError))
          {
               if (OutError)
               {
                    *OutError = "create collection '" + Collection + "' failed: " + SendError;
               }
               return false;
          }

          if (!(StatusCode >= 200 && StatusCode < 300) && StatusCode != 409)
          {
               if (OutError)
               {
                    *OutError = "create collection '" + Collection + "' returned HTTP " + std::to_string(StatusCode);
               }
               return false;
          }

          constexpr int BatchSize = 2000;
          bool CompleteStageSent = (!CollectionStage.empty());
          for (std::size_t Offset = 0; Offset < SnapshotDocIDs.size(); Offset += BatchSize)
          {
               std::vector<Document> Docs;
               const std::size_t BatchEnd = std::min<std::size_t>(Offset + BatchSize, SnapshotDocIDs.size());
               Docs.reserve(BatchEnd - Offset);

               for (std::size_t I = Offset; I < BatchEnd; ++I)
               {
                    const Document Doc = HybridStorageManagerInstance().GetDocument(Collection, SnapshotDocIDs[I]);
                    if (!Doc.ID.empty())
                    {
                         Docs.push_back(Doc);
                    }
               }

               const bool IsLastBatch = (BatchEnd >= SnapshotDocIDs.size());
               if (Docs.empty())
               {
                    if (IsLastCollection && IsLastBatch && !CompleteStageSent)
                    {
                         if (!SendToSlave("GET", "/health", "", "complete", &StatusCode, &ResponseBody, &SendError))
                         {
                              if (OutError)
                              {
                                   *OutError = "finalize resync failed: " + SendError;
                              }
                              return false;
                         }

                         if (StatusCode < 200 || StatusCode >= 300)
                         {
                              if (OutError)
                              {
                                   *OutError = "finalize resync returned HTTP " + std::to_string(StatusCode);
                              }
                              return false;
                         }

                         CompleteStageSent = true;
                    }

                    continue;
               }

               const std::string BatchBody = SerializeDocumentBatchForReplication(Docs);
               const std::string ImportPath = "/collections/" + Collection + "/documents/import?distributed=off";
               const std::string ImportStage = (IsLastCollection && IsLastBatch) ? "complete" : std::string();
               if (!SendToSlave("POST", ImportPath, BatchBody, ImportStage, &StatusCode, &ResponseBody, &SendError))
               {
                    if (OutError)
                    {
                         *OutError = "bulk import '" + Collection + "' failed: " + SendError;
                    }
                    return false;
               }

               if (StatusCode < 200 || StatusCode >= 300)
               {
                    if (OutError)
                    {
                         *OutError = "bulk import '" + Collection + "' returned HTTP " + std::to_string(StatusCode);
                    }
                    return false;
               }

               if (!ImportStage.empty())
               {
                    CompleteStageSent = true;
               }
          }
     }

     return true;
}

void SearchAPI::DistributedLinkMonitorLoop() const
{
     constexpr auto ProbeInterval = std::chrono::seconds(5);

     while (!DistributedLinkMonitorStop.load(std::memory_order_relaxed))
     {
          if (!Instance || !Instance->Config || Instance->Config->GetClusterNodes().empty())
          {
               std::unique_lock<std::mutex> lock(DistributedLinkMonitorCVMutex);
               DistributedLinkMonitorCV.wait_for(lock, ProbeInterval);
               continue;
          }

          std::vector<NodeEndpoint> ClusterNodes;
          if (BuildClusterEndpoints(ClusterNodes))
          {
               for (const auto &Node : ClusterNodes)
               {
                    if (DistributedLinkMonitorStop.load(std::memory_order_relaxed))
                    {
                         break;
                    }

                    if (Node.IsLocal)
                    {
                         ResetPeerReconnectState(Node.Host + ":" + std::to_string(Node.Port));
                         continue;
                    }

                    HttpRequest PingRequest;
                    PingRequest.Method = "GET";
                    PingRequest.Path = "/health";
                    PingRequest.Headers["X-HLQ-Link-Ping"] = "1";

                    const int TimeoutMS = Instance && Instance->Config ? Instance->Config->GetDistributedSearchTimeoutMS() : 2000;
                    const bool UsePersistentTransport = Instance && Instance->Config ? Instance->Config->GetDistributedPersistentTransport() : true;
                    const int PersistentBurst = Instance && Instance->Config ? Instance->Config->GetDistributedTransportBurst() : 32;
                    const int ConnectionsPerPeer = Instance && Instance->Config ? Instance->Config->GetDistributedConnectionsPerPeer() : 4;
                    const int IdleMS = Instance && Instance->Config ? Instance->Config->GetDistributedTransportIdleMS() : 30000;
                    const bool AutoReconnect = Instance && Instance->Config ? Instance->Config->GetDistributedAutoReconnect() : true;
                    const int ReconnectMS = Instance && Instance->Config ? Instance->Config->GetDistributedReconnectMS() : 1500;

                    const int ProbeCount = UsePersistentTransport ? std::max(1, ConnectionsPerPeer) : 1;
                    for (int ProbeIndex = 0; ProbeIndex < ProbeCount; ++ProbeIndex)
                    {
                         if (DistributedLinkMonitorStop.load(std::memory_order_relaxed))
                         {
                              break;
                         }

                         PeerRequestResult PeerResult;
                         (void)ExecutePeerRequestWithFallback(Node.Host,
                                                              Node.Port,
                                                              Node.Endpoint,
                                                              Node.UseSSL,
                                                              PingRequest,
                                                              TimeoutMS,
                                                              UsePersistentTransport,
                                                              PersistentBurst,
                                                              ConnectionsPerPeer,
                                                              IdleMS,
                                                              AutoReconnect,
                                                              ReconnectMS,
                                                              PeerTokenSource::Cluster,
                                                              &PeerResult);
                    }
               }
          }

          if (DistributedLinkMonitorStop.load(std::memory_order_relaxed))
          {
               break;
          }

          std::unique_lock<std::mutex> lock(DistributedLinkMonitorCVMutex);
          DistributedLinkMonitorCV.wait_for(lock, ProbeInterval);
     }
}

void SearchAPI::ReplicationMonitorLoop() const
{
     constexpr auto ProbeInterval = std::chrono::seconds(5);

     while (!ReplicationMonitorStop.load(std::memory_order_relaxed))
     {
          if (!Instance || !Instance->Config || !Instance->Config->GetReplicationEnabled())
          {
               std::unique_lock<std::mutex> lock(ReplicationMonitorCVMutex);
               ReplicationMonitorCV.wait_for(lock, ProbeInterval);
               continue;
          }

          std::vector<NodeEndpoint> SlaveNodes;
          if (BuildSlaveEndpoints(SlaveNodes))
          {
               for (const auto &Node : SlaveNodes)
               {
                    if (Node.IsLocal)
                    {
                         continue;
                    }

                    std::string Error;
                    if (!PingReplicationSlave(Node.Host, Node.Port, &Error))
                    {
                         MarkSlaveDirty(Node.Endpoint);
                         continue;
                    }

                    MarkSlaveReachable(Node.Endpoint);

                    bool ShouldResync = false;
                    {
                         std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
                         if (ReplicationDirtySlaves.find(Node.Endpoint) != ReplicationDirtySlaves.end() &&
                             ReplicationResyncInProgress.find(Node.Endpoint) == ReplicationResyncInProgress.end())
                         {
                              ReplicationResyncInProgress.insert(Node.Endpoint);
                              ShouldResync = true;
                         }
                    }

                    if (ShouldResync)
                    {
                         PersistReplicationSlaveState(Node.Endpoint);
                    }

                    if (!ShouldResync)
                    {
                         continue;
                    }

                    std::string ResyncError;
                    if (!ResyncSlaveFromScratch(Node.Host, Node.Port, &ResyncError))
                    {
                         RecordReplicationFailure("Slave resync failed for " + Node.Endpoint + ": " + ResyncError);
                         std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
                         ReplicationResyncInProgress.erase(Node.Endpoint);
                         ReplicationDirtySlaves.insert(Node.Endpoint);
                         continue;
                    }

                    if (!ReplayPendingReplications(Node.Endpoint, Node.Host, Node.Port))
                    {
                         RecordReplicationFailure("Slave replay backlog still pending for " + Node.Endpoint + " after full resync; leaving replica dirty for retry.");
                         {
                              std::lock_guard<std::mutex> lock(ReplicationSlaveStateMutex);
                              ReplicationResyncInProgress.erase(Node.Endpoint);
                              ReplicationDirtySlaves.insert(Node.Endpoint);
                         }
                         PersistReplicationSlaveState(Node.Endpoint);
                         continue;
                    }

                    MarkSlaveResynced(Node.Endpoint);
               }
          }

          if (ReplicationMonitorStop.load(std::memory_order_relaxed))
          {
               break;
          }

          std::unique_lock<std::mutex> lock(ReplicationMonitorCVMutex);
          ReplicationMonitorCV.wait_for(lock, ProbeInterval);
     }
}
