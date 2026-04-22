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

#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"

/* Comment goes here */

class CoreExport Wildcard
{
   private:

     static bool MatchInternal(const char* Str, const char* Pattern);

   public:

     /*
      * Match a string against a wildcard pattern
      * @param Str The string to match
      * @param Pattern The wildcard pattern (* and ? supported)
      * @return true if the string matches the pattern
      */

     static bool Match(const std::string& Str, const std::string& Pattern);
     static bool MatchCaseSensitive(const std::string& Str, const std::string& Pattern);

     /*
      * Match a C-string against a wildcard pattern
      * @param Str The C-string to match
      * @param Pattern The wildcard pattern (* and ? supported)
      * @return true if the string matches the pattern
      */

     static bool Match(const char* Str, const char* Pattern);
     static bool MatchCaseSensitive(const char* Str, const char* Pattern);

     /*
      * Filter a vector of strings using wildcard pattern
      * @param Strings The vector of strings to filter
      * @param Pattern The wildcard pattern
      * @return vector of strings that match the pattern
      */

     static std::vector<std::string> Filter(const std::vector<std::string>& Strings, const std::string& Pattern);
};
