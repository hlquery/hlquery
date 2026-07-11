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
#include "vendor/json/json.hpp"

/* Provides shared API utility helpers used by multiple handlers. */

std::string TrimRankMetadataValue(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");
     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

/* Implements the lower rank metadata value helper. */

std::string LowerRankMetadataValue(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

/* Implements the try parse double value helper. */

bool TryParseDoubleValue(const std::string &Value, double *Out)
{
     if (!Out)
     {
          return false;
     }

     const std::string Trimmed = TrimRankMetadataValue(Value);
     if (Trimmed.empty())
     {
          return false;
     }

     try
     {
          size_t ParsedChars = 0;
          const double Parsed = std::stod(Trimmed, &ParsedChars);
          if (ParsedChars != Trimmed.size() || !std::isfinite(Parsed))
          {
               return false;
          }

          *Out = Parsed;
          return true;
     }
     catch (...)
     {
          return false;
     }
}

/* Implements the split geo args helper. */

static std::vector<std::string> SplitGeoArgs(const std::string &Input)
{
     std::vector<std::string> Args;
     std::string Current;
     int BracketDepth = 0;
     int BraceDepth = 0;
     int ParenDepth = 0;

     auto Trim = [](std::string Value) -> std::string
     {
          const size_t Start = Value.find_first_not_of(" \t\r\n");
          if (Start == std::string::npos)
          {
               return "";
          }

          const size_t End = Value.find_last_not_of(" \t\r\n");
          Value = Value.substr(Start, End - Start + 1);
          if (Value.size() >= 2 && ((Value.front() == '"' && Value.back() == '"') || (Value.front() == '\'' && Value.back() == '\'')))
          {
               Value = Value.substr(1, Value.size() - 2);
          }
          return Value;
     };

     for (char C : Input)
     {
          if (C == '[')
          {
               ++BracketDepth;
          }
          else if (C == ']' && BracketDepth > 0)
          {
               --BracketDepth;
          }
          else if (C == '{')
          {
               ++BraceDepth;
          }
          else if (C == '}' && BraceDepth > 0)
          {
               --BraceDepth;
          }
          else if (C == '(')
          {
               ++ParenDepth;
          }
          else if (C == ')' && ParenDepth > 0)
          {
               --ParenDepth;
          }

          if (C == ',' && BracketDepth == 0 && BraceDepth == 0 && ParenDepth == 0)
          {
               Args.push_back(Trim(Current));
               Current.clear();
               continue;
          }

          Current.push_back(C);
     }

     if (!Current.empty())
     {
          Args.push_back(Trim(Current));
     }

     return Args;
}

/* Implements the try parse geo distance km helper. */

static bool TryParseGeoDistanceKM(const std::string &Value, double *OutKM)
{
     if (!OutKM)
     {
          return false;
     }

     std::string Text = LowerRankMetadataValue(TrimRankMetadataValue(Value));
     if (Text.empty())
     {
          return false;
     }

     double Multiplier = 1.0;
     auto EndsWith = [&Text](const std::string &Suffix)
     {
          return Text.size() >= Suffix.size() && Text.compare(Text.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
     };

     if (EndsWith("km"))
     {
          Text = Text.substr(0, Text.size() - 2);
     }
     else if (EndsWith("mi") || EndsWith("mile") || EndsWith("miles"))
     {
          const size_t TrimBy = EndsWith("miles") ? 5 : (EndsWith("mile") ? 4 : 2);
          Text = Text.substr(0, Text.size() - TrimBy);
          Multiplier = 1.609344;
     }
     else if (EndsWith("m"))
     {
          Text = Text.substr(0, Text.size() - 1);
          Multiplier = 0.001;
     }

     double Parsed = 0.0;
     if (!TryParseDoubleValue(Text, &Parsed) || Parsed < 0.0)
     {
          return false;
     }

     *OutKM = Parsed * Multiplier;
     return true;
}

/* Checks whether valid geo coordinates applies. */

static bool IsValidGeoCoordinates(double Lat, double Lon)
{
     return std::isfinite(Lat) && std::isfinite(Lon) &&
            Lat >= -90.0 && Lat <= 90.0 &&
            Lon >= -180.0 && Lon <= 180.0;
}

/* Implements the try parse geo point value helper. */

static bool TryParseGeoPointValue(const std::string &Value, GeoPoint *OutPoint)
{
     if (!OutPoint)
     {
          return false;
     }

     const std::string Trimmed = TrimRankMetadataValue(Value);
     if (Trimmed.empty())
     {
          return false;
     }

     auto AssignIfValid = [OutPoint](double Lat, double Lon) -> bool
     {
          if (!IsValidGeoCoordinates(Lat, Lon))
          {
               return false;
          }

          OutPoint->Latitude = Lat;
          OutPoint->Longitude = Lon;
          return true;
     };

     try
     {
          const nlohmann::json Parsed = nlohmann::json::parse(Trimmed);
          if (Parsed.is_array() && Parsed.size() >= 2 && Parsed[0].is_number() && Parsed[1].is_number())
          {
               return AssignIfValid(Parsed[0].get<double>(), Parsed[1].get<double>());
          }

          if (Parsed.is_object())
          {
               const char *LatKeys[] = {"lat", "latitude"};
               const char *LonKeys[] = {"lon", "lng", "longitude"};
               bool HasLat = false;
               bool HasLon = false;
               double Lat = 0.0;
               double Lon = 0.0;

               for (const char *Key : LatKeys)
               {
                    if (Parsed.contains(Key) && Parsed[Key].is_number())
                    {
                         Lat = Parsed[Key].get<double>();
                         HasLat = true;
                         break;
                    }
               }

               for (const char *Key : LonKeys)
               {
                    if (Parsed.contains(Key) && Parsed[Key].is_number())
                    {
                         Lon = Parsed[Key].get<double>();
                         HasLon = true;
                         break;
                    }
               }

               if (HasLat && HasLon)
               {
                    return AssignIfValid(Lat, Lon);
               }
          }
     }
     catch (...)
     {
          /* Fall through to simple string parsing. */
     }

     std::string Normalized = Trimmed;
     std::replace(Normalized.begin(), Normalized.end(), ';', ',');
     std::replace(Normalized.begin(), Normalized.end(), ' ', ',');
     const std::vector<std::string> Parts = SplitGeoArgs(Normalized);
     if (Parts.size() < 2)
     {
          return false;
     }

     double Lat = 0.0;
     double Lon = 0.0;
     if (!TryParseDoubleValue(Parts[0], &Lat) || !TryParseDoubleValue(Parts[1], &Lon))
     {
          return false;
     }

     return AssignIfValid(Lat, Lon);
}

/* Implements the try get geo point from document helper. */

static bool TryGetGeoPointFromDocument(const std::map<std::string, std::string> &Document, const std::string &Field, GeoPoint *OutPoint)
{
     const auto It = Document.find(Field);
     if (It != Document.end() && TryParseGeoPointValue(It->second, OutPoint))
     {
          return true;
     }

     const auto FieldsIt = Document.find("fields");
     if (FieldsIt == Document.end() || FieldsIt->second.empty())
     {
          return false;
     }

     try
     {
          const nlohmann::json FieldsJSON = nlohmann::json::parse(FieldsIt->second);
          if (FieldsJSON.is_object() && FieldsJSON.contains(Field) && !FieldsJSON[Field].is_null())
          {
               return TryParseGeoPointValue(FieldsJSON[Field].is_string() ? FieldsJSON[Field].get<std::string>() : FieldsJSON[Field].dump(), OutPoint);
          }
     }
     catch (...)
     {
     }

     return false;
}

/* Implements the try parse geo distance sort helper. */

static bool TryParseGeoDistanceSort(const std::string &SortSpec, std::string *OutField, GeoPoint *OutOrigin, bool *OutDescending)
{
     const std::string Prefix = "_geo_distance";
     if (SortSpec.compare(0, Prefix.size(), Prefix) != 0)
     {
          return false;
     }

     const size_t Open = SortSpec.find('(');
     const size_t Close = SortSpec.find(')', Open == std::string::npos ? 0 : Open + 1);
     if (Open == std::string::npos || Close == std::string::npos || Close <= Open + 1)
     {
          return false;
     }

     const std::vector<std::string> Args = SplitGeoArgs(SortSpec.substr(Open + 1, Close - Open - 1));
     if (Args.size() < 3)
     {
          return false;
     }

     double Lat = 0.0;
     double Lon = 0.0;
     if (!TryParseDoubleValue(Args[1], &Lat) || !TryParseDoubleValue(Args[2], &Lon))
     {
          return false;
     }

     GeoPoint Origin;
     Origin.Latitude = Lat;
     Origin.Longitude = Lon;
     if (Origin.Latitude < -90.0 || Origin.Latitude > 90.0 || Origin.Longitude < -180.0 || Origin.Longitude > 180.0)
     {
          return false;
     }

     bool Desc = false;
     const size_t Colon = SortSpec.find(':', Close + 1);
     if (Colon != std::string::npos)
     {
          std::string Direction = LowerRankMetadataValue(TrimRankMetadataValue(SortSpec.substr(Colon + 1)));
          Desc = Direction == "desc" || Direction == "descending";
     }

     if (OutField)
     {
          *OutField = Args[0];
     }
     if (OutOrigin)
     {
          *OutOrigin = Origin;
     }
     if (OutDescending)
     {
          *OutDescending = Desc;
     }
     return true;
}

/* Implements the format rank signal value helper. */

std::string FormatRankSignalValue(double Value)
{
     std::ostringstream Stream;
     Stream << std::fixed << std::setprecision(6) << std::clamp(Value, 0.0, 1.0);
     return Stream.str();
}

/* Implements the compare rank tie break helper. */

bool CompareRankTieBreak(const SearchHit &A, const SearchHit &B)
{
     auto ParseField = [](const SearchHit &Hit, const std::string &Field, double *Out) -> bool
     {
          auto It = Hit.Document.find(Field);
          if (It == Hit.Document.end())
          {
               return false;
          }

          return TryParseDoubleValue(It->second, Out);
     };

     double ARankSignal = 0.0;
     double BRankSignal = 0.0;
     if (ParseField(A, "rank_signal", &ARankSignal) && ParseField(B, "rank_signal", &BRankSignal) && ARankSignal != BRankSignal)
     {
          return ARankSignal > BRankSignal;
     }

     double ARank = 0.0;
     double BRank = 0.0;
     if (ParseField(A, "rank", &ARank) && ParseField(B, "rank", &BRank) && ARank != BRank)
     {
          return ARank < BRank;
     }

     const auto AId = A.Document.find("id");
     const auto BId = B.Document.find("id");
     return (AId == A.Document.end() ? "" : AId->second) < (BId == B.Document.end() ? "" : BId->second);
}

/* Implements the try parse ISO 8601 timestamp to milliseconds helper. */

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

/* Implements the format timestamp milliseconds as ISO 8601 helper. */

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

          if (Input.compare(Position, 11, "_geo_radius") == 0 ||
              Input.compare(Position, 8, "_geo_box") == 0)
          {
               const bool is_radius = Input.compare(Position, 11, "_geo_radius") == 0;
               const size_t name_len = is_radius ? 11 : 8;
               Position += name_len;
               SkipWhitespace();

               if (Position >= Input.size() || Input[Position] != '(')
               {
                    return false;
               }

               ++Position;
               const size_t args_start = Position;
               int depth = 1;

               while (Position < Input.size() && depth > 0)
               {
                    if (Input[Position] == '(')
                    {
                         ++depth;
                    }
                    else if (Input[Position] == ')')
                    {
                         --depth;
                         if (depth == 0)
                         {
                              break;
                         }
                    }

                    ++Position;
               }

               if (Position >= Input.size() || Input[Position] != ')')
               {
                    return false;
               }

               filter.Field = is_radius ? "_geo_radius" : "_geo_box";
               filter.Op = is_radius ? "GEO_RADIUS" : "GEO_BOX";
               filter.Value = Trim(Input.substr(args_start, Position - args_start));
               ++Position;
               return true;
          }

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

/* Calculates geo distance values. */

double SearchAPI::CalculateGeoDistance(const GeoPoint &P1, const GeoPoint &P2)
{
     constexpr double EarthRadiusKM = 6371.0088;
     constexpr double Pi = 3.14159265358979323846;
     auto ToRadians = [](double Degrees) -> double
     {
          return Degrees * Pi / 180.0;
     };

     const double Lat1 = ToRadians(P1.Latitude);
     const double Lat2 = ToRadians(P2.Latitude);
     const double DeltaLat = ToRadians(P2.Latitude - P1.Latitude);
     const double DeltaLon = ToRadians(P2.Longitude - P1.Longitude);

     const double RawA = std::sin(DeltaLat / 2.0) * std::sin(DeltaLat / 2.0) +
                         std::cos(Lat1) * std::cos(Lat2) *
                             std::sin(DeltaLon / 2.0) * std::sin(DeltaLon / 2.0);
     const double A = std::clamp(RawA, 0.0, 1.0);
     const double C = 2.0 * std::atan2(std::sqrt(A), std::sqrt(1.0 - A));

     return EarthRadiusKM * C;
}

/* Checks whether within geo radius applies. */

bool SearchAPI::IsWithinGeoRadius(const GeoPoint &Point, const GeoRadius &Radius)
{
     if (!Radius.Enabled || Radius.RadiusKM < 0.0)
     {
          return false;
     }

     GeoPoint Origin;
     Origin.Latitude = Radius.Lat;
     Origin.Longitude = Radius.Lon;
     return CalculateGeoDistance(Point, Origin) <= Radius.RadiusKM;
}

/* Checks whether within geo box applies. */

bool SearchAPI::IsWithinGeoBox(const GeoPoint &Point, const GeoBox &Box)
{
     if (!Box.Enabled)
     {
          return false;
     }

     const double North = std::max(Box.TopLeftLat, Box.BottomRightLat);
     const double South = std::min(Box.TopLeftLat, Box.BottomRightLat);
     const bool LatMatches = Point.Latitude <= North && Point.Latitude >= South;

     if (!LatMatches)
     {
          return false;
     }

     const double West = Box.TopLeftLon;
     const double East = Box.BottomRightLon;

     if (West <= East)
     {
          return Point.Longitude >= West && Point.Longitude <= East;
     }

     return Point.Longitude >= West || Point.Longitude <= East;
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
               return FilterBy.empty() ? Hits : FilteredHits;
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

     if (Condition.Op == "GEO_RADIUS")
     {
          const std::vector<std::string> Args = SplitGeoArgs(Condition.Value);
          if (Args.size() < 4)
          {
               return false;
          }

          double Lat = 0.0;
          double Lon = 0.0;
          double RadiusKM = 0.0;
          if (!TryParseDoubleValue(Args[1], &Lat) ||
              !TryParseDoubleValue(Args[2], &Lon) ||
              !TryParseGeoDistanceKM(Args[3], &RadiusKM))
          {
               return false;
          }

          if (!IsValidGeoCoordinates(Lat, Lon))
          {
               return false;
          }

          GeoPoint Point;
          if (!TryGetGeoPointFromDocument(Document, Args[0], &Point))
          {
               return false;
          }

          GeoRadius Radius;
          Radius.Field = Args[0];
          Radius.Lat = Lat;
          Radius.Lon = Lon;
          Radius.RadiusKM = RadiusKM;
          Radius.Enabled = true;
          return IsWithinGeoRadius(Point, Radius);
     }

     if (Condition.Op == "GEO_BOX")
     {
          const std::vector<std::string> Args = SplitGeoArgs(Condition.Value);
          if (Args.size() < 5)
          {
               return false;
          }

          GeoBox Box;
          Box.Field = Args[0];
          Box.Enabled = true;
          if (!TryParseDoubleValue(Args[1], &Box.TopLeftLat) ||
              !TryParseDoubleValue(Args[2], &Box.TopLeftLon) ||
              !TryParseDoubleValue(Args[3], &Box.BottomRightLat) ||
              !TryParseDoubleValue(Args[4], &Box.BottomRightLon))
          {
               return false;
          }

          if (!IsValidGeoCoordinates(Box.TopLeftLat, Box.TopLeftLon) ||
              !IsValidGeoCoordinates(Box.BottomRightLat, Box.BottomRightLon))
          {
               return false;
          }

          GeoPoint Point;
          if (!TryGetGeoPointFromDocument(Document, Args[0], &Point))
          {
               return false;
          }

          return IsWithinGeoBox(Point, Box);
     }

     auto DocumentIt = Document.find(Condition.Field);
     std::string DerivedFieldValue;

     if (DocumentIt == Document.end())
     {
          const auto FieldsIt = Document.find("fields");
          if (FieldsIt != Document.end() && !FieldsIt->second.empty())
          {
               try
               {
                    const nlohmann::json FieldsJSON = nlohmann::json::parse(FieldsIt->second);
                    if (FieldsJSON.is_object() && FieldsJSON.contains(Condition.Field) && !FieldsJSON[Condition.Field].is_null())
                    {
                         const auto &Value = FieldsJSON[Condition.Field];
                         if (Value.is_string())
                         {
                              DerivedFieldValue = Value.get<std::string>();
                         }
                         else if (Value.is_number_integer())
                         {
                              DerivedFieldValue = std::to_string(Value.get<int64_t>());
                         }
                         else if (Value.is_number_float())
                         {
                              DerivedFieldValue = std::to_string(Value.get<double>());
                         }
                         else if (Value.is_boolean())
                         {
                              DerivedFieldValue = Value.get<bool>() ? "true" : "false";
                         }
                         else
                         {
                              DerivedFieldValue = Value.dump();
                         }
                    }
               }
               catch (...)
               {
                    /* Ignore malformed legacy fields payloads. */
               }
          }
     }

     if (DocumentIt == Document.end() && DerivedFieldValue.empty() && Condition.Field == "created_at")
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
                         std::string GeoField;
                         GeoPoint GeoOrigin;
                         bool GeoDescending = false;
                         if (TryParseGeoDistanceSort(SortFieldVal, &GeoField, &GeoOrigin, &GeoDescending))
                         {
                              GeoPoint APoint;
                              GeoPoint BPoint;
                              const bool HasA = TryGetGeoPointFromDocument(A.Document, GeoField, &APoint);
                              const bool HasB = TryGetGeoPointFromDocument(B.Document, GeoField, &BPoint);

                              if (HasA != HasB)
                              {
                                   return HasA;
                              }

                              if (HasA && HasB)
                              {
                                   const double ADistance = CalculateGeoDistance(APoint, GeoOrigin);
                                   const double BDistance = CalculateGeoDistance(BPoint, GeoOrigin);

                                   if (ADistance != BDistance)
                                   {
                                        return GeoDescending ? ADistance > BDistance : ADistance < BDistance;
                                   }
                              }

                              continue;
                         }

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
                              const float ScoreA = GetEffectiveScore(A);
                              const float ScoreB = GetEffectiveScore(B);

                              if (ScoreA != ScoreB)
                              {
                                   return Descending ? ScoreA > ScoreB : ScoreA < ScoreB;
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
                              double ANumber = 0.0;
                              double BNumber = 0.0;
                              if (TryParseDoubleValue(AVal, &ANumber) && TryParseDoubleValue(BVal, &BNumber) && ANumber != BNumber)
                              {
                                   return Descending ? ANumber > BNumber : ANumber < BNumber;
                              }

                              return Descending ? AVal > BVal : AVal < BVal;
                         }
                    }

                    return CompareRankTieBreak(A, B);
               });

     for (const auto &SortFieldVal : SortBy)
     {
          std::string GeoField;
          GeoPoint GeoOrigin;
          bool GeoDescending = false;
          if (!TryParseGeoDistanceSort(SortFieldVal, &GeoField, &GeoOrigin, &GeoDescending))
          {
               continue;
          }

          for (auto &HitObj : SortedHits)
          {
               GeoPoint Point;
               if (!TryGetGeoPointFromDocument(HitObj.Document, GeoField, &Point))
               {
                    continue;
               }

               std::ostringstream DistanceStream;
               DistanceStream << std::fixed << std::setprecision(6) << CalculateGeoDistance(Point, GeoOrigin);
               HitObj.Document["_geo_distance_km"] = DistanceStream.str();
          }

          break;
     }

     return SortedHits;
}

/* ResolveDefaultCollectionSortBy returns collection-level default sort fields. */

std::vector<std::string> SearchAPI::ResolveDefaultCollectionSortBy(const std::string &Collection)
{
     CollectionConfig Config;
     if (!HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
     {
          return {};
     }

     auto MetadataValue = [&Config](const std::string &Key) -> std::string
     {
          auto It = Config.Metadata.find(Key);
          return It == Config.Metadata.end() ? "" : TrimRankMetadataValue(It->second);
     };

     std::string SortFieldName = MetadataValue("_default_sorting_field");
     std::string SortOrder = LowerRankMetadataValue(MetadataValue("_default_sorting_order"));

     if (SortFieldName.empty())
     {
          SortFieldName = MetadataValue("_rank_field");
          SortOrder = LowerRankMetadataValue(MetadataValue("_rank_order"));
     }

     if (SortFieldName.empty())
     {
          return {};
     }

     if (SortOrder.empty())
     {
          SortOrder = "asc";
     }
     else if (SortOrder == "descending" || SortOrder == "higher" || SortOrder == "higher_is_better")
     {
          SortOrder = "desc";
     }
     else if (SortOrder == "ascending" || SortOrder == "lower" || SortOrder == "lower_is_better")
     {
          SortOrder = "asc";
     }
     else if (SortOrder != "asc" && SortOrder != "desc")
     {
          SortOrder = "asc";
     }

     return {SortFieldName + ":" + SortOrder};
}

/* Applies module weights processing. */

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

/* Applies collection rank weights processing. */

void SearchAPI::ApplyCollectionRankWeights(std::vector<SearchHit> &Hits, const std::string &Collection)
{
     if (Hits.empty())
     {
          return;
     }

     CollectionConfig Config;
     if (!HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
     {
          return;
     }

     auto MetadataValue = [&](const std::string &Key) -> std::string
     {
          auto It = Config.Metadata.find(Key);
          return It == Config.Metadata.end() ? "" : TrimRankMetadataValue(It->second);
     };

     const std::string RankField = MetadataValue("_rank_field");
     if (RankField.empty())
     {
          return;
     }

     double RankWeight = 0.25;
     const std::string RankWeightRaw = MetadataValue("_rank_weight");
     if (!RankWeightRaw.empty() && (!TryParseDoubleValue(RankWeightRaw, &RankWeight) || RankWeight <= 0.0 || !std::isfinite(RankWeight)))
     {
          return;
     }

     RankWeight = std::min(RankWeight, 10.0);

     const std::string RankOrder = LowerRankMetadataValue(MetadataValue("_rank_order"));
     const bool Descending = (RankOrder == "desc" || RankOrder == "descending" || RankOrder == "higher" || RankOrder == "higher_is_better");
     const std::string RankAlgorithm = LowerRankMetadataValue(MetadataValue("_rank_algorithm"));

     struct ParsedRank
     {
          size_t Index = 0;
          double Value = 0.0;
          double Signal = 1.0;
     };

     std::vector<ParsedRank> ParsedRanks;
     ParsedRanks.reserve(Hits.size());

     double MinRank = std::numeric_limits<double>::max();
     double MaxRank = std::numeric_limits<double>::lowest();

     for (size_t Index = 0; Index < Hits.size(); ++Index)
     {
          auto FieldIt = Hits[Index].Document.find(RankField);
          if (FieldIt == Hits[Index].Document.end())
          {
               continue;
          }

          double RankValue = 0.0;
          if (!TryParseDoubleValue(FieldIt->second, &RankValue))
          {
               continue;
          }

          ParsedRanks.push_back({Index, RankValue, 1.0});
          MinRank = std::min(MinRank, RankValue);
          MaxRank = std::max(MaxRank, RankValue);
     }

     if (ParsedRanks.empty())
     {
          return;
     }

     auto BaseScoreForHit = [](const SearchHit &Hit) -> double
     {
          const double BaseScore = static_cast<double>(Hit.HybridScore > 0.0f ? Hit.HybridScore : (Hit.VectorScore > 0.0f ? Hit.VectorScore : Hit.TextMatch));
          return std::isfinite(BaseScore) && BaseScore > 0.0 ? BaseScore : 0.0;
     };

     auto ApplyNormalizedRankFusion = [&](double RankShare)
     {
          if (ParsedRanks.empty())
          {
               return;
          }

          RankShare = std::clamp(RankShare, 0.0, 0.95);

          double BaseMin = std::numeric_limits<double>::max();
          double BaseMax = std::numeric_limits<double>::lowest();

          for (const ParsedRank &Parsed : ParsedRanks)
          {
               const double BaseScore = BaseScoreForHit(Hits[Parsed.Index]);
               BaseMin = std::min(BaseMin, BaseScore);
               BaseMax = std::max(BaseMax, BaseScore);
          }

          if (BaseMax <= 0.0 || !std::isfinite(BaseMax))
          {
               return;
          }

          const double BaseRange = BaseMax - BaseMin;

          for (const ParsedRank &Parsed : ParsedRanks)
          {
               const double BaseScore = BaseScoreForHit(Hits[Parsed.Index]);
               const double RelevanceSignal = BaseRange > 0.0 ? ((BaseScore - BaseMin) / BaseRange) : 1.0;
               const double BlendedSignal = ((1.0 - RankShare) * RelevanceSignal) + (RankShare * Parsed.Signal);
               const double TargetScore = std::max(0.0, BaseMax * BlendedSignal);
               const double Multiplier = BaseScore > 0.0 ? std::clamp(TargetScore / BaseScore, 0.05, 10.0) : 1.0;

               if (std::isfinite(Multiplier) && Multiplier > 0.0)
               {
                    Hits[Parsed.Index].Weight *= static_cast<float>(Multiplier);
               }
          }
     };

     const double Range = MaxRank - MinRank;
     for (ParsedRank &Parsed : ParsedRanks)
     {
          double Signal = 1.0;
          if (Range > 0.0)
          {
               Signal = Descending
                    ? ((Parsed.Value - MinRank) / Range)
                    : ((MaxRank - Parsed.Value) / Range);
          }

          Parsed.Signal = std::clamp(Signal, 0.0, 1.0);
          Hits[Parsed.Index].Document["rank_signal"] = FormatRankSignalValue(Parsed.Signal);
     }

     if ((RankAlgorithm == "linear_algebra" || RankAlgorithm == "advanced_linear_algebra" || RankAlgorithm == "matrix") && ParsedRanks.size() >= 2)
     {
          ApplyNormalizedRankFusion(RankWeight);
          return;
     }

     if (RankAlgorithm == "spectral" && ParsedRanks.size() >= 2)
     {
          double Alpha = 0.85;
          const std::string AlphaRaw = MetadataValue("_rank_alpha");
          if (!AlphaRaw.empty())
          {
               double ParsedAlpha = 0.0;
               if (TryParseDoubleValue(AlphaRaw, &ParsedAlpha) && ParsedAlpha > 0.0 && ParsedAlpha < 1.0)
               {
                    Alpha = ParsedAlpha;
               }
          }

          double Beta = 4.0;
          const std::string BetaRaw = MetadataValue("_rank_beta");
          if (!BetaRaw.empty())
          {
               double ParsedBeta = 0.0;
               if (TryParseDoubleValue(BetaRaw, &ParsedBeta) && ParsedBeta > 0.0 && std::isfinite(ParsedBeta))
               {
                    Beta = std::min(ParsedBeta, 20.0);
               }
          }

          const size_t Count = ParsedRanks.size();
          std::vector<double> BaseSignals(Count, 0.0);
          double BaseMin = std::numeric_limits<double>::max();
          double BaseMax = std::numeric_limits<double>::lowest();

          for (size_t I = 0; I < Count; ++I)
          {
               BaseSignals[I] = BaseScoreForHit(Hits[ParsedRanks[I].Index]);
               BaseMin = std::min(BaseMin, BaseSignals[I]);
               BaseMax = std::max(BaseMax, BaseSignals[I]);
          }

          const double BaseRange = BaseMax - BaseMin;
          std::vector<double> Personalization(Count, 0.0);
          double PersonalizationSum = 0.0;

          for (size_t I = 0; I < Count; ++I)
          {
               const double RelevanceSignal = BaseRange > 0.0 ? ((BaseSignals[I] - BaseMin) / BaseRange) : 1.0;
               Personalization[I] = std::max(1e-9, (0.70 * RelevanceSignal) + (0.30 * ParsedRanks[I].Signal));
               PersonalizationSum += Personalization[I];
          }

          for (double &Value : Personalization)
          {
               Value /= PersonalizationSum;
          }

          std::vector<double> State = Personalization;
          std::vector<double> Next(Count, 0.0);

          for (int Iteration = 0; Iteration < 32; ++Iteration)
          {
               std::fill(Next.begin(), Next.end(), 0.0);

               for (size_t From = 0; From < Count; ++From)
               {
                    double Denominator = 0.0;
                    for (size_t To = 0; To < Count; ++To)
                    {
                         Denominator += std::exp(Beta * (ParsedRanks[To].Signal - ParsedRanks[From].Signal));
                    }

                    if (Denominator <= 0.0 || !std::isfinite(Denominator))
                    {
                         continue;
                    }

                    for (size_t To = 0; To < Count; ++To)
                    {
                         const double Transition = std::exp(Beta * (ParsedRanks[To].Signal - ParsedRanks[From].Signal)) / Denominator;
                         Next[To] += Alpha * Transition * State[From];
                    }
               }

               double Delta = 0.0;
               for (size_t I = 0; I < Count; ++I)
               {
                    Next[I] += (1.0 - Alpha) * Personalization[I];
                    Delta += std::abs(Next[I] - State[I]);
               }

               State.swap(Next);
               if (Delta < 1e-8)
               {
                    break;
               }
          }

          const double MeanState = 1.0 / static_cast<double>(Count);
          for (size_t I = 0; I < Count; ++I)
          {
               const double RelativeInfluence = State[I] / MeanState;
               const double Multiplier = std::clamp(1.0 + (RankWeight * (RelativeInfluence - 1.0)), 0.05, 1.0 + (RankWeight * 4.0));
               if (std::isfinite(Multiplier) && Multiplier > 0.0)
               {
                    Hits[ParsedRanks[I].Index].Weight *= static_cast<float>(Multiplier);
               }
          }

          return;
     }

     ApplyNormalizedRankFusion(RankWeight);
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
