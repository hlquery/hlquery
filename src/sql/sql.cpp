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

#include <cctype>
#include "sql/internal.h"

bool SQLService::LooksLikeSelect(const std::string &sql_text) const
{
     std::size_t index = 0;

     while (index < sql_text.size() && std::isspace(static_cast<unsigned char>(sql_text[index])) != 0)
     {
          ++index;
     }

     if (index >= sql_text.size() || !SQLIsIdentifierStart(sql_text[index]))
     {
          return false;
     }

     const std::size_t start = index++;

     while (index < sql_text.size() && SQLIsIdentifierChar(sql_text[index]))
     {
          ++index;
     }

     return SQLToUpperASCII(sql_text.substr(start, index - start)) == "SELECT";
}

SQLTranslationResult SQLService::ParseSelect(const std::string &sql_text) const
{
     /* SELECT parsing shares the common parser but enforces the public entry point contract. */

     SQLTranslationResult result = Parse(sql_text);

     if (result.Valid && result.Type != SQLTranslationResult::StatementType::Select)
     {
          result.Valid = false;
          result.Error = "Expected SELECT statement.";
     }

     return result;
}
