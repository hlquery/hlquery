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

#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vendor/json/json.hpp>

static SQLTranslationResult BuildTranslationResultFromAST(const SQLTranslationResult &template_result,
                                                          const SQLASTStatement &statement,
                                                          const nlohmann::json &aggregate_config)
{
     SQLTranslationResult result = template_result;

     result.Type = statement.Type;
     result.Valid = statement.Valid;
     result.Distinct = statement.Distinct;
     result.HasExplicitLimit = statement.HasExplicitLimit;
     result.Collection = statement.Collection;
     result.HavingFilter = statement.HavingFilter;
     result.InsertDocument = statement.InsertDocument;
     result.Query.FilterBy = statement.FilterBy;
     result.Query.PerPage = statement.Limit;
     result.Query.Offset = statement.Offset;
     result.Query.Page = (statement.Limit > 0) ? ((statement.Offset / statement.Limit) + 1) : 1;
     result.Query.GroupBy = statement.GroupBy;
     result.Query.SortBy.clear();
     result.SelectFields.clear();
     result.AggregateSpecs.clear();

     for (const auto &OrderByItem : statement.OrderBy)
     {
          result.Query.SortBy.push_back(OrderByItem.Field + ":" + OrderByItem.Direction);
     }

     for (const auto &SelectItem : statement.SelectItems)
     {
          if (SelectItem.Type == SQLASTSelectItem::ItemType::Field)
          {
               result.Query.IncludeFields.push_back(SelectItem.SourceName);
               result.SelectFields.push_back({SelectItem.SourceName, SelectItem.OutputName});
               continue;
          }

          if (SelectItem.Type == SQLASTSelectItem::ItemType::Aggregate)
          {
               result.AggregateSpecs.push_back({SelectItem.FunctionName,
                                                SelectItem.SourceName,
                                                SelectItem.OutputName,
                                                SelectItem.CountAll,
                                                SelectItem.Distinct});
          }
     }

     if (!result.AggregateSpecs.empty())
     {
          result.Query.Aggregations = aggregate_config.dump();
     }

     return result;
}

class Parser
{
   public:

     explicit Parser(std::vector<SQLToken> tokens)
          : Tokens(std::move(tokens))
     {

     }

     SQLTranslationResult Parse()
     {
          SQLTranslationResult result;
          if (MatchKeyword("SELECT"))
          {
               result.Type = SQLTranslationResult::StatementType::Select;
               ParsedStatement.Type = SQLTranslationResult::StatementType::Select;
          }
          else if (MatchKeyword("DELETE"))
          {
               result.Type = SQLTranslationResult::StatementType::Delete;
               ParsedStatement.Type = SQLTranslationResult::StatementType::Delete;
               return ParseDeleteStatement(std::move(result));
          }
          else if (MatchKeyword("INSERT"))
          {
               result.Type = SQLTranslationResult::StatementType::Insert;
               ParsedStatement.Type = SQLTranslationResult::StatementType::Insert;
               return ParseInsertStatement(std::move(result));
          }
          else if (MatchKeyword("UPDATE"))
          {
               result.Type = SQLTranslationResult::StatementType::Update;
               ParsedStatement.Type = SQLTranslationResult::StatementType::Update;
               return ParseUpdateStatement(std::move(result));
          }
          else if (MatchKeyword("DROP"))
          {
               result.Type = SQLTranslationResult::StatementType::Drop;
               ParsedStatement.Type = SQLTranslationResult::StatementType::Drop;
               return ParseDropStatement(std::move(result));
          }
          else if (MatchKeyword("SHOW"))
          {
               if (MatchKeyword("COLS") || MatchKeyword("COLLECTIONS"))
               {
                    result.Type = SQLTranslationResult::StatementType::ShowCollections;
                    ParsedStatement.Type = SQLTranslationResult::StatementType::ShowCollections;
                    result.Valid = true;

                    if (PeekText() == ";")
                    {
                         Advance();
                    }

                    if (!AtEnd())
                    {
                         result.Valid = false;
                         result.Error = "Unexpected trailing tokens after ';'.";
                         return result;
                    }

                    return result;
               }

               result.Error = "Unsupported SQL statement 'SHOW'.";
               return result;
          }
          else
          {
               result.Error = "Only SELECT, INSERT, DELETE, UPDATE, DROP, and SHOW statements are recognized by hlquery SQL.";
               return result;
          }

          result.Distinct = MatchKeyword("DISTINCT");
          ParsedStatement.Distinct = result.Distinct;

          if (!ParseSelectList(result))
          {
               return result;
          }

          if (!MatchKeyword("FROM"))
          {
               result.Error = "SQL query must include FROM <collection>.";
               return result;
          }

          if (!ParseCollection(result))
          {
               return result;
          }

          result.Query.Q = "*";
          result.Query.Highlight = false;

          while (!AtEnd())
          {
               if (MatchKeyword("WHERE"))
               {
                    if (!ParseWhere(result))
                    {
                         return result;
                    }
                    continue;
               }

               if (MatchKeyword("ORDER"))
               {
                    if (!MatchKeyword("BY"))
                    {
                         result.Error = "Expected BY after ORDER.";
                         return result;
                    }

                    if (!ParseOrderBy(result))
                    {
                         return result;
                    }

                    continue;
               }

               if (MatchKeyword("GROUP"))
               {
                    if (!MatchKeyword("BY"))
                    {
                         result.Error = "Expected BY after GROUP.";
                         return result;
                    }

                    if (!ParseGroupBy(result))
                    {
                         return result;
                    }

                    continue;
               }

               if (MatchKeyword("HAVING"))
               {
                    if (!ParseHaving(result))
                    {
                         return result;
                    }

                    continue;
               }

               if (MatchKeyword("LIMIT"))
               {
                    result.HasExplicitLimit = true;
                    ParsedStatement.HasExplicitLimit = true;

                    int parsed_offset = 0;
                    bool has_parsed_offset = false;

                    if (!ParseLimitClause(result.Query.PerPage, parsed_offset, has_parsed_offset, &result.Error))
                    {
                         return result;
                    }

                    ParsedStatement.Limit = result.Query.PerPage;

                    if (has_parsed_offset)
                    {
                         result.Query.Offset = parsed_offset;
                         ParsedStatement.Offset = parsed_offset;
                    }

                    continue;
               }

               if (MatchKeyword("OFFSET"))
               {
                    if (!ParseNonNegativeInt(result.Query.Offset, "OFFSET", &result.Error))
                    {
                         return result;
                    }

                    ParsedStatement.Offset = result.Query.Offset;

                    if (PeekUpper() == "ROW" || PeekUpper() == "ROWS")
                    {
                         Advance();
                    }

                    continue;
               }

               if (MatchKeyword("FETCH"))
               {
                    result.HasExplicitLimit = true;

                    ParsedStatement.HasExplicitLimit = true;

                    if (!ParseFetchClause(result.Query.PerPage, &result.Error))
                    {
                         return result;
                    }

                    ParsedStatement.Limit = result.Query.PerPage;

                    continue;
               }

               if (PeekText() == ";")
               {
                    Advance();

                    if (!AtEnd())
                    {
                         result.Error = "Unexpected trailing tokens after ';'.";
                         return result;
                    }

                    break;
               }

               result.Error = "Unsupported SQL clause near '" + PeekText() + "'.";
               return result;
          }

          if (!result.AggregateSpecs.empty() && result.SelectFields.empty())
          {
               result.Query.Aggregations = AggregateConfig.dump();
               result.AggregateOnly = true;
               result.Query.PerPage = 1;
          }

          if (!result.AggregateSpecs.empty() && !result.SelectFields.empty())
          {
               if (result.Distinct)
               {
                    result.Error = "DISTINCT with aggregate SELECT expressions is not supported by hlquery SQL yet.";
                    return result;
               }

               if (result.Query.GroupBy.empty())
               {
                    result.Error = "Mixing aggregate expressions with regular selected fields requires GROUP BY in hlquery SQL.";
                    return result;
               }

               std::set<std::string> GroupByFields(result.Query.GroupBy.begin(), result.Query.GroupBy.end());
               for (const auto &Field : result.SelectFields)
               {
                    if (GroupByFields.find(Field.SourceName) == GroupByFields.end())
                    {
                         result.Error = "Selected field '" + Field.SourceName + "' must appear in GROUP BY when aggregate expressions are present.";
                         return result;
                    }
               }

               result.GroupedAggregates = true;
               result.Query.Aggregations = AggregateConfig.dump();

               for (const auto &AggSpec : result.AggregateSpecs)
               {
                    if (AggSpec.FunctionName == "STATS")
                    {
                         result.Error = "STATS() is not supported together with GROUP BY in hlquery SQL yet.";
                         return result;
                    }
               }
          }

          if (!result.GroupedAggregates)
          {
               for (auto &SortEntry : result.Query.SortBy)
               {
                    const size_t ColonPos = SortEntry.rfind(':');
                    const std::string SortField = (ColonPos == std::string::npos) ? SortEntry : SortEntry.substr(0, ColonPos);
                    const std::string SortDirection = (ColonPos == std::string::npos) ? "asc" : SortEntry.substr(ColonPos + 1);

                    for (const auto &Field : result.SelectFields)
                    {
                         if (Field.OutputName == SortField)
                         {
                              SortEntry = Field.SourceName + ":" + SortDirection;
                              break;
                         }
                    }
               }
          }

          ParsedStatement.Valid = true;
          return BuildTranslationResultFromAST(result, ParsedStatement, AggregateConfig);
     }

   private:
     static constexpr size_t SQLMaxASTNodes = 256;
     static constexpr size_t SQLMaxExpressionDepth = 32;
     static constexpr size_t SQLMaxInListValues = 100;

     std::vector<SQLToken> Tokens;
     size_t Position = 0;
     std::string CollectionName;
     std::string TableAlias;
     std::vector<std::string> SelectedFields;
     std::vector<SQLSelectField> SelectedOutputFields;
     std::vector<SQLAggregateSpec> CurrentAggregateSpecs;
     std::map<std::string, std::string> SelectAliases;
     nlohmann::json AggregateConfig = nlohmann::json::object();
     SQLASTStatement ParsedStatement;
     size_t ASTNodeCount = 0;
     size_t ExpressionDepth = 0;

     bool TryConsumeASTNode(const std::string &label, std::string *error)
     {
          ++ASTNodeCount;

          if (ASTNodeCount > SQLMaxASTNodes)
          {
               if (error)
               {
                    *error = "SQL query exceeds the maximum supported " + label + " count.";
               }

               return false;
          }

          return true;
     }

     bool EnterExpressionGroup(std::string *error)
     {
          ++ExpressionDepth;

          if (ExpressionDepth > SQLMaxExpressionDepth)
          {
               if (error)
               {
                    *error = "SQL query exceeds the maximum supported expression nesting depth.";
               }

               return false;
          }

          return true;
     }

     void LeaveExpressionGroup()
     {
          if (ExpressionDepth > 0)
          {
               --ExpressionDepth;
          }
     }

     SQLTranslationResult ParseInsertStatement(SQLTranslationResult result)
     {
          if (!MatchKeyword("INTO"))
          {
               result.Error = "SQL insert must use INSERT INTO <collection> (...) VALUES (...).";
               return result;
          }

          std::string collection_name;
          if (!ParseIdentifier(collection_name))
          {
               result.Error = "SQL INSERT must include a collection name after INTO.";
               return result;
          }

          result.Collection = collection_name;
          CollectionName = collection_name;
          ParsedStatement.Collection = collection_name;

          if (PeekText() != "(")
          {
               result.Error = "SQL INSERT must include a field list in parentheses.";
               return result;
          }

          Advance();

          std::vector<std::string> field_names;
          while (!AtEnd())
          {
               std::string field_name;
               if (!ParseIdentifier(field_name))
               {
                    result.Error = "Expected field name in INSERT field list.";
                    return result;
               }

               field_names.push_back(field_name);

               if (PeekText() == ",")
               {
                    Advance();
                    continue;
               }

               break;
          }

          if (field_names.empty())
          {
               result.Error = "INSERT field list cannot be empty.";
               return result;
          }

          if (PeekText() != ")")
          {
               result.Error = "Expected ')' after INSERT field list.";
               return result;
          }

          Advance();

          if (!MatchKeyword("VALUES"))
          {
               result.Error = "SQL INSERT must include VALUES (...).";
               return result;
          }

          if (PeekText() != "(")
          {
               result.Error = "Expected '(' after VALUES in SQL INSERT.";
               return result;
          }

          Advance();

          nlohmann::json insert_document = nlohmann::json::object();
          std::size_t value_index = 0;

          while (!AtEnd())
          {
               if (value_index >= field_names.size())
               {
                    result.Error = "INSERT VALUES count exceeds field count.";
                    return result;
               }

               nlohmann::json value;
               if (!ParseInsertValue(value, &result.Error))
               {
                    return result;
               }

               insert_document[field_names[value_index++]] = std::move(value);

               if (PeekText() == ",")
               {
                    Advance();
                    continue;
               }

               break;
          }

          if (PeekText() != ")")
          {
               result.Error = "Expected ')' after INSERT VALUES list.";
               return result;
          }

          Advance();

          if (value_index != field_names.size())
          {
               result.Error = "INSERT field count must match VALUES count.";
               return result;
          }

          if (PeekText() == ";")
          {
               Advance();

               if (!AtEnd())
               {
                    result.Error = "Unexpected trailing tokens after ';'.";
                    return result;
               }
          }
          else if (!AtEnd())
          {
               result.Error = "Unsupported SQL clause near '" + PeekText() + "'.";
               return result;
          }

          result.InsertDocument = std::move(insert_document);
          ParsedStatement.InsertDocument = result.InsertDocument;
          ParsedStatement.Valid = true;
          return BuildTranslationResultFromAST(result, ParsedStatement, AggregateConfig);
     }

     SQLTranslationResult ParseDeleteStatement(SQLTranslationResult result)
     {
          if (PeekText() == "*")
          {
               Advance();
          }

          if (!MatchKeyword("FROM"))
          {
               result.Error = "SQL delete must use DELETE FROM <collection> ...";
               return result;
          }

          if (!ParseCollection(result))
          {
               return result;
          }

          result.Query.Q = "*";
          result.Query.Highlight = false;

          while (!AtEnd())
          {
               if (MatchKeyword("WHERE"))
               {
                    if (!ParseWhere(result))
                    {
                         return result;
                    }
                    continue;
               }

               if (MatchKeyword("ORDER"))
               {
                    if (!MatchKeyword("BY"))
                    {
                         result.Error = "Expected BY after ORDER.";
                         return result;
                    }

                    if (!ParseOrderBy(result))
                    {
                         return result;
                    }

                    continue;
               }

               if (MatchKeyword("LIMIT"))
               {
                    result.HasExplicitLimit = true;
                    int parsed_offset = 0;
                    bool has_parsed_offset = false;

                    if (!ParseLimitClause(result.Query.PerPage, parsed_offset, has_parsed_offset, &result.Error))
                    {
                         return result;
                    }

                    if (has_parsed_offset)
                    {
                         result.Query.Offset = parsed_offset;
                         ParsedStatement.Offset = parsed_offset;
                    }

                    ParsedStatement.HasExplicitLimit = true;
                    ParsedStatement.Limit = result.Query.PerPage;

                    continue;
               }

               if (MatchKeyword("OFFSET"))
               {
                    int pending_offset = 0;

                    if (!ParseNonNegativeInt(pending_offset, "OFFSET", &result.Error))
                    {
                         return result;
                    }

                    if (PeekUpper() == "ROW" || PeekUpper() == "ROWS")
                    {
                         Advance();
                    }

                    result.Query.Offset = pending_offset;
                    ParsedStatement.Offset = pending_offset;
                    continue;
               }

               if (MatchKeyword("FETCH"))
               {
                    result.HasExplicitLimit = true;
                    if (!ParseFetchClause(result.Query.PerPage, &result.Error))
                    {
                         return result;
                    }

                    ParsedStatement.HasExplicitLimit = true;
                    ParsedStatement.Limit = result.Query.PerPage;

                    continue;
               }

               if (PeekText() == ";")
               {
                    Advance();

                    if (!AtEnd())
                    {
                         result.Error = "Unexpected trailing tokens after ';'.";
                         return result;
                    }

                    break;
               }

               result.Error = "Unsupported SQL clause near '" + PeekText() + "'.";
               return result;
          }

          ParsedStatement.Valid = true;
          return BuildTranslationResultFromAST(result, ParsedStatement, AggregateConfig);
     }

     SQLTranslationResult ParseUpdateStatement(SQLTranslationResult result)
     {
          std::string collection_name;

          if (!ParseIdentifier(collection_name))
          {
               result.Error = "SQL UPDATE must include a collection name.";
               return result;
          }

          result.Collection = collection_name;
          ParsedStatement.Collection = collection_name;

          if (PeekText() == ";")
          {
               Advance();
          }

          if (!AtEnd())
          {
               result.Error = "Unexpected trailing tokens after unsupported SQL UPDATE statement.";
               return result;
          }

          result.Error = "SQL UPDATE is not supported by hlquery yet.";
          return result;
     }

     SQLTranslationResult ParseDropStatement(SQLTranslationResult result)
     {
          if (MatchKeyword("TABLE") || MatchKeyword("COLLECTION"))
          {
          }

          std::string collection_name;

          if (!ParseIdentifier(collection_name))
          {
               result.Error = "SQL DROP must include a collection name.";
               return result;
          }

          result.Collection = collection_name;
          ParsedStatement.Collection = collection_name;

          if (PeekText() == ";")
          {
               Advance();
          }

          if (!AtEnd())
          {
               result.Error = "Unexpected trailing tokens after SQL DROP statement.";
               return result;
          }

          ParsedStatement.Valid = true;
          result.Valid = true;
          return result;
     }

     bool AtEnd() const
     {
          return Position >= Tokens.size();
     }

     const SQLToken *Peek() const
     {
          return AtEnd() ? nullptr : &Tokens[Position];
     }

     const SQLToken *PeekAt(size_t offset) const
     {
          const size_t index = Position + offset;
          return index >= Tokens.size() ? nullptr : &Tokens[index];
     }

     std::string PeekText() const
     {
          const SQLToken *token = Peek();
          return token ? token->Text : "";
     }

     std::string PeekUpper() const
     {
          const SQLToken *token = Peek();
          return token ? token->Upper : "";
     }

     const SQLToken *Advance()
     {
          if (AtEnd())
          {
               return nullptr;
          }

          return &Tokens[Position++];
     }

     bool MatchKeyword(const std::string &keyword)
     {
          if (PeekUpper() != keyword)
          {
               return false;
          }

          Advance();
          return true;
     }

     bool IsClauseKeyword(const std::string &keyword) const
     {
          return keyword == "FROM" || keyword == "WHERE" || keyword == "ORDER" || keyword == "GROUP" ||
                 keyword == "LIMIT" || keyword == "OFFSET" || keyword == "FETCH" || keyword == "HAVING" ||
                 keyword == "UNION" || keyword == "INTERSECT" || keyword == "EXCEPT" ||
                 keyword == "ASC" || keyword == "DESC" || keyword == "AND" || keyword == "OR";
     }

     bool PeekStartsClause() const
     {
          return IsClauseKeyword(PeekUpper()) || PeekText() == "," || PeekText() == ";" || PeekText() == ")";
     }

     bool ParseSelectList(SQLTranslationResult &result)
     {
          if (PeekText() == "*")
          {
               Advance();
               ParsedStatement.SelectItems.push_back({SQLASTSelectItem::ItemType::Wildcard, "", "*", "", false, false});
               return true;
          }

          while (!AtEnd())
          {
               if (IsAggregateSelectItem())
               {
                    if (!ParseAggregateSelectItem(result))
                    {
                         return false;
                    }
               }
               else
               {
                    std::string field_name;
                    std::string alias_name;

                    if (!ParseFieldReference(field_name, &result.Error))
                    {
                         result.Error = "Expected field list after SELECT.";
                         return false;
                    }

                    if (!ParseOptionalAlias(alias_name))
                    {
                         result.Error = "Expected alias name after AS.";
                         return false;
                    }

                    const std::string output_name = alias_name.empty() ? field_name : alias_name;

                    if (!TryConsumeASTNode("SELECT field", &result.Error))
                    {
                         return false;
                    }

                    result.Query.IncludeFields.push_back(field_name);
                    result.SelectFields.push_back({field_name, output_name});
                    SelectedFields.push_back(output_name);
                    SelectedOutputFields.push_back({field_name, output_name});
                    SelectAliases[SQLToUpperASCII(output_name)] = output_name;
                    ParsedStatement.SelectItems.push_back({SQLASTSelectItem::ItemType::Field, field_name, output_name, "", false, false});
               }

               if (PeekText() != ",")
               {
                    break;
               }

               Advance();
          }

          if (result.Query.IncludeFields.empty() && AggregateConfig.empty())
          {
               result.Error = "SELECT field list cannot be empty.";
               return false;
          }

          return true;
     }

     bool ParseCollection(SQLTranslationResult &result)
     {
          std::string collection_name;
          std::string alias_name;

          if (!ParseIdentifier(collection_name))
          {
               result.Error = "Expected collection name after FROM.";
               return false;
          }

          result.Collection = collection_name;
          CollectionName = collection_name;
          ParsedStatement.Collection = collection_name;

          if (!ParseOptionalAlias(alias_name))
          {
               result.Error = "Expected table alias after AS.";
               return false;
          }

          if (!alias_name.empty())
          {
               TableAlias = alias_name;
          }

          return true;
     }

     bool ParseWhere(SQLTranslationResult &result)
     {
          std::string filter_expression;

          if (!ParseExpression(filter_expression, &result.Error, false))
          {
               return false;
          }

          result.Query.FilterBy = filter_expression;
          ParsedStatement.FilterBy = filter_expression;
          return true;
     }

     bool ParseHaving(SQLTranslationResult &result)
     {
          if (result.AggregateSpecs.empty())
          {
               result.Error = "HAVING requires aggregate expressions in the SELECT list.";
               return false;
          }

          std::string filter_expression;

          if (!ParseExpression(filter_expression, &result.Error, true))
          {
               return false;
          }

          result.HavingFilter = filter_expression;
          ParsedStatement.HavingFilter = filter_expression;
          return true;
     }

     bool ParseGroupBy(SQLTranslationResult &result)
     {
          while (!AtEnd())
          {
               std::string field_name;

               if (!ParseOrderingReference(field_name, &result.Error))
               {
                    result.Error = "Expected field name in GROUP BY clause.";
                    return false;
               }

               for (const auto &Field : result.SelectFields)
               {
                    if (Field.OutputName == field_name)
                    {
                         field_name = Field.SourceName;
                         break;
                    }
               }

               if (!TryConsumeASTNode("GROUP BY item", &result.Error))
               {
                    return false;
               }

               result.Query.GroupBy.push_back(field_name);
               ParsedStatement.GroupBy.push_back(field_name);

               if (PeekText() != ",")
               {
                    break;
               }

               Advance();
          }

          return !result.Query.GroupBy.empty();
     }

     bool ParseOrderBy(SQLTranslationResult &result)
     {
          while (!AtEnd())
          {
               std::string field_name;

               if (!ParseOrderingReference(field_name, &result.Error))
               {
                    result.Error = "Expected field name in ORDER BY clause.";
                    return false;
               }

               std::string direction = "asc";

               if (PeekUpper() == "ASC" || PeekUpper() == "DESC")
               {
                    direction = SQLToUpperASCII(Advance()->Text) == "DESC" ? "desc" : "asc";
               }

               if (!TryConsumeASTNode("ORDER BY item", &result.Error))
               {
                    return false;
               }

               result.Query.SortBy.push_back(field_name + ":" + direction);
               ParsedStatement.OrderBy.push_back({field_name, direction});

               if (PeekText() != ",")
               {
                    break;
               }

               Advance();
          }

          return !result.Query.SortBy.empty();
     }

     bool ParseExpression(std::string &out, std::string *error, bool allow_aggregate_refs)
     {
          if (!ParseAndExpression(out, error, allow_aggregate_refs))
          {
               return false;
          }

          while (PeekUpper() == "OR")
          {
               Advance();
               std::string rhs;

               if (!ParseAndExpression(rhs, error, allow_aggregate_refs))
               {
                    return false;
               }

               out = out + "||" + rhs;
          }

          return true;
     }

     bool ParseAndExpression(std::string &out, std::string *error, bool allow_aggregate_refs)
     {
          if (!ParsePrimaryExpression(out, error, allow_aggregate_refs))
          {
               return false;
          }

          while (PeekUpper() == "AND")
          {
               Advance();
               std::string rhs;

               if (!ParsePrimaryExpression(rhs, error, allow_aggregate_refs))
               {
                    return false;
               }

               out = out + "&&" + rhs;
          }

          return true;
     }

     bool ParsePrimaryExpression(std::string &out, std::string *error, bool allow_aggregate_refs)
     {
          if (PeekUpper() == "NOT")
          {
               const SQLToken *next_token = PeekAt(1);
               if (next_token && next_token->Text == "(")
               {
                    Advance();

                    std::string inner;
                    if (!ParsePrimaryExpression(inner, error, allow_aggregate_refs))
                    {
                         return false;
                    }

                    out = "!" + inner;
                    return true;
               }
          }

          if (PeekText() == "(")
          {
               if (!EnterExpressionGroup(error))
               {
                    return false;
               }

               Advance();

               if (!ParseExpression(out, error, allow_aggregate_refs))
               {
                    LeaveExpressionGroup();
                    return false;
               }

               if (PeekText() != ")")
               {
                    LeaveExpressionGroup();

                    if (error)
                    {
                         *error = "Expected ')' to close WHERE expression.";
                    }
                    return false;
               }

               Advance();
               LeaveExpressionGroup();
               out = "(" + out + ")";
               return true;
          }

          return ParseComparison(out, error, allow_aggregate_refs);
     }

     bool ParseComparison(std::string &out, std::string *error, bool allow_aggregate_refs)
     {
          if (!TryConsumeASTNode("comparison", error))
          {
               return false;
          }

          bool negate = false;

          if (MatchKeyword("NOT"))
          {
               negate = true;
          }

          std::string field_name;

          if (!ParsePredicateReference(field_name, error, allow_aggregate_refs))
          {
               if (error)
               {
                    *error = allow_aggregate_refs ? "Expected field name or aggregate expression in HAVING clause."
                                                  : "Expected field name in WHERE clause.";
               }
               return false;
          }

          if (MatchKeyword("NOT"))
          {
               if (MatchKeyword("IN"))
               {
                    return ParseInList(field_name, true, out, error);
               }

               if (MatchKeyword("BETWEEN"))
               {
                    return ParseBetween(field_name, true, out, error);
               }

               if (MatchKeyword("LIKE"))
               {
                    return ParseLike(field_name, false, true, out, error);
               }

               if (MatchKeyword("ILIKE"))
               {
                    return ParseLike(field_name, true, true, out, error);
               }

               if (error)
               {
                    *error = "Unsupported SQL predicate after NOT for field '" + field_name + "'.";
               }
               return false;
          }

          if (MatchKeyword("IN"))
          {
               return ParseInList(field_name, false, out, error);
          }

          if (MatchKeyword("BETWEEN"))
          {
               return ParseBetween(field_name, false, out, error);
          }

          if (MatchKeyword("LIKE"))
          {
               return ParseLike(field_name, false, false, out, error);
          }

          if (MatchKeyword("ILIKE"))
          {
               return ParseLike(field_name, true, false, out, error);
          }

          if (MatchKeyword("IS"))
          {
               return ParseIsPredicate(field_name, out, error);
          }

          if (AtEnd())
          {
               if (error)
               {
                    *error = "Expected operator after field '" + field_name + "'.";
               }
               return false;
          }

          const SQLToken *op_token = Advance();
          std::string op = op_token ? op_token->Upper : "";

          if (op == "<>")
          {
               op = "!=";
          }

          if (op == "LIKE" || op == "IN")
          {
               if (error)
               {
                    *error = "Operator '" + op + "' is not supported by hlquery SQL yet.";
               }
               return false;
          }

          if (op != "=" && op != "!=" && op != ">" && op != ">=" && op != "<" && op != "<=")
          {
               if (error)
               {
                    *error = "Unsupported operator '" + op_token->Text + "' in WHERE clause.";
               }
               return false;
          }

          if (negate)
          {
               if (op == "=")
               {
                    op = "!=";
               }
               else if (op == "!=")
               {
                    op = "=";
               }
               else
               {
                    if (error)
                    {
                         *error = "NOT only supports equality comparisons in hlquery SQL.";
                    }
                    return false;
               }
          }

          std::string value;

          if (!ParseValue(value))
          {
               if (error)
               {
                    *error = "Expected comparison value after operator '" + op + "'.";
               }
               return false;
          }

          return BuildRenderedComparison(field_name, op, value, out, error);
     }

     bool ParseIdentifier(std::string &out)
     {
          if (AtEnd())
          {
               return false;
          }

          const SQLToken *token = Peek();

          if (!token)
          {
               return false;
          }

          if (!token->Text.empty() && token->Text.front() == '`')
          {
               out = SQLStripQuotes(token->Text);
               Advance();
               return !out.empty();
          }

          if (!token->Text.empty() && SQLIsIdentifierStart(token->Text.front()))
          {
               out = token->Text;
               Advance();
               return true;
          }

          return false;
     }

     bool ParseFieldReference(std::string &out, std::string *error)
     {
          std::string identifier;

          if (!ParseIdentifier(identifier))
          {
               return false;
          }

          const size_t dot_pos = identifier.find('.');

          if (dot_pos == std::string::npos)
          {
               out = identifier;
               return true;
          }

          const std::string qualifier = identifier.substr(0, dot_pos);
          const std::string field_name = identifier.substr(dot_pos + 1);

          if (field_name.empty())
          {
               if (error)
               {
                    *error = "Invalid qualified field reference '" + identifier + "'.";
               }
               return false;
          }

          if (TableAlias.empty() && CollectionName.empty())
          {
               out = field_name;
               return true;
          }

          if ((!TableAlias.empty() && qualifier == TableAlias) ||
              (!CollectionName.empty() && qualifier == CollectionName))
          {
               out = field_name;
               return true;
          }

          if (error)
          {
               *error = "Unknown table qualifier '" + qualifier + "' in field reference '" + identifier + "'.";
          }

          return false;
     }

     bool ParsePredicateReference(std::string &out, std::string *error, bool allow_aggregate_refs)
     {
          if (allow_aggregate_refs && IsAggregateSelectItem())
          {
               return ParseAggregateReference(out, error);
          }

          std::string identifier;

          if (!ParseIdentifier(identifier))
          {
               return false;
          }

          auto alias_it = SelectAliases.find(SQLToUpperASCII(identifier));
          if (alias_it != SelectAliases.end())
          {
               out = alias_it->second;
               return true;
          }

          const size_t dot_pos = identifier.find('.');

          if (dot_pos == std::string::npos)
          {
               if (allow_aggregate_refs)
               {
                    for (const auto &Field : SelectedOutputFields)
                    {
                         if (Field.SourceName == identifier)
                         {
                              out = Field.OutputName;
                              return true;
                         }
                    }
               }

               out = identifier;
               return true;
          }

          const std::string qualifier = identifier.substr(0, dot_pos);
          const std::string field_name = identifier.substr(dot_pos + 1);

          if (field_name.empty())
          {
               if (error)
               {
                    *error = "Invalid qualified field reference '" + identifier + "'.";
               }
               return false;
          }

          if (TableAlias.empty() && CollectionName.empty())
          {
               out = field_name;
               return true;
          }

          if ((!TableAlias.empty() && qualifier == TableAlias) ||
              (!CollectionName.empty() && qualifier == CollectionName))
          {
               if (allow_aggregate_refs)
               {
                    for (const auto &Field : SelectedOutputFields)
                    {
                         if (Field.SourceName == field_name)
                         {
                              out = Field.OutputName;
                              return true;
                         }
                    }
               }

               out = field_name;
               return true;
          }

          if (error)
          {
               *error = "Unknown table qualifier '" + qualifier + "' in field reference '" + identifier + "'.";
          }

          return false;
     }

     bool ParseValue(std::string &out)
     {
          if (AtEnd())
          {
               return false;
          }

          const SQLToken *token = Advance();

          if (!token)
          {
               return false;
          }

          if (!token->Text.empty() &&
              (token->Text.front() == '\'' || token->Text.front() == '"' || token->Text.front() == '`'))
          {
               out = SQLStripQuotes(token->Text);
               return true;
          }

          if (token->Upper == "TRUE")
          {
               out = "true";
               return true;
          }

          if (token->Upper == "FALSE")
          {
               out = "false";
               return true;
          }

          if (token->Upper == "NULL")
          {
               out = "NULL";
               return true;
          }

          out = token->Text;
          return true;
     }

     bool ParseInsertValue(nlohmann::json &out, std::string *error)
     {
          if (AtEnd())
          {
               if (error)
               {
                    *error = "Expected value in INSERT VALUES list.";
               }
               return false;
          }

          const SQLToken *token = Advance();
          if (!token)
          {
               if (error)
               {
                    *error = "Expected value in INSERT VALUES list.";
               }
               return false;
          }

          if (!token->Text.empty() &&
              (token->Text.front() == '\'' || token->Text.front() == '"' || token->Text.front() == '`'))
          {
               out = SQLStripQuotes(token->Text);
               return true;
          }

          if (token->Upper == "TRUE")
          {
               out = true;
               return true;
          }

          if (token->Upper == "FALSE")
          {
               out = false;
               return true;
          }

          if (token->Upper == "NULL")
          {
               out = nullptr;
               return true;
          }

          if (SQLIsNumericLiteral(token->Text))
          {
               if (token->Text.find_first_of(".eE") == std::string::npos)
               {
                    try
                    {
                         out = std::stoll(token->Text);
                         return true;
                    }
                    catch (...)
                    {
                    }
               }

               try
               {
                    out = std::stod(token->Text);
                    return true;
               }
               catch (...)
               {
               }
          }

          if (error)
          {
               *error = "Unsupported INSERT value '" + token->Text + "'.";
          }

          return false;
     }

     bool IsAggregateSelectItem() const
     {
          static const std::set<std::string> aggregate_names = {"AVG", "COUNT", "MAX", "MIN", "STATS", "SUM"};
          const SQLToken *function_token = Peek();
          const SQLToken *open_paren = PeekAt(1);
          return function_token && open_paren && open_paren->Text == "(" && aggregate_names.count(function_token->Upper) > 0;
     }

     bool ParseAggregateSelectItem(SQLTranslationResult &result)
     {
          const SQLToken *function_token = Advance();

          if (!function_token)
          {
               result.Error = "Expected aggregate function after SELECT.";
               return false;
          }

          const std::string function_name = SQLToUpperASCII(function_token->Text);

          if (PeekText() != "(")
          {
               result.Error = "Expected '(' after aggregate function '" + function_name + "'.";
               return false;
          }

          Advance();

          std::string field_name;
          bool count_all = false;
          bool distinct_values = false;

          if (MatchKeyword("DISTINCT"))
          {
               distinct_values = true;
          }

          if (function_name == "COUNT" && PeekText() == "*")
          {
               if (distinct_values)
               {
                    result.Error = "COUNT(DISTINCT *) is not supported by hlquery SQL.";
                    return false;
               }

               count_all = true;
               Advance();
          }
          else if (!ParseFieldReference(field_name, &result.Error))
          {
               result.Error = "Expected field name inside aggregate function '" + function_name + "'.";
               return false;
          }

          if (PeekText() != ")")
          {
               result.Error = "Expected ')' after aggregate function '" + function_name + "'.";
               return false;
          }

          Advance();

          std::string alias_name;

          if (!ParseOptionalAlias(alias_name))
          {
               result.Error = "Expected alias name after aggregate expression.";
               return false;
          }

          if (function_name != "COUNT" && field_name.empty())
          {
               result.Error = function_name + " requires a field name.";
               return false;
          }

          std::string aggregation_name = alias_name.empty() ? ToLowerAggregateName(function_name, field_name, count_all, distinct_values) : alias_name;
          nlohmann::json config_body = nlohmann::json::object();

          if (!count_all && !field_name.empty())
          {
               config_body["field"] = field_name;
          }

          if (distinct_values)
          {
               config_body["distinct"] = true;
          }

          AggregateConfig[aggregation_name][ToLowerASCII(function_name)] = config_body;

          if (!TryConsumeASTNode("aggregate expression", &result.Error))
          {
               return false;
          }

          SelectedFields.push_back(aggregation_name);
          SelectAliases[SQLToUpperASCII(aggregation_name)] = aggregation_name;
          result.AggregateSpecs.push_back({function_name, field_name, aggregation_name, count_all, distinct_values});
          CurrentAggregateSpecs = result.AggregateSpecs;
          ParsedStatement.SelectItems.push_back({SQLASTSelectItem::ItemType::Aggregate,
                                                 field_name,
                                                 aggregation_name,
                                                 function_name,
                                                 count_all,
                                                 distinct_values});
          return true;
     }

     std::string ToLowerASCII(std::string value) const
     {
          for (char &character : value)
          {
               character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
          }

          return value;
     }

     std::string ToLowerAggregateName(const std::string &function_name,
                                      const std::string &field_name,
                                      bool count_all,
                                      bool distinct_values) const
     {
          const std::string prefix = ToLowerASCII(function_name);

          if (count_all)
          {
               return prefix + "_all";
          }

          if (distinct_values)
          {
               return prefix + "_distinct_" + field_name;
          }

          return prefix + "_" + field_name;
     }

     bool ParseAggregateReference(std::string &out, std::string *error)
     {
          const SQLToken *function_token = Advance();

          if (!function_token)
          {
               return false;
          }

          const std::string function_name = SQLToUpperASCII(function_token->Text);

          if (PeekText() != "(")
          {
               if (error)
               {
                    *error = "Expected '(' after aggregate function '" + function_name + "'.";
               }
               return false;
          }

          Advance();

          bool distinct_values = false;
          if (MatchKeyword("DISTINCT"))
          {
               distinct_values = true;
          }

          std::string field_name;
          bool count_all = false;

          if (function_name == "COUNT" && PeekText() == "*")
          {
               if (distinct_values)
               {
                    if (error)
                    {
                         *error = "COUNT(DISTINCT *) is not supported by hlquery SQL.";
                    }
                    return false;
               }

               count_all = true;
               Advance();
          }
          else if (!ParseFieldReference(field_name, error))
          {
               if (error)
               {
                    *error = "Expected field name inside aggregate function '" + function_name + "'.";
               }
               return false;
          }

          if (PeekText() != ")")
          {
               if (error)
               {
                    *error = "Expected ')' after aggregate function '" + function_name + "'.";
               }
               return false;
          }

          Advance();

          for (const auto &AggSpec : CurrentAggregateSpecs)
          {
               if (AggSpec.FunctionName == function_name &&
                   AggSpec.FieldName == field_name &&
                   AggSpec.CountAll == count_all &&
                   AggSpec.Distinct == distinct_values)
               {
                    out = AggSpec.OutputName;
                    return true;
               }
          }

          if (error)
          {
               *error = "Aggregate expression '" + ToLowerAggregateName(function_name, field_name, count_all, distinct_values) +
                        "' must appear in the SELECT list before it can be used in HAVING.";
          }

          return false;
     }

     bool ParseOptionalAlias(std::string &alias_name)
     {
          alias_name.clear();

          if (MatchKeyword("AS"))
          {
               return ParseIdentifier(alias_name);
          }

          if (AtEnd() || PeekStartsClause())
          {
               return true;
          }

          const SQLToken *token = Peek();

          if (!token || token->Text.empty() || !SQLIsIdentifierStart(token->Text.front()))
          {
               return true;
          }

          alias_name = token->Text;
          Advance();
          return true;
     }

     bool ParseOrderingReference(std::string &out, std::string *error)
     {
          if (AtEnd())
          {
               return false;
          }

          const SQLToken *token = Peek();

          if (token && SQLIsNumericLiteral(token->Text))
          {
               try
               {
                    size_t consumed = 0;
                    const int ordinal = std::stoi(token->Text, &consumed);

                    if (consumed == token->Text.size() && ordinal > 0)
                    {
                         Advance();

                         if (static_cast<size_t>(ordinal) > SelectedFields.size())
                         {
                              if (error)
                              {
                                   *error = "ORDER/GROUP BY position " + std::to_string(ordinal) + " is out of range for the SELECT list.";
                              }
                              return false;
                         }

                         out = SelectedFields[static_cast<size_t>(ordinal - 1)];
                         return true;
                    }
               }
               catch (...)
               {
               }
          }

          std::string identifier;

          if (!ParseIdentifier(identifier))
          {
               return false;
          }

          auto alias_it = SelectAliases.find(SQLToUpperASCII(identifier));

          if (alias_it != SelectAliases.end())
          {
               out = alias_it->second;
               return true;
          }

          const size_t dot_pos = identifier.find('.');

          if (dot_pos == std::string::npos)
          {
               out = identifier;
               return true;
          }

          const std::string qualifier = identifier.substr(0, dot_pos);
          const std::string field_name = identifier.substr(dot_pos + 1);

          if ((!TableAlias.empty() && qualifier == TableAlias) ||
              (!CollectionName.empty() && qualifier == CollectionName))
          {
               out = field_name;
               return true;
          }

          if (error)
          {
               *error = "Unknown table qualifier '" + qualifier + "' in field reference '" + identifier + "'.";
          }

          return false;
     }

     bool ParseLimitClause(int &per_page, int &offset, bool &has_offset, std::string *error)
     {
          int first_value = 0;

          if (!ParsePositiveInt(first_value, "LIMIT", error))
          {
               return false;
          }

          if (PeekText() == ",")
          {
               Advance();

               int second_value = 0;
               if (!ParsePositiveInt(second_value, "LIMIT", error))
               {
                    return false;
               }

               offset = first_value;
               has_offset = true;
               per_page = second_value;
               return true;
          }

          per_page = first_value;
          return true;
     }

     bool ParseFetchClause(int &per_page, std::string *error)
     {
          if (PeekUpper() == "FIRST" || PeekUpper() == "NEXT")
          {
               Advance();
          }

          if (!ParsePositiveInt(per_page, "FETCH", error))
          {
               return false;
          }

          if (PeekUpper() != "ROW" && PeekUpper() != "ROWS")
          {
               if (error)
               {
                    *error = "FETCH expects ROW or ROWS after the limit value.";
               }
               return false;
          }

          Advance();

          if (!MatchKeyword("ONLY"))
          {
               if (error)
               {
                    *error = "FETCH expects ONLY after ROW or ROWS.";
               }
               return false;
          }

          return true;
     }

     bool ParseBetween(const std::string &field_name, bool negate, std::string &out, std::string *error)
     {
          std::string low_value;
          std::string high_value;

          if (!ParseValue(low_value))
          {
               if (error)
               {
                    *error = "BETWEEN expects a lower bound value.";
               }
               return false;
          }

          if (!MatchKeyword("AND"))
          {
               if (error)
               {
                    *error = "BETWEEN expects AND between the lower and upper bound.";
               }
               return false;
          }

          if (!ParseValue(high_value))
          {
               if (error)
               {
                    *error = "BETWEEN expects an upper bound value.";
               }
               return false;
          }

          if (!ValidateFilterLiteral(low_value, error) || !ValidateFilterLiteral(high_value, error))
          {
               return false;
          }

          if (negate)
          {
               out = field_name + ":<" + low_value + "||" + field_name + ":>" + high_value;
          }
          else
          {
               out = field_name + ":[" + low_value + " TO " + high_value + "]";
          }

          return true;
     }

     bool ParseLike(const std::string &field_name,
                    bool case_insensitive,
                    bool negate,
                    std::string &out,
                    std::string *error)
     {
          std::string value;

          if (!ParseValue(value))
          {
               if (error)
               {
                    *error = "LIKE expects a string or scalar pattern.";
               }
               return false;
          }

          if (!ValidateFilterLiteral(value, error))
          {
               return false;
          }

          std::string pattern = ConvertSQLLikePattern(value);

          if (!ValidateFilterLiteral(pattern, error))
          {
               return false;
          }

          if (pattern.empty())
          {
               if (error)
               {
                    *error = "LIKE pattern cannot be empty.";
               }
               return false;
          }

          out = field_name + ":" + (negate ? "NOT_" : "") + (case_insensitive ? "ILIKE:" : "LIKE:") + pattern;
          return true;
     }

     bool ParseIsPredicate(const std::string &field_name, std::string &out, std::string *error)
     {
          bool negate = false;

          if (MatchKeyword("NOT"))
          {
               negate = true;
          }

          if (MatchKeyword("NULL"))
          {
               out = field_name + ":" + (negate ? "ISNOTNULL" : "ISNULL");
               return true;
          }

          if (MatchKeyword("TRUE"))
          {
               return BuildRenderedComparison(field_name, negate ? "!=" : "=", "true", out, error);
          }

          if (MatchKeyword("FALSE"))
          {
               return BuildRenderedComparison(field_name, negate ? "!=" : "=", "false", out, error);
          }

          if (error)
          {
               *error = "IS only supports NULL, TRUE, and FALSE in hlquery SQL.";
          }

          return false;
     }

     std::string ConvertSQLLikePattern(const std::string &value)
     {
          std::string pattern;
          pattern.reserve(value.size());

          bool escape_next = false;

          for (char character : value)
          {
               if (escape_next)
               {
                    pattern.push_back(character);
                    escape_next = false;
                    continue;
               }

               if (character == '\\')
               {
                    escape_next = true;
                    continue;
               }

               if (character == '%')
               {
                    pattern.push_back('*');
                    continue;
               }

               if (character == '_')
               {
                    pattern.push_back('?');
                    continue;
               }

               pattern.push_back(character);
          }

          if (escape_next)
          {
               pattern.push_back('\\');
          }

          return pattern;
     }

     bool ParseInList(const std::string &field_name, bool negate, std::string &out, std::string *error)
     {
          if (PeekText() != "(")
          {
               if (error)
               {
                    *error = "IN expects a parenthesized value list.";
               }
               return false;
          }

          Advance();

          std::vector<std::string> rendered_terms;

          while (!AtEnd())
          {
               if (rendered_terms.size() >= SQLMaxInListValues)
               {
                    if (error)
                    {
                         *error = "IN lists exceed the maximum supported value count.";
                    }

                    return false;
               }

               std::string value;

               if (!ParseValue(value))
               {
                    if (error)
                    {
                         *error = "IN expects one or more values inside parentheses.";
                    }
                    return false;
               }

               std::string rendered;
               if (!BuildRenderedComparison(field_name, negate ? "!=" : "=", value, rendered, error))
               {
                    return false;
               }

               rendered_terms.push_back(rendered);

               if (PeekText() == ")")
               {
                    break;
               }

               if (PeekText() != ",")
               {
                    if (error)
                    {
                         *error = "IN value lists must be comma-separated.";
                    }
                    return false;
               }

               Advance();
          }

          if (PeekText() != ")")
          {
               if (error)
               {
                    *error = "IN expects a closing ')' after the value list.";
               }
               return false;
          }

          Advance();

          if (rendered_terms.empty())
          {
               if (error)
               {
                    *error = "IN expects at least one value.";
               }
               return false;
          }

          const char *connector = negate ? "&&" : "||";
          std::ostringstream expression;

          for (size_t index = 0; index < rendered_terms.size(); ++index)
          {
               if (index > 0)
               {
                    expression << connector;
               }

               expression << rendered_terms[index];
          }

          out = expression.str();
          return true;
     }

     bool ValidateFilterLiteral(const std::string &value, std::string *error)
     {
          if (SQLIsNumericLiteral(value))
          {
               return true;
          }

          if (!SQLIsSafeFilterLiteral(value))
          {
               if (error)
               {
                    *error = "String literals containing ',', parentheses, logical filter delimiters, or range delimiters are not supported by hlquery SQL yet.";
               }
               return false;
          }

          return true;
     }

     bool BuildRenderedComparison(const std::string &field_name,
                                  const std::string &op,
                                  const std::string &value,
                                  std::string &out,
                                  std::string *error)
     {
          if (SQLToUpperASCII(value) == "NULL")
          {
               if (op == "=" || op == ":=")
               {
                    out = field_name + ":ISNULL";
                    return true;
               }

               if (op == "!=")
               {
                    out = field_name + ":ISNOTNULL";
                    return true;
               }

               if (error)
               {
                    *error = "NULL only supports =, !=, IS NULL, and IS NOT NULL comparisons.";
               }
               return false;
          }

          if (!ValidateFilterLiteral(value, error))
          {
               return false;
          }

          const bool numeric_value = SQLIsNumericLiteral(value);
          const std::string rendered_op = (op == "=" && !numeric_value) ? ":=" : op;
          out = field_name + ":" + rendered_op + value;
          return true;
     }

     bool ParsePositiveInt(int &out, const std::string &label, std::string *error)
     {
          if (!ParseNonNegativeInt(out, label, error))
          {
               return false;
          }

          if (out <= 0)
          {
               if (error)
               {
                    *error = label + " must be greater than zero.";
               }
               return false;
          }

          return true;
     }

     bool ParseNonNegativeInt(int &out, const std::string &label, std::string *error)
     {
          if (AtEnd())
          {
               if (error)
               {
                    *error = "Expected integer value after " + label + ".";
               }
               return false;
          }

          const SQLToken *token = Advance();

          if (!token)
          {
               return false;
          }

          try
          {
               size_t consumed = 0;
               const int parsed = std::stoi(token->Text, &consumed);

               if (consumed != token->Text.size() || parsed < 0)
               {
                    if (error)
                    {
                         *error = label + " expects a non-negative integer.";
                    }
                    return false;
               }

               out = parsed;
               return true;
          }
          catch (...)
          {
               if (error)
               {
                    *error = label + " expects a non-negative integer.";
               }
               return false;
          }
     }
};

SQLTranslationResult SQLService::Parse(const std::string &sql_text) const
{
     SQLTranslationResult result;
     std::string error;
     const std::vector<SQLToken> tokens = SQLTokenize(sql_text, &error);

     if (!error.empty())
     {
          result.Error = error;
          return result;
     }

     Parser parser(tokens);
     result = parser.Parse();

     if (!result.Valid && result.Error.empty())
     {
          result.Error = "Invalid SQL query.";
     }

     return result;
}
