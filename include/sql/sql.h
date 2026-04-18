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

struct SQLSelectField
{
     std::string SourceName;
     std::string OutputName;
};

struct SQLAggregateSpec
{
     std::string FunctionName;
     std::string FieldName;
     std::string OutputName;
     bool CountAll = false;
     bool Distinct = false;
};

struct SQLTranslationResult
{
     enum class StatementType
     {
          Unknown,
          Select,
          Insert,
          Delete,
          Update,
          Drop,
          ShowCollections
     };

     StatementType Type = StatementType::Unknown;
     bool Valid = false;
     bool AggregateOnly = false;
     bool GroupedAggregates = false;
     bool Distinct = false;
     bool HasExplicitLimit = false;
     std::string Error;
     std::string Collection;
     std::string HavingFilter;
     nlohmann::json InsertDocument = nlohmann::json::object();
     ComprehensiveSearchQuery Query;
     std::vector<SQLSelectField> SelectFields;
     std::vector<SQLAggregateSpec> AggregateSpecs;
};

class SQLService
{
   public:

     SQLTranslationResult Parse(const std::string &sql_text) const;
     SQLTranslationResult ParseSelect(const std::string &sql_text) const;
     bool LooksLikeSelect(const std::string &sql_text) const;
};
