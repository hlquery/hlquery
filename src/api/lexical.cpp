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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/lexicalcache.h"
#include "core/hlquery.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides lexical API handlers for token, term, and language inspection. */

/*
 * ToLowerCopy implementation.
 */

static std::string ToLowerCopy(const std::string &input)
{
     std::string out = input;
     std::transform(out.begin(), out.end(), out.begin(), ::tolower);
     return out;
}

/*
 * TrimWhitespace implementation.
 */

static std::string TrimWhitespace(const std::string &input)
{
     size_t start = input.find_first_not_of(" \t\r\n");
     if (start == std::string::npos)
     {
          return "";
     }

     size_t end = input.find_last_not_of(" \t\r\n");
     return input.substr(start, end - start + 1);
}

/*
 * NormalizeQuotes implementation.
 */

static std::string NormalizeQuotes(const std::string &input)
{
     std::string normalized = input;

     auto replace_all = [](std::string &s, const std::string &from, const std::string &to)
     {
          if (from.empty())
          {
               return;
          }
          size_t pos = 0;
          while ((pos = s.find(from, pos)) != std::string::npos)
          {
               s.replace(pos, from.length(), to);
               pos += to.length();
          }
     };

     replace_all(normalized, "\xE2\x80\x9C", "\"");
     replace_all(normalized, "\xE2\x80\x9D", "\"");
     replace_all(normalized, "\xE2\x80\x98", "'");
     replace_all(normalized, "\xE2\x80\x99", "'");

     return normalized;
}

/*
 * StripQuotes implementation.
 */

static std::string StripQuotes(const std::string &input)
{
     if (input.size() >= 2 && ((input.front() == '"' && input.back() == '"') || (input.front() == '\'' && input.back() == '\'')))
     {
          return input.substr(1, input.size() - 2);
     }

     return input;
}

/* Implements the strip wildcard chars helper. */

static std::string StripWildcardChars(const std::string &input)
{
     std::string out;
     out.reserve(input.size());

     for (char ch : input)
     {
          if (ch != '*' && ch != '?')
          {
               out.push_back(ch);
          }
     }

     return out;
}

/*
 * FieldNameHasToken implementation.
 */

static bool FieldNameHasToken(const std::string &field_name, const std::initializer_list<const char *> &tokens)
{
     std::string lower = ToLowerCopy(field_name);

     for (const auto *token : tokens)
     {
          if (lower.find(token) != std::string::npos)
          {
               return true;
          }
     }

     return false;
}

/*
 * NormalizeTermSimple implementation.
 */

static std::string NormalizeTermSimple(const std::string &term, bool case_sensitive = false)
{
     std::string normalized = term;

     if (!case_sensitive)
     {
          std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
     }

     while (!normalized.empty() && !std::isalnum(static_cast<unsigned char>(normalized.front())) && normalized.front() != '*' && normalized.front() != '?' && normalized.front() != '_')
     {
          normalized.erase(0, 1);
     }

     while (!normalized.empty() && !std::isalnum(static_cast<unsigned char>(normalized.back())) && normalized.back() != '*' && normalized.back() != '?' && normalized.back() != '_')
     {
          normalized.pop_back();
     }

     return normalized;
}

/* Checks whether identifier like term simple applies. */

static bool IsIdentifierLikeTermSimple(const std::string &term)
{
     bool has_alpha = false;
     bool has_digit = false;

     for (const unsigned char character : term)
     {
          has_alpha = has_alpha || std::isalpha(character);
          has_digit = has_digit || std::isdigit(character);
     }

     return has_alpha && has_digit;
}

/*
 * ExtractTermsSimple implementation.
 */

static std::vector<std::string> ExtractTermsSimple(const std::string &text, bool case_sensitive = false)
{
     std::vector<std::string> terms;

     if (text.empty())
     {
          return terms;
     }

     const size_t max_text_size = 1000000;
     const std::string &text_to_process = (text.length() > max_text_size) ? text.substr(0, max_text_size) : text;

     std::set<std::string> unique_terms;

     size_t pos = 0;
     const size_t text_len = text_to_process.length();
     const size_t max_terms = 100000;
     size_t term_count = 0;

     while (pos < text_len && term_count < max_terms)
     {
          while (pos < text_len && (std::isspace(static_cast<unsigned char>(text_to_process[pos])) || text_to_process[pos] == '-' || text_to_process[pos] == '.' || text_to_process[pos] == ',' || text_to_process[pos] == ':' || text_to_process[pos] == '/' || text_to_process[pos] == '\\' || text_to_process[pos] == '(' || text_to_process[pos] == ')' || text_to_process[pos] == '[' || text_to_process[pos] == ']' || text_to_process[pos] == '{' || text_to_process[pos] == '}' || text_to_process[pos] == '@' || text_to_process[pos] == '#' || text_to_process[pos] == '$' || text_to_process[pos] == '%' || text_to_process[pos] == '&' || text_to_process[pos] == '+' || text_to_process[pos] == '=' || text_to_process[pos] == ';' || text_to_process[pos] == '|' || text_to_process[pos] == '!' || text_to_process[pos] == '?' || text_to_process[pos] == '~' || text_to_process[pos] == '^' || text_to_process[pos] == '`'))
          {
               pos++;
          }

          if (pos >= text_len)
          {
               break;
          }

          size_t word_start = pos;

          while (pos < text_len && !std::isspace(static_cast<unsigned char>(text_to_process[pos])) && text_to_process[pos] != '-' && text_to_process[pos] != '.' && text_to_process[pos] != ',' && text_to_process[pos] != ':' && text_to_process[pos] != '/' && text_to_process[pos] != '\\' && text_to_process[pos] != '(' && text_to_process[pos] != ')' && text_to_process[pos] != '[' && text_to_process[pos] != ']' && text_to_process[pos] != '{' && text_to_process[pos] != '}' && text_to_process[pos] != '@' && text_to_process[pos] != '#' && text_to_process[pos] != '$' && text_to_process[pos] != '%' && text_to_process[pos] != '&' && text_to_process[pos] != '+' && text_to_process[pos] != '=' && text_to_process[pos] != ';' && text_to_process[pos] != '|' && text_to_process[pos] != '!' && text_to_process[pos] != '?' && text_to_process[pos] != '~' && text_to_process[pos] != '^' && text_to_process[pos] != '`')
          {
               pos++;
          }

          if (pos > word_start)
          {
               std::string word = text_to_process.substr(word_start, pos - word_start);
               std::string normalized = NormalizeTermSimple(word, case_sensitive);

               if (!normalized.empty())
               {
                    unique_terms.insert(normalized);
                    term_count++;
               }
          }
     }

     terms.assign(unique_terms.begin(), unique_terms.end());

     return terms;
}

/* Extracts terms in order simple values. */

static std::vector<std::string> ExtractTermsInOrderSimple(const std::string &text, bool case_sensitive = false)
{
     std::vector<std::string> terms;

     if (text.empty())
     {
          return terms;
     }

     const size_t max_text_size = 1000000;
     const std::string &text_to_process = (text.length() > max_text_size) ? text.substr(0, max_text_size) : text;

     size_t pos = 0;
     const size_t text_len = text_to_process.length();
     const size_t max_terms = 100000;

     while (pos < text_len && terms.size() < max_terms)
     {
          while (pos < text_len && (std::isspace(static_cast<unsigned char>(text_to_process[pos])) || text_to_process[pos] == '-' || text_to_process[pos] == '.' || text_to_process[pos] == ',' || text_to_process[pos] == ':' || text_to_process[pos] == '/' || text_to_process[pos] == '\\' || text_to_process[pos] == '(' || text_to_process[pos] == ')' || text_to_process[pos] == '[' || text_to_process[pos] == ']' || text_to_process[pos] == '{' || text_to_process[pos] == '}' || text_to_process[pos] == '@' || text_to_process[pos] == '#' || text_to_process[pos] == '$' || text_to_process[pos] == '%' || text_to_process[pos] == '&' || text_to_process[pos] == '+' || text_to_process[pos] == '=' || text_to_process[pos] == ';' || text_to_process[pos] == '|' || text_to_process[pos] == '!' || text_to_process[pos] == '?' || text_to_process[pos] == '~' || text_to_process[pos] == '^' || text_to_process[pos] == '`' || text_to_process[pos] == '"'))
          {
               pos++;
          }

          if (pos >= text_len)
          {
               break;
          }

          size_t word_start = pos;

          while (pos < text_len && !std::isspace(static_cast<unsigned char>(text_to_process[pos])) && text_to_process[pos] != '-' && text_to_process[pos] != '.' && text_to_process[pos] != ',' && text_to_process[pos] != ':' && text_to_process[pos] != '/' && text_to_process[pos] != '\\' && text_to_process[pos] != '(' && text_to_process[pos] != ')' && text_to_process[pos] != '[' && text_to_process[pos] != ']' && text_to_process[pos] != '{' && text_to_process[pos] != '}' && text_to_process[pos] != '@' && text_to_process[pos] != '#' && text_to_process[pos] != '$' && text_to_process[pos] != '%' && text_to_process[pos] != '&' && text_to_process[pos] != '+' && text_to_process[pos] != '=' && text_to_process[pos] != ';' && text_to_process[pos] != '|' && text_to_process[pos] != '!' && text_to_process[pos] != '?' && text_to_process[pos] != '~' && text_to_process[pos] != '^' && text_to_process[pos] != '`' && text_to_process[pos] != '"')
          {
               pos++;
          }

          if (pos > word_start)
          {
               std::string word = text_to_process.substr(word_start, pos - word_start);
               std::string normalized = NormalizeTermSimple(word, case_sensitive);

               if (!normalized.empty())
               {
                    terms.push_back(normalized);
               }
          }
     }

     return terms;
}

/* Implements the field contains phrase simple helper. */

static bool FieldContainsPhraseSimple(const std::string &field_value,
                                      const std::string &phrase,
                                      bool case_sensitive = false)
{
     const std::vector<std::string> field_terms = ExtractTermsInOrderSimple(field_value, case_sensitive);
     const std::vector<std::string> phrase_terms = ExtractTermsInOrderSimple(phrase, case_sensitive);

     if (field_terms.empty() || phrase_terms.empty() || phrase_terms.size() > field_terms.size())
     {
          return false;
     }

     for (size_t start = 0; start + phrase_terms.size() <= field_terms.size(); ++start)
     {
          bool matched = true;

          for (size_t index = 0; index < phrase_terms.size(); ++index)
          {
               if (field_terms[start + index] != phrase_terms[index])
               {
                    matched = false;
                    break;
               }
          }

          if (matched)
          {
               return true;
          }
     }

     return false;
}

/*
 * ExtractQuotedPhrases implementation.
 */

static std::vector<std::string> ExtractQuotedPhrases(const std::string &text, bool case_sensitive = false)
{
     std::vector<std::string> phrases;

     if (text.empty())
     {
          return phrases;
     }

     std::string normalized = NormalizeQuotes(text);

     size_t start = 0;
     while ((start = normalized.find('\"', start)) != std::string::npos)
     {
          size_t end = normalized.find('\"', start + 1);
          if (end == std::string::npos)
          {
               break;
          }

          if (end > start + 1)
          {
               std::string phrase = TrimWhitespace(normalized.substr(start + 1, end - start - 1));
               if (!phrase.empty())
               {
                    phrases.push_back(case_sensitive ? phrase : ToLowerCopy(phrase));
               }
          }

          start = end + 1;
     }

     return phrases;
}

/* Calculates levenshtein distance simple values. */

static int CalculateLevenshteinDistanceSimple(const std::string &left, const std::string &right)
{
     if (left == right)
     {
          return 0;
     }

     if (left.empty())
     {
          return static_cast<int>(right.size());
     }

     if (right.empty())
     {
          return static_cast<int>(left.size());
     }

     std::vector<int> previous(right.size() + 1);
     std::vector<int> current(right.size() + 1);

     for (std::size_t j = 0; j <= right.size(); ++j)
     {
          previous[j] = static_cast<int>(j);
     }

     for (std::size_t i = 1; i <= left.size(); ++i)
     {
          current[0] = static_cast<int>(i);

          for (std::size_t j = 1; j <= right.size(); ++j)
          {
               const int substitution_cost = (left[i - 1] == right[j - 1]) ? 0 : 1;
               current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + substitution_cost});
          }

          previous.swap(current);
     }

     return previous[right.size()];
}

/* Checks whether typo tolerant match simple applies. */

static bool IsTypoTolerantMatchSimple(const std::string &field_term, const std::string &term, int max_typos)
{
     if (max_typos <= 0 || field_term.empty() || term.empty())
     {
          return false;
     }

     if (std::abs(static_cast<int>(field_term.size()) - static_cast<int>(term.size())) > max_typos)
     {
          return false;
     }

     return CalculateLevenshteinDistanceSimple(field_term, term) <= max_typos;
}

/* Implements the field matches term simple helper. */

static bool FieldMatchesTermSimple(const std::string &field_value,
                                   const std::string &term,
                                   bool allow_prefix_match,
                                   int max_typos = 0,
                                   bool case_sensitive = false)
{
     if (field_value.empty() || term.empty())
     {
          return false;
     }

     std::vector<std::string> field_terms = ExtractTermsSimple(field_value, case_sensitive);
     if (field_terms.empty())
     {
          return false;
     }

     const bool is_wildcard = term.find('*') != std::string::npos || term.find('?') != std::string::npos;

     for (const auto &field_term : field_terms)
     {
          if (is_wildcard)
          {
               if (case_sensitive ? Wildcard::MatchCaseSensitive(field_term, term) : Wildcard::Match(field_term, term))
               {
                    return true;
               }
               continue;
          }

          if (field_term == term)
          {
               return true;
          }

          if (max_typos > 0 && IsTypoTolerantMatchSimple(field_term, term, max_typos))
          {
               return true;
          }

          if (allow_prefix_match && field_term.size() >= term.size() && field_term.compare(0, term.size(), term) == 0)
          {
               return true;
          }
     }

     return false;
}

/* Builds index query for search data. */

static std::string BuildIndexQueryForSearch(const std::string &query_text, bool allow_prefix_match)
{
     if (!allow_prefix_match)
     {
          return query_text;
     }

     std::vector<std::string> terms = ExtractTermsSimple(query_text);

     if (terms.size() != 1)
     {
          return query_text;
     }

     const std::string &term = terms.front();

     if (term.empty() || term.find('*') != std::string::npos || term.find('?') != std::string::npos)
     {
          return query_text;
     }

     return term + "*";
}

/* Builds relaxed query variants data. */

static std::vector<std::string> BuildRelaxedQueryVariants(const std::string &query_text,
                                                          int drop_tokens_threshold,
                                                          bool case_sensitive = false)
{
     std::vector<std::string> variants;
     variants.push_back(query_text);

     std::vector<std::string> terms = ExtractTermsSimple(query_text, case_sensitive);
     if (drop_tokens_threshold <= 0 || static_cast<int>(terms.size()) <= drop_tokens_threshold || terms.size() <= 1)
     {
          return variants;
     }

     for (std::size_t skip = 0; skip < terms.size(); ++skip)
     {
          std::ostringstream builder;
          bool first = true;

          for (std::size_t i = 0; i < terms.size(); ++i)
          {
               if (i == skip)
               {
                    continue;
               }

               if (!first)
               {
                    builder << " ";
               }

               builder << terms[i];
               first = false;
          }

          const std::string variant = builder.str();
          if (!variant.empty())
          {
               variants.push_back(variant);
          }
     }

     std::sort(variants.begin(), variants.end());
     variants.erase(std::unique(variants.begin(), variants.end()), variants.end());

     return variants;
}

/* Builds highlight terms for search data. */

static std::vector<std::string> BuildHighlightTermsForSearch(const std::string &query_text, bool case_sensitive = false)
{
     std::vector<std::string> raw_terms = ExtractTermsSimple(query_text, case_sensitive);
     std::vector<std::string> highlight_terms;
     highlight_terms.reserve(raw_terms.size());

     for (const auto &term : raw_terms)
     {
          std::string cleaned = NormalizeTermSimple(StripWildcardChars(term), case_sensitive);
          if (!cleaned.empty())
          {
               highlight_terms.push_back(cleaned);
          }
     }

     std::sort(highlight_terms.begin(), highlight_terms.end());
     highlight_terms.erase(std::unique(highlight_terms.begin(), highlight_terms.end()), highlight_terms.end());

     return highlight_terms;
}

struct ParsedQueryClause
{
     std::string Field;
     std::string Value;
     bool Phrase = false;
     bool Prohibited = false;
};

struct ParsedQueryGroup
{
     std::vector<ParsedQueryClause> Clauses;
};

struct ParsedQueryExpression
{
     std::vector<ParsedQueryGroup> Groups;
     bool UsesStructuredSemantics = false;
};

static bool HasRequestedQueryFields(const std::vector<std::string> &query_by);
static void AppendRequestedFieldValues(const Document &doc,
                                       const std::vector<std::string> &query_by,
                                       std::vector<std::pair<std::string, std::string>> &field_values,
                                       bool case_sensitive = false);

/* Checks whether field specifier applies. */

static bool IsFieldSpecifier(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     for (char ch : value)
     {
          if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
          {
               return false;
          }
     }

     return true;
}

/* Implements the tokenize structured query helper. */

static std::vector<std::string> TokenizeStructuredQuery(const std::string &query_text)
{
     std::vector<std::string> tokens;
     const std::string normalized = NormalizeQuotes(query_text);
     std::size_t pos = 0;

     while (pos < normalized.size())
     {
          while (pos < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[pos])))
          {
               ++pos;
          }

          if (pos >= normalized.size())
          {
               break;
          }

          std::size_t start = pos;
          bool in_quotes = false;

          while (pos < normalized.size())
          {
               const char ch = normalized[pos];
               if (ch == '"')
               {
                    in_quotes = !in_quotes;
                    ++pos;
                    continue;
               }

               if (!in_quotes && std::isspace(static_cast<unsigned char>(ch)))
               {
                    break;
               }

               ++pos;
          }

          if (pos > start)
          {
               tokens.push_back(normalized.substr(start, pos - start));
          }
     }

     return tokens;
}

/* Parses structured query expression input. */

static ParsedQueryExpression ParseStructuredQueryExpression(const std::string &query_text, bool case_sensitive = false)
{
     ParsedQueryExpression expression;
     std::vector<std::string> tokens = TokenizeStructuredQuery(query_text);

     if (tokens.empty())
     {
          return expression;
     }

     ParsedQueryGroup current_group;
     bool next_prohibited = false;

     auto push_group = [&expression](ParsedQueryGroup &group)
     {
          if (!group.Clauses.empty())
          {
               expression.Groups.push_back(group);
               group.Clauses.clear();
          }
     };

     for (const auto &raw_token : tokens)
     {
          std::string token = raw_token;
          std::string lowered = ToLowerCopy(token);

          if (lowered == "or")
          {
               expression.UsesStructuredSemantics = true;
               push_group(current_group);
               next_prohibited = false;
               continue;
          }

          if (lowered == "and")
          {
               expression.UsesStructuredSemantics = true;
               continue;
          }

          if (lowered == "not")
          {
               expression.UsesStructuredSemantics = true;
               next_prohibited = true;
               continue;
          }

          ParsedQueryClause clause;

          while (!token.empty() && (token.front() == '+' || token.front() == '-' || token.front() == '!'))
          {
               expression.UsesStructuredSemantics = true;
               if (token.front() == '-' || token.front() == '!')
               {
                    clause.Prohibited = true;
               }
               token.erase(0, 1);
          }

          if (next_prohibited)
          {
               clause.Prohibited = true;
               next_prohibited = false;
          }

          const std::size_t colon_pos = token.find(':');
          if (colon_pos != std::string::npos)
          {
               const std::string possible_field = token.substr(0, colon_pos);
               if (IsFieldSpecifier(possible_field))
               {
                    clause.Field = possible_field;
                    token = token.substr(colon_pos + 1);
                    expression.UsesStructuredSemantics = true;
               }
          }

          token = TrimWhitespace(token);
          if (token.empty())
          {
               continue;
          }

          clause.Phrase = (token.size() >= 2 && token.front() == '"' && token.back() == '"');
          if (clause.Phrase)
          {
               const std::string PhraseValue = TrimWhitespace(StripQuotes(token));
               clause.Value = case_sensitive ? PhraseValue : ToLowerCopy(PhraseValue);
               expression.UsesStructuredSemantics = true;
          }
          else
          {
               clause.Value = NormalizeTermSimple(token, case_sensitive);
          }

          if (clause.Value.empty())
          {
               continue;
          }

          current_group.Clauses.push_back(clause);
     }

     push_group(current_group);
     return expression;
}

/* Implements the collect field values for clause helper. */

static void CollectFieldValuesForClause(const Document &doc,
                                        const ParsedQueryClause &clause,
                                        const std::vector<std::string> &query_by,
                                        std::vector<std::pair<std::string, std::string>> &field_values,
                                        bool case_sensitive)
{
     if (!clause.Field.empty())
     {
          AppendRequestedFieldValues(doc, {clause.Field}, field_values, case_sensitive);
          return;
     }

     AppendRequestedFieldValues(doc, query_by, field_values, case_sensitive);
}

/* Implements the evaluate query clause helper. */

static bool EvaluateQueryClause(const Document &doc,
                                const ParsedQueryClause &clause,
                                const std::vector<std::string> &query_by,
                                bool allow_prefix_match,
                                int max_typos,
                                bool case_sensitive)
{
     std::vector<std::pair<std::string, std::string>> field_values;
     CollectFieldValuesForClause(doc, clause, query_by, field_values, case_sensitive);

     if (field_values.empty())
     {
          return false;
     }

     for (const auto &field_val : field_values)
     {
          if (clause.Phrase)
          {
               if (FieldContainsPhraseSimple(field_val.second, clause.Value, case_sensitive))
               {
                    return true;
               }
               continue;
          }

          if (FieldMatchesTermSimple(field_val.second, clause.Value, allow_prefix_match, max_typos, case_sensitive))
          {
               return true;
          }
     }

     return false;
}

/* Implements the evaluate parsed query expression helper. */

static bool EvaluateParsedQueryExpression(const Document &doc,
                                          const ParsedQueryExpression &expression,
                                          const std::vector<std::string> &query_by,
                                          bool allow_prefix_match,
                                          int max_typos,
                                          bool case_sensitive)
{
     if (expression.Groups.empty())
     {
          return true;
     }

     for (const auto &group : expression.Groups)
     {
          bool group_matches = true;

          for (const auto &clause : group.Clauses)
          {
               const bool clause_matches = EvaluateQueryClause(doc, clause, query_by, allow_prefix_match, max_typos, case_sensitive);

               if (clause.Prohibited)
               {
                    if (clause_matches)
                    {
                         group_matches = false;
                         break;
                    }
               }
               else if (!clause_matches)
               {
                    group_matches = false;
                    break;
               }
          }

          if (group_matches)
          {
               return true;
          }
     }

     return false;
}

/* Builds candidate queries from parsed expression data. */

static std::vector<std::string> BuildCandidateQueriesFromParsedExpression(const ParsedQueryExpression &expression)
{
     std::vector<std::string> queries;

     for (const auto &group : expression.Groups)
     {
          std::ostringstream builder;
          bool first = true;

          for (const auto &clause : group.Clauses)
          {
               if (clause.Prohibited)
               {
                    continue;
               }

               if (!first)
               {
                    builder << " ";
               }

               builder << clause.Value;
               first = false;
          }

          const std::string built = builder.str();
          if (!built.empty())
          {
               queries.push_back(built);
          }
     }

     std::sort(queries.begin(), queries.end());
     queries.erase(std::unique(queries.begin(), queries.end()), queries.end());

     return queries;
}

/* Loads synonym graph for collection data. */

static void LoadSynonymGraphForCollection(const std::string &Collection,
                                          std::unordered_map<std::string, std::vector<std::string>> &SynonymGraph)
{
     const std::string ScopePreference =
          (Instance && Instance->Config) ? Instance->Config->GetQuerySettingsLexicalScopePreference() : "merge";

     auto load_scope = [&SynonymGraph](const std::string &ScopeKey, bool OverrideExisting) -> bool
     {
          std::string Raw = HybridStorageManager::GetInstance().Get(ScopeKey);
          if (Raw.empty())
          {
               return false;
          }

          try
          {
               nlohmann::json Root = nlohmann::json::parse(Raw);
               nlohmann::json Groups = nlohmann::json::array();
               if (Root.is_object() && Root.contains("synonyms") && Root["synonyms"].is_array())
               {
                    Groups = Root["synonyms"];
               }
               else if (Root.is_array())
               {
                    Groups = Root;
               }

               std::vector<std::vector<std::string>> ParsedGroups;
               std::unordered_set<std::string> AffectedTerms;

               for (const auto &Group : Groups)
               {
                    if (!Group.is_object() || !Group.contains("root") || !Group["root"].is_string())
                    {
                         continue;
                    }

                    std::vector<std::string> Terms;
                    std::string RootTerm = NormalizeTermSimple(Group["root"].get<std::string>());
                    if (RootTerm.empty())
                    {
                         continue;
                    }
                    Terms.push_back(RootTerm);

                    if (Group.contains("synonyms") && Group["synonyms"].is_array())
                    {
                         for (const auto &Syn : Group["synonyms"])
                         {
                              if (!Syn.is_string())
                              {
                                   continue;
                              }
                              std::string Normalized = NormalizeTermSimple(Syn.get<std::string>());
                              if (!Normalized.empty())
                              {
                                   Terms.push_back(Normalized);
                              }
                         }
                    }

                    std::sort(Terms.begin(), Terms.end());
                    Terms.erase(std::unique(Terms.begin(), Terms.end()), Terms.end());

                    if (Terms.size() < 2)
                    {
                         continue;
                    }

                    for (const auto &Term : Terms)
                    {
                         AffectedTerms.insert(Term);
                    }

                    ParsedGroups.push_back(Terms);
               }

               if (ParsedGroups.empty())
               {
                    return false;
               }

               if (OverrideExisting)
               {
                    for (const auto &Term : AffectedTerms)
                    {
                         SynonymGraph.erase(Term);
                    }
               }

               for (const auto &Terms : ParsedGroups)
               {
                    for (const auto &Term : Terms)
                    {
                         auto &Targets = SynonymGraph[Term];
                         for (const auto &Candidate : Terms)
                         {
                              if (Candidate == Term)
                              {
                                   continue;
                              }
                              Targets.push_back(Candidate);
                         }
                         std::sort(Targets.begin(), Targets.end());
                         Targets.erase(std::unique(Targets.begin(), Targets.end()), Targets.end());
                    }
               }

               return true;
          }
          catch (const std::exception &)
          {
          }

          return false;
     };

     if (ScopePreference == "local")
     {
          load_scope("synonyms:__global__", false);
          load_scope("synonyms:" + Collection, true);
     }
     else if (ScopePreference == "global")
     {
          load_scope("synonyms:" + Collection, false);
          load_scope("synonyms:__global__", true);
     }
     else
     {
          load_scope("synonyms:__global__", false);
          load_scope("synonyms:" + Collection, false);
     }
}

/* Loads stopwords for collection data. */

static void LoadStopwordsForCollection(const std::string &Collection, std::unordered_set<std::string> &Stopwords)
{
     const std::string ScopePreference =
          (Instance && Instance->Config) ? Instance->Config->GetQuerySettingsLexicalScopePreference() : "merge";

     auto load_scope = [](const std::string &Key, std::unordered_set<std::string> &Target) -> bool
     {
          std::string StopwordsJSON = HybridStorageManagerInstance().Get(Key);
          if (StopwordsJSON.empty())
          {
               return false;
          }

          try
          {
               nlohmann::json Root = nlohmann::json::parse(StopwordsJSON);
               nlohmann::json StopArray = nlohmann::json::array();

               if (Root.is_object() && Root.contains("stopwords") && Root["stopwords"].is_array())
               {
                    StopArray = Root["stopwords"];
               }
               else if (Root.is_array())
               {
                    StopArray = Root;
               }

               for (const auto &Entry : StopArray)
               {
                    std::string Word;
                    if (Entry.is_string())
                    {
                         Word = Entry.get<std::string>();
                    }
                    else if (Entry.is_object())
                    {
                         if (Entry.contains("word") && Entry["word"].is_string())
                         {
                              Word = Entry["word"].get<std::string>();
                         }
                         else if (Entry.contains("text") && Entry["text"].is_string())
                         {
                              Word = Entry["text"].get<std::string>();
                         }
                    }

                    Word = NormalizeTermSimple(Word);
                    if (!Word.empty())
                    {
                         Target.insert(Word);
                    }
               }

               return !Target.empty();
          }
          catch (const std::exception &)
          {
          }

          return false;
     };

     if (ScopePreference == "local")
     {
          std::unordered_set<std::string> LocalStopwords;
          if (load_scope("stopwords:" + Collection, LocalStopwords))
          {
               Stopwords.swap(LocalStopwords);
          }
          else
          {
               load_scope("stopwords:__global__", Stopwords);
          }
     }
     else if (ScopePreference == "global")
     {
          std::unordered_set<std::string> GlobalStopwords;
          if (load_scope("stopwords:__global__", GlobalStopwords))
          {
               Stopwords.swap(GlobalStopwords);
          }
          else
          {
               load_scope("stopwords:" + Collection, Stopwords);
          }
     }
     else
     {
          load_scope("stopwords:__global__", Stopwords);
          load_scope("stopwords:" + Collection, Stopwords);
     }
}

struct LexicalResourceSnapshot
{
     std::unordered_map<std::string, std::vector<std::string>> SynonymGraph;
     std::unordered_set<std::string> Stopwords;
     uint64_t CollectionGeneration = 0;
     uint64_t GlobalGeneration = 0;
     std::string ScopePreference = "merge";
};

struct ExpansionCacheEntry
{
     std::string Collection;
     std::vector<std::string> Variants;
};

static std::mutex LexicalCacheMutex;
static std::unordered_map<std::string, std::shared_ptr<const LexicalResourceSnapshot>> LexicalResourceEntries;
static std::unordered_map<std::string, ExpansionCacheEntry> ExpansionEntries;
static std::deque<std::string> ExpansionOrder;
static std::unordered_map<std::string, uint64_t> CollectionGenerations;
static uint64_t GlobalLexicalGeneration = 1;
static std::atomic<uint64_t> ResourceCacheHits{0};
static std::atomic<uint64_t> ResourceCacheMisses{0};
static std::atomic<uint64_t> ExpansionCacheHits{0};
static std::atomic<uint64_t> ExpansionCacheMisses{0};
static constexpr size_t MaxExpansionCacheEntries = 4096;

/* Returns lexical generations locked values. */

static std::pair<uint64_t, uint64_t> GetLexicalGenerationsLocked(const std::string &Collection)
{
     const auto It = CollectionGenerations.find(Collection);
     return {It == CollectionGenerations.end() ? 0 : It->second, GlobalLexicalGeneration};
}

/* Returns lexical resource snapshot values. */

static std::shared_ptr<const LexicalResourceSnapshot> GetLexicalResourceSnapshot(const std::string &Collection)
{
     uint64_t CollectionGeneration = 0;
     uint64_t GlobalGeneration = 0;
     const std::string ScopePreference =
          (Instance && Instance->Config) ? Instance->Config->GetQuerySettingsLexicalScopePreference() : "merge";
     {
          std::lock_guard<std::mutex> Lock(LexicalCacheMutex);
          const auto Generations = GetLexicalGenerationsLocked(Collection);
          CollectionGeneration = Generations.first;
          GlobalGeneration = Generations.second;
          const auto It = LexicalResourceEntries.find(Collection);
          if (It != LexicalResourceEntries.end() &&
              It->second->CollectionGeneration == CollectionGeneration &&
              It->second->GlobalGeneration == GlobalGeneration &&
              It->second->ScopePreference == ScopePreference)
          {
               ResourceCacheHits.fetch_add(1, std::memory_order_relaxed);
               return It->second;
          }
     }

     ResourceCacheMisses.fetch_add(1, std::memory_order_relaxed);
     auto Snapshot = std::make_shared<LexicalResourceSnapshot>();
     Snapshot->CollectionGeneration = CollectionGeneration;
     Snapshot->GlobalGeneration = GlobalGeneration;
     Snapshot->ScopePreference = ScopePreference;
     LoadSynonymGraphForCollection(Collection, Snapshot->SynonymGraph);
     LoadStopwordsForCollection(Collection, Snapshot->Stopwords);

     std::unique_lock<std::mutex> Lock(LexicalCacheMutex);
     const auto CurrentGenerations = GetLexicalGenerationsLocked(Collection);
     const std::string CurrentScopePreference =
          (Instance && Instance->Config) ? Instance->Config->GetQuerySettingsLexicalScopePreference() : "merge";
     if (CurrentGenerations.first != CollectionGeneration ||
         CurrentGenerations.second != GlobalGeneration ||
         CurrentScopePreference != ScopePreference)
     {
          Lock.unlock();
          return GetLexicalResourceSnapshot(Collection);
     }
     LexicalResourceEntries[Collection] = Snapshot;
     return Snapshot;
}

/* Builds expansion cache key data. */

static std::string BuildExpansionCacheKey(const std::string &Collection,
                                          const std::string &QueryText,
                                          bool EnableSynonyms,
                                          bool EnableStopwords,
                                          uint64_t CollectionGeneration,
                                          uint64_t GlobalGeneration,
                                          const std::string &ScopePreference)
{
     return Collection + "\n" + QueryText + "\n" +
            (EnableSynonyms ? "1" : "0") + (EnableStopwords ? "1" : "0") + "\n" +
            std::to_string(CollectionGeneration) + "\n" + std::to_string(GlobalGeneration) + "\n" +
            ScopePreference;
}

/* Implements the put expansion cache helper. */

static void PutExpansionCache(const std::string &Key,
                              const std::string &Collection,
                              const std::vector<std::string> &Variants)
{
     std::lock_guard<std::mutex> Lock(LexicalCacheMutex);
     ExpansionEntries[Key] = {Collection, Variants};
     ExpansionOrder.push_back(Key);
     while (ExpansionEntries.size() > MaxExpansionCacheEntries && !ExpansionOrder.empty())
     {
          ExpansionEntries.erase(ExpansionOrder.front());
          ExpansionOrder.pop_front();
     }
}

/* Builds expanded queries from synonyms data. */

static std::vector<std::string> BuildExpandedQueriesFromSynonyms(const std::string &QueryText,
                                                                 const std::string &Collection,
                                                                 bool EnableSynonyms,
                                                                 bool EnableStopwords)
{
     std::vector<std::string> Variants;

     if (QueryText.empty() || QueryText == "*")
     {
          Variants.push_back(QueryText);
          return Variants;
     }

     const auto Snapshot = GetLexicalResourceSnapshot(Collection);
     const std::string CacheKey = BuildExpansionCacheKey(Collection, QueryText, EnableSynonyms, EnableStopwords,
                                                         Snapshot->CollectionGeneration, Snapshot->GlobalGeneration,
                                                         Snapshot->ScopePreference);
     {
          std::lock_guard<std::mutex> Lock(LexicalCacheMutex);
          const auto It = ExpansionEntries.find(CacheKey);
          if (It != ExpansionEntries.end())
          {
               ExpansionCacheHits.fetch_add(1, std::memory_order_relaxed);
               return It->second.Variants;
          }
     }
     ExpansionCacheMisses.fetch_add(1, std::memory_order_relaxed);

     std::vector<std::string> Tokens;
     std::stringstream SS(QueryText);
     std::string Token;
     while (SS >> Token)
     {
          Tokens.push_back(Token);
     }

     if (EnableStopwords && !Snapshot->Stopwords.empty())
     {
          std::vector<std::string> Filtered;
          Filtered.reserve(Tokens.size());

          for (const auto &RawToken : Tokens)
          {
               std::string Normalized = NormalizeTermSimple(RawToken);
               if (Normalized.empty())
               {
                    continue;
               }
               if (Snapshot->Stopwords.find(Normalized) != Snapshot->Stopwords.end())
               {
                    continue;
               }
               Filtered.push_back(RawToken);
          }

          Tokens.swap(Filtered);
     }

     if (Tokens.empty())
     {
          PutExpansionCache(CacheKey, Collection, Variants);
          return Variants;
     }

     std::ostringstream BaseBuilder;
     for (size_t I = 0; I < Tokens.size(); ++I)
     {
          if (I > 0)
          {
               BaseBuilder << " ";
          }
          BaseBuilder << Tokens[I];
     }
     Variants.push_back(BaseBuilder.str());

     if (!EnableSynonyms || Snapshot->SynonymGraph.empty())
     {
          PutExpansionCache(CacheKey, Collection, Variants);
          return Variants;
     }

     const size_t MaxVariants = 16;
     const size_t MaxSynonymsPerToken = 4;

     for (size_t I = 0; I < Tokens.size() && Variants.size() < MaxVariants; ++I)
     {
          std::string Normalized = NormalizeTermSimple(Tokens[I]);
          auto SynIt = Snapshot->SynonymGraph.find(Normalized);
          if (Normalized.empty() || SynIt == Snapshot->SynonymGraph.end())
          {
               continue;
          }

          size_t SynCount = 0;
          for (const auto &Syn : SynIt->second)
          {
               if (Syn.empty())
               {
                    continue;
               }

               std::vector<std::string> Modified = Tokens;
               Modified[I] = Syn;

               std::ostringstream QueryBuilder;
               for (size_t J = 0; J < Modified.size(); ++J)
               {
                    if (J > 0)
                    {
                         QueryBuilder << " ";
                    }
                    QueryBuilder << Modified[J];
               }

               Variants.push_back(QueryBuilder.str());
               SynCount++;
               if (SynCount >= MaxSynonymsPerToken || Variants.size() >= MaxVariants)
               {
                    break;
               }
          }
     }

     std::sort(Variants.begin(), Variants.end());
     Variants.erase(std::unique(Variants.begin(), Variants.end()), Variants.end());
     PutExpansionCache(CacheKey, Collection, Variants);
     return Variants;
}

/* Implements the invalidate collection helper. */

void LexicalQueryCache::InvalidateCollection(const std::string &Collection)
{
     std::lock_guard<std::mutex> Lock(LexicalCacheMutex);
     ++CollectionGenerations[Collection];
     LexicalResourceEntries.erase(Collection);
     for (auto It = ExpansionEntries.begin(); It != ExpansionEntries.end();)
     {
          if (It->second.Collection == Collection)
          {
               It = ExpansionEntries.erase(It);
          }
          else
          {
               ++It;
          }
     }
}

/* Implements the invalidate all helper. */

void LexicalQueryCache::InvalidateAll()
{
     std::lock_guard<std::mutex> Lock(LexicalCacheMutex);
     ++GlobalLexicalGeneration;
     LexicalResourceEntries.clear();
     ExpansionEntries.clear();
     ExpansionOrder.clear();
}

/* Returns stats values. */

LexicalQueryCache::Stats LexicalQueryCache::GetStats()
{
     Stats Result;
     Result.ResourceHits = ResourceCacheHits.load(std::memory_order_relaxed);
     Result.ResourceMisses = ResourceCacheMisses.load(std::memory_order_relaxed);
     Result.ExpansionHits = ExpansionCacheHits.load(std::memory_order_relaxed);
     Result.ExpansionMisses = ExpansionCacheMisses.load(std::memory_order_relaxed);
     return Result;
}

/*
 * HighlightTermsSimple wraps matched terms with <em> tags.
 */

static std::string HighlightTermsSimple(const std::string &text,
                                        const std::vector<std::string> &terms,
                                        bool case_sensitive = false)
{
     if (text.empty() || terms.empty())
     {
          return text;
     }

     std::string lowered = ToLowerCopy(text);
     std::string result;
     result.reserve(text.size() + 32);

     size_t pos = 0;

     while (pos < text.size())
     {
          size_t best_pos = std::string::npos;
          size_t best_len = 0;

          for (const auto &term : terms)
          {
               if (term.empty())
               {
                    continue;
               }

               size_t found = case_sensitive ? text.find(term, pos) : lowered.find(term, pos);
               if (found != std::string::npos)
               {
                    if (best_pos == std::string::npos || found < best_pos || (found == best_pos && term.size() > best_len))
                    {
                         best_pos = found;
                         best_len = term.size();
                    }
               }
          }

          if (best_pos == std::string::npos)
          {
               result.append(text.substr(pos));
               break;
          }

          result.append(text.substr(pos, best_pos - pos));
          result.append("<em>");
          result.append(text.substr(best_pos, best_len));
          result.append("</em>");
          pos = best_pos + best_len;
     }

     return result;
}

/* Checks whether requested query fields exists. */

static bool HasRequestedQueryFields(const std::vector<std::string> &query_by)
{
     return !query_by.empty();
}

/* Implements the queries only keyword fields helper. */

static bool QueriesOnlyKeywordFields(const CollectionConfig &config,
                                     const std::vector<std::string> &query_by)
{
     if (query_by.empty())
     {
          return false;
     }

     for (const auto &field_name : query_by)
     {
          if (field_name == "id")
          {
               continue;
          }

          const auto field_it = config.Fields.find(field_name);
          if (field_it == config.Fields.end() || ToLowerCopy(field_it->second) != "keyword")
          {
               return false;
          }
     }

     return true;
}

/* Implements the keyword field matches exact helper. */

static bool KeywordFieldMatchesExact(const std::vector<std::pair<std::string, std::string>> &field_values,
                                     const std::string &query,
                                     bool case_sensitive)
{
     std::string normalized_query = TrimWhitespace(StripQuotes(query));
     if (!case_sensitive)
     {
          normalized_query = ToLowerCopy(normalized_query);
     }

     return !normalized_query.empty() &&
            std::any_of(field_values.begin(), field_values.end(), [&](const auto &field_value)
                        {
                             return TrimWhitespace(field_value.second) == normalized_query;
                        });
}

/* Implements the append requested field values helper. */

static void AppendRequestedFieldValues(const Document &doc,
                                       const std::vector<std::string> &query_by,
                                       std::vector<std::pair<std::string, std::string>> &field_values,
                                       bool case_sensitive)
{
     field_values.clear();

     if (query_by.empty())
     {
          if (!doc.Title.empty())
          {
               field_values.push_back({"title", case_sensitive ? doc.Title : ToLowerCopy(doc.Title)});
          }

          if (!doc.Content.empty())
          {
               field_values.push_back({"content", case_sensitive ? doc.Content : ToLowerCopy(doc.Content)});
          }

          for (const auto &field : doc.Fields)
          {
               if (!field.second.empty())
               {
                    field_values.push_back({field.first, case_sensitive ? field.second : ToLowerCopy(field.second)});
               }
          }

          return;
     }

     for (const auto &field_name : query_by)
     {
          if (field_name == "id")
          {
               if (!doc.ID.empty())
               {
                    field_values.push_back({"id", case_sensitive ? doc.ID : ToLowerCopy(doc.ID)});
               }
               continue;
          }

          if (field_name == "title")
          {
               if (!doc.Title.empty())
               {
                    field_values.push_back({"title", case_sensitive ? doc.Title : ToLowerCopy(doc.Title)});
               }
               continue;
          }

          if (field_name == "content")
          {
               if (!doc.Content.empty())
               {
                    field_values.push_back({"content", case_sensitive ? doc.Content : ToLowerCopy(doc.Content)});
               }
               continue;
          }

          auto field_it = doc.Fields.find(field_name);
          if (field_it != doc.Fields.end() && !field_it->second.empty())
          {
               field_values.push_back({field_name, case_sensitive ? field_it->second : ToLowerCopy(field_it->second)});
          }
     }
}

/* Implements the all query terms match requested fields helper. */

static bool AllQueryTermsMatchRequestedFields(const std::vector<std::pair<std::string, std::string>> &field_values,
                                              const std::vector<std::string> &query_terms,
                                              bool allow_prefix_match,
                                              int max_typos,
                                              bool case_sensitive,
                                              int *matched_terms_out = nullptr)
{
     int matched_terms = 0;

     for (const auto &term : query_terms)
     {
          bool term_found = false;

          for (const auto &field_val : field_values)
          {
               if (FieldMatchesTermSimple(field_val.second, term, allow_prefix_match, max_typos, case_sensitive))
               {
                    term_found = true;
                    break;
               }
          }

          if (!term_found)
          {
               if (matched_terms_out)
               {
                    *matched_terms_out = matched_terms;
               }
               return false;
          }

          matched_terms++;
     }

     if (matched_terms_out)
     {
          *matched_terms_out = matched_terms;
     }

     return true;
}

/* Implements the matches any query variant helper. */

static bool MatchesAnyQueryVariant(const std::vector<std::pair<std::string, std::string>> &field_values,
                                   const std::vector<std::vector<std::string>> &variant_terms_list,
                                   bool allow_prefix_match,
                                   int max_typos,
                                   bool case_sensitive,
                                   int *matched_terms_out = nullptr)
{
     int best_matched_terms = 0;

     for (const auto &variant_terms : variant_terms_list)
     {
          if (variant_terms.empty())
          {
               continue;
          }

          int matched_terms = static_cast<int>(variant_terms.size());
          if (AllQueryTermsMatchRequestedFields(field_values,
                                                variant_terms,
                                                allow_prefix_match,
                                                max_typos,
                                                case_sensitive,
                                                &matched_terms))
          {
               if (matched_terms_out)
               {
                    *matched_terms_out = matched_terms;
               }
               return true;
          }

          best_matched_terms = std::max(best_matched_terms, matched_terms);
     }

     if (matched_terms_out)
     {
          *matched_terms_out = best_matched_terms;
     }

     return false;
}

/* Implements the all quoted phrases match requested fields helper. */

static bool AllQuotedPhrasesMatchRequestedFields(const std::vector<std::pair<std::string, std::string>> &field_values,
                                                 const std::vector<std::string> &quoted_phrases)
{
     for (const auto &phrase : quoted_phrases)
     {
          bool phrase_found = false;

          for (const auto &field_val : field_values)
          {
               if (FieldContainsPhraseSimple(field_val.second, phrase))
               {
                    phrase_found = true;
                    break;
               }
          }

          if (!phrase_found)
          {
               return false;
          }
     }

     return true;
}

/* Calculates requested field exact match boost values. */

static double CalculateRequestedFieldExactMatchBoost(const Document &doc,
                                                     const std::vector<std::string> &query_by,
                                                     const std::string &normalized_query_for_boost,
                                                     double exact_match_boost,
                                                     double title_exact_boost,
                                                     bool case_sensitive = false)
{
     if (normalized_query_for_boost.empty())
     {
          return 1.0;
     }

     std::vector<std::pair<std::string, std::string>> field_values;
     AppendRequestedFieldValues(doc, query_by, field_values, case_sensitive);

     double match_boost = 1.0;

     for (const auto &field_val : field_values)
     {
          if (field_val.second.find(normalized_query_for_boost) == std::string::npos)
          {
               continue;
          }

          const bool is_title_like = FieldNameHasToken(field_val.first, {"title", "name", "subject", "headline"});
          if (is_title_like)
          {
               match_boost = std::max(match_boost, title_exact_boost);
          }
          else
          {
               match_boost = std::max(match_boost, exact_match_boost);
          }
     }

     return match_boost;
}

/* Calculates inline term boost values. */

static double CalculateInlineTermBoost(const Document &doc,
                                       const std::vector<std::string> &query_by,
                                       const std::map<std::string, double> &term_boosts,
                                       bool case_sensitive = false)
{
     if (term_boosts.empty())
     {
          return 1.0;
     }

     std::vector<std::pair<std::string, std::string>> field_values;
     AppendRequestedFieldValues(doc, query_by, field_values, case_sensitive);

     double multiplier = 1.0;
     for (const auto &Boost : term_boosts)
     {
          bool matched = false;
          for (const auto &field_val : field_values)
          {
               if (FieldMatchesTermSimple(field_val.second, Boost.first, false, 0, case_sensitive))
               {
                    matched = true;
                    break;
               }
          }

          if (matched)
          {
               multiplier *= std::max(0.1, Boost.second);
          }
     }

     return multiplier;
}

/* ProcessLexicalSearch performs a lexical search. */

/*
 * SearchAPI::ProcessLexicalSearch implementation.
 */

std::vector<SearchHit> SearchAPI::ProcessLexicalSearch(const std::string &Collection, const ComprehensiveSearchQuery &Query)
{
     std::vector<SearchHit> Hits;

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "ProcessLexicalSearch started: collection='" + Collection + "' query='" + Query.Q + "'.");
     }

     std::string SearchQueryVal = Query.Q;

     if (SearchQueryVal.empty())
     {
          return Hits;
     }

     const ParsedQueryExpression ParsedExpression = ParseStructuredQueryExpression(SearchQueryVal, Query.CaseSensitive);
     std::vector<std::string> quoted_phrases = ExtractQuotedPhrases(SearchQueryVal, Query.CaseSensitive);

     if (SearchQueryVal == "*")
     {
          int RequestedLimit = std::numeric_limits<int>::max();

          if (Query.PerPage > 0)
          {
               if (Query.Offset > 0)
               {
                    const long long RequestedRows = static_cast<long long>(Query.Offset) + static_cast<long long>(Query.PerPage);
                    RequestedLimit = RequestedRows >= static_cast<long long>(std::numeric_limits<int>::max())
                                          ? std::numeric_limits<int>::max()
                                          : static_cast<int>(RequestedRows);
               }
               else if (Query.Page > 1)
               {
                    const long long RequestedRows = static_cast<long long>(Query.Page) * static_cast<long long>(Query.PerPage);
                    RequestedLimit = RequestedRows >= static_cast<long long>(std::numeric_limits<int>::max())
                                          ? std::numeric_limits<int>::max()
                                          : static_cast<int>(RequestedRows);
               }
               else
               {
                    RequestedLimit = Query.PerPage;
               }
          }

          const bool NeedsFullPostProcessing = !Query.SortBy.empty() || !Query.FilterBy.empty() ||
                                               !Query.FacetBy.empty() || !Query.GroupBy.empty() ||
                                               !Query.Aggregations.empty();
          if (NeedsFullPostProcessing)
          {
               const size_t CollectionDocumentCount = HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);
               const int PostProcessingLimit = static_cast<int>(std::min<size_t>(CollectionDocumentCount, 10000));
               RequestedLimit = std::max(RequestedLimit, PostProcessingLimit);
          }

          auto AllDocuments = HybridStorageManager::GetInstance().ListDocuments(Collection, RequestedLimit);

          for (const auto &DocObj : AllDocuments)
          {
               SearchHit HitObj;

               HitObj.Document["id"] = DocObj.ID;
               HitObj.Document["title"] = DocObj.Title;
               HitObj.Document["content"] = DocObj.Content;
               HitObj.Document["score"] = std::to_string(DocObj.Score);
               HitObj.Document["timestamp"] = std::to_string(DocObj.Timestamp);

               for (const auto &Field : DocObj.Fields)
               {
                    HitObj.Document[Field.first] = Field.second;
               }

               HitObj.TextMatch = 1.0f;
               HitObj.Weight = CalculateWeight(HitObj);

               Hits.push_back(HitObj);
          }

          return Hits;
     }

     int SafePerPage = (Query.PerPage > 0 && Query.PerPage <= 1000) ? Query.PerPage : 100;
     long long RequiredRows = SafePerPage;

     if (Query.Offset > 0)
     {
          RequiredRows = static_cast<long long>(Query.Offset) + static_cast<long long>(SafePerPage);
     }
     else if (Query.Page > 1)
     {
          RequiredRows = static_cast<long long>(Query.Page) * static_cast<long long>(SafePerPage);
     }

     RequiredRows = std::max<long long>(RequiredRows, static_cast<long long>(SafePerPage) * 10LL);
     int SearchLimit = static_cast<int>(std::min<long long>(10000LL, RequiredRows));

     if (Query.ExhaustiveSearch)
     {
          SearchLimit = 10000;
     }

     if (!quoted_phrases.empty())
     {
          SearchLimit = std::max(SearchLimit, std::min(10000, static_cast<int>(std::min<long long>(10000LL, RequiredRows))));
     }

     auto &storage = HybridStorageManager::GetInstance();

     CollectionConfig collection_config;
     const bool exact_keyword_query =
          !ParsedExpression.UsesStructuredSemantics &&
          !Query.InlineFuzzy &&
          storage.GetCollectionConfig(Collection, collection_config) &&
          QueriesOnlyKeywordFields(collection_config, Query.QueryBy);

     storage.LazyLoadCollectionIndex(Collection);

     size_t collection_docs = storage.GetCollectionDocumentCount(Collection);
     bool collection_is_indexing = storage.IsCollectionIndexing(Collection);

     if (collection_docs == 0 || collection_is_indexing)
     {
          const size_t stored_docs = storage.CountStoredDocuments(Collection);
          if (stored_docs > collection_docs)
          {
               collection_docs = stored_docs;
          }
     }

     bool has_in_memory_index = Instance->SearchIndex->HasInMemoryIndex(Collection);
     bool collection_index_complete = storage.IsCollectionIndexComplete(Collection, collection_docs);

     if (collection_docs > 0 && (collection_is_indexing || !collection_index_complete))
     {
          auto start = Now();
          /*
           * HasInMemoryIndex() becomes true as soon as the first document is
           * indexed. For moderate collections, wait for lazy indexing to finish
           * so a first search cannot miss documents later in the collection.
           */
          const bool small_or_moderate_collection = collection_docs <= 50000;
          const auto max_wait = small_or_moderate_collection ? std::chrono::milliseconds(5000)
                                                             : std::chrono::milliseconds(800);
          const auto bailout_after = small_or_moderate_collection ? std::chrono::milliseconds(2500)
                                                                  : std::chrono::milliseconds(200);
          const auto deadline = Now() + max_wait;

          while (Now() < deadline)
          {
               std::this_thread::sleep_for(std::chrono::milliseconds(50));
               has_in_memory_index = Instance->SearchIndex->HasInMemoryIndex(Collection);
               collection_is_indexing = storage.IsCollectionIndexing(Collection);
               collection_index_complete = storage.IsCollectionIndexComplete(Collection, collection_docs);
               if (collection_index_complete || (!collection_is_indexing && has_in_memory_index) || (!small_or_moderate_collection && has_in_memory_index))
               {
                    break;
               }

               if (!small_or_moderate_collection && Now() - start >= bailout_after)
               {
                    break;
               }
          }

          has_in_memory_index = Instance->SearchIndex->HasInMemoryIndex(Collection);
          collection_index_complete = storage.IsCollectionIndexComplete(Collection, collection_docs);
     }

     if (ParsedExpression.UsesStructuredSemantics &&
         BuildCandidateQueriesFromParsedExpression(ParsedExpression).empty())
     {
          const bool restrict_to_query_fields = HasRequestedQueryFields(Query.QueryBy);
          const bool allow_prefix_match = Query.Prefix && !exact_keyword_query;
          const int requested_limit = Query.PerPage > 0 ? std::min(Query.PerPage, 1000) : 100;
          const int scan_batch = 200;
          int offset = 0;

          while (static_cast<int>(Hits.size()) < requested_limit)
          {
               auto docs = storage.ListDocuments(Collection, scan_batch, offset);
               if (docs.empty())
               {
                    break;
               }

               for (const auto &doc : docs)
               {
                    if (static_cast<int>(Hits.size()) >= requested_limit)
                    {
                         break;
                    }

                    if (!EvaluateParsedQueryExpression(doc,
                                                       ParsedExpression,
                                                       restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                       allow_prefix_match,
                                                       0,
                                                       Query.CaseSensitive))
                    {
                         continue;
                    }

                    SearchHit HitObj;
                    HitObj.Document["id"] = doc.ID;
                    HitObj.Document["title"] = doc.Title;
                    HitObj.Document["content"] = doc.Content;
                    HitObj.Document["score"] = std::to_string(doc.Score);
                    HitObj.Document["timestamp"] = std::to_string(doc.Timestamp);

                    for (const auto &Field : doc.Fields)
                    {
                         HitObj.Document[Field.first] = Field.second;
                    }

                    HitObj.TextMatch = 1.0F;
                    HitObj.Weight = CalculateWeight(HitObj);
                    Hits.push_back(std::move(HitObj));
               }

               offset += scan_batch;
          }

          return Hits;
     }

     std::vector<std::string> QueryVariants;
     const bool restrict_to_query_fields = HasRequestedQueryFields(Query.QueryBy);
     if (exact_keyword_query)
     {
          QueryVariants.push_back(SearchQueryVal);
     }
     else if (ParsedExpression.UsesStructuredSemantics)
     {
          QueryVariants = BuildCandidateQueriesFromParsedExpression(ParsedExpression);
     }
     else
     {
          const std::vector<std::string> RelaxedVariants = BuildRelaxedQueryVariants(SearchQueryVal, Query.DropTokensThreshold, Query.CaseSensitive);
          for (const auto &VariantSeed : RelaxedVariants)
          {
               if (Query.CaseSensitive)
               {
                    QueryVariants.push_back(VariantSeed);
               }
               else
               {
                    auto ExpandedVariants = BuildExpandedQueriesFromSynonyms(VariantSeed, Collection, Query.EnableSynonyms, Query.EnableStopwords);
                    QueryVariants.insert(QueryVariants.end(), ExpandedVariants.begin(), ExpandedVariants.end());
               }
          }
     }
     std::sort(QueryVariants.begin(), QueryVariants.end());
     QueryVariants.erase(std::unique(QueryVariants.begin(), QueryVariants.end()), QueryVariants.end());
     const bool force_structured_scan = ParsedExpression.UsesStructuredSemantics && QueryVariants.empty();
     std::vector<Posting> Postings;
     std::unordered_map<std::string, Posting> PostingByDoc;
     for (const auto &Variant : QueryVariants)
     {
          auto VariantPostings = Instance->SearchIndex->Search(Collection,
                                                               BuildIndexQueryForSearch(Variant, Query.Prefix && !exact_keyword_query),
                                                               SearchLimit,
                                                               restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{});
          for (const auto &Posting : VariantPostings)
          {
               auto It = PostingByDoc.find(Posting.DocumentID);
               if (It == PostingByDoc.end() || Posting.Score > It->second.Score)
               {
                    PostingByDoc[Posting.DocumentID] = Posting;
               }
          }
     }
     Postings.reserve(PostingByDoc.size());
     for (const auto &Pair : PostingByDoc)
     {
          Postings.push_back(Pair.second);
     }

     double exact_match_boost = 1.2;
     double title_exact_boost = 1.6;

     if (Instance && Instance->Config)
     {
          exact_match_boost = Instance->Config->GetExactMatchBoost();
          title_exact_boost = Instance->Config->GetTitleExactBoost();
     }

     std::string normalized_query = StripQuotes(TrimWhitespace(SearchQueryVal));
     if (!Query.CaseSensitive)
     {
          normalized_query = ToLowerCopy(normalized_query);
     }
     std::string normalized_query_for_boost = TrimWhitespace(StripWildcardChars(normalized_query));
     if (!Query.CaseSensitive)
     {
          normalized_query_for_boost = ToLowerCopy(normalized_query_for_boost);
     }
     std::vector<std::string> highlight_terms = BuildHighlightTermsForSearch(SearchQueryVal, Query.CaseSensitive);
     const std::vector<std::string> base_query_terms = ExtractTermsSimple(SearchQueryVal, Query.CaseSensitive);
     const bool require_exact_identifier_tokens =
          !Instance || !Instance->Config || Instance->Config->GetQuerySettingsRequireExactIdentifierTokens();
     const bool has_identifier_like_term =
          std::any_of(base_query_terms.begin(), base_query_terms.end(), IsIdentifierLikeTermSimple);
     std::vector<std::vector<std::string>> query_variant_terms_list;
     query_variant_terms_list.reserve(QueryVariants.size());
     for (const auto &Variant : QueryVariants)
     {
          std::vector<std::string> VariantTerms = ExtractTermsSimple(Variant, Query.CaseSensitive);
          if (!VariantTerms.empty())
          {
               query_variant_terms_list.push_back(std::move(VariantTerms));
          }
     }
     /* An empty variant list after stopword processing means every query term
      * was intentionally removed. Do not restore the unfiltered terms for the
      * storage-scan fallback, or stopwords would only work on indexed reads. */
     if (query_variant_terms_list.empty() && !base_query_terms.empty() &&
         !(Query.EnableStopwords && QueryVariants.empty()))
     {
          query_variant_terms_list.push_back(base_query_terms);
     }
     const int effective_max_typos =
          (!exact_keyword_query && Query.NumTypos > 0 && !base_query_terms.empty() &&
           (!require_exact_identifier_tokens || Query.NumTyposExplicit || !has_identifier_like_term) &&
           static_cast<int>(base_query_terms.size()) <= Query.TypoTokensThreshold)
               ? Query.NumTypos
               : 0;

     has_in_memory_index = Instance->SearchIndex->HasInMemoryIndex(Collection);
     bool collection_indexing = storage.IsCollectionIndexing(Collection);
     collection_index_complete = storage.IsCollectionIndexComplete(Collection, collection_docs);

     const bool needs_typo_scan_fallback = (Query.NumTyposExplicit &&
                                            effective_max_typos > 0 &&
                                            (Postings.empty() || Query.InlineFuzzy) &&
                                            !query_variant_terms_list.empty());

     const bool prefer_storage_scan_while_indexing = (collection_indexing || !collection_index_complete) && collection_docs > 0;
     const bool needs_zero_hit_storage_fallback = Query.AllowScanFallback && Postings.empty() && collection_docs > 0 && !query_variant_terms_list.empty();

     if ((Query.AllowScanFallback || prefer_storage_scan_while_indexing || force_structured_scan || needs_typo_scan_fallback || needs_zero_hit_storage_fallback) &&
         (Postings.empty() || prefer_storage_scan_while_indexing || force_structured_scan || needs_typo_scan_fallback) &&
         (!has_in_memory_index || needs_zero_hit_storage_fallback || prefer_storage_scan_while_indexing || force_structured_scan || needs_typo_scan_fallback))
     {
          const bool allow_prefix_match = Query.Prefix && !exact_keyword_query;

          if (!query_variant_terms_list.empty())
          {
               const int scan_batch = 200;
               size_t scanned = 0;
               int offset = 0;

               const bool require_complete_scan = prefer_storage_scan_while_indexing || Query.ExhaustiveSearch || needs_zero_hit_storage_fallback;
               const size_t max_scan_docs = (collection_docs > 0)
                                                 ? std::min<size_t>(collection_docs, require_complete_scan ? collection_docs : 10000)
                                                 : ((Query.ExhaustiveSearch || needs_zero_hit_storage_fallback) ? 50000 : 10000);
               const auto scan_deadline =
                    Now() +
                    (require_complete_scan
                          ? std::chrono::milliseconds(collection_docs <= 50000 ? 5000 : 10000)
                          : (collection_docs > 2000 ? std::chrono::milliseconds(1200) : std::chrono::milliseconds(800)));
               int scan_iterations = 0;
               const int max_scan_iterations = std::max<int>(5, static_cast<int>(max_scan_docs / scan_batch) + 4);

               while (scanned < max_scan_docs && static_cast<int>(Hits.size()) < SearchLimit)
               {
                    if (++scan_iterations > max_scan_iterations)
                    {
                         break;
                    }

                    if (Now() >= scan_deadline)
                    {
                         break;
                    }

                    auto docs = storage.ListDocuments(Collection, scan_batch, offset);
                    if (docs.empty())
                    {
                         break;
                    }

                    for (const auto &doc : docs)
                    {
                         if (scanned >= max_scan_docs || static_cast<int>(Hits.size()) >= SearchLimit)
                         {
                              break;
                         }

                         scanned++;

                         std::vector<std::pair<std::string, std::string>> fields;
                         AppendRequestedFieldValues(doc, restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{}, fields, Query.CaseSensitive || exact_keyword_query);

                         int matched_terms = 0;
                         bool query_matches = true;

                         if (ParsedExpression.UsesStructuredSemantics)
                         {
                              query_matches = EvaluateParsedQueryExpression(doc,
                                                                            ParsedExpression,
                                                                            restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                            allow_prefix_match,
                                                                            effective_max_typos,
                                                                            Query.CaseSensitive);
                         }
                         else
                         {
                              query_matches = exact_keyword_query
                                                   ? KeywordFieldMatchesExact(fields, SearchQueryVal, true)
                                                   : MatchesAnyQueryVariant(fields,
                                                                            query_variant_terms_list,
                                                                            allow_prefix_match,
                                                                            effective_max_typos,
                                                                            Query.CaseSensitive,
                                                                            &matched_terms);
                         }

                         if (!query_matches)
                         {
                              continue;
                         }

                         if (!ParsedExpression.UsesStructuredSemantics &&
                             !quoted_phrases.empty() &&
                             !AllQuotedPhrasesMatchRequestedFields(fields, quoted_phrases))
                         {
                              continue;
                         }

                         SearchHit HitObj;
                         HitObj.Document["id"] = doc.ID;
                         HitObj.Document["title"] = doc.Title;
                         HitObj.Document["content"] = doc.Content;
                         HitObj.Document["score"] = std::to_string(doc.Score);
                         HitObj.Document["timestamp"] = std::to_string(doc.Timestamp);

                         for (const auto &Field : doc.Fields)
                         {
                              HitObj.Document[Field.first] = Field.second;
                         }

                         float score = static_cast<float>(matched_terms);

                         if (Query.PrioritizeExactMatch && !normalized_query_for_boost.empty())
                         {
                              double match_boost = CalculateRequestedFieldExactMatchBoost(doc,
                                                                                          restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                                          normalized_query_for_boost,
                                                                                          exact_match_boost,
                                                                                          title_exact_boost,
                                                                                          Query.CaseSensitive);
                              score = static_cast<float>(score * match_boost);
                         }

                         const double inline_term_boost = CalculateInlineTermBoost(doc,
                                                                                   restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                                   Query.TermBoosts,
                                                                                   Query.CaseSensitive);
                         if (inline_term_boost != 1.0)
                         {
                              score = static_cast<float>(score * inline_term_boost);
                         }

                         HitObj.TextMatch = score;
                         if (Query.Highlight)
                         {
                              if (!doc.Title.empty() && (!restrict_to_query_fields || std::find(Query.QueryBy.begin(), Query.QueryBy.end(), "title") != Query.QueryBy.end()))
                              {
                                   HitObj.Highlights["title"] = HighlightTermsSimple(doc.Title, highlight_terms.empty() ? base_query_terms : highlight_terms, Query.CaseSensitive);
                              }
                              if (!doc.Content.empty() && (!restrict_to_query_fields || std::find(Query.QueryBy.begin(), Query.QueryBy.end(), "content") != Query.QueryBy.end()))
                              {
                                   HitObj.Highlights["content"] = HighlightTermsSimple(doc.Content, highlight_terms.empty() ? base_query_terms : highlight_terms, Query.CaseSensitive);
                              }
                              for (const auto &Field : doc.Fields)
                              {
                                   if (Field.second.empty())
                                   {
                                        continue;
                                   }

                                   if (restrict_to_query_fields &&
                                       std::find(Query.QueryBy.begin(), Query.QueryBy.end(), Field.first) == Query.QueryBy.end())
                                   {
                                        continue;
                                   }

                                   HitObj.Highlights[Field.first] = HighlightTermsSimple(Field.second, highlight_terms.empty() ? base_query_terms : highlight_terms, Query.CaseSensitive);
                              }
                         }
                         HitObj.Weight = CalculateWeight(HitObj);
                         Hits.push_back(HitObj);
                    }

                    offset += scan_batch;
               }

               if (!Hits.empty())
               {
                    std::sort(Hits.begin(), Hits.end(), [](const SearchHit &A, const SearchHit &B)
                              {
                                   return A.TextMatch > B.TextMatch;
                              });
               }
          }

          return Hits;
     }

     for (const auto &Posting : Postings)
     {
          Document StorageDoc = HybridStorageManager::GetInstance().GetDocument(Collection, Posting.DocumentID);

          if (!StorageDoc.ID.empty())
          {
               std::vector<std::pair<std::string, std::string>> selected_fields;
               AppendRequestedFieldValues(StorageDoc, restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{}, selected_fields, Query.CaseSensitive || exact_keyword_query);

               if (ParsedExpression.UsesStructuredSemantics)
               {
                    if (!EvaluateParsedQueryExpression(StorageDoc,
                                                       ParsedExpression,
                                                       restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                       Query.Prefix,
                                                       effective_max_typos,
                                                       Query.CaseSensitive))
                    {
                         continue;
                    }
               }
               else if (exact_keyword_query &&
                        !KeywordFieldMatchesExact(selected_fields, SearchQueryVal, true))
               {
                    continue;
               }
               else if (!exact_keyword_query && !query_variant_terms_list.empty() &&
                        !MatchesAnyQueryVariant(selected_fields, query_variant_terms_list, Query.Prefix, effective_max_typos, Query.CaseSensitive, nullptr))
               {
                    continue;
               }

               if (!ParsedExpression.UsesStructuredSemantics && !quoted_phrases.empty())
               {
                    if (!AllQuotedPhrasesMatchRequestedFields(selected_fields, quoted_phrases))
                    {
                         continue;
                    }
               }

               SearchHit HitObj;

               HitObj.Document["id"] = StorageDoc.ID;
               HitObj.Document["title"] = StorageDoc.Title;
               HitObj.Document["content"] = StorageDoc.Content;
               HitObj.Document["score"] = std::to_string(StorageDoc.Score);
               HitObj.Document["timestamp"] = std::to_string(StorageDoc.Timestamp);

               for (const auto &Field : StorageDoc.Fields)
               {
                    HitObj.Document[Field.first] = Field.second;
               }

               HitObj.TextMatch = static_cast<float>(Posting.Score);

               if (Query.PrioritizeExactMatch && !normalized_query_for_boost.empty())
               {
                    double match_boost = CalculateRequestedFieldExactMatchBoost(StorageDoc,
                                                                                restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                                normalized_query_for_boost,
                                                                                exact_match_boost,
                                                                                title_exact_boost,
                                                                                Query.CaseSensitive);

                    if (match_boost > 1.0)
                    {
                         HitObj.TextMatch = static_cast<float>(HitObj.TextMatch * match_boost);
                    }
               }

               const double inline_term_boost = CalculateInlineTermBoost(StorageDoc,
                                                                         restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                         Query.TermBoosts,
                                                                         Query.CaseSensitive);
               if (inline_term_boost != 1.0)
               {
                    HitObj.TextMatch = static_cast<float>(HitObj.TextMatch * inline_term_boost);
               }

               HitObj.Weight = CalculateWeight(HitObj);

               Hits.push_back(HitObj);
          }
     }

     if (Hits.empty() && Query.AllowScanFallback && collection_docs > 0 && !query_variant_terms_list.empty())
     {
          const bool allow_prefix_match = Query.Prefix && !exact_keyword_query;
          const int scan_batch = 200;
          size_t scanned = 0;
          int offset = 0;
          const size_t max_scan_docs = collection_docs;
          const auto scan_deadline = Now() + std::chrono::milliseconds(collection_docs <= 50000 ? 5000 : 2500);

          while (scanned < max_scan_docs && static_cast<int>(Hits.size()) < SearchLimit)
          {
               if (Now() >= scan_deadline)
               {
                    break;
               }

               auto docs = storage.ListDocuments(Collection, scan_batch, offset);
               if (docs.empty())
               {
                    break;
               }

               for (const auto &doc : docs)
               {
                    if (scanned >= max_scan_docs || static_cast<int>(Hits.size()) >= SearchLimit)
                    {
                         break;
                    }

                    scanned++;

                    std::vector<std::pair<std::string, std::string>> fields;
                    AppendRequestedFieldValues(doc, restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{}, fields, Query.CaseSensitive || exact_keyword_query);

                    int matched_terms = 0;
                    bool query_matches = ParsedExpression.UsesStructuredSemantics
                                              ? EvaluateParsedQueryExpression(doc,
                                                                              ParsedExpression,
                                                                              restrict_to_query_fields ? Query.QueryBy : std::vector<std::string>{},
                                                                              allow_prefix_match,
                                                                              effective_max_typos,
                                                                              Query.CaseSensitive)
                                         : exact_keyword_query
                                              ? KeywordFieldMatchesExact(fields, SearchQueryVal, true)
                                              : MatchesAnyQueryVariant(fields,
                                                                       query_variant_terms_list,
                                                                       allow_prefix_match,
                                                                       effective_max_typos,
                                                                       Query.CaseSensitive,
                                                                       &matched_terms);

                    if (!query_matches)
                    {
                         continue;
                    }

                    if (!ParsedExpression.UsesStructuredSemantics &&
                        !quoted_phrases.empty() &&
                        !AllQuotedPhrasesMatchRequestedFields(fields, quoted_phrases))
                    {
                         continue;
                    }

                    SearchHit HitObj;
                    HitObj.Document["id"] = doc.ID;
                    HitObj.Document["title"] = doc.Title;
                    HitObj.Document["content"] = doc.Content;
                    HitObj.Document["score"] = std::to_string(doc.Score);
                    HitObj.Document["timestamp"] = std::to_string(doc.Timestamp);

                    for (const auto &Field : doc.Fields)
                    {
                         HitObj.Document[Field.first] = Field.second;
                    }

                    HitObj.TextMatch = static_cast<float>(std::max(1, matched_terms));
                    HitObj.Weight = CalculateWeight(HitObj);
                    Hits.push_back(HitObj);
               }

               offset += scan_batch;
          }

          if (!Hits.empty())
          {
               std::sort(Hits.begin(), Hits.end(), [](const SearchHit &A, const SearchHit &B)
                         {
                              return A.TextMatch > B.TextMatch;
                         });
          }
     }

     return Hits;
}
