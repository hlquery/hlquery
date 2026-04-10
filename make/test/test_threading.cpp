/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * Threading and Concurrency Tests
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

int test_threading()
{
     try
     {

          /* Test 1: Basic thread creation and joining */

          std::atomic<int> counter{0};
          const int num_threads = 4;
          std::vector<std::thread> threads;

          for (int i = 0; i < num_threads; ++i)
          {
               threads.emplace_back(
                    [&counter]()
                    {
                         for (int j = 0; j < 1000; ++j)
                         {
                              counter++;
                         }
                    }

               );
          }

          for (auto& t : threads)
          {
               t.join();
          }

          if (counter.load() != num_threads * 1000)
          {
               return 1;
          }

          /* Test 2: Mutex and lock_guard */

          std::mutex mtx;
          int shared_data = 0;
          threads.clear();

          for (int i = 0; i < num_threads; ++i)
          {
               threads.emplace_back(
                    [&mtx, &shared_data]()
                    {
                         for (int j = 0; j < 1000; ++j)
                         {
                              std::lock_guard<std::mutex>
                                   lock(mtx);
                              shared_data++;
                         }
                    }

               );
          }

          for (auto& t : threads)
          {
               t.join();
          }

          if (shared_data != num_threads * 1000)
          {
               return 2;
          }

          /* Test 3: Condition variable */

          std::condition_variable cv;
          bool ready = false;
          bool processed = false;

          std::thread worker(
               [&cv, &ready, &processed, &mtx]()
               {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [&ready]
                            {
                                 return ready;
                            });
                    processed = true;
               }

          );

          {
               std::lock_guard<std::mutex> lock(mtx);
               ready = true;
          }

          cv.notify_one();

          worker.join();

          if (!processed)
          {
               return 3;
          }

          /* Test 4: std::async and futures */

          auto future = std::async(
               std::launch::async,
               []()
               {
                    std::this_thread::sleep_for(
                         std::chrono::milliseconds(10));
                    return 42;
               }

          );

          if (future.get() != 42)
          {
               return 4;
          }

          /* Test 5: Thread-local storage */

          thread_local int tls_value = 0;
          std::vector<int> results(num_threads);
          threads.clear();

          for (int i = 0; i < num_threads; ++i)
          {
               threads.emplace_back(
                    [i, &results]()
                    {
                         tls_value = i + 100;
                         results[i] = tls_value;
                    }

               );
          }

          for (auto& t : threads)
          {
               t.join();
          }

          for (int i = 0; i < num_threads; ++i)
          {

               if (results[i] != i + 100)
               {
                    return 5;
               }
          }

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
}
