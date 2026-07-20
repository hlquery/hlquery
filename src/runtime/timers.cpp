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
#include <exception>
#include <limits>
#include <mutex>

#include "core/hlquery.h"
#include "runtime/timers.h"

namespace
{
TimerManager::Clock::time_point GetTimerNow()
{
     return Instance ? Instance->Now() : TimerManager::Clock::now();
}

int ClampTimerMilliseconds(long long Milliseconds)
{
     if (Milliseconds <= 0)
     {
          return 0;
     }

     if (Milliseconds > static_cast<long long>(std::numeric_limits<int>::max()))
     {
          return std::numeric_limits<int>::max();
     }

     return static_cast<int>(Milliseconds);
}
}

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

uint64_t Timer::GetIdentifier() const
{
     return Identifier;
}

void Timer::SetIdentifier(uint64_t IdentifierValue)
{
     Identifier = IdentifierValue;
}

void Timer::ClearCallback()
{
     Callback = nullptr;
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
     Clear();
}

/* Add a new timer */

uint64_t TimerManager::Add(std::function<void()> callback, std::chrono::milliseconds delay, bool repeating)
{
     /* Protect the timer list */

     std::unique_lock<std::shared_mutex> lock(MutexValue);

     /* Current time */

     const auto now = GetTimerNow();
     Timer Entry(std::move(callback), now + delay, delay, repeating);
     const uint64_t Identifier = NextIdentifier++;
     Entry.SetIdentifier(Identifier);
     Entries.push_back(std::move(Entry));
     const size_t total_timers = Entries.size();

     lock.unlock();

     if (Instance && Instance->Modules)
     {
          try
          {
               FOREACH_MOD(OnNewTimer, static_cast<uint64_t>(std::max<int64_t>(0, delay.count())), repeating, total_timers);
          }
          catch (...)
          {
          }
     }

     return Identifier;
}

uint64_t TimerManager::Add(Timer entry)
{
     std::unique_lock<std::shared_mutex> lock(MutexValue);
     if (entry.GetIdentifier() == 0)
     {
          entry.SetIdentifier(NextIdentifier++);
     }
     const uint64_t Identifier = entry.GetIdentifier();
     Entries.push_back(std::move(entry));
     return Identifier;
}

bool TimerManager::CancelAndWait(uint64_t Identifier)
{
     if (Identifier == 0)
     {
          return false;
     }

     std::unique_lock<std::shared_mutex> Lock(MutexValue);
     bool Found = ActiveTimers.find(Identifier) != ActiveTimers.end();

     CancelledTimers.insert(Identifier);
     const auto NewEnd = std::remove_if(Entries.begin(), Entries.end(),
                                        [Identifier](const Timer &Entry)
                                        {
                                             return Entry.GetIdentifier() == Identifier;
                                        });
     Found = Found || NewEnd != Entries.end();
     Entries.erase(NewEnd, Entries.end());

     TimerCondition.wait(Lock, [&]()
                         {
                              return ActiveTimers.find(Identifier) == ActiveTimers.end();
                         });
     CancelledTimers.erase(Identifier);
     return Found;
}

/* Execute due timers */

void TimerManager::Tick()
{
     std::vector<Timer> DueTimers;
     const auto now = GetTimerNow();

     {
          /* Protect the timer list */

          std::unique_lock<std::shared_mutex> lock(MutexValue);

          for (auto It = Entries.begin(); It != Entries.end();)
          {
               if (!It->IsDue(now))
               {
                    ++It;
                    continue;
               }

               const uint64_t Identifier = It->GetIdentifier();
               DueTimers.push_back(std::move(*It));
               ++ActiveTimers[Identifier];
               It = Entries.erase(It);
          }
     }

     std::exception_ptr FirstException;

     for (auto &Entry : DueTimers)
     {
          try
          {
               Entry.Execute();
          }
          catch (...)
          {
               if (!FirstException)
               {
                    FirstException = std::current_exception();
               }
          }

          std::unique_lock<std::shared_mutex> Lock(MutexValue);
          const uint64_t Identifier = Entry.GetIdentifier();
          const bool Cancelled = CancelledTimers.find(Identifier) != CancelledTimers.end();

          if (Entry.IsRepeating() && !Cancelled)
          {
               Entry.UpdateAfterRun(GetTimerNow());
               Entries.push_back(std::move(Entry));
               Entry.ClearCallback();
          }
          else
          {
               /* Destroy the type-erased callback before cancellation waiters can unload its module. */
               Entry.ClearCallback();
               CancelledTimers.erase(Identifier);
          }

          auto ActiveIt = ActiveTimers.find(Identifier);
          if (ActiveIt != ActiveTimers.end() && --ActiveIt->second == 0)
          {
               ActiveTimers.erase(ActiveIt);
          }
          Lock.unlock();
          TimerCondition.notify_all();
     }

     if (FirstException)
     {
          std::rethrow_exception(FirstException);
     }
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

     auto now = GetTimerNow();

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
     return ClampTimerMilliseconds(diff);
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
     for (const auto &Active : ActiveTimers)
     {
          CancelledTimers.insert(Active.first);
     }
     Entries.clear();

     TimerCondition.wait(lock, [&]()
                         {
                              return ActiveTimers.empty();
                         });
     CancelledTimers.clear();
}
