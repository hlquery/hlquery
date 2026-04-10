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
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <ctime>

#include "api/searchapi.h"
#include "core/hlquery.h"
#include "utils/wildcard.h"

namespace
{
bool TryParseISO8601TimestampToMs(const std::string &value, std::uint64_t *parsed_ms)
{
     if (!parsed_ms || value.empty())
     {
          return false;
     }

     std::string trimmed = value;
     const std::size_t first = trimmed.find_first_not_of(" \t\r\n");
     if (first == std::string::npos)
     {
          return false;
     }
     const std::size_t last = trimmed.find_last_not_of(" \t\r\n");
     trimmed = trimmed.substr(first, last - first + 1);

     if (!trimmed.empty() && (trimmed.front() == '\'' || trimmed.front() == '"') &&
         trimmed.back() == trimmed.front() && trimmed.size() >= 2)
     {
          trimmed = trimmed.substr(1, trimmed.size() - 2);
     }

     if (trimmed.size() < 19)
     {
          return false;
     }

     std::string working = trimmed;
     bool has_timezone = false;
     if (!working.empty() && (working.back() == 'Z' || working.back() == 'z'))
     {
          has_timezone = true;
          working.pop_back();
     }

     int fractional_ms = 0;
     const std::size_t dot_pos = working.find('.');
     if (dot_pos != std::string::npos)
     {
          std::string fractional = working.substr(dot_pos + 1);
          working = working.substr(0, dot_pos);

          if (fractional.size() > 3)
          {
               fractional = fractional.substr(0, 3);
          }
          while (fractional.size() < 3)
          {
               fractional.push_back('0');
          }

          for (char ch : fractional)
          {
               if (!std::isdigit(static_cast<unsigned char>(ch)))
               {
                    return false;
               }
          }

          fractional_ms = std::stoi(fractional);
     }

     if (working.size() != 19)
     {
          return false;
     }

     std::tm tm_value = {};
     std::istringstream iss(working);
     iss >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
     if (iss.fail())
     {
          return false;
     }

#if defined(_GNU_SOURCE) || defined(__USE_MISC) || defined(__APPLE__)
     time_t seconds = timegm(&tm_value);
#else
     char *old_tz = getenv("TZ");
     std::string old_tz_value = old_tz ? old_tz : "";
     setenv("TZ", "UTC", 1);
     tzset();
     time_t seconds = std::mktime(&tm_value);
     if (!old_tz_value.empty())
     {
          setenv("TZ", old_tz_value.c_str(), 1);
     }
     else
     {
          unsetenv("TZ");
     }
     tzset();
#endif

     if (seconds < 0)
     {
          return false;
     }

     *parsed_ms = static_cast<std::uint64_t>(seconds) * 1000ULL + static_cast<std::uint64_t>(fractional_ms);
     return has_timezone || trimmed.size() >= 19;
}

std::string FormatTimestampMsAsISO8601(std::uint64_t timestamp_ms)
{
     auto time_point = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
     time_t time_t_val = std::chrono::system_clock::to_time_t(time_point);
     const long long ms_value = static_cast<long long>(timestamp_ms % 1000ULL);

     struct tm tm_buf;
     struct tm *tm_ptr = gmtime_r(&time_t_val, &tm_buf);
     if (!tm_ptr)
     {
          return "";
     }

     std::ostringstream oss;
     oss << std::put_time(tm_ptr, "%Y-%m-%dT%H:%M:%S");
     oss << '.' << std::setfill('0') << std::setw(3) << ms_value << 'Z';
     return oss.str();
}

struct FilterExpressionNode
{
     enum class Type
     {
          Condition,
          Not,
          And,
          Or
     };

     Type NodeType = Type::Condition;
     FilterCondition Condition;
     std::unique_ptr<FilterExpressionNode> Left;
     std::unique_ptr<FilterExpressionNode> Right;
};

class FilterExpressionParser
{
   public:
     explicit FilterExpressionParser(const std::string &input)
          : Input(input)
     {
     }

     bool Parse(std::unique_ptr<FilterExpressionNode> &root)
     {
          if (!ParseOr(root))
          {
               return false;
          }

          SkipWhitespace();
          return Position >= Input.size();
     }

   private:
     const std::string &Input;
     size_t Position = 0;

     static std::string Trim(std::string value)
     {
          const std::size_t first = value.find_first_not_of(" \t\r\n");
          if (first == std::string::npos)
          {
               return "";
          }

          const std::size_t last = value.find_last_not_of(" \t\r\n");
          return value.substr(first, last - first + 1);
     }

     void SkipWhitespace()
     {
          while (Position < Input.size() && std::isspace(static_cast<unsigned char>(Input[Position])) != 0)
          {
               ++Position;
          }
     }

     bool Match(const std::string &token)
     {
          SkipWhitespace();

          if (Input.compare(Position, token.size(), token) != 0)
          {
               return false;
          }

          Position += token.size();
          return true;
     }

     bool ParseOr(std::unique_ptr<FilterExpressionNode> &node)
     {
          if (!ParseAnd(node))
          {
               return false;
          }

          while (true)
          {
               const size_t saved = Position;
               if (!Match("||"))
               {
                    Position = saved;
                    return true;
               }

               std::unique_ptr<FilterExpressionNode> rhs;
               if (!ParseAnd(rhs))
               {
                    return false;
               }

               auto combined = std::make_unique<FilterExpressionNode>();
               combined->NodeType = FilterExpressionNode::Type::Or;
               combined->Left = std::move(node);
               combined->Right = std::move(rhs);
               node = std::move(combined);
          }
     }

     bool ParseAnd(std::unique_ptr<FilterExpressionNode> &node)
     {
          if (!ParsePrimary(node))
          {
               return false;
          }

          while (true)
          {
               const size_t saved = Position;
               if (!Match("&&"))
               {
                    Position = saved;
                    return true;
               }

               std::unique_ptr<FilterExpressionNode> rhs;
               if (!ParsePrimary(rhs))
               {
                    return false;
               }

               auto combined = std::make_unique<FilterExpressionNode>();
               combined->NodeType = FilterExpressionNode::Type::And;
               combined->Left = std::move(node);
               combined->Right = std::move(rhs);
               node = std::move(combined);
          }
     }

     bool ParsePrimary(std::unique_ptr<FilterExpressionNode> &node)
     {
          SkipWhitespace();

          if (Position < Input.size() && Input[Position] == '!')
          {
               ++Position;

               std::unique_ptr<FilterExpressionNode> child;
               if (!ParsePrimary(child))
               {
                    return false;
               }

               node = std::make_unique<FilterExpressionNode>();
               node->NodeType = FilterExpressionNode::Type::Not;
               node->Left = std::move(child);
               return true;
          }

          if (Position < Input.size() && Input[Position] == '(')
          {
               ++Position;

               if (!ParseOr(node))
               {
                    return false;
               }

               SkipWhitespace();
               if (Position >= Input.size() || Input[Position] != ')')
               {
                    return false;
               }

               ++Position;
               return true;
          }

          FilterCondition condition;
          if (!ParseCondition(condition))
          {
               return false;
          }

          node = std::make_unique<FilterExpressionNode>();
          node->NodeType = FilterExpressionNode::Type::Condition;
          node->Condition = std::move(condition);
          return true;
     }

     bool ParseCondition(FilterCondition &filter)
     {
          SkipWhitespace();

          const size_t field_start = Position;
          while (Position < Input.size() &&
                 (std::isalnum(static_cast<unsigned char>(Input[Position])) != 0 || Input[Position] == '_'))
          {
               ++Position;
          }

          filter.Field = Input.substr(field_start, Position - field_start);
          SkipWhitespace();

          if (filter.Field.empty() || Position >= Input.size() || Input[Position] != ':')
          {
               return false;
          }

          ++Position;
          SkipWhitespace();

          if (Position < Input.size() && (Input[Position] == '[' || Input[Position] == '{'))
          {
               const char open = Input[Position++];
               const char close = open == '[' ? ']' : '}';
               const size_t range_start = Position;

               while (Position < Input.size() && Input[Position] != close)
               {
                    ++Position;
               }

               if (Position >= Input.size())
               {
                    return false;
               }

               const std::string range_body = Trim(Input.substr(range_start, Position - range_start));
               ++Position;

               const size_t to_pos = range_body.find(" TO ");
               if (to_pos == std::string::npos)
               {
                    return false;
               }

               filter.Op = (open == '[') ? "RANGE_INCLUSIVE" : "RANGE_EXCLUSIVE";
               filter.Value = Trim(range_body.substr(0, to_pos)) + "," + Trim(range_body.substr(to_pos + 4));
               return true;
          }

          if (Input.compare(Position, 6, "ISNULL") == 0)
          {
               filter.Op = "ISNULL";
               Position += 6;
               return true;
          }

          if (Input.compare(Position, 9, "ISNOTNULL") == 0)
          {
               filter.Op = "ISNOTNULL";
               Position += 9;
               return true;
          }

          if (Input.compare(Position, 10, "NOT_ILIKE:") == 0)
          {
               filter.Op = "NOT_ILIKE";
               Position += 10;
          }
          else if (Input.compare(Position, 9, "NOT_LIKE:") == 0)
          {
               filter.Op = "NOT_LIKE";
               Position += 9;
          }
          else if (Input.compare(Position, 6, "ILIKE:") == 0)
          {
               filter.Op = "ILIKE";
               Position += 6;
          }
          else if (Input.compare(Position, 5, "LIKE:") == 0)
          {
               filter.Op = "LIKE";
               Position += 5;
          }

          if (filter.Op.empty() && Position + 1 < Input.size())
          {
               const std::string two_char_op = Input.substr(Position, 2);
               if (two_char_op == "!=" || two_char_op == ">=" || two_char_op == "<=" || two_char_op == ":=")
               {
                    filter.Op = two_char_op;
                    Position += 2;
               }
          }

          if (filter.Op.empty() && Position < Input.size() &&
              (Input[Position] == '>' || Input[Position] == '<' || Input[Position] == '='))
          {
               filter.Op = Input.substr(Position, 1);
               ++Position;
          }

          if (filter.Op.empty())
          {
               filter.Op = "=";
          }

          SkipWhitespace();

          const size_t value_start = Position;
          while (Position < Input.size())
          {
               if (Input[Position] == ')')
               {
                    break;
               }

               if (Position + 1 < Input.size())
               {
                    const std::string connector = Input.substr(Position, 2);
                    if (connector == "&&" || connector == "||")
                    {
                         break;
                    }
               }

               ++Position;
          }

          filter.Value = Trim(Input.substr(value_start, Position - value_start));
          return !filter.Value.empty();
     }
};

}

/* ApplyFilters filters search hits based on filter conditions. */

std::vector<SearchHit> SearchAPI::ApplyFilters(const std::vector<SearchHit> &Hits, const std::string &FilterBy)
{
     std::vector<SearchHit> FilteredHits;
     std::unique_ptr<FilterExpressionNode> ExpressionRoot;
     FilterExpressionParser Parser(FilterBy);

     if (!Parser.Parse(ExpressionRoot) || !ExpressionRoot)
     {
          auto Conditions = ParseFilters(FilterBy);
          if (Conditions.empty())
          {
               return Hits;
          }

          for (const auto &HitObj : Hits)
          {
               bool Matches = false;

               for (std::size_t Index = 0; Index < Conditions.size(); ++Index)
               {
                    const auto &Condition = Conditions[Index];
                    const bool ConditionMatches = EvaluateFilterCondition(HitObj.Document, Condition);

                    if (Index == 0)
                    {
                         Matches = ConditionMatches;
                         continue;
                    }

                    if (Condition.LogicalConnector == "OR")
                    {
                         Matches = Matches || ConditionMatches;
                    }
                    else
                    {
                         Matches = Matches && ConditionMatches;
                    }
               }

               if (Matches)
               {
                    FilteredHits.push_back(HitObj);
               }
          }

          return FilteredHits;
     }

     std::function<bool(const std::map<std::string, std::string> &, const FilterExpressionNode *)> EvaluateExpression =
         [&](const std::map<std::string, std::string> &Document, const FilterExpressionNode *Node) -> bool
     {
          if (!Node)
          {
               return true;
          }

          if (Node->NodeType == FilterExpressionNode::Type::Condition)
          {
               return EvaluateFilterCondition(Document, Node->Condition);
          }

          if (Node->NodeType == FilterExpressionNode::Type::Not)
          {
               return !EvaluateExpression(Document, Node->Left.get());
          }

          const bool LeftMatches = EvaluateExpression(Document, Node->Left.get());
          const bool RightMatches = EvaluateExpression(Document, Node->Right.get());

          if (Node->NodeType == FilterExpressionNode::Type::Or)
          {
               return LeftMatches || RightMatches;
          }

          return LeftMatches && RightMatches;
     };

     for (const auto &HitObj : Hits)
     {
          if (EvaluateExpression(HitObj.Document, ExpressionRoot.get()))
          {
               FilteredHits.push_back(HitObj);
          }
     }

     return FilteredHits;
}

/* Evaluates a single filter condition against a document. */

bool SearchAPI::EvaluateFilterCondition(const std::map<std::string, std::string> &Document, const FilterCondition &Condition)
{
     auto ToLowerCopy = [](std::string Value) -> std::string
     {
          std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character)
                         {
                              return static_cast<char>(std::tolower(Character));
                         });
          return Value;
     };

     auto DocumentIt = Document.find(Condition.Field);
     std::string DerivedFieldValue;

     if (DocumentIt == Document.end() && Condition.Field == "created_at")
     {
          const auto TimestampIt = Document.find("timestamp");
          if (TimestampIt != Document.end())
          {
               try
               {
                    const std::uint64_t timestamp_ms = std::stoull(TimestampIt->second);
                    DerivedFieldValue = FormatTimestampMsAsISO8601(timestamp_ms);
                    if (!DerivedFieldValue.empty())
                    {
                         DocumentIt = Document.end();
                    }
               }
               catch (...)
               {
               }
          }
     }

     if (Condition.Op == "ISNULL")
     {
          return (DocumentIt == Document.end() && DerivedFieldValue.empty()) ||
                 (DocumentIt != Document.end() ? DocumentIt->second.empty() : DerivedFieldValue.empty());
     }

     if (Condition.Op == "ISNOTNULL")
     {
          return (DocumentIt != Document.end() && !DocumentIt->second.empty()) ||
                 !DerivedFieldValue.empty();
     }

     if (DocumentIt == Document.end() && DerivedFieldValue.empty())
     {
          return false;
     }

     auto TryParseDouble = [](const std::string &Value, double *Parsed) -> bool
     {
          if (!Parsed)
          {
               return false;
          }

          try
          {
               std::size_t Consumed = 0;
               const double Number = std::stod(Value, &Consumed);
               if (Consumed != Value.size())
               {
                    return false;
               }

               *Parsed = Number;
               return true;
          }
          catch (...)
          {
               return false;
          }
     };

     std::string FieldValue = (DocumentIt != Document.end()) ? DocumentIt->second : DerivedFieldValue;
     double FieldNumber = 0.0;
     const bool FieldIsNumber = TryParseDouble(FieldValue, &FieldNumber);
     std::uint64_t FieldTimestampMs = 0;
     const bool FieldIsTimestamp = TryParseISO8601TimestampToMs(FieldValue, &FieldTimestampMs);

     if (Condition.Op == "LIKE" || Condition.Op == "ILIKE" ||
         Condition.Op == "NOT_LIKE" || Condition.Op == "NOT_ILIKE")
     {
          const bool Match = (Condition.Op == "ILIKE" || Condition.Op == "NOT_ILIKE")
                                  ? Wildcard::Match(ToLowerCopy(FieldValue), ToLowerCopy(Condition.Value))
                                  : Wildcard::Match(FieldValue, Condition.Value);

          return (Condition.Op == "NOT_LIKE" || Condition.Op == "NOT_ILIKE") ? !Match : Match;
     }

     if (Condition.Op == "RANGE_INCLUSIVE" || Condition.Op == "RANGE_EXCLUSIVE")
     {
          size_t CommaPos = Condition.Value.find(',');

          if (CommaPos == std::string::npos)
          {
               return false;
          }

          std::string RangeMin = Condition.Value.substr(0, CommaPos);
          std::string RangeMax = Condition.Value.substr(CommaPos + 1);

          const bool MinOpenEnded = (RangeMin == "*");
          const bool MaxOpenEnded = (RangeMax == "*");

          double MinNumber = 0.0;
          double MaxNumber = 0.0;
          const bool MinIsNumber = !MinOpenEnded && TryParseDouble(RangeMin, &MinNumber);
          const bool MaxIsNumber = !MaxOpenEnded && TryParseDouble(RangeMax, &MaxNumber);

          if (FieldIsNumber && (MinOpenEnded || MinIsNumber) && (MaxOpenEnded || MaxIsNumber))
          {
               if (Condition.Op == "RANGE_INCLUSIVE")
               {
                    return (MinOpenEnded || FieldNumber >= MinNumber) &&
                           (MaxOpenEnded || FieldNumber <= MaxNumber);
               }

               return (MinOpenEnded || FieldNumber > MinNumber) &&
                      (MaxOpenEnded || FieldNumber < MaxNumber);
          }

          std::uint64_t MinTimestampMs = 0;
          std::uint64_t MaxTimestampMs = 0;
          const bool MinIsTimestamp = !MinOpenEnded && TryParseISO8601TimestampToMs(RangeMin, &MinTimestampMs);
          const bool MaxIsTimestamp = !MaxOpenEnded && TryParseISO8601TimestampToMs(RangeMax, &MaxTimestampMs);

          if (FieldIsTimestamp && (MinOpenEnded || MinIsTimestamp) && (MaxOpenEnded || MaxIsTimestamp))
          {
               if (Condition.Op == "RANGE_INCLUSIVE")
               {
                    return (MinOpenEnded || FieldTimestampMs >= MinTimestampMs) &&
                           (MaxOpenEnded || FieldTimestampMs <= MaxTimestampMs);
               }

               return (MinOpenEnded || FieldTimestampMs > MinTimestampMs) &&
                      (MaxOpenEnded || FieldTimestampMs < MaxTimestampMs);
          }

          if (Condition.Op == "RANGE_INCLUSIVE")
          {
               return (MinOpenEnded || FieldValue >= RangeMin) &&
                      (MaxOpenEnded || FieldValue <= RangeMax);
          }

          return (MinOpenEnded || FieldValue > RangeMin) &&
                 (MaxOpenEnded || FieldValue < RangeMax);
     }

     if (Condition.Op == "=" || Condition.Op == ":=")
     {
          if (FieldIsTimestamp)
          {
               std::uint64_t EqualityTimestampMs = 0;
               if (TryParseISO8601TimestampToMs(Condition.Value, &EqualityTimestampMs))
               {
                    return FieldTimestampMs == EqualityTimestampMs;
               }
          }

          return (FieldValue == Condition.Value);
     }

     if (Condition.Op == "!=")
     {
          if (FieldIsTimestamp)
          {
               std::uint64_t InequalityTimestampMs = 0;
               if (TryParseISO8601TimestampToMs(Condition.Value, &InequalityTimestampMs))
               {
                    return FieldTimestampMs != InequalityTimestampMs;
               }
          }

          return (FieldValue != Condition.Value);
     }

     double ConditionNumber = 0.0;
     const bool ConditionIsNumber = TryParseDouble(Condition.Value, &ConditionNumber);
     std::uint64_t ConditionTimestampMs = 0;
     const bool ConditionIsTimestamp = TryParseISO8601TimestampToMs(Condition.Value, &ConditionTimestampMs);

     if (Condition.Op == ">" || Condition.Op == ">=" || Condition.Op == "<" || Condition.Op == "<=")
     {
          if (FieldIsNumber && ConditionIsNumber)
          {
               if (Condition.Op == ">")
               {
                    return FieldNumber > ConditionNumber;
               }
               if (Condition.Op == ">=")
               {
                    return FieldNumber >= ConditionNumber;
               }
               if (Condition.Op == "<")
               {
                    return FieldNumber < ConditionNumber;
               }
               return FieldNumber <= ConditionNumber;
          }

          if (FieldIsTimestamp && ConditionIsTimestamp)
          {
               if (Condition.Op == ">")
               {
                    return FieldTimestampMs > ConditionTimestampMs;
               }
               if (Condition.Op == ">=")
               {
                    return FieldTimestampMs >= ConditionTimestampMs;
               }
               if (Condition.Op == "<")
               {
                    return FieldTimestampMs < ConditionTimestampMs;
               }
               return FieldTimestampMs <= ConditionTimestampMs;
          }

          if (Condition.Op == ">")
          {
               return FieldValue > Condition.Value;
          }
          if (Condition.Op == ">=")
          {
               return FieldValue >= Condition.Value;
          }
          if (Condition.Op == "<")
          {
               return FieldValue < Condition.Value;
          }
          return FieldValue <= Condition.Value;
     }

     return false;
}

/* ApplySorting sorts search hits. */

std::vector<SearchHit> SearchAPI::ApplySorting(const std::vector<SearchHit> &Hits, const std::vector<std::string> &SortBy)
{
     std::vector<SearchHit> SortedHits = Hits;

     std::sort(SortedHits.begin(), SortedHits.end(), [this, &SortBy](const SearchHit &A, const SearchHit &B)
               {
                    for (const auto &SortFieldVal : SortBy)
                    {
                         std::string FieldName = SortFieldVal;
                         bool Descending = false;

                         size_t ColonPos = SortFieldVal.find(':');
                         if (ColonPos != std::string::npos)
                         {
                              FieldName = SortFieldVal.substr(0, ColonPos);
                              std::string Direction = SortFieldVal.substr(ColonPos + 1);
                              std::transform(Direction.begin(), Direction.end(), Direction.begin(), [](unsigned char C)
                                             {
                                                  return static_cast<char>(std::tolower(C));
                                             });
                              Descending = (Direction == "desc" || Direction == "descending");
                         }

                         if (FieldName == "_text_match")
                         {
                              if (A.TextMatch != B.TextMatch)
                              {
                                   return Descending ? A.TextMatch > B.TextMatch : A.TextMatch < B.TextMatch;
                              }

                              continue;
                         }

                         if (FieldName == "_score" || FieldName == "score")
                         {
                              const float ScoreA = GetEffectiveScore(A);
                              const float ScoreB = GetEffectiveScore(B);

                              if (ScoreA != ScoreB)
                              {
                                   return Descending ? ScoreA > ScoreB : ScoreA < ScoreB;
                              }

                              continue;
                         }

                         std::string AVal = A.Document.count(FieldName) ? A.Document.at(FieldName) : "";
                         std::string BVal = B.Document.count(FieldName) ? B.Document.at(FieldName) : "";

                         if (AVal != BVal)
                         {
                              return Descending ? AVal > BVal : AVal < BVal;
                         }
                    }

                    return false;
               });

     return SortedHits;
}

void SearchAPI::ApplyModuleWeights(std::vector<SearchHit> &Hits,
                                   const std::string &Collection,
                                   const ComprehensiveSearchQuery &Query,
                                   const std::string &RankingMode)
{
     if (!Instance || !Instance->Modules)
     {
          return;
     }

     for (auto &Hit : Hits)
     {
          const float BaseScore = Hit.HybridScore > 0.0f ? Hit.HybridScore : (Hit.VectorScore > 0.0f ? Hit.VectorScore : Hit.TextMatch);
          const float ModuleMultiplier = Instance->Modules->ComputeSearchWeightMultiplier(Collection, Query.Q, RankingMode, Hit, BaseScore);

          if (std::isfinite(ModuleMultiplier) && ModuleMultiplier > 0.0f)
          {
               Hit.Weight *= ModuleMultiplier;
          }
     }
}

float SearchAPI::GetEffectiveScore(const SearchHit &Hit) const
{
     const float BaseScore = Hit.HybridScore > 0.0f ? Hit.HybridScore : (Hit.VectorScore > 0.0f ? Hit.VectorScore : Hit.TextMatch);
     return BaseScore * (std::isfinite(Hit.Weight) && Hit.Weight > 0.0f ? Hit.Weight : 1.0f);
}

/* ApplyPagination paginates results. */

std::vector<SearchHit> SearchAPI::ApplyPagination(const std::vector<SearchHit> &Hits, int Page, int PerPage)
{
     if (Hits.empty())
     {
          return {};
     }

     if (Page <= 0 || PerPage <= 0)
     {
          return {};
     }

     const std::size_t PageIndex = static_cast<std::size_t>(Page - 1);
     const std::size_t PerPageSize = static_cast<std::size_t>(PerPage);

     if (PageIndex > (std::numeric_limits<std::size_t>::max() / PerPageSize))
     {
          return {};
     }

     const std::size_t StartPosSize = PageIndex * PerPageSize;

     if (StartPosSize >= Hits.size())
     {
          return {};
     }

     std::size_t EndPosSize = std::min(StartPosSize + PerPageSize, Hits.size());

     return std::vector<SearchHit>(
          Hits.begin() + static_cast<std::vector<SearchHit>::difference_type>(StartPosSize),
          Hits.begin() + static_cast<std::vector<SearchHit>::difference_type>(EndPosSize));
}

/* ApplyGrouping groups results. */

std::vector<SearchHit> SearchAPI::ApplyGrouping(const std::vector<SearchHit> &Hits, const std::vector<std::string> &GroupBy, int GroupLimit)
{
     if (GroupLimit <= 0)
     {
          return {};
     }

     std::size_t GroupLimitSize = static_cast<std::size_t>(GroupLimit);
     std::map<std::string, std::vector<std::size_t>> Groups;

	     for (std::size_t HitIndex = 0; HitIndex < Hits.size(); ++HitIndex)
	     {
	          const auto &HitObj = Hits[HitIndex];
	          std::string Key;

	          for (const auto &Field : GroupBy)
	          {
	               auto FieldIt = HitObj.Document.find(Field);
	               if (FieldIt != HitObj.Document.end())
	               {
	                    Key += FieldIt->second;
	               }
	          }

	          if (Groups[Key].size() < GroupLimitSize)
	          {
	               Groups[Key].push_back(HitIndex);
          }
     }

     std::vector<SearchHit> Result;
     Result.reserve(std::min(Hits.size(), GroupLimitSize * Groups.size()));

     for (auto &Pair : Groups)
     {
          for (std::size_t HitIndex : Pair.second)
          {
               Result.push_back(Hits[HitIndex]);
          }
     }

     return Result;
}

/* CalculateRecencyDecay calculates decay factor for old documents. */

float SearchAPI::CalculateRecencyDecay(const SearchHit &Hit, const std::string &Query)
{
     (void)Query;

     if (!Hit.Document.count("timestamp"))
     {
          return 1.0f;
     }

     try
     {
          uint64_t TS = std::stoull(Hit.Document.at("timestamp"));
          uint64_t NowVal = Instance->NowMs();

          if (NowVal <= TS)
          {
               return 1.0f;
          }

          double DiffDays = static_cast<double>(NowVal - TS) / (1000.0 * 60.0 * 60.0 * 24.0);

          return static_cast<float>(std::exp(-0.01 * DiffDays));
     }
     catch (...)
     {
          return 1.0f;
     }
}

/* CalculateWeight calculates base weight. */

float SearchAPI::CalculateWeight(const SearchHit &Hit)
{
     return 1.0f * CalculateRecencyDecay(Hit, "");
}

/* ApplyFieldSelection includes/excludes fields. */

std::map<std::string, std::string> SearchAPI::ApplyFieldSelection(const std::map<std::string, std::string> &Document, const std::vector<std::string> &IncludeFields, const std::vector<std::string> &ExcludeFields)
{
     std::map<std::string, std::string> Result;

     for (const auto &Pair : Document)
     {
          bool Include = IncludeFields.empty();

          if (!Include)
          {
               for (const auto &F : IncludeFields)
               {
                    if (F == Pair.first)
                    {
                         Include = true;

                         break;
                    }
               }
          }

          if (Include)
          {
               for (const auto &F : ExcludeFields)
               {
                    if (F == Pair.first)
                    {
                         Include = false;

                         break;
                    }
               }
          }

          if (Include)
          {
               Result[Pair.first] = Pair.second;
          }
     }

     return Result;
}

/* GenerateHighlights generates search snippets. */

std::map<std::string, std::string> SearchAPI::GenerateHighlights(const std::map<std::string, std::string> &Document, const std::string &Query, const std::vector<std::string> &HighlightFields)
{
     std::map<std::string, std::string> Highlights;

     for (const auto &Pair : Document)
     {
          bool ShouldHighlight = HighlightFields.empty();

          if (!ShouldHighlight)
          {
               for (const auto &F : HighlightFields)
               {
                    if (F == Pair.first)
                    {
                         ShouldHighlight = true;

                         break;
                    }
               }
          }

          if (ShouldHighlight)
          {
               Highlights[Pair.first] = GenerateFieldHighlight(Pair.second, Query);
          }
     }

     return Highlights;
}

/* GenerateFieldHighlight wraps matching terms in tags. */

std::string SearchAPI::GenerateFieldHighlight(const std::string &FieldValue, const std::string &Query)
{
     if (Query.empty())
     {
          return FieldValue;
     }

     auto ToLowerCopy = [](const std::string &input) -> std::string
     {
          std::string out = input;
          std::transform(out.begin(), out.end(), out.begin(), ::tolower);
          return out;
     };

     auto ReplaceAll = [](std::string &s, const std::string &from, const std::string &to)
     {
          if (from.empty())
          {
               return;
          }
          size_t pos = 0;
          while ((pos = s.find(from, pos)) != std::string::npos)
          {
               s.replace(pos, from.length(), to);
               pos += to.length();
          }
     };

     auto NormalizeQuotes = [&](const std::string &input) -> std::string
     {
          std::string normalized = input;
          ReplaceAll(normalized, "\xE2\x80\x9C", "\"");
          ReplaceAll(normalized, "\xE2\x80\x9D", "\"");
          ReplaceAll(normalized, "\xE2\x80\x98", "'");
          ReplaceAll(normalized, "\xE2\x80\x99", "'");
          return normalized;
     };

     auto HighlightTerms = [&](const std::string &text, const std::vector<std::string> &terms) -> std::string
     {
          if (terms.empty())
          {
               return text;
          }

          std::string lowered = ToLowerCopy(text);
          std::string result;
          result.reserve(text.size() + 32);

          size_t pos = 0;

          while (pos < text.size())
          {
               size_t best_pos = std::string::npos;
               size_t best_len = 0;

               for (const auto &term : terms)
               {
                    if (term.empty())
                    {
                         continue;
                    }

                    size_t found = lowered.find(term, pos);
                    if (found != std::string::npos)
                    {
                         if (best_pos == std::string::npos || found < best_pos || (found == best_pos && term.size() > best_len))
                         {
                              best_pos = found;
                              best_len = term.size();
                         }
                    }
               }

               if (best_pos == std::string::npos)
               {
                    result.append(text.substr(pos));
                    break;
               }

               result.append(text.substr(pos, best_pos - pos));
               result.append("<em>");
               result.append(text.substr(best_pos, best_len));
               result.append("</em>");
               pos = best_pos + best_len;
          }

          return result;
     };

     std::string normalized_query = NormalizeQuotes(Query);
     std::vector<std::string> phrases;
     std::vector<std::string> terms;

     size_t start = 0;
     while ((start = normalized_query.find('"', start)) != std::string::npos)
     {
          size_t end = normalized_query.find('"', start + 1);
          if (end == std::string::npos)
          {
               break;
          }
          if (end > start + 1)
          {
               phrases.push_back(normalized_query.substr(start + 1, end - start - 1));
          }
          start = end + 1;
     }

     std::string without_quotes = normalized_query;
     while ((start = without_quotes.find('"')) != std::string::npos)
     {
          size_t end = without_quotes.find('"', start + 1);
          if (end == std::string::npos)
          {
               without_quotes.erase(start, 1);
               break;
          }
          without_quotes.erase(start, end - start + 1);
     }

     std::istringstream iss(without_quotes);
     std::string token;
     while (iss >> token)
     {
          terms.push_back(token);
     }

     auto normalize_list = [&](std::vector<std::string> &items)
     {
          std::vector<std::string> out;
          std::unordered_set<std::string> seen;
          const std::unordered_set<std::string> stopwords = {"on", "in", "at", "by", "to", "of", "and", "or", "the", "a", "an"};

          for (const auto &item : items)
          {
               std::string lowered = ToLowerCopy(item);
               if (lowered.size() < 3)
               {
                    continue;
               }
               if (stopwords.find(lowered) != stopwords.end())
               {
                    continue;
               }
               if (seen.insert(lowered).second)
               {
                    out.push_back(lowered);
               }
          }

          std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b)
                    {
                         return a.size() > b.size();
                    });

          items.swap(out);
     };

     if (!phrases.empty())
     {
          normalize_list(phrases);
          normalize_list(terms);
          std::vector<std::string> combined;
          combined.reserve(phrases.size() + terms.size());
          combined.insert(combined.end(), phrases.begin(), phrases.end());
          combined.insert(combined.end(), terms.begin(), terms.end());
          return HighlightTerms(FieldValue, combined);
     }

     normalize_list(terms);
     return HighlightTerms(FieldValue, terms);
}
