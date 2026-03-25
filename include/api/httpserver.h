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

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/config.h"

#ifdef HLQUERY_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include "common/searchpool.h"
#include "core/serverconfig.h"
#include "core/socketengine.h"

/* HTTP Request structure. */

struct HttpRequest
{
     std::string Method;
     std::string Path;
     std::string Version;
     std::map<std::string, std::string> Headers;
     std::string Body;
     std::string RemoteAddress;
     int RemotePort;

     /* Query parameters parsed from path. */

     std::map<std::string, std::string> QueryParams;

     /* Internal metadata for scoped API keys. */

     std::string APIKeyID;
     std::string EmbeddedFilters;
     bool Authenticated = false;
     std::function<bool()> IsCancelled;

     HttpRequest() : RemotePort(0)
     {
     
     }
};

/* HTTP Response structure. */

struct HttpResponse
{
     int StatusCode;
     std::string StatusText;
     std::map<std::string, std::string> Headers;
     std::string Body;

     HttpResponse(int Code = 200, const std::string& Text = "OK", const std::string& ContentType = "text/plain")
         : StatusCode(Code), StatusText(Text)
     {
          Headers["Content-Type"] = ContentType;
          Headers["Server"] = "hlquery/1.0";
     }
};

/* Route actions returned by HTTP route resolution. */

enum class RouteAction
{
     Status,
     Health,
     Ping,
     Stats,
     Metrics,
     MetricsHistory,
     Connections,
     RocksDB,
     DocTotal,
     Flush,
     UpdateCounters,
     DebugCounters,
     Repair,
     Startup,
     Integrity,
     SelfCheck,
     StorageStatus,
     Etc,
     Root,
     ListCollections,
     ListCollectionsDistributed,
     CreateCollection,
     GetCollection,
     UpdateCollection,
     DeleteCollection,
     DocumentSearch,
     VectorSearch,
     MultiSearch,
     GlobalSearch,
     ListDocuments,
     GetDocument,
     AddDocument,
     BulkImportDocuments,
     UpdateDocument,
     DeleteDocument,
     DeleteDocumentsByFilter,
     UpdateByQuery,
     DeleteByQuery,
     FacetCounts,
     ExportDocuments,
     MaybeSuggest,
     ListSynonyms,
     ListAllSynonyms,
     UpsertSynonym,
     GetSynonym,
     DeleteSynonym,
     ListGlobalSynonyms,
     UpsertGlobalSynonym,
     GetGlobalSynonym,
     DeleteGlobalSynonym,
     ListStopwords,
     ListAllStopwords,
     CreateStopword,
     DeleteStopword,
     ListGlobalStopwords,
     CreateGlobalStopword,
     DeleteGlobalStopword,
     ListOverrides,
     UpsertOverride,
     GetOverride,
     DeleteOverride,
     ListAliases,
     UpsertAlias,
     GetAlias,
     DeleteAlias,
     LinksList,
     LinksPing,
     LinksConnect,
     LinksDisconnect,
     ListUsers,
     CreateUser,
     GetUser,
     UpdateUser,
     DeleteUser,
     ListKeys,
     CreateKey,
     GetKey,
     UpdateKey,
     DeleteKey,
     AnalyticsClick,
     ListModules,
     GetModuleSyntax,
     ModuleAPI,
     NotFound
};

/* Route resolver and action name helpers. */
RouteAction ResolveHttpRoute(const HttpRequest& Request);
const char* RouteActionName(RouteAction ActionVal);

/* HTTP Connection handler. */

class HttpConnection : public EventHandler
{
   public:

     /* Constructor. */

     HttpConnection(int FD, const std::string& ClientIP, int ClientPort, HLQuerySearchThreadPool* ThreadPool = nullptr);

     /* Destructor. */

     virtual ~HttpConnection();

     /* EventHandler interface. */

     void OnEventHandlerRead() override;

     void OnEventHandlerWrite() override;

     void OnEventHandlerError(int ErrorNum) override;

     /* HTTP processing. */

     void ProcessRequest();

     void ProcessMultipleRequests();

     void ProcessSingleRequest(const std::string& RequestStr);

     void SendResponse(const HttpResponse& Response);

     /* Socket optimization. */

     void SetAdvancedSocketOptions(int FD);

     void CleanupConnection();

#ifdef HLQUERY_HAS_OPENSSL
     void SetSSL(SSL* SSLVal)
     {
          SSLValue = SSLVal;
     }

     SSL* GetSSL() const
     {
          return SSLValue;
     }
#endif

   private:

     std::string ClientIP;

     int ClientPort;

#ifdef HLQUERY_HAS_OPENSSL
     SSL* SSLValue = nullptr;
     bool SSLHandshaked = false;
#endif

     std::string RequestBuffer;

     std::string ResponseBuffer;

     /* Track offset to avoid O(N^2) buffer erasures. */

     size_t ResponseSentOffset = 0;

     bool ResponsePending;

     /* Protect ResponseBuffer and ResponsePending. */

     std::mutex ResponseMutex;

     /* Connection pooling and keep-alive. */

     bool KeepAlive;

     int RequestsProcessed;

     std::chrono::steady_clock::time_point LastActivity;

     /* Mark connection as closing to prevent use-after-free. */

     std::atomic<bool> ClosingValue;
     std::atomic<int> ActiveRequestTasks{0};

     /* HTTP parsing. */

     bool ParseHttpRequest(const std::string& RawRequest, HttpRequest& Request);

     std::map<std::string, std::string> ParseQueryParams(const std::string& Path);

     void ParseHeaders(const std::string& HeaderLines, std::map<std::string, std::string>& Headers);

     /* Optional HTTP execution pool assigned by server acceptor. */

     HLQuerySearchThreadPool* ThreadPoolValue{nullptr};

   public:

     bool HasActiveRequests() const
     {
          return ActiveRequestTasks.load(std::memory_order_acquire) > 0;
     }
};

/* Raw HTTP Server using socketengine. */

class HttpServer : public EventHandler
{
   public:

     /* Constructor. */

     HttpServer(const BindConfig& ConfigVal);

     /* Destructor. */

     virtual ~HttpServer();

     /* Server lifecycle. */

     bool Start();

     void Stop();

     bool IsRunning() const
     {
          return Running;
     }

     /* Thread pool integration. */

     void SetThreadPool(HLQuerySearchThreadPool* Pool)
     {
          ThreadPoolValue = Pool;
     }

     /* Server readiness control. */

     void SetReadyToAccept(bool Ready);

     bool IsReadyToAccept() const
     {
          return ReadyToAcceptValue.load();
     }

     /* Loading state control. */

     void SetLoading(bool Loading);

     bool IsLoading() const
     {
          return IsLoadingValue.load();
     }

     /* Route registration. */

     void RegisterRoute(const std::string& Method, const std::string& Path,
                        std::function<HttpResponse(const HttpRequest&)> Handler);

     /* Default handlers. */

     HttpResponse HandleNotFound(const HttpRequest& Request);

     HttpResponse HandleHealth(const HttpRequest& Request);

     /* EventHandler interface. */

     void OnEventHandlerRead() override;

     void OnEventHandlerWrite() override
     {
     }

     void OnEventHandlerError(int ErrorNum) override;

   private:

     BindConfig Config;

     bool Running;

     /* Prevent accepting connections until data loading is complete. */

     std::atomic<bool> ReadyToAcceptValue{false};

     /* Block ALL HTTP requests during data loading. */

     std::atomic<bool> IsLoadingValue{true};

     std::unordered_map<std::string, std::function<HttpResponse(const HttpRequest&)>> Routes;

     /* Connection management. */

     std::vector<std::unique_ptr<HttpConnection>> Connections;

     /* Protect Connections vector from race conditions. */

     std::mutex ConnectionsMutex;

#ifdef HLQUERY_HAS_OPENSSL
     SSL_CTX* SSLCtx = nullptr;
#endif

     void AcceptConnection();

     void CleanupConnections();

     /* Thread pool for request processing. */

     HLQuerySearchThreadPool* ThreadPoolValue{nullptr};
};

/* Validate SSL settings without binding sockets (preflight). */

bool ValidateSSLConfig(const BindConfig& ConfigVal, std::string* ErrorMsg = nullptr);

/* Initialize and start the HTTP server. */

bool InitializeHttpServer(const BindConfig& ConfigVal, HttpServer*& HttpServerPtr, LogManager* Logs);

/* Stop and destroy the HTTP server. */

void ShutdownHttpServer(HttpServer*& HttpServerPtr);

/* URL decoding utility function. */

std::string URLDecode(const std::string& Str);
