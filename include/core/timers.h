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

#pragma once

#include <chrono>
#include <functional>
#include <shared_mutex>
#include <vector>

class CoreExport TimerManager
{
   private:

     /* Timer entry storage */

     struct Entry
     {
          std::chrono::steady_clock::time_point next_run;
          std::chrono::milliseconds interval;
          std::function<void()> task;
          bool repeating;
     };

     /* Active timers */

     std::vector<Entry> Entries;

     /* Synchronization for timer storage */

     mutable std::shared_mutex MutexValue;

   public:

     /* Timer clock type */

     using Clock = std::chrono::steady_clock;

     /* Timer task type */

     using Task = std::function<void()>;

     /* Constructor */

     TimerManager();

     /* Destructor */

     ~TimerManager();

     /* Add a timer */

     void Add(Task task, std::chrono::milliseconds delay, bool repeating = false);

     /* Execute due timers */

     void Tick();

     /*
      * Get milliseconds until next scheduled timer.
      * Returns: 0 if overdue, -1 if none, >0 if pending.
      */

     int GetTimeUntilNextMs();

     /* Get number of active timers */

     size_t GetTimerCount() const;

     /* Clear all timers */

     void Clear();
};
