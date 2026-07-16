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
#include <chrono>
#include <deque>
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
#include "runtime/serverconfig.h"
#include "core/httpcodes.h"
#include "core/socketengine.h"
#include "vendor/json/json.hpp"

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

     /* Initialize an empty request with sane defaults. */

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

     /* Build an HTTP response with headers pre-populated for this server. */

     HttpResponse(int Code = HttpCodes::code::OK, const std::string &Text = HttpCodes::StatusText(HttpCodes::code::OK), const std::string &ContentType = "text/plain")
         : StatusCode(Code), StatusText(Text)
     {
          Headers["Content-Type"] = ContentType;
          Headers["Server"] = "hlquery/1.0";
     }
};

inline HttpResponse BuildRouteNotFoundResponse(const std::string &Path, const std::string &Method = std::string())
{
     HttpResponse Response(HttpCodes::code::NOT_FOUND, StatusText(HttpCodes::code::NOT_FOUND), "application/json");

     nlohmann::json Body;
     Body["error"] = "Route not found";
     Body["path"] = Path;

     if (!Method.empty())
     {
          Body["method"] = Method;
     }

     Response.Body = Body.dump();

     return Response;
}

/* Route actions returned by HTTP route resolution. */

enum class RouteAction
{
     Status,
     SearchConfig,
     ConfigFiles,
     Health,
     Ready,
     Ping,
     Stats,
     Metrics,
     MetricsHistory,
     Cache,
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
     GetCollectionLanguage,
     UpdateCollection,
     DeleteCollection,
     DocumentSearch,
     VectorSearch,
     MultiSearch,
     GlobalSearch,
     ListDocuments,
     GetDocument,
     GetDocumentContext,
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
     ListPresets,
     UpsertPreset,
     GetPreset,
     DeletePreset,
     AnalyticsClick,
     ListModules,
     GetModuleSyntax,
     ModuleLoad,
     ModuleUnload,
     ModuleAPI,
     NotFound
};

/*
 * Identifies the resolved action for one incoming HTTP request.
 * Each value maps a parsed route to the handler logic used by the server.
 */

/* Resolve the route action for one parsed HTTP request. */

RouteAction ResolveHttpRoute(const HttpRequest &Request);

/* Return the human-readable name for one route action value. */

const char *RouteActionName(RouteAction ActionVal);

/* 
 * HTTP connection handler.
 * This event handler owns request parsing, response buffering,
 * socket lifecycle handling, and optional threaded execution.
 */

class HttpConnection : public EventHandler
{
   public:
     /* Construct one HTTP connection handler for an accepted socket. */

     HttpConnection(int FD, const std::string &ClientIP, int ClientPort, SearchThreadPool *ThreadPool = nullptr);

     /* Destroy the connection handler and release any remaining resources. */

     virtual ~HttpConnection();

     /* Handle a readable event for this connection. */

     void OnEventHandlerRead() override;

     /* Handle a writable event for this connection. */

     void OnEventHandlerWrite() override;

     /* Handle a socket error reported by the event engine. */

     void OnEventHandlerError(int ErrorNum) override;

     /* Process one or more pending HTTP requests from the connection buffer. */

     void ProcessRequest();

     /* Process all complete requests currently buffered on the connection. */

     void ProcessMultipleRequests();

     /* Process one complete raw HTTP request string. */

     void ProcessSingleRequest(const std::string &RequestStr);

     /* Queue or send one HTTP response for this connection. */

     void SendResponse(const HttpResponse &Response);

     /* Apply socket-level options used by the HTTP connection. */

     void SetAdvancedSocketOptions(int FD);

     /* Clean up the connection state before final close. */

     void CleanupConnection();

     /* Force the connection into closed state immediately. */

     void ForceClose();

#ifdef HLQUERY_HAS_OPENSSL

     /* Attach the negotiated SSL session for this connection. */

     void SetSSL(SSL *SSLVal)
     {
          SSLValue = SSLVal;
     }

     /* Return the SSL session currently attached to this connection. */

     SSL *GetSSL() const
     {
          return SSLValue;
     }

#endif

   private:
     /* Remote client IP address */

     std::string ClientIP;

     /* Remote client port */

     int ClientPort;

#ifdef HLQUERY_HAS_OPENSSL

     SSL *SSLValue = nullptr;
     bool SSLHandshaked = false;

#endif

     /* Buffered unread request data */

     std::string RequestBuffer;

     /* Buffered response data waiting to be written */

     std::string ResponseBuffer;

     /* Additional complete responses waiting behind the active response. */

     std::deque<std::string> ResponseQueue;

     /* Track offset to avoid O(N^2) buffer erasures. */

     size_t ResponseSentOffset = 0;

     /* Monotonic id for the active response buffer. */

     uint64_t ResponseSerial = 0;

     /* Indicates whether a response is currently pending for write */

     bool ResponsePending;

     /* Protect ResponseBuffer and ResponsePending. */

     std::mutex ResponseMutex;

     /* Connection pooling and keep-alive state */

     bool KeepAlive;

     /* Number of requests already processed on this connection */

     int RequestsProcessed;

     /* Last observed activity time for timeout handling */

     std::chrono::steady_clock::time_point LastActivity;

     /* Mark connection as closing to prevent use-after-free. */

     std::atomic<bool> ClosingValue;

     /* Tracks the active request id so responses can echo it back. */

     std::string ActiveRequestID;

     /* Number of active asynchronous request tasks */

     std::atomic<int> ActiveRequestTasks{0};

     /* Parse one raw HTTP request into the structured request object. */

     bool ParseHttpRequest(const std::string &RawRequest, HttpRequest &Request);

     /* Parse query parameters from one request path. */

     std::map<std::string, std::string> ParseQueryParams(const std::string &Path);

     /* Parse HTTP header lines into the request header map. */

     void ParseHeaders(const std::string &HeaderLines, std::map<std::string, std::string> &Headers);

     /* Optional HTTP execution pool assigned by server acceptor. */

     SearchThreadPool *ThreadPoolValue{nullptr};

   public:
     /* Indicate whether there are in-flight asynchronous requests. */

     bool HasActiveRequests() const
     {
          return ActiveRequestTasks.load(std::memory_order_acquire) > 0;
     }
};

/* 
 * Accepts and coordinates raw HTTP traffic through the socket engine.
 * This server owns listener lifecycle, connection acceptance,
 * and integration with the request handling pipeline.
 */

class HttpServer : public EventHandler
{
   public:
     /* Constructor. */

     HttpServer(const BindConfig &ConfigVal);

     /* Destructor. */

     virtual ~HttpServer();

     /* Start the HTTP listener and begin accepting connections. */

     bool Start();

     /* Stop accepting new connections and close active ones. */

     void Stop();

     /* Return whether the server is currently running. */

     bool IsRunning() const
     {
          return Running;
     }

     /* Assign a thread pool used for request processing. */

     void SetThreadPool(SearchThreadPool *Pool)
     {
          ThreadPoolValue = Pool;
     }

     /* Toggle whether the server should accept new connections. */

     void SetReadyToAccept(bool Ready);

     /* Return whether the server currently accepts new connections. */

     bool IsReadyToAccept() const
     {
          return ReadyToAcceptValue.load();
     }

     /* Toggle whether the server is blocking requests for loading. */

     void SetLoading(bool Loading);

     /* Return whether the server is currently blocking requests for loading. */

     bool IsLoading() const
     {
          return IsLoadingValue.load();
     }

     /* Register one handler for the given method and path. */

     void RegisterRoute(const std::string &Method, const std::string &Path,
                        std::function<HttpResponse(const HttpRequest &)> Handler);

     /* Default handler used when no route matches the request. */

     HttpResponse HandleNotFound(const HttpRequest &Request);

     /* Default handler for health probes. */

     HttpResponse HandleHealth(const HttpRequest &Request);

     /* Default handler for readiness probes. */

     HttpResponse HandleReady(const HttpRequest &Request);

     /* Accept and process pending socket events for the listener. */

     void OnEventHandlerRead() override;

     /* No-op write handler because the server socket is read-only. */

     void OnEventHandlerWrite() override
     {
     }

     /* Handle listener socket errors reported by the event engine. */

     void OnEventHandlerError(int ErrorNum) override;

   private:
     BindConfig Config;

     bool Running;

     /* Prevent accepting connections until data loading is complete. */

     std::atomic<bool> ReadyToAcceptValue{false};

     /* Block ALL HTTP requests during data loading. */

     std::atomic<bool> IsLoadingValue{true};

     std::unordered_map<std::string, std::function<HttpResponse(const HttpRequest &)>> Routes;

     /* Connection management. */

     std::vector<std::unique_ptr<HttpConnection>> Connections;

     /* Protect Connections vector from race conditions. */

     std::mutex ConnectionsMutex;

#ifdef HLQUERY_HAS_OPENSSL

     SSL_CTX *SSLCtx = nullptr;

#endif

     /* Accept new TCP connections from the listener. */

     void AcceptConnection();

     /* Remove and destroy closed connections from the active set. */

     void CleanupConnections();

     /* Thread pool for request processing. */

     SearchThreadPool *ThreadPoolValue{nullptr};
};

/* Validate SSL settings without binding sockets (preflight). */

bool ValidateSSLConfig(const BindConfig &ConfigVal, std::string *ErrorMsg = nullptr);

/* Initialize and start the HTTP server. */

bool InitializeHttpServer(const BindConfig &ConfigVal, HttpServer *&HttpServerPtr);

/* Stop and destroy the HTTP server. */

void ShutdownHttpServer(HttpServer *&HttpServerPtr);

/* URL decoding utility function. */

std::string URLDecode(const std::string &Str);
