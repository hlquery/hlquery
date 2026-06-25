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

#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "api/httpserver.h"
#include "core/helpers.h"
#include "core/hlquery.h"
#include "runtime/timers.h"
#include "runtime/daemon.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/tools.h"
#include "search/storageengine.h"

/* Returns true when either process termination state has been requested. */

bool CoreHelpers::ShouldExitLoop()
{
     return hlquery::ShouldForceExit() || hlquery::ShouldShutdown();
}

/* Transfers ownership of a worker thread to the application thread registry. */

void hlquery::AddBackgroundThread(std::thread &&ThreadVal)
{
     /* Protect the registry while appending the new worker. */

     std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);
     BackgroundThreads.push_back(std::move(ThreadVal));
}

/* Removes and returns all registered worker threads for orderly joining. */

std::vector<std::thread> hlquery::TakeBackgroundThreads()
{
     std::vector<std::thread> ThreadsToJoin;

     /* Swap under the lock to minimize time spent blocking thread registration. */

     {
          std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);
          ThreadsToJoin.swap(BackgroundThreads);
     }

     return ThreadsToJoin;
}

/* Flushes pending database writes without allowing failures to stop periodic work. */

void CoreHelpers::SafePeriodicFlush()
{
     /* A flush is unavailable until both the application and database exist. */

     if (!Instance || !Instance->Database)
     {
          return;
     }

     /* Isolate storage failures from the caller's maintenance loop. */

     try
     {
          Instance->Database->Flush();
     }
     catch (const std::exception &e)
     {
          /* Include the storage exception when logging is available. */

          if (Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Periodic flush failed: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          /* Preserve loop execution when an unknown exception escapes storage. */

          if (Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Periodic flush failed with unknown exception.");
          }
     }
}

/* Runs deferred daemon operations and advances application timers. */

void CoreHelpers::ProcessPeriodicTasks()
{
     /* Process daemon work independently so timer work can still proceed. */

     try
     {
          DaemonHandler::ProcessLazyOperations();
     }
     catch (const std::exception &e)
     {
          /* Record the failure without terminating the periodic task loop. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Lazy operations failed: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          /* Handle non-standard failures through the same diagnostic channel. */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("hlquery", "Lazy operations failed with unknown exception.");
          }
     }

     /* Advance timers only after the timer manager becomes available. */

     if (Instance && Instance->Timers)
     {
          try
          {
               Instance->Timers->Tick();
          }
          catch (const std::exception &e)
          {
               /* Keep later periodic iterations available after a timer failure. */

               if (Instance->Logs)
               {
                    Instance->Logs->Debug("hlquery", "Timer tick failed: " + std::string(e.what()) + ".");
               }
          }
          catch (...)
          {
               /* Report unknown timer failures when logging remains available. */

               if (Instance->Logs)
               {
                    Instance->Logs->Debug("hlquery", "Timer tick failed with unknown exception.");
               }
          }
     }
}

/* Loads configuration when needed and validates every SSL-enabled bind. */

bool CoreHelpers::PreflightSSLConfig()
{
     /* Missing configuration does not require SSL preflight work. */

     if (!Instance || !Instance->Config)
     {
          return true;
     }

     ServerConfig *ConfigPtr = Instance->Config.get();
     const std::string &ConfigFileLoc = ConfigPtr->GetConfigFile();

     if (ConfigFileLoc.empty())
     {
          return true;
     }

     /* Load the configuration before inspecting bind definitions. */

     if (!ConfigPtr->IsValid())
     {
          if (!ConfigPtr->LoadConfig(ConfigFileLoc))
          {
               /* Prefer the parser's specific error when one is available. */

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

     /* Validate only network bindings that explicitly enable SSL. */

     for (const auto &BindConfigVal : BindConfigs)
     {
          if (!BindConfigVal.ssl)
          {
               continue;
          }

          std::string ErrorMsg;

          /* Stop startup at the first invalid certificate or key configuration. */

          if (!ValidateSSLConfig(BindConfigVal, &ErrorMsg))
          {
               print_error("SSL preflight failed for {}:{} ({}): {}",
                           BindConfigVal.address,
                           BindConfigVal.port,
                           BindConfigVal.type,
                           ErrorMsg);

               return false;
          }
     }

     return true;
}

/* Prints a startup heading followed by each loaded module name. */

void CoreHelpers::PrintStartupModuleList(const std::string &Heading, const std::vector<std::string> &ModuleNames)
{
     ConsoleWriter::WriteStartup(Heading + ":", true, false);

     /* Emit an explicit status when the module collection is empty. */

     if (ModuleNames.empty())
     {
          ConsoleWriter::WriteStartup("No modules loaded.", true, false);
          return;
     }

     /* Render one consistently indented startup entry per module. */

     for (const auto &ModuleName : ModuleNames)
     {
          ConsoleWriter::WriteStartupPlain("       - " + ModuleName, false);
     }
}
