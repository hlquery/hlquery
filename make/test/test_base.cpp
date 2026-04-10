/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * Base Library Tests
 * Covers standard and system headers not already tested elsewhere in make/test
 */

#include <array>
#include <csignal>
#include <ctime>
#include <exception>
#include <memory>
#include <random>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unordered_map>

int test_base()
{
     try
     {
          /* std::array */

          std::array<int, 3> arr = {1, 2, 3};
          if (arr[0] != 1 || arr.size() != 3)
          {
               return 1;
          }

          /* std::memory (unique_ptr) */

          auto ptr = std::make_unique<int>(42);
          if (!ptr || *ptr != 42)
          {
               return 2;
          }

          /* std::unordered_map */

          std::unordered_map<int, int> map;
          map[1] = 1;
          if (map[1] != 1)
          {
               return 3;
          }

          /* std::random */

          std::mt19937 rng(1234);
          std::uniform_int_distribution<int> dist(1, 10);
          int val = dist(rng);
          if (val < 1 || val > 10)
          {
               return 4;
          }

          /* <ctime> */

          std::time_t now = std::time(nullptr);
          if (now == static_cast<std::time_t>(-1))
          {
               return 5;
          }

          /* <csignal> */

          if (SIGINT == 0)
          {
               return 6;
          }

          /* <sys/resource.h> */

          struct rlimit rlim;
          if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
          {
               return 7;
          }

          /* <sys/time.h> */

          struct timeval tv;
          if (gettimeofday(&tv, nullptr) != 0)
          {
               return 8;
          }

          /* <sys/wait.h> */

          int status = 0;
          if (!WIFEXITED(status) && !WIFSIGNALED(status))
          {
               return 9;
          }

          return 0;
     }
     catch (const std::exception&)
     {
          return 10;
     }
     catch (...)
     {
          return 11;
     }
}
