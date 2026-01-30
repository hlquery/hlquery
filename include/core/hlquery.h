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

#include <chrono>
#include <csignal>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/listenmanager.h"
#include "core/config.h"
#include "core/logmanager.h"
#include "core/startup.h"
#include "core/thread_limit.h"
#include "core/timers.h"
#include "core/typedefs.h"
#include "core/metrics_storage.h"
#include "core/stats.h"
#include "rocksdb/database_wrapper.h"
#include "rocksdb/inverted_index.h"
#include "serverconfig.h"
#include "utils/tools.h"
#include "common/hlquery_search_thread_pool.h"

extern std::unique_ptr<hlquery> Instance;

class hlquery 
{
  public:
 
    /* Constructor */
     
    hlquery(int argc, char** argv);

    /* Destructor */
     
    ~hlquery();

    /* Initialize all server subsystems */
     
    bool Initialize();

    /* Main server event loop */
     
    void Run();

    /* Start all configured network listeners */
     
    void RunListeners();

    /* Daemonize the process by forking to background */
     
    bool Daemonize();

    /* Configure file descriptors for operation */
     
    void SetupFileDescriptors();

    /* Complete the daemon setup process after forking */

    void CompleteDaemonSetup();

    /* Perform graceful cleanup of all server resources */
     
    void Cleanup();

    /* Increase the core dump size limit for debugging crashes */
     
    void IncreaseCoreDumpSize();

    /* Exit the server with specified status code */
     
    void Exit(int status);
    
    /* Returns the timestamp when the server was started */
     
    time_t GetStartupTime() const 
    {
        try 
        {
            return StatsVal.GetStartupTime(); 
        } 
        catch (...) 
        {
            return 0;
        }
    }

    /* Returns the current time using system clock */
     
    time_t Time() const 
    {
        try 
        {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
            return static_cast<time_t>(seconds.count());
        } 
        catch (...) 
        {
            return 0;
        }
    }

    /* Returns milliseconds since epoch */

    long long NowMs() const 
    {
        try 
        {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
            return static_cast<long long>(milliseconds.count());
        } 
        catch (...) 
        {
            return 0;
        }
    }

    /* Returns current time point from steady clock */
     
    std::chrono::steady_clock::time_point Now() const 
    {
        try 
        {
            return std::chrono::steady_clock::now();
        } 
        catch (...) 
        {
            return std::chrono::steady_clock::time_point{};
        }
    }

    /* Check if another hlquery process is already running */
     
    static bool CheckExistingProcess();

    /* Write the process ID to the configured PID file */
     
    static void WritePID();

    /* Force stop running daemon and remove PID file */
     
    static void ForceStop();

    /* Handle a received signal and update internal state */
     
    static void SetSignal(int signal);

    /* Install signal handlers */
     
    static void SetupSignalHandlers();

    /* Check if the server should begin graceful shutdown */
     
    static bool ShouldShutdown();

    /* Check if the server should exit immediately without cleanup */
     
    static bool ShouldForceExit();

    /* Reset all signal counters to initial state */
     
    static void ResetSignalCounters();

    /* Set the shutdown flag to trigger graceful shutdown */
     
    static void SetShutdownFlag();

    /* Process deferred signal handling from main loop */
     
    static void ProcessDeferredSignals();

    /* Parse command line arguments */
     
    void ParseArgs();

    /* Network listener managers */
    
    std::vector<std::unique_ptr<ListenManager>> Listeners;

    /* Centralized logging system */
    
    std::unique_ptr<LogManager> Logs;

    /* Timer manager */
    
    std::unique_ptr<TimerManager> Timers;

    /* Database instance */
    
    std::unique_ptr<HLManager::AdvancedRocksDBEngine> Database;

    /* Inverted Index for search */
    
    std::unique_ptr<hlquery_storage::InvertedIndex> InvertedIndex;

    /* User Authentication Manager */
    
    std::unique_ptr<UserAuthManager> Users;

    /* IP Filter Manager */
    
    std::unique_ptr<class IPFilter> IPFilter;

    /* HTTP Servers */
    
    std::vector<HttpServer*> HTTPServers;

    /* Metrics Storage */
    
    std::unique_ptr<MetricsStorage::SmartMetricsStorage> CPUMetrics;

    std::unique_ptr<MetricsStorage::SmartMetricsStorage> MemoryMetrics;

    /* Server configuration object */
    
    std::unique_ptr<ServerConfig> Config;
    
    /* Server statistics */
    
    ServerStats StatsVal;

    /* CPU Affinity Manager */
    
    std::unique_ptr<hlquery_threadpool::CPUAffinityManager> Affinity;

    /* Sync lock mechanism */
    
    bool IsSyncInProgress() const 
    { 
        return SyncInProgress.load(std::memory_order_acquire); 
    }

    void SetSyncInProgress(bool in_progress) 
    { 
        SyncInProgress.store(in_progress, std::memory_order_release); 
    }

    std::mutex& GetSyncMutex() 
    { 
        return SyncMutex; 
    }

    /* HTTP port for server */
    
    int HttpPort = 0;

    /* Get startup state */
    
    StartupState GetStartupState() const 
    {
        std::lock_guard<std::mutex> lock(StartupStateMutex);
        return StartupStateInfo;
    }
    
    /* Update startup state */
    
    void SetStartupState(const StartupState& state)
    {
        std::lock_guard<std::mutex> lock(StartupStateMutex);
        StartupStateInfo = state;
    }
    
    /* Update startup state collections info */
    
    void UpdateStartupStateCollections(const std::vector<std::string>& failed_collections, 
                                       size_t loaded_count, size_t expected_count) 
    {
        std::lock_guard<std::mutex> lock(StartupStateMutex);
        StartupStateInfo.FailedCollections = failed_collections;
        StartupStateInfo.CollectionsLoadedCount = loaded_count;
        StartupStateInfo.CollectionsExpectedCount = expected_count;
    }
    
    /* Check if shutdown is in progress */
    
    bool IsShuttingDown() const 
    {
        return ShutdownInProgress.load(std::memory_order_acquire) || ShouldShutdown();
    }
    
    /* Set shutdown in progress flag */
    
    void SetShutdownInProgress(bool in_progress) 
    {
        ShutdownInProgress.store(in_progress, std::memory_order_release);
    }

 private:
    
    /* Internal initialization function */
     
    bool InitializeServer();

    bool CheckExistingProcessInternal();

    bool InitializeCoreSystems();

    /* Display server binding information */

    void DisplayBindingInfo();

    /* Display SSL information if configured */

    void DisplaySSLInfo();
    
    /* Initialize server in no-fork mode */

    bool InitializeNoForkMode();
    
    /* Setup post-fork operations */

    void SetupPostFork();
    
    /* Wait for metadata scan to complete before accepting requests */

    void WaitForMetadataScan();
    
    /* Persist collections load summary to disk at shutdown */

    void SaveCollectionsLoadSummary();

    /* Format standardized startup message with local timestamp */

    std::string FormatStartupMessage() const;

 private:

    StartupState StartupStateInfo;

    mutable std::mutex StartupStateMutex;

    /* Sync lock mechanism */
    
    std::atomic<bool> SyncInProgress{false};

    std::mutex SyncMutex;
    
    /* Shutdown in progress flag */
    
    std::atomic<bool> ShutdownInProgress{false};
};

/* Flag indicating the server should begin graceful shutdown */

extern volatile sig_atomic_t ShuttingDown;

/* Count of SIGINT signals received */

extern volatile sig_atomic_t SigintCount;

/* Flag indicating the server should exit immediately without cleanup */

extern volatile sig_atomic_t ForceExit;

/* Flag indicating signal handler is currently executing */

extern volatile sig_atomic_t InSignalHandler;

/* Pending shutdown signal to be processed in main loop */

extern volatile sig_atomic_t PendingShutdownSignal;
