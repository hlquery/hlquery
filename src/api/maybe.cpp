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
#include <limits>
#include <rapidfuzz/fuzz.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/common.h"
#include "vendor/json/json.hpp"

static std::string ToLowerCopy(const std::string &value)
{
     std::string out = value;
     std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                    {
                         return static_cast<char>(std::tolower(c));
                    });
     return out;
}

static std::string CollapseWhitespace(const std::string &value)
{
     std::ostringstream out;
     bool in_space = false;
     for (unsigned char c : value)
     {
          if (std::isspace(c))
          {
               if (!in_space)
               {
                    out << ' ';
                    in_space = true;
               }
               continue;
          }

          out << static_cast<char>(c);
          in_space = false;
     }

     std::string result = out.str();
     while (!result.empty() && result.front() == ' ')
     {
          result.erase(result.begin());
     }
     while (!result.empty() && result.back() == ' ')
     {
          result.pop_back();
     }
     return result;
}

static std::vector<std::string> TokenizeTerms(const std::string &query)
{
     std::vector<std::string> out;
     std::unordered_set<std::string> seen;
     std::istringstream iss(query);
     std::string token;

     while (iss >> token)
     {
          std::string norm;
          for (unsigned char c : token)
          {
               if (std::isalnum(c))
               {
                    norm.push_back(static_cast<char>(std::tolower(c)));
               }
          }

          if (norm.size() < 2 || !seen.insert(norm).second)
          {
               continue;
          }

          out.push_back(norm);
          if (out.size() >= 8)
          {
               break;
          }
     }

     return out;
}

static std::string BuildSnippet(const std::string &raw, const std::vector<std::string> &terms)
{
     std::string text = CollapseWhitespace(raw);
     if (text.empty())
     {
          return "";
     }

     const size_t max_len = 96;
     std::string lower = ToLowerCopy(text);
     size_t hit_pos = std::string::npos;

     for (const auto &term : terms)
     {
          size_t pos = lower.find(term);
          if (pos != std::string::npos && (hit_pos == std::string::npos || pos < hit_pos))
          {
               hit_pos = pos;
          }
     }

     size_t start = 0;
     if (hit_pos != std::string::npos && hit_pos > 28)
     {
          start = hit_pos - 28;
     }

     std::string out = text.substr(start, max_len);
     if (start > 0)
     {
          out = "..." + out;
     }
     if (start + max_len < text.size())
     {
          out += "...";
     }

     return CollapseWhitespace(out);
}

static std::string KeepAlphaNumSpaces(const std::string &value)
{
     std::string out;
     out.reserve(value.size());
     bool last_space = false;
     for (unsigned char c : value)
     {
          if (std::isalnum(c))
          {
               out.push_back(static_cast<char>(std::tolower(c)));
               last_space = false;
               continue;
          }
          if (!last_space)
          {
               out.push_back(' ');
               last_space = true;
          }
     }
     return CollapseWhitespace(out);
}

static std::string RemoveDigitsKeepSpaces(const std::string &value)
{
     std::string out;
     out.reserve(value.size());
     bool last_space = false;
     for (unsigned char c : value)
     {
          if (std::isdigit(c))
          {
               continue;
          }
          if (std::isalnum(c))
          {
               out.push_back(static_cast<char>(std::tolower(c)));
               last_space = false;
               continue;
          }
          if (!last_space)
          {
               out.push_back(' ');
               last_space = true;
          }
     }
     return CollapseWhitespace(out);
}

static bool IsSuggestionStopword(const std::string &value)
{
     static const std::unordered_set<std::string> Stopwords = {
          "and", "the", "for", "with", "from", "that", "this", "into", "onto", "your", "their", "about",
          "guide", "brief", "notes", "review", "study", "field", "weekly", "practical", "insider", "trends",
          "beginner", "expert", "deep", "dive", "spotlight", "handbook"};

     return Stopwords.find(value) != Stopwords.end();
}

static std::vector<std::string> ExtractCandidateTerms(const std::string &value)
{
     std::vector<std::string> Terms;
     std::unordered_set<std::string> Seen;
     std::string Current;

     for (unsigned char c : value)
     {
          if (std::isalnum(c))
          {
               Current.push_back(static_cast<char>(std::tolower(c)));
               continue;
          }

          if (!Current.empty())
          {
               if (Current.size() >= 3 && Current.size() <= 32 && !IsSuggestionStopword(Current) && Seen.insert(Current).second)
               {
                    Terms.push_back(Current);
               }

               Current.clear();
          }
     }

     if (!Current.empty())
     {
          if (Current.size() >= 3 && Current.size() <= 32 && !IsSuggestionStopword(Current) && Seen.insert(Current).second)
          {
               Terms.push_back(Current);
          }
     }

     return Terms;
}

static std::string SingularizeTerm(const std::string &value)
{
     if (value.size() > 4 && value.size() >= 3 && value.substr(value.size() - 3) == "ies")
     {
          return value.substr(0, value.size() - 3) + "y";
     }

     if (value.size() > 3 && value.back() == 's' && value[value.size() - 2] != 's')
     {
          return value.substr(0, value.size() - 1);
     }

     return value;
}

static std::string PluralizeTerm(const std::string &value)
{
     if (value.empty())
     {
          return value;
     }

     if (value.back() == 'y' && value.size() > 2)
     {
          return value.substr(0, value.size() - 1) + "ies";
     }

     if (value.back() == 's')
     {
          return value;
     }

     return value + "s";
}

static std::vector<std::string> BuildQueryVariants(const std::string &query)
{
     std::vector<std::string> out;
     std::unordered_set<std::string> seen;

     auto push_variant = [&](const std::string &raw)
     {
          std::string normalized = CollapseWhitespace(raw);
          if (normalized.empty())
          {
               return;
          }
          std::string key = ToLowerCopy(normalized);
          if (seen.insert(key).second)
          {
               out.push_back(normalized);
          }
     };

     push_variant(query);
     push_variant(KeepAlphaNumSpaces(query));
     push_variant(RemoveDigitsKeepSpaces(query));

     std::vector<std::string> terms = TokenizeTerms(RemoveDigitsKeepSpaces(query));
     if (terms.size() == 1 && terms[0].size() >= 4)
     {
          push_variant(terms[0].substr(0, terms[0].size() - 1));
     }

     return out;
}

static bool IsLowValueSuggestion(const std::string &candidate)
{
     std::string lower = ToLowerCopy(candidate);
     if (lower.empty())
     {
          return true;
     }
     if (lower.find("lorem ipsum") != std::string::npos)
     {
          return true;
     }
     if (lower.rfind("document ", 0) == 0 && lower.find(" collection ") != std::string::npos)
     {
          return true;
     }
     return false;
}

static std::string SelectCandidate(const SearchHit &hit,
                                   const std::vector<std::string> &terms)
{
     static const std::vector<std::string> strong_fields = {
          "title", "name", "question", "subject", "headline", "summary", "field_0"};

     static const std::vector<std::string> weak_fields = {
          "description", "content", "field_1", "field_2"};

     auto pick_from_fields = [&](const std::vector<std::string> &fields) -> std::string
     {
          for (const auto &field : fields)
          {
               auto it = hit.Document.find(field);
               if (it == hit.Document.end() || it->second.empty())
               {
                    continue;
               }
               std::string value = BuildSnippet(it->second, terms);
               value = CollapseWhitespace(value);
               if (!value.empty() && !IsLowValueSuggestion(value))
               {
                    return value;
               }
          }
          return "";
     };

     std::string selected = pick_from_fields(strong_fields);
     if (!selected.empty())
     {
          return selected;
     }

     selected = pick_from_fields(weak_fields);
     if (!selected.empty())
     {
          return selected;
     }

     for (const auto &kv : hit.Document)
     {
          if (kv.first == "id" || kv.second.empty())
          {
               continue;
          }
          std::string value = BuildSnippet(kv.second, terms);
          value = CollapseWhitespace(value);
          if (!value.empty() && !IsLowValueSuggestion(value))
          {
               return value;
          }
     }

     return "";
}

static double ComputeRapidFuzzSimilarity(const std::string &query, const std::string &candidate)
{
     if (query.empty() || candidate.empty())
     {
          return 0.0;
     }

     const double RatioScore = rapidfuzz::fuzz::ratio(query, candidate);
     const double PartialScore = rapidfuzz::fuzz::partial_ratio(query, candidate);
     const double TokenSetScore = rapidfuzz::fuzz::token_set_ratio(query, candidate);
     const double TokenSortScore = rapidfuzz::fuzz::token_sort_ratio(query, candidate);

     return (TokenSetScore * 0.40) + (PartialScore * 0.30) + (RatioScore * 0.20) + (TokenSortScore * 0.10);
}

static double ComputeTermSuggestionRank(const std::string &query_raw, const std::string &candidate, const std::unordered_set<std::string> &collection_terms)
{
     const std::string Query = KeepAlphaNumSpaces(query_raw);
     const std::string Candidate = KeepAlphaNumSpaces(candidate);

     if (Query.empty() || Candidate.empty())
     {
          return 0.0;
     }

     double Score = ComputeRapidFuzzSimilarity(Query, Candidate);

     if (Candidate.find(Query) != std::string::npos)
     {
          Score += 12.0;
     }

     size_t PrefixLen = 0;

     while (PrefixLen < Query.size() && PrefixLen < Candidate.size() && Query[PrefixLen] == Candidate[PrefixLen])
     {
          PrefixLen++;
     }

     Score += static_cast<double>(PrefixLen) * 3.5;

     int QueryDigits = 0;
     int CandidateDigits = 0;

     for (unsigned char c : Query)
     {
          if (std::isdigit(c))
          {
               QueryDigits++;
          }
     }

     for (unsigned char c : Candidate)
     {
          if (std::isdigit(c))
          {
               CandidateDigits++;
          }
     }

     if (QueryDigits > 0 && CandidateDigits == 0)
     {
          Score += 8.0;
     }

     if (collection_terms.find(Candidate) != collection_terms.end())
     {
          Score += 26.0;
     }
     else
     {
          const std::string SingularCandidate = SingularizeTerm(Candidate);
          const std::string PluralCandidate = PluralizeTerm(Candidate);

          if (collection_terms.find(SingularCandidate) != collection_terms.end())
          {
               Score += 10.0;
          }

          if (collection_terms.find(PluralCandidate) != collection_terms.end())
          {
               Score += 18.0;
          }
     }

     return Score;
}

static double ComputeSuggestionRank(const std::string &query_raw,
                                    const std::string &candidate_raw,
                                    const std::string &doc_id)
{
     std::string query = KeepAlphaNumSpaces(query_raw);
     std::string candidate = KeepAlphaNumSpaces(candidate_raw);
     if (query.empty() || candidate.empty())
     {
          return 0.0;
     }

     double score = 0.0;
     size_t contains_pos = candidate.find(query);
     if (contains_pos != std::string::npos)
     {
          score += 120.0 - static_cast<double>(std::min<size_t>(contains_pos, 80));
     }

     size_t prefix = 0;
     while (prefix < query.size() && prefix < candidate.size() && query[prefix] == candidate[prefix])
     {
          prefix++;
     }
     score += static_cast<double>(prefix) * 6.0;

     score += ComputeRapidFuzzSimilarity(query, candidate) * 0.65;

     if (!doc_id.empty())
     {
          std::string normalized_id = KeepAlphaNumSpaces(doc_id);
          if (normalized_id.find(query) != std::string::npos)
          {
               score += 18.0;
          }
     }

     int query_digits = 0;
     int query_alpha = 0;
     for (unsigned char c : query_raw)
     {
          if (std::isdigit(c))
          {
               query_digits++;
          }
          else if (std::isalpha(c))
          {
               query_alpha++;
          }
     }
     int candidate_digits = 0;
     for (unsigned char c : candidate_raw)
     {
          if (std::isdigit(c))
          {
               candidate_digits++;
          }
     }

     if (query_digits >= 2 && candidate_digits == 0)
     {
          if (query_digits > query_alpha)
          {
               score -= 36.0;
          }
          else
          {
               score -= 24.0;
          }
     }

     return score;
}

static ComprehensiveSearchQuery BuildMaybeQuery(const std::string &query, int per_page)
{
     ComprehensiveSearchQuery q;
     q.Q = query;
     q.PerPage = std::max(1, per_page);
     q.Page = 1;
     q.PrioritizeExactMatch = true;
     q.AllowScanFallback = false;
     q.Prefix = true;
     q.NumTypos = 2;
     q.DropTokensThreshold = 1;
     q.TypoTokensThreshold = 2;
     return q;
}

static void CollectSuggestions(const ComprehensiveSearchResult &result,
                               const std::string &collection,
                               const std::vector<std::string> &terms,
                               const std::string &query_for_rank,
                               double min_rank_score,
                               int limit,
                               std::vector<nlohmann::json> *out,
                               std::unordered_set<std::string> *dedupe)
{
     for (const auto &hit : result.Hits)
     {
          std::string candidate = SelectCandidate(hit, terms);
          candidate = CollapseWhitespace(candidate);
          if (candidate.empty())
          {
               continue;
          }
          if (candidate.size() > 140)
          {
               candidate = candidate.substr(0, 137) + "...";
          }

          std::string key = ToLowerCopy(candidate);
          if (!dedupe->insert(key).second)
          {
               continue;
          }

          nlohmann::json row;
          row["text"] = candidate;
          row["collection"] = collection;

          std::string doc_id;
          auto id_it = hit.Document.find("id");
          if (id_it != hit.Document.end())
          {
               doc_id = id_it->second;
               row["id"] = doc_id;
          }

          float base_score = hit.HybridScore > 0.0f ? hit.HybridScore : (hit.VectorScore > 0.0f ? hit.VectorScore : hit.TextMatch);
          double rank_score = ComputeSuggestionRank(query_for_rank, candidate, doc_id);
          if (rank_score < min_rank_score)
          {
               continue;
          }
          row["score"] = base_score + static_cast<float>(rank_score / 100.0);
          out->push_back(std::move(row));

          if (static_cast<int>(out->size()) >= limit)
          {
               return;
          }
     }
}

static void CollectSuggestionsFromSample(const std::string &collection,
                                         const std::vector<std::string> &terms,
                                         const std::string &query_for_rank,
                                         double min_rank_score,
                                         int limit,
                                         std::vector<nlohmann::json> *out,
                                         std::unordered_set<std::string> *dedupe)
{
     if (static_cast<int>(out->size()) >= limit)
     {
          return;
     }

     const int sample_size = std::min(240, std::max(60, limit * 24));
     std::vector<Document> docs = HybridStorageManagerInstance().ListDocuments(collection, sample_size, 0);

     for (const auto &doc : docs)
     {
          if (static_cast<int>(out->size()) >= limit)
          {
               return;
          }

          SearchHit hit;
          hit.Document["id"] = doc.ID;
          hit.Document["title"] = doc.Title;
          hit.Document["content"] = doc.Content;
          for (const auto &field : doc.Fields)
          {
               hit.Document[field.first] = field.second;
          }

          std::string candidate = SelectCandidate(hit, terms);
          candidate = CollapseWhitespace(candidate);
          if (candidate.empty())
          {
               continue;
          }
          if (candidate.size() > 140)
          {
               candidate = candidate.substr(0, 137) + "...";
          }

          std::string key = ToLowerCopy(candidate);
          if (!dedupe->insert(key).second)
          {
               continue;
          }

          const double rank_score = ComputeSuggestionRank(query_for_rank, candidate, doc.ID);
          if (rank_score < min_rank_score)
          {
               continue;
          }

          nlohmann::json row;
          row["text"] = candidate;
          row["collection"] = collection;
          if (!doc.ID.empty())
          {
               row["id"] = doc.ID;
          }
          row["score"] = static_cast<float>(rank_score / 100.0);
          out->push_back(std::move(row));
     }
}

static void CollectQuerySuggestionsFromSample(const std::string &collection,
                                              const std::string &query_for_rank,
                                              int limit,
                                              std::vector<nlohmann::json> *out,
                                              std::unordered_set<std::string> *dedupe)
{
     if (static_cast<int>(out->size()) >= limit)
     {
          return;
     }

     const std::string CollectionLower = ToLowerCopy(collection);
     std::unordered_set<std::string> CollectionTerms;
     const int SampleSize = std::min(320, std::max(80, limit * 40));
     const std::vector<Document> Docs = HybridStorageManagerInstance().ListDocuments(collection, SampleSize, 0);

     struct RankedTerm
     {
          std::string Term;
          int Frequency = 0;
          double Score = 0.0;
     };

     std::unordered_map<std::string, RankedTerm> Ranked;

     for (const auto &CollectionTerm : ExtractCandidateTerms(CollectionLower))
     {
          CollectionTerms.insert(CollectionTerm);
          CollectionTerms.insert(SingularizeTerm(CollectionTerm));
          CollectionTerms.insert(PluralizeTerm(CollectionTerm));
     }

     auto ConsiderText = [&](const std::string &Text)
     {
          const std::vector<std::string> Terms = ExtractCandidateTerms(Text);

          for (const auto &Term : Terms)
          {
               if (Term == CollectionLower)
               {
                    continue;
               }

               const double SimilarityScore = ComputeTermSuggestionRank(query_for_rank, Term, CollectionTerms);

               if (SimilarityScore < 72.0)
               {
                    continue;
               }

               auto &Entry = Ranked[Term];

               Entry.Term = Term;
               Entry.Frequency++;
               Entry.Score = std::max(Entry.Score, SimilarityScore);
          }
     };

     for (const auto &Doc : Docs)
     {
          ConsiderText(Doc.Title);
          ConsiderText(Doc.Content);

          for (const auto &Field : Doc.Fields)
          {
               if (Field.first == "id")
               {
                    continue;
               }

               ConsiderText(Field.second);
          }
     }

     std::vector<RankedTerm> Candidates;

     for (const auto &Pair : Ranked)
     {
          RankedTerm Entry = Pair.second;
          Entry.Score += static_cast<double>(std::min(Entry.Frequency, 6)) * 4.0;

          if (CollectionTerms.find(Entry.Term) != CollectionTerms.end())
          {
               Entry.Score += 18.0;
          }

          const std::string SingularTerm = SingularizeTerm(Entry.Term);
          const std::string PluralTerm = PluralizeTerm(Entry.Term);

          if (CollectionTerms.find(PluralTerm) != CollectionTerms.end() && PluralTerm == Entry.Term)
          {
               Entry.Score += 14.0;
          }

          if (CollectionTerms.find(SingularTerm) != CollectionTerms.end() && SingularTerm != Entry.Term)
          {
               Entry.Score -= 6.0;
          }

          Candidates.push_back(std::move(Entry));
     }

     std::sort(Candidates.begin(), Candidates.end(), [](const RankedTerm &A, const RankedTerm &B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Frequency != B.Frequency)
                    {
                         return A.Frequency > B.Frequency;
                    }

                    return A.Term < B.Term;
               });

     for (const auto &Candidate : Candidates)
     {
          if (static_cast<int>(out->size()) >= limit)
          {
               return;
          }

          const std::string Key = ToLowerCopy(Candidate.Term);

          if (!dedupe->insert(Key).second)
          {
               continue;
          }

          nlohmann::json Row;
          Row["text"] = Candidate.Term;
          Row["collection"] = collection;
          Row["kind"] = "query";
          Row["score"] = static_cast<float>(Candidate.Score / 10.0);
          Row["frequency"] = Candidate.Frequency;

          out->push_back(std::move(Row));
     }
}

HttpResponse SearchAPI::HandleMaybe(const HttpRequest &Request)
{
     if (Request.Method != "GET" && Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string collection_name = ExtractCollectionFromPath(Request.Path);
     if (collection_name.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::COLLECTION_INVALID_NAME, "Invalid collection name.", "Could not extract collection name from path.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(collection_name))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found.", "Collection '" + collection_name + "' does not exist.");
     }

     std::unordered_map<std::string, std::string> params;
     if (Request.Method == "GET")
     {
          params.insert(Request.QueryParams.begin(), Request.QueryParams.end());
     }
     else
     {
          try
          {
               params = ParseSearchParamsFromJSON(Request.Body);
          }
          catch (...)
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid JSON body.", "Failed to parse maybe parameters.");
          }
     }

     std::string query;
     auto q_it = params.find("q");
     if (q_it != params.end())
     {
          query = CollapseWhitespace(q_it->second);
     }

     if (query.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_QUERY, "Invalid query.", "Parameter 'q' is required.");
     }

     int limit = 5;
     auto limit_it = params.find("limit");
     if (limit_it != params.end())
     {
          try
          {
               limit = std::stoi(limit_it->second);
          }
          catch (...)
          {
               limit = 5;
          }
     }
     if (limit < 1)
     {
          limit = 1;
     }
     if (limit > 20)
     {
          limit = 20;
     }

     const int per_collection_probe = std::max(10, limit * 3);
     const std::vector<std::string> terms = TokenizeTerms(query);
     const std::vector<std::string> query_variants = BuildQueryVariants(query);

     std::vector<nlohmann::json> suggestions;
     std::unordered_set<std::string> dedupe;

     CollectQuerySuggestionsFromSample(collection_name, query, limit, &suggestions, &dedupe);

     for (const auto &query_variant : query_variants)
     {
          ComprehensiveSearchQuery target_query = BuildMaybeQuery(query_variant, per_collection_probe);
          ComprehensiveSearchResult target_result = PerformComprehensiveSearch(collection_name, target_query);
          CollectSuggestions(target_result, collection_name, terms, query, 0.0, limit, &suggestions, &dedupe);
          if (static_cast<int>(suggestions.size()) >= limit)
          {
               break;
          }
     }

     if (suggestions.empty())
     {
          CollectSuggestionsFromSample(collection_name, terms, query, 16.0, limit, &suggestions, &dedupe);
     }

     if (static_cast<int>(suggestions.size()) > limit)
     {
          suggestions.resize(static_cast<size_t>(limit));
     }

     nlohmann::json root;
     root["query"] = query;
     root["collection"] = collection_name;
     root["limit"] = limit;
     root["count"] = suggestions.size();
     root["fallback_used"] = false;
     root["fallback_collections"] = nlohmann::json::array();
     root["suggestions"] = nlohmann::json::array();

     for (const auto &s : suggestions)
     {
          root["suggestions"].push_back(s);
     }

     if (suggestions.empty())
     {
          root["message"] = "nothing man";
     }
     else
     {
          root["message"] = "ok";
     }

     HttpResponse response(Status::OK, StatusText(Status::OK), "application/json");
     response.Body = root.dump();
     return response;
}
