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
#include <fstream>
#include <random>
#include <sched.h>
#include <sstream>
#include <unistd.h>
     
#if defined(__linux__)
     
     #include <sys/sysinfo.h>
     
#endif

#include "common/searchpool.h"
#include "core/hlquery.h"
#include "runtime/threadlimit.h"
#include "search/storageengine.h"

namespace
{
     std::chrono::steady_clock::time_point PoolNow()
     {
          if (Instance)
          {
               return Instance->Now();
          }

          return std::chrono::steady_clock::now();
     }
}

SearchThreadPool::SearchThreadPool(const ThreadPoolConfig &config)
    : Config(config), StartTime(PoolNow())
{
     /* Initialize work stealing queues */

     if (Config.EnableWorkStealing)
     {
          WorkerQueues.resize(Config.MaxThreads);
          WorkerQueueMutexes.resize(Config.MaxThreads);

          for (size_t i = 0; i < Config.MaxThreads; ++i)
          {
               WorkerQueueMutexes[i] = std::make_unique<std::mutex>();
          }
     }

     /* Initialize task queue with priority comparator */

     TaskQueue = std::priority_queue<Task, std::vector<Task>, std::function<bool(const Task &, const Task &)>>(
          [](const Task &a, const Task &b)
          {
               /* Higher priority for newer tasks (FIFO within same priority) */

               return a.SubmitTime > b.SubmitTime;
          });
}

SearchThreadPool::~SearchThreadPool()
{
     Shutdown();
}

void SearchThreadPool::Start()
{
     {
          std::lock_guard<std::mutex> Lock(WorkersMutex);

          if (!Workers.empty())
          {
               return;
          }
     }

     /* Calculate available thread slots respecting global limit */

     size_t MaxAllowed = ThreadLimit::GetMaxThreads();

     size_t CurrentThreads = ThreadLimit::GetCurrentThreadCount();

     /* Account for RocksDB threads (they're not registered with ThreadLimit but still count toward max_threads) */

     size_t RocksDBThreads = 0;

     if (Instance && Instance->Database)
     {
          RocksDBThreads = static_cast<size_t>(Instance->Database->GetBackgroundThreadCount());
     }

     /* Available = max - (registered threads + RocksDB threads) */

     size_t TotalUsed = CurrentThreads + RocksDBThreads;

     size_t Available = (TotalUsed < MaxAllowed) ? (MaxAllowed - TotalUsed) : 0;

     /* Thread counts are now managed per-subsystem via configuration files */

     size_t TargetThreads = (Config.CoreThreads > 0) ? Config.CoreThreads : 1;

     /* Cap threads at config max and available slots */

     size_t ThreadsToCreate = std::min({TargetThreads, Config.MaxThreads, Available});

     if (ThreadsToCreate == 0 && Config.MaxThreads > 0 && Available > 0)
     {
          ThreadsToCreate = 1;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("thread_pool", "Thread limit reached, creating minimum 1 thread for pool.");
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("thread_pool", "Starting thread pool with " + std::to_string(ThreadsToCreate) + " threads (requested=" + std::to_string(Config.MaxThreads) + ", available=" + std::to_string(Available) + ", core=" + std::to_string(Config.CoreThreads) + ").");
     }

     for (size_t i = 0; i < ThreadsToCreate; ++i)
     {
          /* Check thread limit before creating */

          if (!ThreadLimit::TryAcquireThreadSlot())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("thread_pool", "Thread limit reached, stopping at " + std::to_string(i) + " threads (max: " + std::to_string(ThreadLimit::GetMaxThreads()) + ").");
               }

               break;
          }

          auto worker = std::make_unique<WorkerThread>();
          WorkerThread *worker_ptr = worker.get();

          {
               std::lock_guard<std::mutex> Lock(WorkersMutex);
               Workers.push_back(std::move(worker));
               ActiveWorkerCount.store(Workers.size(), std::memory_order_release);
          }

          try
          {
               worker_ptr->Thread = std::thread([this, worker_ptr, i]()
                                                {
                                                     /* Set thread name */

                                                     std::string thread_name = "hlquery:search:" + std::to_string(i);

                                                     ThreadLimit::SetThreadName(thread_name.c_str());

                                                     WorkerLoop(worker_ptr, i);
                                                });
          }
          catch (const std::exception &e)
          {
               {
                    std::lock_guard<std::mutex> Lock(WorkersMutex);

                    if (!Workers.empty() && Workers.back().get() == worker_ptr)
                    {
                         Workers.pop_back();
                    }

                    ActiveWorkerCount.store(Workers.size(), std::memory_order_release);
               }

               ThreadLimit::DecrementThreadCount();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("thread_pool", "Thread creation failed: " + std::string(e.what()) + ", stopping at " + std::to_string(i) + " threads.");
               }

               break;
          }
          catch (...)
          {
               {
                    std::lock_guard<std::mutex> Lock(WorkersMutex);

                    if (!Workers.empty() && Workers.back().get() == worker_ptr)
                    {
                         Workers.pop_back();
                    }

                    ActiveWorkerCount.store(Workers.size(), std::memory_order_release);
               }

               ThreadLimit::DecrementThreadCount();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("thread_pool", "Thread creation failed with unknown error, stopping at " + std::to_string(i) + " threads.");
               }

               break;
          }

          std::vector<int> CPUCoresSnapshot;

          {
               std::lock_guard<std::mutex> Lock(WorkersMutex);
               CPUCoresSnapshot = CPUCores;
          }

          if (Config.EnableCPUAffinity && !CPUCoresSnapshot.empty())
          {
               int cpu_core = CPUCoresSnapshot[i % CPUCoresSnapshot.size()];

               SetThreadAffinity(worker_ptr->Thread, cpu_core);

               worker_ptr->CPUCore = cpu_core;
          }
     }
}

void SearchThreadPool::Shutdown()
{
     ShutdownFlag.store(true, std::memory_order_release);

     QueueCV.notify_all();

     std::vector<std::unique_ptr<WorkerThread>> WorkersToJoin;
     size_t NumThreads = 0;

     {
          std::lock_guard<std::mutex> Lock(WorkersMutex);

          NumThreads = Workers.size();

          for (auto &worker : Workers)
          {
               if (worker)
               {
                    worker->Running = false;
               }
          }

          WorkersToJoin.swap(Workers);
          ActiveWorkerCount.store(0, std::memory_order_release);
     }

     QueueCV.notify_all();

     /* Join all worker threads without holding WorkersMutex. */

     for (auto &worker : WorkersToJoin)
     {
          if (worker && worker->Thread.joinable())
          {
               try
               {
                    worker->Thread.join();
               }
               catch (const std::exception &e)
               {
                    /* Log but continue - thread may have already exited */
               }
               catch (...)
               {
                    /* Continue - thread may have already exited */
               }

               /* Unregister thread from ThreadLimit */

               try
               {
                    ThreadLimit::DecrementThreadCount();
               }
               catch (...)
               {
               }
          }
     }

     try
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("thread_pool", "Thread pool shutdown (freed " + std::to_string(NumThreads) + " threads).");
          }
     }
     catch (...)
     {
     }
}

void SearchThreadPool::Pause()
{
     Paused.store(true, std::memory_order_release);
     QueueCV.notify_all();
}

void SearchThreadPool::Resume()
{
     Paused.store(false, std::memory_order_release);
     QueueCV.notify_all();
}

SearchThreadPool::PoolStats SearchThreadPool::GetStats()
{
     PoolStats Stats;

     Stats.ActiveThreads = 0;
     Stats.CompletedTasks = CompletedTasks.load();
     Stats.RejectedTasks = RejectedTasks.load();
     Stats.QueueSize = 0;
     Stats.TotalThreads = 0;
     Stats.AvgTaskTimeMS = 0.0;
     Stats.CPUUtilization = 0.0;

     std::lock_guard<std::mutex> WorkersLock(WorkersMutex);

     Stats.TotalThreads = Workers.size();

     for (const auto &worker : Workers)
     {
          if (worker->Busy.load())
          {
               Stats.ActiveThreads++;
          }
     }

     {
          std::lock_guard<std::mutex> lock(QueueMutex);

          Stats.QueueSize = TaskQueue.size();
     }

     /* Calculate average task time */

     const size_t Completed = CompletedTasks.load(std::memory_order_relaxed);
     const double TotalTaskTime = TotalTaskTimeMS.load(std::memory_order_relaxed);

     if (Completed > 0)
     {
          Stats.AvgTaskTimeMS = TotalTaskTime / static_cast<double>(Completed);
     }

     /* Calculate CPU utilization (simplified) */

     auto now = PoolNow();

     auto TotalTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - StartTime).count();

     if (TotalTime > 0)
     {
          Stats.CPUUtilization = (TotalTaskTime / static_cast<double>(TotalTime)) * 100.0;
     }

     return Stats;
}

void SearchThreadPool::ScaleUp(size_t additional_threads)
{
     size_t CurrentThreads = 0;

     {
          std::lock_guard<std::mutex> Lock(WorkersMutex);
          CurrentThreads = Workers.size();
     }

     size_t TargetThreads = std::min(CurrentThreads + additional_threads, Config.MaxThreads);

     ScaleThreads(TargetThreads);
}

void SearchThreadPool::ScaleDown(size_t threads_to_remove)
{
     size_t CurrentThreads = 0;

     {
          std::lock_guard<std::mutex> Lock(WorkersMutex);
          CurrentThreads = Workers.size();
     }

     size_t RemainingThreads = (threads_to_remove >= CurrentThreads) ? 0 : CurrentThreads - threads_to_remove;
     size_t TargetThreads = std::max(RemainingThreads, Config.CoreThreads);

     ScaleThreads(TargetThreads);
}

void SearchThreadPool::SetCPUAffinity(const std::vector<int> &cpu_cores)
{
     /* Apply affinity to existing threads */

     std::lock_guard<std::mutex> Lock(WorkersMutex);

     CPUCores = cpu_cores;

     for (size_t i = 0; i < Workers.size() && i < CPUCores.size(); ++i)
     {
          SetThreadAffinity(Workers[i]->Thread, CPUCores[i]);

          Workers[i]->CPUCore = CPUCores[i];
     }
}

void SearchThreadPool::EnableNUMAOptimization()
{
     NUMAOptimized = true;

     if (Instance)
     {
          if (Instance && Instance->Metrics)
          {
               Instance->Metrics->Affinity.EnableNUMAOptimization();
          }
     }
}

void SearchThreadPool::WorkerLoop(WorkerThread *worker, size_t worker_id)
{
     if (!worker)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("thread_pool", "WorkerLoop: Worker null for id " + std::to_string(worker_id) + ", exiting.");
          }

          return;
     }

     while (!ShutdownFlag && worker->Running.load())
     {
          Task task;

          bool HasTask = false;

          {
               std::unique_lock<std::mutex> lock(QueueMutex);

               QueueCV.wait(lock,
                            [this, worker]()
                            {
                                 return ShutdownFlag.load(std::memory_order_acquire) ||
                                        !worker->Running.load(std::memory_order_acquire) ||
                                        (!Paused.load(std::memory_order_acquire) && !TaskQueue.empty());
                            });

               if (ShutdownFlag.load(std::memory_order_acquire) || !worker->Running.load(std::memory_order_acquire))
               {
                    break;
               }

               if (Paused.load(std::memory_order_acquire))
               {
                    continue;
               }

               if (!TaskQueue.empty())
               {
                    task = TaskQueue.top();

                    TaskQueue.pop();

                    HasTask = true;
               }
          }

          if (!HasTask && Config.EnableWorkStealing)
          {
               HasTask = TryStealWork(worker_id, task);
          }

          if (HasTask)
          {
               worker->Busy = true;

               auto start_time = PoolNow();

               try
               {
                    task.Function();
               }
               catch (const std::exception &e)
               {
                    /* Log error but continue processing */
               }

               auto end_time = PoolNow();

               UpdateStatistics(start_time, end_time);

               worker->TasksCompleted++;
               worker->LastWorkTime = end_time;
               worker->Busy = false;
          }

          /* REMOVED: Sleep removed for maximum benchmark performance */
          /* Use std::this_thread::yield() instead to avoid busy waiting without adding delay */

          if (!HasTask && !ShutdownFlag && worker->Running.load())
          {
               std::this_thread::yield(); /* Yield CPU without sleeping */
          }
     }
}

void SearchThreadPool::SetThreadAffinity(std::thread &thread, int cpu_core)
{
#if defined(__linux__)
     if (!thread.joinable())
     {
          return;
     }

     cpu_set_t cpuset;

     CPU_ZERO(&cpuset);
     CPU_SET(cpu_core, &cpuset);

     pthread_t thread_handle = thread.native_handle();

     if (pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset) != 0)
     {
          /* Failed to set affinity, continue without it */
     }
#else
     (void)thread;
     (void)cpu_core;
#endif
}

bool SearchThreadPool::TryStealWork(size_t worker_id, Task &task)
{
     if (!Config.EnableWorkStealing)
     {
          return false;
     }

     /* Try to steal from other workers */

     size_t NumWorkers = ActiveWorkerCount.load(std::memory_order_acquire);

     for (size_t i = 0; i < NumWorkers; ++i)
     {
          size_t TargetWorker = (worker_id + i + 1) % NumWorkers;

          if (TargetWorker == worker_id)
          {
               continue;
          }

          std::lock_guard<std::mutex> lock(*WorkerQueueMutexes[TargetWorker]);

          if (!WorkerQueues[TargetWorker].empty())
          {
               task = WorkerQueues[TargetWorker].front();

               WorkerQueues[TargetWorker].pop();

               return true;
          }
     }

     return false;
}

void SearchThreadPool::UpdateStatistics(const std::chrono::steady_clock::time_point &start, const std::chrono::steady_clock::time_point &end)
{
     auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

     TotalTaskTimeMS.fetch_add(duration.count() / 1000.0, std::memory_order_relaxed);

     CompletedTasks.fetch_add(1);
}

void SearchThreadPool::ScaleThreads(size_t target_threads)
{
     std::vector<std::unique_ptr<WorkerThread>> WorkersToJoin;
     size_t ThreadsToRemove = 0;

     {
          std::lock_guard<std::mutex> Lock(WorkersMutex);

          size_t CurrentThreads = Workers.size();

          if (target_threads > CurrentThreads)
          {
               /* Scale up: create additional threads */

               for (size_t i = CurrentThreads; i < target_threads; ++i)
               {
                    if (!ThreadLimit::TryAcquireThreadSlot())
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("thread_pool", "Thread limit reached during scale-up, stopping at " + std::to_string(i) + " threads (max: " + std::to_string(ThreadLimit::GetMaxThreads()) + ").");
                         }

                         break;
                    }

                    try
                    {
                         auto worker = std::make_unique<WorkerThread>();
                         WorkerThread *worker_ptr = worker.get();

                         Workers.push_back(std::move(worker));
                         ActiveWorkerCount.store(Workers.size(), std::memory_order_release);

                         worker_ptr->Thread = std::thread([this, worker_ptr, i]()
                                                          {
                                                               /* Set thread name */

                                                               std::string thread_name = "hlquery:search:" + std::to_string(i);

                                                               ThreadLimit::SetThreadName(thread_name.c_str());

                                                               WorkerLoop(worker_ptr, i);
                                                          });

                         if (Config.EnableCPUAffinity && !CPUCores.empty())
                         {
                              int cpu_core = CPUCores[i % CPUCores.size()];

                              SetThreadAffinity(worker_ptr->Thread, cpu_core);

                              worker_ptr->CPUCore = cpu_core;
                         }
                    }
                    catch (const std::exception &e)
                    {
                         /* Thread creation failed - unregister to maintain accurate count */

                         if (!Workers.empty())
                         {
                              Workers.pop_back();
                              ActiveWorkerCount.store(Workers.size(), std::memory_order_release);
                         }

                         ThreadLimit::DecrementThreadCount();

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("thread_pool", "Thread creation failed: " + std::string(e.what()) + ", stopping at " + std::to_string(i) + " threads.");
                         }

                         break;
                    }
               }
          }
          else if (target_threads < CurrentThreads)
          {
               /* Scale down: validate minimum requirements before removing threads */

               size_t MinRequired = Config.CoreThreads;

               size_t OriginalTarget = target_threads;

               if (target_threads < MinRequired)
               {
                    target_threads = MinRequired;

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("thread_pool", "Scale-down limited by pool core_threads, adjusting from " + std::to_string(OriginalTarget) + " to " + std::to_string(target_threads) + ".");
                    }
               }

               if (target_threads >= CurrentThreads)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("thread_pool", "Scale-down prevented by minimum requirements, keeping " + std::to_string(CurrentThreads) + " threads.");
                    }

                    return;
               }

               ThreadsToRemove = CurrentThreads - target_threads;

               for (size_t i = target_threads; i < CurrentThreads; ++i)
               {
                    if (i < Workers.size() && Workers[i])
                    {
                         Workers[i]->Running = false;
                    }
               }

               ActiveWorkerCount.store(target_threads, std::memory_order_release);

               WorkersToJoin.reserve(ThreadsToRemove);

               for (size_t i = target_threads; i < CurrentThreads; ++i)
               {
                    WorkersToJoin.push_back(std::move(Workers[i]));
               }

               Workers.erase(Workers.begin() + target_threads, Workers.end());
          }
     }

     if (!WorkersToJoin.empty())
     {
          /* Wake workers so selected tail threads can observe Running=false and exit. */

          QueueCV.notify_all();

          /* Wait for threads to exit without holding WorkersMutex. */

          for (auto &worker : WorkersToJoin)
          {
               if (worker && worker->Thread.joinable())
               {
                    worker->Thread.join(); /* Join instead of detach - daemon mode removed */
               }
          }

          /* Unregister threads after they've been stopped */

          for (size_t i = 0; i < ThreadsToRemove; ++i)
          {
               ThreadLimit::DecrementThreadCount();
          }
     }
}

void SearchThreadPool::OptimizeForNUMA()
{
     if (!NUMAOptimized)
     {
          return;
     }

     /* Implement NUMA-aware thread placement */

     CPUAffinityManager::CPUTopology TopologyResult;

     if (Instance)
     {
          if (Instance && Instance->Metrics)
          {
               TopologyResult = Instance->Metrics->Affinity.DetectTopology();
          }
     }
     else
     {
          return; /* Can't optimize without affinity manager */
     }

     /* Distribute threads across NUMA nodes */

     std::lock_guard<std::mutex> Lock(WorkersMutex);

     for (size_t i = 0; i < Workers.size(); ++i)
     {
          if (TopologyResult.NUMANodes.empty() || TopologyResult.SocketCores.empty())
          {
               continue; /* Skip if topology is invalid */
          }

          int numa_node = i % TopologyResult.NUMANodes.size();

          if (numa_node >= static_cast<int>(TopologyResult.SocketCores.size()) || TopologyResult.SocketCores[numa_node].empty())
          {
               continue; /* Skip if invalid socket/core mapping */
          }

          int cpu_core = TopologyResult.SocketCores[numa_node][i % TopologyResult.SocketCores[numa_node].size()];

          SetThreadAffinity(Workers[i]->Thread, cpu_core);

          Workers[i]->CPUCore = cpu_core;
     }
}

/* ThreadPoolManager implementation */

ThreadPoolManager &ThreadPoolManager::GetInstance()
{
     static ThreadPoolManager instance;

     return instance;
}

/* Legacy free function for backward compatibility */

ThreadPoolManager &ThreadPoolManagerInstance()
{
     return ThreadPoolManager::GetInstance();
}

bool ThreadPoolManager::Initialize()
{
     /* Use mutex to prevent race condition during concurrent initialization */

     std::lock_guard<std::mutex> lock(InitMutex);

     if (Initialized)
     {
          return true;
     }

     /* Thread limit should already be set during startup */

     size_t MaxThreads = ThreadLimit::GetMaxThreads();

     /* Get actual RocksDB background thread count (respects global max_threads limit) */

     size_t RocksDBThreads = 3; /* Default fallback */

     /* Use Instance to access global Instance (not ThreadPoolManager::GetInstance()) */

     if (Instance && Instance->Database)
     {
          RocksDBThreads = static_cast<size_t>(Instance->Database->GetBackgroundThreadCount());
     }

     /* Get thread pool configuration from config (0 = auto-calculate) */

     size_t SearchThreads = 0;

     size_t HTTPThreads = 0;

     size_t WriteThreads = 0;

     size_t MgmtThreads = 0;

     /* Use Instance to access global Instance (not ThreadPoolManager::GetInstance()) */

     if (Instance && Instance->Config)
     {
          SearchThreads = static_cast<size_t>(Instance->Config->SearchPoolThreads);
          HTTPThreads = static_cast<size_t>(Instance->Config->HTTPPoolThreads);
          WriteThreads = static_cast<size_t>(Instance->Config->WritePoolThreads);
          MgmtThreads = static_cast<size_t>(Instance->Config->ManagementPoolThreads);
     }

     /* If any pool is set to 0 (auto), calculate distribution */

     bool AutoCalculate = (SearchThreads == 0 || HTTPThreads == 0 || WriteThreads == 0 || MgmtThreads == 0);

     if (AutoCalculate)
     {
          /* Calculate available threads for pools: max_threads - RocksDB threads - main thread */
          /* Reserve 1 thread for main thread */

          size_t Reserved = RocksDBThreads + 1; /* RocksDB + main thread */
          size_t AvailableForPools = (MaxThreads > Reserved) ? (MaxThreads - Reserved) : 1;

          /* Distribute available threads evenly across 4 pools */

          size_t BaseThreadsPerPool = AvailableForPools / 4;
          size_t ExtraThreads = AvailableForPools % 4;

          if (BaseThreadsPerPool == 0)
          {
               BaseThreadsPerPool = 1; /* At least 1 thread per pool */
          }

          /* Use configured values if set, otherwise use calculated values */

          if (HTTPThreads == 0)
          {
               HTTPThreads = BaseThreadsPerPool + (ExtraThreads >= 1 ? 1 : 0);
          }

          if (SearchThreads == 0)
          {
               SearchThreads = BaseThreadsPerPool + (ExtraThreads >= 2 ? 1 : 0);
          }

          if (WriteThreads == 0)
          {
               WriteThreads = BaseThreadsPerPool + (ExtraThreads >= 3 ? 1 : 0);
          }

          if (MgmtThreads == 0)
          {
               MgmtThreads = BaseThreadsPerPool + (ExtraThreads >= 4 ? 1 : 0);
          }
     }

     /* Validate total doesn't exceed max_threads */

     size_t TotalPoolThreads = HTTPThreads + SearchThreads + WriteThreads + MgmtThreads;

     size_t CurrentReserved = ThreadLimit::GetCurrentThreadCount();

     size_t TotalNeeded = CurrentReserved + TotalPoolThreads + RocksDBThreads;

     if (TotalNeeded > MaxThreads)
     {
          /* Scale down proportionally if we exceed limit */

          size_t AvailableForPools = 0;

          if (MaxThreads > CurrentReserved + RocksDBThreads)
          {
               AvailableForPools = MaxThreads - CurrentReserved - RocksDBThreads;
          }

          if (AvailableForPools == 0)
          {
               HTTPThreads = 0;
               SearchThreads = 0;
               WriteThreads = 0;
               MgmtThreads = 0;
          }
          else
          {
               struct PoolAllocation
               {
                    size_t *Configured;
                    size_t Allocated;
               };

               PoolAllocation PoolAllocations[] = {
                    {&HTTPThreads, 0},
                    {&SearchThreads, 0},
                    {&WriteThreads, 0},
                    {&MgmtThreads, 0},
               };

               size_t RemainingBudget = AvailableForPools;
               size_t RemainingRequested = TotalPoolThreads;

               for (auto &Pool : PoolAllocations)
               {
                    if (RemainingBudget == 0 || RemainingRequested == 0 || *Pool.Configured == 0)
                    {
                         Pool.Allocated = 0;
                         continue;
                    }

                    size_t Share = (*Pool.Configured * RemainingBudget) / RemainingRequested;

                    if (Share == 0)
                    {
                         Share = 1;
                    }

                    Share = std::min(Share, *Pool.Configured);
                    Share = std::min(Share, RemainingBudget);

                    Pool.Allocated = Share;
                    RemainingBudget -= Share;
                    RemainingRequested -= *Pool.Configured;
               }

               if (RemainingBudget > 0)
               {
                    for (auto &Pool : PoolAllocations)
                    {
                         if (RemainingBudget == 0)
                         {
                              break;
                         }

                         if (Pool.Allocated < *Pool.Configured)
                         {
                              size_t Extra = std::min(*Pool.Configured - Pool.Allocated, RemainingBudget);
                              Pool.Allocated += Extra;
                              RemainingBudget -= Extra;
                         }
                    }
               }

               HTTPThreads = PoolAllocations[0].Allocated;
               SearchThreads = PoolAllocations[1].Allocated;
               WriteThreads = PoolAllocations[2].Allocated;
               MgmtThreads = PoolAllocations[3].Allocated;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("thread_pool", "Thread pools scaled down to fit within max_threads=" + std::to_string(MaxThreads) + ".");
          }
     }

     /* Configure HTTP request pool */

     SearchThreadPool::ThreadPoolConfig http_config;

     http_config.CoreThreads = HTTPThreads;
     http_config.MaxThreads = HTTPThreads;
     http_config.KeepAliveTime = std::chrono::milliseconds(60000);
     http_config.QueueCapacity = 10000;
     http_config.EnableCPUAffinity = true;
     http_config.EnableWorkStealing = true;
     http_config.Type = SearchThreadPool::PoolType::HTTP_REQUEST;

     HTTPPool = std::make_unique<SearchThreadPool>(http_config);

     /* Configure search pool */

     SearchThreadPool::ThreadPoolConfig search_config;

     search_config.CoreThreads = SearchThreads;
     search_config.MaxThreads = SearchThreads;
     search_config.KeepAliveTime = std::chrono::milliseconds(30000);
     search_config.QueueCapacity = 5000;
     search_config.EnableCPUAffinity = true;
     search_config.EnableWorkStealing = true;
     search_config.Type = SearchThreadPool::PoolType::SEARCH;

     SearchPool = std::make_unique<SearchThreadPool>(search_config);

     /* Configure write pool */

     SearchThreadPool::ThreadPoolConfig write_config;

     write_config.CoreThreads = WriteThreads;
     write_config.MaxThreads = WriteThreads;
     write_config.KeepAliveTime = std::chrono::milliseconds(120000);
     write_config.QueueCapacity = 2000;
     write_config.EnableCPUAffinity = true;
     write_config.EnableWorkStealing = false; /* Write operations are sequential */

     write_config.Type = SearchThreadPool::PoolType::WRITE;

     WritePool = std::make_unique<SearchThreadPool>(write_config);

     /* Configure management pool */

     SearchThreadPool::ThreadPoolConfig mgmt_config;

     mgmt_config.CoreThreads = MgmtThreads;
     mgmt_config.MaxThreads = MgmtThreads;
     mgmt_config.KeepAliveTime = std::chrono::milliseconds(300000);
     mgmt_config.QueueCapacity = 1000;
     mgmt_config.EnableCPUAffinity = false;
     mgmt_config.EnableWorkStealing = false;
     mgmt_config.Type = SearchThreadPool::PoolType::MANAGEMENT;

     ManagementPool = std::make_unique<SearchThreadPool>(mgmt_config);

     /* Set CPU affinity for all pools */

     std::vector<int> http_cores, search_cores, write_cores;

     if (Instance)
     {
          if (Instance && Instance->Metrics)
          {
               http_cores = Instance->Metrics->Affinity.GetOptimalCoresForPool(SearchThreadPool::PoolType::HTTP_REQUEST);
               search_cores = Instance->Metrics->Affinity.GetOptimalCoresForPool(SearchThreadPool::PoolType::SEARCH);
               write_cores = Instance->Metrics->Affinity.GetOptimalCoresForPool(SearchThreadPool::PoolType::WRITE);
          }
     }

     HTTPPool->SetCPUAffinity(http_cores);
     SearchPool->SetCPUAffinity(search_cores);
     WritePool->SetCPUAffinity(write_cores);

     /* Log thread distribution BEFORE starting pools */

     size_t TotalPoolThreadsLog = HTTPThreads + SearchThreads + WriteThreads + MgmtThreads;

     size_t CurrentBeforePools = ThreadLimit::GetCurrentThreadCount();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("thread_pool", "Thread pool configuration: HTTP=" + std::to_string(HTTPThreads) + ", Search=" + std::to_string(SearchThreads) + ", Write=" + std::to_string(WriteThreads) + ", Management=" + std::to_string(MgmtThreads) + " = " + std::to_string(TotalPoolThreadsLog) + " pool threads.");
          Instance->Logs->Normal("thread_pool", "RocksDB background threads: " + std::to_string(RocksDBThreads) + ".");
          Instance->Logs->Normal("thread_pool", "Total threads: " + std::to_string(CurrentBeforePools) + " existing + " + std::to_string(TotalPoolThreadsLog) + " pools + " + std::to_string(RocksDBThreads) + " RocksDB = " + std::to_string(CurrentBeforePools + TotalPoolThreadsLog + RocksDBThreads) + " (max: " + std::to_string(MaxThreads) + ").");
     }

     /* Start all pools */

     HTTPPool->Start();
     SearchPool->Start();
     WritePool->Start();
     ManagementPool->Start();

     Initialized = true;

     return true;
}

void ThreadPoolManager::Shutdown()
{
     if (!Initialized)
     {
          return;
     }

     HTTPPool->Shutdown();
     SearchPool->Shutdown();
     WritePool->Shutdown();
     ManagementPool->Shutdown();

     Initialized = false;
}

SearchThreadPool &ThreadPoolManager::GetHTTPPool()
{
     return *HTTPPool;
}

SearchThreadPool &ThreadPoolManager::GetSearchPool()
{
     return *SearchPool;
}

SearchThreadPool &ThreadPoolManager::GetWritePool()
{
     return *WritePool;
}

SearchThreadPool &ThreadPoolManager::GetManagementPool()
{
     return *ManagementPool;
}

ThreadPoolManager::GlobalStats ThreadPoolManager::GetGlobalStats()
{
     GlobalStats Stats;

     if (!Initialized || !HTTPPool || !SearchPool || !WritePool || !ManagementPool)
     {
          Stats.TotalActiveThreads = 0;
          Stats.TotalCompletedTasks = 0;
          Stats.SystemCPUUsage = 0.0;
          return Stats;
     }

     Stats.HTTPPool = HTTPPool->GetStats();
     Stats.SearchPool = SearchPool->GetStats();
     Stats.WritePool = WritePool->GetStats();
     Stats.ManagementPool = ManagementPool->GetStats();

     Stats.TotalActiveThreads = Stats.HTTPPool.ActiveThreads + Stats.SearchPool.ActiveThreads + Stats.WritePool.ActiveThreads + Stats.ManagementPool.ActiveThreads;

     Stats.TotalCompletedTasks = Stats.HTTPPool.CompletedTasks + Stats.SearchPool.CompletedTasks + Stats.WritePool.CompletedTasks + Stats.ManagementPool.CompletedTasks;

     /* Calculate system CPU usage (simplified) */

     Stats.SystemCPUUsage = (Stats.HTTPPool.CPUUtilization + Stats.SearchPool.CPUUtilization + Stats.WritePool.CPUUtilization + Stats.ManagementPool.CPUUtilization) / 4.0;

     return Stats;
}

bool ThreadPoolManager::IsInitialized() const
{
     std::lock_guard<std::mutex> lock(InitMutex);
     return Initialized;
}

/* CPUAffinityManager implementation */

CPUAffinityManager::CPUTopology CPUAffinityManager::DetectTopology()
{
     std::lock_guard<std::mutex> lock(TopologyMutex);

     if (Topology.NumCores > 0)
     {
          return Topology; /* Already detected */
     }

     /* Detect number of cores */

     Topology.NumCores = std::thread::hardware_concurrency();

     /* Detect NUMA topology (simplified) */

     Topology.NumSockets = 1; /* Assume single socket for now */

     Topology.SocketCores.resize(1);

     for (int i = 0; i < static_cast<int>(Topology.NumCores); ++i)
     {
          Topology.SocketCores[0].push_back(i);
     }

     /* Detect NUMA nodes */

     Topology.NUMANodes.push_back(0); /* Single NUMA node for now */

     return Topology;
}

bool CPUAffinityManager::SetThreadAffinity(std::thread::id /* thread_id*/, int cpu_core)
{
#if defined(__linux__)
     cpu_set_t cpuset;

     CPU_ZERO(&cpuset);
     CPU_SET(cpu_core, &cpuset);

     pthread_t thread_handle = pthread_self();

     return pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset) == 0;
#else
     (void)cpu_core;
     return false;
#endif
}

bool CPUAffinityManager::SetProcessAffinity(int cpu_core)
{
#if defined(__linux__)
     cpu_set_t cpuset;

     CPU_ZERO(&cpuset);
     CPU_SET(cpu_core, &cpuset);

     return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
#else
     (void)cpu_core;
     return false;
#endif
}

std::vector<int> CPUAffinityManager::GetOptimalCoresForPool(SearchThreadPool::PoolType type)
{
     auto topology = DetectTopology();

     std::vector<int> cores;

     switch (type)
     {
          case SearchThreadPool::PoolType::HTTP_REQUEST:

               /* Use first half of cores for HTTP requests */

               for (size_t i = 0; i < topology.NumCores / 2; ++i)
               {
                    cores.push_back(i);
               }
               break;

          case SearchThreadPool::PoolType::SEARCH:

               /* Use second half of cores for search */

               for (size_t i = topology.NumCores / 2; i < topology.NumCores; ++i)
               {
                    cores.push_back(i);
               }
               break;

          case SearchThreadPool::PoolType::WRITE:

               /* Use specific cores for write operations */

               cores.push_back(0);

               if (topology.NumCores > 1)
               {
                    cores.push_back(1);
               }
               break;

          case SearchThreadPool::PoolType::MANAGEMENT:

               /* Use any available core for management */

               cores.push_back(0);
               break;
     }

     return cores;
}

void CPUAffinityManager::EnableNUMAOptimization()
{
     NUMAEnabled = true;
     
     /* Additional NUMA optimization logic would go here */
}
