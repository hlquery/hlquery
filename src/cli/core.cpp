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
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>

#include "core/config.h"

#ifdef HLQUERY_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <vendor/json/json.hpp>

#include "core/typedefs.h"
#include "cli/cliutils.h"
#include "app.h"
#include "utils/consolewriter.h"

/* HLQueryCLI constructor. */

HLQueryCLI::HLQueryCLI(const std::string &url, bool raw, const std::string &token, const std::string &program_name, bool ssl_auth)
    : BaseURL(url),
      RawMode(raw), AuthToken(token), SSLAuthMode(ssl_auth), ProgramName(program_name), ExitCodeValue(0)
{
     if (AuthToken.empty())
     {
          SSLAuthMode = false;
     }
}

/* Gets the exit code. */

int HLQueryCLI::GetExitCode() const
{
     return ExitCodeValue;
}

void HLQueryCLI::ReconfigureConnection(const std::string &url)
{
     BaseURL = url;
}

void HLQueryCLI::SetDefaultTimeoutSeconds(int timeout_seconds)
{
     if (timeout_seconds < 1)
     {
          timeout_seconds = 1;
     }

     DefaultTimeoutSeconds = timeout_seconds;
}

/* Gets the program display name. */

const std::string &HLQueryCLI::GetProgramName() const
{
     return ProgramName;
}

/* Sets the exit code. */

void HLQueryCLI::SetExitCode(int code)
{
     ExitCodeValue = code;
}

/* Gets the current timestamp. */

std::string HLQueryCLI::GetCurrentTimestamp()
{
     /* Capture the current time and derive milliseconds. */

     auto now = std::chrono::system_clock::now();
     auto time_t_val = std::chrono::system_clock::to_time_t(now);

     auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

     long long ms_val = ms_since_epoch % 1000;

     struct tm tm_buf;

     /* Convert to local time, with a fallback to epoch seconds. */

     struct tm *tm = localtime_r(&time_t_val, &tm_buf);

     if (!tm)
     {
          return std::to_string(time_t_val) + ".000";
     }

     std::ostringstream oss;

     /* Format timestamp with millisecond precision. */

     oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
     oss << '.' << std::setfill('0') << std::setw(3) << ms_val;

     return oss.str();
}

/* Prints an error message. */

void HLQueryCLI::PrintError(const std::string &message, const std::string &details)
{
     std::string full_message = message;

     if (!details.empty())
     {
          full_message += " (" + details + ")";
     }

     if (ProgramName == "talk")
     {
          std::cout << ConsoleWriter::EnsurePeriod(full_message, true) << std::endl;
          return;
     }

     full_message = "Error: " + full_message;

     ConsoleWriter::WriteError(full_message, true);
}

/* Prints a success message. */

void HLQueryCLI::PrintSuccess(const std::string &message)
{
     std::cout << message;

     if (!message.empty() && message.back() != '.')
     {
          std::cout << ".";
     }

     newline();
}

/* Prints an info message. */

void HLQueryCLI::PrintInfo(const std::string &message)
{
     std::cout << message;

     if (!message.empty() && message.back() != '.')
     {
          std::cout << ".";
     }

     newline();
}

/* Helper to format bytes. */

std::string HLQueryCLI::FormatBytes(uint64_t bytes)
{
     if (bytes < 1024)
     {
          return std::to_string(bytes) + " B";
     }
     else if (bytes < 1024 * 1024)
     {
          return std::to_string(bytes / 1024) + " KB";
     }
     else if (bytes < 1024ULL * 1024 * 1024)
     {
          return std::to_string(bytes / (1024 * 1024)) + " MB";
     }
     else
     {
          return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
     }
}

/* Checks if a request failed. */

bool HLQueryCLI::CheckRequestFailed(const HTTPResponse &response, bool silent_on_connection_failure, const std::string &endpoint)
{
     /* Handle transport failures first because there is no HTTP status. */

     if (response.StatusCode == -1)
     {
          if (!silent_on_connection_failure)
          {
               std::string error_msg = "Request failed: Could not connect to server";

               std::string hint = "Check if server is running at " + BaseURL;
               if (!response.Body.empty())
               {
                    std::string reason = response.Body;
                    while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r'))
                    {
                         reason.pop_back();
                    }
                    hint += " (transport: " + reason + ")";
               }

               if (!endpoint.empty())
               {
                    error_msg += " (endpoint: " + endpoint + ")";
               }

               PrintError(error_msg, hint);

               SetExitCode(2);
          }

          return true;
     }

     if (response.StatusCode == 401 || response.StatusCode == 403)
     {
          std::string error_msg = "Unauthorized";

          if (!endpoint.empty())
          {
               error_msg += " (endpoint: " + endpoint + ")";
          }

          error_msg += " (HTTP " + std::to_string(response.StatusCode) + ")";

          PrintError(error_msg, "Check authentication token - maybe server still booting?");

          SetExitCode(2);

          return true;
     }

     if (response.StatusCode == 503)
     {
          std::string error_msg = "Service unavailable";
          std::string error_details = "Server may still be loading metadata - wait a moment and retry";

          if (!endpoint.empty())
          {
               error_msg += " (endpoint: " + endpoint + ")";
          }

          error_msg += " (HTTP " + std::to_string(response.StatusCode) + ")";

          if (!response.Body.empty())
          {
               try
               {
                    nlohmann::json error_json = nlohmann::json::parse(response.Body);

                    if (error_json.contains("error") && error_json["error"].is_string())
                    {
                         error_msg = error_json["error"].get<std::string>();
                         error_msg += " (HTTP " + std::to_string(response.StatusCode) + ")";
                    }

                    if (error_json.contains("message") && error_json["message"].is_string())
                    {
                         error_details = error_json["message"].get<std::string>();
                    }

                    if (error_json.contains("code_text") && error_json["code_text"].is_string())
                    {
                         std::string CodeText = error_json["code_text"].get<std::string>();
                         if (!CodeText.empty())
                         {
                              error_msg = CodeText + " (HTTP " + std::to_string(response.StatusCode) + ")";
                         }
                    }
               }
               catch (...)
               {
                    if (response.Body.length() > 200)
                    {
                         error_details = response.Body.substr(0, 200) + "...";
                    }
                    else
                    {
                         error_details = response.Body;
                    }
               }
          }

          PrintError(error_msg, error_details);

          SetExitCode(2);

          return true;
     }

     if (response.StatusCode < 200 || response.StatusCode >= 300)
     {
          std::string error_msg = "HTTP " + std::to_string(response.StatusCode);

          std::string error_details = "";

          if (!response.Body.empty())
          {
               /* Attempt to parse structured error messages from JSON. */

               try
               {
                    nlohmann::json error_json = nlohmann::json::parse(response.Body);

                    if (error_json.contains("error"))
                    {
                         error_msg = error_json["error"].get<std::string>();

                         if (error_json.contains("message"))
                         {
                              error_details = error_json["message"].get<std::string>();
                         }
                    }
                    else if (error_json.contains("message"))
                    {
                         error_msg = error_json["message"].get<std::string>();
                    }

                    if (error_json.contains("code_text") && error_json["code_text"].is_string())
                    {
                         std::string CodeText = error_json["code_text"].get<std::string>();
                         if (!CodeText.empty())
                         {
                              error_msg = CodeText;
                         }
                    }
               }
               catch (...)
               {
                    /* Fall back to a shortened raw body on parse errors. */

                    if (response.Body.length() > 200)
                    {
                         error_details = response.Body.substr(0, 200) + "...";
                    }
                    else
                    {
                         error_details = response.Body;
                    }
               }
          }

          if (error_details.empty() && response.StatusCode == 404)
          {
               error_details = "Resource not found.";
          }
          else if (error_details.empty() && response.StatusCode == 400)
          {
               error_details = "Bad request - check input parameters.";
          }
          else if (error_details.empty() && response.StatusCode == 401)
          {
               error_details = "Unauthorized - check authentication.";
          }
          else if (error_details.empty() && response.StatusCode == 403)
          {
               error_details = "Forbidden - insufficient permissions.";
          }
          else if (error_details.empty() && response.StatusCode == 500)
          {
               error_details = "Internal server error.";
          }
          else if (error_details.empty() && response.StatusCode == 503)
          {
               error_details = "Service unavailable - server may be starting up.";
          }

          PrintError(error_msg, error_details);

          SetExitCode(2);

          return true;
     }

     return false;
}

/* Confirms a destructive action. */

bool HLQueryCLI::ConfirmDestructiveAction(const std::string &action, const std::string &target)
{
     std::cout << "\n WARNING: You are about to " << action << " '" << target << "'.\n";
     std::cout << " This action is DESTRUCTIVE and cannot be undone.\n\n";
     std::cout << " Are you sure you want to proceed? (y/N): ";

     std::string response;
     std::getline(std::cin, response);

     if (response == "y" || response == "Y")
     {
          return true;
     }

     std::cout << "\n Action cancelled." << std::endl;
     return false;
}

/* Checks if the server is loading. */

bool HLQueryCLI::IsServerLoading()
{
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", "/doctotal", "", DefaultTimeoutSeconds);

     if (response.StatusCode == 503)
     {
          return true;
     }

     HLQueryCLI::HTTPResponse status_resp = MakeRequest("GET", "/status", "", DefaultTimeoutSeconds);

     if (status_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json status_json = nlohmann::json::parse(status_resp.Body);

               if (status_json.contains("loading") && status_json["loading"].get<bool>())
               {
                    return true;
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     return false;
}

/* Makes an HTTP request. */

HLQueryCLI::HTTPResponse HLQueryCLI::MakeRequest(const std::string &method, const std::string &path, const std::string &body, int timeout_seconds)
{
     static thread_local int TLSHandshakeRetryDepth = 0;

     /* Normalize timeouts and validate request inputs. */

     if (timeout_seconds < 0)
     {
          timeout_seconds = DefaultTimeoutSeconds;
     }

     HTTPResponse response;

     if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE")
     {
          response.StatusCode = -1;
          response.Body = "Invalid HTTP method: " + method + ".\n";

          return response;
     }

     if (path.find("..") != std::string::npos || path.find("//") != std::string::npos)
     {
          response.StatusCode = -1;
          response.Body = "Invalid path: " + path + ".\n";

          return response;
     }

     /* Resolve the base URL into host and port values. */

     bool UseSSL = false;
     if (BaseURL.find("://") != std::string::npos)
     {
          size_t SchemePos = BaseURL.find("://");
          std::string Scheme = BaseURL.substr(0, SchemePos);
          std::transform(Scheme.begin(), Scheme.end(), Scheme.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });
          UseSSL = (Scheme == "https");
          size_t start = SchemePos + 3;

          size_t colon_pos = BaseURL.find(":", start);
          size_t slash_pos = BaseURL.find("/", start);
          if (slash_pos == std::string::npos)
          {
               slash_pos = BaseURL.size();
          }

          if (colon_pos != std::string::npos && (slash_pos == std::string::npos || colon_pos < slash_pos))
          {
               Host = BaseURL.substr(start, colon_pos - start);
               Port = std::stoi(BaseURL.substr(colon_pos + 1, slash_pos - colon_pos - 1));
          }
          else
          {
               Host = BaseURL.substr(start, slash_pos - start);
               Port = UseSSL ? 443 : 9200;
          }
     }
     else
     {
          Host = "localhost";
          Port = 9200;
     }

     /* Open and configure a TCP socket for the HTTP request. */

     int sock = socket(AF_INET, SOCK_STREAM, 0);

     if (sock < 0)
     {
          response.StatusCode = -1;
          response.Body = "Failed to create socket.\n";

          return response;
     }

     struct timeval timeout;

     timeout.tv_sec = timeout_seconds;
     timeout.tv_usec = 0;

     setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
     setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

     struct hostent *server = gethostbyname(Host.c_str());

     if (server == NULL)
     {
          response.StatusCode = -1;
          response.Body = "Failed to resolve hostname.\n";

          close(sock);

          return response;
     }

     struct sockaddr_in serv_addr;

     memset(&serv_addr, 0, sizeof(serv_addr));

     serv_addr.sin_family = AF_INET;

     memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

     serv_addr.sin_port = htons(Port);

     int flags = fcntl(sock, F_GETFL, 0);
     if (flags >= 0)
     {
          (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
     }

     int connect_result = connect(sock, reinterpret_cast<struct sockaddr *>(&serv_addr), sizeof(serv_addr));
     if (connect_result < 0 && errno == EINPROGRESS)
     {
          fd_set write_fds;
          FD_ZERO(&write_fds);
          FD_SET(sock, &write_fds);

          struct timeval connect_timeout;
          connect_timeout.tv_sec = timeout_seconds;
          connect_timeout.tv_usec = 0;

          int select_result = select(sock + 1, nullptr, &write_fds, nullptr, &connect_timeout);
          if (select_result <= 0)
          {
               response.StatusCode = -1;
               response.Body = "Failed to connect to server.\n";
               close(sock);
               return response;
          }

          int socket_error = 0;
          socklen_t socket_error_len = sizeof(socket_error);
          if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) < 0 || socket_error != 0)
          {
               response.StatusCode = -1;
               response.Body = "Failed to connect to server.\n";
               close(sock);
               return response;
          }
     }
     else if (connect_result < 0)
     {
          response.StatusCode = -1;
          response.Body = "Failed to connect to server.\n";

          close(sock);

          return response;
     }

     if (flags >= 0)
     {
          (void)fcntl(sock, F_SETFL, flags);
     }

#ifndef HLQUERY_HAS_OPENSSL
     if (UseSSL)
     {
          response.StatusCode = -1;
          response.Body = "HTTPS support is unavailable in this build because OpenSSL support was not enabled.\n";
          close(sock);
          return response;
     }
#endif

#ifdef HLQUERY_HAS_OPENSSL
     SSL_CTX *SSLCtx = nullptr;
     SSL *SSLObj = nullptr;

     if (UseSSL)
     {
          static std::once_flag OpenSSLInitOnce;
          std::call_once(OpenSSLInitOnce, []()
                         {
                              SSL_library_init();
                              SSL_load_error_strings();
                              OpenSSL_add_ssl_algorithms();
                         });

          SSLCtx = SSL_CTX_new(TLS_client_method());
          if (!SSLCtx)
          {
               response.StatusCode = -1;
               response.Body = "Failed to initialize TLS context.\n";
               close(sock);
               return response;
          }

          /* Accept self-signed certs for local/dev use. */

          SSL_CTX_set_verify(SSLCtx, SSL_VERIFY_NONE, nullptr);

          SSLObj = SSL_new(SSLCtx);
          if (!SSLObj)
          {
               response.StatusCode = -1;
               response.Body = "Failed to initialize TLS session.\n";
               SSL_CTX_free(SSLCtx);
               close(sock);
               return response;
          }

          SSL_set_fd(SSLObj, sock);
          if (!Host.empty())
          {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
               SSL_set_tlsext_host_name(SSLObj, Host.c_str());
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
          }

          if (SSL_connect(SSLObj) != 1)
          {
               response.StatusCode = -1;
               unsigned long SslErr = ERR_get_error();
               std::string ErrText = (SslErr != 0) ? ERR_error_string(SslErr, nullptr) : "unknown SSL error";
               response.Body = "TLS handshake failed: " + ErrText + ".\n";
               SSL_free(SSLObj);
               SSL_CTX_free(SSLCtx);
               close(sock);

               const bool RetryableHandshake = ErrText.find("unexpected message") != std::string::npos;
               if (RetryableHandshake && TLSHandshakeRetryDepth < 2)
               {
                    ++TLSHandshakeRetryDepth;
                    usleep(150000);
                    HTTPResponse RetryResponse = MakeRequest(method, path, body, timeout_seconds);
                    --TLSHandshakeRetryDepth;
                    return RetryResponse;
               }

               return response;
          }
     }
#endif

     std::stringstream request;

     /* Build the HTTP request headers and body. */

     request << method << " " << path << " HTTP/1.1\r\n";
     request << "Host: " << Host << ":" << Port << "\r\n";
     request << "User-Agent: hlquery-cli/1.0\r\n";
     request << "Accept: application/json\r\n";
     request << "Connection: close\r\n";

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

     request << "\r\n";

     if (!body.empty())
     {
          request << body;
     }

     if (RawMode)
     {
          /* Emit raw HTTP for troubleshooting when requested. */

          std::cout << "[" << GetCurrentTimestamp() << "] RAW HTTP REQUEST." << std::endl;
          std::cout << request.str() << std::endl;
          std::cout << "" << std::endl;
     }

     std::string request_str = request.str();

     int SendResult = -1;
#ifdef HLQUERY_HAS_OPENSSL
     if (UseSSL && SSLObj)
     {
          SendResult = SSL_write(SSLObj, request_str.c_str(), static_cast<int>(request_str.length()));
     }
     else
#endif
     {
          SendResult = static_cast<int>(send(sock, request_str.c_str(), request_str.length(), 0));
     }

     if (SendResult < 0)
     {
          response.StatusCode = -1;
          response.Body = "Failed to send request.\n";

#ifdef HLQUERY_HAS_OPENSSL
          if (SSLObj)
          {
               SSL_free(SSLObj);
               SSLObj = nullptr;
          }
          if (SSLCtx)
          {
               SSL_CTX_free(SSLCtx);
               SSLCtx = nullptr;
          }
#endif
          close(sock);

          return response;
     }

     char buffer[4096];

     std::string response_str;

     response_str.reserve(8192);

     int bytes_received = 0;

     size_t max_response_size = 100 * 1024 * 1024;

     bool has_headers = false;
     bool has_content_length = false;

     int expected_content_length = 0;

     size_t header_end_pos = 0;

     /* Read the response stream until the server closes the connection. */

     while (true)
     {
          bytes_received = -1;
#ifdef HLQUERY_HAS_OPENSSL
          if (UseSSL && SSLObj)
          {
               bytes_received = SSL_read(SSLObj, buffer, sizeof(buffer) - 1);
          }
          else
#endif
          {
               bytes_received = static_cast<int>(recv(sock, buffer, sizeof(buffer) - 1, 0));
          }

          if (bytes_received == 0)
          {
               break;
          }

          if (bytes_received < 0)
          {
               break;
          }

          buffer[bytes_received] = '\0';

          if (response_str.size() + bytes_received > max_response_size)
          {
               close(sock);
#ifdef HLQUERY_HAS_OPENSSL
               if (SSLObj)
               {
                    SSL_free(SSLObj);
                    SSLObj = nullptr;
               }
               if (SSLCtx)
               {
                    SSL_CTX_free(SSLCtx);
                    SSLCtx = nullptr;
               }
#endif

               response.StatusCode = -1;
               response.Body = "Response too large (>" + std::to_string(max_response_size / (1024 * 1024)) + "MB).\n";

               return response;
          }

          response_str += buffer;

          if (!has_headers && response_str.find("\r\n\r\n") != std::string::npos)
          {
               /* Parse headers early to detect content length limits. */

               has_headers = true;
               header_end_pos = response_str.find("\r\n\r\n");

               size_t content_length_pos = response_str.find("Content-Length: ");

               if (content_length_pos != std::string::npos)
               {
                    has_content_length = true;

                    size_t content_length_end = response_str.find("\r\n", content_length_pos);

                    try
                    {
                         expected_content_length = std::stoi(response_str.substr(content_length_pos + 16, content_length_end - content_length_pos - 16));
                    }
                    catch (...)
                    {
                         has_content_length = false;
                    }

                    if (has_content_length && expected_content_length > static_cast<int>(max_response_size))
                    {
                         close(sock);

                         response.StatusCode = -1;
                         response.Body = "Content-Length too large (>" + std::to_string(max_response_size / (1024 * 1024)) + "MB).\n";

                         return response;
                    }
               }
          }

          if (has_headers && has_content_length)
          {
               size_t content_start = header_end_pos + 4;

               if (response_str.length() - content_start >= static_cast<size_t>(expected_content_length))
               {
                    break;
               }
          }
     }

     /* Fail on read errors after the receive loop completes. */

     if (bytes_received < 0)
     {
#ifdef HLQUERY_HAS_OPENSSL
          if (SSLObj)
          {
               SSL_free(SSLObj);
               SSLObj = nullptr;
          }
          if (SSLCtx)
          {
               SSL_CTX_free(SSLCtx);
               SSLCtx = nullptr;
          }
#endif
          close(sock);

          response.StatusCode = -1;

          if (errno == EAGAIN || errno == ETIMEDOUT)
          {
               response.Body = "Request timed out after " + std::to_string(timeout_seconds) + " seconds. Try reducing limit or using pagination.\n";
          }
          else
          {
               response.Body = "Error receiving data from server (errno=" + std::to_string(errno) + ").\n";
          }

          return response;
     }

#ifdef HLQUERY_HAS_OPENSSL
     if (SSLObj)
     {
          SSL_free(SSLObj);
          SSLObj = nullptr;
     }
     if (SSLCtx)
     {
          SSL_CTX_free(SSLCtx);
          SSLCtx = nullptr;
     }
#endif
     close(sock);

     /* Split headers from body and extract the HTTP status code. */

     size_t header_end = response_str.find("\r\n\r\n");

     if (header_end == std::string::npos)
     {
          response.StatusCode = -1;
          response.Body = "Invalid HTTP response.\n";

          return response;
     }

     std::string headers_str = response_str.substr(0, header_end);

     response.Body = response_str.substr(header_end + 4);

     size_t first_space = headers_str.find(' ');
     size_t second_space = headers_str.find(' ', first_space + 1);

     if (first_space != std::string::npos && second_space != std::string::npos)
     {
          response.StatusCode = std::stoi(headers_str.substr(first_space + 1, second_space - first_space - 1));
     }

     if (RawMode)
     {
          /* Emit the raw response for troubleshooting. */

          std::cout << "[" << GetCurrentTimestamp() << "] RAW HTTP RESPONSE." << std::endl;
          std::cout << response_str << std::endl;
          std::cout << "" << std::endl;
     }

     return response;
}

/* Prints a table of data. */

void HLQueryCLI::PrintTable(const std::vector<std::string> &headers, const std::vector<std::vector<std::string>> &rows)
{
     /* Ignore empty tables to keep output clean. */

     if (headers.empty() || rows.empty())
     {
          return;
     }

     size_t terminal_width = 120;

#ifdef TIOCGSIZE
     struct winsize w;

     if (ioctl(STDOUT_FILENO, TIOCGSIZE, &w) == 0 && w.ws_col > 0)
     {
          terminal_width = w.ws_col;
     }
#endif

     std::vector<size_t> col_widths(headers.size(), 0);

     /* Compute column widths with a wider description column. */

     for (size_t i = 0; i < headers.size(); i++)
     {
          col_widths[i] = std::max(col_widths[i], headers[i].length());
     }

     size_t min_value_col_width = 40;

     if (col_widths.size() > 1)
     {
          size_t other_cols_width = 0;

          for (size_t i = 0; i < col_widths.size(); i++)
          {
               if (i != 1)
               {
                    other_cols_width += col_widths[i] + 3;
               }
          }

          size_t available_width = (terminal_width > other_cols_width + 10) ? (terminal_width - other_cols_width - 3) : min_value_col_width;

          for (const auto &row : rows)
          {
               if (row.size() > 1)
               {
                    col_widths[1] = std::max(col_widths[1], std::min(row[1].length(), available_width));
               }
          }

          col_widths[1] = std::max(col_widths[1], min_value_col_width);
     }

     for (const auto &row : rows)
     {
          for (size_t i = 0; i < row.size() && i < col_widths.size(); i++)
          {
               if (i != 1)
               {
                    col_widths[i] = std::max(col_widths[i], std::min(row[i].length(), size_t(30)));
               }
          }
     }

     /* Wrap long values to fit within the calculated column width. */

     auto wrapText = [](const std::string &text, size_t width) -> std::vector<std::string>
     {
          if (text.length() <= width)
          {
               return {text};
          }

          std::vector<std::string> lines;

          size_t pos = 0;

          while (pos < text.length())
          {
               if (pos + width >= text.length())
               {
                    lines.push_back(text.substr(pos));
                    break;
               }

               size_t break_pos = text.find_last_of(" \t", pos + width);

               if (break_pos == std::string::npos || break_pos < pos)
               {
                    break_pos = pos + width;
               }
               else
               {
                    break_pos++;
               }

               lines.push_back(text.substr(pos, break_pos - pos));

               pos = break_pos;

               while (pos < text.length() && (text[pos] == ' ' || text[pos] == '\t'))
               {
                    pos++;
               }
          }

          return lines;
     };

     std::cout << "+";

     for (size_t width : col_widths)
     {
          std::cout << std::string(width + 2, '-') << "+";
     }

     newline();

     std::cout << "|";

     for (size_t i = 0; i < headers.size(); i++)
     {
          std::cout << " " << std::left << std::setw(col_widths[i]) << headers[i] << " |";
     }

     newline();

     std::cout << "+";

     for (size_t width : col_widths)
     {
          std::cout << std::string(width + 2, '-') << "+";
     }

     newline();

     for (const auto &row : rows)
     {
          std::vector<std::vector<std::string>> cell_lines(headers.size());

          size_t max_lines = 1;

          for (size_t i = 0; i < headers.size(); i++)
          {
               std::string cell = (i < row.size()) ? row[i] : "";

               if (cell.length() > col_widths[i])
               {
                    cell_lines[i] = wrapText(cell, col_widths[i]);
                    max_lines = std::max(max_lines, cell_lines[i].size());
               }
               else
               {
                    cell_lines[i].push_back(cell);
               }
          }

          for (size_t line = 0; line < max_lines; line++)
          {
               std::cout << "|";

               for (size_t i = 0; i < headers.size(); i++)
               {
                    std::string cell_content = (line < cell_lines[i].size()) ? cell_lines[i][line] : "";

                    std::cout << " " << std::left << std::setw(col_widths[i]) << cell_content << " |";
               }

               newline();
          }
     }

     std::cout << "+";

     for (size_t width : col_widths)
     {
          std::cout << std::string(width + 2, '-') << "+";
     }

     newline();
}
