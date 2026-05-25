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

#include "api/searchapi.h"

/* Describes one projected field in a translated SELECT statement. */

struct SQLSelectField
{
     /* Field name read from the source collection. */

     std::string SourceName;

     /* Field name exposed in the translated output. */

     std::string OutputName;
};

/* Describes one aggregate expression requested by a SQL query. */

struct SQLAggregateSpec
{
     /* Aggregate function name such as count, sum, min, or max. */

     std::string FunctionName;

     /* Source field consumed by the aggregate function. */

     std::string FieldName;

     /* Output field name assigned to the aggregate result. */

     std::string OutputName;

     /* Tracks whether the aggregate is COUNT(*). */

     bool CountAll = false;

     /* Tracks whether DISTINCT applies to the aggregate input. */

     bool Distinct = false;
};

/* Holds the public result produced by SQL parsing and translation. */

struct SQLTranslationResult
{
     /* Lists the SQL statement categories supported by the translator. */

     enum class StatementType
     {
          /* No recognized statement type was found. */

          Unknown,

          /* Statement reads documents from a collection. */

          Select,

          /* Statement inserts one document into a collection. */

          Insert,

          /* Statement deletes documents from a collection. */

          Delete,

          /* Statement updates documents in a collection. */

          Update,

          /* Statement drops a collection. */

          Drop,

          /* Statement lists available collections. */

          ShowCollections
     };

     /* Statement category selected by the parser. */

     StatementType Type = StatementType::Unknown;

     /* Indicates whether parsing and translation completed successfully. */

     bool Valid = false;

     /* Indicates whether the query returns only aggregate values. */

     bool AggregateOnly = false;

     /* Indicates whether aggregate values are grouped by one or more fields. */

     bool GroupedAggregates = false;

     /* Indicates whether DISTINCT applies to the selected rows. */

     bool Distinct = false;

     /* Indicates whether the SQL statement supplied an explicit limit. */

     bool HasExplicitLimit = false;

     /* Error message produced when translation fails. */

     std::string Error;

     /* Collection targeted by the translated statement. */

     std::string Collection;

     /* Filter expression applied after aggregate grouping. */

     std::string HavingFilter;

     /* Document payload produced for INSERT statements. */

     nlohmann::json InsertDocument = nlohmann::json::object();

     /* Search API query generated from the SQL statement. */

     ComprehensiveSearchQuery Query;

     /* Non-aggregate fields requested by the SELECT list. */

     std::vector<SQLSelectField> SelectFields;

     /* Aggregate expressions requested by the SELECT list. */

     std::vector<SQLAggregateSpec> AggregateSpecs;
};

/* Provides the public SQL parsing entry points. */

class SQLService
{
   public:

     /* Parses any supported SQL statement into a translation result. */

     SQLTranslationResult Parse(const std::string &sql_text) const;

     /* Parses a SELECT statement into a search query translation result. */

     SQLTranslationResult ParseSelect(const std::string &sql_text) const;

     /* Checks whether a SQL string appears to begin with a SELECT statement. */

     bool LooksLikeSelect(const std::string &sql_text) const;
};
