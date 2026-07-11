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
#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "api/apikeys.h"
#include "api/httpserver.h"
#include "api/ipfilter.h"
#include "api/searchapi.h"
#include "api/userauth.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/httpcodes.h"
#include "core/modulemanager.h"
#include "utils/consolewriter.h"
#include "utils/jsonbuilder.h"
#include "vendor/json/json.hpp"

/* Implements HTTP request parsing, authorization, routing, and socket handling. */

#define HTTP_MAX_HEADER_SIZE (64 * 1024)

/* Maximum number of HTTP headers (1000) - Prevent DoS from too many headers. */

#define HTTP_MAX_HEADER_COUNT 1000

/* ProcessRequestWithAPI handles API calls. */

HttpResponse ProcessRequestWithAPI(SearchAPI &API, const HttpRequest &Request);
static bool ExtractAuthTokenFromRequest(const HttpRequest &Request, std::string &OutAuthHeader, std::string &OutToken);

static RouteAction ResolveRouteWithFallback(const HttpRequest &Request);
static bool IsPublicRouteAction(RouteAction ActionVal);
static bool IsAdminOnlyRouteAction(RouteAction ActionVal);
static std::string NormalizeRequestPath(const std::string &Path);
static bool IsModuleControlRoutePath(const std::string &Path);
static bool IsAuthorizedReplicationRequest(const HttpRequest &Request);
static bool IsHealthLikePath(const std::string &Path);
static bool IsDocumentIngestionRequest(const std::string &Method, const std::string &Path);

struct RouteContext
{
     std::string NormalizedPath;
     RouteAction ActionVal = RouteAction::NotFound;
     std::string CollectionName;
     bool IsPublic = false;
     bool IsAdminOnly = false;
     bool IsModuleControl = false;
     bool IsHealthCheck = false;
     bool IsCollectionCreation = false;
     bool IsDocumentImport = false;
     bool IsListDocuments = false;
     bool IsExpensiveQuery = false;
};

static RouteContext BuildRouteContext(const HttpRequest &Request, SearchAPI *API = nullptr);

/* Checks whether use async HTTP dispatch should be used. */

static bool ShouldUseAsyncHttpDispatch()
{
     static const bool UseThreadPool = []
     {
          const char *EnvValue = std::getenv("HLQUERY_HTTP_USE_THREAD_POOL");

          if (!EnvValue)
          {
               return false;
          }

          std::string Value(EnvValue);
          std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });

          if (!(Value == "1" || Value == "true" || Value == "yes" || Value == "on"))
          {
               return false;
          }

          /*
           * HttpConnection instances are owned by HttpServer::Connections and can be
           * destroyed by the event loop. Until connection ownership is ref-counted,
           * dispatching lambdas that capture `this` is unsafe.
           */

          const char *UnsafeValue = std::getenv("HLQUERY_HTTP_ALLOW_UNSAFE_ASYNC_CONNECTIONS");

          if (!UnsafeValue)
          {
               return false;
          }

          std::string UnsafeFlag(UnsafeValue);
          std::transform(UnsafeFlag.begin(), UnsafeFlag.end(), UnsafeFlag.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });

          return UnsafeFlag == "1" || UnsafeFlag == "true" || UnsafeFlag == "yes" || UnsafeFlag == "on";
     }();

     return UseThreadPool;
}

/* UrlDecode decodes a URL string. */

std::string UrlDecode(const std::string &Str, bool DecodePlusAsSpace = true);

/* Checks whether hex digit applies. */

static bool IsHexDigit(char C)
{
     return std::isxdigit(static_cast<unsigned char>(C)) != 0;
}

/* Implements the hex digit value helper. */

static int HexDigitValue(char C)
{
     if (C >= '0' && C <= '9')
     {
          return C - '0';
     }

     C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
     return 10 + (C - 'a');
}

/* Checks whether valid request framing headers exists. */

static bool HasValidRequestFramingHeaders(const std::string &HeadersPart, std::string *Error)
{
     std::istringstream Stream(HeadersPart);
     std::string Line;
     bool FirstLine = true;
     size_t ContentLengthCount = 0;

     while (std::getline(Stream, Line))
     {
          if (!Line.empty() && Line.back() == '\r')
          {
               Line.pop_back();
          }

          if (FirstLine)
          {
               FirstLine = false;
               continue;
          }

          if (Line.empty())
          {
               break;
          }

          const size_t Colon = Line.find(':');
          if (Colon == std::string::npos)
          {
               if (Error)
               {
                    *Error = "Malformed HTTP header line";
               }
               return false;
          }

          std::string Name = Line.substr(0, Colon);
          Name.erase(0, Name.find_first_not_of(" \t"));
          const size_t LastNameCharacter = Name.find_last_not_of(" \t");
          if (LastNameCharacter == std::string::npos)
          {
               if (Error)
               {
                    *Error = "Empty HTTP header name";
               }
               return false;
          }
          Name.erase(LastNameCharacter + 1);
          std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (Name == "content-length")
          {
               ++ContentLengthCount;
               if (ContentLengthCount > 1)
               {
                    if (Error)
                    {
                         *Error = "Duplicate Content-Length headers are not allowed";
                    }
                    return false;
               }
          }
          else if (Name == "transfer-encoding")
          {
               if (Error)
               {
                    *Error = "Transfer-Encoding is not supported";
               }
               return false;
          }
     }

     return true;
}

/* Use HTTP status codes from core/httpcodes.h. */

using http_code = HttpCodes::code;

/* Bring StatusText function into scope. */

/* Access control logging helper. */

static void LogAccessControl(const std::string &Reason, const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("access_control", Reason + " (endpoint: " + Request.Path + ", method: " + Request.Method + ", remote: " + Request.RemoteAddress + ":" + std::to_string(Request.RemotePort) + ").");
     }
}

/* Implements the authorize HTTP request helper. */

static bool AuthorizeHttpRequest(HttpRequest &Request, HttpResponse &Response)
{
     if (!Instance || !Instance->Users)
     {
          Response = HttpResponse(http_code::INTERNAL_SERVER_ERROR, StatusText(http_code::INTERNAL_SERVER_ERROR), "application/json");
          Response.Body = "{\"error\":\"Authentication system not available\"}";
          return false;
     }

     UserAuthManager &AuthManager = *Instance->Users;

     std::string AuthHeader;
     std::string AuthToken;
     bool HasAuthToken = ExtractAuthTokenFromRequest(Request, AuthHeader, AuthToken);

     if (!AuthManager.IsAuthEnabled() && HasAuthToken && !IsAuthorizedReplicationRequest(Request) &&
         !IsHealthLikePath(Request.Path))
     {
          Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
          Response.Body = "{\"error\":\"Authentication is disabled\",\"message\":\"Tokens are not accepted when authentication is disabled. Remove the Authorization, X-API-Key, or X-TYPESENSE-API-KEY header.\"}";

          LogAccessControl("Forbidden: token provided while auth is disabled", Request);

          return false;
     }

     if (!AuthManager.IsAuthEnabled() || IsHealthLikePath(Request.Path))
     {
          return true;
     }

     if (!HasAuthToken)
     {
          Response = HttpResponse(http_code::UNAUTHORIZED, StatusText(http_code::UNAUTHORIZED), "application/json");
          Response.Body = "{\"error\":\"Authentication required\",\"message\":\"Missing Authorization, X-API-Key, or X-TYPESENSE-API-KEY header\"}";
          Response.Headers["WWW-Authenticate"] = "Bearer";

          LogAccessControl("Unauthorized: missing authentication token", Request);

          return false;
     }

     APIKey KeyObj;

     if (!APIKeyManager::Instance().ValidateKey(AuthToken, &KeyObj))
     {
          AuthResult AuthResultVal = AuthManager.AuthenticateRequest(AuthHeader);

          if (!AuthResultVal.Valid)
          {
               Response = HttpResponse(http_code::UNAUTHORIZED, StatusText(http_code::UNAUTHORIZED), "application/json");

               nlohmann::json AuthErrorJSON;
               AuthErrorJSON["error"] = "Authentication failed";
               AuthErrorJSON["message"] = AuthResultVal.ErrorMessage;
               Response.Body = AuthErrorJSON.dump();

               Response.Headers["WWW-Authenticate"] = "Bearer";

               LogAccessControl("Unauthorized: authentication failed (" + AuthResultVal.ErrorMessage + ")", Request);

               return false;
          }
     }

     Request.Authenticated = true;

     return true;
}

/* Returns a request header value using case-insensitive lookup. */

static std::string GetHeaderValueInsensitive(const std::map<std::string, std::string> &Headers, const std::string &Name)
{
     std::string LowerName = Name;
     std::transform(LowerName.begin(), LowerName.end(), LowerName.begin(), [](unsigned char c)
                    {
                         return static_cast<char>(std::tolower(c));
                    });

     for (const auto &Header : Headers)
     {
          std::string Key = Header.first;
          std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });
          if (Key == LowerName)
          {
               return Header.second;
          }
     }

     return "";
}

/* Checks whether health like path applies. */

static bool IsHealthLikePath(const std::string &Path)
{
     return Path == "/health" || Path == "/health/" ||
            Path == "/ready" || Path == "/ready/" ||
            Path == "/status" || Path == "/status/" ||
            Path == "/query" || Path == "/query/" ||
            Path == "/ping" || Path == "/ping/" ||
            Path == "/stats" || Path == "/stats/";
}

/* Checks whether document ingestion request applies. */

static bool IsDocumentIngestionRequest(const std::string &Method, const std::string &Path)
{
     return Method == "POST" &&
            Path.find("/collections/") == 0 &&
            Path.find("/documents") != std::string::npos &&
            Path.find("/documents/search") == std::string::npos &&
            Path.find("/documents/facet_counts") == std::string::npos &&
            Path.find("/documents/maybe") == std::string::npos &&
            Path.find("/documents/export") == std::string::npos;
}

/* Checks whether replication hop request applies. */

static bool IsReplicationHopRequest(const HttpRequest &Request)
{
     for (const auto &Header : Request.Headers)
     {
          std::string Key = Header.first;
          std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });
          if (Key != "x-hlq-replication-hop")
          {
               continue;
          }

          std::string Value = Header.second;
          std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });
          return Value == "1" || Value == "true";
     }

     return false;
}

/* Checks whether authorized replication request applies. */

static bool IsAuthorizedReplicationRequest(const HttpRequest &Request)
{
     if (!IsReplicationHopRequest(Request) || !Instance || !Instance->Config)
     {
          return false;
     }

     std::string AuthHeader;
     std::string AuthToken;

     if (!ExtractAuthTokenFromRequest(Request, AuthHeader, AuthToken) || AuthToken.empty())
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

/* Implements the record analytics for response helper. */

static void RecordAnalyticsForResponse(const HttpRequest &Request, const HttpResponse &Response, RouteAction ActionVal)
{
     FOREACH_MOD(OnRequestAnalytics, Request, Response, ActionVal);

     if (Request.Authenticated)
     {
          FOREACH_MOD(OnAuthenticatedRequest, Request, ActionVal);
     }
}

/* Implements the record analytics for response helper. */

static void RecordAnalyticsForResponse(const HttpRequest &Request, const HttpResponse &Response)
{
     RecordAnalyticsForResponse(Request, Response, ResolveRouteWithFallback(Request));
}

/* Checks whether mutating request method applies. */

static bool IsMutatingRequestMethod(const HttpRequest &Request)
{
     return Request.Method == "POST" || Request.Method == "PUT" || Request.Method == "DELETE" || Request.Method == "PATCH";
}

static std::atomic<uint64_t> BackpressureConnectionRejects{0};
static std::atomic<uint64_t> BackpressureQueueRejects{0};

/* Builds backpressure response data. */

static HttpResponse BuildBackpressureResponse(const std::string &Source, const std::string &Message)
{
     HttpResponse Response(503, "Service Unavailable", "application/json");
     Response.Headers["Retry-After"] = "2";

     nlohmann::json Body;
     Body["error"] = "server_overloaded";
     Body["source"] = Source;
     Body["message"] = Message;
     Response.Body = Body.dump();

     return Response;
}

/* Builds backpressure raw response data. */

static std::string BuildBackpressureRawResponse(const std::string &Source, const std::string &Message)
{
     nlohmann::json BodyJSON;
     BodyJSON["error"] = "server_overloaded";
     BodyJSON["source"] = Source;
     BodyJSON["message"] = Message;

     const std::string Body = BodyJSON.dump();
     std::string Response = "HTTP/1.1 503 Service Unavailable\r\n";
     Response += "Content-Type: application/json\r\n";
     Response += "Server: hlquery/1.0\r\n";
     Response += "Connection: close\r\n";
     Response += "Retry-After: 2\r\n";
     Response += "Content-Length: " + std::to_string(Body.size()) + "\r\n";
     Response += "\r\n";
     Response += Body;
     return Response;
}

/* Implements the trim HTTP field value helper. */

static std::string TrimHTTPFieldValue(const std::string &Value)
{
     size_t Start = Value.find_first_not_of(" \t");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Value.find_last_not_of(" \t");
     return Value.substr(Start, End - Start + 1);
}

/* Extracts auth token from request values. */

static bool ExtractAuthTokenFromRequest(const HttpRequest &Request, std::string &OutAuthHeader, std::string &OutToken)
{
     OutAuthHeader.clear();
     OutToken.clear();

     auto AuthIt = Request.Headers.find("Authorization");

     if (AuthIt == Request.Headers.end())
     {
          AuthIt = Request.Headers.find("authorization");
     }

     if (AuthIt != Request.Headers.end())
     {
          OutAuthHeader = TrimHTTPFieldValue(AuthIt->second);
     }
     else
     {
          auto APIKeyIt = Request.Headers.find("X-API-Key");

          if (APIKeyIt == Request.Headers.end())
          {
               APIKeyIt = Request.Headers.find("x-api-key");
          }

          if (APIKeyIt == Request.Headers.end())
          {
               APIKeyIt = Request.Headers.find("X-TYPESENSE-API-KEY");
          }

          if (APIKeyIt == Request.Headers.end())
          {
               APIKeyIt = Request.Headers.find("x-typesense-api-key");
          }

          if (APIKeyIt != Request.Headers.end())
          {
               std::string APIKey = TrimHTTPFieldValue(APIKeyIt->second);

               if (!APIKey.empty())
               {
                    OutAuthHeader = "Bearer " + APIKey;
               }
          }
     }

     OutToken = OutAuthHeader;

     if (OutToken.find("Bearer ") == 0)
     {
          OutToken = OutToken.substr(7);
     }

     OutToken = TrimHTTPFieldValue(OutToken);

     return !OutToken.empty();
}

static RouteAction ResolveRouteWithFallback(const HttpRequest &Request);
static bool IsPublicRouteAction(RouteAction ActionVal);
static bool IsAdminOnlyRouteAction(RouteAction ActionVal);
APIKeyAction MapRouteToKeyAction(RouteAction ActionVal);

/* HttpConnection implementation. */

HttpConnection::HttpConnection(int FDVal, const std::string &ClientIPVal, int ClientPortVal, SearchThreadPool *ThreadPool)
    : ClientIP(ClientIPVal), ClientPort(ClientPortVal), ResponseSentOffset(0), ResponsePending(false),
      KeepAlive(false), RequestsProcessed(0), LastActivity(Instance->Now()),
      ClosingValue(false), ThreadPoolValue(ThreadPool)
{
     SetFD(FDVal);

     /* Set advanced socket options for HTTP performance. */

     SetAdvancedSocketOptions(FDVal);

#ifdef HLQUERY_HAS_OPENSSL

     SSLValue = nullptr;

#endif
}

/* HttpConnection destructor. */

HttpConnection::~HttpConnection()
{
     /* Mark as closing during destruction. */

     ClosingValue.store(true);

#ifdef HLQUERY_HAS_OPENSSL

     if (SSLValue)
     {
          SSL_shutdown(SSLValue);
          SSL_free(SSLValue);
          SSLValue = nullptr;
     }

#endif

     /* Prevent use-after-free by checking and caching fd before operations. */

     int FDValue = -1;
     bool HasValidFD = false;

     {
          std::lock_guard<std::mutex> Lock(ResponseMutex);

          /* Cache fd value while holding lock to prevent race condition. */

          if (HasFD())
          {
               FDValue = GetFD();
               HasValidFD = (FDValue >= 0);
          }

          /*
           * On Close(), drain pending write buffer before closing.
           * Attempt to send any pending response data before closing connection.
           */

          if (ResponsePending && !ResponseBuffer.empty() && HasValidFD)
          {
               /*
                * Try to send remaining data synchronously (non-blocking).
                * This ensures we don't lose response data when connection closes.
                */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "HttpConnection destructor: Attempting to drain " + std::to_string(ResponseBuffer.size() - ResponseSentOffset) + " bytes from write buffer.");
               }
          }
     }

     /*
      * Clean up connection resources using cached fd value.
      * Use cached fd to prevent use-after-free if SocketEngine::DelFD() invalidates handler.
      */

     if (HasValidFD && FDValue >= 0)
     {
          /* Remove from socket engine first (may invalidate handler). */

          SocketEngine::DelFD(this);

          /* Then close the file descriptor. */

          close(FDValue);

          /* Mark as invalid. */

          SetFD(-1);
     }
}

/* OnEventHandlerRead handles read events from the socket engine. */

void HttpConnection::OnEventHandlerRead()
{
     auto NowVal = Instance->Now();
     auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(NowVal - LastActivity).count();

     if (Elapsed > HTTP_CONNECTION_TIMEOUT_SECONDS)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http", "Connection timeout after " + std::to_string(Elapsed) + "s - closing socket.");
          }

          int FDValue = GetFD();
          ClosingValue.store(true);
          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);
          }

          SetFD(-1);
          return;
     }

     size_t BufferSize = RequestBuffer.size();

     if (BufferSize > HTTP_MAX_REQUEST_SIZE * 0.8)
     {
          auto StrictElapsed = std::chrono::duration_cast<std::chrono::seconds>(NowVal - LastActivity).count();

          if (StrictElapsed > 10)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http", "Connection timeout (progressive) after " + std::to_string(StrictElapsed) + "s with large buffer - closing socket.");
               }

               int FDValue = GetFD();
               ClosingValue.store(true);
               SocketEngine::DelFD(this);

               if (FDValue >= 0)
               {
                    close(FDValue);
               }

               SetFD(-1);
               return;
          }
     }

     if (RequestBuffer.size() >= HTTP_MAX_REQUEST_SIZE)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http", "Request buffer too large: " + std::to_string(RequestBuffer.size()) + " bytes - REJECTING!.");
          }

          HttpResponse ErrorResp;
          ErrorResp.StatusCode = 413;
          ErrorResp.StatusText = "Payload Too Large";
          ErrorResp.Body = "{\"error\":\"Request too large\"}";

          SendResponse(ErrorResp);

          int FDValue = GetFD();
          ClosingValue.store(true);
          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);
          }

          SetFD(-1);
          return;
     }

     char Buffer[HTTP_READ_BUFFER_SIZE];
     ssize_t BytesRead = 0;

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLValue && !SSLHandshaked)
     {
          int Ret = SSL_accept(SSLValue);

          if (Ret <= 0)
          {
               int Err = SSL_get_error(SSLValue, Ret);

               if (Err == SSL_ERROR_WANT_READ)
               {
                    return;
               }
               else if (Err == SSL_ERROR_WANT_WRITE)
               {
                    SocketEngine::RegisterPendingWrite(this);
                    return;
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http", "SSL handshake failed (errno=" + std::to_string(Err) + ") - closing socket.");
                    }

                    int FDValue = GetFD();
                    ClosingValue.store(true);
                    SocketEngine::DelFD(this);

                    if (FDValue >= 0)
                    {
                         close(FDValue);
                    }

                    SetFD(-1);
                    return;
               }
          }

          SSLHandshaked = true;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http", "SSL handshake successful for " + ClientIP + ".");
          }
     }

     if (SSLValue)
     {
          BytesRead = SSL_read(SSLValue, Buffer, sizeof(Buffer) - 1);
     }
     else
#endif
     {
          BytesRead = recv(GetFD(), Buffer, sizeof(Buffer) - 1, MSG_DONTWAIT);
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "OnEventHandlerRead: read returned " + std::to_string(BytesRead) + " bytes (errno=" + (BytesRead < 0 ? std::string(strerror(errno)) : "0") + ", fd=" + std::to_string(GetFD()) + ").");
     }

     while (true)
     {
          if (BytesRead <= 0)
          {
#ifdef HLQUERY_HAS_OPENSSL
               if (SSLValue)
               {
                    int Err = SSL_get_error(SSLValue, BytesRead);

                    if (Err == SSL_ERROR_WANT_READ || Err == SSL_ERROR_WANT_WRITE)
                    {
                         break;
                    }

                    if (Err == SSL_ERROR_ZERO_RETURN || Err == SSL_ERROR_SYSCALL)
                    {
                         int FDValue = GetFD();
                         ClosingValue.store(true);
                         SocketEngine::DelFD(this);

                         if (FDValue >= 0)
                         {
                              close(FDValue);
                         }

                         SetFD(-1);
                         return;
                    }
               }
               else
#endif
               {
                    if (BytesRead == 0 || errno == ECONNRESET)
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("http_server", "OnEventHandlerRead: Client disconnected (fd=" + std::to_string(GetFD()) + ", bytes_read=" + std::to_string(BytesRead) + ").");
                         }

                         int FDValue = GetFD();
                         ClosingValue.store(true);
                         SocketEngine::DelFD(this);

                         if (FDValue >= 0)
                         {
                              close(FDValue);
                         }

                         SetFD(-1);
                         return;
                    }

                    if (errno == EAGAIN)
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("http_server", "OnEventHandlerRead: EAGAIN - all data read (fd=" + std::to_string(GetFD()) + ").");
                         }
                         break;
                    }
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "OnEventHandlerRead: read error, calling OnEventHandlerError (fd=" + std::to_string(GetFD()) + ").");
               }

               OnEventHandlerError(errno);

               return;
          }

          if (BytesRead > 0 && BytesRead < static_cast<ssize_t>(sizeof(Buffer)))
          {
               Buffer[BytesRead] = '\0';
          }

          if (RequestBuffer.size() > HTTP_MAX_REQUEST_SIZE - static_cast<size_t>(BytesRead))
          {
               int FDValue = GetFD();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server", "Request buffer overflow - closing connection (fd=" + std::to_string(FDValue) + ").");
               }

               ClosingValue.store(true);
               SocketEngine::DelFD(this);

               if (FDValue >= 0)
               {
                    close(FDValue);
               }

               SetFD(-1);
               return;
          }

          if (RequestBuffer.empty() && BytesRead > 0)
          {
               RequestBuffer.reserve(16384);
          }

          if (RequestBuffer.capacity() < RequestBuffer.size() + static_cast<size_t>(BytesRead))
          {
               size_t NewCapacity = std::max(
                    RequestBuffer.capacity() * 2,
                    RequestBuffer.size() + static_cast<size_t>(BytesRead) + 16384);
               RequestBuffer.reserve(NewCapacity);
          }

          RequestBuffer.append(Buffer, BytesRead);
          SocketEngine::IncrementBytesProcessed(static_cast<uint64_t>(BytesRead));

#ifdef HLQUERY_HAS_OPENSSL

          if (SSLValue)
          {
               BytesRead = SSL_read(SSLValue, Buffer, sizeof(Buffer) - 1);
          }
          else
#endif
          {
               BytesRead = recv(GetFD(), Buffer, sizeof(Buffer) - 1, MSG_DONTWAIT);
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "OnEventHandlerRead: Added data to buffer (total_size=" + std::to_string(RequestBuffer.size()) + ").");
     }

     size_t HeaderEnd = RequestBuffer.find("\r\n\r\n");

     if (HeaderEnd != std::string::npos)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "OnEventHandlerRead: Found complete headers at position " + std::to_string(HeaderEnd) + ".");
          }

          std::string HeadersPart = RequestBuffer.substr(0, HeaderEnd + 4);

          if (HeadersPart.size() > HTTP_MAX_HEADER_SIZE)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http", "Header size too large: " + std::to_string(HeadersPart.size()) + " bytes (max=" + std::to_string(HTTP_MAX_HEADER_SIZE) + ") - REJECTING!.");
               }

               HttpResponse ErrorResp;
               ErrorResp.StatusCode = 431;
               ErrorResp.StatusText = "Request Header Fields Too Large";
               ErrorResp.Body = "{\"error\":\"Header size too large\"}";

               SendResponse(ErrorResp);

               int FDValue = GetFD();
               ClosingValue.store(true);
               SocketEngine::DelFD(this);

               if (FDValue >= 0)
               {
                    close(FDValue);
               }

               SetFD(-1);
               return;
          }

          std::string FramingError;
          if (!HasValidRequestFramingHeaders(HeadersPart, &FramingError))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http", "Invalid request framing: " + FramingError + ".");
               }

               HttpResponse ErrorResp(400, "Bad Request", "application/json");
               ErrorResp.Body = "{\"error\":\"Invalid request framing\"}";
               KeepAlive = false;
               SendResponse(ErrorResp);
               return;
          }

          std::string HeadersLower = HeadersPart;
          std::transform(HeadersLower.begin(), HeadersLower.end(), HeadersLower.begin(), ::tolower);

          size_t ContentLengthPos = HeadersLower.find("content-length:");

          if (ContentLengthPos != std::string::npos)
          {
               size_t ColonPos = HeadersPart.find(':', ContentLengthPos);
               size_t ValueStart = std::string::npos;

               if (ColonPos != std::string::npos)
               {
                    ValueStart = HeadersPart.find_first_not_of(" \t", ColonPos + 1);
               }

               if (ValueStart != std::string::npos)
               {
                    size_t ValueEnd = HeadersPart.find_first_of("\r\n", ValueStart);

                    if (ValueEnd != std::string::npos)
                    {
                         std::string LengthStr = HeadersPart.substr(ValueStart, ValueEnd - ValueStart);
                         size_t LengthEnd = LengthStr.find_last_not_of(" \t");
                         if (LengthEnd != std::string::npos)
                         {
                              LengthStr.erase(LengthEnd + 1);
                         }

                         try
                         {
                              if (LengthStr.length() > 20)
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("http", "Content-Length string too long: " + LengthStr + " - REJECTING!.");
                                   }

                                   HttpResponse ErrorResp;
                                   ErrorResp.StatusCode = 413;
                                   ErrorResp.StatusText = "Payload Too Large";
                                   ErrorResp.Body = "{\"error\":\"Content-Length string too long\"}";

                                   SendResponse(ErrorResp);

                                   int FDValue = GetFD();
                                   ClosingValue.store(true);
                                   SocketEngine::DelFD(this);

                                   if (FDValue >= 0)
                                   {
                                        close(FDValue);
                                   }

                                   SetFD(-1);
                                   return;
                              }

                              if (LengthStr.empty() || !std::all_of(LengthStr.begin(), LengthStr.end(), ::isdigit))
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("http", "Content-Length contains non-numeric characters: '" + LengthStr + "' - REJECTING!.");
                                   }

                                   HttpResponse ErrorResp;
                                   ErrorResp.StatusCode = 400;
                                   ErrorResp.StatusText = "Bad Request";
                                   ErrorResp.Body = "{\"error\":\"Invalid Content-Length format\"}";

                                   SendResponse(ErrorResp);

                                   int FDValue = GetFD();
                                   ClosingValue.store(true);
                                   SocketEngine::DelFD(this);

                                   if (FDValue >= 0)
                                   {
                                        close(FDValue);
                                   }

                                   SetFD(-1);
                                   return;
                              }

                              unsigned long long ParsedLength = 0;
                              auto [ptr, ec] = std::from_chars(LengthStr.data(), LengthStr.data() + LengthStr.size(), ParsedLength);

                              if (ec != std::errc())
                              {
                                   throw std::runtime_error("Invalid Content-Length format");
                              }

                              const char *EndPtr = LengthStr.data() + LengthStr.size();

                              if (ptr != EndPtr)
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("http", "Content-Length partial parse - trailing garbage: '" + std::string(ptr, EndPtr) + "' in '" + LengthStr + "'.");
                                   }
                                   throw std::runtime_error("Content-Length contains invalid trailing characters");
                              }

                              size_t BodyStart = HeaderEnd + 4;

                              if (ParsedLength > HTTP_MAX_REQUEST_SIZE)
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("http", "Content-Length too large: " + LengthStr + " - REJECTING!.");
                                   }

                                   HttpResponse ErrorResp;
                                   ErrorResp.StatusCode = 413;
                                   ErrorResp.StatusText = "Payload Too Large";
                                   ErrorResp.Body = "{\"error\":\"Content-Length too large\"}";

                                   SendResponse(ErrorResp);

                                   int FDValue = GetFD();
                                   ClosingValue.store(true);
                                   SocketEngine::DelFD(this);

                                   if (FDValue >= 0)
                                   {
                                        close(FDValue);
                                   }

                                   SetFD(-1);
                                   return;
                              }

                              if (BodyStart > SIZE_MAX || ParsedLength > SIZE_MAX - BodyStart)
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("http", "Content-Length overflow: " + LengthStr + " - REJECTING!.");
                                   }

                                   HttpResponse ErrorResp;
                                   ErrorResp.StatusCode = 413;
                                   ErrorResp.StatusText = "Payload Too Large";
                                   ErrorResp.Body = "{\"error\":\"Content-Length overflow\"}";

                                   SendResponse(ErrorResp);

                                   int FDValue = GetFD();
                                   ClosingValue.store(true);
                                   SocketEngine::DelFD(this);

                                   if (FDValue >= 0)
                                   {
                                        close(FDValue);
                                   }

                                   SetFD(-1);
                                   return;
                              }

                              size_t ContentLength = static_cast<size_t>(ParsedLength);
                              size_t BodyReceived = RequestBuffer.size() - BodyStart;

                              if (BodyReceived >= ContentLength)
                              {
                                   if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                   {
                                        Instance->Logs->Debug("http_server", "OnEventHandlerRead: Complete request received (body_received=" + std::to_string(BodyReceived) + ", content_length=" + std::to_string(ContentLength) + "), calling ProcessMultipleRequests().");
                                   }
                                   ProcessMultipleRequests();
                              }
                              else
                              {
                                   if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                   {
                                        Instance->Logs->Debug("http_server", "OnEventHandlerRead: Waiting for more body data (body_received=" + std::to_string(BodyReceived) + ", content_length=" + std::to_string(ContentLength) + ").");
                                   }
                              }
                         }
                         catch (...)
                         {
                              ProcessMultipleRequests();
                         }
                    }
               }
               else
               {
                    /* ValueStart not found. */

                    ProcessMultipleRequests();
               }
          }
          else
          {
               /* No Content-Length header, request is complete after headers */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "OnEventHandlerRead: No Content-Length, request complete after headers, calling ProcessMultipleRequests().");
               }

               ProcessMultipleRequests();
          }
     }
     else
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "OnEventHandlerRead: Headers not complete yet (buffer_size=" + std::to_string(RequestBuffer.size()) + ").");
          }
     }
}

/* Handle write events. */

void HttpConnection::OnEventHandlerWrite()
{
     /* Check ClosingValue flag first to prevent use-after-free. */

     if (ClosingValue.load())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "OnEventHandlerWrite: Connection is closing, skipping write (fd=" + std::to_string(GetFD()) + ").");
          }

          return;
     }

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLValue && !SSLHandshaked)
     {
          OnEventHandlerRead();

          return;
     }
#endif

     /* Validate file descriptor is still valid before acquiring lock. */

     if (!HasFD() || GetFD() < 0)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "OnEventHandlerWrite: Invalid FD, marking as closing (fd=" + std::to_string(GetFD()) + ").");
          }

          ClosingValue.store(true);

          return;
     }

     /*
     * CRITICAL FIX: Minimize mutex hold time - only lock to copy buffer state.
     * This prevents deadlocks when SendResponse is called while OnEventHandlerWrite is running.
     */

     std::string BufferCopy;

     size_t OffsetCopy = 0;

     uint64_t SerialCopy = 0;

     bool ContinueWriting = false;

     {
          std::lock_guard<std::mutex> Lock(ResponseMutex);

          /* Check ClosingValue again after acquiring lock. */

          if (ClosingValue.load() || !ResponsePending || ResponseBuffer.empty())
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "OnEventHandlerWrite: Skipping write - closing=" + std::string(ClosingValue.load() ? "true" : "false") + ", pending=" + std::string(ResponsePending ? "true" : "false") + ", buffer_empty=" + std::string(ResponseBuffer.empty() ? "true" : "false") + ".");
               }

               return;
          }

          /* Ensure offset is valid. */

          if (ResponseSentOffset >= ResponseBuffer.size())
          {
               if (!ResponseQueue.empty())
               {
                    ResponseBuffer = std::move(ResponseQueue.front());
                    ResponseQueue.pop_front();
                    ResponseSentOffset = 0;
                    ResponsePending = true;
                    ResponseSerial++;
               }
               else
               {
                    ResponseBuffer.clear();
                    ResponseSentOffset = 0;
                    ResponsePending = false;
                    SocketEngine::UnregisterPendingWrite(this);

                    return;
               }
          }

          /* Copy buffer size and offset while holding lock (minimal time). */
          /* We'll work with a copy to avoid holding lock during send operations. */

          OffsetCopy = ResponseSentOffset;
          SerialCopy = ResponseSerial;

          /* Copy buffer data - for large responses this is expensive but necessary. */
          /* to avoid holding lock during send. Alternative would be reference counting. */

          BufferCopy = ResponseBuffer;
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "OnEventHandlerWrite: Sending " + std::to_string(BufferCopy.size()) + " bytes from offset " + std::to_string(OffsetCopy) + " to fd=" + std::to_string(GetFD()) + ".");
     }

     /* IMPROVEMENT: Use edge-triggered epoll correctly: write in a loop until EAGAIN. */
     /* to ensure all data is sent (preventing missed events from stalling communication). */

     size_t CurrentOffset = OffsetCopy;

     while (CurrentOffset < BufferCopy.size())
     {
          /* Use MSG_NOSIGNAL for better performance. */

          const char *SendPtr = BufferCopy.data() + CurrentOffset;

          size_t SendLen = BufferCopy.size() - CurrentOffset;

          ssize_t BytesSent = 0;

#ifdef HLQUERY_HAS_OPENSSL
          if (SSLValue)
          {
               BytesSent = SSL_write(SSLValue, SendPtr, SendLen);
          }
          else
#endif
          {
               BytesSent = send(GetFD(), SendPtr, SendLen, MSG_NOSIGNAL);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "OnEventHandlerWrite: send returned " + std::to_string(BytesSent) + " bytes (errno=" + (BytesSent < 0 ? std::string(strerror(errno)) : "0") + ").");
          }

          if (BytesSent > 0)
          {
               /* Increment bytes processed counter. */

               SocketEngine::IncrementBytesProcessed(static_cast<uint64_t>(BytesSent));

               CurrentOffset += BytesSent;

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "OnEventHandlerWrite: Sent " + std::to_string(BytesSent) + " bytes, remaining=" + std::to_string(BufferCopy.size() - CurrentOffset) + " bytes (fd=" + std::to_string(GetFD()) + ").");
               }
          }
          else if (BytesSent < 0)
          {
#ifdef HLQUERY_HAS_OPENSSL
               int Err = SSLValue ? SSL_get_error(SSLValue, BytesSent) : 0;

               if (SSLValue && (Err == SSL_ERROR_WANT_READ || Err == SSL_ERROR_WANT_WRITE))
               {
                    /* Update offset and wait for next event. */

                    std::lock_guard<std::mutex> Lock(ResponseMutex);

                    if (ResponseSerial == SerialCopy)
                    {
                         ResponseSentOffset = CurrentOffset;
                    }

                    return;
               }
#endif
               if (errno == EAGAIN)
               {
                    /* Socket buffer full - edge-triggered mode: we've sent all we can for now. */
                    /* Will retry when socket becomes writable again. */

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("http_server", "OnEventHandlerWrite: EAGAIN - socket buffer full, will retry (fd=" + std::to_string(GetFD()) + ").");
                    }

                    /* Update offset while holding lock briefly. */

                    {
                         std::lock_guard<std::mutex> Lock(ResponseMutex);

                         if (ResponseSerial == SerialCopy)
                         {
                              ResponseSentOffset = CurrentOffset;
                         }
                    }

                    /* Keep ResponsePending = true so we retry later. */

                    return;
               }
               else
               {
                    /* Send error - close connection. */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("http_server", "OnEventHandlerWrite: send() error: " + std::string(strerror(errno)) + " (fd=" + std::to_string(GetFD()) + ").");
                    }

                    /* FIX: Cache fd before DelFD() to prevent use-after-free. */

                    int FDValue = GetFD();

                    ClosingValue.store(true);

                    SocketEngine::DelFD(this);

                    if (FDValue >= 0)
                    {
                         close(FDValue);
                         SetFD(-1);
                    }

                    return;
               }
          }
          else
          {
               /* bytes_sent == 0 - connection closed. */

               /* FIX: Cache fd before DelFD() to prevent use-after-free. */

               int FDValue = GetFD();

               ClosingValue.store(true);

               SocketEngine::DelFD(this);

               if (FDValue >= 0)
               {
                    close(FDValue);
                    SetFD(-1);
               }

               return;
          }
     }

     /* All data sent - update state while holding lock. */

     {
          std::lock_guard<std::mutex> Lock(ResponseMutex);

          if (ResponseSerial == SerialCopy)
          {
               if (!ResponseQueue.empty())
               {
                    ResponseBuffer = std::move(ResponseQueue.front());
                    ResponseQueue.pop_front();
                    ResponseSentOffset = 0;
                    ResponsePending = true;
                    ResponseSerial++;
                    ContinueWriting = true;
               }
               else
               {
                    ResponseBuffer.clear();
                    ResponseSentOffset = 0;
                    ResponsePending = false;
               }
          }

          if (!ResponsePending)
          {
               SocketEngine::UnregisterPendingWrite(this);
          }
     }

     if (ContinueWriting)
     {
          SocketEngine::RegisterPendingWrite(this);
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Normal("http_server", "[OnEventHandlerWrite] Response complete, unregistered write (fd=" + std::to_string(GetFD()) + ", keep_alive=" + std::string(KeepAlive ? "true" : "false") + ").");
     }

     /* Don't close connection if keep-alive! */
     /* Only close if client requested Connection: close. */

     if (!KeepAlive)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Normal("http_server", "[OnEventHandlerWrite] Closing connection (keep-alive=false, fd=" + std::to_string(GetFD()) + ").");
          }

          /* FIX: Cache fd before DelFD() to prevent use-after-free. */

          int FDValue = GetFD();

          ClosingValue.store(true);

          /* Use atomic flag to prevent race condition. */
          /* DelFD may access this object, so set ClosingValue first. */

          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);

               SetFD(-1);

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Normal("http_server", "[OnEventHandlerWrite] Connection closed and fd set to -1.");
               }
          }
     }
     else
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Normal("http_server", "[OnEventHandlerWrite] Keeping connection alive (fd=" + std::to_string(GetFD()) + ").");
          }

          /* Keep connection alive for reuse during benchmarks!. */
     }
}

/* Handle error events. */

void HttpConnection::OnEventHandlerError(int ErrorNum)
{
     /* FIX: Cache fd before DelFD() to prevent use-after-free. */

     int FDValue = GetFD();

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "OnEventHandlerError: ENTRY (errno=" + std::string(strerror(ErrorNum)) + ", fd=" + std::to_string(FDValue) + ", closing=" + std::string(ClosingValue.load() ? "true" : "false") + ").");
     }

     /* Mark as closing on error. */

     ClosingValue.store(true);

     SocketEngine::DelFD(this);

     if (FDValue >= 0)
     {
          close(FDValue);
     }

     SetFD(-1);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "OnEventHandlerError: Connection closed and cleaned up (errno=" + std::string(strerror(ErrorNum)) + ").");
     }
}

/* Process HTTP request. */

void HttpConnection::ProcessRequest()
{
     HttpRequest Request;

     if (!ParseHttpRequest(RequestBuffer, Request))
     {
          /* Log the problematic request for debugging. */

          if (Instance && Instance->Logs)
          {
               std::string RequestPreview = RequestBuffer.substr(0, std::min(RequestBuffer.size(), size_t(500)));

               Instance->Logs->Normal("http_server", "Failed to parse HTTP request. Preview (first 500 chars): " + RequestPreview + ".");

               ConsoleWriter::WriteError("Failed to parse HTTP request.");

               Instance->Logs->Normal("http_server", "Request buffer size: " + std::to_string(RequestBuffer.size()) + ".");
          }

          /* Example: Using HttpResponseBuilder for cleaner error responses. */

          HttpResponse Response = HttpResponseBuilder::BadRequest("Failed to parse HTTP request.")
                                       .Add("error", "Invalid HTTP request")
                                       .Build();

          SendResponse(Response);

          return;
     }

     Request.RemoteAddress = ClientIP;
     Request.RemotePort = ClientPort;

     /* Handle CORS preflight (OPTIONS) requests. */

     if (Request.Method == "OPTIONS")
     {
          HttpResponse Response(200, "OK");

          Response.Headers["Content-Type"] = "text/plain";
          Response.Body = "";

          SendResponse(Response);

          return;
     }

     HttpResponse AuthResponse;

     if (!AuthorizeHttpRequest(Request, AuthResponse))
     {
          SendResponse(AuthResponse);
          return;
     }

     /* Route to search API handlers. */

     SearchAPI &API = SearchAPI::GetInstance();

     HttpResponse ResponseVal = BuildRouteNotFoundResponse(Request.Path);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[ProcessRequest] About to call ProcessRequestWithAPI for: " + Request.Method + " " + Request.Path + ".");
     }

     if (IsMutatingRequestMethod(Request))
     {
          const std::string Operation = Request.Method + " " + Request.Path;
          HttpResponse DedupResponse = API.CheckReplicationOperationDedup(Request, Operation);
          if (DedupResponse.StatusCode != 0)
          {
               RecordAnalyticsForResponse(Request, DedupResponse);
               SendResponse(DedupResponse);
               return;
          }

          HttpResponse ReadOnlyResponse = API.CheckReadOnlyMode(Request, Operation);
          if (ReadOnlyResponse.StatusCode != 0)
          {
               RecordAnalyticsForResponse(Request, ReadOnlyResponse);
               SendResponse(ReadOnlyResponse);
               return;
          }
     }

     ResponseVal = ProcessRequestWithAPI(API, Request);

     RecordAnalyticsForResponse(Request, ResponseVal);
     API.FinalizeReplicationOperation(Request, ResponseVal);
     API.FinalizeReplicationResyncRequest(Request, ResponseVal);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[ProcessRequest] ProcessRequestWithAPI returned status " + std::to_string(ResponseVal.StatusCode) + ", body size: " + std::to_string(ResponseVal.Body.size()) + ".");
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[ProcessRequest] About to call SendResponse.");
     }

     SendResponse(ResponseVal);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[ProcessRequest] SendResponse completed, clearing request buffer.");
     }

     RequestBuffer.clear();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[ProcessRequest] EXIT - Request processed successfully.");
     }
}

/* SetAdvancedSocketOptions sets advanced socket options for performance. */

/* Updates set advanced socket options values. */

void HttpConnection::SetAdvancedSocketOptions(int FDValue)
{
     /* Enable TCP_NODELAY for low latency (disable Nagle's algorithm). */

     int Flag = 1;

     if (setsockopt(FDValue, IPPROTO_TCP, TCP_NODELAY, &Flag, sizeof(Flag)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("http_server", "Failed to set TCP_NODELAY: " + std::string(strerror(errno)) + ".");
          }
     }

     /* NOTE: SO_REUSEPORT should only be set on listening server sockets, not client connections. */
     /* Removed incorrect SO_REUSEPORT setting on client sockets. */

     /* Set large send/receive buffers for high throughput. */

     int BufSize = HLQUERY_SOCKET_BUFFER_SIZE;

     setsockopt(FDValue, SOL_SOCKET, SO_SNDBUF, &BufSize, sizeof(BufSize));
     setsockopt(FDValue, SOL_SOCKET, SO_RCVBUF, &BufSize, sizeof(BufSize));

     /* Enable TCP_QUICKACK for immediate ACK sending. */

     Flag = 1;

#ifdef TCP_QUICKACK
     if (setsockopt(FDValue, IPPROTO_TCP, TCP_QUICKACK, &Flag, sizeof(Flag)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("http_server", "Failed to set TCP_QUICKACK: " + std::string(strerror(errno)) + ".");
          }
     }
#endif

     /* Set socket to non-blocking mode. */

     int Flags = fcntl(FDValue, F_GETFL, 0);

     if (Flags >= 0)
     {
          fcntl(FDValue, F_SETFL, Flags | O_NONBLOCK);
     }
}

/* ProcessMultipleRequests processes multiple HTTP requests from buffer. */

void HttpConnection::ProcessMultipleRequests()
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "ProcessMultipleRequests: ENTRY (buffer_size=" + std::to_string(RequestBuffer.size()) + ", fd=" + std::to_string(GetFD()) + ").");
     }

     /* Process all complete HTTP requests in the buffer (with proper Content-Length handling). */

     int RequestsProcessedVal = 0;
     int RequestsDispatchedAsync = 0;

     while (true)
     {
          size_t HeaderEnd = RequestBuffer.find("\r\n\r\n");

          if (HeaderEnd == std::string::npos)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessMultipleRequests: No complete headers found, breaking (processed=" + std::to_string(RequestsProcessedVal) + " requests).");
               }

               break;
          }

          /* Extract headers to check for Content-Length (case-insensitive). */

          std::string HeadersPart = RequestBuffer.substr(0, HeaderEnd + 4);

          /* Limit header size to prevent DoS from massive headers. */

          if (HeadersPart.size() > HTTP_MAX_HEADER_SIZE)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http", "Header size too large: " + std::to_string(HeadersPart.size()) + " bytes (max=" + std::to_string(HTTP_MAX_HEADER_SIZE) + ") - REJECTING!.");
               }

               HttpResponse ErrorResp;

               ErrorResp.StatusCode = 431;
               ErrorResp.StatusText = "Request Header Fields Too Large";
               ErrorResp.Body = "{\"error\":\"Header size too large\"}";

               KeepAlive = false;
               SendResponse(ErrorResp);

               return;
          }

          std::string FramingError;
          if (!HasValidRequestFramingHeaders(HeadersPart, &FramingError))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http", "Invalid pipelined request framing: " + FramingError + ".");
               }

               HttpResponse ErrorResp(400, "Bad Request", "application/json");
               ErrorResp.Body = "{\"error\":\"Invalid request framing\"}";
               KeepAlive = false;
               SendResponse(ErrorResp);
               return;
          }

          std::string HeadersLower = HeadersPart;

          std::transform(HeadersLower.begin(), HeadersLower.end(), HeadersLower.begin(), ::tolower);

          size_t ContentLengthPos = HeadersLower.find("content-length:");

          size_t BodyStart = HeaderEnd + 4;

          size_t BodyLength = 0;

          if (ContentLengthPos != std::string::npos)
          {
               /* Extract Content-Length value. */

               size_t ColonPos = HeadersPart.find(':', ContentLengthPos);

               size_t ValueStart = std::string::npos;

               if (ColonPos != std::string::npos)
               {
                    ValueStart = HeadersPart.find_first_not_of(" \t", ColonPos + 1);
               }

               if (ValueStart != std::string::npos)
               {
                    size_t ValueEnd = HeadersPart.find_first_of("\r\n", ValueStart);

                    if (ValueEnd != std::string::npos)
                    {
                         std::string LengthStr = HeadersPart.substr(ValueStart, ValueEnd - ValueStart);

                         unsigned long long ParsedLength = 0;
                         auto [ParsedPtr, ParsedEC] = std::from_chars(LengthStr.data(), LengthStr.data() + LengthStr.size(), ParsedLength);

                         if (ParsedEC != std::errc() || ParsedPtr != LengthStr.data() + LengthStr.size())
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("http", "Invalid Content-Length in pipelined request: '" + LengthStr + "' - REJECTING!.");
                              }

                              HttpResponse ErrorResp;
                              ErrorResp.StatusCode = 400;
                              ErrorResp.StatusText = "Bad Request";
                              ErrorResp.Body = "{\"error\":\"Invalid Content-Length format\"}";

                              KeepAlive = false;
                              SendResponse(ErrorResp);

                              return;
                         }

                         /* Prevent integer overflow - validate Content-Length. */

                         if (ParsedLength > HTTP_MAX_REQUEST_SIZE)
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("http", "Content-Length too large: " + LengthStr + " - REJECTING!.");
                              }

                              /* Reject request with 413 Payload Too Large. */

                              HttpResponse ErrorResp;

                              ErrorResp.StatusCode = 413;
                              ErrorResp.StatusText = "Payload Too Large";
                              ErrorResp.Body = "{\"error\":\"Content-Length too large\"}";

                              KeepAlive = false;
                              SendResponse(ErrorResp);

                              return;
                         }

                         /* Check for overflow in addition: body_start + body_length. */

                         if (ParsedLength > SIZE_MAX - BodyStart)
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("http", "Content-Length overflow: " + LengthStr + " - REJECTING!.");
                              }

                              HttpResponse ErrorResp;

                              ErrorResp.StatusCode = 413;
                              ErrorResp.StatusText = "Payload Too Large";
                              ErrorResp.Body = "{\"error\":\"Content-Length overflow\"}";

                              KeepAlive = false;
                              SendResponse(ErrorResp);

                              return;
                         }

                         BodyLength = static_cast<size_t>(ParsedLength);
                    }
               }
          }

          /* Check if we have complete request (headers + body if present). */
          /* Safe addition after overflow check. */

          size_t TotalExpected = BodyStart + BodyLength;

          if (RequestBuffer.size() < TotalExpected)
          {
               break;
          }

          /* Extract the complete request (headers + body). */

          std::string RequestStr = RequestBuffer.substr(0, TotalExpected);

          RequestBuffer.erase(0, TotalExpected);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessMultipleRequests: Extracted request (size=" + std::to_string(RequestStr.size()) + ", remaining_buffer=" + std::to_string(RequestBuffer.size()) + "), calling ProcessSingleRequest().");
          }

          /* Process the request through the configured HTTP pool when available. */

          if (ThreadPoolValue && ShouldUseAsyncHttpDispatch())
          {
               ActiveRequestTasks.fetch_add(1, std::memory_order_acq_rel);

               auto RequestFuture = ThreadPoolValue->Submit(
                    [this, RequestStr]()
                    {
                         struct RequestTaskGuard
                         {
                              HttpConnection *Connection;

                              ~RequestTaskGuard()
                              {
                                   Connection->ActiveRequestTasks.fetch_sub(1, std::memory_order_acq_rel);
                              }
                         } Guard{this};

                         ProcessSingleRequest(RequestStr);
                    });

               if (RequestFuture.valid())
               {
                    RequestsDispatchedAsync++;

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("http_server", "ProcessMultipleRequests: Request dispatched to HTTP pool; deferring local processed count until worker completes.");
                    }

                    break;
               }
               else
               {
                    ActiveRequestTasks.fetch_sub(1, std::memory_order_acq_rel);
                    const uint64_t RejectCount = BackpressureQueueRejects.fetch_add(1, std::memory_order_relaxed) + 1;

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "Backpressure: HTTP thread pool queue full. reject_count=" + std::to_string(RejectCount) + ".");
                    }

                    KeepAlive = false;
                    SendResponse(BuildBackpressureResponse("http_queue", "Request queue is full; retry shortly."));
                    break;
               }
          }
          else
          {
               if (ThreadPoolValue && !ShouldUseAsyncHttpDispatch() && Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessMultipleRequests: HTTP pool available but disabled via HLQUERY_HTTP_USE_THREAD_POOL; processing inline.");
               }

               ProcessSingleRequest(RequestStr);
          }

          RequestsProcessedVal++;

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessMultipleRequests: Processed request #" + std::to_string(RequestsProcessedVal) + " (keep_alive=" + std::string(KeepAlive ? "true" : "false") + ", RequestsProcessed=" + std::to_string(RequestsProcessed) + ", max=" + std::to_string(HTTP_MAX_REQUESTS_PER_CONNECTION) + ").");
          }

          /* Check if we should continue processing. */

          if (!KeepAlive || (RequestsProcessed >= HTTP_MAX_REQUESTS_PER_CONNECTION && RequestBuffer.empty()))
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessMultipleRequests: Stopping pipeline (keep_alive=" + std::string(KeepAlive ? "true" : "false") + ", max_reached=" + std::string(RequestsProcessed >= HTTP_MAX_REQUESTS_PER_CONNECTION ? "true" : "false") + ").");
               }

               break;
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "ProcessMultipleRequests: EXIT (processed_local=" + std::to_string(RequestsProcessedVal) + ", processed_total=" + std::to_string(RequestsProcessed) + ", dispatched_async=" + std::to_string(RequestsDispatchedAsync) + ", remaining_buffer=" + std::to_string(RequestBuffer.size()) + ").");
     }
}

/* Helper to map Request.Path and Method to APIKeyAction. */

APIKeyAction MapRequestToKeyAction(const HttpRequest &Request)
{
     std::string Path = Request.Path;
     std::string Method = Request.Method;

     if (Path.find("/search") != std::string::npos || Path.find("/facet_counts") != std::string::npos || Path.find("/export") != std::string::npos)
     {
          return APIKeyAction::SEARCH;
     }

     if (Path.find("/documents") != std::string::npos)
     {
          if (Method == "GET")
               return APIKeyAction::SEARCH;
          if (Method == "POST" && Path.find("/import") != std::string::npos)
               return APIKeyAction::IMPORT;
          if (Method == "POST")
               return APIKeyAction::CREATE;
          if (Method == "PUT")
               return APIKeyAction::UPDATE;
          if (Method == "DELETE")
               return APIKeyAction::DELETE;
     }

     if (Path.find("/collections") != std::string::npos)
     {
          if (Method == "GET")
               return APIKeyAction::COLLECTIONS_LIST;
          if (Method == "POST")
               return APIKeyAction::COLLECTIONS_CREATE;
          if (Method == "DELETE")
               return APIKeyAction::COLLECTIONS_DELETE;
     }

     return APIKeyAction::SEARCH;
}

/* ProcessSingleRequest processes single HTTP request. */

void HttpConnection::ProcessSingleRequest(const std::string &RequestStr)
{
     if (Instance && Instance->Logs)
     {
     }

     /* Check if connection is closing before processing. */

     if (ClosingValue.load())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: Connection is closing, skipping request.");
          }

          return;
     }

     HttpRequest Request;

     if (!ParseHttpRequest(RequestStr, Request))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: Failed to parse HTTP request (size: " + std::to_string(RequestStr.size()) + " bytes).");
          }

          HttpResponse Response(400, "Bad Request");

          Response.Body = "Invalid HTTP request";

          SendResponse(Response);

          return;
     }

     Request.RemoteAddress = ClientIP;
     Request.RemotePort = ClientPort;
     ActiveRequestID = GetHeaderValueInsensitive(Request.Headers, "X-Request-Id");
     Request.IsCancelled = [this]()
     {
          return ClosingValue.load(std::memory_order_acquire) || !HasFD();
     };

     HttpResponse Response;

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "ProcessSingleRequest: " + Request.Method + " " + Request.Path + " from " + ClientIP + ":" + std::to_string(ClientPort) + " (body_size: " + std::to_string(Request.Body.size()) + ").");
     }

     if (!ActiveRequestID.empty() && !IsHealthLikePath(Request.Path) && Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "request_id=" + ActiveRequestID + " method=" + Request.Method + " path=" + Request.Path + " remote=" + ClientIP + ":" + std::to_string(ClientPort) + ".");
     }

     /* Handle CORS preflight (OPTIONS) requests. */

     if (Request.Method == "OPTIONS")
     {
          Response = HttpResponse(200, "OK");

          Response.Headers["Content-Type"] = "text/plain";
          Response.Body = "";

          SendResponse(Response);

          return;
     }

     try
     {
          /* Block queries until collections are loaded after restart, but allow write operations. */
          /* This ensures queries return accurate results with all collections available. */
          /* Allow collection creation and document import during loading (needed for benchmarks). */

          bool IsAnyServerLoading = false;

          for (auto *ServerVal : Instance->HTTPServers)
          {
               if (ServerVal && ServerVal->IsLoading())
               {
                    IsAnyServerLoading = true;
                    break;
               }
          }

          if (Instance && IsAnyServerLoading)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessSingleRequest: Server is loading, checking if request is allowed: " + Request.Method + " " + Request.Path + ".");
               }

               /* Allow health check endpoints during loading. */

               bool IsHealthCheck = IsHealthLikePath(Request.Path);

               /* Allow collection creation during loading (needed for benchmarks). */

               bool IsCollectionCreation = (Request.Path == "/collections" && Request.Method == "POST");

               /* Allow document import during loading (needed for benchmarks). */

               bool IsDocumentImport = IsDocumentIngestionRequest(Request.Method, Request.Path);

               /* Block queries (GET requests) until collections are loaded after restart. */
               /* This ensures queries return accurate results with all collections available. */

               bool IsQuery = (Request.Method == "GET");

               /* Allow write operations (collection creation, document import) but block queries. */

               if (!IsHealthCheck && !IsCollectionCreation && !IsDocumentImport && IsQuery)
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("http_server", "ProcessSingleRequest: Blocking query during loading: " + Request.Method + " " + Request.Path + ".");
                    }

                    Response = HttpResponse(503, "Service Unavailable", "application/json");

                    Response.Body = "{\"error\":\"Server is loading data\",\"message\":\"Database is still loading collections after restart. Queries are blocked until all collections are loaded. Write operations (collection creation, document import) are allowed.\"}";
                    Response.Headers["Retry-After"] = "5";

                    SendResponse(Response);

                    return;
               }
               else if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessSingleRequest: Allowing request during loading: " + Request.Method + " ." + Request.Path + " (is_health=" + std::string(IsHealthCheck ? "true" : "false") + ", is_collection_creation=" + std::string(IsCollectionCreation ? "true" : "false") + ", is_document_import=" + std::string(IsDocumentImport ? "true" : "false") + ").");
               }
          }

          /* CRITICAL FIX: Block queries during database sync, but allow collection creation and document import. */
          /* Collection creation and document import are safe during sync and needed for benchmarks. */

          if (Instance && Instance->IsSyncInProgress())
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessSingleRequest: Database sync in progress, checking if request is allowed: " + Request.Method + " " + Request.Path + ".");
               }

               /* Allow health check endpoints during sync. */

               bool IsHealthCheck = IsHealthLikePath(Request.Path);

               /* Allow collection creation during sync - it's safe and needed for benchmarks. */

               bool IsCollectionCreation = (Request.Path == "/collections" && Request.Method == "POST");

               /* Allow document import during sync - it's safe and needed for benchmarks. */

               bool IsDocumentImport = IsDocumentIngestionRequest(Request.Method, Request.Path);

               /* Block queries and other write operations during sync. */

               bool IsQuery = (Request.Method == "GET");

               bool IsOtherWrite = (Request.Method == "POST" || Request.Method == "PUT" || Request.Method == "DELETE") &&
                                   !IsCollectionCreation && !IsDocumentImport;

               if (!IsAuthorizedReplicationRequest(Request) && !IsHealthCheck && (IsQuery || IsOtherWrite))
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("http_server", "ProcessSingleRequest: Blocking request during sync: " + Request.Method + " " + Request.Path + ".");
                    }

                    Response = HttpResponse(503, "Service Unavailable", "application/json");

                    Response.Body = "{\"error\":\"Database sync in progress\",\"message\":\"Database is currently syncing. Queries and most write operations are blocked until sync completes. Collection creation and document import are allowed.\"}";
                    Response.Headers["Retry-After"] = "5";

                    SendResponse(Response);

                    return;
               }
               else if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessSingleRequest: Allowing request during sync: " + Request.Method + " ." + Request.Path + " (is_health=" + std::string(IsHealthCheck ? "true" : "false") + ", is_collection_creation=" + std::string(IsCollectionCreation ? "true" : "false") + ", is_document_import=" + std::string(IsDocumentImport ? "true" : "false") + ").");
               }
          }

          if (!AuthorizeHttpRequest(Request, Response))
          {
               SendResponse(Response);
               return;
          }

          /* Check for keep-alive header. */

          KeepAlive = (Request.Version == "HTTP/1.1");

          auto ConnHeader = Request.Headers.find("connection");

          if (ConnHeader != Request.Headers.end())
          {
               std::string ConnectionValue = ConnHeader->second;
               std::transform(ConnectionValue.begin(), ConnectionValue.end(), ConnectionValue.begin(), [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (ConnectionValue == "close")
               {
                    KeepAlive = false;
               }
               else if (ConnectionValue == "keep-alive")
               {
                    KeepAlive = true;
               }
          }

          RequestsProcessed++;

          if (RequestsProcessed >= HTTP_MAX_REQUESTS_PER_CONNECTION && RequestBuffer.empty())
          {
               KeepAlive = false;
          }

          /* Route to search API handlers. */

          SearchAPI &API = SearchAPI::GetInstance();

          /* API Key and Admin permission check. */

          bool IsAdminVal = false;
          std::string TokenVal = "";

          auto AuthIt = Request.Headers.find("authorization");

          if (AuthIt != Request.Headers.end())
          {
               TokenVal = AuthIt->second;
          }
          else
          {
               auto APIKeyIt = Request.Headers.find("x-api-key");

               if (APIKeyIt != Request.Headers.end())
               {
                    TokenVal = APIKeyIt->second;
               }
               else
               {
                    APIKeyIt = Request.Headers.find("x-typesense-api-key");

                    if (APIKeyIt != Request.Headers.end())
                    {
                         TokenVal = APIKeyIt->second;
                    }
               }
          }

          if (TokenVal.find("Bearer ") == 0)
          {
               TokenVal = TokenVal.substr(7);
          }

          /* Scoped Key metadata. */

          std::string KeyIDVal;
          std::string KeyEmbeddedFilters;
          bool AuthenticatedRequest = false;

          RouteContext Context = BuildRouteContext(Request, &API);
          RouteAction ActionVal = Context.ActionVal;
          const std::string &NormalizedPath = Context.NormalizedPath;

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: route_resolved=" + std::string(RouteActionName(ActionVal)) + ", method=" + Request.Method + ", path=" + Request.Path + ", version=" + Request.Version + ", requests_processed_total=" + std::to_string(RequestsProcessed) + ".");
          }

          APIKey KeyObj;

          if (APIKeyManager::Instance().ValidateKey(TokenVal, &KeyObj))
          {
               if (!APIKeyManager::Instance().CheckRateLimit(KeyObj.ID))
               {
                    Response = HttpResponse(http_code::TOO_MANY_REQUESTS, StatusText(http_code::TOO_MANY_REQUESTS), "application/json");
                    Response.Body = "{\"error\":\"Rate limit exceeded\"}";
                    SendResponse(Response);
                    return;
               }

               APIKeyAction ReqAction = MapRouteToKeyAction(ActionVal);
               std::string ColNameVal = Context.CollectionName;

               /* Handle /multi_search and system endpoints. */

               if (!Context.IsPublic && ColNameVal.empty())
               {
                    if (ActionVal == RouteAction::MultiSearch || ActionVal == RouteAction::GlobalSearch)
                    {
                         /* Allowed at this level, HandleMultiSearch will check individual collections. */
                    }
                    else
                    {
                         /* System-wide endpoint (e.g. /stats, /health, etc.) - check for wildcard scope. */

                         if (!KeyObj.CanAccessCollection("*"))
                         {
                              Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
                              Response.Body = "{\"error\":\"Access to system endpoints not allowed for this key\"}";
                              LogAccessControl("Forbidden: key '" + KeyObj.ID + "' cannot access system endpoints", Request);
                              SendResponse(Response);
                              return;
                         }

                         if (!KeyObj.HasAction("*", ReqAction))
                         {
                              Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
                              Response.Body = "{\"error\":\"Action not allowed for system endpoints with this key\"}";
                              LogAccessControl("Forbidden: key '" + KeyObj.ID + "' action not allowed for system endpoints", Request);
                              SendResponse(Response);
                              return;
                         }

                         ColNameVal = "*";
                    }
               }
               else if (!Context.IsPublic)
               {
                    if (!KeyObj.CanAccessCollection(ColNameVal))
                    {
                         Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");

                         nlohmann::json ErrorJSON;
                         ErrorJSON["error"] = "Access to collection not allowed for this key";
                         ErrorJSON["collection"] = ColNameVal;
                         Response.Body = ErrorJSON.dump();

                         LogAccessControl("Forbidden: key '" + KeyObj.ID + "' cannot access collection '" + ColNameVal + "'", Request);
                         SendResponse(Response);
                         return;
                    }

                    if (!KeyObj.HasAction(ColNameVal, ReqAction))
                    {
                         Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
                         Response.Body = "{\"error\":\"Action not allowed for this collection with this key\"}";
                         LogAccessControl("Forbidden: key '" + KeyObj.ID + "' action not allowed for collection '" + ColNameVal + "'", Request);
                         SendResponse(Response);
                         return;
                    }
               }

               APIKeyManager::Instance().UpdateLastUsed(KeyObj.ID);
               KeyIDVal = KeyObj.ID;
               KeyEmbeddedFilters = KeyObj.GetEmbeddedFilters(ColNameVal);
               IsAdminVal = false;
               AuthenticatedRequest = true;
          }
          else
          {
               if (Instance && Instance->Users && Instance->Users->IsAuthEnabled())
               {
                    IsAdminVal = Instance->Users->IsAdmin(TokenVal);
               }
               else
               {
                    IsAdminVal = true;
               }

               if (!TokenVal.empty() && IsAdminVal)
               {
                    AuthenticatedRequest = true;
               }
          }

          /* Admin-only routes. */

          if (Context.IsAdminOnly)
          {
               if (!IsAdminVal)
               {
                    Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
                    Response.Body = "{\"error\":\"Only administrators can access this endpoint\"}";
                    SendResponse(Response);
                    return;
               }
          }

          if (Context.IsModuleControl && !IsAdminVal)
          {
               Response = HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "application/json");
               Response.Body = "{\"error\":\"Only administrators can access this endpoint\"}";
               SendResponse(Response);
               return;
          }

          /* Scoped search: inject key metadata into request. */

          if (!KeyIDVal.empty() || AuthenticatedRequest)
          {
               HttpRequest &ModRequest = const_cast<HttpRequest &>(Request);

               if (!KeyIDVal.empty())
               {
                    ModRequest.APIKeyID = KeyIDVal;
                    ModRequest.EmbeddedFilters = KeyEmbeddedFilters;
               }

               if (AuthenticatedRequest)
               {
                    ModRequest.Authenticated = true;
               }
          }

          /* Handle /ping route in ProcessSingleRequest BEFORE setting 404. */

          if (Request.Path == "/ping" && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", " Direct /ping route match in ProcessSingleRequest - calling HandlePing.");
               }

               Response = API.HandlePing(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /etc route in ProcessSingleRequest BEFORE setting 404. */
          /* Ensure /etc route is caught early and reliably (protocol codes for API communication). */

          if (ActionVal == RouteAction::Etc)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /etc route match in ProcessSingleRequest (backup) - calling HandleEtc.");
               }

               Response = API.HandleEtc(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /connections in ProcessSingleRequest BEFORE setting 404. */

          if (Request.Path == "/connections" && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /connections route match in ProcessSingleRequest - calling HandleConnections.");
               }

               Response = API.HandleConnections(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /rocksdb and /_rocksdb in ProcessSingleRequest BEFORE setting 404. */

          if ((Request.Path == "/rocksdb" || Request.Path == "/_rocksdb") && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /rocksdb route match in ProcessSingleRequest - calling HandleRocksDB.");
               }

               Response = API.HandleRocksDB(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /startup and /boot-status in ProcessSingleRequest BEFORE setting 404. */

          if ((Request.Path == "/startup" || Request.Path == "/boot-status") && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /startup route match in ProcessSingleRequest - calling HandleStartup.");
               }

               Response = API.HandleStartup(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /integrity and /consistency in ProcessSingleRequest BEFORE setting 404. */

          if ((Request.Path == "/integrity" || Request.Path == "/consistency") && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /integrity route match in ProcessSingleRequest - calling HandleIntegrity.");
               }

               Response = API.HandleIntegrity(Request);

               SendResponse(Response);

               return;
          }

          /* WORKAROUND: Handle /self-check in ProcessSingleRequest BEFORE setting 404. */

          if (Request.Path == "/self-check" && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", " Direct /self-check route match in ProcessSingleRequest - calling HandleSelfCheck.");
               }

               Response = API.HandleSelfCheck(Request);

               SendResponse(Response);

               return;
          }

          /* Handle /flush route in ProcessSingleRequest BEFORE setting 404. */

          if (Request.Method == "POST" && (Request.Path == "/flush" || Request.Path == "/flush/"))
          {
               HttpResponse DedupResponse = API.CheckReplicationOperationDedup(Request, "POST /flush");
               if (DedupResponse.StatusCode != 0)
               {
                    SendResponse(DedupResponse);
                    return;
               }

               HttpResponse ReadOnlyResponse = API.CheckReadOnlyMode(Request, "POST /flush");
               if (ReadOnlyResponse.StatusCode != 0)
               {
                    SendResponse(ReadOnlyResponse);
                    return;
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", " FLUSH ROUTE CAUGHT IN ProcessSingleRequest (before 404) - calling HandleFlush.");
               }

               Response = API.HandleFlush(Request);
               API.FinalizeReplicationOperation(Request, Response);
               API.FinalizeReplicationResyncRequest(Request, Response);

               SendResponse(Response);

               return;
          }

          bool MutatingMethod = IsMutatingRequestMethod(Request);
          if (MutatingMethod)
          {
               std::string Operation = Request.Method + " " + Request.Path;
               HttpResponse DedupResponse = API.CheckReplicationOperationDedup(Request, Operation);
               if (DedupResponse.StatusCode != 0)
               {
                    SendResponse(DedupResponse);
                    return;
               }

               HttpResponse ReadOnlyResponse = API.CheckReadOnlyMode(Request, Operation);
               if (ReadOnlyResponse.StatusCode != 0)
               {
                    SendResponse(ReadOnlyResponse);
                    return;
               }
          }

          /* The structured resolver is authoritative. Do not let the legacy
      * substring dispatcher reinterpret malformed or unsupported paths. */

          if (ActionVal == RouteAction::NotFound)
          {
               Response = BuildRouteNotFoundResponse(Request.Path);
               RecordAnalyticsForResponse(Request, Response, ActionVal);
               SendResponse(Response);
               return;
          }

          Response = BuildRouteNotFoundResponse(Request.Path);

          if (Instance && Instance->Logs)
          {
               bool HasDocuments = (Request.Path.find("/documents") != std::string::npos);

               bool HasDocumentsSlash = (Request.Path.find("/documents/") != std::string::npos);

               bool HasCollectionsPrefix = (Request.Path.find("/collections/") == 0);

               bool ShouldRouteDocs = HasCollectionsPrefix && HasDocuments && !HasDocumentsSlash && Request.Method == "GET";

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_server", "ProcessRequestWithAPI: method=" + Request.Method + " (len=" + std::to_string(Request.Method.size()) + ") path=" + Request.Path + " has_collections_prefix=" + std::string(HasCollectionsPrefix ? "true" : "false") + " has_documents=" + std::string(HasDocuments ? "true" : "false") + " has_documents_slash=" + std::string(HasDocumentsSlash ? "true" : "false") + " should_route_docs=" + std::string(ShouldRouteDocs ? "true" : "false") + ".");
               }
          }

          bool VectorSearchAlias = false;

          if (NormalizedPath.find("/collections/") == 0 && NormalizedPath.find("/documents") == std::string::npos)
          {
               if (NormalizedPath.size() >= 7 && NormalizedPath.rfind("/search") == NormalizedPath.size() - 7)
               {
                    VectorSearchAlias = true;
               }
          }

          /* Route to specific API handlers. */

          if (NormalizedPath == "/users" && Request.Method == "GET")
          {
               Response = API.HandleListUsers(Request);
          }
          else if (NormalizedPath == "/users" && Request.Method == "POST")
          {
               Response = API.HandleCreateUser(Request);
          }
          else if (NormalizedPath.find("/users/") == 0 && Request.Method == "GET")
          {
               Response = API.HandleGetUser(Request);
          }
          else if (NormalizedPath.find("/users/") == 0 && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteUser(Request);
          }
          else if (NormalizedPath.find("/users/") == 0 && Request.Method == "PUT")
          {
               Response = API.HandleUpdateUser(Request);
          }
          else if (NormalizedPath == "/keys" && Request.Method == "GET")
          {
               Response = API.HandleListKeys(Request);
          }
          else if (NormalizedPath == "/keys" && Request.Method == "POST")
          {
               Response = API.HandleCreateKey(Request);
          }
          else if (NormalizedPath.find("/keys/") == 0 && Request.Method == "GET")
          {
               Response = API.HandleGetKey(Request);
          }
          else if (NormalizedPath.find("/keys/") == 0 && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteKey(Request);
          }
          else if (NormalizedPath.find("/keys/") == 0 && Request.Method == "PUT")
          {
               Response = API.HandleUpdateKey(Request);
          }
          else if (NormalizedPath == "/presets" && Request.Method == "GET")
          {
               Response = API.HandleListPresets(Request);
          }
          else if (NormalizedPath.find("/presets/") == 0 && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdatePreset(Request);
          }
          else if (NormalizedPath.find("/presets/") == 0 && Request.Method == "GET")
          {
               Response = API.HandleGetPreset(Request);
          }
          else if (NormalizedPath.find("/presets/") == 0 && Request.Method == "DELETE")
          {
               Response = API.HandleDeletePreset(Request);
          }

          /* Check for synonyms/stopwords/overrides FIRST before search to avoid false matches. */
          /* Check synonyms BEFORE search, because synonym IDs might contain "search". */
          /* Example: /collections/books/synonyms/search_test_syn_123 should NOT match search route. */

          if (Request.Path.find("/collections/") == 0 && Request.Path.find("/synonyms") != std::string::npos && Request.Path.find("/synonyms/") != std::string::npos && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdateSynonym(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/synonyms") != std::string::npos && Request.Path.find("/synonyms/") == std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleListSynonyms(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/synonyms/") != std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleGetSynonym(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/synonyms/") != std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteSynonym(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/stopwords") != std::string::npos && Request.Path.find("/stopwords/") == std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleListStopwords(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/stopwords") != std::string::npos && Request.Path.find("/stopwords/") == std::string::npos && Request.Method == "POST")
          {
               Response = API.HandleCreateStopword(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/stopwords/") != std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteStopword(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/search") != std::string::npos && (Request.Method == "GET" || Request.Method == "POST"))
          {
               /* Check for search endpoints before general POST /documents. */
               /* Otherwise /documents/search gets routed to HandleAddDocument. */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "Routing to HandleSearch for path: " + Request.Path + ".");
               }

               Response = API.HandleSearch(Request);
          }
          else if (Request.Path == "/sql" && (Request.Method == "GET" || Request.Method == "POST"))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "Routing to HandleSearch for direct SQL path: " + Request.Path + ".");
               }

               Response = API.HandleSearch(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && NormalizedPath.find("/search") != std::string::npos && NormalizedPath.find("/documents") == std::string::npos && NormalizedPath.find("/synonyms") == std::string::npos && NormalizedPath.find("/stopwords") == std::string::npos && (Request.Method == "GET" || Request.Method == "POST"))
          {
               /* Handle /collections/{name}/search endpoint (without /documents). */
               /* BUT exclude paths with /synonyms or /stopwords to avoid false matches. */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "Routing to HandleSearch for path: " + Request.Path + ".");
               }

               Response = API.HandleSearch(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/facet_counts") != std::string::npos && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleFacetCounts(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/maybe") != std::string::npos && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleMaybe(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/") != std::string::npos && Request.Path.find("/context") != std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleGetDocumentContext(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/export") != std::string::npos && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleExportDocuments(Request);
          }
          else if (Request.Method == "POST" && Request.Path.find("/collections/") == 0 && Request.Path.find("/documents") != std::string::npos)
          {
               /* Check if this is a bulk import request. */

               if (Request.Path.find("/documents/import") != std::string::npos)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "ROUTING TO HandleBulkImportDocuments for path: " + Request.Path + ".");
                    }

                    Response = API.HandleBulkImportDocuments(Request);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "HandleBulkImportDocuments returned status: " + std::to_string(Response.StatusCode) + ".");
                    }
               }
               else
               {
                    /* Single document insert. */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "SIMPLIFIED ROUTING TO HandleAddDocument for path: " + Request.Path + ".");
                    }

                    Response = API.HandleAddDocument(Request);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "HandleAddDocument returned status: " + std::to_string(Response.StatusCode) + ".");
                    }
               }
          }
          else if ((Request.Path == "/status" || Request.Path == "/query") && Request.Method == "GET")
          {
               Response = API.HandleStatus(Request);
          }
          else if (Request.Path == "/search-config" && Request.Method == "GET")
          {
               Response = API.HandleSearchConfig(Request);
          }
          else if (Request.Path == "/config-files" && Request.Method == "GET")
          {
               Response = API.HandleConfigFiles(Request);
          }
          else if (Request.Path == "/synonyms" && Request.Method == "GET")
          {
               Response = API.HandleListAllSynonyms(Request);
          }
          else if (Request.Path == "/synonym_sets" && Request.Method == "GET")
          {
               Response = API.HandleListAllSynonyms(Request);
          }
          else if ((Request.Path == "/synonyms/global" || Request.Path == "/synonym_sets/global") && Request.Method == "GET")
          {
               Response = API.HandleListGlobalSynonyms(Request);
          }
          else if ((Request.Path.find("/synonyms/global/") == 0 || Request.Path.find("/synonym_sets/global/") == 0) && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdateGlobalSynonym(Request);
          }
          else if ((Request.Path.find("/synonyms/global/") == 0 || Request.Path.find("/synonym_sets/global/") == 0) && Request.Method == "GET")
          {
               Response = API.HandleGetGlobalSynonym(Request);
          }
          else if ((Request.Path.find("/synonyms/global/") == 0 || Request.Path.find("/synonym_sets/global/") == 0) && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteGlobalSynonym(Request);
          }
          else if (Request.Path == "/stopwords" && Request.Method == "GET")
          {
               Response = API.HandleListAllStopwords(Request);
          }
          else if (Request.Path == "/stopword_sets" && Request.Method == "GET")
          {
               Response = API.HandleListAllStopwords(Request);
          }
          else if ((Request.Path == "/stopwords/global" || Request.Path == "/stopword_sets/global") && Request.Method == "GET")
          {
               Response = API.HandleListGlobalStopwords(Request);
          }
          else if ((Request.Path == "/stopwords/global" || Request.Path == "/stopword_sets/global") && Request.Method == "POST")
          {
               Response = API.HandleCreateGlobalStopword(Request);
          }
          else if ((Request.Path.find("/stopwords/global/") == 0 || Request.Path.find("/stopword_sets/global/") == 0) && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteGlobalStopword(Request);
          }
          else if (Request.Path == "/links" && Request.Method == "GET")
          {
               Response = API.HandleLinksList(Request);
          }
          else if (Request.Path == "/links/ping" && Request.Method == "GET")
          {
               Response = API.HandleLinksPing(Request);
          }
          else if (Request.Path == "/links/connect" && Request.Method == "POST")
          {
               Response = API.HandleLinksConnect(Request);
          }
          else if (Request.Path == "/links/disconnect" && Request.Method == "POST")
          {
               Response = API.HandleLinksDisconnect(Request);
          }
          else if (Request.Path == "/collections/distributed" && Request.Method == "GET")
          {
               Response = API.HandleListCollectionsDistributed(Request);
          }
          else if (Request.Path == "/collections" && Request.Method == "GET")
          {
               Response = API.HandleListCollections(Request);
          }
          else if (Request.Path == "/collections" && Request.Method == "POST")
          {
               Response = API.HandleCreateCollection(Request);
          }
          else if (Request.Path == "/admin/storage_status" && Request.Method == "GET")
          {
               Response = API.HandleStorageStatus(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && (Request.Path.find("/vector_search") != std::string::npos || VectorSearchAlias) && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleVectorSearch(Request);
          }
          else if (Request.Path.find("/collections/") == 0 &&
                   NormalizedPath.size() > std::string("/collections/").size() &&
                   NormalizedPath.rfind("/lang") == (NormalizedPath.size() - std::string("/lang").size()) &&
                   Request.Method == "GET")
          {
               Response = API.HandleGetCollectionLanguage(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && NormalizedPath.find("/search") == std::string::npos && Request.Path.find("/documents") == std::string::npos && Request.Path.find("/synonyms") == std::string::npos && Request.Path.find("/stopwords") == std::string::npos && Request.Path.find("/overrides") == std::string::npos && Request.Path.find("/curations") == std::string::npos && Request.Path.find("/curation_sets") == std::string::npos && Request.Path.find("/aliases") == std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleGetCollection(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents") == std::string::npos && Request.Path.find("/synonyms") == std::string::npos && Request.Path.find("/stopwords") == std::string::npos && Request.Path.find("/overrides") == std::string::npos && Request.Path.find("/curations") == std::string::npos && Request.Path.find("/curation_sets") == std::string::npos && Request.Path.find("/aliases") == std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteCollection(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/update") != std::string::npos && Request.Method == "POST")
          {
               Response = API.HandleUpdateCollection(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents") != std::string::npos && Request.Path.find("/documents/") == std::string::npos && Request.Path.find("/search") == std::string::npos && Request.Path.find("/facet_counts") == std::string::npos && Request.Path.find("/export") == std::string::npos && Request.Path.find("/import") == std::string::npos && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "Routing to HandleListDocuments (legacy dispatcher) for path: " + Request.Path + ".");
               }

               Response = API.HandleListDocuments(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/") != std::string::npos && Request.Path.find("/search") == std::string::npos && Request.Path.find("/facet_counts") == std::string::npos && Request.Path.find("/export") == std::string::npos && Request.Path.find("/maybe") == std::string::npos && Request.Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", "ROUTING TO HandleGetDocument for path: " + Request.Path + ".");
               }

               Response = API.HandleGetDocument(Request);

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", "HandleGetDocument returned status: " + std::to_string(Response.StatusCode) + ".");
               }
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/") != std::string::npos && Request.Path.find("/search") == std::string::npos && Request.Path.find("/facet_counts") == std::string::npos && Request.Path.find("/export") == std::string::npos && Request.Path.find("/maybe") == std::string::npos && Request.Method == "PUT")
          {
               Response = API.HandleUpdateDocument(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents/") != std::string::npos && Request.Path.find("/search") == std::string::npos && Request.Path.find("/facet_counts") == std::string::npos && Request.Path.find("/export") == std::string::npos && Request.Path.find("/maybe") == std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteDocument(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/documents") != std::string::npos && Request.Path.find("/documents/") == std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteDocumentsByFilter(Request);
          }
          else if (Request.Path == "/multi_search" && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleMultiSearch(Request);
          }
          else if (Request.Path == "/search" && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleGlobalSearch(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/overrides") != std::string::npos && Request.Path.find("/overrides/") == std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleListOverrides(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && (Request.Path.find("/curations") != std::string::npos || Request.Path.find("/curation_sets") != std::string::npos) && Request.Path.find("/curations/") == std::string::npos && Request.Path.find("/curation_sets/") == std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleListOverrides(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/overrides/") != std::string::npos && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdateOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && (Request.Path.find("/curations/") != std::string::npos || Request.Path.find("/curation_sets/") != std::string::npos) && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdateOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/overrides/") != std::string::npos && Request.Method == "GET")
          {
               Response = API.HandleGetOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && (Request.Path.find("/curations/") != std::string::npos || Request.Path.find("/curation_sets/") != std::string::npos) && Request.Method == "GET")
          {
               Response = API.HandleGetOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && Request.Path.find("/overrides/") != std::string::npos && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && (Request.Path.find("/curations/") != std::string::npos || Request.Path.find("/curation_sets/") != std::string::npos) && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteOverride(Request);
          }
          else if (Request.Path.find("/collections/") == 0 && NormalizedPath.rfind("/aliases") == NormalizedPath.size() - 8 && Request.Method == "GET")
          {
               Response = API.HandleListAliases(Request);
          }
          else if (Request.Path == "/aliases" && Request.Method == "GET")
          {
               Response = API.HandleListAliases(Request);
          }
          else if (Request.Path == "/modules" && Request.Method == "GET")
          {
               Response = API.HandleListModules(Request);
          }
          else if (Request.Path.find("/modules/") == 0 && NormalizedPath.rfind("/syntax") == NormalizedPath.size() - 7 && Request.Method == "GET")
          {
               Response = API.HandleModuleSyntax(Request);
          }
          else if (Request.Path.find("/modules/") == 0)
          {
               Response = API.HandleModuleAPI(Request);
          }
          else if (Request.Path.find("/aliases/") == 0 && (Request.Method == "POST" || Request.Method == "PUT"))
          {
               Response = API.HandleCreateOrUpdateAlias(Request);
          }
          else if (Request.Path.find("/aliases/") == 0 && Request.Method == "GET")
          {
               Response = API.HandleGetAlias(Request);
          }
          else if (Request.Path.find("/aliases/") == 0 && Request.Method == "DELETE")
          {
               Response = API.HandleDeleteAlias(Request);
          }
          else if (Request.Path == "/health" && Request.Method == "GET")
          {
               Response = API.HandleHealth(Request);
          }
          else if (Request.Path == "/ready" && Request.Method == "GET")
          {
               Response = API.HandleReady(Request);
          }
          else if ((Request.Path == "/metrics" || Request.Path == "/metrics.json") && Request.Method == "GET")
          {
               Response = API.HandleMetrics(Request);
          }
          else if (Request.Path == "/stats" && Request.Method == "GET")
          {
               Response = API.HandleStats(Request);
          }
          else if (Request.Path == "/doctotal" && Request.Method == "GET")
          {
               Response = API.HandleDocTotal(Request);
          }
          else if (Request.Path == "/update-counters" && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleUpdateCounters(Request);
          }
          else if (Request.Path == "/repair" && (Request.Method == "GET" || Request.Method == "POST"))
          {
               Response = API.HandleRepair(Request);
          }
          else if (Request.Path == "/admin/storage_status" && Request.Method == "GET")
          {
               Response = API.HandleStorageStatus(Request);
          }
          else if (Request.Path == "/" && Request.Method == "GET")
          {
               /* Root endpoint - same as /status. */

               Response = API.HandleStatus(Request);
          }

          RecordAnalyticsForResponse(Request, Response, ActionVal);
          API.FinalizeReplicationOperation(Request, Response);
          API.FinalizeReplicationResyncRequest(Request, Response);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: response_ready=true status=" + std::to_string(Response.StatusCode) + " route=" + std::string(RouteActionName(ActionVal)) + " keep_alive=" + std::string(KeepAlive ? "true" : "false") + " body_size=" + std::to_string(Response.Body.size()) + ".");
          }

          SendResponse(Response);

          LastActivity = Instance->Now();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: SendResponse invoked - " + std::to_string(Response.StatusCode) + " " + Response.StatusText + " (body_size: " + std::to_string(Response.Body.size()) + ", path: " + Request.Path + ", fd=" + std::to_string(GetFD()) + ").");
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "ProcessSingleRequest: exception while handling " + Request.Method + " " + Request.Path + ": " + std::string(E.what()) + ".");
          }

          HttpResponse ErrorResponse(http_code::INTERNAL_SERVER_ERROR, StatusText(http_code::INTERNAL_SERVER_ERROR), "application/json");
          ErrorResponse.Body = "{\"error\":\"Internal server error\",\"message\":\"" + SearchAPI::GetInstance().EscapeJSONString(E.what()) + "\"}";

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: exception path queued 500 response for " + Request.Method + " " + Request.Path + ".");
          }

          SendResponse(ErrorResponse);
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "ProcessSingleRequest: unknown exception while handling " + Request.Method + " " + Request.Path + ".");
          }

          HttpResponse ErrorResponse(http_code::INTERNAL_SERVER_ERROR, StatusText(http_code::INTERNAL_SERVER_ERROR), "application/json");
          ErrorResponse.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred\"}";

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "ProcessSingleRequest: unknown exception path queued 500 response for " + Request.Method + " " + Request.Path + ".");
          }

          SendResponse(ErrorResponse);
     }
}

/* Send HTTP response */

void HttpConnection::SendResponse(const HttpResponse &Response)
{
     HttpResponse EffectiveResponse = Response;

     if (!ActiveRequestID.empty())
     {
          EffectiveResponse.Headers["X-Request-Id"] = ActiveRequestID;
     }

     if (ClosingValue.load(std::memory_order_acquire) || !HasFD())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "SendResponse: Dropping response because connection is already closed.");
          }

          return;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server.", "[SendResponse] ENTRY - status=" + std::to_string(EffectiveResponse.StatusCode) + " " + EffectiveResponse.StatusText + ", body_size=" + std::to_string(EffectiveResponse.Body.length()) + ", keep_alive=" + std::string(KeepAlive ? "true" : "false") + ", closing=" + std::string(ClosingValue.load() ? "true" : "false") + ", fd=" + std::to_string(GetFD()) + ".");
     }

     /* Cap response body size to prevent OOM. */

     if (EffectiveResponse.Body.length() > HTTP_MAX_RESPONSE_SIZE)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http", "Response body too large: " + std::to_string(EffectiveResponse.Body.length()) + " bytes - REJECTING!.");
          }

          HttpResponse ErrorResp;

          ErrorResp.StatusCode = 413;
          ErrorResp.StatusText = "Payload Too Large";
          ErrorResp.Body = "{\"error\":\"Response too large\"}";

          SendResponse(ErrorResp); /* Send small error instead. */

          return;
     }

     /* Use string builder for better performance. */

     std::string ResponseStr;

     ResponseStr.reserve(1024 + std::min(EffectiveResponse.Body.length(), static_cast<size_t>(HTTP_MAX_RESPONSE_SIZE))); /* Pre-allocate safely. */

     /* Status line. */

     ResponseStr += "HTTP/1.1 ";
     ResponseStr += std::to_string(EffectiveResponse.StatusCode);
     ResponseStr += " ";
     ResponseStr += EffectiveResponse.StatusText;
     ResponseStr += "\r\n";

     /* Add keep-alive header if supported. */

     if (KeepAlive)
     {
          ResponseStr += "Connection: keep-alive\r\n";
          ResponseStr += "Keep-Alive: timeout=";
          ResponseStr += std::to_string(HTTP_KEEP_ALIVE_TIMEOUT_SEC);
          ResponseStr += ", max=";
          ResponseStr += std::to_string(std::max(0, HTTP_MAX_REQUESTS_PER_CONNECTION - RequestsProcessed));
          ResponseStr += "\r\n";
     }
     else
     {
          ResponseStr += "Connection: close\r\n";
     }

     /* CORS headers - allow all origins for development. */

     ResponseStr += "Access-Control-Allow-Origin: *\r\n";
     ResponseStr += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, PATCH\r\n";
     ResponseStr += "Access-Control-Allow-Headers: Content-Type, Authorization, Accept, X-Requested-With, X-API-Key, X-TYPESENSE-API-KEY, X-Request-Id\r\n";
     ResponseStr += "Access-Control-Max-Age: 86400\r\n";

     /* Headers. */

     for (const auto &HeaderPair : EffectiveResponse.Headers)
     {
          ResponseStr += HeaderPair.first;
          ResponseStr += ": ";
          ResponseStr += HeaderPair.second;
          ResponseStr += "\r\n";
     }

     /* Content-Length. */

     ResponseStr += "Content-Length: ";
     ResponseStr += std::to_string(EffectiveResponse.Body.length());
     ResponseStr += "\r\n";

     /* End of headers. */

     ResponseStr += "\r\n";

     /* Body. */

     ResponseStr += EffectiveResponse.Body;

     /* Protect response buffer with mutex to prevent race conditions. */

     {
          std::lock_guard<std::mutex> Lock(ResponseMutex);

          if (ResponsePending && !ResponseBuffer.empty())
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http", "Response buffer busy, appending queued response (active size=" + std::to_string(ResponseBuffer.size()) + ", queued size=" + std::to_string(ResponseStr.size()) + ").");
               }

               ResponseQueue.push_back(std::move(ResponseStr));
          }
          else
          {
               ResponseBuffer = std::move(ResponseStr);
               ResponseSentOffset = 0;
               ResponsePending = true;
               ResponseSerial++;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "SendResponse: Response queued - active_buffer_size=" + std::to_string(ResponseBuffer.size()) + ", queued_responses=" + std::to_string(ResponseQueue.size()) + ", response_pending=" + std::string(ResponsePending ? "true" : "false") + ".");
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "SendResponse: Registering pending write for fd=" + std::to_string(GetFD()) + " (response_pending=true, buffered_bytes=" + std::to_string(ResponseBuffer.size()) + ").");
     }

     SocketEngine::RegisterPendingWrite(this);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "[SendResponse] Response registered for sending, fd=" + std::to_string(GetFD()) + ".");
     }
}

/* CleanupConnection cleans up connection resources. */

void HttpConnection::CleanupConnection()
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "CleanupConnection: ENTRY (fd=" + std::to_string(GetFD()) + ", closing=" + std::string(ClosingValue.load() ? "true" : "false") + ", RequestsProcessed=" + std::to_string(RequestsProcessed) + ").");
     }

     if (HasActiveRequests())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "CleanupConnection: Skipping cleanup while request is still running.");
          }

          return;
     }

     /* Check if connection should be closed due to timeout. */

     auto NowVal = Instance->Now();

     auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(NowVal - LastActivity).count();

     if (Elapsed > HTTP_KEEP_ALIVE_TIMEOUT_SEC)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "CleanupConnection: Keep-alive timeout (elapsed=" + std::to_string(Elapsed) + "s, max=" + std::to_string(HTTP_KEEP_ALIVE_TIMEOUT_SEC) + "s), closing.");
          }

          /* FIX: Cache fd before DelFD() to prevent use-after-free. */

          int FDValue = GetFD();

          ClosingValue.store(true);

          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);
          }

          SetFD(-1);

          return;
     }

     /* Check if we've exceeded max requests per connection. */

     if (RequestsProcessed >= HTTP_MAX_REQUESTS_PER_CONNECTION)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "CleanupConnection: Max requests reached (" + std::to_string(RequestsProcessed) + " >= " + std::to_string(HTTP_MAX_REQUESTS_PER_CONNECTION) + "), closing.");
          }

          /* FIX: Cache fd before DelFD() to prevent use-after-free. */

          int FDValue = GetFD();

          ClosingValue.store(true);

          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);
          }

          SetFD(-1);

          return;
     }
}

/* Forces the HTTP connection to close. */

void HttpConnection::ForceClose()
{
     if (!HasFD())
     {
          return;
     }

     int FDValue = GetFD();

     ClosingValue.store(true);

     {
          std::lock_guard<std::mutex> Lock(ResponseMutex);
          ResponseQueue.clear();
          ResponseBuffer.clear();
          ResponsePending = false;
          ResponseSentOffset = 0;
     }

     SocketEngine::DelFD(this);

     if (FDValue >= 0)
     {
          close(FDValue);
     }

     SetFD(-1);
}

/* ParseHttpRequest parses raw HTTP request string into HttpRequest structure. */

bool HttpConnection::ParseHttpRequest(const std::string &RawRequest, HttpRequest &Request)
{
     const size_t RawHeaderEnd = RawRequest.find("\r\n\r\n");
     if (RawHeaderEnd == std::string::npos || RawHeaderEnd + 4 > HTTP_MAX_HEADER_SIZE)
     {
          return false;
     }

     std::string FramingError;
     if (!HasValidRequestFramingHeaders(RawRequest.substr(0, RawHeaderEnd + 4), &FramingError))
     {
          return false;
     }

     std::istringstream ISS(RawRequest);

     std::string Line;

     /* Parse request line. */

     if (!std::getline(ISS, Line))
     {
          return false;
     }

     /* Remove trailing \r. */

     if (!Line.empty() && Line.back() == '\r')
     {
          Line.pop_back();
     }

     std::istringstream RequestLine(Line);

     if (!(RequestLine >> Request.Method >> Request.Path >> Request.Version))
     {
          /* Log parsing failure details. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_server", "Failed to parse request line: '" + Line + "'.");

               ConsoleWriter::WriteError("Failed to parse HTTP request line.");
          }

          return false;
     }

     /* Parse query parameters. */

     size_t QueryPos = Request.Path.find('?');

     if (QueryPos != std::string::npos)
     {
          std::string QueryString = Request.Path.substr(QueryPos + 1);

          Request.Path = Request.Path.substr(0, QueryPos);
          Request.QueryParams = ParseQueryParams(QueryString);
     }

     /* URL decode the path to handle encoded collection names and document IDs. */

     Request.Path = UrlDecode(Request.Path, false);

     /* Parse headers. */

     std::string HeaderLines;

     std::string HeaderLine;

     while (std::getline(ISS, HeaderLine) && HeaderLine != "\r")
     {
          if (!HeaderLine.empty() && HeaderLine.back() == '\r')
          {
               HeaderLine.pop_back();
          }

          HeaderLines += HeaderLine + "\n";
     }

     ParseHeaders(HeaderLines, Request.Headers);

     /* Parse body - extract directly from RawRequest string to preserve exact content. */

     size_t HeaderEndPos = RawRequest.find("\r\n\r\n");

     if (HeaderEndPos != std::string::npos)
     {
          size_t BodyStart = HeaderEndPos + 4;

          /* Get Content-Length if present (case-insensitive search). */

          size_t ContentLength = 0;

          for (const auto &HeaderVal : Request.Headers)
          {
               std::string KeyLower = HeaderVal.first;

               std::transform(KeyLower.begin(), KeyLower.end(), KeyLower.begin(), ::tolower);

               if (KeyLower == "content-length")
               {
                    try
                    {
                         size_t Parsed = 0;
                         ContentLength = std::stoull(HeaderVal.second, &Parsed);
                         if (Parsed != HeaderVal.second.size())
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Normal("http_server", "Invalid Content-Length trailing data: '" + HeaderVal.second + "'.");
                              }

                              return false;
                         }
                    }
                    catch (const std::invalid_argument &E)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("http_server", "Invalid Content-Length argument: '" + HeaderVal.second + "'.");
                         }

                         return false;
                    }
                    catch (const std::out_of_range &E)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("http_server", "Content-Length out of range: '" + HeaderVal.second + "'.");
                         }

                         return false;
                    }
                    catch (...)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("http_server", "Unknown exception parsing Content-Length: '" + HeaderVal.second + "'.");
                         }

                         return false;
                    }

                    break;
               }
          }

          /* Debug logging. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("http_server", "Body extraction: HeaderEndPos=" + std::to_string(HeaderEndPos) + ", BodyStart=" + std::to_string(BodyStart) + ", ContentLength=" + std::to_string(ContentLength) + ", RawRequest.size()=" + std::to_string(RawRequest.size()) + ".");
          }

          /* Extract body using Content-Length if available, otherwise everything after headers. */

          if (ContentLength > 0)
          {
               if (RawRequest.size() >= BodyStart + ContentLength)
               {
                    Request.Body = RawRequest.substr(BodyStart, ContentLength);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Debug("http_server", "Extracted body (Content-Length): " + std::to_string(Request.Body.size()) + " bytes.");
                    }
               }
               else
               {
                    /* Body incomplete - wait for more data. */

                    return false;
               }
          }
          else if (BodyStart < RawRequest.size())
          {
               /* No Content-Length or invalid, take everything after headers. */

               Request.Body = RawRequest.substr(BodyStart);

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "Extracted body (no Content-Length): " + std::to_string(Request.Body.size()) + " bytes.");
               }
          }
          else
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("http_server", "No body extracted - BodyStart >= RawRequest.size().");
               }
          }
     }
     else
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("http_server", "No header end marker found in request.");
          }
     }

     return true;
}

/* UrlDecode decodes a URL-encoded string. */

std::string UrlDecode(const std::string &Str, bool DecodePlusAsSpace)
{
     std::string Result;

     Result.reserve(Str.size());

     /* Limit decoded string size to prevent DoS. */

     for (size_t I = 0; I < Str.size() && Result.size() < HTTP_MAX_DECODED_SIZE; ++I)
     {
          if (Str[I] == '%' && I + 2 < Str.size())
          {
               /* Decode %XX hex sequence. */

               if (IsHexDigit(Str[I + 1]) && IsHexDigit(Str[I + 2]))
               {
                    int HexVal = (HexDigitValue(Str[I + 1]) << 4) | HexDigitValue(Str[I + 2]);

                    /* Validate decoded character is safe (not null byte). */

                    if (HexVal == 0)
                    {
                         /* Null byte in URL - potential security issue, skip it. */

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("http", "SECURITY: Null byte detected in URL decode - REJECTED.");
                         }

                         Result += Str[I];
                    }
                    else
                    {
                         Result += static_cast<char>(HexVal);
                    }

                    I += 2;
               }
               else
               {
                    Result += Str[I];
               }
          }
          else if (Str[I] == '+' && DecodePlusAsSpace)
          {
               Result += ' ';
          }
          else
          {
               Result += Str[I];
          }
     }

     /* Check if we hit the size limit. */

     if (Result.size() >= HTTP_MAX_DECODED_SIZE)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http", "SECURITY: URL decode result too large - TRUNCATED.");
          }
     }

     return Result;
}

/* ParseQueryParams parses query parameters from path. */

std::map<std::string, std::string> HttpConnection::ParseQueryParams(const std::string &QueryString)
{
     std::map<std::string, std::string> Params;

     std::istringstream ISS(QueryString);

     std::string Pair;

     while (std::getline(ISS, Pair, '&'))
     {
          size_t EqPos = Pair.find('=');

          if (EqPos != std::string::npos)
          {
               std::string Key = Pair.substr(0, EqPos);
               std::string Value = Pair.substr(EqPos + 1);

               /* URL decode both key and value. */

               Params[UrlDecode(Key, true)] = UrlDecode(Value, true);
          }
          else if (!Pair.empty())
          {
               Params[UrlDecode(Pair, true)] = "true";
          }
     }

     return Params;
}

/*
 * Add comprehensive header validation and sanitization to prevent header injection.
 * Limit number of headers to prevent DoS.
 */

void HttpConnection::ParseHeaders(const std::string &HeaderLines, std::map<std::string, std::string> &Headers)
{
     size_t HeaderCount = 0;

     std::istringstream ISS(HeaderLines);

     std::string Line;

     while (std::getline(ISS, Line))
     {
          /* Enforce maximum header count. */

          if (HeaderCount >= HTTP_MAX_HEADER_COUNT)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http", "Too many headers: " + std::to_string(HeaderCount) + " (max=" + std::to_string(HTTP_MAX_HEADER_COUNT) + ") - REJECTING!.");
               }

               Headers.clear(); /* Clear partial headers. */

               return; /* Reject request with too many headers. */
          }

          size_t ColonPos = Line.find(':');

          if (ColonPos != std::string::npos)
          {
               std::string Key = Line.substr(0, ColonPos);
               std::string Value = Line.substr(ColonPos + 1);

               /* Trim whitespace. */

               Key.erase(0, Key.find_first_not_of(" \t"));
               Key.erase(Key.find_last_not_of(" \t") + 1);
               Value.erase(0, Value.find_first_not_of(" \t"));
               Value.erase(Value.find_last_not_of(" \t") + 1);

               /*
     * SECURITY: Validate header key and value to prevent injection attacks.
     * Check for CRLF injection (newlines in headers).
     */

               if (Key.find('\r') != std::string::npos || Key.find('\n') != std::string::npos ||
                   Value.find('\r') != std::string::npos || Value.find('\n') != std::string::npos)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("http", "Header injection attempt detected - rejecting header with CRLF.");
                    }

                    Headers.clear();

                    return; /* Reject request with injection attempt. */
               }

               /* Validate header key format (should be printable ASCII, no control chars). */

               bool ValidKey = true;

               for (char C : Key)
               {
                    if (C < 32 || C > 126)
                    {
                         ValidKey = false;
                         break;
                    }
               }

               if (!ValidKey || Key.empty())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("http", "Invalid header key format - rejecting.");
                    }

                    Headers.clear();

                    return;
               }

               /* Limit header key and value length to prevent DoS. */

               const size_t MaxHeaderKeyLen = 256;
               const size_t MaxHeaderValueLen = 8192;

               /*
     * Validate header key doesn't contain injection characters.
     * Prevent header injection attacks by rejecting dangerous characters.
     */

               for (char C : Key)
               {
                    if (C == '\r' || C == '\n' || C == '\0')
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("http_server", "Header injection attempt detected in key - rejecting (key contains control character).");
                         }

                         return; /* Reject header with injection characters. */
                    }
               }

               /* Validate header value doesn't contain injection characters. */

               for (char C : Value)
               {
                    if (C == '\r' || C == '\n' || C == '\0')
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("http_server", "Header injection attempt detected in value - rejecting (value contains control character).");
                         }

                         return; /* Reject header with injection characters. */
                    }
               }

               /*
     * Sanitize header key - convert to lowercase and trim whitespace.
     * This prevents case-sensitivity issues and header smuggling.
     */

               std::string SanitizedKey = Key;

               /* Trim leading/trailing whitespace. */

               SanitizedKey.erase(0, SanitizedKey.find_first_not_of(" \t"));
               SanitizedKey.erase(SanitizedKey.find_last_not_of(" \t") + 1);

               /* Convert to lowercase for consistency. */

               std::transform(SanitizedKey.begin(), SanitizedKey.end(), SanitizedKey.begin(), ::tolower);

               /* Sanitize header value - trim whitespace. */

               std::string SanitizedValue = Value;

               SanitizedValue.erase(0, SanitizedValue.find_first_not_of(" \t"));
               SanitizedValue.erase(SanitizedValue.find_last_not_of(" \t") + 1);

               if (Key.length() > MaxHeaderKeyLen || Value.length() > MaxHeaderValueLen)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("http", "Header key or value too long - rejecting (key_len=" + std::to_string(Key.length()) + ", value_len=" + std::to_string(Value.length()) + ").");
                    }

                    Headers.clear();

                    return;
               }

               Headers[SanitizedKey] = SanitizedValue;
               HeaderCount++;
          }
     }
}

/* HttpServer implementation. */

HttpServer::HttpServer(const BindConfig &Conf) : Config(Conf), Running(false)
{
#ifdef HLQUERY_HAS_OPENSSL
     SSLCtx = nullptr;
#endif

     /* Register default routes. */

     RegisterRoute("GET", "/health", [this](const HttpRequest &Req)
                   {
                        return HandleHealth(Req);
                   });

     RegisterRoute("GET", "/ready", [this](const HttpRequest &Req)
                   {
                        return HandleReady(Req);
                   });

     RegisterRoute("GET", "/", [](const HttpRequest & /* Req */)
                   {
                        HttpResponse Resp(200, "OK");

                        Resp.Body = "hlquery HTTP Server\nAvailable endpoints:\n- GET /health\n- GET /ready\n";

                        return Resp;
                   });
}

/* Releases HTTP server resources on shutdown. */

HttpServer::~HttpServer()
{
     Stop();
}

/* Start starts the HTTP server. */

bool HttpServer::Start()
{
     /*
     * Perform early SSL validation before allocating system resources.
     * If SSL is requested but not available or certificates are missing,
     * we fail fast to avoid partial initialization.
     */

     if (Config.ssl)
     {
          std::string ErrorMsg;

          if (!ValidateSSLConfig(Config, &ErrorMsg))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server.", ErrorMsg);
               }

               ConsoleWriter::WriteError("[FATAL] " + ErrorMsg, true);

               return false;
          }
     }

     /* Create socket. */

     int ServerFD = socket(AF_INET, SOCK_STREAM, 0);

     if (ServerFD < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to create socket: " + std::string(strerror(errno)) + ".");
          }

          return false;
     }

     /* Set socket options. */
     /* IMPROVEMENT: Set SO_REUSEADDR and SO_REUSEPORT on the server socket before binding. */
     /* to allow immediate reuse of the port after a restart, preventing "address already in use" errors. */

     int Opt = 1;

     if (setsockopt(ServerFD, SOL_SOCKET, SO_REUSEADDR, &Opt, sizeof(Opt)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to set SO_REUSEADDR: " + std::string(strerror(errno)) + ".");
          }

          close(ServerFD);

          return false;
     }

     /* Set SO_REUSEPORT for load balancing across multiple processes (if supported). */

#ifdef SO_REUSEPORT
     if (setsockopt(ServerFD, SOL_SOCKET, SO_REUSEPORT, &Opt, sizeof(Opt)) < 0)
     {
          /* SO_REUSEPORT may not be supported on all systems - log but don't fail. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("httpserver", "SO_REUSEPORT not supported: " + std::string(strerror(errno)) + ".");
          }
     }
#endif

     /* Bug #12 Fix: Enable TCP_FASTOPEN for lower latency. */

#ifdef TCP_FASTOPEN
     int QLen = 5; /* Fast Open queue length. */

     if (setsockopt(ServerFD, IPPROTO_TCP, TCP_FASTOPEN, &QLen, sizeof(QLen)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("httpserver", "TCP_FASTOPEN not supported or failed: " + std::string(strerror(errno)) + ".");
          }
     }
#endif

     /* Bind to port. */

     struct sockaddr_in Address;

     Address.sin_family = AF_INET;
     Address.sin_addr.s_addr = inet_addr(Config.address.c_str());
     Address.sin_port = htons(Config.port);

     if (bind(ServerFD, reinterpret_cast<struct sockaddr *>(&Address), sizeof(Address)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to bind to port " + std::to_string(Config.port) + ": " + std::string(strerror(errno)) + ".");
          }

          close(ServerFD);

          return false;
     }

#ifdef HLQUERY_HAS_OPENSSL
     if (Config.ssl)
     {
          /* Initialize SSL context. */

          const SSL_METHOD *SSLMethod = TLS_server_method();

          SSLCtx = SSL_CTX_new(SSLMethod);

          if (!SSLCtx)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server", "Failed to create SSL context.");
               }

               close(ServerFD);

               return false;
          }

          /* Set SSL protocols. */

          if (Config.ssl_protocols.find("TLSv1.3") != std::string::npos)
          {
               SSL_CTX_set_min_proto_version(SSLCtx, TLS1_3_VERSION);
          }
          else if (Config.ssl_protocols.find("TLSv1.2") != std::string::npos)
          {
               SSL_CTX_set_min_proto_version(SSLCtx, TLS1_2_VERSION);
          }

          /* Set cipher list. */

          if (!Config.ssl_ciphers.empty())
          {
               SSL_CTX_set_cipher_list(SSLCtx, Config.ssl_ciphers.c_str());
          }

          /* Load certificate and private key. */

          if (SSL_CTX_use_certificate_chain_file(SSLCtx, Config.ssl_cert.c_str()) <= 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server", "Failed to load SSL certificate chain: " + Config.ssl_cert + " (check file format).");
               }

               ConsoleWriter::WriteError("[FATAL] Failed to load SSL certificate chain: " + Config.ssl_cert + " (check file format).", true);

               SSL_CTX_free(SSLCtx);
               SSLCtx = nullptr;
               close(ServerFD);

               return false;
          }

          if (SSL_CTX_use_PrivateKey_file(SSLCtx, Config.ssl_key.c_str(), SSL_FILETYPE_PEM) <= 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server", "Failed to load SSL private key: " + Config.ssl_key + " (check file format).");
               }

               ConsoleWriter::WriteError("[FATAL] Failed to load SSL private key: " + Config.ssl_key + " (check file format).", true);

               SSL_CTX_free(SSLCtx);
               SSLCtx = nullptr;
               close(ServerFD);

               return false;
          }

          if (!SSL_CTX_check_private_key(SSLCtx))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("http_server", "SSL private key does not match certificate: " + Config.ssl_key + ".");
               }

               ConsoleWriter::WriteError("[FATAL] SSL private key does not match certificate: " + Config.ssl_key, true);

               SSL_CTX_free(SSLCtx);
               SSLCtx = nullptr;
               close(ServerFD);

               return false;
          }
     }
#endif

     /* Listen for connections. */

     if (listen(ServerFD, 128) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to listen: " + std::string(strerror(errno)) + ".");
          }

          close(ServerFD);

          return false;
     }

     /* Set socket non-blocking. */

     int ServerFlags = fcntl(ServerFD, F_GETFL);

     if (ServerFlags >= 0)
     {
          fcntl(ServerFD, F_SETFL, ServerFlags | O_NONBLOCK);
     }

     /* Set CLOEXEC. */

     ServerFlags = fcntl(ServerFD, F_GETFD);

     if (ServerFlags >= 0)
     {
          fcntl(ServerFD, F_SETFD, ServerFlags | FD_CLOEXEC);
     }

     SetFD(ServerFD);

     /* Verify socket is in listening state before registering with epoll. */
     /* This ensures the socket is fully ready to accept connections. */

     int ListenState = 0;

     socklen_t ListenStateLen = sizeof(ListenState);

     if (getsockopt(ServerFD, SOL_SOCKET, SO_ACCEPTCONN, &ListenState, &ListenStateLen) == 0)
     {
          if (!ListenState)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("httpserver", "Socket is not in listening state after listen() call - socket may be invalid.");
               }

               close(ServerFD);

               return false;
          }
     }

     /* Register with socketengine. */

     if (!SocketEngine::AddFD(this, EPOLLIN))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to register HTTP server socket (fd=" + std::to_string(ServerFD) + ") with socketengine - server will not accept connections!.");
          }

          close(ServerFD);
          SetFD(-1);

          return false;
     }

     Running = true;

     /* Verify socket is actually registered and ready. */

     int EpollFD = SocketEngine::GetEpollFD();

     /*
     * Double-check socket is still valid and listening.
     * Verify the socket FD is still valid and the port is actually bound.
     */

     struct sockaddr_in CheckAddr;

     socklen_t CheckLen = sizeof(CheckAddr);

     if (getsockname(ServerFD, reinterpret_cast<struct sockaddr *>(&CheckAddr), &CheckLen) == 0)
     {
          int BoundPort = ntohs(CheckAddr.sin_port);

          if (BoundPort != static_cast<int>(Config.port))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("httpserver", "Socket bound to wrong port! Expected " + std::to_string(Config.port) + ", got " + std::to_string(BoundPort) + ".");
               }

               close(ServerFD);
               SetFD(-1);

               return false;
          }
     }
     else
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("httpserver", "Failed to verify socket binding (getsockname failed: " + std::string(strerror(errno)) + ").");
          }

          close(ServerFD);
          SetFD(-1);

          return false;
     }

     if (Instance && Instance->Logs)
     {
          std::string LogMsg = "HTTP server started on " + Config.address + ":" + std::to_string(Config.port) + " (fd=" + std::to_string(ServerFD) + ", epoll_fd=" + std::to_string(EpollFD) + ", type=" + Config.type + ", ssl=" + std::string(Config.ssl ? "yes" : "no") + ", verified bound and listening, registered with epoll, ready to accept connections).";

          Instance->Logs->Normal("httpserver.", LogMsg);
     }

     return true;
}

/* SetReadyToAccept sets ready to accept flag. */

/* Updates set ready to accept values. */

void HttpServer::SetReadyToAccept(bool Ready)
{
     bool OldValue = ReadyToAcceptValue.load();

     ReadyToAcceptValue.store(Ready);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode() && OldValue != Ready)
     {
          Instance->Logs->Debug("httpserver", "SetReadyToAccept: " + std::string(Ready ? "true" : "false") + " (was: " + std::string(OldValue ? "true" : "false") + ").");
     }
}

/* SetLoading sets loading flag. */

/* Updates set loading values. */

void HttpServer::SetLoading(bool Loading)
{
     bool OldValue = IsLoadingValue.load();

     IsLoadingValue.store(Loading);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode() && OldValue != Loading)
     {
          Instance->Logs->Debug("httpserver", "SetLoading: " + std::string(Loading ? "true" : "false") + " (was: " + std::string(OldValue ? "true" : "false") + ").");
     }
}

/* Stop stops HTTP server. */

void HttpServer::Stop()
{
     if (!Running)
     {
          return;
     }

     Running = false;

     /* Force-close live connections so shutdown does not wait on keep-alive peers. */

     {
          std::lock_guard<std::mutex> Lock(ConnectionsMutex);
          for (auto &Conn : Connections)
          {
               if (Conn)
               {
                    Conn->ForceClose();
               }
          }
     }

     CleanupConnections();

     /* Remove from socketengine and close socket. */

     if (HasFD())
     {
          /* FIX: Cache fd before DelFD() to prevent use-after-free. */

          int FDValue = GetFD();

          SocketEngine::DelFD(this);

          if (FDValue >= 0)
          {
               close(FDValue);
          }

          SetFD(-1);
     }

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLCtx)
     {
          SSL_CTX_free(SSLCtx);
          SSLCtx = nullptr;
     }
#endif

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("httpserver", "HTTP server stopped.");
     }
}

/* RegisterRoute registers HTTP route. */

void HttpServer::RegisterRoute(const std::string &Method, const std::string &Path, std::function<HttpResponse(const HttpRequest &)> Handler)
{
     std::string RouteKey = Method + " " + Path;

     Routes[RouteKey] = Handler;
}

/* HandleNotFound handles not found requests. */

HttpResponse HttpServer::HandleNotFound(const HttpRequest &Request)
{
     /* Always return JSON response for API clients. */

     return HttpResponseBuilder::NotFound(Request.Path, Request.Method)
          .Build();
}

/* HandleHealth handles health check requests. */

HttpResponse HttpServer::HandleHealth(const HttpRequest &Request)
{
     auto HeaderTruthy = [&](const std::string &Name) -> bool
     {
          for (const auto &Pair : Request.Headers)
          {
               std::string Key = Pair.first;
               std::string Value = Pair.second;
               std::transform(Key.begin(), Key.end(), Key.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               std::transform(Value.begin(), Value.end(), Value.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (Key == Name && (Value == "1" || Value == "true" || Value == "yes" || Value == "on"))
               {
                    return true;
               }
          }
          return false;
     };

     if (HeaderTruthy("x-hlq-link-ping") && Instance && Instance->Logs)
     {
          const std::string Remote = Request.RemoteAddress.empty() ? std::string("unknown") : Request.RemoteAddress;
          Instance->Logs->Normal("links", "Ping received from " + Remote + ":" + std::to_string(Request.RemotePort) + " on /health.");
     }

     bool HealthDegraded = false;
     std::string HealthReason;

     if (Instance)
     {
          const auto HealthStatus = Instance->StatsVal.GetHealthStatus();
          HealthDegraded = HealthStatus.Degraded;
          HealthReason = HealthStatus.Reason;
     }

     HttpResponseBuilder Builder(200, "OK");

     Builder.Add("status", HealthDegraded ? "degraded" : "healthy")
          .Add("server", "hlquery")
          .Add("health_degraded", HealthDegraded);

     if (!HealthReason.empty())
     {
          Builder.Add("health_reason", HealthReason);
     }

     return Builder.Build();
}

/* Handles ready requests. */

HttpResponse HttpServer::HandleReady(const HttpRequest &Request)
{
     return SearchAPI::GetInstance().HandleReady(Request);
}

/* OnEventHandlerRead handles read events. */

void HttpServer::OnEventHandlerRead()
{
     AcceptConnection();
}

/*
 * CRITICAL FIX: Don't stop server on transient errors.
 * EINTR, EAGAIN, and other transient errors are normal and shouldn't stop the server.
 * Only stop on fatal errors that indicate the socket is truly broken.
 */

void HttpServer::OnEventHandlerError(int ErrorNum)
{
     if (ErrorNum == EINTR || ErrorNum == EAGAIN || ErrorNum == EWOULDBLOCK)
     {
          /* Transient error - log but don't stop. */

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("httpserver", "Server socket transient error (ignored): " + std::string(strerror(ErrorNum)) + ".");
          }

          return;
     }

     /* Fatal error - log and stop server. */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Critical("httpserver", "Server socket fatal error: " + std::string(strerror(ErrorNum)) + " - stopping server.");
     }

     Stop();
}

/*
 * CRITICAL FIX: Check ReadyToAcceptValue flag before accepting connections.
 * In daemon mode, this flag is set to false during initialization and set to true
 * after collections are loaded. This prevents queries from hanging on connections
 * that are accepted but can't be processed yet.
 */

void HttpServer::AcceptConnection()
{
     const bool ReadyToAccept = ReadyToAcceptValue.load();

     if (!ReadyToAccept)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "[AcceptConnection] Server not ready; accepting and rejecting queued clients with 503.");
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("http_server", "[AcceptConnection] ENTRY - server fd=" + std::to_string(GetFD()) + ".");
     }

     struct sockaddr_in ClientAddr;

     socklen_t ClientLen = sizeof(ClientAddr);

     /* Accept multiple connections per tick for better performance (similar to ListenManager). */

     int ConnectionsAccepted = 0;
     bool AcceptSliceLimitReached = false;

     while (ConnectionsAccepted < HTTP_MAX_ACCEPTS_PER_TICK)
     {
          ClientLen = sizeof(ClientAddr);
          int ClientFD = accept(GetFD(), reinterpret_cast<struct sockaddr *>(&ClientAddr), &ClientLen);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_server", "[AcceptConnection] accept() returned fd=" + std::to_string(ClientFD) + ", errno=" + (ClientFD < 0 ? std::string(strerror(errno)) : "0") + ".");
          }

          if (ClientFD < 0)
          {
               if (errno == EAGAIN)
               {
                    break; /* No more pending connections. */
               }

               /* Handle specific errors gracefully. */

               if (errno == EMFILE || errno == ENFILE)
               {
                    /* Too many open files - log once and continue. */

                    static bool LoggedFileLimit = false;

                    if (!LoggedFileLimit)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Sparse("httpserver", "Accept failed: Too many open files - reducing connection rate.");
                         }

                         LoggedFileLimit = true;
                    }

                    break;
               }
               else if (errno == ECONNABORTED || errno == EINTR)
               {
                    /* Connection aborted or interrupted - continue normally. */

                    continue;
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("httpserver", "Accept failed: " + std::string(strerror(errno)) + ".");
               }

               break;
          }

          /* Get client info. */

          char IPBuffer[INET_ADDRSTRLEN];

          inet_ntop(AF_INET, &ClientAddr.sin_addr, IPBuffer, INET_ADDRSTRLEN);

          std::string ClientIP = IPBuffer;

          int ClientPort = ntohs(ClientAddr.sin_port);

          if (!ReadyToAccept)
          {
               const std::string ErrorResponse = BuildBackpressureRawResponse(
                    "startup",
                    "Server is still loading; retry shortly.");

               send(ClientFD, ErrorResponse.c_str(), ErrorResponse.length(), MSG_NOSIGNAL);
               close(ClientFD);
               ConnectionsAccepted++;
               continue;
          }

          /* Check IP allow filter if enabled. */

          if (Instance && Instance->IPFilter && Instance->IPFilter->IsEnabled())
          {
               if (!Instance->IPFilter->IsAllowed(std::string(ClientIP)))
               {
                    if (Instance && Instance->Logs)
                    {
                         std::string LogMsg = "Connection blocked from IP: " + std::string(ClientIP);

                         /* If DNS is enabled, include hostname in log. */

                         if (Instance->IPFilter->IsDNSEnabled())
                         {
                              std::string Hostname = Instance->IPFilter->GetHostnameForIP(std::string(ClientIP));

                              if (!Hostname.empty())
                              {
                                   LogMsg += " (" + Hostname + ")";
                              }
                         }

                         LogMsg += " (IP not allowed).";

                         Instance->Logs->Normal("httpserver.", LogMsg);
                    }

                    /* Send HTTP 403 Forbidden response before closing. */

                    std::string ErrorResponse = "HTTP/1.1 403 Forbidden\r\n";

                    ErrorResponse += "Content-Type: application/json\r\n";
                    ErrorResponse += "Server: hlquery/1.0\r\n";
                    ErrorResponse += "Connection: close\r\n";
                    ErrorResponse += "\r\n";
                    ErrorResponse += "{\"error\":\"IP not allowed\",\"message\":\"Your IP address (" + std::string(ClientIP) + ") is not in the allowed list\",\"status\":403}\r\n";

                    /* Send response (non-blocking, best effort). */

                    send(ClientFD, ErrorResponse.c_str(), ErrorResponse.length(), MSG_NOSIGNAL);

                    close(ClientFD);

                    continue; /* Skip this connection. */
               }
          }

          /* Set client socket non-blocking. */

          int ClientFlags = fcntl(ClientFD, F_GETFL);

          if (ClientFlags >= 0)
          {
               fcntl(ClientFD, F_SETFL, ClientFlags | O_NONBLOCK);
          }

          /* Set CLOEXEC. */

          ClientFlags = fcntl(ClientFD, F_GETFD);

          if (ClientFlags >= 0)
          {
               fcntl(ClientFD, F_SETFD, ClientFlags | FD_CLOEXEC);
          }

          /* Check connection limit before accepting. */

          {
               std::lock_guard<std::mutex> Lock(ConnectionsMutex);

               /*
     * Get max_connections from config, fallback to MAX_CONNECTIONS if config not available.
     * Prevent connection exhaustion - reject if too many connections.
     */

               int MaxConn = (Instance && Instance->Config) ? Instance->Config->GetMaxConnections() : MAX_CONNECTIONS;

               if (Connections.size() >= static_cast<size_t>(MaxConn))
               {
                    const uint64_t RejectCount = BackpressureConnectionRejects.fetch_add(1, std::memory_order_relaxed) + 1;

                    const std::string ErrorResponse = BuildBackpressureRawResponse(
                         "max_connections",
                         "Maximum concurrent connections reached; retry shortly.");

                    send(ClientFD, ErrorResponse.c_str(), ErrorResponse.length(), MSG_NOSIGNAL);
                    close(ClientFD);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Sparse("httpserver", "Backpressure: rejecting connection at max_connections=" + std::to_string(MaxConn) + ", reject_count=" + std::to_string(RejectCount) + ".");
                    }

                    continue; /* Skip this connection. */
               }
          }

          /* Create connection handler. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_server", "[AcceptConnection] Creating HttpConnection for client " + std::string(ClientIP) + ":" + std::to_string(ClientPort) + ", fd=" + std::to_string(ClientFD) + ".");
          }

          auto NewConnection = std::make_unique<HttpConnection>(ClientFD, std::string(ClientIP), ClientPort, ThreadPoolValue);

#ifdef HLQUERY_HAS_OPENSSL
          if (Config.ssl && SSLCtx)
          {
               SSL *SSLVal = SSL_new(SSLCtx);

               if (SSLVal)
               {
                    SSL_set_fd(SSLVal, ClientFD);
                    NewConnection->SetSSL(SSLVal);
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("httpserver", "Failed to create SSL object for new connection.");
                    }

                    close(ClientFD);

                    continue;
               }
          }
#endif

          /* Register with socketengine. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_server", "[AcceptConnection] About to register fd=" + std::to_string(ClientFD) + " with SocketEngine.");
          }

          if (SocketEngine::AddFD(NewConnection.get(), EPOLLIN))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", "[AcceptConnection] Successfully registered fd=" + std::to_string(ClientFD) + " with SocketEngine.");
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("httpserver", "AcceptConnection: Registered connection from " + std::string(ClientIP) + ":" + std::to_string(ClientPort) + " (fd=" + std::to_string(ClientFD) + ", total_connections=" + std::to_string(Connections.size() + 1) + ").");
               }

               /* Protect Connections vector with mutex. */

               HttpConnection *RegisteredConnection = NewConnection.get();

               {
                    std::lock_guard<std::mutex> Lock(ConnectionsMutex);

                    Connections.push_back(std::move(NewConnection));
               }

               /*
                * Drain any request bytes that arrived before or during epoll registration.
                * With edge-triggered epoll, relying only on a later readiness edge can leave
                * a freshly accepted client idle until the peer times out.
                */
               if (RegisteredConnection && RegisteredConnection->HasFD())
               {
                    RegisteredConnection->OnEventHandlerRead();
               }

               /* Active connection counter is incremented in AddFD(). */

               if (Instance && Instance->Logs)
               {
                    std::string LogMsg = "Accepted HTTP connection from " + std::string(ClientIP) + ":" + std::to_string(ClientPort);

                    /* If DNS is enabled, include hostname in log. */

                    if (Instance->IPFilter && Instance->IPFilter->IsDNSEnabled())
                    {
                         std::string Hostname = Instance->IPFilter->GetHostnameForIP(std::string(ClientIP));

                         if (!Hostname.empty())
                         {
                              LogMsg += " (" + Hostname + ")";
                         }
                    }

                    Instance->Logs->Debug("httpserver", LogMsg + ".");
               }
          }
          else
          {
               close(ClientFD);

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("httpserver", "Failed to register client connection.");
               }
          }

          ConnectionsAccepted++;
     }

     if (ConnectionsAccepted >= HTTP_MAX_ACCEPTS_PER_TICK)
     {
          AcceptSliceLimitReached = true;
     }

     if (AcceptSliceLimitReached)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_server", "[AcceptConnection] Accepted " + std::to_string(HTTP_MAX_ACCEPTS_PER_TICK) + " connections; rearming HTTP listener.");
          }

          SocketEngine::DelFD(this);
          SocketEngine::AddFD(this, EPOLLIN);
     }

     /*
     * CRITICAL FIX: Cleanup connections more aggressively to prevent accumulation.
     * This prevents second benchmark from being slow due to thousands of stale connections.
     * Always cleanup after accepting connections to prevent hangs.
     */

     CleanupConnections();
}

/* CleanupConnections cleans up closed connections. */

void HttpServer::CleanupConnections()
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("httpserver", "CleanupConnections: ENTRY.");
     }

     /* Protect Connections vector access with mutex to prevent race conditions. */

     std::lock_guard<std::mutex> Lock(ConnectionsMutex);

     size_t InitialSize = Connections.size();

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("httpserver", "CleanupConnections: Locked, InitialSize=" + std::to_string(InitialSize) + ".");
     }

     if (InitialSize == 0)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("httpserver", "CleanupConnections: No connections to clean up.");
          }

          return; /* Nothing to clean up. */
     }

     /*
     * Mark connections as closing before cleanup to prevent use-after-free.
     * CleanupConnection() will set ClosingValue flag internally.
     */

     for (auto &Conn : Connections)
     {
          if (Conn && Conn->HasFD())
          {
               Conn->CleanupConnection();
          }
     }

     /*
     * Then remove dead connections from vector.
     * Only remove connections that are fully closed (no FD).
     */

     auto It = std::remove_if(Connections.begin(), Connections.end(),
                              [](const std::unique_ptr<HttpConnection> &Conn)
                              {
                                   if (!Conn)
                                   {
                                        return true; /* Remove null pointers. */
                                   }

                                   if (!Conn->HasFD() && !Conn->HasActiveRequests())
                                   {
                                        return true; /* Remove connections without FD. */
                                   }

                                   return false;
                              });

     size_t RemovedCount = std::distance(It, Connections.end());

     Connections.erase(It, Connections.end());

     size_t FinalSize = Connections.size();

     /* Log if we cleaned up a significant number of connections. */

     if (RemovedCount > 50 && Instance && Instance->Logs)
     {
          Instance->Logs->Debug("httpserver", "CleanupConnections: Removed " + std::to_string(RemovedCount) + " dead connections (remaining: " + std::to_string(FinalSize) + ", was: " + std::to_string(InitialSize) + ").");
     }

     /* If we still have too many connections, force cleanup of old ones. */

     int MaxConn = (Instance && Instance->Config) ? Instance->Config->GetMaxConnections() : MAX_CONNECTIONS;

     if (FinalSize > static_cast<size_t>(MaxConn * 0.8))
     {
          /* Force cleanup connections that haven't been active recently. */

          for (auto &Conn : Connections)
          {
               if (Conn && Conn->HasFD())
               {
                    /* Check last activity time - if connection is idle for too long, close it. */
                    /* This is handled by CleanupConnection() timeout check, but we can be more aggressive. */

                    Conn->CleanupConnection();
               }
          }
     }
}

/* Helper to map RouteAction to APIKeyAction. */

APIKeyAction MapRouteToKeyAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::DocumentSearch:
          case RouteAction::VectorSearch:
          case RouteAction::MultiSearch:
          case RouteAction::GlobalSearch:
          case RouteAction::GetDocument:
          case RouteAction::ListDocuments:
          case RouteAction::GetDocumentContext:
          case RouteAction::FacetCounts:
          case RouteAction::ExportDocuments:
               return APIKeyAction::SEARCH;
          case RouteAction::MaybeSuggest:
               return APIKeyAction::SEARCH;

          case RouteAction::AddDocument:
               return APIKeyAction::CREATE;

          case RouteAction::UpdateDocument:
          case RouteAction::UpdateByQuery:
               return APIKeyAction::UPDATE;

          case RouteAction::DeleteDocument:
          case RouteAction::DeleteByQuery:
          case RouteAction::DeleteDocumentsByFilter:
               return APIKeyAction::DELETE;

          case RouteAction::ListCollections:
          case RouteAction::ListCollectionsDistributed:
          case RouteAction::GetCollection:
          case RouteAction::GetCollectionLanguage:
          case RouteAction::ListSynonyms:
          case RouteAction::ListGlobalSynonyms:
          case RouteAction::GetSynonym:
          case RouteAction::GetGlobalSynonym:
          case RouteAction::ListStopwords:
          case RouteAction::ListGlobalStopwords:
          case RouteAction::ListOverrides:
          case RouteAction::GetOverride:
          case RouteAction::ListAliases:
          case RouteAction::GetAlias:
               return APIKeyAction::COLLECTIONS_LIST;

          case RouteAction::CreateCollection:
          case RouteAction::UpsertSynonym:
          case RouteAction::UpsertGlobalSynonym:
          case RouteAction::CreateStopword:
          case RouteAction::CreateGlobalStopword:
          case RouteAction::UpsertOverride:
          case RouteAction::UpsertAlias:
               return APIKeyAction::COLLECTIONS_CREATE;

          case RouteAction::DeleteCollection:
          case RouteAction::DeleteSynonym:
          case RouteAction::DeleteGlobalSynonym:
          case RouteAction::DeleteStopword:
          case RouteAction::DeleteGlobalStopword:
          case RouteAction::DeleteOverride:
          case RouteAction::DeleteAlias:
               return APIKeyAction::COLLECTIONS_DELETE;

          case RouteAction::BulkImportDocuments:
               return APIKeyAction::IMPORT;

          default:
               return APIKeyAction::SEARCH;
     }
}

/* Resolves route with fallback values. */

static RouteAction ResolveRouteWithFallback(const HttpRequest &Request)
{
     return BuildRouteContext(Request).ActionVal;
}

/* Checks whether public route action applies. */

static bool IsPublicRouteAction(RouteAction ActionVal)
{
     return (ActionVal == RouteAction::Health ||
             ActionVal == RouteAction::Ready ||
             ActionVal == RouteAction::Status ||
             ActionVal == RouteAction::SearchConfig ||
             ActionVal == RouteAction::Ping ||
             ActionVal == RouteAction::LinksList ||
             ActionVal == RouteAction::LinksPing);
}

/* Checks whether admin only route action applies. */

static bool IsAdminOnlyRouteAction(RouteAction ActionVal)
{
     return (ActionVal == RouteAction::ListKeys ||
             ActionVal == RouteAction::CreateKey ||
             ActionVal == RouteAction::GetKey ||
             ActionVal == RouteAction::DeleteKey ||
             ActionVal == RouteAction::UpdateKey ||
             ActionVal == RouteAction::ConfigFiles ||
             ActionVal == RouteAction::ListPresets ||
             ActionVal == RouteAction::UpsertPreset ||
             ActionVal == RouteAction::GetPreset ||
             ActionVal == RouteAction::DeletePreset ||
             ActionVal == RouteAction::ListUsers ||
             ActionVal == RouteAction::CreateUser ||
             ActionVal == RouteAction::GetUser ||
             ActionVal == RouteAction::DeleteUser ||
             ActionVal == RouteAction::UpdateUser ||
             ActionVal == RouteAction::LinksConnect ||
             ActionVal == RouteAction::LinksDisconnect ||
             ActionVal == RouteAction::Flush ||
             ActionVal == RouteAction::Repair ||
             ActionVal == RouteAction::Cache ||
             ActionVal == RouteAction::ModuleLoad ||
             ActionVal == RouteAction::ModuleUnload ||
             ActionVal == RouteAction::StorageStatus);
}

/* Normalizes request path values. */

static std::string NormalizeRequestPath(const std::string &Path)
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

/* Checks whether module control route path applies. */

static bool IsModuleControlRoutePath(const std::string &Path)
{
     return Path == "/loadmodule" ||
            Path == "/unloadmodule" ||
            Path.rfind("/loadmodule/", 0) == 0 ||
            Path.rfind("/unloadmodule/", 0) == 0 ||
            Path == "/modules/load" ||
            Path == "/modules/unload" ||
            Path.rfind("/modules/load/", 0) == 0 ||
            Path.rfind("/modules/unload/", 0) == 0;
}

/* Builds route context data. */

static RouteContext BuildRouteContext(const HttpRequest &Request, SearchAPI *API)
{
     RouteContext Context;
     Context.NormalizedPath = NormalizeRequestPath(Request.Path);
     Context.ActionVal = ResolveHttpRoute(Request);
     Context.IsPublic = IsPublicRouteAction(Context.ActionVal);
     Context.IsAdminOnly = IsAdminOnlyRouteAction(Context.ActionVal);
     Context.IsModuleControl = IsModuleControlRoutePath(Context.NormalizedPath);
     Context.IsHealthCheck = IsHealthLikePath(Context.NormalizedPath);
     Context.IsCollectionCreation = Context.ActionVal == RouteAction::CreateCollection;
     Context.IsDocumentImport = Context.ActionVal == RouteAction::AddDocument ||
                                Context.ActionVal == RouteAction::BulkImportDocuments ||
                                IsDocumentIngestionRequest(Request.Method, Context.NormalizedPath);
     Context.IsListDocuments = Context.ActionVal == RouteAction::ListDocuments;
     Context.IsExpensiveQuery = Context.ActionVal == RouteAction::DocumentSearch ||
                                Context.ActionVal == RouteAction::VectorSearch ||
                                Context.ActionVal == RouteAction::MultiSearch ||
                                Context.ActionVal == RouteAction::GlobalSearch;

     if (API && !Context.IsPublic && Context.ActionVal != RouteAction::MultiSearch &&
         Context.ActionVal != RouteAction::GlobalSearch)
     {
          Context.CollectionName = API->ExtractCollectionFromPath(Context.NormalizedPath);
     }

     return Context;
}

/* ProcessRequestWithAPI handles requests with SearchAPI. */

HttpResponse ProcessRequestWithAPI(SearchAPI &API, const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("http_server", "ProcessRequestWithAPI called with path: " + Request.Path + ", method: " + Request.Method + ".");
     }

     RouteAction ActionVal = RouteAction::NotFound;
     RouteContext Context;

     try
     {
          Context = BuildRouteContext(Request, &API);
          ActionVal = Context.ActionVal;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("http_server", "ResolveHttpRoute returned: " + std::string(RouteActionName(ActionVal)) + " for path: " + Request.Path + " method: " + Request.Method + ".");
          }

          /* API Key and Admin permission check. */

          bool IsAdminVal = false;
          std::string TokenVal = "";

          auto AuthIt = Request.Headers.find("Authorization");
          if (AuthIt == Request.Headers.end())
          {
               AuthIt = Request.Headers.find("authorization");
          }

          if (AuthIt != Request.Headers.end())
          {
               TokenVal = AuthIt->second;
          }
          else
          {
               auto APIKeyIt = Request.Headers.find("X-API-Key");
               if (APIKeyIt == Request.Headers.end())
               {
                    APIKeyIt = Request.Headers.find("x-api-key");
               }

               if (APIKeyIt == Request.Headers.end())
               {
                    APIKeyIt = Request.Headers.find("X-TYPESENSE-API-KEY");
               }

               if (APIKeyIt == Request.Headers.end())
               {
                    APIKeyIt = Request.Headers.find("x-typesense-api-key");
               }

               if (APIKeyIt != Request.Headers.end())
               {
                    TokenVal = APIKeyIt->second;
               }
          }

          if (TokenVal.find("Bearer ") == 0)
          {
               TokenVal = TokenVal.substr(7);
          }

          if (Instance && Instance->Users && Instance->Users->IsAuthEnabled())
          {
               IsAdminVal = Instance->Users->IsAdmin(TokenVal);
          }
          else
          {
               IsAdminVal = true;
          }

          /* Admin-only routes. */

          if (Context.IsAdminOnly)
          {
               if (!IsAdminVal)
               {
                    LogAccessControl("Forbidden: non-admin attempted admin-only operation", Request);
                    return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Only administrators can access this endpoint\"}");
               }
          }

          if (Context.IsModuleControl && !IsAdminVal)
          {
               LogAccessControl("Forbidden: non-admin attempted module control operation", Request);
               return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Only administrators can access this endpoint\"}");
          }

          if (!IsAdminVal && !Context.IsPublic)
          {
               APIKey KeyObj;

               if (APIKeyManager::Instance().ValidateKey(TokenVal, &KeyObj))
               {
                    if (!APIKeyManager::Instance().CheckRateLimit(KeyObj.ID))
                    {
                         return HttpResponse(http_code::TOO_MANY_REQUESTS, StatusText(http_code::TOO_MANY_REQUESTS), "{\"error\":\"Rate limit exceeded\"}");
                    }

                    APIKeyAction ReqAction = MapRouteToKeyAction(ActionVal);
                    std::string ColNameVal = Context.CollectionName;

                    /* Handle /multi_search and system endpoints. */

                    if (ColNameVal.empty())
                    {
                         if (ActionVal == RouteAction::MultiSearch || ActionVal == RouteAction::GlobalSearch)
                         {
                              /* Allowed at this level, HandleMultiSearch will check individual collections. */
                         }
                         else
                         {
                              /* System-wide endpoint (e.g. /stats, /health, etc.) - check for wildcard scope. */

                              if (!KeyObj.CanAccessCollection("*"))
                              {
                                   LogAccessControl("Forbidden: key '" + KeyObj.ID + "' cannot access system endpoints", Request);
                                   return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Access to system endpoints not allowed for this key\"}");
                              }

                              if (!KeyObj.HasAction("*", ReqAction))
                              {
                                   LogAccessControl("Forbidden: key '" + KeyObj.ID + "' action not allowed for system endpoints", Request);
                                   return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Action not allowed for system endpoints with this key\"}");
                              }

                              ColNameVal = "*";
                         }
                    }
                    else
                    {
                         if (!KeyObj.CanAccessCollection(ColNameVal))
                         {
                              LogAccessControl("Forbidden: key '" + KeyObj.ID + "' cannot access collection '" + ColNameVal + "'", Request);
                              return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Access to collection '" + ColNameVal + "' not allowed for this key\"}");
                         }

                         if (!KeyObj.HasAction(ColNameVal, ReqAction))
                         {
                              LogAccessControl("Forbidden: key '" + KeyObj.ID + "' action not allowed for collection '" + ColNameVal + "'", Request);
                              return HttpResponse(http_code::FORBIDDEN, StatusText(http_code::FORBIDDEN), "{\"error\":\"Action not allowed for this collection with this key\"}");
                         }
                    }

                    APIKeyManager::Instance().UpdateLastUsed(KeyObj.ID);

                    /* Scoped search: inject key metadata into request. */

                    HttpRequest &ModRequest = const_cast<HttpRequest &>(Request);
                    ModRequest.APIKeyID = KeyObj.ID;
                    ModRequest.EmbeddedFilters = KeyObj.GetEmbeddedFilters(ColNameVal);
               }
               else if (Instance && Instance->Users && Instance->Users->IsAuthEnabled())
               {
                    if (!Instance->Users->IsUser(TokenVal))
                    {
                         LogAccessControl("Unauthorized: invalid token", Request);
                         return HttpResponse(http_code::UNAUTHORIZED, StatusText(http_code::UNAUTHORIZED), "{\"error\":\"Invalid token\"}");
                    }
               }
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "EXCEPTION in ResolveHttpRoute: " + std::string(E.what()) + " for path: " + Request.Path + ".");

               ConsoleWriter::WriteError("EXCEPTION in ResolveHttpRoute: " + std::string(E.what()) + " for path: " + Request.Path + ".");
          }

          ActionVal = RouteAction::NotFound;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "UNKNOWN EXCEPTION in ResolveHttpRoute for path: " + Request.Path + ".");

               ConsoleWriter::WriteError("UNKNOWN EXCEPTION in ResolveHttpRoute for path: " + Request.Path + ".");
          }

          ActionVal = RouteAction::NotFound;
     }

     /* Block requests that need accurate data until collections are loaded. */
     /* Block queries until collections are loaded after restart, but allow write operations. */
     /* This ensures queries return accurate results with all collections available. */
     /* Allow collection creation and document import during loading (needed for benchmarks). */

     if (Instance && !HybridStorageManagerInstance().IsMetadataScanComplete())
     {
          if (Context.IsExpensiveQuery && !Context.IsCollectionCreation && !Context.IsDocumentImport && !Context.IsListDocuments)
          {
               HttpResponse ResponseVal(503, "Service Unavailable", "application/json");

               ResponseVal.Body = "{\"error\":\"Initializing\",\"message\":\"Server is still loading collections after restart. Expensive queries are blocked until all collections are loaded. Write operations and simple reads are allowed.\"}";
               ResponseVal.Headers["Retry-After"] = "5";

               return ResponseVal;
          }

          /* Collection creation, document import, and ListDocuments are allowed - proceed to handle the request. */
     }

     /* CRITICAL FIX: Block queries during sync, but allow collection creation and document import. */
     /* This check is redundant but kept for safety (sync check also done earlier). */

     if (Instance && Instance->IsSyncInProgress())
     {
          /* Block queries and other write operations during sync. */

          bool IsQuery = (Request.Method == "GET");

          bool IsOtherWrite = (Request.Method == "POST" || Request.Method == "PUT" || Request.Method == "DELETE") &&
                              !Context.IsCollectionCreation && !Context.IsDocumentImport;

          if (!IsAuthorizedReplicationRequest(Request) && !Context.IsHealthCheck && (IsQuery || IsOtherWrite))
          {
               HttpResponse ResponseVal(503, "Service Unavailable", "application/json");

               ResponseVal.Body = "{\"error\":\"Database sync in progress\",\"message\":\"Database is currently syncing. Queries and most write operations are blocked until sync completes. Collection creation and document import are allowed.\"}";

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_server", "Blocked request during sync: " + Request.Method + " " + Request.Path + ".");
               }

               return ResponseVal;
          }
     }

     try
     {
          switch (ActionVal)
          {
               case RouteAction::Status:
                    return API.HandleStatus(Request);

               case RouteAction::SearchConfig:
                    return API.HandleSearchConfig(Request);

               case RouteAction::ConfigFiles:
                    return API.HandleConfigFiles(Request);

               case RouteAction::LinksList:
                    return API.HandleLinksList(Request);

               case RouteAction::LinksPing:
                    return API.HandleLinksPing(Request);

               case RouteAction::LinksConnect:
                    return API.HandleLinksConnect(Request);

               case RouteAction::LinksDisconnect:
                    return API.HandleLinksDisconnect(Request);

               case RouteAction::Startup:
                    return API.HandleStartup(Request);

               case RouteAction::Integrity:
                    return API.HandleIntegrity(Request);

               case RouteAction::SelfCheck:
                    return API.HandleSelfCheck(Request);

               case RouteAction::ListAllSynonyms:
                    return API.HandleListAllSynonyms(Request);

               case RouteAction::ListGlobalSynonyms:
                    return API.HandleListGlobalSynonyms(Request);

               case RouteAction::Health:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Debug("http_server", "Routing to HandleHealth.");
                    }

                    return API.HandleHealth(Request);

               case RouteAction::Ready:
                    return API.HandleReady(Request);

               case RouteAction::Ping:
                    return API.HandlePing(Request);

               case RouteAction::Stats:
                    return API.HandleStats(Request);

               case RouteAction::Metrics:
                    return API.HandleMetrics(Request);

               case RouteAction::MetricsHistory:
                    return API.HandleMetricsHistory(Request);

               case RouteAction::Cache:
                    return API.HandleCache(Request);

               case RouteAction::Connections:
                    return API.HandleConnections(Request);

               case RouteAction::RocksDB:
                    return API.HandleRocksDB(Request);

               case RouteAction::DocTotal:
                    return API.HandleDocTotal(Request);

               case RouteAction::UpdateCounters:
                    return API.HandleUpdateCounters(Request);

               case RouteAction::DebugCounters:
                    return API.HandleDebugCounters(Request);

               case RouteAction::Repair:
                    return API.HandleRepair(Request);

               case RouteAction::StorageStatus:
                    return API.HandleStorageStatus(Request);

               case RouteAction::ListCollections:
                    return API.HandleListCollections(Request);

               case RouteAction::ListCollectionsDistributed:
                    return API.HandleListCollectionsDistributed(Request);

               case RouteAction::CreateCollection:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Debug("http_server", "Routing to HandleCreateCollection.");
                    }

                    return API.HandleCreateCollection(Request);

               case RouteAction::UpdateCollection:
                    return API.HandleUpdateCollection(Request);

               case RouteAction::VectorSearch:
                    return API.HandleVectorSearch(Request);

               case RouteAction::DocumentSearch:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Debug("http_server", "ProcessRequestWithAPI: Routing to HandleSearch for path: " + Request.Path + ".");
                    }

                    return API.HandleSearch(Request);

               case RouteAction::GetCollection:
                    return API.HandleGetCollection(Request);

               case RouteAction::GetCollectionLanguage:
                    return API.HandleGetCollectionLanguage(Request);

               case RouteAction::DeleteCollection:
                    return API.HandleDeleteCollection(Request);

               case RouteAction::Flush:
                    return API.HandleFlush(Request);

               case RouteAction::Etc:
                    return API.HandleEtc(Request);

               case RouteAction::ListUsers:
                    return API.HandleListUsers(Request);

               case RouteAction::CreateUser:
                    return API.HandleCreateUser(Request);

               case RouteAction::GetUser:
                    return API.HandleGetUser(Request);

               case RouteAction::DeleteUser:
                    return API.HandleDeleteUser(Request);

               case RouteAction::UpdateUser:
                    return API.HandleUpdateUser(Request);

               case RouteAction::ListDocuments:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Debug("http_server", "Routing to HandleListDocuments for path: " + Request.Path + ".");
                    }

                    return API.HandleListDocuments(Request);

               case RouteAction::GetDocument:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "ROUTING TO HandleGetDocument for path: " + Request.Path + ".");
                    }

                    return API.HandleGetDocument(Request);

               case RouteAction::GetDocumentContext:
                    return API.HandleGetDocumentContext(Request);
               case RouteAction::AddDocument:
                    return API.HandleAddDocument(Request);

               case RouteAction::BulkImportDocuments:
                    return API.HandleBulkImportDocuments(Request);

               case RouteAction::UpdateDocument:
                    return API.HandleUpdateDocument(Request);

               case RouteAction::DeleteDocument:
                    return API.HandleDeleteDocument(Request);

               case RouteAction::DeleteDocumentsByFilter:
                    return API.HandleDeleteDocumentsByFilter(Request);

               case RouteAction::UpdateByQuery:
                    return API.HandleUpdateByQuery(Request);

               case RouteAction::DeleteByQuery:
                    return API.HandleDeleteByQuery(Request);

               case RouteAction::FacetCounts:
                    return API.HandleFacetCounts(Request);

               case RouteAction::ExportDocuments:
                    return API.HandleExportDocuments(Request);

               case RouteAction::MaybeSuggest:
                    return API.HandleMaybe(Request);

               case RouteAction::ListSynonyms:
                    return API.HandleListSynonyms(Request);

               case RouteAction::UpsertSynonym:
                    return API.HandleCreateOrUpdateSynonym(Request);

               case RouteAction::GetSynonym:
                    return API.HandleGetSynonym(Request);

               case RouteAction::DeleteSynonym:
                    return API.HandleDeleteSynonym(Request);

               case RouteAction::UpsertGlobalSynonym:
                    return API.HandleCreateOrUpdateGlobalSynonym(Request);

               case RouteAction::GetGlobalSynonym:
                    return API.HandleGetGlobalSynonym(Request);

               case RouteAction::DeleteGlobalSynonym:
                    return API.HandleDeleteGlobalSynonym(Request);

               case RouteAction::ListAllStopwords:
                    return API.HandleListAllStopwords(Request);

               case RouteAction::ListGlobalStopwords:
                    return API.HandleListGlobalStopwords(Request);

               case RouteAction::ListStopwords:
                    return API.HandleListStopwords(Request);

               case RouteAction::CreateStopword:
                    return API.HandleCreateStopword(Request);

               case RouteAction::CreateGlobalStopword:
                    return API.HandleCreateGlobalStopword(Request);

               case RouteAction::DeleteStopword:
                    return API.HandleDeleteStopword(Request);

               case RouteAction::DeleteGlobalStopword:
                    return API.HandleDeleteGlobalStopword(Request);

               case RouteAction::ListOverrides:
                    return API.HandleListOverrides(Request);

               case RouteAction::UpsertOverride:
                    return API.HandleCreateOrUpdateOverride(Request);

               case RouteAction::GetOverride:
                    return API.HandleGetOverride(Request);

               case RouteAction::DeleteOverride:
                    return API.HandleDeleteOverride(Request);

               case RouteAction::ListAliases:
                    return API.HandleListAliases(Request);

               case RouteAction::UpsertAlias:
                    return API.HandleCreateOrUpdateAlias(Request);

               case RouteAction::GetAlias:
                    return API.HandleGetAlias(Request);

               case RouteAction::DeleteAlias:
                    return API.HandleDeleteAlias(Request);

               case RouteAction::MultiSearch:
                    return API.HandleMultiSearch(Request);

               case RouteAction::GlobalSearch:
                    return API.HandleGlobalSearch(Request);

               case RouteAction::ListKeys:
                    return API.HandleListKeys(Request);

               case RouteAction::CreateKey:
                    return API.HandleCreateKey(Request);

               case RouteAction::GetKey:
                    return API.HandleGetKey(Request);

               case RouteAction::DeleteKey:
                    return API.HandleDeleteKey(Request);

               case RouteAction::UpdateKey:
                    return API.HandleUpdateKey(Request);

               case RouteAction::ListPresets:
                    return API.HandleListPresets(Request);

               case RouteAction::UpsertPreset:
                    return API.HandleCreateOrUpdatePreset(Request);

               case RouteAction::GetPreset:
                    return API.HandleGetPreset(Request);

               case RouteAction::DeletePreset:
                    return API.HandleDeletePreset(Request);

               case RouteAction::AnalyticsClick:
                    return API.HandleAnalyticsClick(Request);

               case RouteAction::ListModules:
                    return API.HandleListModules(Request);

               case RouteAction::GetModuleSyntax:
                    return API.HandleModuleSyntax(Request);

               case RouteAction::ModuleLoad:
                    return API.HandleModuleLoad(Request);

               case RouteAction::ModuleUnload:
                    return API.HandleModuleUnload(Request);

               case RouteAction::ModuleAPI:
                    return API.HandleModuleAPI(Request);

               case RouteAction::Root:
                    /* Root endpoint - same as /status. */

                    return API.HandleStatus(Request);

               case RouteAction::NotFound:
               default:
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("http_server", "NO ROUTE MATCHED - Returning 404 for path: " + Request.Path + " method: " + Request.Method + " action: " + std::string(RouteActionName(ActionVal)) + ".");
                    }

                    break;
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "Exception in ProcessRequestWithAPI for path: " + Request.Path + " method: " + Request.Method + " error: " + std::string(E.what()) + ".");
          }

          HttpResponse ResponseVal(http_code::INTERNAL_SERVER_ERROR, StatusText(http_code::INTERNAL_SERVER_ERROR), "application/json");

          ResponseVal.Body = "{\"error\":\"Internal server error\",\"message\":\"" + API.EscapeJSONString(E.what()) + "\"}";

          return ResponseVal;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("http_server", "Unknown exception in ProcessRequestWithAPI for path: " + Request.Path + " method: " + Request.Method + ".");
          }

          HttpResponse ResponseVal(http_code::INTERNAL_SERVER_ERROR, StatusText(http_code::INTERNAL_SERVER_ERROR), "application/json");

          ResponseVal.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred\"}";

          return ResponseVal;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_server", "Route not found: " + Request.Method + " " + Request.Path + ".");
     }

     /* Always return JSON response for API clients. */

     HttpResponse ResponseVal = BuildRouteNotFoundResponse(Request.Path, Request.Method);

     return ResponseVal;
}
