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

#include "sql/internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace
{
static constexpr size_t SQLMaxQueryBytes = 32768;
static constexpr size_t SQLMaxTokens = 1024;
static constexpr size_t SQLMaxParenthesisDepth = 64;
static constexpr size_t SQLMaxIdentifierBytes = 256;
static constexpr size_t SQLMaxQuotedLiteralBytes = 4096;

bool CanStartSignedNumber(const std::vector<SQLToken> &tokens)
{
     if (tokens.empty())
     {
          return true;
     }

     const std::string &upper = tokens.back().Upper;
     const std::string &text = tokens.back().Text;

     return upper == "LIMIT" || upper == "OFFSET" || upper == "BETWEEN" || upper == "AND" ||
            text == "(" || text == "," || text == "=" || text == "!=" || text == "<>" ||
            text == ">" || text == ">=" || text == "<" || text == "<=";
}

static bool SQLContainsDisallowedControlCharacters(const std::string &sql_text, std::string *error)
{
     for (size_t index = 0; index < sql_text.size(); ++index)
     {
          const unsigned char character = static_cast<unsigned char>(sql_text[index]);

          if ((character < 0x20U && std::isspace(character) == 0) || character == 0x7FU)
          {
               if (error)
               {
                    *error = "SQL query contains disallowed control characters.";
               }

               return true;
          }
     }

     return false;
}

static bool SQLCanAppendToken(const std::vector<SQLToken> &tokens, std::string *error)
{
     if (tokens.size() >= SQLMaxTokens)
     {
          if (error)
          {
               *error = "SQL query exceeds the maximum supported token count.";
          }

          return false;
     }

     return true;
}
}

std::string SQLTrimWhitespace(const std::string &value)
{
     size_t start = 0;

     while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
     {
          ++start;
     }

     size_t end = value.size();

     while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
     {
          --end;
     }

     return value.substr(start, end - start);
}

std::string SQLToUpperASCII(std::string value)
{
     std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                    { return static_cast<char>(std::toupper(character)); });
     return value;
}

bool SQLIsIdentifierStart(char character)
{
     return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '$';
}

bool SQLIsIdentifierChar(char character)
{
     return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
            character == '_' || character == '$' || character == '-' || character == '.';
}

bool SQLIsNumericLiteral(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     char *end_ptr = nullptr;
     std::strtod(value.c_str(), &end_ptr);
     return end_ptr != nullptr && *end_ptr == '\0';
}

std::string SQLStripQuotes(const std::string &value)
{
     if (value.size() >= 2)
     {
          const char first = value.front();
          const char last = value.back();

          if ((first == '\'' && last == '\'') || (first == '"' && last == '"') || (first == '`' && last == '`'))
          {
               return value.substr(1, value.size() - 2);
          }
     }

     return value;
}

bool SQLIsSafeFilterLiteral(const std::string &value)
{
     for (char character : value)
     {
          if (character == '&' || character == '|' || character == '[' || character == ']' || character == '{' || character == '}' || character == ',')
          {
               return false;
          }
     }

     return true;
}

std::vector<SQLToken> SQLTokenize(const std::string &sql_text, std::string *error)
{
     std::vector<SQLToken> tokens;
     size_t parenthesis_depth = 0;

     if (sql_text.size() > SQLMaxQueryBytes)
     {
          if (error)
          {
               *error = "SQL query exceeds the maximum supported length.";
          }

          return {};
     }

     if (SQLContainsDisallowedControlCharacters(sql_text, error))
     {
          return {};
     }

     for (size_t index = 0; index < sql_text.size();)
     {
          const char character = sql_text[index];

          if (std::isspace(static_cast<unsigned char>(character)) != 0)
          {
               ++index;
               continue;
          }

          if (character == '\'' || character == '"' || character == '`')
          {
               std::string token_text;
               token_text.push_back(character);
               ++index;
               bool closed = false;

               while (index < sql_text.size())
               {
                    const char current = sql_text[index++];
                    token_text.push_back(current);

                    if (current == character)
                    {
                         if (index < sql_text.size() && sql_text[index] == character && character != '`')
                         {
                              token_text.push_back(sql_text[index++]);
                              continue;
                         }

                         closed = true;
                         break;
                    }
               }

               if (!closed)
               {
                    if (error)
                    {
                         *error = "Unterminated quoted string in SQL query.";
                    }

                    return {};
               }

               if (token_text.size() > SQLMaxQuotedLiteralBytes)
               {
                    if (error)
                    {
                         *error = "SQL quoted literal exceeds the maximum supported length.";
                    }

                    return {};
               }

               if (!SQLCanAppendToken(tokens, error))
               {
                    return {};
               }

               tokens.push_back({token_text, SQLToUpperASCII(token_text)});
               continue;
          }

          if (SQLIsIdentifierStart(character))
          {
               size_t start = index++;

               while (index < sql_text.size() && SQLIsIdentifierChar(sql_text[index]))
               {
                    ++index;
               }

               const std::string text = sql_text.substr(start, index - start);

               if (text.size() > SQLMaxIdentifierBytes)
               {
                    if (error)
                    {
                         *error = "SQL identifier exceeds the maximum supported length.";
                    }

                    return {};
               }

               if (!SQLCanAppendToken(tokens, error))
               {
                    return {};
               }

               tokens.push_back({text, SQLToUpperASCII(text)});
               continue;
          }

          if (std::isdigit(static_cast<unsigned char>(character)) != 0 ||
              ((character == '-' || character == '+') &&
               index + 1 < sql_text.size() &&
               std::isdigit(static_cast<unsigned char>(sql_text[index + 1])) != 0 &&
               CanStartSignedNumber(tokens)))
          {
               size_t start = index++;

               while (index < sql_text.size())
               {
                    const char current = sql_text[index];

                    if (std::isdigit(static_cast<unsigned char>(current)) != 0 || current == '.')
                    {
                         ++index;
                         continue;
                    }

                    break;
               }

               const std::string text = sql_text.substr(start, index - start);

               if (!SQLCanAppendToken(tokens, error))
               {
                    return {};
               }

               tokens.push_back({text, SQLToUpperASCII(text)});
               continue;
          }

          if (index + 1 < sql_text.size())
          {
               const std::string two_char = sql_text.substr(index, 2);

               if (two_char == ">=" || two_char == "<=" || two_char == "!=" || two_char == "<>")
               {
                    if (!SQLCanAppendToken(tokens, error))
                    {
                         return {};
                    }

                    tokens.push_back({two_char, two_char});
                    index += 2;
                    continue;
               }
          }

          if (character == ',' || character == '(' || character == ')' || character == '*' ||
              character == ';' || character == '=' || character == '>' || character == '<')
          {
               if (character == '(')
               {
                    ++parenthesis_depth;

                    if (parenthesis_depth > SQLMaxParenthesisDepth)
                    {
                         if (error)
                         {
                              *error = "SQL query exceeds the maximum supported parenthesis depth.";
                         }

                         return {};
                    }
               }
               else if (character == ')')
               {
                    if (parenthesis_depth == 0)
                    {
                         if (error)
                         {
                              *error = "SQL query contains an unmatched closing parenthesis.";
                         }

                         return {};
                    }

                    --parenthesis_depth;
               }

               const std::string text(1, character);

               if (!SQLCanAppendToken(tokens, error))
               {
                    return {};
               }

               tokens.push_back({text, SQLToUpperASCII(text)});
               ++index;
               continue;
          }

          if (error)
          {
               *error = "Unsupported SQL token near '" + std::string(1, character) + "'.";
          }

          return {};
     }

     if (parenthesis_depth != 0)
     {
          if (error)
          {
               *error = "SQL query contains unmatched parentheses.";
          }

          return {};
     }

     return tokens;
}
