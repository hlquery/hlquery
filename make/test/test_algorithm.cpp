/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * STL Algorithm Library Tests
 * Tests for <algorithm> library functions used throughout hlquery source code
 */

#include <algorithm>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

int test_algorithm()
{
     try
     {
          /* Test 1: std::sort */

          std::vector<int> vec1 = {5, 2, 8, 1, 9, 3};
          std::sort(vec1.begin(), vec1.end());

          if (vec1[0] != 1 || vec1[5] != 9)
          {
               return 1;
          }

          /* Test 2: std::find */

          std::vector<int> vec2 = {10, 20, 30, 40, 50};
          auto it = std::find(vec2.begin(), vec2.end(), 30);

          if (it == vec2.end() || *it != 30)
          {
               return 2;
          }

          /* Test 3: std::find_if */

          std::vector<int> vec3 = {1, 2, 3, 4, 5, 6};
          auto it2 = std::find_if(vec3.begin(), vec3.end(),
                                  [](int x)
                                  {
                                       return x > 4;
                                  });

          if (it2 == vec3.end() || *it2 != 5)
          {
               return 3;
          }

          /* Test 4: std::count */

          std::vector<int> vec4 = {1, 2, 2, 3, 2, 4, 2};
          int count = std::count(vec4.begin(), vec4.end(), 2);

          if (count != 4)
          {
               return 4;
          }

          /* Test 5: std::count_if */

          std::vector<int> vec5 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
          int even_count = std::count_if(vec5.begin(), vec5.end(),
                                         [](int x)
                                         {
                                              return x % 2 == 0;
                                         });

          if (even_count != 5)
          {
               return 5;
          }

          /* Test 6: std::transform */

          std::vector<int> vec6 = {1, 2, 3, 4, 5};
          std::vector<int> vec6_result(5);
          std::transform(vec6.begin(), vec6.end(), vec6_result.begin(),
                         [](int x)
                         {
                              return x * 2;
                         });

          if (vec6_result[0] != 2 || vec6_result[4] != 10)
          {
               return 6;
          }

          /* Test 7: std::for_each */

          std::vector<int> vec7 = {1, 2, 3, 4, 5};
          int sum = 0;
          std::for_each(vec7.begin(), vec7.end(), [&sum](int x)
                        {
                             sum += x;
                        });

          if (sum != 15)
          {
               return 7;
          }

          /* Test 8: std::min_element / std::max_element */

          std::vector<int> vec8 = {5, 2, 8, 1, 9, 3};
          auto min_it = std::min_element(vec8.begin(), vec8.end());
          auto max_it = std::max_element(vec8.begin(), vec8.end());

          if (*min_it != 1 || *max_it != 9)
          {
               return 8;
          }

          /* Test 9: std::copy */

          std::vector<int> vec9_src = {1, 2, 3, 4, 5};
          std::vector<int> vec9_dst(5);
          std::copy(vec9_src.begin(), vec9_src.end(), vec9_dst.begin());

          if (vec9_dst != vec9_src)
          {
               return 9;
          }

          /* Test 10: std::fill */

          std::vector<int> vec10(5);
          std::fill(vec10.begin(), vec10.end(), 42);

          if (vec10[0] != 42 || vec10[4] != 42)
          {
               return 10;
          }

          /* Test 11: std::remove / std::remove_if */

          std::vector<int> vec11 = {1, 2, 3, 2, 4, 2, 5};
          vec11.erase(std::remove(vec11.begin(), vec11.end(), 2), vec11.end());

          if (std::count(vec11.begin(), vec11.end(), 2) != 0)
          {
               return 11;
          }

          /* Test 12: std::unique */

          std::vector<int> vec12 = {1, 1, 2, 2, 3, 3, 4};
          vec12.erase(std::unique(vec12.begin(), vec12.end()), vec12.end());

          if (vec12.size() != 4)
          {
               return 12;
          }

          /* Test 13: std::reverse */

          std::vector<int> vec13 = {1, 2, 3, 4, 5};
          std::reverse(vec13.begin(), vec13.end());

          if (vec13[0] != 5 || vec13[4] != 1)
          {
               return 13;
          }

          /* Test 14: std::all_of / std::any_of / std::none_of */

          std::vector<int> vec14 = {2, 4, 6, 8, 10};
          bool all_even = std::all_of(vec14.begin(), vec14.end(),
                                      [](int x)
                                      {
                                           return x % 2 == 0;
                                      });
          bool any_odd = std::any_of(vec14.begin(), vec14.end(),
                                     [](int x)
                                     {
                                          return x % 2 != 0;
                                     });
          bool none_odd = std::none_of(vec14.begin(), vec14.end(),
                                       [](int x)
                                       {
                                            return x % 2 != 0;
                                       });

          if (!all_even || any_odd || !none_odd)
          {
               return 14;
          }

          /* Test 15: std::accumulate */

          std::vector<int> vec15 = {1, 2, 3, 4, 5};
          int total = std::accumulate(vec15.begin(), vec15.end(), 0);

          if (total != 15)
          {
               return 15;
          }

          /* Test 16: std::binary_search */

          std::vector<int> vec16 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
          bool found = std::binary_search(vec16.begin(), vec16.end(), 5);
          bool not_found = std::binary_search(vec16.begin(), vec16.end(), 11);

          if (!found || not_found)
          {
               return 16;
          }

          /* Test 17: std::lower_bound / std::upper_bound */

          std::vector<int> vec17 = {1, 2, 2, 2, 3, 4, 5};
          auto lower = std::lower_bound(vec17.begin(), vec17.end(), 2);
          auto upper = std::upper_bound(vec17.begin(), vec17.end(), 2);

          if (std::distance(lower, upper) != 3)
          {
               return 17;
          }

          /* Test 18: std::equal */

          std::vector<int> vec18a = {1, 2, 3, 4, 5};
          std::vector<int> vec18b = {1, 2, 3, 4, 5};
          std::vector<int> vec18c = {1, 2, 3, 4, 6};

          if (!std::equal(vec18a.begin(), vec18a.end(), vec18b.begin()))
          {
               return 18;
          }
          if (std::equal(vec18a.begin(), vec18a.end(), vec18c.begin()))
          {
               return 19;
          }

          /* Test 19: std::mismatch */

          std::vector<int> vec19a = {1, 2, 3, 4, 5};
          std::vector<int> vec19b = {1, 2, 3, 5, 5};
          auto mismatch_pair = std::mismatch(vec19a.begin(), vec19a.end(), vec19b.begin());

          if (*mismatch_pair.first != 4 || *mismatch_pair.second != 5)
          {
               return 20;
          }

          /* Test 20: std::swap */

          int a = 10, b = 20;
          std::swap(a, b);

          if (a != 20 || b != 10)
          {
               return 21;
          }

          /* Test 21: std::min / std::max */

          int min_val = std::min(10, 20);
          int max_val = std::max(10, 20);

          if (min_val != 10 || max_val != 20)
          {
               return 22;
          }

          /* Test 22: std::partition */

          std::vector<int> vec22 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
          auto pivot = std::partition(vec22.begin(), vec22.end(),
                                      [](int x)
                                      {
                                           return x % 2 == 0;
                                      });

          bool all_even_before = std::all_of(vec22.begin(), pivot,
                                             [](int x)
                                             {
                                                  return x % 2 == 0;
                                             });
          bool all_odd_after = std::all_of(pivot, vec22.end(),
                                           [](int x)
                                           {
                                                return x % 2 != 0;
                                           });

          if (!all_even_before || !all_odd_after)
          {
               return 23;
          }

          /* Test 23: std::rotate */

          std::vector<int> vec23 = {1, 2, 3, 4, 5};
          std::rotate(vec23.begin(), vec23.begin() + 2, vec23.end());

          if (vec23[0] != 3 || vec23[4] != 2)
          {
               return 24;
          }

          /* Test 24: std::is_sorted */

          std::vector<int> vec24_sorted = {1, 2, 3, 4, 5};
          std::vector<int> vec24_unsorted = {5, 2, 3, 1, 4};

          if (!std::is_sorted(vec24_sorted.begin(), vec24_sorted.end()))
          {
               return 25;
          }
          if (std::is_sorted(vec24_unsorted.begin(), vec24_unsorted.end()))
          {
               return 26;
          }

          /* Test 25: std::adjacent_find */

          std::vector<int> vec25 = {1, 2, 3, 3, 4, 5};
          auto adj_it = std::adjacent_find(vec25.begin(), vec25.end());

          if (adj_it == vec25.end() || *adj_it != 3)
          {
               return 27;
          }

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
}
