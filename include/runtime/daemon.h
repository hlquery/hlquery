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
#include <cstdint>

#include "core/hlquery.h"

/*
 * Coordinates daemon-side optimization routines and lazy processing
 * for the hlquery runtime.
 */

class DaemonHandler
{
   private:

     /* Static optimization state variables */

     /* Current backoff delay applied between daemon work cycles. */

     static std::atomic<int> AdaptiveSleepMS;

     /* Number of consecutive cycles that processed meaningful work. */

     static std::atomic<int> ConsecutiveBusyIterations;

     /* Number of consecutive cycles that observed no useful work. */

     static std::atomic<int> ConsecutiveIdleIterations;

     /* Flag-like state indicating whether the daemon is in high-throughput mode. */

     static std::atomic<int> HighThroughputModeValue;

     /* Event count captured on the previous optimization pass. */

     static std::atomic<int> LastEventCount;

     /* Monotonic counter used to defer expensive background work. */

     static std::atomic<uint64_t> LazyProcessingCounter;

     /* Current work batch size selected by the optimizer. */

     static std::atomic<int> BatchSize;

   public:

     /* Stores runtime counters used to track adaptive sleep behavior
      * and daemon throughput adjustments.
      */

     struct OptimizationStats
     {
          int adaptive_sleep_ms;
          int consecutive_busy_iterations;
          int consecutive_idle_iterations;
          int high_throughput_mode;
          int last_event_count;
          int current_event_count;
          int events_delta;
          uint64_t lazy_processing_counter;
          int batch_size;
     };

     /* Constructor */

     DaemonHandler() = default;

     /* Destructor */

     ~DaemonHandler() = default;

     /* Recomputes daemon pacing based on recent socket and event activity. */

     static void ProcessSocketEngineOptimization();

     /* Runs deferred maintenance work on a lower-frequency cadence. */

     static void ProcessLazyOperations();

     /* Restores the optimization state to its startup defaults. */

     static void ResetOptimizationState();

     /* Returns a snapshot of the current optimization counters. */

     static OptimizationStats GetOptimizationStats();
};
