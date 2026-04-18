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
#include <mutex>
#include <vector>

#include "core/config.h"

/* ExitManager handles application exit routines and cleanup. */

class CoreExport ExitManager
{
   private:

     inline static std::vector<void (*)()> CleanupFuncs;
     inline static std::mutex CleanupMutex;
     inline static std::atomic<bool> ShuttingDownValue{false};

   public:

     /* Register a cleanup function to be called on exit */

     static void RegisterCleanup(void (*func)());

     /* Call all registered cleanup functions */

     static void RunCleanups();

     /* Returns true if shutdown is in progress */

     static bool IsShuttingDown();

     /* Request graceful shutdown with status code */

     [[noreturn]] static void Exit(int status = 0);

     /* Force immediate exit with status code */

     [[noreturn]] static void QuickExit(int status = 1);

     /* Immediate exit without cleanups (async-signal-safe) */

     [[noreturn]] static void EmergencyExit(int status = 1);
};
