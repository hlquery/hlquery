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
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/hlquery.h"
#include "runtime/threadlimit.h"

/*
 * High-Performance Connection Pool System.
 *
 * Features:
 * - Connection reuse and pooling.
 * - Automatic connection health checking.
 * - Load balancing across connections.
 * - Connection warming and pre-allocation.
 * - Circuit breaker pattern.
 * - Connection metrics and monitoring.
 * - Async connection management.
 */

/* Connection state. */

enum class ConnectionState
{
     IDLE,
     ACTIVE,
     CONNECTING,
     DISCONNECTED,
     FAILED,
     MAINTENANCE
};

/* Connection metrics. */

struct ConnectionMetrics
{
     std::atomic<uint64_t> TotalRequests{0};
     std::atomic<uint64_t> SuccessfulRequests{0};
     std::atomic<uint64_t> FailedRequests{0};
     std::atomic<uint64_t> BytesSent{0};
     std::atomic<uint64_t> BytesReceived{0};
     std::atomic<uint64_t> ConnectionTimeMS{0};
     std::atomic<uint64_t> LastActivityTime{0};
     std::atomic<uint64_t> AvgResponseTimeMS{0};
     std::atomic<uint64_t> MaxResponseTimeMS{0};
     std::atomic<uint64_t> MinResponseTimeMS{UINT64_MAX};
};

/* Connection wrapper with advanced features. */

class PooledConnection
{
   private:

     /* Underlying socket owned by this pooled connection instance. */

     int SocketFD;

     /* Remote endpoint information used for reconnect and diagnostics. */

     std::string Host;
     uint16_t Port;

     /* Current lifecycle state for pool health decisions. */

     ConnectionState StateValue;

     /* Per-connection counters updated during send and receive operations. */

     ConnectionMetrics Metrics;

     /* Creation and last-use timestamps for age-based eviction. */

     std::chrono::steady_clock::time_point CreatedTime;
     std::chrono::steady_clock::time_point LastUsedTime;

     /* Per-connection lock that serializes acquire and release transitions. */

     std::mutex ConnectionMutex;

     /* Fast flags used by the pool manager without copying the connection. */

     std::atomic<bool> InUse{false};
     std::atomic<uint32_t> RefCount{0};

     /* Circuit breaker. */

     std::atomic<uint32_t> ConsecutiveFailures{0};
     std::atomic<uint64_t> LastFailureTime{0};
     std::atomic<bool> CircuitOpen{false};

     /* Health check. */

     std::atomic<bool> HealthCheckPending{false};
     std::atomic<uint64_t> LastHealthCheck{0};

   public:

     PooledConnection(int FD, const std::string& HostVal, uint16_t PortVal)
         : SocketFD(FD), Host(HostVal), Port(PortVal), StateValue(ConnectionState::IDLE),
           CreatedTime(Instance ? Instance->Now() : std::chrono::steady_clock::now()),
           LastUsedTime(Instance ? Instance->Now() : std::chrono::steady_clock::now())
     {
          Metrics.LastActivityTime = GetCurrentTime();
     }

     ~PooledConnection()
     {
          if (SocketFD >= 0)
          {
               close(SocketFD);
          }
     }

     /* Connection management. */

     bool IsHealthy() const
     {
          if (StateValue != ConnectionState::ACTIVE)
          {
               return false;
          }

          /* Check circuit breaker. */

          if (CircuitOpen.load())
          {
               uint64_t NowVal = GetCurrentTime();
               uint64_t LastFailure = LastFailureTime.load();

               /* Reset circuit breaker after 30 seconds. */

               if (NowVal - LastFailure > 30000)
               {
                    const_cast<PooledConnection*>(this)->CircuitOpen = false;
                    const_cast<PooledConnection*>(this)->ConsecutiveFailures = 0;
               }
               else
               {
                    return false;
               }
          }

          /* Check if connection is too old (1 hour). */

          auto NowVal = Instance ? Instance->Now() : std::chrono::steady_clock::now();

          if (NowVal - CreatedTime > std::chrono::hours(1))
          {
               return false;
          }

          return true;
     }

     bool Acquire()
     {
          std::lock_guard<std::mutex> Lock(ConnectionMutex);

          /* A connection can only be leased when it is idle and healthy. */

          if (InUse.load() || !IsHealthy())
          {
               return false;
          }

          InUse = true;
          RefCount++;
          LastUsedTime = Instance ? Instance->Now() : std::chrono::steady_clock::now();
          Metrics.LastActivityTime = GetCurrentTime();

          return true;
     }

     void Release()
     {
          std::lock_guard<std::mutex> Lock(ConnectionMutex);

          /* Release only updates local lease bookkeeping. The pool decides reuse. */

          InUse = false;
          RefCount--;
          LastUsedTime = Instance ? Instance->Now() : std::chrono::steady_clock::now();
     }

     /* Request handling with metrics. */

     bool SendRequest(const std::string& RequestVal)
     {
          /* Measure write-side latency independently from response processing. */

          auto StartTime = std::chrono::high_resolution_clock::now();

          Metrics.TotalRequests++;

          ssize_t BytesSentVal = send(SocketFD, RequestVal.c_str(), RequestVal.length(), MSG_NOSIGNAL);

          if (BytesSentVal < 0)
          {
               Metrics.FailedRequests++;
               ConsecutiveFailures++;
               LastFailureTime = GetCurrentTime();

               if (ConsecutiveFailures.load() >= 5)
               {
                    CircuitOpen = true;
               }

               return false;
          }

          Metrics.BytesSent += BytesSentVal;
          Metrics.SuccessfulRequests++;
          ConsecutiveFailures = 0;

          auto EndTime = std::chrono::high_resolution_clock::now();
          auto DurationVal = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
          uint64_t ResponseTime = DurationVal.count();

          /* Update response time metrics. */

          Metrics.AvgResponseTimeMS = (Metrics.AvgResponseTimeMS.load() + ResponseTime) / 2;
          Metrics.MaxResponseTimeMS = std::max(Metrics.MaxResponseTimeMS.load(), ResponseTime);
          Metrics.MinResponseTimeMS = std::min(Metrics.MinResponseTimeMS.load(), ResponseTime);

          return true;
     }

     bool ReceiveResponse(std::string& ResponseVal, size_t MaxSize = 1024 * 1024)
     {
          (void)MaxSize;

          /* Read-side latency is tracked separately so transport stalls are visible. */

          auto StartTime = std::chrono::high_resolution_clock::now();

          char Buffer[8192];
          ssize_t BytesReceivedVal = recv(SocketFD, Buffer, sizeof(Buffer), MSG_DONTWAIT);

          if (BytesReceivedVal < 0)
          {
               if (errno == EAGAIN || errno == EWOULDBLOCK)
               {
                    return false; /* No data available. */
               }

               Metrics.FailedRequests++;
               ConsecutiveFailures++;
               LastFailureTime = GetCurrentTime();

               if (ConsecutiveFailures.load() >= 5)
               {
                    CircuitOpen = true;
               }

               return false;
          }

          ResponseVal.assign(Buffer, BytesReceivedVal);
          Metrics.BytesReceived += BytesReceivedVal;

          auto EndTime = std::chrono::high_resolution_clock::now();
          auto DurationVal = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
          uint64_t ResponseTime = DurationVal.count();

          /* Update response time metrics. */

          Metrics.AvgResponseTimeMS = (Metrics.AvgResponseTimeMS.load() + ResponseTime) / 2;
          Metrics.MaxResponseTimeMS = std::max(Metrics.MaxResponseTimeMS.load(), ResponseTime);
          Metrics.MinResponseTimeMS = std::min(Metrics.MinResponseTimeMS.load(), ResponseTime);

          return true;
     }

     /* Getters. */

     int GetSocketFD() const
     {
          return SocketFD;
     }

     const std::string& GetHost() const
     {
          return Host;
     }

     uint16_t GetPort() const
     {
          return Port;
     }

     ConnectionState GetState() const
     {
          return StateValue;
     }

     const ConnectionMetrics& GetMetrics() const
     {
          return Metrics;
     }

     bool IsInUse() const
     {
          return InUse.load();
     }

     uint32_t GetRefCount() const
     {
          return RefCount.load();
     }

     /* Health check. */

     bool PerformHealthCheck()
     {
          if (HealthCheckPending.load())
          {
               return true;
          }

          /* Only one lightweight health probe should run at a time per connection. */

          HealthCheckPending = true;
          LastHealthCheck = GetCurrentTime();

          /* Simple ping/pong health check. */

          std::string PingVal = "PING\r\n";
          std::string PongVal;

          bool SuccessVal = SendRequest(PingVal) && ReceiveResponse(PongVal);

          HealthCheckPending = false;

          return SuccessVal;
     }

   private:

     uint64_t GetCurrentTime() const
     {
          if (Instance)
          {
               auto NowVal = Instance->Now();

               return std::chrono::duration_cast<std::chrono::milliseconds>(NowVal.time_since_epoch()).count();
          }

          return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
     }
};

/* Connection pool with advanced management. */

class AdvancedConnectionPool : public std::enable_shared_from_this<AdvancedConnectionPool>
{
   private:

     /* Shared remote endpoint for every connection managed by this pool. */

     std::string Host;
     uint16_t Port;

     /* Pool sizing bounds and the current live connection count. */

     size_t MinConnections;
     size_t MaxConnectionsVal;
     size_t CurrentConnections;

     /* Owned connections plus the idle queue used for fast lease operations. */

     std::vector<std::shared_ptr<PooledConnection>> Connections;
     std::queue<std::shared_ptr<PooledConnection>> AvailableConnections;
     std::mutex PoolMutex;
     std::condition_variable PoolCV;

     /* Connection management. */

     std::thread ConnectionManager;
     std::thread WarmupThread;
     std::atomic<bool> ManagerRunning{false};
     std::atomic<bool> WarmingUp{false};
     bool ConnectionManagerRegistered{false};

     /* Metrics. */

     /* Pool-level counters used for hit ratio and creation pressure reporting. */

     std::atomic<uint64_t> TotalRequests{0};
     std::atomic<uint64_t> PoolHits{0};
     std::atomic<uint64_t> PoolMisses{0};
     std::atomic<uint64_t> ConnectionCreates{0};
     std::atomic<uint64_t> ConnectionDestroys{0};

     /* Circuit breaker for the entire pool. */

     std::atomic<uint32_t> PoolFailures{0};
     std::atomic<uint64_t> LastPoolFailure{0};
     std::atomic<bool> PoolCircuitOpen{false};

   public:

     AdvancedConnectionPool(const std::string& HostVal, uint16_t PortVal, size_t MinConn = 5, size_t MaxConn = 50)
         : Host(HostVal), Port(PortVal), MinConnections(MinConn),
           MaxConnectionsVal(MaxConn), CurrentConnections(0)
     {
          /* Start connection manager. */

          ManagerRunning = true;
          ConnectionManagerRegistered = ThreadLimit::TryAcquireThreadSlot();

          if (ConnectionManagerRegistered)
          {
               ConnectionManager = std::thread([this]()
                                               {
                                                    ThreadLimit::SetThreadName("hlquery:connmgr");
                                                    ConnectionManagerLoop();
                                               });
          }
          else
          {
               ManagerRunning = false;
          }

          /* Warm up connections. */

          WarmUpConnections();
     }

     ~AdvancedConnectionPool()
     {
          ManagerRunning = false;
          PoolCV.notify_all();

          if (WarmupThread.joinable())
          {
               WarmupThread.join();
          }

          if (ConnectionManager.joinable())
          {
               ConnectionManager.join();
          }

          if (ConnectionManagerRegistered)
          {
               ThreadLimit::DecrementThreadCount();
          }

          /* Close all connections. */

          std::lock_guard<std::mutex> Lock(PoolMutex);

          Connections.clear();
     }

     /* Get connection from pool. */

     std::shared_ptr<PooledConnection> GetConnection()
     {
          std::unique_lock<std::mutex> Lock(PoolMutex);

          TotalRequests++;

          /* Wait until an idle connection exists or the pool may grow. */

          PoolCV.wait(Lock, [this]()
                      {
                           return !AvailableConnections.empty() || CurrentConnections < MaxConnectionsVal;
                      });

          std::shared_ptr<PooledConnection> Conn;

          if (!AvailableConnections.empty())
          {
               Conn = AvailableConnections.front();
               AvailableConnections.pop();
               PoolHits++;
          }
          else if (CurrentConnections < MaxConnectionsVal)
          {
               Conn = CreateNewConnectionLocked();

               if (Conn)
               {
                    PoolMisses++;
               }
          }

          if (!Conn || !Conn->Acquire())
          {
               return nullptr;
          }

          /* Return a shared handle that automatically requeues the connection. */

          std::weak_ptr<AdvancedConnectionPool> PoolWeak = shared_from_this();

          return std::shared_ptr<PooledConnection>(Conn.get(), [PoolWeak, OwnedConnection = Conn](PooledConnection* C) mutable
                                                   {
                                                        C->Release();

                                                        if (auto Pool = PoolWeak.lock())
                                                        {
                                                             Pool->ReturnConnection(OwnedConnection);
                                                        }

                                                        OwnedConnection.reset();
                                                   });
     }

     /* Async connection request. */

     std::future<std::shared_ptr<PooledConnection>> GetConnectionAsync()
     {
          return std::async(std::launch::async, [this]()
                            {
                                 return GetConnection();
                            });
     }

     /* Return connection to pool. */

     void ReturnConnection(const std::shared_ptr<PooledConnection> &Conn)
     {
          if (!Conn)
          {
               return;
          }

          std::lock_guard<std::mutex> Lock(PoolMutex);

          if (Conn->IsHealthy() && AvailableConnections.size() < MaxConnectionsVal)
          {
               AvailableConnections.push(Conn);
          }
          else
          {
               /* Remove connections that are unhealthy or no longer worth retaining. */

               RemoveConnection(Conn.get());
          }

          PoolCV.notify_one();
     }

     /* Pool statistics structure. */

     struct PoolStats
     {
          size_t TotalConnections;
          size_t AvailableConnections;
          size_t ActiveConnections;
          uint64_t TotalRequests;
          uint64_t PoolHits;
          uint64_t PoolMisses;
          uint64_t ConnectionCreates;
          uint64_t ConnectionDestroys;
          double HitRatio;
          bool CircuitOpen;
     };

     PoolStats GetStats() const
     {
          std::lock_guard<std::mutex> Lock(PoolMutex);

          /* Snapshot pool counters while the queue and owned storage are stable. */

          PoolStats StatsVal;

          StatsVal.TotalConnections = CurrentConnections;
          StatsVal.AvailableConnections = AvailableConnections.size();
          StatsVal.ActiveConnections = CurrentConnections - AvailableConnections.size();
          StatsVal.TotalRequests = TotalRequests.load();
          StatsVal.PoolHits = PoolHits.load();
          StatsVal.PoolMisses = PoolMisses.load();
          StatsVal.ConnectionCreates = ConnectionCreates.load();
          StatsVal.ConnectionDestroys = ConnectionDestroys.load();
          StatsVal.HitRatio = StatsVal.TotalRequests > 0 ? static_cast<double>(StatsVal.PoolHits) / StatsVal.TotalRequests : 0.0;
          StatsVal.CircuitOpen = PoolCircuitOpen.load();

          return StatsVal;
     }

     /* Health check all connections. */

     void HealthCheckAll()
     {
          std::lock_guard<std::mutex> Lock(PoolMutex);

          for (auto& Conn : Connections)
          {
               if (Conn && !Conn->IsInUse())
               {
                    Conn->PerformHealthCheck();
               }
          }
     }

     /* Warm up connections. */

     void WarmUpConnections()
     {
          WarmingUp = true;

          /* Skip asynchronous warmup when the process cannot reserve a worker slot. */

          if (!ThreadLimit::TryAcquireThreadSlot())
          {
               WarmingUp = false;
               return;
          }

          WarmupThread = std::thread([this]()
                                     {
                                          ThreadLimit::SetThreadName("hlquery:connwarm");

                                          for (size_t I = 0; I < MinConnections; ++I)
                                          {
                                               auto Conn = CreateNewConnection();

                                               if (Conn)
                                               {
                                                    std::lock_guard<std::mutex> Lock(PoolMutex);

                                                    AvailableConnections.push(Conn);
                                               }

                                               std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                          }

                                          WarmingUp = false;
                                          ThreadLimit::DecrementThreadCount();
                                     });
     }

   private:

     std::shared_ptr<PooledConnection> CreateNewConnectionLocked()
     {
          /* Caller already owns PoolMutex and wants a new live socket immediately. */

          int Sock = socket(AF_INET, SOCK_STREAM, 0);

          if (Sock < 0)
          {
               return nullptr;
          }

          struct sockaddr_in Addr;

          Addr.sin_family = AF_INET;
          Addr.sin_port = htons(Port);
          inet_pton(AF_INET, Host.c_str(), &Addr.sin_addr);

          if (connect(Sock, (struct sockaddr*)&Addr, sizeof(Addr)) < 0)
          {
               close(Sock);

               return nullptr;
          }

          /* Set socket options. */

          int Flags = fcntl(Sock, F_GETFL, 0);

          fcntl(Sock, F_SETFL, Flags | O_NONBLOCK);

          auto Conn = std::make_shared<PooledConnection>(Sock, Host, Port);

          Connections.push_back(Conn);
          CurrentConnections++;
          ConnectionCreates++;

          return Conn;
     }

     std::shared_ptr<PooledConnection> CreateNewConnection()
     {
          /* Standalone creation path used by background warmup and maintenance work. */

          int Sock = socket(AF_INET, SOCK_STREAM, 0);

          if (Sock < 0)
          {
               return nullptr;
          }

          struct sockaddr_in Addr;

          Addr.sin_family = AF_INET;
          Addr.sin_port = htons(Port);
          inet_pton(AF_INET, Host.c_str(), &Addr.sin_addr);

          if (connect(Sock, (struct sockaddr*)&Addr, sizeof(Addr)) < 0)
          {
               close(Sock);

               return nullptr;
          }

          /* Set socket options. */

          int Flags = fcntl(Sock, F_GETFL, 0);

          fcntl(Sock, F_SETFL, Flags | O_NONBLOCK);

          auto Conn = std::make_shared<PooledConnection>(Sock, Host, Port);

          std::lock_guard<std::mutex> Lock(PoolMutex);

          Connections.push_back(Conn);
          CurrentConnections++;
          ConnectionCreates++;

          return Conn;
     }

     void RemoveConnection(PooledConnection* Conn)
     {
          /* Erase the owned object so destructor-driven socket close happens once. */

          auto It = std::find_if(Connections.begin(), Connections.end(), [Conn](const std::shared_ptr<PooledConnection> &C)
                                 {
                                      return C.get() == Conn;
                                 });

          if (It != Connections.end())
          {
               Connections.erase(It);
               CurrentConnections--;
               ConnectionDestroys++;
          }
     }

     void ConnectionManagerLoop()
     {
          while (ManagerRunning)
          {
               std::this_thread::sleep_for(std::chrono::seconds(30));

               /* Periodic maintenance keeps idle sockets healthy and capacity steady. */

               HealthCheckAll();

               /* Remove old or broken idle connections before replenishing the floor. */

               CleanupOldConnections();

               /* Rebuild the idle floor after cleanup or transient connection loss. */

               MaintainMinimumConnections();
          }
     }

     void CleanupOldConnections()
     {
          std::lock_guard<std::mutex> Lock(PoolMutex);

          auto NowVal = Instance ? Instance->Now() : std::chrono::steady_clock::now();
          auto It = Connections.begin();

          while (It != Connections.end())
          {
               if (!(*It)->IsHealthy() && !(*It)->IsInUse())
               {
                    It = Connections.erase(It);
                    CurrentConnections--;
                    ConnectionDestroys++;
               }
               else
               {
                    ++It;
               }
          }
     }

     void MaintainMinimumConnections()
     {
          std::lock_guard<std::mutex> Lock(PoolMutex);

          while (CurrentConnections < MinConnections && CurrentConnections < MaxConnectionsVal)
          {
               auto Conn = CreateNewConnectionLocked();

               if (Conn)
               {
                    AvailableConnections.push(Conn);
               }
               else
               {
                    break;
               }
          }
     }
};

/* Global connection pool manager. */

class ConnectionPoolManager
{
   private:

     /* Pools are keyed by host:port so remote endpoints share reusable sockets. */

     std::unordered_map<std::string, std::shared_ptr<AdvancedConnectionPool>> Pools;
     std::mutex ManagerMutex;

   public:

     static ConnectionPoolManager& Instance()
     {
          static ConnectionPoolManager SInstance;

          return SInstance;
     }

     std::shared_ptr<PooledConnection> GetConnection(const std::string& HostVal, uint16_t PortVal)
     {
          /* Lazily create pools so unused endpoints do not reserve background threads. */

          std::string Key = HostVal + ":" + std::to_string(PortVal);

          std::lock_guard<std::mutex> Lock(ManagerMutex);

          auto It = Pools.find(Key);

          if (It == Pools.end())
          {
               Pools[Key] = std::make_shared<AdvancedConnectionPool>(HostVal, PortVal);
               It = Pools.find(Key);
          }

          return It->second->GetConnection();
     }

     void RemovePool(const std::string& HostVal, uint16_t PortVal)
     {
          std::string Key = HostVal + ":" + std::to_string(PortVal);

          std::lock_guard<std::mutex> Lock(ManagerMutex);

          Pools.erase(Key);
     }

     std::vector<std::string> GetPoolStats()
     {
          /* Export lightweight summaries for diagnostics without exposing internals. */

          std::vector<std::string> Stats;

          std::lock_guard<std::mutex> Lock(ManagerMutex);

          for (auto& [Key, Pool] : Pools)
          {
               auto PoolStatsVal = Pool->GetStats();

               Stats.push_back("Pool " + Key + ": " + std::to_string(PoolStatsVal.TotalConnections) + " connections, hit ratio: " + std::to_string(PoolStatsVal.HitRatio));
          }

          return Stats;
     }
};
