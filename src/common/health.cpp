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

#include <ctime>
#include <string>

#include "common/actionlist.h"
#include "common/health.h"
#include "common/searchpool.h"
#include "core/hlquery.h"

void EmitDaemonHealthSnapshot(time_t NowTimeVal)
{
     if (!Instance || NowTimeVal <= 0)
     {
          return;
     }

     static time_t LastSnapshotTime = 0;
     static int ConsecutiveUnhealthy = 0;

     constexpr time_t SnapshotIntervalSec = 15;
     constexpr size_t MaxPendingActions = 2000;
     constexpr size_t MaxHTTPQueue = 8000;
     constexpr size_t MaxSearchQueue = 4000;

     if (LastSnapshotTime != 0 && (NowTimeVal - LastSnapshotTime) < SnapshotIntervalSec)
     {
          return;
     }

     LastSnapshotTime = NowTimeVal;

     const size_t PendingActions = ActionList::GetActionCount();
     const bool PoolInitialized = ThreadPoolManager::GetInstance().IsInitialized();
     ThreadPoolManager::GlobalStats PoolStats{};

     if (PoolInitialized)
     {
          PoolStats = ThreadPoolManager::GetInstance().GetGlobalStats();
     }

     size_t LoadedModules = 0;

     if (Instance->Modules)
     {
          LoadedModules = Instance->Modules->GetLoadedModuleNames().size();
     }

     const bool QueuePressure = PendingActions > MaxPendingActions ||
                                (PoolInitialized && PoolStats.HTTPPool.QueueSize > MaxHTTPQueue) ||
                                (PoolInitialized && PoolStats.SearchPool.QueueSize > MaxSearchQueue);

     if (QueuePressure)
     {
          ConsecutiveUnhealthy++;
     }
     else
     {
          ConsecutiveUnhealthy = 0;
     }

     std::string SnapshotMessage =
          "ts=" + std::to_string(static_cast<long long>(NowTimeVal)) +
          " pending_actions=" + std::to_string(PendingActions) +
          " pools_initialized=" + std::string(PoolInitialized ? "yes" : "no") +
          " http_active=" + std::to_string(PoolStats.HTTPPool.ActiveThreads) +
          " http_queue=" + std::to_string(PoolStats.HTTPPool.QueueSize) +
          " search_active=" + std::to_string(PoolStats.SearchPool.ActiveThreads) +
          " search_queue=" + std::to_string(PoolStats.SearchPool.QueueSize) +
          " write_active=" + std::to_string(PoolStats.WritePool.ActiveThreads) +
          " mgmt_active=" + std::to_string(PoolStats.ManagementPool.ActiveThreads) +
          " modules_loaded=" + std::to_string(LoadedModules) +
          " shutdown_in_progress=" + std::string(Instance->IsShuttingDown() ? "yes" : "no") +
          " signal_shutdown=" + std::to_string(static_cast<int>(ShuttingDown)) +
          " force_exit=" + std::to_string(static_cast<int>(ForceExit)) +
          " unhealthy_streak=" + std::to_string(ConsecutiveUnhealthy);

     if (Instance->Logs)
     {
          Instance->Logs->Normal("daemon_health", SnapshotMessage);
     }
}
