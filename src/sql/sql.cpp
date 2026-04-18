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

bool SQLService::LooksLikeSelect(const std::string &sql_text) const
{
     const std::string trimmed = SQLTrimWhitespace(sql_text);

     if (trimmed.size() < 6)
     {
          return false;
     }

     return SQLToUpperASCII(trimmed.substr(0, 6)) == "SELECT";
}

SQLTranslationResult SQLService::ParseSelect(const std::string &sql_text) const
{
     return Parse(sql_text);
}
