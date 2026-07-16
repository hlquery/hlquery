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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>

#include "common/actionlist.h"
#include "common/searchpool.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/daemon.h"

std::atomic<int> DaemonHandler::AdaptiveSleepMS(0);

std::atomic<int> DaemonHandler::ConsecutiveBusyIterations(0);

std::atomic<int> DaemonHandler::ConsecutiveIdleIterations(0);

std::atomic<int> DaemonHandler::HighThroughputModeValue(0);

std::atomic<int> DaemonHandler::LastEventCount(0);

/* Use uint64_t to prevent overflow during long-running process life cycles */

std::atomic<uint64_t> DaemonHandler::LazyProcessingCounter(0);

std::atomic<int> DaemonHandler::BatchSize(10000);

std::atomic<int> DaemonHandler::LazyInterval(10000);

std::atomic<int> DaemonHandler::PressureScore(0);

std::atomic<int> DaemonHandler::AdmissionOpenValue(1);

std::atomic<uint64_t> DaemonHandler::MaintenanceRuns(0);

std::atomic<uint64_t> DaemonHandler::MaintenanceDeferrals(0);

decltype(DaemonHandler::StageRuns) DaemonHandler::StageRuns{};

decltype(DaemonHandler::StageDeferrals) DaemonHandler::StageDeferrals{};

decltype(DaemonHandler::StageLastRuntimeUS) DaemonHandler::StageLastRuntimeUS{};

struct DaemonPressureSnapshot
{
     size_t PendingActions = 0;
     size_t HTTPQueue = 0;
     size_t SearchQueue = 0;
     size_t WriteQueue = 0;
     size_t ManagementQueue = 0;
     bool PoolsInitialized = false;
     bool HasPendingSocketWork = false;
     int PressureScoreValue = 0;
};

static const char *GetDaemonStageName(std::size_t StageValue)
{
     switch (StageValue)
     {
          case DaemonHandler::StageSocketPacing:
               return "socket_pacing";
          case DaemonHandler::StageLazyMaintenance:
               return "lazy_maintenance";
          case DaemonHandler::StageStorageHealth:
               return "storage_health";
          case DaemonHandler::StageQueryPressure:
               return "query_pressure";
          case DaemonHandler::StageCompactionPressure:
               return "compaction_pressure";
          default:
               return "unknown";
     }
}

static int ScoreRatio(size_t Value, size_t SoftLimit, int Weight)
{
     if (SoftLimit == 0 || Value == 0)
     {
          return 0;
     }

     const size_t Capped = std::min(Value, SoftLimit * 2);
     return static_cast<int>((static_cast<double>(Capped) / static_cast<double>(SoftLimit * 2)) * Weight);
}

static DaemonPressureSnapshot BuildDaemonPressureSnapshot(bool HasPendingSocketWork)
{
     DaemonPressureSnapshot Snapshot;
     Snapshot.HasPendingSocketWork = HasPendingSocketWork;
     Snapshot.PendingActions = ActionList::GetActionCount();
     Snapshot.PoolsInitialized = ThreadPoolManager::GetInstance().IsInitialized();

     if (Snapshot.PoolsInitialized)
     {
          ThreadPoolManager::GlobalStats PoolStats = ThreadPoolManager::GetInstance().GetGlobalStats();
          Snapshot.HTTPQueue = PoolStats.HTTPPool.QueueSize;
          Snapshot.SearchQueue = PoolStats.SearchPool.QueueSize;
          Snapshot.WriteQueue = PoolStats.WritePool.QueueSize;
          Snapshot.ManagementQueue = PoolStats.ManagementPool.QueueSize;
     }

     int Score = 0;
     Score += HasPendingSocketWork ? 25 : 0;
     Score += ScoreRatio(Snapshot.PendingActions, 2000, 20);
     Score += ScoreRatio(Snapshot.HTTPQueue, 8000, 20);
     Score += ScoreRatio(Snapshot.SearchQueue, 4000, 25);
     Score += ScoreRatio(Snapshot.WriteQueue, 2000, 15);
     Score += ScoreRatio(Snapshot.ManagementQueue, 1000, 10);

     Snapshot.PressureScoreValue = std::min(100, Score);

     return Snapshot;
}

static int SelectLazyIntervalForPressure(int PressureScoreValue)
{
     if (PressureScoreValue >= 80)
     {
          return 50000;
     }

     if (PressureScoreValue >= 60)
     {
          return 25000;
     }

     if (PressureScoreValue >= 35)
     {
          return 10000;
     }

     if (PressureScoreValue <= 10)
     {
          return 2500;
     }

     return 5000;
}

static int RuntimeUSSince(const std::chrono::steady_clock::time_point &StartedAt)
{
     auto Duration = std::chrono::steady_clock::now() - StartedAt;
     return static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(Duration).count());
}

/* Handles HLQuery-style adaptive sleep and high-throughput optimization logic */

void DaemonHandler::ProcessSocketEngineOptimization()
{
     const auto StageStartedAt = std::chrono::steady_clock::now();
     StageRuns[StageSocketPacing].fetch_add(1, std::memory_order_relaxed);

     /*
      * Check for pending network work FIRST before executing any sleep logic.
      * If there is any pending work, immediately disable adaptive sleep and enable
      * HighThroughputModeValue to ensure the server remains responsive.
      */

     bool HasPendingWork = SocketEngine::HasPendingWork();
     DaemonPressureSnapshot PressureSnapshot = BuildDaemonPressureSnapshot(HasPendingWork);
     StageRuns[StageQueryPressure].fetch_add(1, std::memory_order_relaxed);
     PressureScore.store(PressureSnapshot.PressureScoreValue, std::memory_order_relaxed);
     LazyInterval.store(SelectLazyIntervalForPressure(PressureSnapshot.PressureScoreValue), std::memory_order_relaxed);
     AdmissionOpenValue.store(PressureSnapshot.PressureScoreValue < 70 ? 1 : 0, std::memory_order_relaxed);

     /* Pending work means the loop should stay in an aggressively responsive mode. */

     if (HasPendingWork)
     {
          AdaptiveSleepMS.store(0, std::memory_order_relaxed);
          HighThroughputModeValue.store(1, std::memory_order_relaxed);
          ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
          ConsecutiveBusyIterations.fetch_add(1, std::memory_order_relaxed);
          StageLastRuntimeUS[StageSocketPacing].store(RuntimeUSSince(StageStartedAt), std::memory_order_relaxed);

          return;
     }

     /* Track activity levels based on socket event deltas */

     int CurrentEventCount = SocketEngine::GetEventCount();
     int LastCount = LastEventCount.load(std::memory_order_relaxed);
     int EventsDelta = CurrentEventCount - LastCount;

     /* Any positive delta means the engine is still actively consuming socket work. */

     if (EventsDelta > 0)
     {
          int BusyCount = ConsecutiveBusyIterations.fetch_add(1, std::memory_order_relaxed);
          ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);

          /*
           * Reset sleep time IMMEDIATELY when any activity is detected.
           * This ensures subsequent operations do not suffer from accumulated
           * sleep time from previous idle periods.
           */

          AdaptiveSleepMS.store(0, std::memory_order_relaxed);
          HighThroughputModeValue.store(1, std::memory_order_relaxed);

          if (BusyCount > 100)
          {
               ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);
          }
     }
     else
     {
          int IdleCount = ConsecutiveIdleIterations.fetch_add(1, std::memory_order_relaxed);

          ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);

          /*
           * Require a significant number of idle iterations before introducing sleep.
           * This keeps HighThroughputModeValue active longer during processing bursts.
           */

          if (IdleCount > 1000)
          {
               int CurrentSleep = AdaptiveSleepMS.load(std::memory_order_relaxed);

               /* Increment sleep time gradually up to a 1ms maximum cap */

               int NewSleep = std::min(CurrentSleep + 1, 1);
               AdaptiveSleepMS.store(NewSleep, std::memory_order_relaxed);
               HighThroughputModeValue.store(0, std::memory_order_relaxed);

               ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
          }
     }

     LastEventCount.store(CurrentEventCount, std::memory_order_relaxed);

     /*
      * The actual sleep/yield logic is handled externally based on these flags.
      * We ensure that memory ordering is preserved for these optimization signals.
      */

     (void)HighThroughputModeValue.load(std::memory_order_relaxed);
     (void)AdaptiveSleepMS.load(std::memory_order_relaxed);
     StageLastRuntimeUS[StageSocketPacing].store(RuntimeUSSince(StageStartedAt), std::memory_order_relaxed);
}

/* Manages ultra-lazy processing tasks for deferred or expensive operations */

void DaemonHandler::ProcessLazyOperations()
{
     const auto StageStartedAt = std::chrono::steady_clock::now();
     StageRuns[StageLazyMaintenance].fetch_add(1, std::memory_order_relaxed);

     uint64_t Counter = LazyProcessingCounter.fetch_add(1, std::memory_order_relaxed);

     /* Protect against counter overflow by resetting when approaching limits */

     if (Counter >= UINT64_MAX - 10000)
     {
          LazyProcessingCounter.store(0, std::memory_order_relaxed);
          Counter = 0;
     }

     const bool HasPendingSocketWork = SocketEngine::HasPendingWork();

     DaemonPressureSnapshot PressureSnapshot = BuildDaemonPressureSnapshot(HasPendingSocketWork);
     StageRuns[StageQueryPressure].fetch_add(1, std::memory_order_relaxed);
     PressureScore.store(PressureSnapshot.PressureScoreValue, std::memory_order_relaxed);

     const int CurrentInterval = SelectLazyIntervalForPressure(PressureSnapshot.PressureScoreValue);
     LazyInterval.store(CurrentInterval, std::memory_order_relaxed);

     const bool AdmitOptionalWork = PressureSnapshot.PressureScoreValue < 70;
     AdmissionOpenValue.store(AdmitOptionalWork ? 1 : 0, std::memory_order_relaxed);

     if (!AdmitOptionalWork)
     {
          MaintenanceDeferrals.fetch_add(1, std::memory_order_relaxed);
          StageDeferrals[StageLazyMaintenance].fetch_add(1, std::memory_order_relaxed);
          StageDeferrals[StageStorageHealth].fetch_add(1, std::memory_order_relaxed);
          StageDeferrals[StageCompactionPressure].fetch_add(1, std::memory_order_relaxed);
          StageLastRuntimeUS[StageLazyMaintenance].store(RuntimeUSSince(StageStartedAt), std::memory_order_relaxed);

          return;
     }

     /* Trigger background database maintenance tasks at adaptive intervals. */

     if (Instance && Instance->Database && CurrentInterval > 0 && (Counter % static_cast<uint64_t>(CurrentInterval) == 0))
     {
          MaintenanceRuns.fetch_add(1, std::memory_order_relaxed);
          StageRuns[StageStorageHealth].fetch_add(1, std::memory_order_relaxed);
          StageRuns[StageCompactionPressure].fetch_add(1, std::memory_order_relaxed);

          /* Sampling time here preserves the historical hook without forcing extra work. */

          (void)time(nullptr);

          /*
           * Background maintenance like compaction and expiration is handled
           * automatically by the underlying LSM storage engine.
           */
     }

     StageLastRuntimeUS[StageLazyMaintenance].store(RuntimeUSSince(StageStartedAt), std::memory_order_relaxed);
}

/* Resets all internal optimization counters and state flags */

void DaemonHandler::ResetOptimizationState()
{
     AdaptiveSleepMS.store(0, std::memory_order_relaxed);
     ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);
     ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
     HighThroughputModeValue.store(0, std::memory_order_relaxed);
     LastEventCount.store(0, std::memory_order_relaxed);
     LazyProcessingCounter.store(0, std::memory_order_relaxed);
     LazyInterval.store(10000, std::memory_order_relaxed);
     PressureScore.store(0, std::memory_order_relaxed);
     AdmissionOpenValue.store(1, std::memory_order_relaxed);
     MaintenanceRuns.store(0, std::memory_order_relaxed);
     MaintenanceDeferrals.store(0, std::memory_order_relaxed);

     for (std::size_t Index = 0; Index < StageCount; ++Index)
     {
          StageRuns[Index].store(0, std::memory_order_relaxed);
          StageDeferrals[Index].store(0, std::memory_order_relaxed);
          StageLastRuntimeUS[Index].store(0, std::memory_order_relaxed);
     }
}

/* Retrieves current snapshots of optimization and performance statistics */

DaemonHandler::OptimizationStats DaemonHandler::GetOptimizationStats()
{
     OptimizationStats Stats;
     Stats.adaptive_sleep_ms = AdaptiveSleepMS.load(std::memory_order_relaxed);
     Stats.consecutive_busy_iterations = ConsecutiveBusyIterations.load(std::memory_order_relaxed);
     Stats.consecutive_idle_iterations = ConsecutiveIdleIterations.load(std::memory_order_relaxed);
     Stats.high_throughput_mode = HighThroughputModeValue.load(std::memory_order_relaxed);
     Stats.last_event_count = LastEventCount.load(std::memory_order_relaxed);
     Stats.current_event_count = SocketEngine::GetEventCount();
     Stats.events_delta = Stats.current_event_count - Stats.last_event_count;
     Stats.lazy_processing_counter = LazyProcessingCounter.load(std::memory_order_relaxed);
     Stats.batch_size = BatchSize.load(std::memory_order_relaxed);
     Stats.lazy_interval = LazyInterval.load(std::memory_order_relaxed);
     Stats.pressure_score = PressureScore.load(std::memory_order_relaxed);
     Stats.admission_open = AdmissionOpenValue.load(std::memory_order_relaxed);
     Stats.maintenance_runs = MaintenanceRuns.load(std::memory_order_relaxed);
     Stats.maintenance_deferrals = MaintenanceDeferrals.load(std::memory_order_relaxed);

     DaemonPressureSnapshot PressureSnapshot = BuildDaemonPressureSnapshot(SocketEngine::HasPendingWork());
     Stats.pending_actions = PressureSnapshot.PendingActions;
     Stats.http_queue = PressureSnapshot.HTTPQueue;
     Stats.search_queue = PressureSnapshot.SearchQueue;
     Stats.write_queue = PressureSnapshot.WriteQueue;
     Stats.management_queue = PressureSnapshot.ManagementQueue;

     for (std::size_t Index = 0; Index < StageCount; ++Index)
     {
          Stats.stages[Index].name = GetDaemonStageName(Index);
          Stats.stages[Index].runs = StageRuns[Index].load(std::memory_order_relaxed);
          Stats.stages[Index].deferrals = StageDeferrals[Index].load(std::memory_order_relaxed);
          Stats.stages[Index].last_runtime_us = StageLastRuntimeUS[Index].load(std::memory_order_relaxed);
     }

     return Stats;
}
