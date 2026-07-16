/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

struct LexicalSortOptions
{
     std::string SortBy;
     std::string SortOrder;
};

inline std::string NormalizeLexicalSortValue(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     std::string Result = Value.substr(Start, End - Start + 1);
     std::transform(Result.begin(), Result.end(), Result.begin(),
                    [](unsigned char Ch)
                    {
                         return static_cast<char>(std::tolower(Ch));
                    });

     return Result;
}

inline bool ResolveLexicalSortOptions(const std::map<std::string, std::string> &QueryParams,
                                      const std::string &DefaultSortBy,
                                      const std::vector<std::string> &AllowedSortFields,
                                      LexicalSortOptions *OutOptions,
                                      std::string *OutError)
{
     if (OutOptions == nullptr)
     {
          return false;
     }

     const auto SortByIt = QueryParams.find("sort_by");
     const auto SortOrderIt = QueryParams.find("sort_order");
     OutOptions->SortBy = SortByIt != QueryParams.end() ? NormalizeLexicalSortValue(SortByIt->second) : DefaultSortBy;
     OutOptions->SortOrder = SortOrderIt != QueryParams.end() ? NormalizeLexicalSortValue(SortOrderIt->second) : "asc";

     if (std::find(AllowedSortFields.begin(), AllowedSortFields.end(), OutOptions->SortBy) == AllowedSortFields.end())
     {
          if (OutError != nullptr)
          {
               *OutError = "Parameter 'sort_by' must be one of the supported lexical sort fields.";
          }
          return false;
     }

     if (OutOptions->SortOrder != "asc" && OutOptions->SortOrder != "desc")
     {
          if (OutError != nullptr)
          {
               *OutError = "Parameter 'sort_order' must be 'asc' or 'desc'.";
          }

          return false;
     }

     return true;
}

inline bool CompareLexicalSortValues(const std::string &LeftValue,
                                     const std::string &RightValue,
                                     const std::string &LeftTieBreaker,
                                     const std::string &RightTieBreaker,
                                     const std::string &SortOrder)
{
     if (LeftValue != RightValue)
     {
          return SortOrder == "asc" ? LeftValue < RightValue : LeftValue > RightValue;
     }

     return SortOrder == "asc" ? LeftTieBreaker < RightTieBreaker : LeftTieBreaker > RightTieBreaker;
}
