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

#include "sql/parser_internal.h"

static bool SQLCanReadStatementClause(const std::string &clause_name,
                                      int clause_rank,
                                      int &current_rank,
                                      std::set<std::string> &seen_clauses,
                                      std::string &error)
{
     /* Statement clauses must appear once and in SQL statement order. */

     if (seen_clauses.count(clause_name) > 0)
     {
          error = "Duplicate SQL " + clause_name + " clause.";
          return false;
     }

     if (clause_rank < current_rank)
     {
          error = "SQL " + clause_name + " clause appears out of order.";
          return false;
     }

     seen_clauses.insert(clause_name);
     current_rank = clause_rank;
     return true;
}

/* Statement-level parsing (INSERT / DELETE / UPDATE / DROP). */

SQLTranslationResult Parser::ParseInsertStatement(SQLTranslationResult result)
{
     /* INSERT is converted into an explicit JSON document for the write path. */

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

     /* Field list. */

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

     /* VALUES list. */

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

     /* Trailing semicolon is accepted; any other trailing tokens are rejected. */

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

SQLTranslationResult Parser::ParseDeleteStatement(SQLTranslationResult result)
{
     /* DELETE is mapped to a filter-only query with optional ordering and pagination. */

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

     int clause_rank = 0;
     std::set<std::string> seen_clauses;

     while (!AtEnd())
     {
          if (MatchKeyword("WHERE"))
          {
               if (!SQLCanReadStatementClause("WHERE", 1, clause_rank, seen_clauses, result.Error))
               {
                    return result;
               }

               if (!ParseWhere(result))
               {
                    return result;
               }
               continue;
          }

          if (MatchKeyword("ORDER"))
          {
               if (!SQLCanReadStatementClause("ORDER BY", 2, clause_rank, seen_clauses, result.Error))
               {
                    return result;
               }

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
               if (!SQLCanReadStatementClause("LIMIT", 3, clause_rank, seen_clauses, result.Error))
               {
                    return result;
               }

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
                    seen_clauses.insert("OFFSET");
                    clause_rank = 4;
               }

               ParsedStatement.HasExplicitLimit = true;
               ParsedStatement.Limit = result.Query.PerPage;

               continue;
          }

          if (MatchKeyword("OFFSET"))
          {
               if (!SQLCanReadStatementClause("OFFSET", 4, clause_rank, seen_clauses, result.Error))
               {
                    return result;
               }

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
               if (seen_clauses.count("LIMIT") > 0)
               {
                    result.Error = "SQL FETCH cannot be combined with LIMIT.";
                    return result;
               }

               if (!SQLCanReadStatementClause("FETCH", 5, clause_rank, seen_clauses, result.Error))
               {
                    return result;
               }

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

SQLTranslationResult Parser::ParseUpdateStatement(SQLTranslationResult result)
{
     /* UPDATE is currently rejected, but the parser still validates basic statement shape. */

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

SQLTranslationResult Parser::ParseDropStatement(SQLTranslationResult result)
{
     /* DROP is treated as a validated statement type by the router; execution is handled elsewhere. */

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

bool Parser::ParseInsertValue(nlohmann::json &out, std::string *error)
{
     /* INSERT values are parsed into JSON types: strings, numbers, booleans, and null. */

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
