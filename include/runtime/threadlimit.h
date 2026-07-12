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
#include <cstddef>

/*
 * Global thread limit manager
 * Tracks total threads created and enforces MaxThreads limit from config
 */

class ThreadLimit
{

   private:

     static std::atomic<size_t> MaxThreadsValue;

     static std::atomic<size_t> CurrentThreads;

   public:

     /* Get the maximum number of threads allowed globally */

     static size_t GetMaxThreads();

     /* Set the maximum number of threads allowed globally */

     static void SetMaxThreads(size_t max_threads);

     /* Initialize threading system from configuration */

     static bool Initialize(class ServerConfig* config);

     /* Calculate how many threads a subsystem can use */

     static size_t CalculateThreadCount(size_t requested, int priority = 0);

     /* Calculate fair thread distribution across multiple pools */

     static size_t CalculateThreadDistribution(size_t total_requested, size_t num_pools);

     /* Get current total thread count */

     static size_t GetCurrentThreadCount()
     {
          return CurrentThreads.load();
     }

     /* Try to reserve one thread slot without exceeding the global limit */

     static bool TryAcquireThreadSlot();

     /* Increment thread count */

     static void IncrementThreadCount()
     {
          CurrentThreads.fetch_add(1, std::memory_order_acq_rel);
     }

     /* Decrement thread count */

     static void DecrementThreadCount()
     {
          size_t CurrentValue = CurrentThreads.load(std::memory_order_acquire);

          while (CurrentValue > 0)
          {
               if (CurrentThreads.compare_exchange_weak(CurrentValue, CurrentValue - 1, std::memory_order_acq_rel, std::memory_order_acquire))
               {
                    return;
               }
          }
     }

     /* Reset thread counters after fork */

     static void ResetAfterFork();

     /* Set the name of the current thread */

     static void SetThreadName(const char* name);
};
