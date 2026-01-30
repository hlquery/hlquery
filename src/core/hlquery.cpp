
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

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "vendor/json/json.hpp"
#include "api/http_server.h"
#include "api/ip_filter.h"
#include "api/search_api.h"
#include "api/user_auth.h"
#include "common/action_list.h"
#include "common/hlquery_search_thread_pool.h"
#include "common/io_optimization.h"
#include "common/listenmanager.h"
#include "core/config.h"
#include "core/daemon_handler.h"
#include "core/exitmanager.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "core/thread_limit.h"
#include "rocksdb/hybrid_storage.h"
#include "rocksdb/inverted_index.h"
#include "rocksdb/database_wrapper.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/simd_utils.h"

std::unique_ptr<hlquery> Instance = nullptr;

/* Constructor for the main hlquery class */

hlquery::hlquery(int ArgcCount, char** ArgvList)
{
     ThreadLimit::SetThreadName("hlquery");

     Config = std::make_unique<ServerConfig>(ArgcCount, ArgvList);

     Affinity = std::make_unique<hlquery_threadpool::CPUAffinityManager>();

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

/* Entry point for the application */

int main(int argc, char** argv)
{
     Instance = std::make_unique<hlquery>(argc, argv);

     Instance->Run();

     Instance.reset();

     return 0;
}

/* Destructor for the hlquery class */
    
hlquery::~hlquery() 
{
     /* Enforce a strict teardown order to avoid memory corruption or data loss */

     try 
     {
          /* Set shutdown in progress flag to block new operations */
        
          SetShutdownInProgress(true);

          SetSyncInProgress(true);
        
          if (Database && Database->IsOpen())
          {
               try
               {
                    if (Logs)
                    {
                         Logs->Normal("hlquery", "Shutting down: Flushing and syncing database.");
                    }
                    else
                    {
                         std::cerr << "Shutting down: Flushing and syncing database." << std::endl;
                    }
                
                    bool SyncSuccess = Database->FlushAndSync();
        
                    if (!SyncSuccess)
                    {
                         std::cerr << "[CRITICAL] WAL sync failed during shutdown - DATA MAY BE LOST!" << std::endl;

                         if (Logs)
                         {
                              Logs->Critical("hlquery", "WAL sync failed during shutdown - DATA MAY BE LOST.");
                         }
                    }
                    else
                    {
                         if (Logs)
                         {
                              Logs->Normal("hlquery", "Shutting down: Database sync completed successfully.");
                         }
                         else
                         {
                              std::cerr << "Shutting down: Database sync completed successfully." << std::endl;
                         }
                    }
                
                    /* Write clean shutdown marker file for recovery tracking */

                    try
                    {
                         std::string ShutdownMarker = std::string(HLQUERY_DATA_DIR) + "/.clean_shutdown";

                         std::ofstream MarkerFileStream(ShutdownMarker);

                         if (MarkerFileStream.is_open())
                         {
                              MarkerFileStream << time(nullptr) << std::endl;

                              MarkerFileStream.close();
                         }
                    }
                    catch (...)
                    {
                         /* Ignore errors when creating the marker file */
                    }
               }
               catch (const std::exception& e)
               {
                    /* Database flush may fail during shutdown - log as CRITICAL error */

                    std::cerr << "[CRITICAL] Database flush/sync failed during shutdown: " << e.what() 
                              << " - DATA MAY BE LOST!" << std::endl;
                          
                    if (Logs)
                    {
                         try
                         {
                              Logs->Critical("hlquery", "Database flush/sync failed during shutdown: " + std::string(e.what()) + " - DATA MAY BE LOST.");
                         }
                         catch (...)
                         {
                              std::cerr << "Shutting down: Database flush failed: " << e.what() << "." << std::endl;
                         }
                    }
                    else
                    { 
                         std::cerr << "Shutting down: Database flush failed: " << e.what() << "." << std::endl;
                    }
                
                    /* Continue with destructor cleanup even if flush fails */
               }
               catch (...)
               {
                    /* 
                     * Catch all exceptions to prevent process crash during destructor.
                     * Still log as CRITICAL to ensure the failure is visible to operators.
                     */
 
                    std::cerr << "[CRITICAL] Database flush/sync failed during shutdown (unknown error) - DATA MAY BE LOST!" << std::endl;
 
                    if (Logs)
                    {
                         try
                         {
                              Logs->Critical("hlquery", 
                                  "Database flush/sync failed during shutdown (unknown error) - DATA MAY BE LOST.");
                         }
                         catch (...)
                         {
                              std::cerr << "Shutting down: Database flush failed with unknown exception - DATA MAY BE LOST!" << std::endl;
                         }
                    }
                    else
                    {
                         std::cerr << "Shutting down: Database flush failed with unknown exception - DATA MAY BE LOST!" << std::endl;
                    }
               }
          }
          else if (Database)
          {
               /* Database was initialized but never opened - no sync needed */
          }
        
          /* Release all network listeners to stop accepting new client connections */

          Listeners.clear();
        
          /* Destroy the timer subsystem to stop periodic tasks */

          if (Timers)
          {
               Timers.reset();
          }
        
          /* Clear sync lock after shutdown is fully completed */
        
          SetSyncInProgress(false);
     } 
     catch (...) 
     {
          /* Destructors must swallow exceptions to avoid unexpected process termination */

          std::cerr << "Exception in hlquery destructor - ignoring." << std::endl;
     }
}

/* Performs server initialization and sets up all core subsystems */
 
bool hlquery::Initialize() 
{
     if (Logs && !HTTPServers.empty())
     {
          /* Skip if already initialized to avoid double setup issues */

          return true;
     }
     
     std::cout << std::endl;
     ConsoleWriter::WriteStartup(FormatStartupMessage(), true);
    
     /* Initialize the core server logic */

     bool ServerInitResult = this->InitializeServer();

     if (!ServerInitResult)
     {
          return false;
     }

     /* Verify that critical subsystems are initialized properly */

    if (HTTPServers.empty())
    {
          std::cerr << "[FATAL] No HTTP/HTTPS servers initialized!" << std::endl;

          return false;
    }

     if (!Logs)
     {
          std::cerr << "[FATAL] Logs is null after initialization!" << std::endl;

          return false;
     }

     if (Logs)
     {
          Logs->Debug("hlquery", "Initializing network listeners for custom protocols.");
     }

     /* Initialize Network Listeners for configured protocols */

     const auto& BindConfigs = Config->GetBindConfigs();
    
     if (Logs)
     {
          Logs->Debug("hlquery", "Checking " + std::to_string(BindConfigs.size()) + " bind configurations for custom protocols.");
     }
    
     for (const auto& BindConfigVal : BindConfigs)
     {
          /* Skip HTTP/HTTPS ports as the HttpServer subsystem manages those internally */

          if (BindConfigVal.type == "http" || BindConfigVal.type == "https")
          {
               if (Logs)
               {
                    Logs->Debug("hlquery", "Skipping HTTP/HTTPS port " + std::to_string(BindConfigVal.port) + " (HttpServer is self-contained).");
               }

               continue;
          }
        
          /* Provision a ListenManager for each remaining custom protocol bind entry */

          if (Logs)
          {
               Logs->Debug("hlquery", "Creating ListenManager for custom protocol on " + BindConfigVal.address + ":" + std::to_string(BindConfigVal.port) + ".");
          }

          auto ListenerInstance = std::make_unique<ListenManager>(BindConfigVal.address, BindConfigVal.port);

          Listeners.push_back(std::move(ListenerInstance));
     }

     RunListeners();
    
     for (auto* server : HTTPServers)
     {
          server->SetReadyToAccept(true);
     }

     if (Logs)
     {
          Logs->Normal("hlquery", "HTTP servers verified ready to accept connections after initialization.");
     }
    
     return true;
}

std::string hlquery::FormatStartupMessage() const
{
     std::time_t startup_time_raw = std::time(nullptr);
     std::tm startup_time_local{};

     if (localtime_r(&startup_time_raw, &startup_time_local) == nullptr)
     {
          return "Starting hlquery";
     }

     std::array<char, 64> startup_time_str{};

     if (std::strftime(startup_time_str.data(), startup_time_str.size(), "%b/%d - %H:%M:%S", &startup_time_local) == 0)
     {
          return "Starting hlquery";
     }

     return "Starting hlquery [" + std::string(startup_time_str.data()) + "]";
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
    
     for (auto& ListenerVal : Listeners)
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
     catch (const std::exception& e)
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
     catch (const std::exception& e)
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
          catch (const std::exception& e)
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

static bool PreflightSSLConfig(ServerConfig* ConfigPtr)
{
     if (!ConfigPtr)
     {
          return true;
     }

     const std::string& ConfigFileLoc = ConfigPtr->GetConfigFile();

     if (ConfigFileLoc.empty())
     {
          return true;
     }

     if (!ConfigPtr->IsValid())
     {
          if (!ConfigPtr->LoadConfig(ConfigFileLoc))
          {
               const std::string& ErrorMsg = ConfigPtr->GetError();

               if (!ErrorMsg.empty())
               {
                    std::cerr << "[FATAL] " << ErrorMsg << std::endl;
               }
               else
               {
                    std::cerr << "[FATAL] Failed to load configuration file: " << ConfigFileLoc << "." << std::endl;
               }

               return false;
          }
     }

     const auto& BindConfigs = ConfigPtr->GetBindConfigs();

     for (const auto& BindConfigVal : BindConfigs)
     {
          if (!BindConfigVal.ssl)
          {
               continue;
          }

          std::string ErrorMsg;

          if (!hlquery_server::ValidateSSLConfig(BindConfigVal, &ErrorMsg))
          {
               std::cerr << "[FATAL] SSL preflight failed for " << BindConfigVal.address << ":" << BindConfigVal.port
                         << " (" << BindConfigVal.type << "): " << ErrorMsg << std::endl;

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
               std::cerr << "[FATAL] Daemonization failed." << std::endl;

               ExitManager::Exit(1);
          }
     }
    
     /* Ensure all critical systems are initialized before entering main loop */

     if (!Logs || HTTPServers.empty())
     {
          if (!Initialize())
          {
               std::cerr << "[FATAL] Initialization failed during Run()." << std::endl;

               ExitManager::Exit(1);
          }

          if (!Logs || HTTPServers.empty())
          {
               std::cerr << "[FATAL] Initialization failed - Logs or HTTPServers is null/empty after Initialize()." << std::endl;

               std::cerr << "[FATAL] Logs=" << (Logs ? "valid" : "null") << ", HTTPServers count=" << HTTPServers.size() << std::endl;

               std::cerr << "[FATAL] Exiting - cannot continue without critical systems." << std::endl;

               ExitManager::Exit(1);
          }
     }
    
     /* Register signal handlers for graceful shutdown management */

     if (Config && !Config->GetTestMode())
     {
          SetupSignalHandlers();
     }

     time_t OldTimeVal = Time();

     hlquery::WritePID();

     if (!Logs)
     {
          std::cerr << "[FATAL] Logs unique_ptr is null in Run() - LogManager failed to initialize." << std::endl;

          std::cerr << "[FATAL] Exiting - cannot continue without logging system." << std::endl;

          ExitManager::Exit(1);
     }
    
     /* Enter the primary server processing loop */

     while (true)
     {
          if (ShouldExitLoop())
          {
               break;
          }
        
          time_t NowTimeVal = 0;
        
          try
          {
               NowTimeVal = Time();
          }
          catch (...)
          {
               /* Fallback to 0 if time retrieval fails */
          }

          /* Execute periodic maintenance tasks based on clock movement */

          if (NowTimeVal != OldTimeVal)
          {
               SafePeriodicFlush();

               OldTimeVal = NowTimeVal;
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
        
          if (ShouldExitLoop()) 
          {
               break;
          }
     }
    
     /* Begin the server shutdown sequence */

     print_info("\nShutting down...");
    
     for (auto* server : HTTPServers)
     {
          server->SetReadyToAccept(false);  

          server->Stop();  

          hlquery_server::ShutdownHttpServer(server);
     }

     HTTPServers.clear();
    
     CleanupHLManagerIOOptimizations();
    
     /* Force termination to ensure the process actually exits */

     ExitManager::EmergencyExit(0);
}
