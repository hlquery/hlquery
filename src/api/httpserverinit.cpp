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

#include <filesystem>
#include <memory>

#include "api/httpserver.h"
#include "common/searchpool.h"
#include "core/hlquery.h"
#include "core/logmanager.h"
#include "utils/consolewriter.h"

namespace
{
#ifdef HLQUERY_HAS_OPENSSL
void EnsureOpenSSLInitialized()
{
     static bool OpenSSLInitialized = false;

     if (!OpenSSLInitialized)
     {
          SSL_library_init();

          SSL_load_error_strings();

          OpenSSL_add_all_algorithms();

          OpenSSLInitialized = true;
     }
}
#endif
}
/* Validate SSL settings without binding sockets (preflight). */

bool ValidateSSLConfig(const BindConfig &Config, std::string *ErrorMsg)
{
     if (!Config.ssl)
     {
          return true;
     }

#ifndef HLQUERY_HAS_OPENSSL
     if (ErrorMsg)
     {
          *ErrorMsg = "SSL requested but hlquery was built without OpenSSL support!.";
     }

     return false;
#else
     EnsureOpenSSLInitialized();

     if (Config.ssl_cert.empty() || !std::filesystem::exists(Config.ssl_cert))
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "SSL certificate file not found: '" + Config.ssl_cert + "'.";
          }

          return false;
     }

     if (Config.ssl_key.empty() || !std::filesystem::exists(Config.ssl_key))
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "SSL private key file not found: '" + Config.ssl_key + "'.";
          }

          return false;
     }

     const SSL_METHOD *SSLMethod = TLS_server_method();

     SSL_CTX *SSLCtx = SSL_CTX_new(SSLMethod);

     if (!SSLCtx)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Failed to create SSL context.";
          }

          return false;
     }

     if (Config.ssl_protocols.find("TLSv1.3") != std::string::npos)
     {
          SSL_CTX_set_min_proto_version(SSLCtx, TLS1_3_VERSION);
     }
     else if (Config.ssl_protocols.find("TLSv1.2") != std::string::npos)
     {
          SSL_CTX_set_min_proto_version(SSLCtx, TLS1_2_VERSION);
     }

     if (!Config.ssl_ciphers.empty())
     {
          SSL_CTX_set_cipher_list(SSLCtx, Config.ssl_ciphers.c_str());
     }

     if (SSL_CTX_use_certificate_chain_file(SSLCtx, Config.ssl_cert.c_str()) <= 0)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Failed to load SSL certificate chain: " + Config.ssl_cert + " (check file format).";
          }

          SSL_CTX_free(SSLCtx);

          return false;
     }

     if (SSL_CTX_use_PrivateKey_file(SSLCtx, Config.ssl_key.c_str(), SSL_FILETYPE_PEM) <= 0)
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "Failed to load SSL private key: " + Config.ssl_key + " (check file format).";
          }

          SSL_CTX_free(SSLCtx);

          return false;
     }

     if (!SSL_CTX_check_private_key(SSLCtx))
     {
          if (ErrorMsg)
          {
               *ErrorMsg = "SSL private key does not match certificate: " + Config.ssl_key + ".";
          }

          SSL_CTX_free(SSLCtx);

          return false;
     }

     SSL_CTX_free(SSLCtx);

     return true;
#endif
}

/* InitializeHttpServer initializes and starts the HTTP server. */

bool InitializeHttpServer(const BindConfig &Config, HttpServer *&HttpServerPtr, LogManager *LogsPtr)
{
#ifdef HLQUERY_HAS_OPENSSL
     EnsureOpenSSLInitialized();
#endif

     HttpServer *NewServer = nullptr;

     try
     {
          if (HttpServerPtr)
          {
               HttpServerPtr->Stop();

               delete HttpServerPtr;

               HttpServerPtr = nullptr;
          }

          auto ServerObj = std::make_unique<HttpServer>(Config);

          if (Instance && Instance->ThreadPools)
          {
               ServerObj->SetThreadPool(&Instance->ThreadPools->GetHTTPPool());
          }
          else
          {
               ServerObj->SetThreadPool(&ThreadPoolManager::GetInstance().GetHTTPPool());
          }

          if (!ServerObj->Start())
          {
               if (LogsPtr)
               {
                    LogsPtr->Normal("hlquery", "Failed to start HTTP server on port " + std::to_string(Config.port) + ".");
               }

               ConsoleWriter::WriteError("[FATAL] Failed to start HTTP server on port " + std::to_string(Config.port) + " (" + Config.type + ").", true);

               return false;
          }

          NewServer = ServerObj.release();

          HttpServerPtr = NewServer;

          if (LogsPtr)
          {
               LogsPtr->Normal("hlquery", "HTTP server started on port " + std::to_string(Config.port) + ".");
          }

          return true;
     }
     catch (const std::exception &E)
     {
          if (NewServer && HttpServerPtr != NewServer)
          {
               delete NewServer;

               NewServer = nullptr;
          }

          if (LogsPtr)
          {
               LogsPtr->Normal("hlquery", "Exception starting HTTP server: " + std::string(E.what()) + ".");
          }

          if (HttpServerPtr)
          {
               delete HttpServerPtr;

               HttpServerPtr = nullptr;
          }

          return false;
     }
     catch (...)
     {
          if (NewServer && HttpServerPtr != NewServer)
          {
               delete NewServer;

               NewServer = nullptr;
          }

          if (LogsPtr)
          {
               LogsPtr->Normal("hlquery", "Unknown exception starting HTTP server.");
          }

          if (HttpServerPtr)
          {
               delete HttpServerPtr;

               HttpServerPtr = nullptr;
          }

          return false;
     }
}

/* ShutdownHttpServer shuts down the HTTP server. */

void ShutdownHttpServer(HttpServer *&HttpServerPtr)
{
     if (!HttpServerPtr)
     {
          return;
     }

     try
     {
          HttpServerPtr->Stop();
     }
     catch (...)
     {
     }

     delete HttpServerPtr;

     HttpServerPtr = nullptr;
}
