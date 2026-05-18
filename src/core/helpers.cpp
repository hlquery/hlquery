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

#include "core/helpers.h"

#include <exception>

#include "api/httpserver.h"
#include "core/hlquery.h"
#include "runtime/daemon.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/tools.h"
#include "search/storageengine.h"

bool CoreHelpers::ShouldExitLoop()
{
     return ForceExit != 0 || ShuttingDown != 0;
}

void CoreHelpers::SafePeriodicFlush()
{
     if (!Instance || !Instance->Database)
     {
          return;
     }

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

void CoreHelpers::ProcessPeriodicTasks()
{
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

bool CoreHelpers::PreflightSSLConfig(ServerConfig *ConfigPtr)
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

void CoreHelpers::PrintStartupModuleList(const std::string &Heading, const std::vector<std::string> &ModuleNames)
{
     ConsoleWriter::WriteStartup(Heading + ":", true, false);

     if (ModuleNames.empty())
     {
          ConsoleWriter::WriteStartup("No optional modules loaded.", true, false);
          return;
     }

     for (const auto &ModuleName : ModuleNames)
     {
          ConsoleWriter::WriteStartupPlain("       - " + ModuleName, false);
     }
}
