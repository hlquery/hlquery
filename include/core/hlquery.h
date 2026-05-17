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
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/searchpool.h"
#include "common/listenmanager.h"
#include "core/config.h"
#include "core/metrics.h"
#include "core/logmanager.h"
#include "core/llm.h"
#include "core/modulemanager.h"
#include "runtime/startup.h"
#include "core/stats.h"
#include "runtime/clock.h"
#include "runtime/threadlimit.h"
#include "runtime/timers.h"
#include "core/forwards.h"
#include "runtime/serverconfig.h"
#include "sql/sql.h"
#include "utils/tools.h"

/* Global hlquery engine instance exported. */

CoreExport extern hlquery* Instance;

/* 
 * hlquery's main class.
 * This object coordinates process startup, runtime services,
 * shutdown handling, and access to the major server subsystems.
 */

class CoreExport hlquery
{
   private:

     /* Internal initialization function */

     bool InitializeServer();

     /* Check if another hlquery process is already running (internal) */

     bool CheckExistingProcessInternal();

     /* Initialize core server subsystems */

     bool InitializeCoreSystems();

     /* Display server binding information */

     void DisplayBindingInfo();

     /* Display SSL information if configured */

     void DisplaySSLInfo();

     /* Print startup banner and module list */

     void WriteStartupBanner();

     /* Initialize server in no-fork mode */

     bool InitializeNoForkMode();

     /* Setup post-fork operations */

     void SetupPostFork();

     /* Wait for metadata scan to complete before accepting requests */

     void WaitForMetadataScan();

     /* Persist collections load summary to disk at shutdown */

     void SaveCollectionsLoadSummary();

     /* Sync lock mechanism */

     std::atomic<bool> SyncInProgress{false};

     /* Mutex guarding sync state changes */

     std::mutex SyncMutex;

     /* Shutdown in progress flag */

     std::atomic<bool> ShutdownInProgress{false};

     /* Background threads spawned during initialization that must be joined at shutdown. */

     std::vector<std::thread> BackgroundThreads;

     /* Mutex guarding BackgroundThreads. */

     std::mutex BackgroundThreadsMutex;

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

     /* Register one background thread to be joined during shutdown. */

     void AddBackgroundThread(std::thread &&ThreadVal);

     /* Take ownership of all background threads for joining outside the lock. */

     std::vector<std::thread> TakeBackgroundThreads();

     /* Increase the core dump size limit for debugging crashes */

     void IncreaseCoreDumpSize();

     /* Exit the server with specified status code */

     void Exit(int status);

     /* Returns the current time using system clock */

     time_t Time() const
     {
          return ::Time();
     }

     /* Returns milliseconds since epoch */

     long long NowMs() const
     {
          return ::NowMs();
     }

     /* Returns current time point from steady clock */

     std::chrono::steady_clock::time_point Now() const
     {
          return ::Now();
     }

     /* Check if another hlquery process is already running */

     static bool CheckExistingProcess();

     /* Write the process ID to the configured PID file */

     static bool WritePID();

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

     /* Runtime-loaded modules */

     std::unique_ptr<ModuleManager> Modules;

     /* Database instance */

     std::unique_ptr<DBManager> Database;

     /* Non-owning convenience alias for the active RocksDB engine */

     DBManager *Engine = nullptr;

     /* Inverted Index for search */

     std::unique_ptr<InvertedIndex> SearchIndex;

     /* User Authentication Manager */

     std::unique_ptr<UserAuthManager> Users;

     /* IP Filter Manager */

     std::unique_ptr<class IPFilter> IPFilter;

     /* Non-owning convenience alias for the singleton thread pool manager */

     ThreadPoolManager *ThreadPools = nullptr;

     /* HTTP Servers */

     std::vector<HttpServer*> HTTPServers;

     /* Non-owning convenience alias for the singleton SearchAPI instance */

     SearchAPI *API = nullptr;

     /* Metrics and affinity state */

     std::unique_ptr<HLQueryMetrics> Metrics;

     /* SQL translation service */

     std::unique_ptr<SQLService> SQL;

     /* Server configuration object */

     std::unique_ptr<ServerConfig> Config;

     /* Resolved local LLM runtime configuration */

     std::unique_ptr<llm> LLM;

     /* Secondary Assistant Manager */

     std::unique_ptr<SAM> Sam;

     /* Server statistics */

     ServerStats StatsVal;

     /* Sync lock mechanism */

     bool IsSyncInProgress() const
     {
          return SyncInProgress.load(std::memory_order_acquire);
     }

     /* Update the sync-in-progress state */

     void SetSyncInProgress(bool in_progress)
     {
          SyncInProgress.store(in_progress, std::memory_order_release);
     }

     /* Get the mutex used to serialize sync operations */

     std::mutex& GetSyncMutex()
     {
          return SyncMutex;
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
};

int main(int argc, char** argv);

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
