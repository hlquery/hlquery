/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <algorithm>
#include <cctype>
#include <cstring>

#include "utils/wildcard.h"

namespace
{

/*
      * Normalize patterns by collapsing consecutive '*'.
      */

std::string NormalizePattern(const std::string &Pattern)
{
     if (Pattern.empty())
     {
          return Pattern;
     }

     /* Quick check: if no consecutive stars, return original. */

     bool NeedsNormalization = false;

     for (size_t i = 0; i < Pattern.size() - 1; ++i)
     {
          if (Pattern[i] == '*' && Pattern[i + 1] == '*')
          {
               NeedsNormalization = true;
               break;
          }
     }

     if (!NeedsNormalization)
     {
          return Pattern;
     }

     std::string Normalized;

     Normalized.reserve(Pattern.size());

     bool LastWasStar = false;

     for (char c : Pattern)
     {
          if (c == '*')
          {
               if (!LastWasStar)
               {
                    Normalized.push_back(c);
                    LastWasStar = true;
               }
          }
          else
          {
               Normalized.push_back(c);
               LastWasStar = false;
          }
     }

     return Normalized;
}

/*
      * Compare ASCII characters case-insensitively.
      */

inline bool CharEqualCaseInsensitive(char a, char b)
{
     /* Fast path for ASCII: most common case. */

     if ((a | b) & 0x80)
     {
          /* Non-ASCII, fall back to tolower. */

          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
     }

     if (a == b)
     {
          return true;
     }

     return (a ^ b) == 0x20 && ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z'));
}

/*
      * Check for exact matches without wildcards.
      */

bool MatchExact(const char *Str, const char *Pattern)
{
     /* Try fast memcmp first (case-sensitive). */

     if (std::strcmp(Str, Pattern) == 0)
     {
          return true;
     }

     /* Fall back to case-insensitive comparison. */

     while (*Str && *Pattern)
     {
          if (!CharEqualCaseInsensitive(*Str, *Pattern))
          {
               return false;
          }

          Str++;
          Pattern++;
     }

     return *Str == *Pattern;
}

/*
      * Check prefix matches where the pattern ends with '*'.
      */

bool MatchPrefix(const char *Str, const char *Pattern, size_t PrefixLen)
{
     /* Fast path: try case-sensitive first. */

     if (std::memcmp(Str, Pattern, PrefixLen) == 0)
     {
          return true;
     }

     /* Fall back to case-insensitive comparison. */

     for (size_t i = 0; i < PrefixLen; ++i)
     {
          if (!CharEqualCaseInsensitive(Str[i], Pattern[i]))
          {
               return false;
          }
     }

     return true;
}

/*
      * Check suffix matches where the pattern starts with '*'.
      */

bool MatchSuffix(const char *Str, const char *Pattern, size_t StrLen, size_t SuffixLen)
{
     if (StrLen < SuffixLen)
     {
          return false;
     }

     const char *StrSuffix = Str + (StrLen - SuffixLen);

     const char *PatternSuffix = Pattern + 1;

     /* Fast path: try case-sensitive first. */

     if (std::memcmp(StrSuffix, PatternSuffix, SuffixLen) == 0)
     {
          return true;
     }

     /* Fall back to case-insensitive comparison. */

     for (size_t i = 0; i < SuffixLen; ++i)
     {
          if (!CharEqualCaseInsensitive(StrSuffix[i], PatternSuffix[i]))
          {
               return false;
          }
     }

     return true;
}

/*
      * Check substring matches for patterns like "*substring*".
      */

bool MatchContains(const char *Str, const char *Pattern, size_t StrLen, size_t SubstrLen)
{
     if (StrLen < SubstrLen)
     {
          return false;
     }

     const char *SubPattern = Pattern + 1;

     for (size_t i = 0; i <= StrLen - SubstrLen; ++i)
     {
          /* Fast path: try case-sensitive first. */

          if (std::memcmp(Str + i, SubPattern, SubstrLen) == 0)
          {
               return true;
          }

          /* Fall back to case-insensitive comparison. */

          bool Matches = true;

          for (size_t j = 0; j < SubstrLen; ++j)
          {
               if (!CharEqualCaseInsensitive(Str[i + j], SubPattern[j]))
               {
                    Matches = false;
                    break;
               }
          }

          if (Matches)
          {
               return true;
          }
     }

     return false;
}

/*
      * General wildcard match for patterns with '*' and '?'.
      */

bool MatchWildcard(const char *Str, const char *Pattern)
{
     const char *Star = nullptr;

     const char *StrCheckpoint = nullptr;

     while (*Str)
     {
          if (*Pattern == '*')
          {
               Star = Pattern++;
               StrCheckpoint = Str;
               continue;
          }

          if (*Pattern == '?' || CharEqualCaseInsensitive(*Str, *Pattern))
          {
               Str++;
               Pattern++;
               continue;
          }

          if (Star)
          {
               Pattern = Star + 1;
               Str = ++StrCheckpoint;
               continue;
          }

          return false;
     }

     while (*Pattern == '*')
     {
          Pattern++;
     }

     return *Pattern == '\0';
}

}/*
 * Check whether a string matches a wildcard pattern.
 */

bool Wildcard::Match(const std::string &Str, const std::string &Pattern)
{
     return MatchInternal(Str.c_str(), Pattern.c_str());
}

bool Wildcard::Match(const char *Str, const char *Pattern)
{
     if (!Str || !Pattern)
     {
          return false;
     }

     return MatchInternal(Str, Pattern);
}

bool Wildcard::MatchInternal(const char *Str, const char *Pattern)
{
     if (std::strcmp(Pattern, "*") == 0)
     {
          return true;
     }

     std::string PatternVal(Pattern);

     std::string Normalized = NormalizePattern(PatternVal);

     if (Normalized.find('*') == std::string::npos && Normalized.find('?') == std::string::npos)
     {
          return MatchExact(Str, Normalized.c_str());
     }

     size_t StrLen = std::strlen(Str);

     if (Normalized.front() == '*' && Normalized.back() == '*' && Normalized.size() > 2)
     {
          return MatchContains(Str, Normalized.c_str(), StrLen, Normalized.size() - 2);
     }

     if (Normalized.front() == '*' && Normalized.find_first_of("*?", 1) == std::string::npos)
     {
          return MatchSuffix(Str, Normalized.c_str(), StrLen, Normalized.size() - 1);
     }

     if (Normalized.back() == '*' && Normalized.find_first_of("*?", 0) == Normalized.size() - 1)
     {
          return MatchPrefix(Str, Normalized.c_str(), Normalized.size() - 1);
     }

     return MatchWildcard(Str, Normalized.c_str());
}

std::vector<std::string> Wildcard::Filter(const std::vector<std::string> &Strings, const std::string &Pattern)
{
     std::vector<std::string> Matches;

     Matches.reserve(Strings.size());

     for (const std::string &Value : Strings)
     {
          if (Match(Value, Pattern))
          {
               Matches.push_back(Value);
          }
     }

     return Matches;
}
