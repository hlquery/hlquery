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
#include <mutex>

#include "core/hlquery.h"
#include "core/timers.h"

Timer::Timer() : NextRun(Clock::time_point::max()), Interval(0), Repeating(false)
{

}

Timer::Timer(std::function<void()> callback, Clock::time_point next_run, std::chrono::milliseconds interval, bool repeating)
     : NextRun(next_run), Interval(interval), Callback(std::move(callback)), Repeating(repeating)
{

}

Timer::~Timer()
{

}

bool Timer::IsDue(Clock::time_point now) const
{
     return now >= NextRun;
}

void Timer::Execute() const
{
     if (Callback)
     {
          std::function<void()> CallbackCopy = Callback;
          CallbackCopy();
     }
}

void Timer::UpdateAfterRun(Clock::time_point now)
{
     NextRun = Repeating ? now + Interval : Clock::time_point::max();
}

bool Timer::IsRepeating() const
{
     return Repeating;
}

bool Timer::IsRetired() const
{
     return !Repeating && NextRun == Clock::time_point::max();
}

Timer::Clock::time_point Timer::GetNextRun() const
{
     return NextRun;
}

TimerManager::TimerManager()
{

}

/* Default destructor */

TimerManager::~TimerManager()
{

}

/* Add a new timer */

void TimerManager::Add(std::function<void()> callback, std::chrono::milliseconds delay, bool repeating)
{
     /* Protect the timer list */

     std::unique_lock<std::shared_mutex> lock(MutexValue);

     /* Current time */

     auto now = Instance->Now();

     Entries.push_back(Timer(std::move(callback), now + delay, delay, repeating));

     const size_t total_timers = Entries.size();

     lock.unlock();

     if (Instance && Instance->Modules)
     {
          try
          {
               NOTIFY_MODULES(OnNewTimer, static_cast<uint64_t>(std::max<int64_t>(0, delay.count())), repeating, total_timers);
          }
          catch (...)
          {
          
          }
     }
}

/* Execute due timers */

void TimerManager::Tick()
{
     /* Protect the timer list */

     std::unique_lock<std::shared_mutex> lock(MutexValue);

     /* Current time */

     auto now = Instance->Now();

     for (auto &Entry : Entries)
     {
          if (Entry.IsDue(now))
          {
               /* Execute task without holding the lock to avoid deadlocks */

               lock.unlock();
               Entry.Execute();
               lock.lock();

               Entry.UpdateAfterRun(now);
          }
     }

     /* Drop non-repeating timers that have fired */

     Entries.erase(
          std::remove_if(Entries.begin(), Entries.end(),
                         [](const Timer &Entry)
                         {
                              return Entry.IsRetired();
                         }),
          Entries.end());
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

     for (const auto &Entry : Entries)
     {
          if (Entry.GetNextRun() < earliest)
          {
               earliest = Entry.GetNextRun();
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
