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

#include <cstdlib>

#include "runtime/exitmanager.h"

/* Register a cleanup function to be called on process termination */

void ExitManager::RegisterCleanup(void (*FuncPointer)())
{
     std::lock_guard<std::mutex> Lock(CleanupMutex);
     CleanupFuncs.push_back(FuncPointer);
}

/* Execute all registered cleanup functions in the order they were registered */

void ExitManager::RunCleanups()
{
     std::vector<void (*)()> CleanupFuncsToRun;

     {
          std::lock_guard<std::mutex> Lock(CleanupMutex);
          CleanupFuncsToRun.swap(CleanupFuncs);
     }

     for (auto &FuncPointer : CleanupFuncsToRun)
     {
          FuncPointer();
     }
}

/* Returns true if a shutdown sequence is currently in progress */

bool ExitManager::IsShuttingDown()
{
     return ShuttingDownValue.load();
}

/* Initiates a graceful shutdown sequence with the specified exit status */

void ExitManager::Exit(int ExitStatus)
{
     ShuttingDownValue.store(true);
     RunCleanups();
     std::exit(ExitStatus);
}

/* Forces an immediate process termination with the specified status */

void ExitManager::QuickExit(int ExitStatus)
{
     ShuttingDownValue.store(true);
     RunCleanups();
     std::quick_exit(ExitStatus);
}

/* Immediate process termination without executing any cleanup logic */

void ExitManager::EmergencyExit(int ExitStatus)
{
     std::_Exit(ExitStatus);
}
