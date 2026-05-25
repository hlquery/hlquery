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
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/config.h"

#ifdef HLQUERY_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "runtime/clock.h"
#include "runtime/exitmanager.h"

#ifndef HLQUERY_HAS_OPENSSL
namespace
{
HTTPResponse MakeSSLMissingResponse()
{
     HTTPResponse response;
     response.StatusCode = -1;
     response.ErrorMessage = "HTTPS support is unavailable in this build because OpenSSL support was not enabled.";
     return response;
}
}
#endif

/* Global stats forward declarations. */

extern std::atomic<int> collections_created;

extern std::atomic<int> documents_inserted;

extern std::atomic<int> collections_skipped;

extern std::atomic<int> documents_skipped;

extern std::atomic<bool> g_benchmark_should_stop;
static std::atomic<bool> g_benchmark_ssl_auth_mode{false};

void BenchmarkClient::SetGlobalSSLAuthMode(bool enabled)
{
     g_benchmark_ssl_auth_mode.store(enabled);
}

/* Connects a socket with retry. */

bool BenchmarkClient::ConnectSocket(int &sock, int max_retries, int connect_timeout_sec)
{
     for (int retry = 0; retry < max_retries; retry++)
     {
          struct addrinfo hints, *res, *p;

          memset(&hints, 0, sizeof(hints));

          hints.ai_family = AF_INET;
          hints.ai_socktype = SOCK_STREAM;

          std::string port_str = std::to_string(Port);

          if (getaddrinfo(Host.c_str(), port_str.c_str(), &hints, &res) != 0)
          {
               if (retry < max_retries - 1)
               {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (1 << retry)));
                    continue;
               }

               return false;
          }

          for (p = res; p != NULL; p = p->ai_next)
          {
               if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
               {
                    continue;
               }

               struct timeval timeout;

               timeout.tv_sec = connect_timeout_sec;
               timeout.tv_usec = 0;

               setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
               setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

               int flags = fcntl(sock, F_GETFL, 0);

               fcntl(sock, F_SETFL, flags | O_NONBLOCK);

               int opt = 1;

               setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
               setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

               int connect_result = connect(sock, p->ai_addr, p->ai_addrlen);

               if (connect_result == -1)
               {
                    if (errno == EINPROGRESS)
                    {
                         fd_set write_fds;

                         FD_ZERO(&write_fds);
                         FD_SET(sock, &write_fds);

                         struct timeval select_timeout;

                         select_timeout.tv_sec = connect_timeout_sec;
                         select_timeout.tv_usec = 0;

                         int select_result = select(sock + 1, NULL, &write_fds, NULL, &select_timeout);

                         if (select_result <= 0)
                         {
                              close(sock);
                              continue;
                         }

                         int so_error = 0;

                         socklen_t len = sizeof(so_error);

                         if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
                         {
                              close(sock);
                              continue;
                         }
                    }
                    else
                    {
                         close(sock);
                         continue;
                    }
               }

               fcntl(sock, F_SETFL, flags);

               break;
          }

          freeaddrinfo(res);

          if (p == NULL)
          {
               if (retry < max_retries - 1)
               {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (1 << retry)));
                    continue;
               }

               return false;
          }

          return true;
     }

     return false;
}

/* Closes the socket. */

void BenchmarkClient::CloseSocket()
{
     std::lock_guard<std::mutex> lock(SocketMutex);

     if (SocketFD >= 0)
     {
          close(SocketFD);

          SocketFD = -1;
          SocketConnected = false;
     }

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLObj)
     {
          SSL_free(SSLObj);
          SSLObj = nullptr;
     }
#endif
}

/* Gets a connection. */

bool BenchmarkClient::GetConnection(int &sock)
{
     std::lock_guard<std::mutex> lock(SocketMutex);

     if (SocketFD >= 0 && SocketConnected)
     {
          sock = SocketFD;
          return true;
     }

     int new_sock = -1;

     if (!ConnectSocket(new_sock, 3, 5))
     {
          return false;
     }

     SocketFD = new_sock;
     SocketConnected = true;

     RequestCount.store(0);

     sock = new_sock;

     return true;
}

/* BenchmarkClient constructor. */

BenchmarkClient::BenchmarkClient(const std::string &base_url, const std::string &token, bool reuse_collections)
    : AuthToken(token), SocketFD(-1), SocketConnected(false), ReuseCollections(reuse_collections)
{
     ParseURL(base_url);
     SSLAuthMode = g_benchmark_ssl_auth_mode.load();

#ifdef HLQUERY_HAS_OPENSSL
     if (UseSSL)
     {
          static std::once_flag init_flag;

          std::call_once(init_flag, []()
                         {
                              SSL_library_init();
                              SSL_load_error_strings();
                              OpenSSL_add_all_algorithms();
                         });

          SSLCtx = SSL_CTX_new(TLS_client_method());

          if (SSLCtx)
          {
               SSL_CTX_set_verify(SSLCtx, SSL_VERIFY_NONE, nullptr);
          }
     }
#endif
}

/* BenchmarkClient destructor. */

BenchmarkClient::~BenchmarkClient()
{
     CloseSocket();

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLCtx)
     {
          SSL_CTX_free(SSLCtx);
     }
#endif
}

/* Resets connection state. */

void BenchmarkClient::Reset()
{
     CloseSocket();

     RequestCount.store(0);
}

/* Resets the connection. */

void BenchmarkClient::ResetConnection()
{
     CloseSocket();
}

/* Makes an HTTP request. */

HTTPResponse BenchmarkClient::MakeRequest(const std::string &method, const std::string &path, const std::string &body, int max_retries, bool use_keep_alive, int timeout_ms)
{
     HTTPResponse response;

#ifndef HLQUERY_HAS_OPENSSL
     if (UseSSL)
     {
          return MakeSSLMissingResponse();
     }
#endif

     int sock = -1;

     if (!GetConnection(sock))
     {
          response.StatusCode = -1;
          response.ErrorMessage = "Could not create/connect socket.";

          return response;
     }

     /* Rotate the connection when we hit the reuse limit. */

     int current_count = RequestCount.fetch_add(1) + 1;

     if (current_count >= MAX_REQUESTS_PER_CONNECTION && use_keep_alive)
     {
          CloseSocket();

          if (!GetConnection(sock))
          {
               response.StatusCode = -1;
               response.ErrorMessage = "Could not create new connection after max requests.";

               return response;
          }
     }

     /* Retry request execution on connection failures. */

     for (int attempt = 0; attempt < max_retries; attempt++)
     {
          if (attempt > 0)
          {
               CloseSocket();

               if (!GetConnection(sock))
               {
                    if (attempt == max_retries - 1)
                    {
                         response.StatusCode = -1;
                         response.ErrorMessage = "Could not reconnect.";

                         return response;
                    }

                    continue;
               }
          }

#ifdef HLQUERY_HAS_OPENSSL
          if (UseSSL && !SSLObj)
          {
               if (!SSLCtx)
               {
                    response.StatusCode = -1;
                    response.ErrorMessage = "Failed to initialize TLS context.";

                    return response;
               }

               SSLObj = SSL_new(SSLCtx);

               if (!SSLObj)
               {
                    response.StatusCode = -1;
                    response.ErrorMessage = "Failed to initialize TLS session.";

                    return response;
               }

               SSL_set_fd(SSLObj, sock);

               if (SSL_connect(SSLObj) != 1)
               {
                    CloseSocket();

                    if (attempt == max_retries - 1)
                    {
                         response.StatusCode = -1;
                         response.ErrorMessage = "TLS handshake failed.";

                         return response;
                    }

                    continue;
               }
          }
#endif

          std::string request_path = path;

          /* Keep benchmark writes local even when cluster distributed ingest is enabled. */

          if ((method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH") &&
              request_path.find("distributed=") == std::string::npos)
          {
               request_path += (request_path.find('?') == std::string::npos) ? "?" : "&";
               request_path += "distributed=off";
          }

          std::stringstream request;

          request << method << " " << request_path << " HTTP/1.1\r\n";
          request << "Host: " << Host << ":" << Port << "\r\n";
          request << "User-Agent: hlquery-benchmark/1.0\r\n";
          request << "Accept: application/json\r\n";
          request << "Connection: " << (use_keep_alive ? "keep-alive" : "close") << "\r\n";

          /* Benchmarks should not be blocked on replication acknowledgements. */
          if (method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH")
          {
               request << "X-HLQ-Replication-Hop: 1\r\n";
          }

          if (!AuthToken.empty())
          {
               request << "Authorization: Bearer " << AuthToken << "\r\n";
               if (SSLAuthMode && UseSSL)
               {
                    request << "X-API-Key: " << AuthToken << "\r\n";
               }
          }

          if (!body.empty())
          {
               request << "Content-Type: application/json\r\n";
               request << "Content-Length: " << body.length() << "\r\n";
          }

          request << "\r\n"
                  << body;

          std::string request_str = request.str();

          size_t sent = 0;
          bool retry_after_write_failure = false;
          auto write_start = Now();

          while (sent < request_str.length())
          {
               if (ElapsedMs(write_start) > timeout_ms)
               {
                    CloseSocket();
                    response.StatusCode = -1;
                    response.ErrorMessage = "Timed out while writing HTTP request to server.";

                    return response;
               }

               int write_result = 0;

               if (UseSSL)
               {
// The SSL path is only compiled when OpenSSL is enabled.
#ifdef HLQUERY_HAS_OPENSSL
                    write_result = SSL_write(SSLObj,
                                             request_str.c_str() + sent,
                                             static_cast<int>(request_str.length() - sent));
#else
                    write_result = -1;
#endif
               }
               else
               {
                    write_result = static_cast<int>(send(sock,
                                                         request_str.c_str() + sent,
                                                         request_str.length() - sent,
                                                         0));
               }

               if (write_result > 0)
               {
                    sent += static_cast<size_t>(write_result);
                    continue;
               }

               if (write_result < 0)
               {
                    int write_errno = errno;

                    if (write_errno == EINTR)
                    {
                         continue;
                    }

#if EAGAIN == EWOULDBLOCK

                    if (write_errno == EAGAIN)
                    {
#else

                    if (write_errno == EAGAIN || write_errno == EWOULDBLOCK)
                    {

#endif
                         fd_set write_fds;
                         FD_ZERO(&write_fds);
                         FD_SET(sock, &write_fds);

                         struct timeval write_timeout;
                         write_timeout.tv_sec = 0;
                         write_timeout.tv_usec = 100000;

                         int select_result = select(sock + 1, NULL, &write_fds, NULL, &write_timeout);
                         if (select_result < 0 && errno != EINTR)
                         {
                              CloseSocket();
                              response.StatusCode = -1;
                              response.ErrorMessage = "Failed while waiting to write HTTP request to server.";

                              return response;
                         }

                         continue;
                    }
               }

               CloseSocket();

               if (attempt < max_retries - 1)
               {
                    retry_after_write_failure = true;
                    break;
               }

               response.StatusCode = -1;
               response.ErrorMessage = "Failed to write HTTP request to server.";

               return response;
          }

          if (retry_after_write_failure)
          {
               continue;
          }

          char buffer[8192];

          std::string response_str;

          bool response_complete = false;

          auto read_start = Now();

          int sock_flags = fcntl(sock, F_GETFL, 0);

          if (sock_flags >= 0)
          {
               fcntl(sock, F_SETFL, sock_flags | O_NONBLOCK);
          }

          while (!response_complete)
          {
               if (g_benchmark_should_stop.load())
               {
                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    response.StatusCode = -1;
                    response.ErrorMessage = "Interrupted by user (Ctrl+C).";

                    return response;
               }

               if (ElapsedMs(read_start) > timeout_ms)
               {
                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    if (attempt < max_retries - 1)
                    {
                         break;
                    }

                    response.StatusCode = -1;
                    response.ErrorMessage = "Timed out waiting for HTTP response from server.";

                    return response;
               }

               if (!UseSSL)
               {
                    fd_set read_fds;

                    FD_ZERO(&read_fds);
                    FD_SET(sock, &read_fds);

                    struct timeval select_timeout;

                    select_timeout.tv_sec = 0;
                    select_timeout.tv_usec = 100000;

                    int select_result = select(sock + 1, &read_fds, NULL, NULL, &select_timeout);

                    if (select_result < 0 && errno != EINTR)
                    {
                         if (sock_flags >= 0)
                         {
                              fcntl(sock, F_SETFL, sock_flags);
                         }

                         CloseSocket();

                         if (attempt < max_retries - 1)
                         {
                              break;
                         }

                         response.StatusCode = -1;

                         return response;
                    }

                    if (select_result == 0 || !FD_ISSET(sock, &read_fds))
                    {
                         continue;
                    }
               }

               int bytes = 0;

               if (UseSSL)
               {
#ifdef HLQUERY_HAS_OPENSSL
                    bytes = SSL_read(SSLObj, buffer, sizeof(buffer) - 1);
#else
                    bytes = -1;
#endif
               }
               else
               {
                    bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

                    if (bytes < 0)
                    {
                         int recv_errno = errno;

                         if (recv_errno == EAGAIN)
                         {
                              continue;
                         }

#if EAGAIN != EWOULDBLOCK

                         if (recv_errno == EWOULDBLOCK)
                         {
                              continue;
                         }

#endif
                    }
               }

               if (g_benchmark_should_stop.load())
               {
                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    response.StatusCode = -1;
                    response.ErrorMessage = "Interrupted by user (Ctrl+C).";

                    return response;
               }

               if (bytes <= 0)
               {
                    if (bytes == 0)
                    {
                         if (response_str.find("\r\n\r\n") != std::string::npos)
                         {
                              response_complete = true;
                              break;
                         }
                    }

                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    if (attempt < max_retries - 1)
                    {
                         break;
                    }

                    response.StatusCode = -1;
                    response.ErrorMessage = (bytes == 0)
                                                  ? "Connection closed before a complete HTTP response was received."
                                                  : "Failed to read HTTP response from server.";

                    return response;
               }

               response_str.append(buffer, static_cast<size_t>(bytes));

               size_t header_end = response_str.find("\r\n\r\n");

               if (header_end != std::string::npos)
               {
                    std::string headers_part = response_str.substr(0, header_end);
                    std::string headers_lower = headers_part;
                    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(),
                                   [](unsigned char C)
                                   {
                                        return static_cast<char>(std::tolower(C));
                                   });

                    size_t cl_pos = headers_lower.find("content-length:");

                    if (cl_pos != std::string::npos)
                    {
                         size_t colon_pos = headers_part.find(':', cl_pos);
                         size_t value_start = colon_pos == std::string::npos ? std::string::npos : headers_part.find_first_not_of(" \t", colon_pos + 1);
                         size_t cl_end = headers_part.find("\r\n", cl_pos);
                         if (cl_end == std::string::npos)
                         {
                              cl_end = headers_part.size();
                         }

                         try
                         {
                              if (value_start == std::string::npos || value_start > cl_end)
                              {
                                   throw std::invalid_argument("missing Content-Length value");
                              }

                              std::string length_value = headers_part.substr(value_start, cl_end - value_start);
                              size_t value_end = length_value.find_last_not_of(" \t");
                              if (value_end != std::string::npos)
                              {
                                   length_value.erase(value_end + 1);
                              }

                              size_t parsed = 0;
                              int len = std::stoi(length_value, &parsed);

                              if (parsed != length_value.size() || len < 0)
                              {
                                   throw std::invalid_argument("invalid Content-Length value");
                              }

                              if (response_str.length() >= header_end + 4 + len)
                              {
                                   response_complete = true;
                              }
                         }
                         catch (...)
                         {
                              response_complete = true;
                         }
                    }
                    else
                    {
                         if (response_str.find("}") != std::string::npos)
                         {
                              response_complete = true;
                         }
                    }
               }
          }

          if (sock_flags >= 0)
          {
               fcntl(sock, F_SETFL, sock_flags);
          }

          if (response_complete)
          {
               size_t first_http = response_str.find("HTTP/");

               if (first_http == std::string::npos)
               {
                    response.StatusCode = -1;
                    response.ErrorMessage = "Invalid HTTP response: no HTTP/ header found.";

                    return response;
               }

               size_t header_end = response_str.find("\r\n\r\n", first_http);

               if (header_end == std::string::npos)
               {
                    response.StatusCode = -1;
                    response.ErrorMessage = "Invalid HTTP response: no header end found.";

                    return response;
               }

               response.StatusCode = -1;

               try
               {
                    size_t space1 = response_str.find(' ', first_http);

                    if (space1 != std::string::npos && space1 < header_end)
                    {
                         size_t space2 = response_str.find(' ', space1 + 1);

                         if (space2 != std::string::npos && space2 < header_end)
                         {
                              std::string status_str = response_str.substr(space1 + 1, space2 - space1 - 1);

                              int parsed_code = std::stoi(status_str);

                              if (parsed_code >= 100 && parsed_code < 600)
                              {
                                   response.StatusCode = parsed_code;
                              }
                              else
                              {
                                   if (log_file_stream && log_file_stream->is_open())
                                   {
                                        *log_file_stream << "[ERROR] Invalid HTTP status code parsed: " << parsed_code << " (raw: '" << status_str << "').\n";
                                        log_file_stream->flush();
                                   }

                                   response.StatusCode = -1;
                              }
                         }
                    }
               }
               catch (const std::exception &e)
               {
                    if (log_file_stream && log_file_stream->is_open())
                    {
                         *log_file_stream << "[ERROR] Exception parsing HTTP status code: " << e.what() << ".\n";
                         log_file_stream->flush();
                    }

                    response.StatusCode = -1;
               }
               catch (...)
               {
                    if (log_file_stream && log_file_stream->is_open())
                    {
                         *log_file_stream << "[ERROR] Unknown exception parsing HTTP status code.\n";
                         log_file_stream->flush();
                    }

                    response.StatusCode = -1;
               }

               if (response.StatusCode == -1)
               {
                    response.ErrorMessage = "Failed to parse HTTP status code from response.";

                    if (log_file_stream && log_file_stream->is_open())
                    {
                         *log_file_stream << "[ERROR] Could not parse HTTP status code. First 200 chars of response: "
                                          << response_str.substr(0, 200) << ".\n";
                         log_file_stream->flush();
                    }

                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    return response;
               }

               if (response.StatusCode == 401 || response.StatusCode == 403)
               {
                    response.ErrorMessage = "Authentication required (HTTP " + std::to_string(response.StatusCode) + ").";

                    if (sock_flags >= 0)
                    {
                         fcntl(sock, F_SETFL, sock_flags);
                    }

                    CloseSocket();

                    return response;
               }

               size_t body_start = header_end + 4;

               size_t next_http_start = response_str.find("\r\n\r\nHTTP/", header_end);

               size_t body_end;

               if (next_http_start != std::string::npos)
               {
                    body_end = next_http_start;
               }
               else
               {
                    body_end = response_str.length();
               }

               response.Body = response_str.substr(body_start, body_end - body_start);

               while (!response.Body.empty() && (response.Body.back() == '\r' || response.Body.back() == '\n' || response.Body.back() == ' ' || response.Body.back() == '\t'))
               {
                    response.Body.pop_back();
               }

               if (log_file_stream && log_file_stream->is_open())
               {
                    if (response.StatusCode != 200 && response.StatusCode != 201 && response.StatusCode != 409)
                    {
                         *log_file_stream << "[DEBUG] Unexpected HTTP status " << response.StatusCode
                                          << ". Response preview (first 300 chars): "
                                          << response_str.substr(0, 300) << ".\n";
                         log_file_stream->flush();
                    }
               }

               if (!use_keep_alive)
               {
                    CloseSocket();
               }

               return response;
          }
     }

     return response;
}

/* Parses the URL. */

void BenchmarkClient::ParseURL(const std::string &url)
{
     if (url.find("://") == std::string::npos)
     {
          Host = url;
          Port = 9200;
          UseSSL = false;

          return;
     }

     size_t protocol_end = url.find("://");

     std::string protocol = url.substr(0, protocol_end);

     UseSSL = (protocol == "https");

     size_t start = protocol_end + 3;

     size_t colon_pos = url.find(":", start);
     size_t slash_pos = url.find("/", start);

     if (colon_pos != std::string::npos && (slash_pos == std::string::npos || colon_pos < slash_pos))
     {
          Host = url.substr(start, colon_pos - start);
          Port = std::stoi(url.substr(colon_pos + 1, slash_pos - colon_pos - 1));
     }
     else
     {
          Host = url.substr(start, slash_pos - start);
          Port = UseSSL ? 9443 : 9200;
     }
}

/* Tests connection. */

std::string BenchmarkClient::TestConnection()
{
     auto response = MakeRequest("GET", "/collections");

     if (response.StatusCode == -1)
     {
          return response.ErrorMessage.empty() ? "Could not connect to server." : response.ErrorMessage;
     }

     return "";
}

/* Deletes a collection. */

bool BenchmarkClient::DeleteCollection(const std::string &name)
{
     HTTPResponse response = MakeRequest("DELETE", "/collections/" + name);

     return (response.StatusCode == 200 || response.StatusCode == 404);
}

/* Creates a collection. */

static std::string AppendLocalOnlyQuery(const std::string &path, bool local_only)
{
     if (!local_only)
     {
          return path;
     }

     if (path.find('?') == std::string::npos)
     {
          return path + "?distributed=off";
     }

     return path + "&distributed=off";
}

static bool IsLocalWriteCommittedDespiteReplicationError(const HTTPResponse &response)
{
     if (response.StatusCode != 503)
     {
          return false;
     }

     const std::string lower_body = [&response]()
     {
          std::string value = response.Body;
          std::transform(value.begin(), value.end(), value.begin(), ::tolower);
          return value;
     }();

     return (lower_body.find("replication incomplete") != std::string::npos &&
             (lower_body.find("written locally") != std::string::npos ||
              lower_body.find("completed locally") != std::string::npos));
}

static std::string LowercaseCopy(std::string value)
{
     std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                    { return static_cast<char>(std::tolower(ch)); });
     return value;
}

static void AddBenchmarkDocumentIdField(nlohmann::json &fields)
{
     if (!fields.is_array())
     {
          fields = nlohmann::json::array();
     }

     for (const auto &field : fields)
     {
          if (field.is_object() && field.value("name", "") == "document_id")
          {
               return;
          }
     }

     fields.insert(fields.begin(), {{"name", "document_id"}, {"type", "string"}});
}

static void AddBenchmarkDocumentIdValue(nlohmann::json &doc)
{
     if (doc.is_object() && doc.contains("id") && !doc.contains("document_id"))
     {
          doc["document_id"] = doc["id"];
     }
}

bool BenchmarkClient::CreateCollection(const std::string &name)
{
     return CreateCollection(name, 10000);
}

bool BenchmarkClient::CreateCollection(const std::string &name, int timeout_ms)
{
     return CreateCollectionLocal(name, timeout_ms);
}

bool BenchmarkClient::CreateCollectionLocal(const std::string &name)
{
     return CreateCollectionLocal(name, 10000);
}

bool BenchmarkClient::CreateCollectionLocal(const std::string &name, int timeout_ms)
{
     nlohmann::json schema;

     schema["name"] = name;
     schema["fields"] = nlohmann::json::array();

     AddBenchmarkDocumentIdField(schema["fields"]);

     nlohmann::json title_field;

     title_field["name"] = "title";
     title_field["type"] = "string";
     title_field["facet"] = false;

     schema["fields"].push_back(title_field);

     nlohmann::json content_field;

     content_field["name"] = "content";
     content_field["type"] = "string";
     content_field["facet"] = false;

     schema["fields"].push_back(content_field);

     std::string json_str = schema.dump();

     HTTPResponse response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3, true, timeout_ms);

     if (response.StatusCode == 409)
     {
          if (ReuseCollections)
          {
               collections_skipped.fetch_add(1);

               return true;
          }

          DeleteCollection(name);
          response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3, true, timeout_ms);

          if (response.StatusCode == 409)
          {
               DeleteCollection(name);
               response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3, true, timeout_ms);
          }

          if (response.StatusCode == 409)
          {
               std::cerr << "  [ERROR] Failed to replace existing collection '" << name << "'.\n";
               std::cerr << "  [ERROR] Consider using --cleanup flag or --reuse-collections flag.\n";

               return false;
          }
     }

     bool success = false;

     std::string failure_reason;

     if (response.StatusCode == 201 || response.StatusCode == 200)
     {
          bool has_explicit_error = false;

          if (!response.Body.empty())
          {
               std::string lower_body = response.Body;

               std::transform(lower_body.begin(), lower_body.end(), lower_body.begin(), ::tolower);

               if (lower_body.find("\"error\"") != std::string::npos ||
                   (lower_body.find("error") != std::string::npos &&
                    (lower_body.find("already exists") != std::string::npos ||
                     lower_body.find("failed") != std::string::npos ||
                     lower_body.find("invalid") != std::string::npos)))
               {
                    has_explicit_error = true;
               }
          }

          if (has_explicit_error)
          {
               failure_reason = "HTTP " + std::to_string(response.StatusCode) + " returned but response contains error.";
          }
          else
          {
               success = true;

               bool found_in_body = false;

               if (response.Body.find("Collection created successfully") != std::string::npos || response.Body.find("\"message\"") != std::string::npos)
               {
                    found_in_body = true;
               }
               else
               {
                    std::string name_pattern = "\"name\":\"" + name + "\"";

                    if (response.Body.find(name_pattern) != std::string::npos)
                    {
                         found_in_body = true;
                    }
                    else
                    {
                         size_t name_pos = response.Body.find(name);

                         if (name_pos != std::string::npos)
                         {
                              size_t before = (name_pos > 0) ? name_pos - 1 : 0;
                              size_t after = name_pos + name.length();

                              if (before < response.Body.length() && after < response.Body.length())
                              {
                                   char before_char = response.Body[before];
                                   char after_char = response.Body[after];

                                   if ((before_char == '"' || before_char == ':') && (after_char == '"' || after_char == ',' || after_char == '}'))
                                   {
                                        found_in_body = true;
                                   }
                              }
                         }
                    }
               }

               if (!found_in_body)
               {
                    failure_reason = "HTTP " + std::to_string(response.StatusCode) + " returned but collection name not found in response body (server accepted request - treating as success).";
               }
          }
     }
     else if (response.StatusCode >= 400)
     {
          failure_reason = "HTTP " + std::to_string(response.StatusCode) + " error.";

          if (!response.Body.empty())
          {
               try
               {
                    nlohmann::json error_json = nlohmann::json::parse(response.Body);

                    if (error_json.contains("error"))
                    {
                         failure_reason += ": " + error_json["error"].get<std::string>();
                    }

                    if (error_json.contains("message"))
                    {
                         failure_reason += " - " + error_json["message"].get<std::string>();
                    }
               }
               catch (...)
               {
                    std::string body_preview = response.Body.length() > 150 ? response.Body.substr(0, 150) + "..." : response.Body;

                    failure_reason += ": " + body_preview;
               }
          }
     }
     else
     {
          failure_reason = "Unexpected HTTP status " + std::to_string(response.StatusCode);

          if (!response.ErrorMessage.empty())
          {
               failure_reason += " - " + response.ErrorMessage;
          }
     }

     if (!success)
     {
          static std::atomic<int> error_count(0);

          if (error_count.fetch_add(1) < 10)
          {
               std::string error_msg = "  [ERROR] Failed to create collection '" + name + "': " + failure_reason + "\n";

               std::lock_guard<std::mutex> lock(console_mutex);

               std::cerr << error_msg;
          }
     }

     return success;
}

/* Creates a collection with custom schema. */

bool BenchmarkClient::CreateCollectionWithSchema(const std::string &name, const nlohmann::json &fields, const std::string &default_sorting_field, const nlohmann::json &metadata)
{
     return CreateCollectionWithSchemaLocal(name, fields, default_sorting_field, metadata);
}

bool BenchmarkClient::CreateCollectionWithSchemaLocal(const std::string &name, const nlohmann::json &fields, const std::string &default_sorting_field, const nlohmann::json &metadata)
{
     nlohmann::json schema;

     schema["name"] = name;
     schema["fields"] = fields;
     AddBenchmarkDocumentIdField(schema["fields"]);
     if (metadata.is_object() && !metadata.empty())
     {
          schema["metadata"] = metadata;
     }

     if (!default_sorting_field.empty())
     {
          schema["default_sorting_field"] = default_sorting_field;
     }

     std::string json_str = schema.dump();

     HTTPResponse response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3);

     if (response.StatusCode == 409)
     {
          if (ReuseCollections)
          {
               return true;
          }

          DeleteCollection(name);
          response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3);

          if (response.StatusCode == 409)
          {
               DeleteCollection(name);
               response = MakeRequest("POST", AppendLocalOnlyQuery("/collections", true), json_str, 3);
          }
     }

     bool success = false;

     std::string failure_reason;

     if (response.StatusCode == 201 || response.StatusCode == 200)
     {
          bool has_explicit_error = false;

          if (!response.Body.empty())
          {
               std::string lower_body = response.Body;

               std::transform(lower_body.begin(), lower_body.end(), lower_body.begin(), ::tolower);

               if (lower_body.find("\"error\"") != std::string::npos ||
                   (lower_body.find("error") != std::string::npos &&
                    (lower_body.find("already exists") != std::string::npos ||
                     lower_body.find("failed") != std::string::npos ||
                     lower_body.find("invalid") != std::string::npos)))
               {
                    has_explicit_error = true;
               }
          }

          if (has_explicit_error)
          {
               failure_reason = "HTTP " + std::to_string(response.StatusCode) + " returned but response contains error.";
          }
          else
          {
               success = true;

               bool found_in_body = false;

               if (response.Body.find("Collection created successfully") != std::string::npos || response.Body.find("\"message\"") != std::string::npos)
               {
                    found_in_body = true;
               }
               else
               {
                    std::string name_pattern = "\"name\":\"" + name + "\"";

                    if (response.Body.find(name_pattern) != std::string::npos)
                    {
                         found_in_body = true;
                    }
                    else
                    {
                         size_t name_pos = response.Body.find(name);

                         if (name_pos != std::string::npos)
                         {
                              size_t before = (name_pos > 0) ? name_pos - 1 : 0;
                              size_t after = name_pos + name.length();

                              if (before < response.Body.length() && after < response.Body.length())
                              {
                                   char before_char = response.Body[before];
                                   char after_char = response.Body[after];

                                   if ((before_char == '"' || before_char == ':') && (after_char == '"' || after_char == ',' || after_char == '}'))
                                   {
                                        found_in_body = true;
                                   }
                              }
                         }
                    }
               }

               if (!found_in_body)
               {
                    failure_reason = "HTTP " + std::to_string(response.StatusCode) + " returned but collection name not found in response body (server accepted request - treating as success).";
               }
          }
     }
     else if (response.StatusCode >= 400)
     {
          failure_reason = "HTTP " + std::to_string(response.StatusCode) + " error.";

          if (!response.Body.empty())
          {
               try
               {
                    nlohmann::json error_json = nlohmann::json::parse(response.Body);

                    if (error_json.contains("error"))
                    {
                         failure_reason += ": " + error_json["error"].get<std::string>();
                    }

                    if (error_json.contains("message"))
                    {
                         failure_reason += " - " + error_json["message"].get<std::string>();
                    }
               }
               catch (...)
               {
                    std::string body_preview = response.Body.length() > 150 ? response.Body.substr(0, 150) + "..." : response.Body;

                    failure_reason += ": " + body_preview;
               }
          }
     }
     else
     {
          failure_reason = "Unexpected HTTP status " + std::to_string(response.StatusCode);

          if (!response.ErrorMessage.empty())
          {
               failure_reason += " - " + response.ErrorMessage;
          }
     }

     if (!success)
     {
          static std::atomic<int> error_count(0);

          if (error_count.fetch_add(1) < 10)
          {
               std::string error_msg = "  [ERROR] Failed to create collection '" + name + "': " + failure_reason + "\n";

               std::lock_guard<std::mutex> lock(console_mutex);

               std::cerr << error_msg;
          }
     }

     return success;
}

/* Inserts a document. */

bool BenchmarkClient::InsertDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content)
{
     nlohmann::json doc;

     doc["id"] = doc_id;
     doc["title"] = title;
     doc["content"] = content;
     AddBenchmarkDocumentIdValue(doc);

     std::string json_str = doc.dump();

     HTTPResponse response = MakeRequest("POST", "/collections/" + collection + "/documents", json_str);

     if (response.StatusCode == 401 || response.StatusCode == 403)
     {
          std::cerr << "\nERROR: Authentication required!.\n";
          std::cerr << "   Server returned HTTP " << response.StatusCode;

          if (response.StatusCode == 401)
          {
               std::cerr << " (Unauthorized)";
          }
          else
          {
               std::cerr << " (Forbidden)";
          }

          std::cerr << "\n";

          if (AuthToken.empty())
          {
               std::cerr << "\n   No authentication token provided.\n";
               std::cerr << "   Please provide a token using: --auth <token>.\n";
          }
          else
          {
               std::cerr << "\n   The provided authentication token is invalid or expired.\n";
               std::cerr << "   Please check your token and try again.\n";
          }

          std::cerr << "\n   Benchmark aborted.\n";

          ExitManager::Exit(1);
     }

     if (response.StatusCode != 201 && response.StatusCode != 200)
     {
          std::string error_msg = "  [ERROR] Failed to insert document '" + doc_id + "' into collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               error_msg += " - " + response.Body.substr(0, 200);
          }

          error_msg += "\n";

          LogError(error_msg);
     }

     return response.StatusCode == 201 || response.StatusCode == 200;
}

/* Upserts a document with fields. */

bool BenchmarkClient::UpsertDocumentWithFields(const std::string &collection, const nlohmann::json &doc)
{
     nlohmann::json payload = doc;
     AddBenchmarkDocumentIdValue(payload);

     std::string json_str = payload.dump();

     HTTPResponse response = MakeRequest("POST", "/collections/" + collection + "/documents", json_str, 3, false);

     if (response.StatusCode == -1)
     {
          return false;
     }

     if (IsLocalWriteCommittedDespiteReplicationError(response))
     {
          return true;
     }

     if (response.StatusCode == 409)
     {
          std::string doc_id = payload.contains("id") ? payload["id"].get<std::string>() : "";

          if (!doc_id.empty())
          {
               response = MakeRequest("PUT", "/collections/" + collection + "/documents/" + doc_id, json_str, 3, false);
          }
     }

     if (response.StatusCode != 201 && response.StatusCode != 200)
     {
          std::string doc_id = payload.contains("id") ? payload["id"].get<std::string>() : "";
          std::string error_msg = "  [ERROR] Failed to upsert document '" + doc_id + "' in collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               error_msg += " - " + response.Body.substr(0, 200);
          }

          error_msg += "\n";
          LogError(error_msg);
     }

     return response.StatusCode == 201 || response.StatusCode == 200;
}

bool BenchmarkClient::UpsertDocumentWithFieldsLocal(const std::string &collection, const nlohmann::json &doc)
{
     nlohmann::json payload = doc;
     AddBenchmarkDocumentIdValue(payload);

     std::string json_str = payload.dump();

     HTTPResponse response = MakeRequest("POST", AppendLocalOnlyQuery("/collections/" + collection + "/documents", true), json_str, 3, false);

     if (response.StatusCode == -1)
     {
          return false;
     }

     if (IsLocalWriteCommittedDespiteReplicationError(response))
     {
          return true;
     }

     if (response.StatusCode == 409)
     {
          std::string doc_id = payload.contains("id") ? payload["id"].get<std::string>() : "";

          if (!doc_id.empty())
          {
               response = MakeRequest("PUT", AppendLocalOnlyQuery("/collections/" + collection + "/documents/" + doc_id, true), json_str, 3, false);
          }
     }

     if (response.StatusCode != 201 && response.StatusCode != 200)
     {
          std::string doc_id = payload.contains("id") ? payload["id"].get<std::string>() : "";
          std::string error_msg = "  [ERROR] Failed to upsert document '" + doc_id + "' in collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               error_msg += " - " + response.Body.substr(0, 200);
          }

          error_msg += "\n";
          LogError(error_msg);
     }

     return response.StatusCode == 201 || response.StatusCode == 200;
}

/* Updates a document. */

bool BenchmarkClient::UpdateDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content)
{
     nlohmann::json doc;

     doc["id"] = doc_id;
     doc["title"] = title;
     doc["content"] = content;
     AddBenchmarkDocumentIdValue(doc);

     std::string json_str = doc.dump();

     std::string encoded_collection = UrlEncode(collection);
     std::string encoded_id = UrlEncode(doc_id);

     HTTPResponse response = MakeRequest("PUT", "/collections/" + encoded_collection + "/documents/" + encoded_id, json_str, 3, false);

     if (response.StatusCode != 200 && response.StatusCode != 201)
     {
          std::string error_msg = "  [ERROR] Failed to update document '" + doc_id + "' in collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               error_msg += " - " + response.Body.substr(0, 200);
          }

          error_msg += "\n";

          LogError(error_msg);
     }

     return response.StatusCode == 200 || response.StatusCode == 201;
}

/* Upserts a document. */

bool BenchmarkClient::UpsertDocument(const std::string &collection, const std::string &doc_id, const std::string &title, const std::string &content)
{
     nlohmann::json doc;

     doc["id"] = doc_id;
     doc["title"] = title;
     doc["content"] = content;
     AddBenchmarkDocumentIdValue(doc);

     std::string json_str = doc.dump();

     HTTPResponse response = MakeRequest("POST", "/collections/" + collection + "/documents", json_str, 3, false);

     if (collection == "history" && log_file_stream && log_file_stream->is_open())
     {
          *log_file_stream << "[DEBUG] UpsertDocument POST response for history doc_id='" << doc_id << "': status=" << response.StatusCode;

          if (!response.Body.empty())
          {
               *log_file_stream << " body_preview='" << response.Body.substr(0, 100) << "'";
          }

          if (!response.ErrorMessage.empty())
          {
               *log_file_stream << " error='" << response.ErrorMessage << "'";
          }

          *log_file_stream << "\n";
          log_file_stream->flush();
     }

     if (response.StatusCode == -1)
     {
          if (collection == "history" && log_file_stream && log_file_stream->is_open())
          {
               *log_file_stream << "[DEBUG] ✗ Failed to parse HTTP response for history doc_id='" << doc_id << "': " << response.ErrorMessage << "\n";
               log_file_stream->flush();
          }

          std::string error_msg = "  [ERROR] Failed to parse HTTP response for document '" + doc_id + "' in collection '" + collection + "': " + response.ErrorMessage + "\n";

          LogError(error_msg);

          return false;
     }

     if (response.StatusCode == 201 || response.StatusCode == 200)
     {
          bool body_looks_valid = true;

          if (!response.Body.empty())
          {
               bool has_success_indicator = (response.Body.find("successfully") != std::string::npos || response.Body.find("success") != std::string::npos || response.Body.find("\"id\"") != std::string::npos);

               if (!has_success_indicator)
               {
                    if (response.Body.find("\"error\"") != std::string::npos || response.Body.find("\"error\":") != std::string::npos || (response.Body.find("error") != std::string::npos && (response.Body.find("\"message\"") != std::string::npos || response.Body.find("failed") != std::string::npos)))
                    {
                         body_looks_valid = false;

                         if (log_file_stream && log_file_stream->is_open())
                         {
                              *log_file_stream << "[DEBUG] POST returned " << response.StatusCode << " but body contains error for " << collection << " doc_id='" << doc_id << "': " << response.Body.substr(0, 200) << "\n";
                              log_file_stream->flush();
                         }
                    }
               }
          }

          if (body_looks_valid)
          {
               if (collection == "history" && log_file_stream && log_file_stream->is_open())
               {
                    *log_file_stream << "[DEBUG] ✓ POST succeeded for history doc_id='" << doc_id << "'\n";
                    log_file_stream->flush();
               }

               return true;
          }
          else
          {
               std::string error_msg = "  [ERROR] Document insertion returned HTTP " + std::to_string(response.StatusCode) + " but response body indicates error for document '" + doc_id + "' in collection '" + collection + "': " + response.Body.substr(0, 200) + "\n";

               LogError(error_msg);

               return false;
          }
     }

     if (response.StatusCode == 409)
     {
          if (collection == "history" && log_file_stream && log_file_stream->is_open())
          {
               *log_file_stream << "[DEBUG] Document exists (409), trying PUT for history doc_id='" << doc_id << "'\n";
               log_file_stream->flush();
          }

          bool updated = UpdateDocument(collection, doc_id, title, content);

          if (!updated)
          {
               std::this_thread::sleep_for(std::chrono::milliseconds(100));

               response = MakeRequest("POST", "/collections/" + collection + "/documents", json_str, 3, false);

               if (collection == "history" && log_file_stream && log_file_stream->is_open())
               {
                    *log_file_stream << "[DEBUG] Retry POST for history doc_id='" << doc_id << "': status=" << response.StatusCode << "\n";
                    log_file_stream->flush();
               }

               if (response.StatusCode == 201 || response.StatusCode == 200)
               {
                    return true;
               }

               std::string error_msg = "  [ERROR] Failed to upsert document '" + doc_id + "' in collection '" + collection + "': POST returned 409, PUT failed, retry POST returned " + std::to_string(response.StatusCode);

               if (!response.Body.empty())
               {
                    error_msg += " - " + response.Body.substr(0, 200);
               }

               error_msg += "\n";

               LogError(error_msg);

               return false;
          }

          if (collection == "history" && log_file_stream && log_file_stream->is_open())
          {
               *log_file_stream << "[DEBUG] ✓ PUT succeeded for history doc_id='" << doc_id << "'\n";
               log_file_stream->flush();
          }

          return true;
     }

     std::string error_msg = "  [ERROR] Failed to insert document '" + doc_id + "' into collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

     if (!response.Body.empty())
     {
          error_msg += " - " + response.Body.substr(0, 200);
     }

     error_msg += "\n";

     LogError(error_msg);

     if (collection == "history" && log_file_stream && log_file_stream->is_open())
     {
          *log_file_stream << "[DEBUG] ✗ Failed to insert history doc_id='" << doc_id << "': HTTP " << response.StatusCode << "\n";
          log_file_stream->flush();
     }

     return false;
}

/* Adds a synonym to a collection. */

bool BenchmarkClient::AddSynonym(const std::string &collection, const std::string &synonym_id, const std::string &root_term, const std::vector<std::string> &synonyms)
{
     nlohmann::json synonym_data;

     synonym_data["root"] = root_term;
     synonym_data["synonyms"] = synonyms;

     std::string json_str = synonym_data.dump();

     std::string encoded_collection = UrlEncode(collection);
     std::string encoded_id = UrlEncode(synonym_id);

     HTTPResponse response = MakeRequest("POST", "/collections/" + encoded_collection + "/synonyms/" + encoded_id, json_str, 1, false, 5000);

     return response.StatusCode == 200 || response.StatusCode == 201;
}

/* Creates or updates an alias. */

bool BenchmarkClient::CreateAlias(const std::string &alias_name, const std::string &collection)
{
     nlohmann::json alias_data;

     alias_data["collection_name"] = collection;
     alias_data["collection"] = collection;

     std::string json_str = alias_data.dump();

     std::string encoded_alias = UrlEncode(alias_name);

     HTTPResponse response = MakeRequest("PUT", "/aliases/" + encoded_alias, json_str, 1, false, 5000);

     return response.StatusCode == 200 || response.StatusCode == 201;
}

/* Adds a stopword to a collection. */

bool BenchmarkClient::AddStopword(const std::string &collection, const std::string &word)
{
     nlohmann::json stopword_data;

     stopword_data["word"] = word;

     std::string json_str = stopword_data.dump();

     std::string encoded_collection = UrlEncode(collection);

     HTTPResponse response = MakeRequest("POST", "/collections/" + encoded_collection + "/stopwords", json_str, 1, false, 5000);

     return response.StatusCode == 200 || response.StatusCode == 201;
}

/* Inserts documents in bulk. */

int BenchmarkClient::InsertDocumentsBulkRequest(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs, HTTPResponse &response)
{
     nlohmann::json payload;

     payload["documents"] = nlohmann::json::array();

     for (const auto &doc_tuple : docs)
     {
          nlohmann::json doc;

          doc["id"] = std::get<0>(doc_tuple);
          doc["title"] = std::get<1>(doc_tuple);
          doc["content"] = std::get<2>(doc_tuple);
          AddBenchmarkDocumentIdValue(doc);

          payload["documents"].push_back(doc);
     }

     std::string json_str = payload.dump();

     std::string encoded_collection = UrlEncode(collection);

     const int import_timeout_ms = std::max(5000, static_cast<int>(docs.size()) * 100);

     response = MakeRequest("POST",
                            "/collections/" + encoded_collection + "/documents/import?assume_new=true&batch_size=" + std::to_string(docs.size()),
                            json_str,
                            3,
                            false,
                            import_timeout_ms);

     if (response.StatusCode == 401 || response.StatusCode == 403)
     {
          std::cerr << "\nERROR: Authentication required!.\n";
          std::cerr << "   Server returned HTTP " << response.StatusCode;

          if (response.StatusCode == 401)
          {
               std::cerr << " (Unauthorized)";
          }
          else
          {
               std::cerr << " (Forbidden)";
          }

          std::cerr << "\n";

          if (AuthToken.empty())
          {
               std::cerr << "\n   No authentication token provided.\n";
               std::cerr << "   Please provide a token using: --auth <token>.\n";
          }
          else
          {
               std::cerr << "\n   The provided authentication token is invalid or expired.\n";
               std::cerr << "   Please check your token and try again.\n";
          }

          std::cerr << "\n   Benchmark aborted.\n";

          ExitManager::Exit(1);
     }

     if (response.StatusCode == 200 || response.StatusCode == 201)
     {
          try
          {
               nlohmann::json result = nlohmann::json::parse(response.Body);

               if (result.contains("imported"))
               {
                    return result["imported"].get<int>();
               }
          }
          catch (...)
          {
          }

          return docs.size();
     }

     if (IsLocalWriteCommittedDespiteReplicationError(response))
     {
          return static_cast<int>(docs.size());
     }

     return 0;
}

bool BenchmarkClient::IsRetryableBulkInsertResponse(const HTTPResponse &response) const
{
     if (response.StatusCode == -1 || response.StatusCode == 408 || response.StatusCode == 425 ||
         response.StatusCode == 429 || response.StatusCode == 500 || response.StatusCode == 502 ||
         response.StatusCode == 503 || response.StatusCode == 504)
     {
          return true;
     }

     const std::string body = LowercaseCopy(response.Body);

     if (body.find("temporarily unavailable") != std::string::npos ||
         body.find("try again") != std::string::npos ||
         body.find("timed out") != std::string::npos ||
         body.find("replication outbox") != std::string::npos ||
         body.find("failed to persist replication outbox record") != std::string::npos ||
         body.find("sync in progress") != std::string::npos)
     {
          return true;
     }

     return false;
}

void BenchmarkClient::SleepBeforeBulkRetry(int attempt, int split_depth) const
{
     const int capped_attempt = std::max(0, std::min(attempt, 6));
     const int capped_split_depth = std::max(0, std::min(split_depth, 8));
     const int sleep_ms = 150 * (1 << capped_attempt) + (capped_split_depth * 75);

     std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
}

int BenchmarkClient::InsertDocumentsBulkInternal(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs, int split_depth)
{
     if (docs.empty())
     {
          return 0;
     }

     HTTPResponse response;

     for (int attempt = 0; attempt < 3; ++attempt)
     {
          const int inserted = InsertDocumentsBulkRequest(collection, docs, response);

          if (inserted > 0 || (response.StatusCode == 200 || response.StatusCode == 201) || IsLocalWriteCommittedDespiteReplicationError(response))
          {
               return inserted;
          }

          if (!IsRetryableBulkInsertResponse(response))
          {
               break;
          }

          ResetConnection();
          SleepBeforeBulkRetry(attempt, split_depth);
     }

     if (IsRetryableBulkInsertResponse(response) && docs.size() > 1)
     {
          const size_t midpoint = docs.size() / 2;
          std::vector<std::tuple<std::string, std::string, std::string>> left(docs.begin(), docs.begin() + midpoint);
          std::vector<std::tuple<std::string, std::string, std::string>> right(docs.begin() + midpoint, docs.end());

          if (verbose_mode && split_depth < 4)
          {
               std::lock_guard<std::mutex> lock(console_mutex);
               std::cerr << "  [WARN] Bulk insert for '" << collection << "' is backing off and splitting batch "
                         << docs.size() << " -> " << left.size() << "+" << right.size()
                         << " after transient HTTP " << response.StatusCode << ".\n";
          }

          const int left_inserted = InsertDocumentsBulkInternal(collection, left, split_depth + 1);
          const int right_inserted = InsertDocumentsBulkInternal(collection, right, split_depth + 1);

          return left_inserted + right_inserted;
     }

     if (response.StatusCode != -1)
     {
          std::string error_msg = "  [ERROR] InsertDocumentsBulk failed for collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               std::string body_preview = response.Body.length() > 200 ? response.Body.substr(0, 200) + "..." : response.Body;

               error_msg += " - " + body_preview;
          }

          if (!response.ErrorMessage.empty())
          {
               error_msg += " - " + response.ErrorMessage;
          }

          error_msg += "\n";

          static std::atomic<int> error_count(0);

          if (error_count.fetch_add(1) < 10)
          {
               std::lock_guard<std::mutex> lock(console_mutex);
               std::cerr << error_msg;
          }
     }

     return 0;
}

int BenchmarkClient::InsertDocumentsBulk(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs)
{
     return InsertDocumentsBulkInternal(collection, docs, 0);
}

int BenchmarkClient::InsertDocumentsBulkLocal(const std::string &collection, const std::vector<std::tuple<std::string, std::string, std::string>> &docs)
{
     nlohmann::json payload;

     payload["documents"] = nlohmann::json::array();

     for (const auto &doc_tuple : docs)
     {
          nlohmann::json doc;

          doc["id"] = std::get<0>(doc_tuple);
          doc["title"] = std::get<1>(doc_tuple);
          doc["content"] = std::get<2>(doc_tuple);
          AddBenchmarkDocumentIdValue(doc);

          payload["documents"].push_back(doc);
     }

     std::string json_str = payload.dump();

     std::string encoded_collection = UrlEncode(collection);

     const int import_timeout_ms = std::max(5000, static_cast<int>(docs.size()) * 100);

     HTTPResponse response = MakeRequest("POST",
                                         AppendLocalOnlyQuery("/collections/" + encoded_collection + "/documents/import?assume_new=true&batch_size=" + std::to_string(docs.size()), true),
                                         json_str,
                                         3,
                                         false,
                                         import_timeout_ms);

     if (response.StatusCode == 401 || response.StatusCode == 403)
     {
          std::cerr << "\nERROR: Authentication required!.\n";
          std::cerr << "   Server returned HTTP " << response.StatusCode;

          if (response.StatusCode == 401)
          {
               std::cerr << " (Unauthorized)";
          }
          else
          {
               std::cerr << " (Forbidden)";
          }

          std::cerr << "\n";

          if (AuthToken.empty())
          {
               std::cerr << "\n   No authentication token provided.\n";
               std::cerr << "   Please provide a token using: --auth <token>.\n";
          }
          else
          {
               std::cerr << "\n   The provided authentication token is invalid or expired.\n";
               std::cerr << "   Please check your token and try again.\n";
          }

          std::cerr << "\n   Benchmark aborted.\n";

          ExitManager::Exit(1);
     }

     if (response.StatusCode == 200 || response.StatusCode == 201)
     {
          try
          {
               nlohmann::json result = nlohmann::json::parse(response.Body);

               if (result.contains("imported"))
               {
                    return result["imported"].get<int>();
               }
          }
          catch (...)
          {
               /* Ignore parsing failure. */
          }

          return docs.size();
     }

     if (IsLocalWriteCommittedDespiteReplicationError(response))
     {
          return static_cast<int>(docs.size());
     }

     if (response.StatusCode != -1)
     {
          std::string error_msg = "  [ERROR] InsertDocumentsBulk failed for collection '" + collection + "': HTTP " + std::to_string(response.StatusCode);

          if (!response.Body.empty())
          {
               std::string body_preview = response.Body.length() > 200 ? response.Body.substr(0, 200) + "..." : response.Body;

               error_msg += " - " + body_preview;
          }

          if (!response.ErrorMessage.empty())
          {
               error_msg += " - " + response.ErrorMessage;
          }

          error_msg += "\n";

          static std::atomic<int> error_count(0);

          if (error_count.fetch_add(1) < 10)
          {
               std::lock_guard<std::mutex> lock(console_mutex);
               std::cerr << error_msg;
          }
     }

     return 0;
}

/* Performs a search in a collection. */

HTTPResponse BenchmarkClient::Search(const std::string &collection, const std::string &query, const std::map<std::string, std::string> &params)
{
     std::string path = "/collections/" + collection + "/documents/search";
     std::string query_string = "q=" + UrlEncode(query) + "&query_by=title,content,document_id";

     for (const auto &param : params)
     {
          query_string += "&" + param.first + "=" + UrlEncode(param.second);
     }

     return MakeRequest("GET", path + "?" + query_string);
}

/* Performs a search using POST. */

HTTPResponse BenchmarkClient::SearchPost(const std::string &collection, const nlohmann::json &Search_params)
{
     std::string path = "/collections/" + collection + "/documents/search";
     std::string json_str = Search_params.dump();

     return MakeRequest("POST", path, json_str);
}

/* Performs a multi-search across collections. */

HTTPResponse BenchmarkClient::MultiSearch(const nlohmann::json &multi_Search_params)
{
     std::string json_str = multi_Search_params.dump();

     return MakeRequest("POST", "/multi_search", json_str);
}

/* Gets documents from a collection. */

HTTPResponse BenchmarkClient::GetCollectionDocuments(const std::string &collection, int offset, int limit)
{
     std::string path = "/collections/" + collection + "/documents";
     std::string query = "offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);

     return MakeRequest("GET", path + "?" + query);
}

/* Gets a specific document from a collection. */

HTTPResponse BenchmarkClient::GetDocument(const std::string &collection, const std::string &doc_id)
{
     std::string path = "/collections/" + collection + "/documents/" + doc_id;

     return MakeRequest("GET", path);
}

/* Lists all collections. */

std::vector<std::string> BenchmarkClient::ListCollections()
{
     HTTPResponse response = MakeRequest("GET", "/collections");

     std::vector<std::string> collections;

     if (response.StatusCode == 200)
     {
          try
          {
               nlohmann::json result = nlohmann::json::parse(response.Body);

               if (result.contains("collections") && result["collections"].is_array())
               {
                    for (const auto &col : result["collections"])
                    {
                         if (col.contains("name"))
                         {
                              collections.push_back(col["name"].get<std::string>());
                         }
                    }
               }
          }
          catch (...)
          {
               /* Ignore parsing issue. */
          }
     }

     return collections;
}

/* Gets statistics for a collection. */

HTTPResponse BenchmarkClient::GetCollectionStats(const std::string &collection_name)
{
     return MakeRequest("GET", "/collections/" + UrlEncode(collection_name) + "/stats");
}

/* Gets information for a specific collection. */

HTTPResponse BenchmarkClient::GetCollection(const std::string &collection_name)
{
     return MakeRequest("GET", "/collections/" + UrlEncode(collection_name));
}

/* Gets server statistics. */

HTTPResponse BenchmarkClient::GetStats()
{
     return MakeRequest("GET", "/stats");
}

/* Gets total document count from the server. */

HTTPResponse BenchmarkClient::GetDocTotal(const std::string &prefix)
{
     if (!prefix.empty())
     {
          return MakeRequest("GET", "/doctotal?prefix=" + UrlEncode(prefix));
     }

     return MakeRequest("GET", "/doctotal");
}

/* Gets server health status. */

HTTPResponse BenchmarkClient::GetHealth()
{
     return MakeRequest("GET", "/health");
}

/* Gets server metrics. */

HTTPResponse BenchmarkClient::GetMetrics()
{
     return MakeRequest("GET", "/metrics");
}

/* Triggers a counter update on the server. */

HTTPResponse BenchmarkClient::UpdateCounters(const std::string &prefix)
{
     if (!prefix.empty())
     {
          return MakeRequest("POST", "/update-counters?prefix=" + UrlEncode(prefix));
     }

     return MakeRequest("POST", "/update-counters");
}

/* Triggers a flush and sync on the server. */

HTTPResponse BenchmarkClient::FlushSync()
{
     HTTPResponse response = MakeRequest("POST", "/sync");

     if (response.StatusCode == 200 || response.StatusCode == 201)
     {
          return response;
     }

     return UpdateCounters("");
}

HTTPResponse BenchmarkClient::PauseSAM(uint64_t pause_until_ms)
{
     return MakeRequest("POST", "/sam/pause?pause=" + std::to_string(pause_until_ms));
}

/* Encodes a string for use in a URL. */

std::string BenchmarkClient::UrlEncode(const std::string &value)
{
     std::ostringstream escaped;

     escaped.fill('0');
     escaped << std::hex;

     for (char c : value)
     {
          if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
          {
               escaped << c;
          }
          else
          {
               escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
          }
     }

     return escaped.str();
}
