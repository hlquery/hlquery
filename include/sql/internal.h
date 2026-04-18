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

#include <string>
#include <vector>

#include "sql/sql.h"

struct SQLToken
{
     std::string Text;
     std::string Upper;
};

struct SQLASTOrderByItem
{
     std::string Field;
     std::string Direction = "asc";
};

struct SQLASTSelectItem
{
     enum class ItemType
     {
          Wildcard,
          Field,
          Aggregate
     };

     ItemType Type = ItemType::Field;
     std::string SourceName;
     std::string OutputName;
     std::string FunctionName;
     bool CountAll = false;
     bool Distinct = false;
};

struct SQLASTStatement
{
     SQLTranslationResult::StatementType Type = SQLTranslationResult::StatementType::Unknown;
     bool Valid = false;
     bool Distinct = false;
     bool HasExplicitLimit = false;
     std::string Collection;
     std::string FilterBy;
     std::string HavingFilter;
     int Limit = 10;
     int Offset = 0;
     std::vector<SQLASTSelectItem> SelectItems;
     std::vector<std::string> GroupBy;
     std::vector<SQLASTOrderByItem> OrderBy;
     nlohmann::json InsertDocument = nlohmann::json::object();
};

std::string SQLTrimWhitespace(const std::string &value);
std::string SQLToUpperASCII(std::string value);
bool SQLIsIdentifierStart(char character);
bool SQLIsIdentifierChar(char character);
bool SQLIsNumericLiteral(const std::string &value);
std::string SQLStripQuotes(const std::string &value);
bool SQLIsSafeFilterLiteral(const std::string &value);
std::vector<SQLToken> SQLTokenize(const std::string &sql_text, std::string *error);
