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
#include <cstring>
#include <pthread.h>

#include "core/hlquery.h"
#include "runtime/serverconfig.h"
#include "runtime/threadlimit.h"

/* Initialize the shared atomic thread tracking values */

std::atomic<size_t> ThreadLimit::MaxThreadsValue(8);

std::atomic<size_t> ThreadLimit::CurrentThreads(0);

/* Returns the global maximum thread count currently permitted */

size_t ThreadLimit::GetMaxThreads()
{
     return MaxThreadsValue.load();
}

bool ThreadLimit::TryAcquireThreadSlot()
{
     size_t CurrentValue = CurrentThreads.load();

     while (CurrentValue < GetMaxThreads())
     {
          if (CurrentThreads.compare_exchange_weak(CurrentValue, CurrentValue + 1))
          {
               return true;
          }
     }

     return false;
}

/* Configures the global maximum thread limit and logs the change */

void ThreadLimit::SetMaxThreads(size_t MaxThreads)
{
     MaxThreadsValue.store(MaxThreads);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("thread_limit", "Global thread limit set to: " + std::to_string(MaxThreads) + ".");
     }
}

/* Initializes the threading management system from the provided configuration */

bool ThreadLimit::Initialize(class ServerConfig *ConfigPointer)
{
     if (ConfigPointer)
     {
          size_t MaxThreadsVal = static_cast<size_t>(ConfigPointer->GetMaxThreads());

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("startup", "Config says max_threads should be: " + std::to_string(MaxThreadsVal) + ".");
          }

          SetMaxThreads(MaxThreadsVal);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("startup", "Global thread limit has been set to: " + std::to_string(MaxThreadsVal) + ".");
          }
     }
     else
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("startup", "WARNING: Config is null, cannot set thread limit.");
          }
     }

     return true;
}

/* Resets internal thread tracking state after a process fork */

void ThreadLimit::ResetAfterFork()
{
     CurrentThreads.store(0);
}

/* Calculates the appropriate thread allocation for a specific subsystem request */

size_t ThreadLimit::CalculateThreadCount(size_t RequestedThreads, int PriorityValue)
{
     (void)PriorityValue;

     size_t MaxLimit = GetMaxThreads();

     size_t CurrentCount = CurrentThreads.load();

     size_t AvailableCount = (CurrentCount < MaxLimit) ? (MaxLimit - CurrentCount) : 0;

     /* Determine the final allocation based on requested and available resources */

     size_t AllocatedCount = std::min(RequestedThreads, AvailableCount);

     if (Instance && Instance->Logs)
     {
          if (AllocatedCount < RequestedThreads)
          {
               Instance->Logs->Normal("thread_limit", "Thread limit reached: requested " + std::to_string(RequestedThreads) + ", allocated " + std::to_string(AllocatedCount) + " (max: " + std::to_string(MaxLimit) + ", current: " + std::to_string(CurrentCount) + ").");
          }
          else
          {
               Instance->Logs->Debug("thread_limit", "Thread allocation complete: " + std::to_string(AllocatedCount) + " threads (max: " + std::to_string(MaxLimit) + ", current: " + std::to_string(CurrentCount) + ").");
          }
     }

     return AllocatedCount;
}

/* Distributes threads fairly across multiple resource pools */

size_t ThreadLimit::CalculateThreadDistribution(size_t TotalRequestedPerPool, size_t NumPools)
{
     size_t MaxLimitVal = GetMaxThreads();
     size_t CurrentTotal = CurrentThreads.load();
     size_t AvailableTotal = (CurrentTotal < MaxLimitVal) ? (MaxLimitVal - CurrentTotal) : 0;

     if (NumPools == 0)
     {
          return 0;
     }

     /* Compute the baseline thread count for each pool */

     size_t BaseThreadsPerPool = AvailableTotal / NumPools;
     size_t ExtraThreadsCount = AvailableTotal % NumPools;

     /* Determine individual pool allocation, adding extra capacity if available */

     size_t AllocatedPerPool = BaseThreadsPerPool;

     if (ExtraThreadsCount > 0)
     {
          AllocatedPerPool = BaseThreadsPerPool + 1;
     }

     /* Cap the allocation at the originally requested amount */

     AllocatedPerPool = std::min(AllocatedPerPool, TotalRequestedPerPool);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("thread_limit", "Thread distribution results: " + std::to_string(BaseThreadsPerPool) + " base threads per pool, " + std::to_string(ExtraThreadsCount) + " pools with bonus thread" + " (available: " + std::to_string(AvailableTotal) + ", max: " + std::to_string(MaxLimitVal) + ", current: " + std::to_string(CurrentTotal) + ").");
     }

     return AllocatedPerPool;
}

/* Configures a human-readable name for the current execution thread */

void ThreadLimit::SetThreadName(const char *ThreadNameStr)
{
#ifdef __linux__

     pthread_setname_np(pthread_self(), ThreadNameStr);

#elif defined(__APPLE__) || defined(__MACH__)

     pthread_setname_np(ThreadNameStr);

#else

     (void)ThreadNameStr;

#endif
}
