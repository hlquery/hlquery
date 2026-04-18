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
#include <ctime>
#include <time.h>

/* 
 * Return the current wall-clock time as whole seconds since the Unix epoch.
 * This helper uses `std::chrono::system_clock` so callers get a portable
 * timestamp source for persisted values, log correlation, and coarse runtime
 * comparisons. The conversion intentionally truncates to seconds because the
 * public `time_t` interface does not promise sub-second precision here.
 * If the underlying clock access or conversion throws unexpectedly, the
 * helper returns `0` so callers receive a deterministic failure value.
 */

inline time_t Time()
{
     try
     {
          const auto now = std::chrono::system_clock::now();
          const auto duration = now.time_since_epoch();
          const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);

          return static_cast<time_t>(seconds.count());
     }
     catch (...)
     {
          return 0;
     }
}

/* 
 * Return the current wall-clock time as milliseconds since the Unix epoch.
 * This helper shares the same clock source as `Time()` but preserves
 * millisecond precision for metrics, cache timestamps, and timeout bookkeeping
 * that need a denser value than `time_t` can provide. The result is stored in
 * `long long` to give call sites a stable signed integer type across the code
 * base. If the clock query or duration conversion fails, the helper returns
 * `0` so error handling remains simple for existing callers.
 */

inline long long NowMs()
{
     try
     {
          const auto now = std::chrono::system_clock::now();
          const auto duration = now.time_since_epoch();
          const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);

          return static_cast<long long>(milliseconds.count());
     }
     catch (...)
     {
          return 0;
     }
}

/* 
 * Return a monotonic steady-clock time point for interval measurement.
 * On Linux, this prefers `CLOCK_BOOTTIME` when available so elapsed-time
 * calculations can continue across suspend and resume cycles. On BSD and
 * macOS, it falls back to `CLOCK_MONOTONIC` when that POSIX clock is exposed
 * by the platform headers. If no suitable POSIX clock is available or the
 * syscall fails, the helper falls back to `std::chrono::steady_clock::now()`.
 * If all clock access unexpectedly throws, the helper returns a
 * default-constructed steady-clock time point.
 */

inline std::chrono::steady_clock::time_point Now()
{
     try
     {
          struct timespec ts;

#if defined(__linux__) && defined(CLOCK_BOOTTIME)

          if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0)
          {
               const auto duration =
                    std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);

               return std::chrono::steady_clock::time_point(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
          }

#endif

#if defined(CLOCK_MONOTONIC)

          if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
          {
               const auto duration = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);

               return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
          }

#endif

          return std::chrono::steady_clock::now();
     }
     catch (...)
     {
          return std::chrono::steady_clock::time_point{};
     }
}
