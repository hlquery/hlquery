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
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "core/config.h"
#include "core/hlquery.h"
#include "runtime/serverconfig.h"
#include "search/bm25_scoring.h"
#include "search/lexical_inverted_index.h"
#include "search/mapped_posting_index.h"
#include "utils/wildcard.h"

/* ToLowerAsciiSafe - Lowercases one byte using unsigned-char promotion to avoid undefined behavior. */

static char ToLowerAsciiSafe(char ch)
{
     return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

/* BuildFieldScopedTerm - Builds the synthetic term used for field-restricted search clauses. */

static std::string BuildFieldScopedTerm(const std::string &field_name, const std::string &term)
{
     return "__field__" + field_name + ":" + term;
}

/* AddQueryTermVariants - Adds conservative singular/plural variants for lexical lookup. */

static void AddQueryTermVariants(const std::string &term, std::vector<std::pair<std::string, double>> &variants)
{
     if (term.empty())
     {
          return;
     }

     auto AddVariant = [&variants](const std::string &value, double weight)
     {
          if (value.empty())
          {
               return;
          }

          for (const auto &Existing : variants)
          {
               if (Existing.first == value)
               {
                    return;
               }
          }

          variants.push_back({value, weight});
     };

     AddVariant(term, 1.0);

     if (term.size() <= 3 || term.find('*') != std::string::npos || term.find('?') != std::string::npos)
     {
          return;
     }

     const char Last = term.back();

     if (term.size() > 4 && term.substr(term.size() - 3) == "ies")
     {
          AddVariant(term.substr(0, term.size() - 3) + "y", 0.92);
     }
     else if (Last == 'y')
     {
          const char BeforeLast = term[term.size() - 2];
          if (std::string("aeiou").find(BeforeLast) == std::string::npos)
          {
               AddVariant(term.substr(0, term.size() - 1) + "ies", 0.88);
          }
          else
          {
               AddVariant(term + "s", 0.86);
          }
     }
     else if (term.size() > 4 && (term.substr(term.size() - 2) == "es"))
     {
          const std::string Stem = term.substr(0, term.size() - 2);
          if (!Stem.empty() && (Stem.back() == 's' || Stem.back() == 'x' || Stem.back() == 'z' ||
                                Stem.back() == 'h'))
          {
               AddVariant(Stem, 0.86);
          }
     }
     else if (Last == 's' && term.size() > 4 && term.substr(term.size() - 2) != "ss")
     {
          AddVariant(term.substr(0, term.size() - 1), 0.88);
     }
     else
     {
          AddVariant(term + "s", 0.84);
          AddVariant(term + "es", 0.80);
     }
}

/*
 * InvertedIndex::SearchTerm - Resolves a normalized term from mmap and in-memory indexes.
 */

std::vector<Posting> InvertedIndex::SearchTerm(const std::string &Collection, const std::string &Term)
{
     std::string NormalizedTerm = NormalizeTerm(Term);
     std::unordered_map<std::string, Posting> TermDocs;

     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

     /* Mapped results are loaded first and then merged with newer in-memory mutations. */

     auto MMapIt = MMapIndexes.find(Collection);

     if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid() && MMapIt->second->GetTermCount() > 0)
     {
          auto Results = MMapIt->second->SearchTerm(NormalizedTerm);

          for (auto &Post : Results)
          {
               Post.Collection = Collection;
               TermDocs[Post.DocumentID] = Post;
          }
     }

     auto CollectionIt = Index.find(Collection);

     if (CollectionIt != Index.end() && !CollectionIt->second.empty())
     {
          /* In-memory postings win by addition when the same document exists in both sources. */

          auto TermIt = CollectionIt->second.find(NormalizedTerm);

          if (TermIt != CollectionIt->second.end())
          {
               for (const auto &Post : TermIt->second)
               {
                    auto ExistingIt = TermDocs.find(Post.DocumentID);

                    if (ExistingIt == TermDocs.end())
                    {
                         TermDocs[Post.DocumentID] = Post;
                    }
                    else
                    {
                         ExistingIt->second.Score += Post.Score;
                    }
               }
          }
     }

     std::vector<Posting> Results;
     Results.reserve(TermDocs.size());

     for (auto &Pair : TermDocs)
     {
          Results.push_back(Pair.second);
     }

     return Results;
}

/*
 * InvertedIndex::Search - Parses the query, scores matches, and merges in-memory and mmap-backed results.
 */

std::vector<Posting> InvertedIndex::Search(const std::string &Collection, const std::string &Query, int Limit, const std::vector<std::string> &QueryFields)
{
     if (Limit < 0)
     {
          Limit = 0;
     }

     if (Limit > 10000)
     {
          Limit = 10000;
     }

     /* Query limits are clamped before any candidate allocation. */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "Search: Searching for '" + Query + "' in collection '" + Collection + "' with limit " + std::to_string(Limit) + ".");
     }

     std::vector<std::string> QueryTerms = ExtractTerms(Query);
     std::vector<std::string> NegativeTerms;

     /* Negative terms support both NOT token syntax and leading exclamation syntax. */

     {
          std::istringstream QueryStream(Query);
          std::string TokenValue;
          bool NextTokenIsNegative = false;

          while (QueryStream >> TokenValue)
          {
               std::string LowerToken = TokenValue;
               std::transform(LowerToken.begin(), LowerToken.end(), LowerToken.begin(), ToLowerAsciiSafe);

               if (LowerToken == "not")
               {
                    NextTokenIsNegative = true;
                    continue;
               }

               bool IsNegativeToken = NextTokenIsNegative;
               NextTokenIsNegative = false;

               if (!TokenValue.empty() && TokenValue.front() == '!')
               {
                    IsNegativeToken = true;
                    TokenValue.erase(0, 1);
               }

               if (IsNegativeToken)
               {
                    std::string NormalizedNegative = NormalizeTerm(TokenValue);

                    if (!NormalizedNegative.empty())
                    {
                         NegativeTerms.push_back(NormalizedNegative);
                    }
               }
          }
     }

     if (!NegativeTerms.empty())
     {
          /* Remove negated terms from the positive query stream before scoring. */

          std::unordered_set<std::string> NegativeTermSet(NegativeTerms.begin(), NegativeTerms.end());

          QueryTerms.erase(std::remove_if(QueryTerms.begin(), QueryTerms.end(), [&NegativeTermSet](const std::string &TermValue)
                                          {
                                               return TermValue == "not" || NegativeTermSet.find(TermValue) != NegativeTermSet.end();
                                          }),
                           QueryTerms.end());
     }

     {
          /* Deduplicate positive terms while preserving the user's original term order. */

          std::unordered_set<std::string> SeenQueryTerms;
          std::vector<std::string> UniqueQueryTerms;
          UniqueQueryTerms.reserve(QueryTerms.size());

          for (const auto &TermValue : QueryTerms)
          {
               if (TermValue.empty() || !SeenQueryTerms.insert(TermValue).second)
               {
                    continue;
               }

               UniqueQueryTerms.push_back(TermValue);
          }

          QueryTerms = std::move(UniqueQueryTerms);
     }

     if (!NegativeTerms.empty())
     {
          /* Deduplicate exclusions so each negative term is resolved only once. */

          std::unordered_set<std::string> SeenNegativeTerms;
          std::vector<std::string> UniqueNegativeTerms;
          UniqueNegativeTerms.reserve(NegativeTerms.size());

          for (const auto &TermValue : NegativeTerms)
          {
               if (TermValue.empty() || !SeenNegativeTerms.insert(TermValue).second)
               {
                    continue;
               }

               UniqueNegativeTerms.push_back(TermValue);
          }

          NegativeTerms = std::move(UniqueNegativeTerms);
     }

     if (Instance && Instance->Config && Instance->Config->GetQuerySettingsMaxQueryTerms() > 0)
     {
          /* Query-term capping prevents unusually long input from expanding into excessive lookups. */

          const size_t MaxQueryTerms = static_cast<size_t>(Instance->Config->GetQuerySettingsMaxQueryTerms());
          if (QueryTerms.size() > MaxQueryTerms)
          {
               QueryTerms.resize(MaxQueryTerms);
          }
     }

     if (QueryTerms.empty() && NegativeTerms.empty())
     {
          return {};
     }

     bool UseMMap = false;

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          TouchCollectionLocked(Collection);

          /* The mmap flag is captured early for diagnostics; the actual merge checks run under lock later. */

          auto MMapIt = MMapIndexes.find(Collection);

          if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
          {
               if (MMapIt->second->GetTermCount() > 0)
               {
                    UseMMap = true;
               }
          }
     }

     std::vector<std::unordered_map<std::string, Posting>> TermResults;
     std::unordered_set<std::string> NegativeDocIDs;
     TermResults.reserve(QueryTerms.empty() ? 1 : QueryTerms.size());
     NegativeDocIDs.reserve(NegativeTerms.size() * 16);
     size_t ValidQueryTermCount = 0;

     auto IsWildcardTerm = [](const std::string &term) -> bool
     {
          return term.find('*') != std::string::npos || term.find('?') != std::string::npos;
     };

     /* Prefix wildcards can use a cheaper prefix lookup, while complex patterns use wildcard matching. */

     auto IsPrefixWildcardTerm = [](const std::string &term) -> bool
     {
          size_t StarPos = term.find('*');

          if (StarPos == std::string::npos || StarPos != term.size() - 1)
          {
               return false;
          }

          return term.find('?') == std::string::npos && term.find('*', StarPos + 1) == std::string::npos;
     };

     auto BuildScopedKeys = [&QueryFields](const std::string &normalized_term) -> std::vector<std::string>
     {
          if (QueryFields.empty())
          {
               return {normalized_term};
          }

          /* Field-restricted queries translate each normalized term into synthetic scoped keys. */

          std::vector<std::string> ScopedKeys;
          ScopedKeys.reserve(QueryFields.size());

          for (const auto &FieldName : QueryFields)
          {
               if (!FieldName.empty())
               {
                    ScopedKeys.push_back(BuildFieldScopedTerm(FieldName, normalized_term));
               }
          }

          return ScopedKeys;
     };

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          TouchCollectionLocked(Collection);
          auto CollectionIt = Index.find(Collection);
          auto MMapIt = MMapIndexes.find(Collection);
          const bool HasMMapIndex = MMapIt != MMapIndexes.end() &&
                                    MMapIt->second &&
                                    MMapIt->second->IsValid() &&
                                    MMapIt->second->GetTermCount() > 0;
          const bool HasMemoryIndex = CollectionIt != Index.end() && !CollectionIt->second.empty();

          auto LoadTermDocs = [&](const std::string &Normalized, std::unordered_map<std::string, Posting> &TermDocs)
          {
               /* Singular and plural variants are merged into one logical query term result. */

               std::vector<std::pair<std::string, double>> TermVariants;
               AddQueryTermVariants(Normalized, TermVariants);

               if (HasMMapIndex)
               {
                    /* Mapped lookups handle exact, prefix, and general wildcard patterns through index APIs. */

                    for (const auto &[VariantTerm, VariantWeight] : TermVariants)
                    {
                         const std::vector<std::string> ScopedKeys = BuildScopedKeys(VariantTerm);

                         for (const auto &ScopedKey : ScopedKeys)
                         {
                              std::vector<Posting> ScopedPostings;

                              if (IsWildcardTerm(VariantTerm))
                              {
                                   if (IsPrefixWildcardTerm(VariantTerm) && ScopedKey.size() > 1)
                                   {
                                        std::string Prefix = ScopedKey.substr(0, ScopedKey.size() - 1);
                                        ScopedPostings = MMapIt->second->SearchPrefix(Prefix, 0);
                                   }
                                   else
                                   {
                                        ScopedPostings = MMapIt->second->SearchWildcard(ScopedKey, 0);
                                   }
                              }
                              else
                              {
                                   ScopedPostings = MMapIt->second->SearchTerm(ScopedKey);
                              }

                              for (auto &ScopedPost : ScopedPostings)
                              {
                                   ScopedPost.Score *= VariantWeight;

                                   auto It = TermDocs.find(ScopedPost.DocumentID);
                                   if (It == TermDocs.end())
                                   {
                                        TermDocs[ScopedPost.DocumentID] = ScopedPost;
                                        TermDocs[ScopedPost.DocumentID].Collection = Collection;
                                   }
                                   else
                                   {
                                        It->second.Score += ScopedPost.Score;
                                   }
                              }
                         }
                    }
               }

               if (HasMemoryIndex)
               {
                    /* In-memory lookups scan wildcard patterns directly because the map is already resident. */

                    for (const auto &[VariantTerm, VariantWeight] : TermVariants)
                    {
                         const std::vector<std::string> ScopedKeys = BuildScopedKeys(VariantTerm);

                         if (IsWildcardTerm(VariantTerm))
                         {
                              for (const auto &ScopedKey : ScopedKeys)
                              {
                                   for (const auto &[IndexedTerm, MemoryPostings] : CollectionIt->second)
                                   {
                                        if (!Wildcard::Match(IndexedTerm, ScopedKey))
                                        {
                                             continue;
                                        }

                                        for (const auto &Post : MemoryPostings)
                                        {
                                             Posting WeightedPost = Post;
                                             WeightedPost.Score *= VariantWeight;

                                             auto It = TermDocs.find(WeightedPost.DocumentID);
                                             if (It == TermDocs.end())
                                             {
                                                  TermDocs[WeightedPost.DocumentID] = WeightedPost;
                                             }
                                             else
                                             {
                                                  It->second.Score += WeightedPost.Score;
                                             }
                                        }
                                   }
                              }
                         }
                         else
                         {
                              for (const auto &ScopedKey : ScopedKeys)
                              {
                                   auto TermIt = CollectionIt->second.find(ScopedKey);

                                   if (TermIt == CollectionIt->second.end())
                                   {
                                        continue;
                                   }

                                   for (const auto &Post : TermIt->second)
                                   {
                                        Posting WeightedPost = Post;
                                        WeightedPost.Score *= VariantWeight;

                                        auto It = TermDocs.find(WeightedPost.DocumentID);
                                        if (It == TermDocs.end())
                                        {
                                             TermDocs[WeightedPost.DocumentID] = WeightedPost;
                                        }
                                        else
                                        {
                                             It->second.Score += WeightedPost.Score;
                                        }
                                   }
                              }
                         }
                    }
               }
          };

          for (const auto &TermValue : NegativeTerms)
          {
               /* Negative result sets are converted into document IDs and removed after positive merging. */

               std::string Normalized = NormalizeTerm(TermValue);

               if (Normalized.empty())
               {
                    continue;
               }

               std::unordered_map<std::string, Posting> TermDocs;
               LoadTermDocs(Normalized, TermDocs);

               for (const auto &Pair : TermDocs)
               {
                    NegativeDocIDs.insert(Pair.first);
               }
          }

          for (const auto &TermValue : QueryTerms)
          {
               /* Each positive term produces one document map that is later combined according to match mode. */

               std::string Normalized = NormalizeTerm(TermValue);

               if (Normalized.empty())
               {
                    continue;
               }

               ValidQueryTermCount++;

               std::unordered_map<std::string, Posting> TermDocs;

               LoadTermDocs(Normalized, TermDocs);

               if (TermDocs.empty())
               {
                    if (Instance && Instance->Config && Instance->Config->GetSearchMatchMode() == "and")
                    {
                         if (Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("inverted_index", "Search: Term '" + Normalized + "' not found in index, returning empty results (AND logic).");
                         }

                         return {};
                    }

                    continue;
               }

               TermResults.push_back(TermDocs);
          }

          if (TermResults.empty() && !NegativeTerms.empty())
          {
               /* A query with only exclusions starts from all known documents and subtracts negative matches. */

               std::unordered_map<std::string, Posting> AllDocs;

               auto DocsIt = DocumentTerms.find(Collection);

               if (DocsIt != DocumentTerms.end())
               {
                    for (const auto &DocPair : DocsIt->second)
                    {
                         Posting Post;
                         Post.DocumentID = DocPair.first;
                         Post.Collection = Collection;
                         Post.Score = 1.0;
                         AllDocs[Post.DocumentID] = std::move(Post);
                    }
               }

               if (HasMemoryIndex)
               {
                    for (const auto &[IndexedTerm, MemoryPostings] : CollectionIt->second)
                    {
                         (void)IndexedTerm;

                         for (const auto &PostValue : MemoryPostings)
                         {
                              if (PostValue.DocumentID.empty())
                              {
                                   continue;
                              }

                              Posting Post = PostValue;
                              Post.Collection = Collection;
                              Post.Score = std::max(Post.Score, 1.0);
                              AllDocs.emplace(Post.DocumentID, std::move(Post));
                         }
                    }
               }

               if (!AllDocs.empty())
               {
                    TermResults.push_back(std::move(AllDocs));
               }
          }
     }

     if (TermResults.empty())
     {
          return {};
     }

     std::unordered_map<std::string, Posting> DocScores;
     std::unordered_map<std::string, size_t> DocMatchCounts;

     /* Ranking defaults are local fallbacks and are normalized after configuration is loaded. */

     double K1 = 1.2;

     double B = 0.75;

     double Delta = 1.0;

     std::string SearchAlgorithm = "bm25+";

     double IdfSmooth = 1.0;

     bool NormalizeTFIDF = true;

     double BM25Weight = 0.7;

     double TFIDFWeight = 0.3;

     bool PivotEnabled = false;

     double PivotValue = 0.25;

     std::string IdfMode = "legacy";

     bool IdfClampNegative = true;

     double IdfFloorFactor = 0.05;

     std::string MatchMode = "and";

     int MinShouldMatch = 1;

     int CandidatePruneMultiplier = 25;

     if (Instance && Instance->Config)
     {
          K1 = Instance->Config->GetRankingK1();

          B = Instance->Config->GetRankingB();

          Delta = Instance->Config->GetRankingDelta();

          SearchAlgorithm = Instance->Config->GetSearchAlgorithm();

          IdfSmooth = Instance->Config->GetRankingIdfSmooth();

          NormalizeTFIDF = Instance->Config->GetRankingNormalize();

          BM25Weight = Instance->Config->GetRankingBm25Weight();

          TFIDFWeight = Instance->Config->GetRankingTfidfWeight();

          PivotEnabled = Instance->Config->GetPivotNormEnabled();

          PivotValue = Instance->Config->GetPivotNormPivot();

          IdfMode = Instance->Config->GetRankingIdfMode();

          IdfClampNegative = Instance->Config->GetRankingIdfClampNegative();

          IdfFloorFactor = Instance->Config->GetRankingIdfFloorFactor();

          MatchMode = Instance->Config->GetSearchMatchMode();

          MinShouldMatch = Instance->Config->GetSearchMinShouldMatch();

          CandidatePruneMultiplier = Instance->Config->GetSearchCandidatePruneMultiplier();
     }

     if (!std::isfinite(K1) || K1 < 0.0)
     {
          /* Invalid numeric settings are replaced with stable defaults before scoring begins. */

          K1 = 1.2;
     }

     if (!std::isfinite(B))
     {
          B = 0.75;
     }
     B = std::clamp(B, 0.0, 1.0);

     if (!std::isfinite(Delta) || Delta < 0.0)
     {
          Delta = 1.0;
     }

     if (!std::isfinite(IdfSmooth) || IdfSmooth < 0.0)
     {
          IdfSmooth = 1.0;
     }

     if (!std::isfinite(BM25Weight) || BM25Weight < 0.0)
     {
          BM25Weight = 0.7;
     }

     if (!std::isfinite(TFIDFWeight) || TFIDFWeight < 0.0)
     {
          TFIDFWeight = 0.3;
     }

     if (!std::isfinite(PivotValue))
     {
          PivotValue = 0.25;
     }
     PivotValue = std::clamp(PivotValue, 0.0, 1.0);

     std::transform(IdfMode.begin(), IdfMode.end(), IdfMode.begin(), ToLowerAsciiSafe);
     if (IdfMode != "smooth")
     {
          IdfMode = "legacy";
     }

     if (!std::isfinite(IdfFloorFactor))
     {
          IdfFloorFactor = 0.05;
     }
     IdfFloorFactor = std::clamp(IdfFloorFactor, 0.0, 1.0);

     std::transform(SearchAlgorithm.begin(), SearchAlgorithm.end(), SearchAlgorithm.begin(), ToLowerAsciiSafe);
     if (SearchAlgorithm != "bm25" && SearchAlgorithm != "bm25+" && SearchAlgorithm != "tfidf" && SearchAlgorithm != "hybrid")
     {
          /* Unknown algorithms fall back to BM25+ to preserve historical behavior. */

          SearchAlgorithm = "bm25+";
     }

     std::transform(MatchMode.begin(), MatchMode.end(), MatchMode.begin(), ToLowerAsciiSafe);
     if (MatchMode != "and" && MatchMode != "or" && MatchMode != "min_should_match")
     {
          MatchMode = "and";
     }

     if (MinShouldMatch < 1)
     {
          MinShouldMatch = 1;
     }

     if (CandidatePruneMultiplier < 0)
     {
          CandidatePruneMultiplier = 0;
     }

     size_t RequiredMatches = TermResults.size();
     if (MatchMode == "or")
     {
          /* OR mode admits any candidate that matched at least one resolved query term. */

          RequiredMatches = 1;
     }
     else if (MatchMode == "min_should_match")
     {
          /* Min-should-match is capped at the number of resolved positive terms. */

          RequiredMatches = static_cast<size_t>(std::max(1, MinShouldMatch));
          RequiredMatches = std::min(RequiredMatches, TermResults.size());
     }

     double AvgDocLengthValue = 1.0;

     size_t CollectionSizeValue = 0;

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);

          /* Collection statistics are read under lock and then used outside the critical section. */

          auto AvgIt = AvgDocLengths.find(Collection);

          if (AvgIt != AvgDocLengths.end())
          {
               AvgDocLengthValue = AvgIt->second;
          }

          auto CountIt = DocCounts.find(Collection);

          if (CountIt != DocCounts.end())
          {
               CollectionSizeValue = CountIt->second;
          }
     }

     std::vector<size_t> TermOrder(TermResults.size());

     std::iota(TermOrder.begin(), TermOrder.end(), 0);

     /* Intersecting the smallest posting lists first keeps AND queries cheaper. */

     std::sort(TermOrder.begin(), TermOrder.end(), [&TermResults](size_t IndexA, size_t IndexB)
               {
                    if (TermResults[IndexA].size() != TermResults[IndexB].size())
                    {
                         return TermResults[IndexA].size() < TermResults[IndexB].size();
                    }

                    return IndexA < IndexB;
               });

     size_t ExpectedCandidateCount = 0;
     if (MatchMode == "and")
     {
          /* In AND mode the smallest term set is an upper bound for candidates. */

          ExpectedCandidateCount = TermResults[TermOrder[0]].size();
     }
     else
     {
          /* OR-like modes may see every posting before duplicate document IDs are merged. */

          for (const auto &TermDocs : TermResults)
          {
               if (ExpectedCandidateCount > std::numeric_limits<size_t>::max() - TermDocs.size())
               {
                    ExpectedCandidateCount = std::numeric_limits<size_t>::max();
                    break;
               }

               ExpectedCandidateCount += TermDocs.size();
          }
     }

     DocScores.reserve(ExpectedCandidateCount);
     DocMatchCounts.reserve(ExpectedCandidateCount);

     if (MatchMode == "and")
     {
          /* Seed the intersection with the rarest term and narrow candidates term by term. */

          for (const auto &Pair : TermResults[TermOrder[0]])
          {
               DocScores[Pair.first] = Pair.second;
               DocMatchCounts[Pair.first] = 1;
          }

          for (size_t Idx = 1; Idx < TermOrder.size(); ++Idx)
          {
               size_t I = TermOrder[Idx];

               const auto &CurrentTermDocs = TermResults[I];

               if (CurrentTermDocs.size() > DocScores.size() * 10 && DocScores.size() > 100)
               {
                    /* For highly uneven sets, sorting both sides avoids many hash lookups. */

                    std::vector<std::pair<std::string, Posting>> SortedDocs(DocScores.begin(), DocScores.end());

                    std::sort(SortedDocs.begin(), SortedDocs.end(), [](const auto &DocA, const auto &DocB)
                              {
                                   return DocA.first < DocB.first;
                              });

                    std::vector<std::pair<std::string, Posting>> SortedCurrent(CurrentTermDocs.begin(), CurrentTermDocs.end());

                    std::sort(SortedCurrent.begin(), SortedCurrent.end(), [](const auto &DocA, const auto &DocB)
                              {
                                   return DocA.first < DocB.first;
                              });

                    std::unordered_map<std::string, Posting> Intersection;
                    std::unordered_map<std::string, size_t> IntersectionCounts;
                    const size_t ReserveSize = std::min(DocScores.size(), CurrentTermDocs.size());
                    Intersection.reserve(ReserveSize);
                    IntersectionCounts.reserve(ReserveSize);

                    size_t Pos1 = 0;

                    size_t Pos2 = 0;

                    while (Pos1 < SortedDocs.size() && Pos2 < SortedCurrent.size())
                    {
                         if (SortedDocs[Pos1].first < SortedCurrent[Pos2].first)
                         {
                              Pos1++;
                         }
                         else if (SortedCurrent[Pos2].first < SortedDocs[Pos1].first)
                         {
                              Pos2++;
                         }
                         else
                         {
                              Intersection[SortedDocs[Pos1].first] = SortedDocs[Pos1].second;

                              for (const auto &PosVal : SortedCurrent[Pos2].second.Positions)
                              {
                                   Intersection[SortedDocs[Pos1].first].Positions.push_back(PosVal);
                              }

                              Intersection[SortedDocs[Pos1].first].Score += SortedCurrent[Pos2].second.Score;
                              IntersectionCounts[SortedDocs[Pos1].first] = DocMatchCounts[SortedDocs[Pos1].first] + 1;

                              Pos1++;

                              Pos2++;
                         }
                    }

                    DocScores = std::move(Intersection);
                    DocMatchCounts = std::move(IntersectionCounts);
               }
               else
               {
                    /* For similarly sized sets, hash lookup is cheaper than building sorted vectors. */

                    std::unordered_map<std::string, Posting> Intersection;
                    std::unordered_map<std::string, size_t> IntersectionCounts;
                    const size_t ReserveSize = std::min(DocScores.size(), CurrentTermDocs.size());
                    Intersection.reserve(ReserveSize);
                    IntersectionCounts.reserve(ReserveSize);

                    for (const auto &Pair : CurrentTermDocs)
                    {
                         auto It = DocScores.find(Pair.first);

                         if (It != DocScores.end())
                         {
                              Intersection[Pair.first] = It->second;

                              for (const auto &PosVal : Pair.second.Positions)
                              {
                                   Intersection[Pair.first].Positions.push_back(PosVal);
                              }

                              Intersection[Pair.first].Score += Pair.second.Score;
                              IntersectionCounts[Pair.first] = DocMatchCounts[Pair.first] + 1;
                         }
                    }

                    DocScores = std::move(Intersection);
                    DocMatchCounts = std::move(IntersectionCounts);
               }
          }
     }
     else
     {
          /* OR and min-should-match modes accumulate scores from every matching term. */

          for (const auto &TermDocs : TermResults)
          {
               for (const auto &Pair : TermDocs)
               {
                    auto It = DocScores.find(Pair.first);

                    if (It == DocScores.end())
                    {
                         DocScores[Pair.first] = Pair.second;
                    }
                    else
                    {
                         It->second.Score += Pair.second.Score;

                         for (const auto &PosVal : Pair.second.Positions)
                         {
                              It->second.Positions.push_back(PosVal);
                         }
                    }

                    DocMatchCounts[Pair.first]++;
               }
          }

          if (RequiredMatches > 1)
          {
               /* Min-should-match pruning runs after all term maps have contributed match counts. */

               for (auto It = DocScores.begin(); It != DocScores.end();)
               {
                    if (DocMatchCounts[It->first] < RequiredMatches)
                    {
                         DocMatchCounts.erase(It->first);
                         It = DocScores.erase(It);
                    }
                    else
                    {
                         ++It;
                    }
               }
          }
     }

     if (!NegativeDocIDs.empty())
     {
          /* Exclusions are applied after candidate merging so they work with every match mode. */

          for (const auto &DocID : NegativeDocIDs)
          {
               DocScores.erase(DocID);
               DocMatchCounts.erase(DocID);
          }
     }

     if (DocScores.empty())
     {
          return {};
     }

     CollectionSizeValue = std::max(CollectionSizeValue, DocScores.size());

     std::vector<Posting> Results;

     Results.reserve(DocScores.size());

     struct CandidateDocument
     {
          std::string DocumentID;
          Posting *PostingValue;
          double DocumentLength;
     };

     /* CandidateDocument stores pointers into DocScores to avoid copying postings before final scoring. */

     std::vector<CandidateDocument> CandidateDocs;
     CandidateDocs.reserve(DocScores.size());

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          const auto CollectionLengthsIt = DocumentLengths.find(Collection);

          if (CollectionLengthsIt != DocumentLengths.end() && !CollectionLengthsIt->second.empty())
          {
               /* Length data may be fresher than cached stats after incremental indexing. */

               CollectionSizeValue = std::max(CollectionSizeValue, CollectionLengthsIt->second.size());

               if (!std::isfinite(AvgDocLengthValue) || AvgDocLengthValue <= 0.0)
               {
                    size_t TotalLength = 0;
                    for (const auto &[LengthDocID, LengthValue] : CollectionLengthsIt->second)
                    {
                         (void)LengthDocID;
                         TotalLength += LengthValue;
                    }

                    if (TotalLength > 0)
                    {
                         AvgDocLengthValue = static_cast<double>(TotalLength) / static_cast<double>(CollectionLengthsIt->second.size());
                    }
               }
          }

          if (!std::isfinite(AvgDocLengthValue) || AvgDocLengthValue <= 0.0)
          {
               AvgDocLengthValue = 1.0;
          }

          for (auto &[DocID, Post] : DocScores)
          {
               /* Missing length rows use a neutral length so legacy postings remain searchable. */

               double DocLengthValue = 1.0;
               if (CollectionLengthsIt != DocumentLengths.end())
               {
                    const auto DocLengthIt = CollectionLengthsIt->second.find(DocID);
                    if (DocLengthIt != CollectionLengthsIt->second.end())
                    {
                         DocLengthValue = static_cast<double>(DocLengthIt->second);
                    }
               }

               CandidateDocs.push_back({DocID, &Post, DocLengthValue});
          }
     }

     const double CollectionSizeDouble = static_cast<double>(CollectionSizeValue);
     const double EffectiveB = PivotEnabled ? 0.0 : B;
     const bool EffectiveTFIDFNormalization = NormalizeTFIDF && !PivotEnabled;
     const double HybridWeightTotal = BM25Weight + TFIDFWeight;

     /* Pivot normalization replaces document-length normalization in the per-algorithm formula. */

     struct TermScoringState
     {
          double BM25Idf = 0.0;
          double TFIDFIdf = 0.0;
     };

     auto ComputeBM25Idf = [&](double DocFreq) -> double
     {
          /* BM25 IDF is computed once per resolved query term and reused across candidates. */

          return BM25Scoring::CalculateIdf(DocFreq, CollectionSizeDouble, IdfMode,
                                            IdfSmooth, IdfClampNegative,
                                            IdfFloorFactor);
     };

     auto ComputeTFIDFIdf = [&](double DocFreq) -> double
     {
          /* TF-IDF uses a positive smoothed IDF so frequent terms do not produce negative scores. */

          if (!std::isfinite(DocFreq) || DocFreq <= 0.0 || CollectionSizeDouble <= 0.0 || DocFreq > CollectionSizeDouble)
          {
               return 0.0;
          }

          const double IdfDenominator = DocFreq + IdfSmooth;
          const double IdfNumerator = CollectionSizeDouble + IdfSmooth;
          if (IdfDenominator <= 0.0 || IdfNumerator <= 0.0)
          {
               return 0.0;
          }

          const double Idf = 1.0 + std::log(IdfNumerator / IdfDenominator);
          return std::isfinite(Idf) && Idf > 0.0 ? Idf : 0.0;
     };

     std::vector<TermScoringState> TermScoringStates;
     TermScoringStates.reserve(TermResults.size());
     for (const auto &TermDocs : TermResults)
     {
          /* Store both IDF variants because hybrid scoring may need both for the same term. */

          const double DocFreq = static_cast<double>(TermDocs.size());
          TermScoringStates.push_back({ComputeBM25Idf(DocFreq),
                                       ComputeTFIDFIdf(DocFreq)});
     }

     if (Limit > 0 && CandidatePruneMultiplier > 0)
     {
          /* Prune by pre-ranking boost score before running full ranking on very large candidate sets. */

          const size_t SafeLimitValue = static_cast<size_t>(Limit);
          const size_t SafeMultiplierValue = static_cast<size_t>(CandidatePruneMultiplier);
          const size_t ProductLimitValue = SafeMultiplierValue > 0 && SafeLimitValue > (std::numeric_limits<size_t>::max() / SafeMultiplierValue)
                                                ? std::numeric_limits<size_t>::max()
                                                : SafeLimitValue * SafeMultiplierValue;
          const size_t CandidateLimitValue = std::max(SafeLimitValue, ProductLimitValue);

          if (CandidateDocs.size() > CandidateLimitValue)
          {
               std::nth_element(CandidateDocs.begin(),
                                CandidateDocs.begin() + static_cast<std::ptrdiff_t>(CandidateLimitValue),
                                CandidateDocs.end(),
                                [](const CandidateDocument &DocA, const CandidateDocument &DocB)
                                {
                                     if (DocA.PostingValue->Score != DocB.PostingValue->Score)
                                     {
                                          return DocA.PostingValue->Score > DocB.PostingValue->Score;
                                     }

                                     return DocA.DocumentID < DocB.DocumentID;
                                });

               CandidateDocs.resize(CandidateLimitValue);
          }
     }

     for (const auto &[DocID, PostPtr, DocLengthValue] : CandidateDocs)
     {
          /* Final score combines ranking math with boost weights stored in the matching postings. */

          Posting &Post = *PostPtr;
          double MatchedTermScore = Post.Score;

          double TotalScore = 0.0;
          std::vector<Posting> MatchedPostingsForProximity;
          if (QueryTerms.size() >= 2)
          {
               MatchedPostingsForProximity.reserve(std::min(QueryTerms.size(), TermResults.size()));
          }

          for (size_t TermIdx = 0; TermIdx < TermResults.size(); ++TermIdx)
          {
               /* Only terms actually present in this document contribute to its final score. */

               const auto &TermDocs = TermResults[TermIdx];
               auto TermDocIt = TermDocs.find(DocID);
               if (TermDocIt == TermDocs.end())
               {
                    continue;
               }

               if (QueryTerms.size() >= 2 && !TermDocIt->second.Positions.empty())
               {
                    MatchedPostingsForProximity.push_back(TermDocIt->second);
               }

               double TermFreq = static_cast<double>(TermDocIt->second.Positions.empty() ? 1 : TermDocIt->second.Positions.size());
               const TermScoringState &TermState = TermScoringStates[TermIdx];
               const double SafeDocLengthValue = std::max(1e-9, DocLengthValue);

               double TermScoreValue = 0.0;

               if (SearchAlgorithm == "bm25")
               {
                    /* BM25 uses saturating term frequency with document-length normalization. */

                    const double NormalizedLengthValue = std::max(1e-9, SafeDocLengthValue / AvgDocLengthValue);
                    const double DenominatorValue = TermFreq + K1 * (1.0 - EffectiveB + EffectiveB * NormalizedLengthValue);
                    if (DenominatorValue > 0.0)
                    {
                         TermScoreValue = TermState.BM25Idf * ((TermFreq * (K1 + 1.0)) / DenominatorValue);
                    }
               }
               else if (SearchAlgorithm == "tfidf")
               {
                    /* TF-IDF uses sublinear term frequency and optional document-length normalization. */

                    TermScoreValue = 1.0 + std::log(TermFreq);
                    if (EffectiveTFIDFNormalization)
                    {
                         TermScoreValue /= std::sqrt(SafeDocLengthValue);
                    }
                    TermScoreValue *= TermState.TFIDFIdf;
               }
               else if (SearchAlgorithm == "hybrid")
               {
                    /* Hybrid scoring blends BM25 and TF-IDF after each component is computed. */

                    if (HybridWeightTotal > 0.0)
                    {
                         const double NormalizedLengthValue = std::max(1e-9, SafeDocLengthValue / AvgDocLengthValue);
                         const double DenominatorValue = TermFreq + K1 * (1.0 - EffectiveB + EffectiveB * NormalizedLengthValue);
                         double BM25Score = 0.0;
                         if (DenominatorValue > 0.0)
                         {
                              BM25Score = TermState.BM25Idf * ((TermFreq * (K1 + 1.0)) / DenominatorValue);
                         }

                         double TFIDFScore = 1.0 + std::log(TermFreq);
                         if (EffectiveTFIDFNormalization)
                         {
                              TFIDFScore /= std::sqrt(SafeDocLengthValue);
                         }
                         TFIDFScore *= TermState.TFIDFIdf;

                         TermScoreValue = ((BM25Weight * BM25Score) + (TFIDFWeight * TFIDFScore)) / HybridWeightTotal;
                    }
               }
               else
               {
                    /* BM25+ adds Delta so very long documents are not over-penalized for matching terms. */

                    const double NormalizedLengthValue = std::max(1e-9, SafeDocLengthValue / AvgDocLengthValue);
                    const double DenominatorValue = TermFreq + K1 * (1.0 - EffectiveB + EffectiveB * NormalizedLengthValue);
                    if (DenominatorValue > 0.0)
                    {
                         TermScoreValue = TermState.BM25Idf * (((TermFreq * (K1 + 1.0)) / DenominatorValue) + Delta);
                    }
               }

               if (PivotEnabled && AvgDocLengthValue > 0.0)
               {
                    /* Pivot normalization is applied after the base score to keep algorithm selection simple. */

                    const double PivotNorm = (1.0 - PivotValue) + PivotValue * (DocLengthValue / AvgDocLengthValue);
                    if (std::isfinite(PivotNorm) && PivotNorm > 0.0)
                    {
                         TermScoreValue /= PivotNorm;
                    }
               }

               TotalScore += TermScoreValue * std::max(0.0, TermDocIt->second.Score);
          }

          if (QueryTerms.size() >= 2 && MatchedPostingsForProximity.size() >= 2 && DocMatchCounts[DocID] >= 2)
          {
               /* Proximity boosts documents where multiple query terms appear near each other. */

               double ProximityBoostValue = CalculateProximityBoost(QueryTerms, MatchedPostingsForProximity, DocID);

               TotalScore *= ProximityBoostValue;
          }

          if (ValidQueryTermCount > 1)
          {
               /* Coverage gives a small advantage to documents matching more of the positive query. */

               const double MatchedTerms = static_cast<double>(DocMatchCounts[DocID]);
               const double CoverageRatio = std::min(1.0, MatchedTerms / static_cast<double>(ValidQueryTermCount));
               TotalScore *= 0.75 + (0.25 * CoverageRatio);
          }

          Post.Score = TotalScore;

          if (!std::isfinite(TotalScore) || TotalScore <= 0.0)
          {
               /* Preserve a tiny positive score when ranking math collapses because boosts still found a match. */

               Post.Score = std::max(MatchedTermScore, 1e-9);
          }

          Results.push_back(Post);
     }

     if (Limit > 0 && Results.size() > static_cast<size_t>(Limit))
     {
          /* Use partial selection before final sorting to avoid sorting large result sets unnecessarily. */

          const std::size_t LimitSize = static_cast<std::size_t>(Limit);

          std::nth_element(Results.begin(),
                           Results.begin() + static_cast<std::ptrdiff_t>(LimitSize),
                           Results.end(),
                           [](const Posting &PostingA, const Posting &PostingB)
                           {
                                if (PostingA.Score != PostingB.Score)
                                {
                                     return PostingA.Score > PostingB.Score;
                                }

                                return PostingA.DocumentID < PostingB.DocumentID;
                           });

          Results.resize(LimitSize);
     }

     std::sort(Results.begin(), Results.end(), [](const Posting &PostingA, const Posting &PostingB)
               {
                    /* Stable ordering for equal scores keeps pagination and tests deterministic. */

                    if (PostingA.Score != PostingB.Score)
                    {
                         return PostingA.Score > PostingB.Score;
                    }

                    return PostingA.DocumentID < PostingB.DocumentID;
               });

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "Search: Found " + std::to_string(Results.size()) + " results for query '" + Query + "' (algorithm=" + SearchAlgorithm + ", index=" + (UseMMap ? "mmap" : "memory") + ").");
     }

     return Results;
}

/*
 * SearchPrefix - Searches for terms with a given prefix in the index.
 */

/* InvertedIndex::SearchPrefix - Searches prefix terms. */

std::vector<Posting> InvertedIndex::SearchPrefix(const std::string &Collection, const std::string &Prefix, int Limit)
{
     if (Limit < 0)
     {
          Limit = 0;
     }

     if (Limit > 10000)
     {
          Limit = 10000;
     }

     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "SearchPrefix: Searching for prefix '" + Prefix + "' in collection '" + Collection + "' with limit " + std::to_string(Limit) + ".");
     }

     std::string NormalizedPrefix = NormalizeTerm(Prefix);

     if (NormalizedPrefix.empty())
     {
          return {};
     }

     /* Prefix search currently scans the in-memory collection map. */

     auto CollectionIt = Index.find(Collection);

     if (CollectionIt == Index.end())
     {
          return {};
     }

     std::unordered_map<std::string, Posting> DocScores;
     const size_t ResultLimitValue = Limit > 0 ? static_cast<size_t>(Limit) : 0;
     DocScores.reserve(ResultLimitValue > 0 ? std::min<size_t>(ResultLimitValue * 8, 8192) : 1024);

     for (const auto &[TermValue, Postings] : CollectionIt->second)
     {
          if (TermValue.length() >= NormalizedPrefix.length() &&
              TermValue.compare(0, NormalizedPrefix.length(), NormalizedPrefix) == 0)
          {
               /* Matching terms are collapsed by document ID with a small additional prefix weight. */

               for (const auto &Post : Postings)
               {
                    auto It = DocScores.find(Post.DocumentID);

                    if (It == DocScores.end())
                    {
                         DocScores[Post.DocumentID] = Post;
                    }
                    else
                    {
                         It->second.Score += Post.Score * 0.9;
                    }
               }
          }
     }

     std::vector<Posting> Results;

     Results.reserve(DocScores.size());

     for (auto &Pair : DocScores)
     {
          Results.push_back(Pair.second);
     }

     std::sort(Results.begin(), Results.end(), [](const Posting &PostingA, const Posting &PostingB)
               {
                    if (PostingA.Score != PostingB.Score)
                    {
                         return PostingA.Score > PostingB.Score;
                    }
                    return PostingA.DocumentID < PostingB.DocumentID;
               });

     if (Limit > 0)
     {
          const std::size_t LimitSize = static_cast<std::size_t>(Limit);
          if (Results.size() > LimitSize)
          {
               Results.resize(LimitSize);
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "SearchPrefix: Found " + std::to_string(Results.size()) + " results for prefix '" + Prefix + "'.");
     }

     return Results;
}
