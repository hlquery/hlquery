/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * Chrono Library Tests
 * Tests for <chrono> library used throughout hlquery source code for timing
 */

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int test_chrono()
{
     try
     {
          using namespace std::chrono;

          /* Test 1: system_clock - get current time */

          auto now = system_clock::now();
          auto time_t_now = system_clock::to_time_t(now);

          if (time_t_now == 0)
          {
               return 1; /* Should have valid time */
          }

          /* Test 2: steady_clock - monotonic clock */

          auto start = steady_clock::now();
          std::this_thread::sleep_for(milliseconds(10));
          auto end = steady_clock::now();

          auto elapsed_ms = duration_cast<milliseconds>(end - start);
          if (elapsed_ms.count() < 5)
          { /* Should be at least 5ms */
               return 2;
          }

          /* Test 3: duration types */

          milliseconds ms(1000);
          seconds sec = duration_cast<seconds>(ms);

          if (sec.count() != 1)
          {
               return 3;
          }

          /* Test 4: duration arithmetic */

          milliseconds ms1(500);
          milliseconds ms2(500);
          milliseconds ms_sum = ms1 + ms2;

          if (ms_sum.count() != 1000)
          {
               return 4;
          }

          /* Test 5: duration comparison */

          milliseconds ms3(100);
          milliseconds ms4(200);

          if (ms3 >= ms4 || ms4 <= ms3)
          {
               return 5;
          }
          if (ms3 < ms4 && ms4 > ms3)
          {
               /* Good */
          }
          else
          {
               return 6;
          }

          /* Test 6: time_point arithmetic */

          auto tp1 = steady_clock::now();
          auto tp2 = tp1 + milliseconds(100);
          auto diff = tp2 - tp1;

          if (duration_cast<milliseconds>(diff).count() != 100)
          {
               return 7;
          }

          /* Test 7: different duration units */

          seconds sec1(1);
          milliseconds ms5 = duration_cast<milliseconds>(sec1);
          microseconds us = duration_cast<microseconds>(sec1);
          nanoseconds ns = duration_cast<nanoseconds>(sec1);

          if (ms5.count() != 1000)
          {
               return 8;
          }
          if (us.count() != 1000000)
          {
               return 9;
          }
          if (ns.count() != 1000000000)
          {
               return 10;
          }

          /* Test 8: high_resolution_clock */

          auto hr_start = high_resolution_clock::now();
          std::this_thread::sleep_for(microseconds(100));
          auto hr_end = high_resolution_clock::now();

          auto hr_duration = duration_cast<microseconds>(hr_end - hr_start);
          if (hr_duration.count() < 50)
          {
               return 11;
          }

          /* Test 9: duration literals (C++14) */

          auto one_sec = seconds(1);
          auto one_ms = milliseconds(1);
          auto one_us = microseconds(1);

          if (one_sec.count() != 1 || one_ms.count() != 1 || one_us.count() != 1)
          {
               return 12;
          }

          /* Test 10: time_point comparison */

          auto tp3 = steady_clock::now();
          std::this_thread::sleep_for(milliseconds(1));
          auto tp4 = steady_clock::now();

          if (tp3 >= tp4 || tp4 <= tp3)
          {
               return 13;
          }
          if (tp3 < tp4 && tp4 > tp3)
          {
               /* Good */
          }
          else
          {
               return 14;
          }

          /* Test 11: duration_cast precision */

          milliseconds ms6(1500);
          seconds sec2 = duration_cast<seconds>(ms6);
          milliseconds ms7 = duration_cast<milliseconds>(sec2);

          if (sec2.count() != 1)
          { /* Should truncate to 1 second */
               return 15;
          }
          if (ms7.count() != 1000)
          {
               return 16;
          }

          /* Test 12: zero duration */

          milliseconds zero_ms(0);
          if (zero_ms.count() != 0)
          {
               return 17;
          }

          /* Test 13: negative duration (if supported) */

          milliseconds ms8(100);
          milliseconds ms9 = -ms8;

          if (ms9.count() != -100)
          {
               return 18;
          }

          /* Test 14: duration multiplication */

          milliseconds ms10(100);
          milliseconds ms11 = ms10 * 3;

          if (ms11.count() != 300)
          {
               return 19;
          }

          /* Test 15: duration division */

          milliseconds ms12(300);
          milliseconds ms13 = ms12 / 3;

          if (ms13.count() != 100)
          {
               return 20;
          }

          /* Test 16: time_point duration since epoch */

          auto tp5 = system_clock::now();
          auto duration_since_epoch = tp5.time_since_epoch();

          if (duration_since_epoch.count() <= 0)
          {
               return 21;
          }

          /* Test 17: sleep_for */

          auto before_sleep = steady_clock::now();
          std::this_thread::sleep_for(milliseconds(50));
          auto after_sleep = steady_clock::now();

          auto sleep_duration = duration_cast<milliseconds>(after_sleep - before_sleep);
          if (sleep_duration.count() < 40)
          { /* Allow some variance */
               return 22;
          }

          /* Test 18: sleep_until */

          auto target_time = steady_clock::now() + milliseconds(50);
          std::this_thread::sleep_until(target_time);
          auto after_sleep_until = steady_clock::now();

          if (after_sleep_until < target_time)
          {
               return 23; /* Should have reached target time */
          }

          /* Test 19: different clock types */

          auto sys_now = system_clock::now();
          auto steady_now = steady_clock::now();
          auto hr_now = high_resolution_clock::now();

          /* All should be valid time_points */

          (void)sys_now;
          (void)steady_now;
          (void)hr_now;

          /* Test 20: duration ratio */

          using ratio_sec = std::ratio<1, 1>;
          using ratio_ms = std::ratio<1, 1000>;

          duration<long, ratio_sec> sec_dur(1);
          duration<long, ratio_ms> ms_dur = duration_cast<duration<long, ratio_ms>>(sec_dur);

          if (ms_dur.count() != 1000)
          {
               return 24;
          }

          /* Test 21: time_point from duration */

          auto epoch = system_clock::time_point();
          auto one_sec_later = epoch + seconds(1);
          auto diff_from_epoch = one_sec_later - epoch;

          if (duration_cast<seconds>(diff_from_epoch).count() != 1)
          {
               return 25;
          }

          /* Test 22: duration min/max */

          auto min_dur = milliseconds::min();
          auto max_dur = milliseconds::max();

          if (min_dur >= max_dur)
          {
               return 26;
          }

          /* Test 23: time_point min/max */

          auto min_tp = system_clock::time_point::min();
          auto max_tp = system_clock::time_point::max();

          if (min_tp >= max_tp)
          {
               return 27;
          }

          /* Test 24: duration zero */

          auto zero_dur = milliseconds::zero();
          if (zero_dur.count() != 0)
          {
               return 28;
          }

          /* Test 25: elapsed time measurement (common pattern in hlquery) */

          auto measure_start = steady_clock::now();
          /* Simulate some work */

          std::this_thread::sleep_for(milliseconds(10));
          auto measure_end = steady_clock::now();
          auto elapsed = duration_cast<microseconds>(measure_end - measure_start);

          if (elapsed.count() < 5000)
          { /* Should be at least 5ms = 5000us */
               return 29;
          }

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
}
