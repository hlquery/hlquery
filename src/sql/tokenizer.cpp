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

/* Hard safety limits for tokenization and basic syntax sanity checks. */

static constexpr size_t SQLMaxQueryBytes = 32768;
static constexpr size_t SQLMaxTokens = 1024;
static constexpr size_t SQLMaxParenthesisDepth = 64;
static constexpr size_t SQLMaxIdentifierBytes = 256;
static constexpr size_t SQLMaxQuotedLiteralBytes = 4096;

bool CanStartSignedNumber(const std::vector<SQLToken> &tokens)
{
     /* A leading + or - is treated as part of a numeric literal only in contexts where a value is expected. */

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
     /* Reject non-whitespace control characters early. */

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
     /* Keep worst-case parser work bounded by enforcing a maximum token count. */

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

std::string SQLTrimWhitespace(const std::string &value)
{
     /* Removes leading and trailing ASCII whitespace. */

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
     /* Converts ASCII letters to upper-case for keyword matching. */

     std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                    {
                         return static_cast<char>(std::toupper(character));
                    });
     return value;
}

bool SQLIsIdentifierStart(char character)
{
     /* Allows unquoted identifiers similar to common SQL dialects. */

     return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '$';
}

bool SQLIsIdentifierChar(char character)
{
     /* Allows a restricted identifier alphabet that is compatible with hlquery fields. */

     return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
            character == '_' || character == '$' || character == '-' || character == '.';
}

bool SQLIsNumericLiteral(const std::string &value)
{
     /* Uses libc parsing rules; the parser later decides how to render numeric values. */

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
     /* Strips matching quotes when a token is already known to be quoted. */

     if (value.size() >= 2)
     {
          const char first = value.front();
          const char last = value.back();

          if ((first == '\'' && last == '\'') || (first == '"' && last == '"') || (first == '`' && last == '`'))
          {
               const std::string inner = value.substr(1, value.size() - 2);

               if (first == '`')
               {
                    return inner;
               }

               std::string unescaped;
               unescaped.reserve(inner.size());

               for (size_t index = 0; index < inner.size(); ++index)
               {
                    const char character = inner[index];

                    if (character == first && index + 1 < inner.size() && inner[index + 1] == first)
                    {
                         unescaped.push_back(first);
                         ++index;
                         continue;
                    }

                    unescaped.push_back(character);
               }

               return unescaped;
          }
     }

     return value;
}

bool SQLIsSafeFilterLiteral(const std::string &value)
{
     /* Prevents accidental injection of hlquery filter syntax via SQL string literals. */

     for (char character : value)
     {
          if (character == '&' || character == '|' ||
              character == '[' || character == ']' ||
              character == '{' || character == '}' ||
              character == '(' || character == ')' ||
              character == ',' || character == ':')
          {
               return false;
          }
     }

     return true;
}

std::vector<SQLToken> SQLTokenize(const std::string &sql_text, std::string *error)
{
     /* Converts a SQL query into a token stream suitable for the SQL parser. */

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

          /* Skip whitespace without producing tokens. */

          if (std::isspace(static_cast<unsigned char>(character)) != 0)
          {
               ++index;
               continue;
          }

          /* Quoted literals keep their quotes, so downstream code can distinguish types safely. */

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

               /* Store an upper-cased variant for keyword matching (quoted tokens are matched by Text). */

               tokens.push_back({token_text, SQLToUpperASCII(token_text)});
               continue;
          }

          /* Identifiers include keywords and field names. */

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

          /* Numeric literals are parsed as a single token, including an optional leading sign. */

          if (std::isdigit(static_cast<unsigned char>(character)) != 0 ||
              ((character == '-' || character == '+') &&
               index + 1 < sql_text.size() &&
               std::isdigit(static_cast<unsigned char>(sql_text[index + 1])) != 0 &&
               CanStartSignedNumber(tokens)))
          {
               size_t start = index++;
               bool seen_dot = false;

               while (index < sql_text.size())
               {
                    const char current = sql_text[index];

                    if (std::isdigit(static_cast<unsigned char>(current)) != 0)
                    {
                         ++index;
                         continue;
                    }

                    if (current == '.')
                    {
                         if (seen_dot)
                         {
                              break;
                         }

                         seen_dot = true;
                         ++index;
                         continue;
                    }

                    break;
               }

               if (index < sql_text.size() && (sql_text[index] == 'e' || sql_text[index] == 'E'))
               {
                    ++index;

                    if (index < sql_text.size() && (sql_text[index] == '-' || sql_text[index] == '+'))
                    {
                         ++index;
                    }

                    const size_t exponent_start = index;

                    while (index < sql_text.size() && std::isdigit(static_cast<unsigned char>(sql_text[index])) != 0)
                    {
                         ++index;
                    }

                    if (index == exponent_start)
                    {
                         if (error)
                         {
                              *error = "Invalid numeric literal in SQL query.";
                         }

                         return {};
                    }
               }

               const std::string text = sql_text.substr(start, index - start);

               if (text == "-" || text == "+")
               {
                    if (error)
                    {
                         *error = "Invalid numeric literal in SQL query.";
                    }

                    return {};
               }

               if (!text.empty() && text.back() == '.')
               {
                    if (error)
                    {
                         *error = "Invalid numeric literal in SQL query.";
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

          /* Two-character operators must be recognized before consuming one-character operators. */

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
               /* Parenthesis depth is tracked to provide friendly syntax errors early. */

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
