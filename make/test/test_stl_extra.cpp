/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * STL Extra Library Tests
 * Covers standard headers not already tested elsewhere in make/test
 */

#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

int test_stl_extra()
{
     try
     {
          /* <cctype> */

          if (!std::isdigit('9'))
          {
               return 1;
          }

          /* <cerrno> */

          errno = 0;
          if (errno != 0)
          {
               return 2;
          }

          /* <charconv> */

          char buf[16];
          auto result = std::to_chars(buf, buf + sizeof(buf), 12345);
          if (result.ec != std::errc())
          {
               return 3;
          }

          /* <climits>, <limits> */

          if (INT_MAX <= 0 || std::numeric_limits<int>::min() >= 0)
          {
               return 4;
          }

          /* <cmath> */

          if (std::sqrt(16.0) != 4.0)
          {
               return 5;
          }

          /* <cstdint> */

          std::uint64_t u64 = 42;
          if (u64 != 42)
          {
               return 6;
          }

          /* <cstdio> */

          char out[8];
          if (std::snprintf(out, sizeof(out), "%d", 7) <= 0)
          {
               return 7;
          }

          /* <cstdlib> */

          long n = std::strtol("123", nullptr, 10);
          if (n != 123)
          {
               return 8;
          }

          /* <iomanip>, <sstream> */

          std::stringstream ss;
          ss << std::setw(3) << 7;
          if (ss.str().size() < 3)
          {
               return 9;
          }

          /* <iterator> */

          std::list<int> lst = {1, 2, 3};
          auto it = std::next(lst.begin());
          if (*it != 2)
          {
               return 10;
          }

          /* <map>, <set>, <unordered_set> */

          std::map<int, int> mp;
          mp[1] = 2;
          std::set<int> st = {1, 2};
          std::unordered_set<int> ust = {3, 4};
          if (mp[1] != 2 || st.count(2) == 0 || ust.count(4) == 0)
          {
               return 11;
          }

          /* <optional> */

          std::optional<int> opt = 5;
          if (!opt || *opt != 5)
          {
               return 12;
          }

          /* <queue> */

          std::queue<int> q;
          q.push(1);
          q.push(2);
          if (q.front() != 1 || q.back() != 2)
          {
               return 13;
          }

          /* <shared_mutex> */

          std::shared_mutex sm;
          sm.lock_shared();
          sm.unlock_shared();

          /* <stdexcept>, <system_error> */

          try
          {
               throw std::runtime_error("test");
          }
          catch (const std::runtime_error&)
          {
          }
          std::error_code ec;
          if (ec.value() != 0)
          {
               return 14;
          }

          return 0;
     }
     catch (const std::exception&)
     {
          return 15;
     }
     catch (...)
     {
          return 16;
     }
}
