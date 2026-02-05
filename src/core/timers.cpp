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

#include <algorithm>
#include <mutex>

#include "core/hlquery.h"
#include "core/timers.h"

/* Default constructor */

TimerManager::TimerManager()
{

}

/* Default destructor */

TimerManager::~TimerManager()
{

}

/* Add a new timer */

void TimerManager::Add(Task task, std::chrono::milliseconds delay, bool repeating)
{
     /* Protect the timer list */

     std::unique_lock<std::shared_mutex> lock(MutexValue);

     /* Current time */

     auto now = Instance->Now();

     Entries.push_back(Entry{now + delay, delay, std::move(task), repeating});
}

/* Execute due timers */

void TimerManager::Tick()
{
     /* Protect the timer list */

     std::unique_lock<std::shared_mutex> lock(MutexValue);

     /* Current time */

     auto now = Instance->Now();

     for (auto& e : Entries)
     {
          if (now >= e.next_run)
          {
               if (e.task)
               {
                    /* Execute task without holding the lock to avoid deadlocks */

                    Task task_copy = e.task;

                    lock.unlock();

                    task_copy();

                    lock.lock();
               }

               e.next_run = e.repeating ? now + e.interval : Clock::time_point::max();
          }
     }

     /* Drop non-repeating timers that have fired */

     Entries.erase(
          std::remove_if(Entries.begin(), Entries.end(),
               [](const Entry& e)
               {
                    return !e.repeating && e.next_run == Clock::time_point::max();
               }),
          Entries.end()
     );
}

/* Get milliseconds until next scheduled timer */

int TimerManager::GetTimeUntilNextMs()
{
     /* Read with shared lock */

     std::shared_lock<std::shared_mutex> lock(MutexValue);

     if (Entries.empty())
     {
          return -1;
     }

     /* Current time */

     auto now = Instance->Now();

     /* Earliest pending timer */

     auto earliest = Clock::time_point::max();

     for (const auto& e : Entries)
     {
          if (e.next_run < earliest)
          {
               earliest = e.next_run;
          }
     }

     if (earliest == Clock::time_point::max())
     {
          return -1;
     }

     auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now).count();

     return diff <= 0 ? 0 : static_cast<int>(diff);
}

/* Get number of active timers */

size_t TimerManager::GetTimerCount() const
{
     std::shared_lock<std::shared_mutex> lock(MutexValue);

     return Entries.size();
}

/* Clear all timers */

void TimerManager::Clear()
{
     std::unique_lock<std::shared_mutex> lock(MutexValue);

     Entries.clear();
}
