/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * Regular Expression Tests
 */

#include <iostream>
#include <regex>
#include <string>
#include <vector>

int test_regex()
{
     try
     {

          /* Test 1: Basic pattern matching */

          std::string text = "hlquery is a search engine";
          std::regex pattern("search");

          if (!std::regex_search(text, pattern))
          {
               return 1;
          }

          /* Test 2: Case-insensitive matching */

          std::regex pattern_case("SEARCH", std::regex_constants::icase);

          if (!std::regex_search(text, pattern_case))
          {
               return 2;
          }

          /* Test 3: Pattern with groups */

          std::string email = "user@hlquery.com";
          std::regex email_pattern(R"((\w+)@(\w+)\.(\w+))");
          std::smatch matches;

          if (!std::regex_search(email, matches, email_pattern))
          {
               return 3;
          }

          if (matches.size() != 4 || matches[1].str() != "user" ||
              matches[2].str() != "hlquery" || matches[3].str() != "com")
          {
               return 4;
          }

          /* Test 4: Replace operations */

          std::string source = "foo bar foo baz";
          std::regex foo_pattern("foo");
          std::string replaced =
               std::regex_replace(source, foo_pattern, "FOO");

          if (replaced != "FOO bar FOO baz")
          {
               return 5;
          }

          /* Test 5: Multiple matches */

          std::string numbers = "123 456 789";
          std::regex num_pattern(R"(\d+)");
          std::sregex_iterator iter(
               numbers.begin(), numbers.end(), num_pattern);
          std::sregex_iterator end;

          std::vector<std::string> found_numbers;

          for (; iter != end; ++iter)
          {
               found_numbers.push_back(iter->str());
          }

          if (found_numbers.size() != 3 || found_numbers[0] != "123" ||
              found_numbers[1] != "456" || found_numbers[2] != "789")
          {
               return 6;
          }

          /* Test 6: Complex pattern (URL validation) */

          std::string url = "https://www.hlquery.com/search?q=test";
          std::regex url_pattern(R"(https?://[\w\.-]+/[\w\?=&]*)");

          if (!std::regex_match(url, url_pattern))
          {
               return 7;
          }

          /* Test 7: Word boundaries */

          std::string sentence = "The cat in the cathedral";
          std::regex word_pattern(R"(\bcat\b)");

          auto word_iter = std::sregex_iterator(
               sentence.begin(), sentence.end(), word_pattern);
          int word_count = 0;

          for (; word_iter != std::sregex_iterator(); ++word_iter)
          {
               word_count++;
          }

          if (word_count != 1)
          { /* Should only match "cat", not "cat" in "cathedral" */
               return 8;
          }

          /* Test 8: Character classes */

          std::string mixed = "abc123DEF456ghi";
          std::regex digit_pattern(R"(\d+)");
          std::regex alpha_pattern(R"([a-zA-Z]+)");

          auto digit_iter = std::sregex_iterator(
               mixed.begin(), mixed.end(), digit_pattern);
          int digit_groups = 0;

          for (; digit_iter != std::sregex_iterator(); ++digit_iter)
          {
               digit_groups++;
          }

          if (digit_groups != 2)
          { /* Should find "123" and "456" */
               return 9;
          }

          /* Test 9: Quantifiers */

          std::string repeated = "aaabbbcccc";
          std::regex repeat_pattern(R"(a{3}b{3}c{4})");

          if (!std::regex_match(repeated, repeat_pattern))
          {
               return 10;
          }

          /* Test 10: Anchors */

          std::string line = "start middle end";
          std::regex start_pattern(R"(^start)");
          std::regex end_pattern(R"(end$)");

          if (!std::regex_search(line, start_pattern) ||
              !std::regex_search(line, end_pattern))
          {
               return 11;
          }

          return 0; /* Success */
     }
     catch (const std::regex_error& e)
     {
          return 90 + (e.code() % 10);
     }
     catch (...)
     {
          return 99;
     }
}
