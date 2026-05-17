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
#include <thread>
#include <unistd.h>
#include <vector>

#include "api/httpserver.h"
#include "api/ipfilter.h"
#include "api/searchapi.h"
#include "api/userauth.h"
#include "common/actionlist.h"
#include "common/health.h"
#include "common/searchpool.h"
#include "common/listenmanager.h"
#include "core/config.h"
#include "runtime/daemon.h"
#include "runtime/exitmanager.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "core/typedefs.h"
#include "search/storageengine.h"
#include "sam/sam.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/simdutils.h"
#include "utils/tools.h"

hlquery *Instance = nullptr;

/* Entry point for the daemon. */

int main(int argc, char** argv)
{
     new hlquery(argc, argv);
     Instance->Run();
     delete Instance;
     Instance = nullptr;

     return 0;
}

/* Constructor for the main hlquery class */

hlquery::hlquery(int argc, char** argv)
{
     ThreadLimit::SetThreadName("hlquery");

     Instance       =       this;
     Metrics        =       std::make_unique<HLQueryMetrics>();
     SQL            =       std::make_unique<SQLService>();
     Config         =       std::make_unique<ServerConfig>(argc, argv);

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
     API         =        nullptr;
     ThreadPools =        nullptr;
     Engine      =        nullptr;

     HTTPServers.clear();
}

void hlquery::AddBackgroundThread(std::thread &&ThreadVal)
{
     std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);
     BackgroundThreads.push_back(std::move(ThreadVal));
}

std::vector<std::thread> hlquery::TakeBackgroundThreads()
{
     std::vector<std::thread> ThreadsToJoin;
     {
          std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);
          ThreadsToJoin.swap(BackgroundThreads);
     }
     return ThreadsToJoin;
}

/* Performs server initialization and sets up all core subsystems */

bool hlquery::Initialize()
{
     if (Logs && !HTTPServers.empty())
     {
          /* Skip if already initialized to avoid double setup issues */

          return true;
     }

     WriteStartupBanner();

     /* Initialize the core server logic */

     if (!InitializeServer())
     {
          return false;
     }

     LLM = std::make_unique<llm>(*Config);

     if (Config && Config->GetSamEnabled())
     {
          Sam = std::make_unique<SAM>();

          if (Sam && !Sam->Initialize())
          {
               Sam.reset();
          }
     }
     else if (Logs)
     {
          Logs->Normal("sam", "SAM disabled in configuration.");
     }

     /* Verify that critical subsystems are initialized properly */

     if (HTTPServers.empty())
     {
          print_error("No HTTP/HTTPS servers initialized!");
          return false;
     }

     if (!Logs)
     {
          print_error("Logs is null after initialization!");
          return false;
     }

     if (Logs)
     {
          Logs->Debug("hlquery", "Initializing network listeners for custom protocols.");
     }

     /* Initialize network listeners for configured non-HTTP protocols. */

     Listeners = ListenManager::CreateCustomProtocolListeners(*Config);
     RunListeners();

     if (Logs)
     {
          Logs->Normal("hlquery", "HTTP servers initialized; readiness will follow startup loading state.");
     }

     return true;
}

void hlquery::WriteStartupBanner()
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
     if (Logs)
     {
          Logs->Debug("hlquery", "Socket engine verified/initialized for listeners.");
     }

     if (Logs)
     {
          Logs->Debug("hlquery", "Starting " + std::to_string(Listeners.size()) + " listeners.");
     }

     bool AnyListenerStartedValue = false;

     for (auto &ListenerVal : Listeners)
     {
          if (Logs)
          {
               Logs->Debug("hlquery", "Attempting to bind listener.");
          }

          if (ListenerVal->BindAndListen())
          {
               AnyListenerStartedValue = true;

               if (Logs)
               {
                    Logs->Debug("hlquery", "Listener bound successfully.");
               }
          }
          else
          {
               if (Logs)
               {
                    Logs->Debug("hlquery", "Listener skipped (port busy).");
               }
          }
     }

     /* Treat failure to start listeners as fatal when protocols are explicitly configured */

     if (!AnyListenerStartedValue && !Listeners.empty())
     {
          print_failed("Failed to start any listening socket.");
          ExitManager::Exit(1);
     }
}

/* Helper function to check if the main processing loop should terminate */

static inline bool ShouldExitLoop()
{
     return ForceExit != 0 || ShuttingDown != 0;
}

/* Helper function to perform periodic database flush with robust error handling */

static void SafePeriodicFlush()
{
     if (!Instance || !Instance->Database)
     {
          return;
     }

     /* Perform the periodic flush operation */

     try
     {
          Instance->Database->Flush();
     }
     catch (const std::exception &e)
     {
          if (Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Periodic flush failed: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          if (Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Periodic flush failed with unknown exception.");
          }
     }
}

/* Helper function to process background tasks and management operations */

static void ProcessPeriodicTasks()
{
     /* Execute lazy processing operations for performance optimization */

     try
     {
          DaemonHandler::ProcessLazyOperations();
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Lazy operations failed: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Lazy operations failed with unknown exception.");
          }
     }

     /* Advance the timer management subsystem */

     if (Instance && Instance->Timers)
     {
          try
          {
               Instance->Timers->Tick();
          }
          catch (const std::exception &e)
          {
               if (Instance->Logs)
               {
                    Instance->Logs->Debug("hlquery", "Timer tick failed: " + std::string(e.what()) + ".");
               }
          }
          catch (...)
          {
               if (Instance->Logs)
               {
                    Instance->Logs->Debug("hlquery", "Timer tick failed with unknown exception.");
               }
          }
     }
}

static bool PreflightSSLConfig(ServerConfig *ConfigPtr)
{
     if (!ConfigPtr)
     {
          return true;
     }

     const std::string &ConfigFileLoc = ConfigPtr->GetConfigFile();

     if (ConfigFileLoc.empty())
     {
          return true;
     }

     if (!ConfigPtr->IsValid())
     {
          if (!ConfigPtr->LoadConfig(ConfigFileLoc))
          {
               const std::string &ErrorMsg = ConfigPtr->GetError();

               if (!ErrorMsg.empty())
               {
                    print_error("{}", ErrorMsg);
               }
               else
               {
                    print_error("Failed to load configuration file: {}.", ConfigFileLoc);
               }

               return false;
          }
     }

     const auto &BindConfigs = ConfigPtr->GetBindConfigs();

     for (const auto &BindConfigVal : BindConfigs)
     {
          if (!BindConfigVal.ssl)
          {
               continue;
          }

          std::string ErrorMsg;

          if (!ValidateSSLConfig(BindConfigVal, &ErrorMsg))
          {
               print_error("SSL preflight failed for {}:{} ({}): {}", BindConfigVal.address, BindConfigVal.port, BindConfigVal.type, ErrorMsg);

               return false;
          }
     }

     return true;
}

/* Core execution method that manages the main application life cycle */

void hlquery::Run()
{
     /* Handle daemonization process if configured for background operation */

     if (!PreflightSSLConfig(Config.get()))
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

     if (!hlquery::WritePID())
     {
          print_error("Failed to acquire PID file lock.");
          print_error("Another daemon instance may already be starting, or the PID path is not writable.");

          ExitManager::Exit(1);
     }

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
          if (ShouldExitLoop())
          {
               break;
          }

          time_t NowTimeVal = 0;
          time_t CurrentMinute = -1;

          try
          {
               NowTimeVal = Time();

               if (NowTimeVal > 0)
               {
                    CurrentMinute = NowTimeVal / 60;
               }
          }
          catch (...)
          {
               /* Fallback to 0 if time retrieval fails */
          }

          /* Execute periodic maintenance tasks based on clock movement */

          if (NowTimeVal != old_time)
          {
               SafePeriodicFlush();
               old_time = NowTimeVal;
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
               ShuttingDown = 1;

               break;
          }

          /* Dispatch network events via the socket engine */

          SocketEngine::DispatchEvents();

          if (ShouldExitLoop())
          {
               break;
          }

          /* Perform trial writes for pending network data */

          SocketEngine::DispatchTrialWrites();

          if (ShouldExitLoop())
          {
               break;
          }

          /* Execute queued background actions */

          ActionList::ProcessActions();

          if (ShouldExitLoop())
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

          ProcessPeriodicTasks();

          if (Instance && Instance->Modules)
          {
               FOREACH_MOD(OnIdleTick, NowTimeVal);
          }

          if (ShouldExitLoop())
          {
               break;
          }
     }

     /* Exit via ExitManager so registered cleanup runs on normal shutdown. */

     ExitManager::Exit(0);
}
