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

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "api/searchapi.h"
#include "core/hlquery.h"
#include "vendor/json/json.hpp"

static void ApplyInlineQueryDirectives(std::string &QueryText, bool &CaseSensitive)
{
     if (QueryText.empty())
     {
          return;
     }

     std::stringstream Stream(QueryText);
     std::string Token;
     std::vector<std::string> KeptTokens;

     while (Stream >> Token)
     {
          std::string Lowered = Token;
          std::transform(Lowered.begin(), Lowered.end(), Lowered.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (Lowered == "do:casesensitive" || Lowered == "do:case_sensitive" || Lowered == "do:case-sensitive" ||
              Lowered == "is:casesensitive" || Lowered == "is:case_sensitive" || Lowered == "is:case-sensitive")
          {
               CaseSensitive = true;
               continue;
          }

          KeptTokens.push_back(Token);
     }

     std::ostringstream Rebuilt;
     for (std::size_t I = 0; I < KeptTokens.size(); ++I)
     {
          if (I > 0)
          {
               Rebuilt << ' ';
          }

          Rebuilt << KeptTokens[I];
     }

     QueryText = Rebuilt.str();
}

/* ParseMultiSearchRequest parses a multi-search request body. */

/*
 * SearchAPI::ParseMultiSearchRequest implementation.
 */

std::vector<std::pair<std::string, ComprehensiveSearchQuery>> SearchAPI::ParseMultiSearchRequest(const std::string &JSONBody)
{
     std::vector<std::pair<std::string, ComprehensiveSearchQuery>> SearchRequests;

     try
     {
          nlohmann::json RequestJSON = nlohmann::json::parse(JSONBody);

          if (!RequestJSON.contains("searches") || !RequestJSON["searches"].is_array())
          {
               return SearchRequests;
          }

          for (const auto &SearchObj : RequestJSON["searches"])
          {
               if (!SearchObj.is_object())
               {
                    continue;
               }

               std::string Collection;
               std::string QueryStr;

               if (SearchObj.contains("collection") && SearchObj["collection"].is_string())
               {
                    Collection = SearchObj["collection"].get<std::string>();
               }

               if (SearchObj.contains("q") && SearchObj["q"].is_string())
               {
                    QueryStr = SearchObj["q"].get<std::string>();
               }

               if (Collection.empty() || QueryStr.empty())
               {
                    continue;
               }

               ComprehensiveSearchQuery SearchQueryObj;

               SearchQueryObj.Q = QueryStr;

               if (SearchObj.contains("query_by"))
               {
                    if (SearchObj["query_by"].is_string())
                    {
                         SearchQueryObj.QueryBy = ParseCommaSeparated(SearchObj["query_by"].get<std::string>());
                    }
                    else if (SearchObj["query_by"].is_array())
                    {
                         for (const auto &Field : SearchObj["query_by"])
                         {
                              if (Field.is_string())
                              {
                                   SearchQueryObj.QueryBy.push_back(Field.get<std::string>());
                              }
                         }
                    }
               }

               if (SearchObj.contains("filter_by") && SearchObj["filter_by"].is_string())
               {
                    SearchQueryObj.FilterBy = SearchObj["filter_by"].get<std::string>();
               }

               if (SearchObj.contains("per_page"))
               {
                    try
                    {
                         if (SearchObj["per_page"].is_number())
                         {
                              SearchQueryObj.PerPage = SearchObj["per_page"].get<int>();
                         }
                         else if (SearchObj["per_page"].is_string())
                         {
                              SearchQueryObj.PerPage = std::stoi(SearchObj["per_page"].get<std::string>());
                         }

                         if (SearchQueryObj.PerPage < 1)
                         {
                              SearchQueryObj.PerPage = 1;
                         }

                         if (SearchQueryObj.PerPage > 1000)
                         {
                              SearchQueryObj.PerPage = 1000;
                         }
                    }
                    catch (...)
                    {
                         SearchQueryObj.PerPage = 10;
                    }
               }

               if (SearchObj.contains("page"))
               {
                    try
                    {
                         if (SearchObj["page"].is_number())
                         {
                              SearchQueryObj.Page = SearchObj["page"].get<int>();
                         }
                         else if (SearchObj["page"].is_string())
                         {
                              SearchQueryObj.Page = std::stoi(SearchObj["page"].get<std::string>());
                         }

                         if (SearchQueryObj.Page < 1)
                         {
                              SearchQueryObj.Page = 1;
                         }
                    }
                    catch (...)
                    {
                         SearchQueryObj.Page = 1;
                    }
               }

               if (SearchObj.contains("highlight"))
               {
                    if (SearchObj["highlight"].is_boolean())
                    {
                         SearchQueryObj.Highlight = SearchObj["highlight"].get<bool>();
                    }
                    else if (SearchObj["highlight"].is_string())
                    {
                         SearchQueryObj.Highlight = (SearchObj["highlight"].get<std::string>() == "true");
                    }
               }

               if (SearchObj.contains("prefix"))
               {
                    if (SearchObj["prefix"].is_boolean())
                    {
                         SearchQueryObj.Prefix = SearchObj["prefix"].get<bool>();
                    }
                    else if (SearchObj["prefix"].is_string())
                    {
                         std::string PrefixValue = SearchObj["prefix"].get<std::string>();
                         std::transform(PrefixValue.begin(), PrefixValue.end(), PrefixValue.begin(),
                                        [](unsigned char C)
                                        {
                                             return static_cast<char>(std::tolower(C));
                                        });
                         SearchQueryObj.Prefix = (PrefixValue == "true" || PrefixValue == "1" || PrefixValue == "yes" || PrefixValue == "on");
                    }
               }

               if (SearchObj.contains("num_typos"))
               {
                    try
                    {
                         if (SearchObj["num_typos"].is_number_integer())
                         {
                              SearchQueryObj.NumTypos = SearchObj["num_typos"].get<int>();
                         }
                         else if (SearchObj["num_typos"].is_string())
                         {
                              SearchQueryObj.NumTypos = std::stoi(SearchObj["num_typos"].get<std::string>());
                         }

                         if (SearchQueryObj.NumTypos < 0)
                         {
                              SearchQueryObj.NumTypos = 0;
                         }
                         if (SearchQueryObj.NumTypos > 4)
                         {
                              SearchQueryObj.NumTypos = 4;
                         }

                         SearchQueryObj.NumTyposExplicit = true;
                    }
                    catch (...)
                    {
                         SearchQueryObj.NumTypos = 2;
                    }
               }

               if (SearchObj.contains("typo_tokens_threshold"))
               {
                    try
                    {
                         if (SearchObj["typo_tokens_threshold"].is_number_integer())
                         {
                              SearchQueryObj.TypoTokensThreshold = SearchObj["typo_tokens_threshold"].get<int>();
                         }
                         else if (SearchObj["typo_tokens_threshold"].is_string())
                         {
                              SearchQueryObj.TypoTokensThreshold = std::stoi(SearchObj["typo_tokens_threshold"].get<std::string>());
                         }
                    }
                    catch (...)
                    {
                         SearchQueryObj.TypoTokensThreshold = 2;
                    }
               }

               SearchRequests.push_back({Collection, SearchQueryObj});
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "ParseMultiSearchRequest: Failed to parse JSON: " + std::string(E.what()) + ".");
          }
     }

     return SearchRequests;
}

/* ParseCommaSeparated splits a string by comma and trims values. */

/*
 * SearchAPI::ParseCommaSeparated implementation.
 */

std::vector<std::string> SearchAPI::ParseCommaSeparated(const std::string &Input)
{
     std::vector<std::string> Result;

     if (Input.empty())
     {
          return Result;
     }

     const size_t MaxInputSize = 1024 * 1024;

     if (Input.size() > MaxInputSize)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "ParseCommaSeparated: Input too large, truncating.");
          }

          std::string Truncated = Input.substr(0, MaxInputSize);

          return ParseCommaSeparated(Truncated);
     }

     std::stringstream SS(Input);
     std::string Item;

     const size_t MaxParts = 10000;
     size_t PartCount = 0;

     while (std::getline(SS, Item, ',') && PartCount < MaxParts)
     {
          PartCount++;

          size_t FirstNonWS = Item.find_first_not_of(" \t\n\r\f\v");

          if (FirstNonWS != std::string::npos)
          {
               Item.erase(0, FirstNonWS);

               size_t LastNonWS = Item.find_last_not_of(" \t\n\r\f\v");

               if (LastNonWS != std::string::npos)
               {
                    Item.erase(LastNonWS + 1);
               }
               else
               {
                    Item.clear();
               }
          }
          else
          {
               Item.clear();
          }

          if (!Item.empty())
          {
               Result.push_back(std::move(Item));
          }
     }

     return Result;
}

/* ParseFilters parses filter string into FilterCondition objects. */

/*
 * SearchAPI::ParseFilters implementation.
 */

std::vector<FilterCondition> SearchAPI::ParseFilters(const std::string &FilterString)
{
     std::vector<FilterCondition> Filters;

     if (FilterString.empty())
     {
          return Filters;
     }

     auto Trim = [](std::string Value) -> std::string
     {
          const std::size_t First = Value.find_first_not_of(" \t\r\n");
          if (First == std::string::npos)
          {
               return "";
          }

          const std::size_t Last = Value.find_last_not_of(" \t\r\n");
          return Value.substr(First, Last - First + 1);
     };

     auto SkipWhitespace = [&FilterString](std::size_t &Pos)
     {
          while (Pos < FilterString.size() && std::isspace(static_cast<unsigned char>(FilterString[Pos])))
          {
               ++Pos;
          }
     };

     std::size_t Pos = 0;
     std::string NextConnector;

     while (Pos < FilterString.size())
     {
          SkipWhitespace(Pos);
          if (Pos >= FilterString.size())
          {
               break;
          }

          const std::size_t FieldStart = Pos;
          while (Pos < FilterString.size() &&
                 (std::isalnum(static_cast<unsigned char>(FilterString[Pos])) || FilterString[Pos] == '_'))
          {
               ++Pos;
          }

          const std::string Field = FilterString.substr(FieldStart, Pos - FieldStart);
          SkipWhitespace(Pos);

          if (Field.empty() || Pos >= FilterString.size() || FilterString[Pos] != ':')
          {
               break;
          }

          ++Pos;
          SkipWhitespace(Pos);

          FilterCondition Filter;
          Filter.Field = Field;
          Filter.LogicalConnector = NextConnector;
          NextConnector.clear();

          if (Pos < FilterString.size() && (FilterString[Pos] == '[' || FilterString[Pos] == '{'))
          {
               const char Open = FilterString[Pos++];
               const char Close = (Open == '[') ? ']' : '}';
               const std::size_t RangeStart = Pos;

               while (Pos < FilterString.size() && FilterString[Pos] != Close)
               {
                    ++Pos;
               }

               if (Pos >= FilterString.size())
               {
                    break;
               }

               const std::string RangeBody = Trim(FilterString.substr(RangeStart, Pos - RangeStart));
               ++Pos;

               const std::size_t ToPos = RangeBody.find(" TO ");
               if (ToPos == std::string::npos)
               {
                    break;
               }

               Filter.Op = (Open == '[') ? "RANGE_INCLUSIVE" : "RANGE_EXCLUSIVE";
               Filter.Value = Trim(RangeBody.substr(0, ToPos)) + "," + Trim(RangeBody.substr(ToPos + 4));
          }
          else
          {
               if (FilterString.compare(Pos, 6, "ISNULL") == 0)
               {
                    Filter.Op = "ISNULL";
                    Pos += 6;
               }
               else if (FilterString.compare(Pos, 9, "ISNOTNULL") == 0)
               {
                    Filter.Op = "ISNOTNULL";
                    Pos += 9;
               }
               else if (FilterString.compare(Pos, 10, "NOT_ILIKE:") == 0)
               {
                    Filter.Op = "NOT_ILIKE";
                    Pos += 10;
               }
               else if (FilterString.compare(Pos, 9, "NOT_LIKE:") == 0)
               {
                    Filter.Op = "NOT_LIKE";
                    Pos += 9;
               }
               else if (FilterString.compare(Pos, 6, "ILIKE:") == 0)
               {
                    Filter.Op = "ILIKE";
                    Pos += 6;
               }
               else if (FilterString.compare(Pos, 5, "LIKE:") == 0)
               {
                    Filter.Op = "LIKE";
                    Pos += 5;
               }

               if (Pos + 1 < FilterString.size())
               {
                    const std::string TwoCharOp = FilterString.substr(Pos, 2);
                    if (TwoCharOp == "!=" || TwoCharOp == ">=" || TwoCharOp == "<=" || TwoCharOp == ":=")
                    {
                         Filter.Op = TwoCharOp;
                         Pos += 2;
                    }
               }

               if (Filter.Op.empty() && Pos < FilterString.size() &&
                   (FilterString[Pos] == '>' || FilterString[Pos] == '<' || FilterString[Pos] == '='))
               {
                    Filter.Op = FilterString.substr(Pos, 1);
                    ++Pos;
               }

               if (Filter.Op.empty())
               {
                    Filter.Op = "=";
               }

               if (Filter.Op != "ISNULL" && Filter.Op != "ISNOTNULL")
               {
                    SkipWhitespace(Pos);

                    const std::size_t ValueStart = Pos;
                    while (Pos < FilterString.size())
                    {
                         if (Pos + 1 < FilterString.size())
                         {
                              const std::string Connector = FilterString.substr(Pos, 2);
                              if (Connector == "&&" || Connector == "||")
                              {
                                   break;
                              }
                         }

                         ++Pos;
                    }

                    Filter.Value = Trim(FilterString.substr(ValueStart, Pos - ValueStart));
               }
          }

          if (!Filter.Field.empty() && !Filter.Op.empty() &&
              (!Filter.Value.empty() || Filter.Op == "ISNULL" || Filter.Op == "ISNOTNULL"))
          {
               Filters.push_back(Filter);
          }

          SkipWhitespace(Pos);
          if (Pos + 1 < FilterString.size())
          {
               const std::string Connector = FilterString.substr(Pos, 2);
               if (Connector == "&&")
               {
                    NextConnector = "AND";
                    Pos += 2;
               }
               else if (Connector == "||")
               {
                    NextConnector = "OR";
                    Pos += 2;
               }
          }
     }

     return Filters;
}

/* ParseSortFields parses sort string. */

/*
 * SearchAPI::ParseSortFields implementation.
 */

std::vector<SearchAPI::SortField> SearchAPI::ParseSortFields(const std::string &SortString)
{
     std::vector<SortField> SortFields;

     if (SortString.empty())
     {
          return SortFields;
     }

     std::stringstream SS(SortString);
     std::string Item;

     while (std::getline(SS, Item, ','))
     {
          Item.erase(0, Item.find_first_not_of(" \t"));
          Item.erase(Item.find_last_not_of(" \t") + 1);

          if (!Item.empty())
          {
               SortField SortFieldVal;
               size_t ColonPos = Item.find(':');

               if (ColonPos != std::string::npos)
               {
                    SortFieldVal.Field = Item.substr(0, ColonPos);

                    std::string Direction = Item.substr(ColonPos + 1);

                    if (Direction == "asc" || Direction == "ascending")
                    {
                         SortFieldVal.Direction = "asc";
                    }
                    else if (Direction == "desc" || Direction == "descending")
                    {
                         SortFieldVal.Direction = "desc";
                    }
                    else
                    {
                         SortFieldVal.Direction = "asc";
                    }
               }
               else
               {
                    SortFieldVal.Field = Item;
                    SortFieldVal.Direction = "asc";
               }

               SortFields.push_back(SortFieldVal);
          }
     }

     return SortFields;
}

/* ParseSearchParamsFromJSON extracts parameters from JSON body. */

/*
 * SearchAPI::ParseSearchParamsFromJSON implementation.
 */

std::unordered_map<std::string, std::string> SearchAPI::ParseSearchParamsFromJSON(const std::string &Json)
{
     std::unordered_map<std::string, std::string> Params;
     nlohmann::json JsonBodyObj = nlohmann::json::parse(Json, nullptr, true, true);

     for (const auto &[Key, Value] : JsonBodyObj.items())
     {
          if (Value.is_string())
          {
               std::string StrValue = Value.get<std::string>();

               Params[Key] = StrValue;

               if (Key == "q" && Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "ParseSearchParamsFromJSON: Extracted query='" + StrValue + "'.");
               }
          }
          else if (Value.is_number())
          {
               Params[Key] = std::to_string(Value.get<double>());
          }
          else if (Value.is_boolean())
          {
               Params[Key] = Value.get<bool>() ? "true" : "false";
          }
          else
          {
               Params[Key] = Value.dump();
          }
     }

     return Params;
}

/* ParseComprehensiveSearchQuery builds a search query object from params. */

/*
 * SearchAPI::ParseComprehensiveSearchQuery implementation.
 */

ComprehensiveSearchQuery SearchAPI::ParseComprehensiveSearchQuery(const std::unordered_map<std::string, std::string> &Params)
{
     ComprehensiveSearchQuery QueryObj;

     auto ParseBool = [](const std::string &Val, bool DefaultVal = false) -> bool
     {
          if (Val.empty())
          {
               return DefaultVal;
          }

          std::string Lower = Val;
          std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (Lower == "1" || Lower == "true" || Lower == "yes" || Lower == "on")
          {
               return true;
          }

          if (Lower == "0" || Lower == "false" || Lower == "no" || Lower == "off")
          {
               return false;
          }

          return DefaultVal;
     };

     if (Params.count("q"))
     {
          QueryObj.Q = Params.at("q");
     }

     if (Params.count("case_sensitive"))
     {
          QueryObj.CaseSensitive = ParseBool(Params.at("case_sensitive"), false);
     }

     ApplyInlineQueryDirectives(QueryObj.Q, QueryObj.CaseSensitive);

     if (Params.count("query_by"))
     {
          std::string QueryByValue = Params.at("query_by");

          if (QueryByValue != "*" && QueryByValue != "*," && QueryByValue != ",*")
          {
               std::vector<std::string> Fields = ParseCommaSeparated(QueryByValue);
               bool HasWildcard = false;

               for (const auto &Field : Fields)
               {
                    if (Field == "*")
                    {
                         HasWildcard = true;
                         break;
                    }
               }

               if (!HasWildcard)
               {
                    QueryObj.QueryBy = Fields;
               }
          }
     }

     if (Params.count("filter_by"))
     {
          QueryObj.FilterBy = Params.at("filter_by");
     }

     if (Params.count("facet_by"))
     {
          QueryObj.FacetBy = ParseCommaSeparated(Params.at("facet_by"));
     }

     if (Params.count("facet_query"))
     {
          try
          {
               nlohmann::json FacetQueryJSON = nlohmann::json::parse(Params.at("facet_query"), nullptr, true, true);
               if (FacetQueryJSON.is_object())
               {
                    for (auto It = FacetQueryJSON.begin(); It != FacetQueryJSON.end(); ++It)
                    {
                         if (It.value().is_string())
                         {
                              QueryObj.FacetQuery[It.key()] = It.value().get<std::string>();
                         }
                    }
               }
          }
          catch (...)
          {
               /* Invalid facet_query JSON. Keep defaults. */
          }
     }

     if (Params.count("max_facet_values"))
     {
          try
          {
               QueryObj.MaxFacetValues = std::stoi(Params.at("max_facet_values"));
               if (QueryObj.MaxFacetValues < 1)
               {
                    QueryObj.MaxFacetValues = 1;
               }
               if (QueryObj.MaxFacetValues > 1000)
               {
                    QueryObj.MaxFacetValues = 1000;
               }
          }
          catch (...)
          {
               QueryObj.MaxFacetValues = 10;
          }
     }

     if (Params.count("per_page"))
     {
          try
          {
               QueryObj.PerPage = std::stoi(Params.at("per_page"));

               if (QueryObj.PerPage < 1)
               {
                    QueryObj.PerPage = 1;
               }

               if (QueryObj.PerPage > 1000)
               {
                    QueryObj.PerPage = 1000;
               }
          }
          catch (...)
          {
               QueryObj.PerPage = 10;
          }
     }

     if (Params.count("page"))
     {
          try
          {
               QueryObj.Page = std::stoi(Params.at("page"));

               if (QueryObj.Page < 1)
               {
                    QueryObj.Page = 1;
               }
          }
          catch (...)
          {
               QueryObj.Page = 1;
          }
     }

     if (Params.count("limit"))
     {
          try
          {
               QueryObj.PerPage = std::stoi(Params.at("limit"));

               if (QueryObj.PerPage < 1)
               {
                    QueryObj.PerPage = 1;
               }

               if (QueryObj.PerPage > 1000)
               {
                    QueryObj.PerPage = 1000;
               }
          }
          catch (...)
          {
               QueryObj.PerPage = 10;
          }
     }

     if (Params.count("offset"))
     {
          try
          {
               int Offset = std::stoi(Params.at("offset"));

               if (Offset < 0)
               {
                    Offset = 0;
               }

               QueryObj.Offset = Offset;

               if (QueryObj.PerPage > 0)
               {
                    QueryObj.Page = (Offset / QueryObj.PerPage) + 1;
               }
          }
          catch (...)
          {
               QueryObj.Offset = 0;
               QueryObj.Page = 1;
          }
     }

     if (QueryObj.PerPage <= 0)
     {
          QueryObj.PerPage = 10;
     }

     if (Params.count("sort_by"))
     {
          QueryObj.SortBy = ParseCommaSeparated(Params.at("sort_by"));
     }

     if (Params.count("highlight"))
     {
          QueryObj.Highlight = (Params.at("highlight") == "true");
     }
     else if (!QueryObj.Q.empty())
     {
          QueryObj.Highlight = true;
     }

     if (Params.count("highlight_fields"))
     {
          QueryObj.HighlightFields = ParseCommaSeparated(Params.at("highlight_fields"));
     }

     if (Params.count("highlight_full_fields"))
     {
          QueryObj.HighlightFullFields = Params.at("highlight_full_fields");
     }

     if (Params.count("include_fields"))
     {
          QueryObj.IncludeFields = ParseCommaSeparated(Params.at("include_fields"));
     }

     if (Params.count("exclude_fields"))
     {
          QueryObj.ExcludeFields = ParseCommaSeparated(Params.at("exclude_fields"));
     }

     if (Params.count("group_by"))
     {
          QueryObj.GroupBy = ParseCommaSeparated(Params.at("group_by"));
     }

     if (Params.count("group_limit"))
     {
          try
          {
               QueryObj.GroupLimit = std::stoi(Params.at("group_limit"));
               if (QueryObj.GroupLimit < 1)
               {
                    QueryObj.GroupLimit = 1;
               }
               if (QueryObj.GroupLimit > 1000)
               {
                    QueryObj.GroupLimit = 1000;
               }
          }
          catch (...)
          {
               QueryObj.GroupLimit = 3;
          }
     }

     if (Params.count("num_typos"))
     {
          QueryObj.NumTyposExplicit = true;
          try
          {
               QueryObj.NumTypos = std::stoi(Params.at("num_typos"));
               if (QueryObj.NumTypos < 0)
               {
                    QueryObj.NumTypos = 0;
               }
               if (QueryObj.NumTypos > 4)
               {
                    QueryObj.NumTypos = 4;
               }
          }
          catch (...)
          {
               QueryObj.NumTypos = 2;
          }
     }

     if (Params.count("drop_tokens_threshold"))
     {
          try
          {
               QueryObj.DropTokensThreshold = std::stoi(Params.at("drop_tokens_threshold"));
          }
          catch (...)
          {
               QueryObj.DropTokensThreshold = 0;
          }
     }

     if (Params.count("typo_tokens_threshold"))
     {
          try
          {
               QueryObj.TypoTokensThreshold = std::stoi(Params.at("typo_tokens_threshold"));
          }
          catch (...)
          {
               QueryObj.TypoTokensThreshold = 2;
          }
     }

     if (Params.count("prefix"))
     {
          QueryObj.Prefix = ParseBool(Params.at("prefix"), false);
     }

     if (Params.count("prioritize_exact_match"))
     {
          QueryObj.PrioritizeExactMatch = ParseBool(Params.at("prioritize_exact_match"), true);
     }

     if (Params.count("exhaustive_search"))
     {
          QueryObj.ExhaustiveSearch = ParseBool(Params.at("exhaustive_search"), false);
     }

     if (Params.count("aggregations"))
     {
          QueryObj.Aggregations = Params.at("aggregations");
     }

     if (Params.count("include_created_at"))
     {
          QueryObj.IncludeCreatedAt = ParseBool(Params.at("include_created_at"), false);
     }

     if (Params.count("include_distance"))
     {
          QueryObj.IncludeVectorDistance = ParseBool(Params.at("include_distance"), false);
     }

     if (Params.count("hybrid_alpha"))
     {
          try
          {
               float Alpha = std::stof(Params.at("hybrid_alpha"));

               QueryObj.HybridAlpha = std::max(0.0f, std::min(1.0f, Alpha));
          }
          catch (...)
          {
               QueryObj.HybridAlpha = 0.5f;
          }
     }

     if (Params.count("vector_query"))
     {
          QueryObj.VectorQueryStr = Params.at("vector_query");
     }
     else if (Params.count("vector"))
     {
          QueryObj.VectorQueryStr = Params.at("vector");
     }
     else if (Params.count("vector_queries"))
     {
          QueryObj.VectorQueryStr = Params.at("vector_queries");
     }
     else if (Params.count("embedding"))
     {
          QueryObj.Embedding = Params.at("embedding");
     }

     return QueryObj;
}
