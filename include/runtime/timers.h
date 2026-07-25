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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"

/*
 * Represents one scheduled task entry.
 * A timer stores the next execution point, the repeat interval,
 * and the callback that will run when the timer becomes due.
 */

class CoreExport Timer
{
   private:
     /* Timer clock type */

     using Clock = std::chrono::steady_clock;

     /* Next scheduled execution time */

     Clock::time_point NextRun;

     /* Repeat interval */

     std::chrono::milliseconds Interval;

     /* Callback task */

     std::function<void()> Callback;

     /* Whether the timer repeats */

     bool Repeating;

     /* Manager-assigned identity used for cancellation and draining. */

     uint64_t Identifier = 0;

   public:
     /* Constructor */

     Timer();

     /*
      * Construct one scheduled timer.
      * The timer can be configured as one-shot or repeating.
      */

     Timer(std::function<void()> callback, Clock::time_point next_run, std::chrono::milliseconds interval, bool repeating);

     /* Destructor */

     ~Timer();

     /* Returns whether the timer is due */

     bool IsDue(Clock::time_point now) const;

     /* 
      * Execute the timer callback.
      * The callback is invoked only when a valid callable exists.
      */

     void Execute() const;

     /* 
      * Reschedule or retire the timer after execution.
      * Repeating timers are moved forward. One-shot timers are retired.
      */

     void UpdateAfterRun(Clock::time_point now);

     /* Returns whether the timer repeats */

     bool IsRepeating() const;

     /* Returns whether the timer has been retired */

     bool IsRetired() const;

     /* Returns or assigns the manager-owned timer identifier. */

     uint64_t GetIdentifier() const;

     void SetIdentifier(uint64_t IdentifierValue);

     /* Releases the callback while its defining module is still loaded. */

     void ClearCallback();

     /* Returns the next execution time */

     Clock::time_point GetNextRun() const;
};

/* 
 * Coordinates the collection of active timers.
 * The manager owns timer entries, protects them with a shared mutex,
 * and exposes operations for scheduling, ticking, and clearing timers.
 */

class CoreExport TimerManager
{
   private:
     /* Active timers */

     std::vector<Timer> Entries;

     /* Synchronization for timer storage */

     mutable std::shared_mutex MutexValue;

     /* Coordinates cancellation with callbacks already selected by Tick(). */

     std::condition_variable_any TimerCondition;

     uint64_t NextIdentifier = 1;

     std::unordered_map<uint64_t, size_t> ActiveTimers;

     std::unordered_set<uint64_t> CancelledTimers;

   public:

     /* Timer clock type */

     using Clock = std::chrono::steady_clock;

     /* Constructor */

     TimerManager();

     /* Destructor */

     ~TimerManager();

     /* 
      * Add(): Creates a new timer.
      *
      * The delay determines the first run and also the repeat interval
      * when the timer is configured to execute repeatedly.
      */

     uint64_t Add(std::function<void()> callback, std::chrono::milliseconds delay, bool repeating = false);

     /* Add one preconstructed timer entry. */

     uint64_t Add(Timer entry);

     /* Cancels a timer and waits for any selected callback copy to be destroyed. */

     bool CancelAndWait(uint64_t Identifier);

     /* 
      * Execute due timers.
      * Any timer whose next run time has already passed is dispatched.
      */

     void Tick();

     /*
      * Get milliseconds until next scheduled timer.
      * Returns 0 when a timer is already overdue.
      * Returns -1 when there are no pending timers.
      * Returns a positive value when the next timer is still pending.
      */

     int GetTimeUntilNextMs();

     /* Get number of active timers */

     size_t GetTimerCount() const;

     /* Clear all timers */

     void Clear();
};
