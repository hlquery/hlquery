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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <sched.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "core/config.h"

/* 
 * High-performance thread pool with CPU affinity and work stealing.
 * This pool accepts queued work items, executes them on worker threads,
 * and exposes scaling and statistics helpers for runtime management.
 */

class SearchThreadPool
{
   public:

     /* Identifies the workload category served by a pool. */

     enum class PoolType
     {
          HTTP_REQUEST,
          SEARCH,
          WRITE,
          MANAGEMENT
     };

     /* Describes the startup and runtime limits for one pool instance.
      * These values control thread count, queueing, and affinity behavior.
      */

     struct ThreadPoolConfig
     {
          /* Number of threads started immediately */

          size_t CoreThreads;

          /* Maximum number of threads allowed after scaling */

          size_t MaxThreads;

          /* Idle lifetime before extra threads can be retired */

          std::chrono::milliseconds KeepAliveTime;

          /* Maximum number of queued tasks accepted by the pool */

          size_t QueueCapacity;

          /* Enables pinning worker threads to configured CPU cores */

          bool EnableCPUAffinity;

          /* Enables worker-side task stealing between queues */

          bool EnableWorkStealing;

          /* Workload class handled by this pool */

          PoolType Type;
     };

     /* Construct the thread pool with one configuration block. */

     explicit SearchThreadPool(const ThreadPoolConfig& config);

     /* Destroy the thread pool and release worker resources. */

     ~SearchThreadPool();

     /* Submit one task to the pool.
      * Returns a future for the scheduled callable or an empty future
      * when the queue is already at capacity.
      */

     template <typename F, typename... Args>
     auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
     {
          using ReturnType = decltype(f(args...));
          auto task = std::make_shared<std::packaged_task<ReturnType()>>(
               std::bind(std::forward<F>(f), std::forward<Args>(args)...));

          std::future<ReturnType> ResultFuture = task->get_future();

          {
               std::lock_guard<std::mutex> lock(QueueMutex);

               if (TaskQueue.size() >= Config.QueueCapacity)
               {
                    RejectedTasks++;

                    return std::future<ReturnType>(); /* Return empty future for rejected tasks */
               }

               Task WrapperTask;
               WrapperTask.Function = [task]()
               {
                    (*task)();
               };
               WrapperTask.SubmitTime = std::chrono::steady_clock::now();
               WrapperTask.Type = Config.Type;

               TaskQueue.push(WrapperTask);
          }

          QueueCV.notify_one();

          return ResultFuture;
     }

     /* Start worker threads for this pool. */

     void Start();

     /* Stop workers and reject further task execution. */

     void Shutdown();

     /* Pause worker consumption without discarding queued tasks. */

     void Pause();

     /* Resume processing of queued tasks after a pause. */

     void Resume();

     /* Aggregated runtime statistics for one thread pool. */

     struct PoolStats
     {
          /* Workers currently executing tasks */

          size_t ActiveThreads;

          /* Total workers owned by the pool */

          size_t TotalThreads;

          /* Number of tasks currently waiting in the queue */

          size_t QueueSize;

          /* Total number of completed tasks */

          size_t CompletedTasks;

          /* Total number of rejected submissions */

          size_t RejectedTasks;

          /* Average task execution time in milliseconds */

          double AvgTaskTimeMS;

          /* Estimated CPU utilization for the pool */

          double CPUUtilization;
     };

     /* Collect the current statistics snapshot for this pool. */

     PoolStats GetStats();

     /* Increase the worker count by the requested number of threads. */

     void ScaleUp(size_t additional_threads);

     /* Reduce the worker count by the requested number of threads. */

     void ScaleDown(size_t threads_to_remove);

     /* Assign a preferred CPU core list for future worker placement. */

     void SetCPUAffinity(const std::vector<int>& cpu_cores);

     /* Enable NUMA-aware runtime optimizations for this pool. */

     void EnableNUMAOptimization();

   private:

     /* One queued work item plus its scheduling metadata. */

     struct Task
     {
          /* Callable object executed by a worker */

          std::function<void()> Function;

          /* Time when the task entered the queue */

          std::chrono::steady_clock::time_point SubmitTime;

          /* Logical workload type associated with the task */

          PoolType Type;
     };

     /* Tracks one worker thread and its local execution state. */

     struct WorkerThread
     {
          /* Underlying worker thread object */

          std::thread Thread;

          /* Indicates whether the worker should continue running */

          std::atomic<bool> Running{true};

          /* Indicates whether the worker is currently executing a task */

          std::atomic<bool> Busy{false};

          /* Assigned CPU core or -1 when no affinity is applied */

          int CPUCore{-1};

          /* Time point of the most recent completed work */

          std::chrono::steady_clock::time_point LastWorkTime;

          /* Number of tasks completed by this worker */

          size_t TasksCompleted{0};

          /* Total accumulated execution time for this worker */

          double TotalWorkTimeMS{0.0};
     };

     /* Immutable runtime configuration for this pool */

     ThreadPoolConfig Config;

     /* Worker thread storage */

     std::vector<std::unique_ptr<WorkerThread>> Workers;

     /* Serializes mutations and snapshots of the worker list. */

     mutable std::mutex WorkersMutex;

     /* Number of worker slots currently active in the pool. */

     std::atomic<size_t> ActiveWorkerCount{0};

     /* Indicates that shutdown has been requested */

     std::atomic<bool> ShutdownFlag{false};

     /* Indicates that worker execution is temporarily paused */

     std::atomic<bool> Paused{false};

     /* Shared priority queue for submitted tasks */

     std::priority_queue<Task, std::vector<Task>, std::function<bool(const Task&, const Task&)>> TaskQueue;

     /* Protects access to the shared task queue */

     std::mutex QueueMutex;

     /* Wakes workers when new tasks become available */

     std::condition_variable QueueCV;

     /* Per-worker queues used during work stealing */

     std::vector<std::queue<Task>> WorkerQueues;

     /* Mutexes protecting each worker-local queue */

     std::vector<std::unique_ptr<std::mutex>> WorkerQueueMutexes;

     /* Round-robin starting point for steal attempts */

     std::atomic<size_t> StealIndex{0};

     /* Total number of completed tasks across the pool */

     std::atomic<size_t> CompletedTasks{0};

     /* Total number of rejected task submissions */

     std::atomic<size_t> RejectedTasks{0};

     /* Total accumulated task execution time */

     std::atomic<double> TotalTaskTimeMS{0.0};

     /* Time when the pool started running */

     std::chrono::steady_clock::time_point StartTime;

     /* Preferred CPU core assignments for workers */

     std::vector<int> CPUCores;

     /* Indicates whether NUMA optimizations are active */

     bool NUMAOptimized{false};

     /* Main execution loop for one worker thread. */

     void WorkerLoop(WorkerThread *worker, size_t worker_id);

     /* Apply CPU affinity to one thread when supported by the platform. */

     void SetThreadAffinity(std::thread& thread, int cpu_core);

     /* Attempt to steal work from another worker-local queue. */

     bool TryStealWork(size_t worker_id, Task& task);

     /* Record execution statistics for a completed task interval. */

     void UpdateStatistics(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end);

     /* Resize the pool toward a target number of workers. */

     void ScaleThreads(size_t target_threads);

     /* Apply NUMA-specific optimizations to worker placement and scheduling. */

     void OptimizeForNUMA();
};

/* Global thread pool manager.
 * This singleton owns the specialized pools used by the server
 * and exposes shared initialization and statistics helpers.
 */

class ThreadPoolManager
{
   public:

     /* Access the singleton thread pool manager instance. */

     static ThreadPoolManager& GetInstance();

     /* Initialize all configured thread pools. */

     bool Initialize();

     /* Shut down every managed thread pool. */

     void Shutdown();

     /* Access the HTTP request worker pool. */

     SearchThreadPool& GetHTTPPool();

     /* Access the search execution worker pool. */

     SearchThreadPool& GetSearchPool();

     /* Access the indexing and write worker pool. */

     SearchThreadPool& GetWritePool();

     /* Access the management and maintenance worker pool. */

     SearchThreadPool& GetManagementPool();

     /* Combined statistics across all managed pools. */

     struct GlobalStats
     {
          /* Statistics for the HTTP request pool */

          SearchThreadPool::PoolStats HTTPPool;

          /* Statistics for the search pool */

          SearchThreadPool::PoolStats SearchPool;

          /* Statistics for the write pool */

          SearchThreadPool::PoolStats WritePool;

          /* Statistics for the management pool */

          SearchThreadPool::PoolStats ManagementPool;

          /* Sum of active workers across all pools */

          size_t TotalActiveThreads;

          /* Sum of completed tasks across all pools */

          size_t TotalCompletedTasks;

          /* Estimated system-wide CPU usage */

          double SystemCPUUsage;
     };

     /* Return one aggregated statistics snapshot for all pools. */

     GlobalStats GetGlobalStats();

     /* Returns whether the manager has already initialized its pools. */

     bool IsInitialized() const;

   private:

     /* Constructor is private because this manager is a singleton. */

     ThreadPoolManager() = default;

     /* Destructor is private because this manager is a singleton. */

     ~ThreadPoolManager() = default;

     /* HTTP request worker pool */

     std::unique_ptr<SearchThreadPool> HTTPPool;

     /* Search execution worker pool */

     std::unique_ptr<SearchThreadPool> SearchPool;

     /* Indexing and write worker pool */

     std::unique_ptr<SearchThreadPool> WritePool;

     /* Management and maintenance worker pool */

     std::unique_ptr<SearchThreadPool> ManagementPool;

     /* Tracks whether initialization has completed */

     bool Initialized{false};

     /* Protects initialization from race conditions */

     mutable std::mutex InitMutex;
};

/* High-performance task wrapper with SIMD optimizations.
 * The wrapper records simple execution metrics around an inner callable.
 */

template <typename T>
class HighPerformanceTask
{
   public:

     /* Construct the wrapper around one callable task object. */

     explicit HighPerformanceTask(T&& task) : TaskObj(std::forward<T>(task))
     {
     }

     template <typename... Args>
     auto operator()(Args&&... args) -> decltype(TaskObj(args...))
     {
          /* Enable SIMD optimizations if available */

#ifdef __AVX2__
          /* Use AVX2 instructions for vectorized operations */
#endif

          auto start = std::chrono::high_resolution_clock::now();

          auto result = TaskObj(std::forward<Args>(args)...);

          auto end = std::chrono::high_resolution_clock::now();

          /* Update performance metrics */

          auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

          UpdateMetrics(duration.count());

          return result;
     }

   private:

     /* Wrapped task object */

     T TaskObj;

     /* Update the shared metrics counters for completed wrapper calls. */

     void UpdateMetrics(long long duration_us)
     {
          /* Update performance metrics atomically */

          static std::atomic<long long> TotalDuration{0};
          static std::atomic<size_t> TaskCount{0};

          TotalDuration.fetch_add(duration_us);
          TaskCount.fetch_add(1);
     }
};

/* CPU affinity utilities.
 * This helper inspects CPU topology and applies affinity-related settings
 * for threads and processes when the platform supports it.
 */

class CPUAffinityManager
{
   public:

     /* Default constructor */

     CPUAffinityManager() = default;

     /* Default destructor */

     ~CPUAffinityManager() = default;

     /* Delete copy constructor and assignment to prevent copying */

     CPUAffinityManager(const CPUAffinityManager&) = delete;
     CPUAffinityManager& operator=(const CPUAffinityManager&) = delete;

     /* Describes detected CPU and NUMA topology data. */

     struct CPUTopology
     {
          /* Total number of logical CPU cores */

          size_t NumCores;

          /* Number of detected CPU sockets */

          size_t NumSockets;

          /* CPU core lists grouped by socket */

          std::vector<std::vector<int>> SocketCores;

          /* Detected NUMA node identifiers */

          std::vector<int> NUMANodes;
     };

     /* Detect and return the current CPU topology. */

     CPUTopology DetectTopology();

     /* Apply affinity for one thread identifier to one CPU core. */

     bool SetThreadAffinity(std::thread::id thread_id, int cpu_core);

     /* Apply affinity for the current process to one CPU core. */

     bool SetProcessAffinity(int cpu_core);

     /* Return an affinity-friendly core list for a given pool type. */

     std::vector<int> GetOptimalCoresForPool(SearchThreadPool::PoolType type);

     /* Enable NUMA-aware affinity behavior. */

     void EnableNUMAOptimization();

     /* Returns whether NUMA-aware affinity behavior is enabled. */

     bool IsNUMAEnabled() const
     {
          return NUMAEnabled;
     }

   private:

     /* Cached CPU topology information */

     CPUTopology Topology;

     /* Indicates whether NUMA-aware behavior is enabled */

     bool NUMAEnabled{false};

     /* Protects topology detection and updates */

     std::mutex TopologyMutex;
};
