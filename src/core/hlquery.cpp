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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "api/httpserver.h"
#include "api/ipfilter.h"
#include "api/searchapi.h"
#include "api/userauth.h"
#include "common/actionlist.h"
#include "common/health.h"
#include "common/listenmanager.h"
#include "common/searchpool.h"
#include "runtime/daemon.h"
#include "runtime/exitmanager.h"
#include "runtime/threadlimit.h"
#include "runtime/timers.h"
#include "core/config.h"
#include "core/helpers.h"
#include "core/hlquery.h"
#include "core/metrics.h"
#include "core/modulemanager.h"
#include "core/modules.h"
#include "core/socketengine.h"
#include "sql/sql.h"
#include "search/document_collection_store.h"
#include "search/lexical_inverted_index.h"
#include "search/rocksdb_storage_engine.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/simdutils.h"
#include "utils/tools.h"

hlquery *Instance = nullptr;

/* Entry point for the daemon. */

int main(int argc, char **argv)
{
     new hlquery(argc, argv);
     Instance->Run();
     delete Instance;
     Instance = nullptr;

     return 0;
}

/* Constructor for the main hlquery class */

hlquery::hlquery(int argc, char **argv)
{
     ThreadLimit::SetThreadName("hlquery");

     Instance     =    this;
     Metrics      =    std::make_unique<HLQueryMetrics>();
     SQL          =    std::make_unique<SQLService>();
     Config       =    std::make_unique<ServerConfig>(argc, argv);

     ParseArgs();
     StatsVal.Start();

     /* Register cleanup wrapper with ExitManager to ensure resources are released */

     ExitManager::RegisterCleanup([]()
     {
             if (Instance)
             {
                 Instance->Cleanup();
             }
     });
}

/* Destructor for the hlquery class */

hlquery::~hlquery()
{
     API = nullptr;
     ThreadPools = nullptr;
     Engine = nullptr;

     HTTPServers.clear();
}

/* Performs server initialization and sets up all core subsystems */

bool hlquery::Initialize()
{
     if (Logs && !HTTPServers.empty())
     {
          /* Skip if already initialized to avoid double setup issues */

          return true;
     }

     StartupBanner();

     /* Initialize the core server logic */

     if (!StartServer())
     {
          return false;
     }

     if (!InitializeOptionalServices())
     {
          return false;
     }

     if (!ValidateInitializedSubsystems())
     {
          return false;
     }

     InitializeNetworkListeners();
     Logs->Normal("hlquery", "HTTP servers initialized; readiness will follow startup loading state.");

     return true;
}

bool hlquery::InitializeOptionalServices()
{
     if (!HasConfig())
     {
          print_error("Config is null during optional service initialization!");
          return false;
     }

     return true;
}

bool hlquery::ValidateInitializedSubsystems() const
{
     if (HTTPServers.empty())
     {
          print_error("No HTTP/HTTPS servers initialized!");
          return false;
     }

     if (!HasLogs())
     {
          print_error("Logs is null after initialization!");
          return false;
     }

     return true;
}

void hlquery::InitializeNetworkListeners()
{
     if (HasLogs())
     {
          Logs->Debug("hlquery", "Initializing network listeners for custom protocols.");
     }

     Listeners = ListenManager::CreateCustomProtocolListeners();
     RunListeners();
}

void hlquery::StartupBanner()
{
     newline();
     std::vector<std::string> loaded_modules;

     if (Modules)
     {
          loaded_modules = Modules->GetLoadedModuleNames();
     }

     ConsoleWriter::WriteStartup(Tools::FormatStartupMessage(loaded_modules), true, false);
}

/* Start all network listeners to begin accepting client connections */

void hlquery::RunListeners()
{
     ConfiguredListenerCount = Listeners.size();
     StartedListenerCount = 0;
     SkippedListenerCount = 0;
     LastListenerError.clear();

     if (Logs)
     {
          Logs->Debug("hlquery", "Socket engine verified/initialized for listeners.");
          Logs->Debug("hlquery", "Starting " + std::to_string(ConfiguredListenerCount) + " listeners.");
     }

     bool AnyListenerStartedValue = false;

     for (auto &Host : Listeners)
     {
          if (Logs)
          {
               Logs->Debug("hlquery", "Attempting to bind listener.");
          }

          if (Host->BindAndListen())
          {
               AnyListenerStartedValue = true;
               StartedListenerCount++;

               if (Logs)
               {
                    Logs->Debug("hlquery", "Listener bound successfully.");
               }
          }
          else
          {
               SkippedListenerCount++;
               LastListenerError = "A configured listener failed to bind or was skipped.";

               if (Logs)
               {
                    Logs->Debug("hlquery", "Listener skipped after bind/listen failure.");
               }
          }
     }

     /* Treat failure to start listeners as fatal when protocols are explicitly configured */

     if (!AnyListenerStartedValue && !Listeners.empty())
     {
          LastListenerError = "Failed to start any listening socket.";
          print_failed("Failed to start any listening socket.");
          ExitManager::Exit(1);
     }
}

/* Core execution method that manages the main application life cycle */

void hlquery::Run()
{
     /* Handle daemonization process if configured for background operation */

     if (!CoreHelpers::PreflightSSLConfig())
     {
          ExitManager::Exit(1);
     }

     if (Config && !Config->GetNoForkMode() && !Config->GetTestMode())
     {
          if (!Daemonize())
          {
               print_error("Daemonization failed.");
               ExitManager::Exit(1);
          }
     }

     /* Ensure all critical systems are initialized before entering main loop */

     if (!Logs || HTTPServers.empty())
     {
          if (!Initialize())
          {
               print_error("Initialization failed during Run().");
               ExitManager::Exit(1);
          }

          if (!Logs || HTTPServers.empty())
          {
               print_error("Initialization failed - Logs or HTTPServers is null/empty after Initialize().");
               print_error("Logs={}, HTTPServers count={}", (Logs ? "valid" : "null"), HTTPServers.size());
               print_error("Exiting - cannot continue without critical systems.");

               ExitManager::Exit(1);
          }
     }

     /* Register signal handlers for graceful shutdown management */

     if (Config && !Config->GetTestMode())
     {
          SetupSignalHandlers();
     }

     time_t old_time = Time();
     time_t LastMinuteRun = (old_time > 0) ? (old_time / 60) : -1;

     if (!Logs)
     {
          print_error("Logs unique_ptr is null in Run() - LogManager failed to initialize.");
          print_error("Exiting - cannot continue without logging system.");

          ExitManager::Exit(1);
     }

     FOREACH_MOD(OnStartup);

     /* Enter the primary server processing loop */

     while (true)
     {
          if (CoreHelpers::ShouldExitLoop())
          {
               break;
          }

          time_t CurrentTime = 0;
          time_t CurrentMinute = -1;

          try
          {
               CurrentTime = Time();

               if (CurrentTime > 0)
               {
                    CurrentMinute = CurrentTime / 60;
               }
          }
          catch (...)
          {
               /* Fallback to 0 if time retrieval fails */
          }

          /* Execute periodic maintenance tasks based on clock movement */

          if (CurrentTime != old_time)
          {
               CoreHelpers::SafePeriodicFlush();
               old_time = CurrentTime;
          }

          if (Instance && Instance->Modules && CurrentMinute >= 0 && CurrentMinute != LastMinuteRun)
          {
               LastMinuteRun = CurrentMinute;
               FOREACH_MOD(OnEveryOneMinute);
          }

          /* Handle signals received during loop execution */

          hlquery::ProcessDeferredSignals();

          /* Manage emergency exit scenarios if requested */

          if (ShouldForceExit())
          {
               print_warning("Force exit requested, initiating graceful shutdown.");
               std::cout.flush();
               SetShutdownFlag();

               break;
          }

          /* Dispatch network events via the socket engine */

          DaemonHandler::ProcessSocketEngineOptimization();
          SocketEngine::DispatchEvents();

          if (CoreHelpers::ShouldExitLoop())
          {
               break;
          }

          /* Perform trial writes for pending network data */

          SocketEngine::DispatchTrialWrites();

          if (CoreHelpers::ShouldExitLoop())
          {
               break;
          }

          /* Execute queued background actions */

          ActionList::ProcessActions();

          if (CoreHelpers::ShouldExitLoop())
          {
               if (Logs)
               {
                    Logs->Normal("main", "Exit loop requested - exiting main loop.");
               }

               break;
          }

          /* Check if a graceful shutdown has been requested */

          if (ShouldShutdown())
          {
               if (Logs)
               {
                    Logs->Normal("main", "Shutdown requested - exiting main loop.");
               }

               break;
          }

          /* Process other recurring server tasks */

          CoreHelpers::ProcessPeriodicTasks();
          FOREACH_MOD(OnIdleTick, CurrentTime);

          if (CoreHelpers::ShouldExitLoop())
          {
               break;
          }
     }

     /* Exit via ExitManager so registered cleanup runs on normal shutdown. */

     ExitManager::Exit(0);
}
