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
#include <functional>
#include <mutex>
#include <vector>

#include "core/config.h"

/*
 * ActionList - Deferred Action Execution System
 *
 * This system allows commands to queue actions for execution in the next
 * event loop iteration. This prevents immediate execution that could cause
 * crashes or improper cleanup.
 *
 * Features:
 *
 * - Thread-safe action queuing and processing
 * - Capacity limits to prevent unbounded growth
 * - Statistics tracking for monitoring
 * - Exception-safe execution with proper error handling
 * - RAII-based processing guard to prevent recursive processing
 * - Memory-efficient with automatic capacity management
 */

class CoreExport ActionList
{
   private:
     /* Active processing queue; holds actions currently being executed. */

     static std::vector<std::function<void()>> &Actions;

     /* Incoming queue; holds actions waiting for the next processing cycle. */

     static std::vector<std::function<void()>> &PendingActions;

     /* Synchronizes access to both action queues and related state. */

     static std::mutex &ActionsMutex;

     /* Re-entrancy guard indicating whether processing is currently running. */

     static std::atomic<bool> &Processing;

     /* Statistics counters */

     /* Total number of actions accepted into the queue. */

     static std::atomic<size_t> &TotalQueued;

     /* Total number of actions executed without exceptions. */

     static std::atomic<size_t> &TotalProcessed;

     /* Total number of actions that failed during execution. */

     static std::atomic<size_t> &TotalFailed;

     /* Total number of actions rejected due to queue capacity limits. */

     static std::atomic<size_t> &TotalDropped;

     /* Number of actions currently being executed outside the queue lock. */

     static std::atomic<size_t> &CurrentProcessingCount;

   public:
     using Action = std::function<void()>;

     /* Statistics structure for monitoring action list performance. */

     struct Statistics
     {
          /* Total actions queued since last reset. */

          size_t TotalQueued = 0;

          /* Total actions successfully processed. */

          size_t TotalProcessed = 0;

          /* Total actions that threw exceptions. */

          size_t TotalFailed = 0;

          /* Total actions dropped due to queue capacity limits. */

          size_t TotalDropped = 0;

          /* Current number of pending actions. */

          size_t CurrentPending = 0;

          /* Current number of actions being processed. */

          size_t CurrentProcessing = 0;
     };

     /*
      * Queue an action for execution in the next event loop iteration.
      *
      * @param action The action to queue. Must be callable with no arguments.
      *               Null actions are ignored.
      *
      * @note If the queue is full (MAX_QUEUE_SIZE), the action will be dropped
      *       and a warning will be logged.
      */

     static void QueueAction(Action action);

     /*
      * Process all queued actions (called from main event loop).
      *
      * This method:
      * - Atomically swaps pending actions with processing queue
      * - Executes all actions outside the lock to minimize contention
      * - Handles exceptions from individual actions gracefully
      * - Updates statistics
      *
      * @note This method is thread-safe and prevents recursive processing.
      */

     static void ProcessActions();

     /*
      * Get the number of queued actions waiting to be processed.
      *
      * @return Number of pending actions
      */

     static size_t GetActionCount();

     /*
      * Clear all queued actions (for shutdown).
      *
      * This method safely clears both pending and processing queues.
      * It will not throw exceptions even during destruction.
      */

     static void ClearActions();

     /*
      * Get statistics about action list performance.
      *
      * @return Statistics structure with current metrics
      */

     static Statistics GetStatistics();

     /*
      * Reset all statistics counters to zero.
      *
      * This does not affect queued or processing actions, only the counters.
      */

     static void ResetStatistics();
};
