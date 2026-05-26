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

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <thread>
#include <unistd.h>

#include "core/config.h"
#include "core/hlquery.h"
#include "listenmanager.h"

/* Constructor */

ListenManager::ListenManager(const std::string &address, int port_num) : BindAddr(address), Port(port_num)
{
     Instance->Logs->Debug("listenmanager", "ListenManager created for " + address + ":" + std::to_string(port_num) + ".");
}

/* Destructor */

ListenManager::~ListenManager()
{
     if (HasFD())
     {
          /* Cache FD before DelFD() to prevent use-after-free */

          int fd_val = GetFD();
          SocketEngine::DelFD(this);

          if (fd_val >= 0)
          {
               close(fd_val);
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("listenmanager", "ListenManager destroyed.");
          }
     }
}

std::vector<std::unique_ptr<ListenManager>> ListenManager::CreateCustomProtocolListeners()
{
     std::vector<std::unique_ptr<ListenManager>> Listeners;

     if (!Instance || !Instance->HasConfig())
     {
          return Listeners;
     }

     const auto &BindConfigs = Instance->GetConfig().GetBindConfigs();
     Listeners.reserve(BindConfigs.size());

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("listenmanager", "Checking " + std::to_string(BindConfigs.size()) + " bind configurations for custom protocols.");
     }

     for (const auto &BindConfigVal : BindConfigs)
     {
          if (BindConfigVal.type == "http" || BindConfigVal.type == "https")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Skipping HTTP/HTTPS port " + std::to_string(BindConfigVal.port) + " (HttpServer is self-contained).");
               }

               continue;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("listenmanager", "Creating ListenManager for custom protocol on " + BindConfigVal.address + ":" + std::to_string(BindConfigVal.port) + ".");
          }

          Listeners.push_back(std::make_unique<ListenManager>(BindConfigVal.address, BindConfigVal.port));
     }

     return Listeners;
}

/* Binds and listens to the socket */

bool ListenManager::BindAndListen()
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("listenmanager", "Custom protocol listeners are not implemented for " + BindAddr + ":" + std::to_string(Port));
     }

     return false;

     /* Create socket */

     int fd_val = socket(AF_INET, SOCK_STREAM, 0);

     if (fd_val < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to create listen socket: " + std::string(strerror(errno)) + ".");
          }

          return false;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("listenmanager", "Created listen socket with fd: " + std::to_string(fd_val) + ".");
     }

     /* 
      * Set socket options.
      *
      * IMPROVEMENT: Set SO_REUSEADDR and SO_REUSEPORT on the server socket before binding
      * to allow immediate reuse of the port after a restart, preventing "address already in use" errors
      */

     int opt = 1;

     if (setsockopt(fd_val, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to set SO_REUSEADDR: " + std::string(strerror(errno)) + ".");
          }

          close(fd_val);
          return false;
     }

     /* Set SO_REUSEPORT for load balancing across multiple processes (if supported) */

     if (setsockopt(fd_val, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
     {
          /* SO_REUSEPORT may not be supported on all systems - log but don't fail */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("listenmanager", "SO_REUSEPORT not supported: " + std::string(strerror(errno)) + ".");
          }
     }

     /* Set non-blocking */

     if (fcntl(fd_val, F_SETFL, fcntl(fd_val, F_GETFL) | O_NONBLOCK) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to set non-blocking: " + std::string(strerror(errno)) + ".");
          }

          close(fd_val);
          return false;
     }

     /* Bind to address */

     memset(&ServerAddr, 0, sizeof(ServerAddr));

     ServerAddr.sin_family = AF_INET;
     ServerAddr.sin_port = htons(Port);

     if (inet_pton(AF_INET, BindAddr.c_str(), &ServerAddr.sin_addr) <= 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Invalid bind address: " + BindAddr + ".");
          }

          close(fd_val);
          return false;
     }

     if (bind(fd_val, reinterpret_cast<struct sockaddr *>(&ServerAddr), sizeof(ServerAddr)) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to bind to " + BindAddr + ":" + std::to_string(Port) + ": " + std::string(strerror(errno)) + ".");
          }

          close(fd_val);
          return false;
     }

     /* Listen */

     if (listen(fd_val, SOMAXCONN) < 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to listen on " + BindAddr + ":" + std::to_string(Port) + ": " + std::string(strerror(errno)) + ".");
          }

          close(fd_val);
          return false;
     }

     /* Set file descriptor and add to socket engine */

     SetFD(fd_val);

     /* Ensure listen fd will not leak across exec() */

     int Clo = fcntl(fd_val, F_GETFD);

     if (Clo != -1)
     {
          fcntl(fd_val, F_SETFD, Clo | FD_CLOEXEC);
     }

     if (!SocketEngine::AddFD(this, EPOLLIN))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("listenmanager", "Failed to add listen socket to socket engine.");
          }

          SetFD(-1);
          close(fd_val);
          return false;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("listenmanager", "ListenManager listening on " + BindAddr + ":" + std::to_string(Port) + ".");
     }

     return true;
}

/* Called when there is a read event */

void ListenManager::OnEventHandlerRead()
{
     struct sockaddr_in ClientAddr;
     socklen_t ClientLen = sizeof(ClientAddr);
     int ConnectionsProcessed = 0;
     bool LimitReached = false;

     /*
      * Accept connections until queue is drained or limit is reached
      * With edge-triggered epoll, we must drain the accept queue fully
      * or re-arm the event, otherwise remaining connections won't trigger new events.
      * We continue accepting until accept() returns EAGAIN/EWOULDBLOCK.
      */
 
      /* HTTP connections are handled by HttpServer */

     while (true)
     {
          if (ConnectionsProcessed >= MAX_CONNECTIONS_PER_TICK)
          {
               LimitReached = true;
               int TestFD = accept(GetFD(), reinterpret_cast<struct sockaddr *>(&ClientAddr), &ClientLen);

               if (TestFD < 0)
               {
                    int SavedErrno = errno;

#if EAGAIN == EWOULDBLOCK

                    if (SavedErrno == EAGAIN)
                    {
#else

                    if (SavedErrno == EAGAIN || SavedErrno == EWOULDBLOCK)
                    {

#endif
                         /* Queue is drained, safe to break */

                         break;
                    }

                    /* Other error - close TestFD if it was valid, then break */

                    if (TestFD >= 0)
                    {
                         close(TestFD);
                    }

                    break;
               }

               /*
                * Still have connections, but we've hit the limit 
                * Close the test connection and break (remaining will be processed next tick) 
                */

               close(TestFD);

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Connection slice limit reached (" + std::to_string(MAX_CONNECTIONS_PER_TICK) + "), processing remaining in next tick.");
               }

               SocketEngine::DelFD(this);
               SocketEngine::AddFD(this, EPOLLIN);

               break;
          }

          int ClientFD = accept(GetFD(), reinterpret_cast<struct sockaddr *>(&ClientAddr), &ClientLen);

          if (ClientFD < 0)
          {
               int SavedErrno = errno;

               /* 
                * EAGAIN and EWOULDBLOCK are the same on Linux, different on some other systems 
                * Use compile-time check to avoid logical-op warning while maintaining portability 
                */

#if EAGAIN == EWOULDBLOCK

               if (SavedErrno == EAGAIN)
               {
#else

               if (SavedErrno == EAGAIN || SavedErrno == EWOULDBLOCK)
               {

#endif
                    /* No more connections to accept - queue is drained */

                    break;
               }

               /* Handle specific errors gracefully */

               if (SavedErrno == EMFILE || SavedErrno == ENFILE)
               {
                    /* Too many open files - log once and break */

                    static bool LoggedFileLimit = false;

                    if (!LoggedFileLimit)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("listenmanager", "Accept failed: Too many open files - reducing connection rate.");
                         }

                         LoggedFileLimit = true;
                    }

                    /* 
                     * Sleep removed for maximum benchmark performance *
                     * Connection pressure will be handled by connection limit checks 
                     */

                    break;
               }
               else if (SavedErrno == ECONNABORTED || SavedErrno == EINTR)
               {
                    /* Connection aborted or interrupted - continue normally */

                    continue;
               }
               else
               {
                    /* Other errors - log and break */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("listenmanager", "Accept failed: " + std::string(strerror(SavedErrno)) + ".");
                    }

                    break;
               }
          }

          char ClientIP[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &ClientAddr.sin_addr, ClientIP, INET_ADDRSTRLEN);
          int ClientPortValue = ntohs(ClientAddr.sin_port);

          /* Set CLOEXEC to avoid fd leaking across exec */

          int Flags = fcntl(ClientFD, F_GETFD);

          if (Flags >= 0)
          {
               fcntl(ClientFD, F_SETFD, Flags | FD_CLOEXEC);
          }

          /* Set client socket non-blocking */

          Flags = fcntl(ClientFD, F_GETFL);

          if (Flags >= 0)
          {
               fcntl(ClientFD, F_SETFL, Flags | O_NONBLOCK);
          }

          /* Optimize TCP for low-latency, high-throughput messaging */

          int Yes = 1;

          if (setsockopt(ClientFD, IPPROTO_TCP, TCP_NODELAY, &Yes, sizeof(Yes)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set TCP_NODELAY: " + std::string(strerror(errno)) + ".");
               }
          }

          /* Enable TCP keepalive to detect dead connections */

          if (setsockopt(ClientFD, SOL_SOCKET, SO_KEEPALIVE, &Yes, sizeof(Yes)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set SO_KEEPALIVE: " + std::string(strerror(errno)) + ".");
               }
          }

          /* Configure keepalive parameters for faster dead connection detection */

          int KeepIdle  = 10; /* Start keepalive after 10 seconds of inactivity */
          int KeepIntvl = 5; /* Send keepalive probes every 5 seconds */
          int KeepCnt   = 3; /* Send 3 probes before considering connection dead */

#if defined(__APPLE__) && defined(TCP_KEEPALIVE)

          if (setsockopt(ClientFD, IPPROTO_TCP, TCP_KEEPALIVE, &KeepIdle, sizeof(KeepIdle)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set TCP_KEEPALIVE: " + std::string(strerror(errno)) + ".");
               }
          }

#elif defined(TCP_KEEPIDLE)

          if (setsockopt(ClientFD, IPPROTO_TCP, TCP_KEEPIDLE, &KeepIdle, sizeof(KeepIdle)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set TCP_KEEPIDLE: " + std::string(strerror(errno)) + ".");
               }
          }

#endif

#if defined(TCP_KEEPINTVL)

          if (setsockopt(ClientFD, IPPROTO_TCP, TCP_KEEPINTVL, &KeepIntvl, sizeof(KeepIntvl)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set TCP_KEEPINTVL: " + std::string(strerror(errno)) + ".");
               }
          }
#endif

#if defined(TCP_KEEPCNT)

          if (setsockopt(ClientFD, IPPROTO_TCP, TCP_KEEPCNT, &KeepCnt, sizeof(KeepCnt)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set TCP_KEEPCNT: " + std::string(strerror(errno)) + ".");
               }
          }

#endif

          /* Grow kernel socket buffers for high-bandwidth transfers */

          int BufSize = HLQUERY_SOCKET_BUFFER_SIZE;

          if (setsockopt(ClientFD, SOL_SOCKET, SO_SNDBUF, &BufSize, sizeof(BufSize)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set SO_SNDBUF: " + std::string(strerror(errno)) + ".");
               }
          }

          if (setsockopt(ClientFD, SOL_SOCKET, SO_RCVBUF, &BufSize, sizeof(BufSize)) < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("listenmanager", "Failed to set SO_RCVBUF: " + std::string(strerror(errno)) + ".");
               }
          }

          Instance->Logs->Debug("listenmanager", "Accepted connection from " + std::string(ClientIP) + ":" + std::to_string(ClientPortValue) + " (fd: " + std::to_string(ClientFD) + ").");
          close(ClientFD);

          ConnectionsProcessed++;
     }

     if (LimitReached && ConnectionsProcessed > 0)
     {

     }
}

/* Called when there is an error event */

void ListenManager::OnEventHandlerError(int ErrorNum)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("listenmanager", "ListenManager error: " + std::string(strerror(ErrorNum)) + ".");
     }
}
