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

/* Represents one lexical token extracted from a SQL statement. */

struct SQLToken
{
     /* Original token text as it appeared in the SQL statement. */

     std::string Text;

     /* Uppercase representation used for keyword comparisons. */

     std::string Upper;
};

/* Represents one ORDER BY expression. */

struct SQLASTOrderByItem
{
     /* Field or expression used to order the result. */

     std::string Field;

     /* Sort direction, normalized to asc or desc. */

     std::string Direction = "asc";
};

/* Represents one item in a SELECT list. */

struct SQLASTSelectItem
{
     /* Identifies the kind of expression represented by the item. */

     enum class ItemType
     {
          /* Selects every field from the source document. */

          Wildcard,

          /* Selects one field from the source document. */

          Field,

          /* Computes an aggregate from one or more source values. */

          Aggregate
     };

     /* Kind of SELECT item represented by this node. */

     ItemType Type = ItemType::Field;

     /* Source field referenced by the item. */

     std::string SourceName;

     /* Alias exposed for the translated result. */

     std::string OutputName;

     /* Aggregate function applied to the source field. */

     std::string FunctionName;

     /* Indicates whether the aggregate applies to every row. */

     bool CountAll = false;

     /* Indicates whether duplicate values are removed before aggregation. */

     bool Distinct = false;
};

/* Represents the parsed structure of one SQL statement. */

struct SQLASTStatement
{
     /* Statement category identified by the parser. */

     SQLTranslationResult::StatementType Type = SQLTranslationResult::StatementType::Unknown;

     /* Indicates whether parsing produced a valid statement. */

     bool Valid = false;

     /* Indicates whether duplicate result rows are removed. */

     bool Distinct = false;

     /* Indicates whether the statement explicitly supplied a limit. */

     bool HasExplicitLimit = false;

     /* Collection targeted by the statement. */

     std::string Collection;

     /* Filter expression applied before grouping or projection. */

     std::string FilterBy;

     /* Filter expression applied after aggregate grouping. */

     std::string HavingFilter;

     /* Maximum number of rows returned when no limit is specified. */

     int Limit = 10;

     /* Number of rows skipped before returning results. */

     int Offset = 0;

     /* Items requested by the SELECT list. */

     std::vector<SQLASTSelectItem> SelectItems;

     /* Fields used to group aggregate results. */

     std::vector<std::string> GroupBy;

     /* Expressions used to order the result set. */

     std::vector<SQLASTOrderByItem> OrderBy;

     /* Document payload produced for INSERT statements. */

     nlohmann::json InsertDocument = nlohmann::json::object();
};

/* Removes leading and trailing whitespace from a SQL value. */

std::string SQLTrimWhitespace(const std::string &value);

/* Converts ASCII letters in a string to uppercase. */

std::string SQLToUpperASCII(std::string value);

/* Checks whether a character may begin an SQL identifier. */

bool SQLIsIdentifierStart(char character);

/* Checks whether a character may continue an SQL identifier. */

bool SQLIsIdentifierChar(char character);

/* Checks whether a string contains a valid numeric literal. */

bool SQLIsNumericLiteral(const std::string &value);

/* Removes matching SQL identifier or string quotes. */

std::string SQLStripQuotes(const std::string &value);

/* Checks whether a filter literal is safe to use in a translated query. */

bool SQLIsSafeFilterLiteral(const std::string &value);

/* Converts SQL source text into lexical tokens and reports parsing errors. */

std::vector<SQLToken> SQLTokenize(const std::string &sql_text, std::string *error);
