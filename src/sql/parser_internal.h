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

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sql/internal.h"

/* Internal SQL parser implementation shared across translation units.
 * This header is not part of the public SQL API.
 */

class Parser
{
   private:

     /* Limits that prevent excessive parser work on hostile inputs. */

     static constexpr std::size_t SQLMaxASTNodes = 256;
     static constexpr std::size_t SQLMaxExpressionDepth = 32;
     static constexpr std::size_t SQLMaxInListValues = 100;

     std::vector<SQLToken> Tokens;
     std::size_t Position = 0;
     std::string CollectionName;
     std::string TableAlias;
     std::vector<std::string> SelectedFields;
     std::vector<SQLSelectField> SelectedOutputFields;
     std::vector<SQLAggregateSpec> CurrentAggregateSpecs;
     std::map<std::string, std::string> SelectAliases;
     nlohmann::json AggregateConfig = nlohmann::json::object();
     SQLASTStatement ParsedStatement;
     std::size_t ASTNodeCount = 0;
     std::size_t ExpressionDepth = 0;

     /* Converts the built AST into the public translation result structure. */

     static SQLTranslationResult BuildTranslationResultFromAST(const SQLTranslationResult &template_result,
                                                              const SQLASTStatement &statement,
                                                              const nlohmann::json &aggregate_config);

     bool TryConsumeASTNode(const std::string &label, std::string *error);
     bool EnterExpressionGroup(std::string *error);
     void LeaveExpressionGroup();

     bool AtEnd() const;
     const SQLToken *Peek() const;
     const SQLToken *PeekAt(std::size_t offset) const;
     std::string PeekText() const;
     std::string PeekUpper() const;
     const SQLToken *Advance();

     bool MatchKeyword(const std::string &keyword);
     bool IsClauseKeyword(const std::string &keyword) const;
     bool PeekStartsClause() const;
     static bool CanReadClause(const std::string &clause_name,
                               int clause_rank,
                               int &current_rank,
                               std::set<std::string> &seen_clauses,
                               std::string &error);

     bool ParseSelectList(SQLTranslationResult &result);
     bool ParseCollection(SQLTranslationResult &result);
     bool ParseWhere(SQLTranslationResult &result);
     bool ParseHaving(SQLTranslationResult &result);
     bool ParseGroupBy(SQLTranslationResult &result);
     bool ParseOrderBy(SQLTranslationResult &result);

     bool ParseExpression(std::string &out, std::string *error, bool allow_aggregate_refs);
     bool ParseAndExpression(std::string &out, std::string *error, bool allow_aggregate_refs);
     bool ParsePrimaryExpression(std::string &out, std::string *error, bool allow_aggregate_refs);
     bool ParseComparison(std::string &out, std::string *error, bool allow_aggregate_refs);

     bool ParseIdentifier(std::string &out);
     bool ParseFieldReference(std::string &out, std::string *error);
     bool ParsePredicateReference(std::string &out, std::string *error, bool allow_aggregate_refs);
     bool ParseValue(std::string &out);

     bool ParseAggregateSelectItem(SQLTranslationResult &result);
     bool ParseAggregateReference(std::string &out, std::string *error);
     bool ParseOptionalAlias(std::string &alias_name);
     bool ParseOrderingReference(std::string &out, std::string *error);

     std::string ToLowerASCII(std::string value) const;
     std::string ToLowerAggregateName(const std::string &function_name,
                                      const std::string &field_name,
                                      bool count_all,
                                      bool distinct_values) const;

     bool IsAggregateSelectItem() const;

     bool ParseLimitClause(int &per_page, int &offset, bool &has_offset, std::string *error);
     bool ParseFetchClause(int &per_page, std::string *error);

     bool ParseBetween(const std::string &field_name, bool negate, std::string &out, std::string *error);
     bool ParseLike(const std::string &field_name, bool case_insensitive, bool negate, std::string &out, std::string *error);
     bool ParseContains(const std::string &field_name, bool negate, std::string &out, std::string *error);
     bool ParseIsPredicate(const std::string &field_name, std::string &out, std::string *error);
     bool ParseInList(const std::string &field_name, bool negate, std::string &out, std::string *error);

     std::string ConvertSQLLikePattern(const std::string &value);
     bool ValidateFilterLiteral(const std::string &value, std::string *error);
     bool BuildRenderedComparison(const std::string &field_name,
                                  const std::string &op,
                                  const std::string &value,
                                  std::string &out,
                                  std::string *error);

     bool ParsePositiveInt(int &out, const std::string &label, std::string *error);
     bool ParseNonNegativeInt(int &out, const std::string &label, std::string *error);

     SQLTranslationResult ParseInsertStatement(SQLTranslationResult result);
     SQLTranslationResult ParseDeleteStatement(SQLTranslationResult result);
     SQLTranslationResult ParseUpdateStatement(SQLTranslationResult result);
     SQLTranslationResult ParseDropStatement(SQLTranslationResult result);

     bool ParseInsertValue(nlohmann::json &out, std::string *error);

   public:

     /* Creates a parser instance with a pre-tokenized query. */

     explicit Parser(std::vector<SQLToken> tokens);

     /* Parses a full SQL statement into a translation result. */

     SQLTranslationResult Parse();
};
