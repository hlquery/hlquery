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
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <regex>
#include <rocksdb/write_batch.h>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unordered_map>
#include <unordered_set>

#include "core/hlquery.h"
#include "search/sam.h"
#include "search/storageengine.h"
#include "utils/tools.h"
#include "vendor/json/json.hpp"

bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry);

std::string TrimCopy(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

std::string NormalizeTerm(const std::string& Value)
{
     std::string Normalized;
     Normalized.reserve(Value.size());
     bool LastWasSpace = false;

     for (unsigned char C : Value)
     {
          if (std::isalnum(C))
          {
               Normalized.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '_' || C == '/' || C == '.')
          {
               if (!Normalized.empty() && !LastWasSpace)
               {
                    Normalized.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     return TrimCopy(Normalized);
}

std::vector<std::string> TokenizeNormalized(const std::string& Value)
{
     std::vector<std::string> Tokens;
     std::istringstream Input(Value);
     std::string Token;

     while (Input >> Token)
     {
          Tokens.push_back(Token);
     }

     return Tokens;
}

std::string CollapsePossessiveTail(const std::string& Value)
{
     std::vector<std::string> Tokens = TokenizeNormalized(Value);

     if (Tokens.size() >= 2 && Tokens.back() == "s")
     {
          Tokens.pop_back();
     }

     std::string Result;

     for (size_t Index = 0; Index < Tokens.size(); ++Index)
     {
          if (Index > 0)
          {
               Result.push_back(' ');
          }

          Result += Tokens[Index];
     }

     return Result;
}

bool IsWeakSamToken(const std::string& Value)
{
     static const std::unordered_set<std::string> WeakTokens = {
          "article", "articles", "page", "pages", "document", "documents", "content",
          "collection", "collections", "id", "official", "profile", "artist profile",
          "biography", "discography", "benchmark", "fake", "recognized", "music",
          "catalog", "blends", "blend", "drove", "transformed", "this", "that",
          "these", "those", "their", "there", "here", "brief", "guide"};
     return WeakTokens.find(Value) != WeakTokens.end();
}

bool IsSamStopword(const std::string& Value)
{
     /* Keep this intentionally small and phrase-focused.
      * These are glue words we want to ignore in SAM core-token matching,
      * not a general-purpose IR stoplist. */
     static const std::unordered_set<std::string> Stopwords = {
          "a", "an", "the",
          "and", "or",
          "of", "to", "for", "in", "on", "at", "by", "with", "from",
          "into", "over", "after", "before", "about", "through", "during",
          "without", "under", "between", "against"};
     return Stopwords.find(Value) != Stopwords.end();
}

std::string SingularizeToken(const std::string& Token)
{
     if (Token.size() <= 3)
     {
          return Token;
     }

     if (Token.size() > 4 && Token.substr(Token.size() - 3) == "ies")
     {
          return Token.substr(0, Token.size() - 3) + "y";
     }

     if (Token.size() > 4 && Token.substr(Token.size() - 2) == "es")
     {
          const std::string Stem = Token.substr(0, Token.size() - 2);

          if (Stem.size() >= 2)
          {
               const std::string Tail = Stem.substr(Stem.size() - 2);

               if (Tail == "ch" || Tail == "sh" || Tail.back() == 's' ||
                   Tail.back() == 'x' || Tail.back() == 'z' || Tail.back() == 'o')
               {
                    return Stem;
               }
          }
     }

     if (Token.back() == 's' && Token[Token.size() - 2] != 's')
     {
          return Token.substr(0, Token.size() - 1);
     }

     return Token;
}

std::string JoinTokens(const std::vector<std::string>& Tokens)
{
     std::string Result;

     for (size_t Index = 0; Index < Tokens.size(); ++Index)
     {
          if (Index > 0)
          {
               Result.push_back(' ');
          }

          Result += Tokens[Index];
     }

     return Result;
}

struct SAMQueryTokenViews
{
     bool Quoted = false;
     std::string NormalizedQuery;
     std::string NormalizedPhrase;
     std::vector<std::string> FullTokens;
     std::vector<std::string> CoreTokens;
};

struct SAMTokenMatchResult
{
     bool Matched = false;
     bool UsedSynonym = false;
     size_t Distance = static_cast<size_t>(-1);
};

size_t EditDistance(const std::string& A, const std::string& B);
bool LooksLikeWeakLLMSuffix(const std::string& Value);

bool IsQuotedSAMQuery(const std::string& Value)
{
     const std::string Trimmed = TrimCopy(Value);
     return Trimmed.size() >= 2 &&
            ((Trimmed.front() == '"' && Trimmed.back() == '"') ||
             (Trimmed.front() == '\'' && Trimmed.back() == '\''));
}

std::string StripSAMQueryQuotes(const std::string& Value)
{
     const std::string Trimmed = TrimCopy(Value);

     if (!IsQuotedSAMQuery(Trimmed))
     {
          return Trimmed;
     }

     return Trimmed.substr(1, Trimmed.size() - 2);
}

std::vector<std::string> NormalizeSAMTokens(const std::string& Value, bool RemoveStopwords)
{
     std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Value));
     std::vector<std::string> Result;
     Result.reserve(Tokens.size());

     for (const auto& Token : Tokens)
     {
          if (RemoveStopwords && IsSamStopword(Token))
          {
               continue;
          }

          Result.push_back(SingularizeToken(Token));
     }

     return Result;
}

std::vector<std::string> TrimOuterSAMStopwords(std::vector<std::string> Tokens)
{
     while (!Tokens.empty() && IsSamStopword(Tokens.front()))
     {
          Tokens.erase(Tokens.begin());
     }

     while (!Tokens.empty() && IsSamStopword(Tokens.back()))
     {
          Tokens.pop_back();
     }

     return Tokens;
}

std::vector<std::string> GetSAMTokenAlternatives(const std::string& Token)
{
     std::vector<std::string> Alternatives;
     Alternatives.push_back(Token);

     if (!Instance || !Instance->Config || !Instance->Config->GetSam25EnableSynonymExpansion())
     {
          return Alternatives;
     }

     static const std::unordered_map<std::string, std::vector<std::string>> Equivalences = {
          {"movie", {"film"}},
          {"film", {"movie"}},
          {"car", {"automobile", "vehicle"}},
          {"automobile", {"car", "vehicle"}},
          {"vehicle", {"car", "automobile"}},
          {"tv", {"television"}},
          {"television", {"tv"}},
          {"phone", {"telephone"}},
          {"telephone", {"phone"}},
          {"photo", {"picture", "image"}},
          {"picture", {"photo", "image"}},
          {"image", {"photo", "picture"}},
          {"song", {"track"}},
          {"track", {"song"}},
          {"artist", {"performer"}},
          {"performer", {"artist"}}
     };

     auto It = Equivalences.find(Token);

     if (It == Equivalences.end())
     {
          return Alternatives;
     }

     const int MaxSynonyms = Instance->Config->GetSam25MaxSynonymsPerToken();
     int Added = 0;

     for (const auto& Candidate : It->second)
     {
          if (Added >= MaxSynonyms)
          {
               break;
          }

          Alternatives.push_back(Candidate);
          ++Added;
     }

     return Alternatives;
}

SAMTokenMatchResult MatchSAMQueryTokenToTermToken(const std::string& QueryToken,
                                                  const std::string& TermToken)
{
     SAMTokenMatchResult Result;
     const std::vector<std::string> Alternatives = GetSAMTokenAlternatives(QueryToken);

     for (size_t Index = 0; Index < Alternatives.size(); ++Index)
     {
          const std::string& Candidate = Alternatives[Index];

          if (Candidate == TermToken)
          {
               Result.Matched = true;
               Result.UsedSynonym = Index > 0;
               Result.Distance = 0;
               return Result;
          }

          if (Index == 0)
          {
               const size_t Distance = EditDistance(Candidate, TermToken);
               const size_t MaxDistance = Candidate.size() >= 8 ? 2 : 1;

               if (Distance <= MaxDistance)
               {
                    Result.Matched = true;
                    Result.UsedSynonym = false;
                    Result.Distance = Distance;
                    return Result;
               }
          }
     }

     return Result;
}

SAMQueryTokenViews NormalizeSAMQueryTokenViews(const std::string& Query)
{
     SAMQueryTokenViews Views;
     Views.Quoted = IsQuotedSAMQuery(Query);
     Views.NormalizedQuery = NormalizeTerm(Query);
     Views.NormalizedPhrase = NormalizeTerm(StripSAMQueryQuotes(Query));
     Views.FullTokens = NormalizeSAMTokens(StripSAMQueryQuotes(Query), false);
     Views.CoreTokens = NormalizeSAMTokens(StripSAMQueryQuotes(Query), true);
     return Views;
}

std::vector<std::string> BuildQueryVariants(const std::string& Query)
{
     std::vector<std::string> Variants;
     std::unordered_set<std::string> Seen;
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);
     const std::string& Normalized = QueryViews.NormalizedPhrase.empty() ? QueryViews.NormalizedQuery : QueryViews.NormalizedPhrase;

     auto AppendVariant = [&](const std::string& Value)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (Candidate.empty() || Seen.find(Candidate) != Seen.end())
          {
               return;
          }

          Seen.insert(Candidate);
          Variants.push_back(Candidate);
     };

     AppendVariant(Normalized);

     std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty())
     {
          return Variants;
     }

     std::vector<std::string> Filtered;
     Filtered.reserve(Tokens.size());

     for (const auto& Token : Tokens)
     {
          if (IsSamStopword(Token))
          {
               continue;
          }

          Filtered.push_back(Token);
     }

     if (!Filtered.empty() && Filtered != Tokens)
     {
          AppendVariant(JoinTokens(Filtered));
     }

     for (auto& Token : Filtered)
     {
          Token = SingularizeToken(Token);
     }

     if (!Filtered.empty())
     {
          AppendVariant(JoinTokens(Filtered));
     }

     if (Filtered.size() >= 2)
     {
          for (size_t Start = 0; Start < Filtered.size(); ++Start)
          {
               for (size_t End = Start + 1; End < Filtered.size(); ++End)
               {
                    std::vector<std::string> Slice(Filtered.begin() + static_cast<long>(Start),
                                                   Filtered.begin() + static_cast<long>(End + 1));
                    AppendVariant(JoinTokens(Slice));
               }
          }

          for (size_t First = 0; First < Filtered.size(); ++First)
          {
               for (size_t Second = First + 1; Second < Filtered.size(); ++Second)
               {
                    AppendVariant(Filtered[First] + " " + Filtered[Second]);
               }
          }
     }

     for (const auto& Token : Filtered)
     {
          AppendVariant(Token);
     }

     return Variants;
}

size_t EditDistance(const std::string& A, const std::string& B)
{
     if (A == B)
     {
          return 0;
     }

     if (A.empty())
     {
          return B.size();
     }

     if (B.empty())
     {
          return A.size();
     }

     std::vector<size_t> Prev(B.size() + 1);
     std::vector<size_t> Curr(B.size() + 1);

     for (size_t J = 0; J <= B.size(); ++J)
     {
          Prev[J] = J;
     }

     for (size_t I = 1; I <= A.size(); ++I)
     {
          Curr[0] = I;

          for (size_t J = 1; J <= B.size(); ++J)
          {
               const size_t Cost = (A[I - 1] == B[J - 1]) ? 0 : 1;
               Curr[J] = std::min({Prev[J] + 1, Curr[J - 1] + 1, Prev[J - 1] + Cost});
          }

          Prev.swap(Curr);
     }

     return Prev[B.size()];
}

bool TokensFuzzyMatch(const std::vector<std::string>& QueryTokens,
                     const std::vector<std::string>& TermTokens)
{
     if (QueryTokens.empty() || TermTokens.empty())
     {
          return false;
     }

     size_t Matched = 0;

     for (const auto& QueryToken : QueryTokens)
     {
          bool TokenMatched = false;

          for (const auto& TermToken : TermTokens)
          {
               if (MatchSAMQueryTokenToTermToken(QueryToken, TermToken).Matched)
               {
                    TokenMatched = true;
                    break;
               }
          }

          if (TokenMatched)
          {
               Matched++;
          }
     }

     return Matched > 0 && (Matched == QueryTokens.size() || Matched + 1 >= QueryTokens.size());
}

bool ContainsNormalizedPhrase(const std::string& Value, const std::string& Phrase)
{
     if (Value.empty() || Phrase.empty())
     {
          return false;
     }

     if (Value == Phrase)
     {
          return true;
     }

     const size_t Position = Value.find(Phrase);

     if (Position == std::string::npos)
     {
          return false;
     }

     const bool LeftBoundary = (Position == 0 || Value[Position - 1] == ' ');
     const size_t End = Position + Phrase.size();
     const bool RightBoundary = (End == Value.size() || Value[End] == ' ');
     return LeftBoundary && RightBoundary;
}

std::string BuildSAMExactPhraseCandidate(const std::string& Value)
{
     const bool RequiresStopwords = !Instance || !Instance->Config
          ? true
          : Instance->Config->GetSam25ExactPhraseRequiresStopwords();
     const bool IgnoreOuterStopwords = !Instance || !Instance->Config
          ? true
          : Instance->Config->GetSam25ExactPhraseIgnoreOuterStopwords();
     std::vector<std::string> Tokens = NormalizeSAMTokens(Value, !RequiresStopwords);

     if (IgnoreOuterStopwords)
     {
          Tokens = TrimOuterSAMStopwords(std::move(Tokens));
     }

     return JoinTokens(Tokens);
}

double ComputeUnorderedWindowBoost(const std::vector<std::string>& QueryTokens,
                                   const std::vector<std::string>& TermTokens)
{
     if (QueryTokens.size() < 2 || TermTokens.size() < QueryTokens.size())
     {
          return 0.0;
     }

     size_t BestWindow = static_cast<size_t>(-1);

     for (size_t Start = 0; Start < TermTokens.size(); ++Start)
     {
          std::unordered_set<std::string> Seen;

          for (size_t End = Start; End < TermTokens.size(); ++End)
          {
               for (const auto& QueryToken : QueryTokens)
               {
                    if (MatchSAMQueryTokenToTermToken(QueryToken, TermTokens[End]).Matched)
                    {
                         Seen.insert(QueryToken);
                    }
               }

               if (Seen.size() == QueryTokens.size())
               {
                    const size_t WindowSize = End - Start + 1;

                    if (WindowSize < BestWindow)
                    {
                         BestWindow = WindowSize;
                    }

                    break;
               }
          }
     }

     if (BestWindow == static_cast<size_t>(-1))
     {
          return 0.0;
     }

     const size_t MinimumWindow = QueryTokens.size();

     if (BestWindow <= MinimumWindow)
     {
          return 0.70;
     }

     const double Slack = static_cast<double>(BestWindow - MinimumWindow);
     const double MaxSlack = static_cast<double>(
          Instance && Instance->Config
               ? std::max(1, Instance->Config->GetSam25UnorderedWindowSlop())
               : std::max<size_t>(2, QueryTokens.size()));
     const double Tightness = std::max(0.0, 1.0 - (Slack / MaxSlack));
     return 0.25 + (0.35 * Tightness);
}

double ComputeOrderedWindowBoost(const std::vector<std::string>& QueryTokens,
                                 const std::vector<std::string>& TermTokens)
{
     if (QueryTokens.size() < 2 || TermTokens.size() < QueryTokens.size())
     {
          return 0.0;
     }

     size_t BestWindow = static_cast<size_t>(-1);

     for (size_t Start = 0; Start < TermTokens.size(); ++Start)
     {
          size_t QueryIndex = 0;

          for (size_t End = Start; End < TermTokens.size() && QueryIndex < QueryTokens.size(); ++End)
          {
               if (MatchSAMQueryTokenToTermToken(QueryTokens[QueryIndex], TermTokens[End]).Matched)
               {
                    ++QueryIndex;

                    if (QueryIndex == QueryTokens.size())
                    {
                         const size_t WindowSize = End - Start + 1;

                         if (WindowSize < BestWindow)
                         {
                              BestWindow = WindowSize;
                         }
                    }
               }
          }
     }

     if (BestWindow == static_cast<size_t>(-1))
     {
          return 0.0;
     }

     const size_t MinimumWindow = QueryTokens.size();

     if (BestWindow <= MinimumWindow)
     {
          return 0.95;
     }

     const double Slack = static_cast<double>(BestWindow - MinimumWindow);
     const double MaxSlack = static_cast<double>(
          Instance && Instance->Config
               ? std::max(1, Instance->Config->GetSam25OrderedSlop())
               : std::max<size_t>(2, QueryTokens.size()));
     const double Tightness = std::max(0.0, 1.0 - (Slack / MaxSlack));
     return 0.35 + (0.40 * Tightness);
}

double GetSAMSourceWeight(const std::string& Source)
{
     if (Source == "title")
     {
          return 1.18;
     }

     if (Source == "title_reduced" || Source == "title_pair")
     {
          return 1.12;
     }

     if (Source == "label_pair")
     {
          return 1.10;
     }

     if (Source == "label" || Source == "topic")
     {
          return 1.00;
     }

     if (Source == "label_reduced" || Source == "topic_join" || Source == "category")
     {
          return 0.96;
     }

     if (Source == "llm")
     {
          return 0.90;
     }

     return 1.0;
}

double ClampSAMScore(double Value)
{
     return std::max(0.0, std::min(1.0, Value));
}

double GetSAM25QueryPhraseWeight(const SAMQueryTokenViews& QueryViews)
{
     if (!Instance || !Instance->Config)
     {
          return 1.0;
     }

     if (!Instance->Config->GetSam25DynamicQueryWeight())
     {
          return 1.0;
     }

     const double ShortBoost = Instance->Config->GetSam25ShortQueryPhraseBoost();
     const double LongBoost = Instance->Config->GetSam25LongQueryPhraseBoost();
     const size_t QueryLength = QueryViews.CoreTokens.empty() ? QueryViews.FullTokens.size() : QueryViews.CoreTokens.size();

     if (QueryLength <= 2)
     {
          return ShortBoost;
     }

     if (QueryLength >= 5)
     {
          return LongBoost;
     }

     const double Ratio = static_cast<double>(QueryLength - 2) / 3.0;
    return ShortBoost + ((LongBoost - ShortBoost) * Ratio);
}

double GetSAM25PhraseSourceWeight(const std::string& Source)
{
     if (!Instance || !Instance->Config)
     {
          return 1.0;
     }

     if (Source == "title" || Source == "title_reduced" || Source == "title_pair")
     {
          return Instance->Config->GetSam25SourcePhraseBoostTitle();
     }

     if (Source == "label_pair")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLabelPair();
     }

     if (Source == "label" || Source == "label_reduced")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLabel();
     }

     if (Source == "llm")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLlm();
     }

     return 1.0;
}

size_t CountSAMTermDocuments(rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& TermText)
{
     if (!Database || TermText.empty())
     {
          return 0;
     }

     const std::string NormalizedTerm = NormalizeTerm(TermText);

     if (NormalizedTerm.empty())
     {
          return 0;
     }

     const std::string Prefix = Collection.empty()
          ? "sam:term:" + NormalizedTerm + ":"
          : "sam:term:" + NormalizedTerm + ":" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     size_t Count = 0;

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          ++Count;
     }

     return Count;
}

double GetSAM25TermSpecificityWeight(rocksdb::DB* Database,
                                     const std::string& Collection,
                                     const std::string& TermText)
{
     if (!Instance || !Instance->Config || !Instance->Config->GetSam25EnableIdf())
     {
          return 1.0;
     }

     const double Floor = Instance->Config->GetSam25IdfFloor();
     const double Ceiling = Instance->Config->GetSam25IdfCeiling();

     if (Ceiling <= Floor)
     {
          return std::max(0.0, Ceiling);
     }

     const size_t DocumentFrequency = CountSAMTermDocuments(Database, Collection, TermText);

     if (DocumentFrequency == 0)
     {
          return Ceiling;
     }

     const double Rarity = 1.0 / std::sqrt(static_cast<double>(DocumentFrequency));
     return Floor + ((Ceiling - Floor) * ClampSAMScore(Rarity));
}

double GetSAM25NoisePenalty(const SAM::TermEntry& Term)
{
     if (!Instance || !Instance->Config || !Instance->Config->GetSam25EnableNoisePenalty())
     {
          return 0.0;
     }

     static const std::unordered_set<std::string> WeakSingleTokens = {
          "guide", "overview", "summary", "notes", "insights", "analysis",
          "review", "profile", "spotlight", "brief", "official"
     };

     const std::string Normalized = NormalizeTerm(Term.Text);
     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);
     double Penalty = 0.0;

     if (!Tokens.empty())
     {
          const std::string& Last = Tokens.back();
          const std::string& First = Tokens.front();

          if (WeakSingleTokens.find(Last) != WeakSingleTokens.end() ||
              WeakSingleTokens.find(First) != WeakSingleTokens.end())
          {
               Penalty += Instance->Config->GetSam25NoisePenalty();
          }
     }

     if (LooksLikeWeakLLMSuffix(Normalized) ||
         Normalized.find("profile") != std::string::npos ||
         Normalized.find("guide") != std::string::npos ||
         Normalized.find("overview") != std::string::npos ||
         Normalized.find("notes") != std::string::npos)
     {
          Penalty += Instance->Config->GetSam25NoisePenalty() * 0.5;
     }

     if (Term.Source == "llm")
     {
          Penalty += Instance->Config->GetSam25NoisePenaltyLlmExtra();
     }

     return ClampSAMScore(Penalty);
}

double ParseSAMNumericValue(const std::string& Value, bool& Ok)
{
     Ok = false;

     if (Value.empty())
     {
          return 0.0;
     }

     char* End = nullptr;
     const double Parsed = std::strtod(Value.c_str(), &End);

     if (End == Value.c_str() || (End != nullptr && *End != '\0'))
     {
          return 0.0;
     }

     Ok = true;
     return Parsed;
}

double GetSAM25DocumentPriorScore(const SAM::LookupHit& Hit)
{
     if (!Instance || !Instance->Config || !Instance->Config->GetSam25EnableDocPrior())
     {
          return 0.0;
     }

     const std::string& PriorField = Instance->Config->GetSam25DocPriorField();

     if (Hit.Collection.empty() || Hit.DocumentID.empty() || PriorField.empty())
     {
          return 0.0;
     }

     Document StorageDoc = HybridStorageManager::GetInstance().GetDocument(Hit.Collection, Hit.DocumentID);
     bool ParseOk = false;

     if (PriorField == "score")
     {
          return ClampSAMScore(StorageDoc.Score);
     }

     auto It = StorageDoc.Fields.find(PriorField);

     if (It == StorageDoc.Fields.end())
     {
          return 0.0;
     }

     const double Parsed = ParseSAMNumericValue(It->second, ParseOk);

     if (!ParseOk)
     {
          return 0.0;
     }

     return ClampSAMScore(Parsed);
}

struct SAMAggregatedHit
{
     SAM::LookupHit BestHit;
     size_t EvidenceCount = 0;
};

struct SAM25ScoreDebug
{
     double BaseTermScore = 0.0;
     double SignalScore = 0.0;
     double Coverage = 0.0;
     double EditSimilarity = 0.0;
     double OrderedBoost = 0.0;
     double UnorderedBoost = 0.0;
     double SynonymBoost = 0.0;
     double PhraseBoost = 0.0;
     double QueryPhraseWeight = 1.0;
     double NoisePenalty = 0.0;
     double SourceWeight = 1.0;
     double SpecificityWeight = 1.0;
     double FinalScore = -1.0;
     bool RejectedMinCoverage = false;
     bool RejectedMinFinalScore = false;
};

struct SAM25SourceDocMatch
{
     double Score = -1.0;
     std::string Field;
     std::string Explain;
};

bool IsSAM25DebugExplainEnabled()
{
     return Instance && Instance->Config && Instance->Config->GetSam25DebugExplain();
}

std::string FormatSAM25Explain(const SAM25ScoreDebug& Debug)
{
     std::ostringstream Stream;
     Stream << "sam25+";

     if (Debug.RejectedMinCoverage)
     {
          Stream << " reject=min_coverage";
          return Stream.str();
     }

     if (Debug.RejectedMinFinalScore)
     {
          Stream << " reject=min_final_score";
     }

     if (!(Instance && Instance->Config && Instance->Config->GetSam25DebugIncludeComponents()))
     {
          Stream << " score=" << Debug.FinalScore;
          return Stream.str();
     }

     Stream << " score=" << Debug.FinalScore
            << " base=" << Debug.BaseTermScore
            << " signal=" << Debug.SignalScore
            << " coverage=" << Debug.Coverage
            << " edit=" << Debug.EditSimilarity
            << " ordered=" << Debug.OrderedBoost
            << " unordered=" << Debug.UnorderedBoost
            << " synonym=" << Debug.SynonymBoost
            << " phrase=" << Debug.PhraseBoost
            << " qpw=" << Debug.QueryPhraseWeight
            << " noise=" << Debug.NoisePenalty
            << " source_w=" << Debug.SourceWeight
            << " specificity_w=" << Debug.SpecificityWeight;
     return Stream.str();
}

double GetSAM25SourceDocFieldWeight(const std::string& FieldName)
{
     if (!(Instance && Instance->Config))
     {
          return 1.0;
     }

     if (FieldName == "title")
     {
          return Instance->Config->GetSam25SourceDocTitleWeight();
     }

     if (FieldName == "description")
     {
          return Instance->Config->GetSam25SourceDocDescriptionWeight();
     }

     if (FieldName == "labels")
     {
          return Instance->Config->GetSam25SourceDocLabelsWeight();
     }

     if (FieldName == "content")
     {
          return Instance->Config->GetSam25SourceDocContentWeight();
     }

     return 0.96;
}

std::vector<std::pair<std::string, std::string>> GetSAM25SourceDocumentFields(const Document& Doc)
{
     std::vector<std::pair<std::string, std::string>> Fields;

     if (!Doc.Title.empty())
     {
          Fields.push_back({"title", Doc.Title});
     }

     const auto AddFieldIfPresent = [&Fields, &Doc](const std::string& Name)
     {
          const auto It = Doc.Fields.find(Name);

          if (It != Doc.Fields.end() && !It->second.empty())
          {
               Fields.push_back({Name, It->second});
          }
     };

     AddFieldIfPresent("description");
     AddFieldIfPresent("labels");

     if (!Doc.Content.empty())
     {
          Fields.push_back({"content", Doc.Content});
     }

     for (const auto& Entry : Doc.Fields)
     {
          if (Entry.second.empty())
          {
               continue;
          }

          if (Entry.first == "description" || Entry.first == "labels" ||
              Entry.first == "id" || Entry.first == "title" ||
              Entry.first == "content" || Entry.first == "timestamp" ||
              Entry.first == "created_at")
          {
               continue;
          }

          Fields.push_back(Entry);
     }

     return Fields;
}

double ComputeSAM25SourceFieldScore(const SAMQueryTokenViews& QueryViews,
                                    const std::string& FieldName,
                                    const std::string& FieldValue,
                                    SAM25ScoreDebug* DebugOut = nullptr)
{
     const std::vector<std::string>& QueryTokens = QueryViews.CoreTokens;

     if (QueryTokens.empty() || FieldValue.empty())
     {
          return -1.0;
     }

     std::vector<std::string> FieldTokens = TokenizeNormalized(FieldValue);

     for (auto& Token : FieldTokens)
     {
          Token = SingularizeToken(Token);
     }

     if (!TokensFuzzyMatch(QueryTokens, FieldTokens))
     {
          return -1.0;
     }

     size_t Matched = 0;
     size_t DistancePenalty = 0;
     size_t SynonymMatches = 0;

     for (const auto& QueryToken : QueryTokens)
     {
          SAMTokenMatchResult BestMatch;

          for (const auto& FieldToken : FieldTokens)
          {
               const SAMTokenMatchResult Match = MatchSAMQueryTokenToTermToken(QueryToken, FieldToken);

               if (!Match.Matched)
               {
                    continue;
               }

               if (!BestMatch.Matched || Match.Distance < BestMatch.Distance ||
                   (Match.Distance == BestMatch.Distance && !Match.UsedSynonym && BestMatch.UsedSynonym))
               {
                    BestMatch = Match;
               }
          }

          if (BestMatch.Matched)
          {
               Matched++;
               DistancePenalty += (BestMatch.Distance == static_cast<size_t>(-1) ? 0 : BestMatch.Distance);
               SynonymMatches += BestMatch.UsedSynonym ? 1U : 0U;
          }
     }

     if (Matched == 0)
     {
          return -1.0;
     }

     const double Coverage = static_cast<double>(Matched) / static_cast<double>(QueryTokens.size());
     const double EditSimilarity = ClampSAMScore(1.0 - (static_cast<double>(DistancePenalty) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size() * 2))));
     const double OrderedBoost = ClampSAMScore(ComputeOrderedWindowBoost(QueryTokens, FieldTokens));
     const double UnorderedBoost = ClampSAMScore(ComputeUnorderedWindowBoost(QueryTokens, FieldTokens));
     const double SynonymCoverage = ClampSAMScore(static_cast<double>(SynonymMatches) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size())));
     const double QueryPhraseWeight = GetSAM25QueryPhraseWeight(QueryViews);
     const double MinCoverage = (Instance && Instance->Config ? Instance->Config->GetSam25MinCoverage() : 0.50);
     const double MinOrderedBoostForPhrase = (Instance && Instance->Config ?
          Instance->Config->GetSam25MinOrderedBoostForPhrase() : 0.20);
     const double MinFinalScore = (Instance && Instance->Config ? Instance->Config->GetSam25SourceDocMinScore() : 0.32);
     const double SynonymBoost = ClampSAMScore(SynonymCoverage *
          (Instance && Instance->Config ? Instance->Config->GetSam25SynonymBoost() : 0.72));
     double PhraseBoost = 0.0;

     if (Coverage < MinCoverage)
     {
          if (DebugOut)
          {
               DebugOut->Coverage = Coverage;
               DebugOut->RejectedMinCoverage = true;
               DebugOut->FinalScore = -1.0;
          }

          return -1.0;
     }

     if (!QueryViews.NormalizedPhrase.empty())
     {
          const std::string ExactPhraseQuery = BuildSAMExactPhraseCandidate(QueryViews.NormalizedPhrase);
          const std::string ExactPhraseField = BuildSAMExactPhraseCandidate(FieldValue);

          if (OrderedBoost >= MinOrderedBoostForPhrase &&
               !ExactPhraseQuery.empty() && ContainsNormalizedPhrase(ExactPhraseField, ExactPhraseQuery))
          {
               PhraseBoost = QueryViews.Quoted ? 0.80 : 0.30;
          }
     }

     const double WeightedOrderedBoost = ClampSAMScore(OrderedBoost * QueryPhraseWeight);
     const double WeightedUnorderedBoost = ClampSAMScore(UnorderedBoost * QueryPhraseWeight);
     const double WeightedPhraseBoost = ClampSAMScore(ClampSAMScore(PhraseBoost) * QueryPhraseWeight);
     const double FieldWeight = GetSAM25SourceDocFieldWeight(FieldName);

     double Score =
          (0.38 * Coverage) +
          (0.18 * EditSimilarity) +
          (0.20 * WeightedOrderedBoost) +
          (0.12 * WeightedUnorderedBoost) +
          (0.04 * SynonymBoost) +
          (0.08 * WeightedPhraseBoost);

     Score = ClampSAMScore(Score);
     Score *= FieldWeight;

     if (DebugOut)
     {
          DebugOut->Coverage = Coverage;
          DebugOut->EditSimilarity = EditSimilarity;
          DebugOut->OrderedBoost = WeightedOrderedBoost;
          DebugOut->UnorderedBoost = WeightedUnorderedBoost;
          DebugOut->SynonymBoost = SynonymBoost;
          DebugOut->PhraseBoost = WeightedPhraseBoost;
          DebugOut->QueryPhraseWeight = QueryPhraseWeight;
          DebugOut->SourceWeight = FieldWeight;
          DebugOut->FinalScore = Score;
     }

     if (Score < MinFinalScore)
     {
          if (DebugOut)
          {
               DebugOut->RejectedMinFinalScore = true;
               DebugOut->FinalScore = Score;
          }

          return -1.0;
     }

     return Score;
}

SAM25SourceDocMatch ComputeSAM25SourceDocumentMatch(const std::string& Collection,
                                                    const std::string& DocumentID,
                                                    const SAMQueryTokenViews& QueryViews)
{
     SAM25SourceDocMatch BestMatch;

     if (!(Instance && Instance->Config && Instance->Config->GetSam25EnableSourceDocMerge()) ||
         Collection.empty() || DocumentID.empty())
     {
          return BestMatch;
     }

     const Document StorageDoc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

     if (StorageDoc.ID.empty())
     {
          return BestMatch;
     }

     for (const auto& Field : GetSAM25SourceDocumentFields(StorageDoc))
     {
          SAM25ScoreDebug Debug;
          const double FieldScore = ComputeSAM25SourceFieldScore(QueryViews, Field.first, Field.second, &Debug);

          if (FieldScore <= BestMatch.Score)
          {
               continue;
          }

          BestMatch.Score = FieldScore;
          BestMatch.Field = Field.first;

          if (IsSAM25DebugExplainEnabled())
          {
               std::ostringstream Stream;
               Stream << "sam25+ source_doc field=" << Field.first << " " << FormatSAM25Explain(Debug);
               BestMatch.Explain = Stream.str();
          }
     }

     return BestMatch;
}

void EmitSAM25DebugLog(const std::string& Query, const std::vector<SAM::LookupHit>& Hits)
{
     if (!IsSAM25DebugExplainEnabled())
     {
          return;
     }

     const int TopK = Instance && Instance->Config ? Instance->Config->GetSam25DebugLogTopK() : 10;

     if (TopK <= 0)
     {
          return;
     }

     std::cerr << "[sam25] query=\"" << Query << "\" hits=" << Hits.size() << std::endl;

     for (size_t Index = 0; Index < Hits.size() && Index < static_cast<size_t>(TopK); ++Index)
     {
          const auto& Hit = Hits[Index];
          std::cerr << "[sam25] #" << (Index + 1)
                    << " " << Hit.Collection << "/" << Hit.DocumentID
                    << " title=\"" << Hit.Title << "\""
                    << " term=\"" << Hit.MatchedTerm << "\""
                    << " source=" << Hit.MatchedSource
                    << " score=" << Hit.MatchedScore;

          if (!Hit.Explain.empty())
          {
               std::cerr << " explain=\"" << Hit.Explain << "\"";
          }

          std::cerr << std::endl;
     }
}

std::string MakeSAMHitKey(const SAM::LookupHit& Hit)
{
     return Hit.Collection + "/" + Hit.DocumentID;
}

void AccumulateSAMHit(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                      const SAM::LookupHit& Hit)
{
     if (Hit.Collection.empty() || Hit.DocumentID.empty())
     {
          return;
     }

     SAMAggregatedHit& Aggregate = AggregatedHits[MakeSAMHitKey(Hit)];
     const bool ReplaceBest =
          Aggregate.EvidenceCount == 0 ||
          Hit.MatchedScore > Aggregate.BestHit.MatchedScore ||
          (Hit.MatchedScore == Aggregate.BestHit.MatchedScore && Hit.MatchedSignal > Aggregate.BestHit.MatchedSignal) ||
          (Hit.MatchedScore == Aggregate.BestHit.MatchedScore && Hit.MatchedSignal == Aggregate.BestHit.MatchedSignal &&
           Hit.MatchedTerm < Aggregate.BestHit.MatchedTerm);

     if (ReplaceBest)
     {
          Aggregate.BestHit = Hit;
     }

     ++Aggregate.EvidenceCount;
}

void FinalizeSAMAggregatedHits(std::vector<SAM::LookupHit>& Hits,
                               const std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                               const SAMQueryTokenViews& QueryViews,
                               size_t Limit)
{
     Hits.clear();
     Hits.reserve(AggregatedHits.size());

     for (const auto& Entry : AggregatedHits)
     {
          SAM::LookupHit FinalHit = Entry.second.BestHit;
          const double EvidenceBonus = std::min(0.12, 0.03 * static_cast<double>(Entry.second.EvidenceCount > 0
               ? Entry.second.EvidenceCount - 1
               : 0));
          const double DocPrior = GetSAM25DocumentPriorScore(FinalHit);
          const SAM25SourceDocMatch SourceMatch = ComputeSAM25SourceDocumentMatch(FinalHit.Collection,
               FinalHit.DocumentID, QueryViews);
          const double SourceDocWeight = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocWeight()
               : 0.90);
          const double SourceDocMergeBonus = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocMergeBonus()
               : 0.10);
          const double SourceScoreBonus = SourceMatch.Score > 0.0
               ? std::min(SourceDocMergeBonus, SourceMatch.Score * SourceDocWeight)
               : 0.0;
          FinalHit.MatchedScore += EvidenceBonus;
          FinalHit.MatchedScore += Instance && Instance->Config
               ? (DocPrior * Instance->Config->GetSam25DocPriorWeight())
               : 0.0;
          FinalHit.MatchedScore += SourceScoreBonus;
          if (IsSAM25DebugExplainEnabled() && !FinalHit.Explain.empty())
          {
               std::ostringstream Stream;
               Stream << FinalHit.Explain
                      << " evidence_bonus=" << EvidenceBonus
                      << " doc_prior=" << DocPrior
                      << " source_doc_bonus=" << SourceScoreBonus;

               if (!SourceMatch.Field.empty())
               {
                    Stream << " source_doc_field=" << SourceMatch.Field;
               }

               if (!SourceMatch.Explain.empty())
               {
                    Stream << " source_doc_explain={" << SourceMatch.Explain << "}";
               }

               Stream
                      << " final=" << FinalHit.MatchedScore;
               FinalHit.Explain = Stream.str();
          }
          Hits.push_back(std::move(FinalHit));
     }

     std::sort(Hits.begin(), Hits.end(),
               [](const SAM::LookupHit& A, const SAM::LookupHit& B)
               {
                    if (A.MatchedScore != B.MatchedScore)
                    {
                         return A.MatchedScore > B.MatchedScore;
                    }

                    if (A.MatchedSignal != B.MatchedSignal)
                    {
                         return A.MatchedSignal > B.MatchedSignal;
                    }

                    if (A.Collection != B.Collection)
                    {
                         return A.Collection < B.Collection;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Limit > 0 && Hits.size() > Limit)
     {
          Hits.resize(Limit);
     }
}

double ComputeSAM25TermScore(rocksdb::DB* Database,
                             const std::string& Collection,
                             const SAMQueryTokenViews& QueryViews,
                             const SAM::TermEntry& Term,
                             SAM25ScoreDebug* DebugOut = nullptr)
{
     const std::vector<std::string>& QueryTokens = QueryViews.CoreTokens;

     if (QueryTokens.empty() || Term.Text.empty())
     {
          return -1.0;
     }

     std::vector<std::string> TermTokens = TokenizeNormalized(Term.Text);

     for (auto& Token : TermTokens)
     {
          Token = SingularizeToken(Token);
     }

     if (!TokensFuzzyMatch(QueryTokens, TermTokens))
     {
          return -1.0;
     }

     size_t Matched = 0;
     size_t DistancePenalty = 0;
     size_t SynonymMatches = 0;

     for (const auto& QueryToken : QueryTokens)
     {
          SAMTokenMatchResult BestMatch;

          for (const auto& TermToken : TermTokens)
          {
               const SAMTokenMatchResult Match = MatchSAMQueryTokenToTermToken(QueryToken, TermToken);

               if (!Match.Matched)
               {
                    continue;
               }

               if (!BestMatch.Matched || Match.Distance < BestMatch.Distance ||
                   (Match.Distance == BestMatch.Distance && !Match.UsedSynonym && BestMatch.UsedSynonym))
               {
                    BestMatch = Match;
               }
          }

          if (BestMatch.Matched)
          {
               Matched++;
               DistancePenalty += (BestMatch.Distance == static_cast<size_t>(-1) ? 0 : BestMatch.Distance);
               SynonymMatches += BestMatch.UsedSynonym ? 1U : 0U;
          }
     }

     if (Matched == 0)
     {
          return -1.0;
     }

     const double Coverage = static_cast<double>(Matched) / static_cast<double>(QueryTokens.size());
     const double EditSimilarity = ClampSAMScore(1.0 - (static_cast<double>(DistancePenalty) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size() * 2))));
     const double OrderedBoost = ClampSAMScore(ComputeOrderedWindowBoost(QueryTokens, TermTokens));
     const double UnorderedBoost = ClampSAMScore(ComputeUnorderedWindowBoost(QueryTokens, TermTokens));
     const double SynonymCoverage = ClampSAMScore(static_cast<double>(SynonymMatches) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size())));
     const double BaseTermScore = ClampSAMScore(Term.Score);
     const double SignalScore = ClampSAMScore(Term.Signal);
     const double QueryPhraseWeight = GetSAM25QueryPhraseWeight(QueryViews);
     const double MinCoverage = (Instance && Instance->Config ? Instance->Config->GetSam25MinCoverage() : 0.50);
     const double MinOrderedBoostForPhrase = (Instance && Instance->Config ?
          Instance->Config->GetSam25MinOrderedBoostForPhrase() : 0.20);
     const double MinFinalScore = (Instance && Instance->Config ? Instance->Config->GetSam25MinFinalScore() : 0.35);
     const double SynonymBoost = ClampSAMScore(SynonymCoverage *
          (Instance && Instance->Config ? Instance->Config->GetSam25SynonymBoost() : 0.72));
     double PhraseBoost = 0.0;

     if (Coverage < MinCoverage)
     {
          if (DebugOut)
          {
               DebugOut->Coverage = Coverage;
               DebugOut->RejectedMinCoverage = true;
               DebugOut->FinalScore = -1.0;
          }
          return -1.0;
     }

     if (!QueryViews.NormalizedPhrase.empty())
     {
          const std::string ExactPhraseQuery = BuildSAMExactPhraseCandidate(QueryViews.NormalizedPhrase);
          const std::string ExactPhraseTerm = BuildSAMExactPhraseCandidate(Term.Text);

          if (OrderedBoost >= MinOrderedBoostForPhrase &&
               !ExactPhraseQuery.empty() && ContainsNormalizedPhrase(ExactPhraseTerm, ExactPhraseQuery))
          {
               PhraseBoost = (QueryViews.Quoted ? 0.80 : 0.30) * GetSAM25PhraseSourceWeight(Term.Source);
          }
     }

     PhraseBoost = ClampSAMScore(PhraseBoost);
     const double WeightedOrderedBoost = ClampSAMScore(OrderedBoost * QueryPhraseWeight);
     const double WeightedUnorderedBoost = ClampSAMScore(UnorderedBoost * QueryPhraseWeight);
     const double WeightedPhraseBoost = ClampSAMScore(PhraseBoost * QueryPhraseWeight);

     double Score =
          (0.22 * BaseTermScore) +
          (0.12 * SignalScore) +
          (0.20 * Coverage) +
          (0.12 * EditSimilarity) +
          (0.18 * WeightedOrderedBoost) +
          (0.09 * WeightedUnorderedBoost) +
          (0.02 * SynonymBoost) +
          (0.05 * WeightedPhraseBoost);

     Score = ClampSAMScore(Score);
     const double NoisePenalty = GetSAM25NoisePenalty(Term);
     Score = ClampSAMScore(Score - NoisePenalty);
     const double SourceWeight = GetSAMSourceWeight(Term.Source);
     const double SpecificityWeight = GetSAM25TermSpecificityWeight(Database, Collection, Term.Text);
     Score *= SourceWeight;
     Score *= SpecificityWeight;
     if (DebugOut)
     {
          DebugOut->BaseTermScore = BaseTermScore;
          DebugOut->SignalScore = SignalScore;
          DebugOut->Coverage = Coverage;
          DebugOut->EditSimilarity = EditSimilarity;
          DebugOut->OrderedBoost = WeightedOrderedBoost;
          DebugOut->UnorderedBoost = WeightedUnorderedBoost;
          DebugOut->SynonymBoost = SynonymBoost;
          DebugOut->PhraseBoost = WeightedPhraseBoost;
          DebugOut->QueryPhraseWeight = QueryPhraseWeight;
          DebugOut->NoisePenalty = NoisePenalty;
          DebugOut->SourceWeight = SourceWeight;
          DebugOut->SpecificityWeight = SpecificityWeight;
          DebugOut->FinalScore = Score;
     }
     if (Score < MinFinalScore)
     {
          if (DebugOut)
          {
               DebugOut->RejectedMinFinalScore = true;
               DebugOut->FinalScore = Score;
          }
          return -1.0;
     }
     return Score;
}

void AppendFuzzyFallbackHits(std::vector<SAM::LookupHit>& Hits,
                             std::unordered_set<std::string>& SeenDocuments,
                             rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& Query,
                             size_t Limit)
{
     if (!Database || Hits.size() >= Limit)
     {
          return;
     }

     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);
     const std::vector<std::string>& QueryTokens = QueryViews.CoreTokens;

     if (QueryTokens.empty())
     {
          return;
     }

     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<SAM::LookupHit> Candidates;

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          if (!Collection.empty() && Entry.Collection != Collection)
          {
               continue;
          }

          const std::string DedupKey = Entry.Collection + "/" + Entry.DocumentID;

          if (SeenDocuments.find(DedupKey) != SeenDocuments.end())
          {
               continue;
          }

          double BestScore = -1.0;
          SAM::LookupHit BestHit;

          for (const auto& Term : Entry.Terms)
          {
               SAM25ScoreDebug Debug;
               const double FuzzyScore = ComputeSAM25TermScore(Database, Collection, QueryViews, Term, &Debug);

               if (FuzzyScore <= BestScore)
               {
                    continue;
               }

               BestScore = FuzzyScore;
               BestHit.Collection = Entry.Collection;
               BestHit.DocumentID = Entry.DocumentID;
               BestHit.Title = Entry.Title;
               BestHit.MatchedTerm = Term.Text;
               BestHit.MatchedKind = Term.Kind;
               BestHit.MatchedSource = Term.Source;
               BestHit.MatchedScore = FuzzyScore;
               BestHit.MatchedSignal = Term.Signal;
               if (IsSAM25DebugExplainEnabled())
               {
                    BestHit.Explain = FormatSAM25Explain(Debug);
               }
          }

          const SAM25SourceDocMatch SourceMatch = ComputeSAM25SourceDocumentMatch(Entry.Collection, Entry.DocumentID, QueryViews);
          const double SourceDocWeight = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocWeight()
               : 0.90);
          const double SourceDocMergeBonus = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocMergeBonus()
               : 0.10);
          const double EffectiveSourceScore = SourceMatch.Score > 0.0
               ? SourceMatch.Score * SourceDocWeight
               : -1.0;

          if (EffectiveSourceScore > BestScore)
          {
               BestScore = EffectiveSourceScore;
               BestHit.Collection = Entry.Collection;
               BestHit.DocumentID = Entry.DocumentID;
               BestHit.Title = Entry.Title;
               BestHit.MatchedTerm = QueryViews.NormalizedPhrase.empty()
                    ? QueryViews.NormalizedQuery
                    : QueryViews.NormalizedPhrase;
               BestHit.MatchedKind = "source_doc";
               BestHit.MatchedSource = SourceMatch.Field.empty() ? "source_doc" : "source_" + SourceMatch.Field;
               BestHit.MatchedScore = EffectiveSourceScore;
               BestHit.MatchedSignal = EffectiveSourceScore;

               if (IsSAM25DebugExplainEnabled())
               {
                    BestHit.Explain = SourceMatch.Explain;
               }
          }
          else if (BestScore > 0.0 && EffectiveSourceScore > 0.0)
          {
               const double MergeBonus = std::min(SourceDocMergeBonus, EffectiveSourceScore);
               BestHit.MatchedScore += MergeBonus;

               if (IsSAM25DebugExplainEnabled() && !BestHit.Explain.empty())
               {
                    std::ostringstream Stream;
                    Stream << BestHit.Explain
                           << " source_doc_bonus=" << MergeBonus;

                    if (!SourceMatch.Field.empty())
                    {
                         Stream << " source_doc_field=" << SourceMatch.Field;
                    }

                    if (!SourceMatch.Explain.empty())
                    {
                         Stream << " source_doc_explain={" << SourceMatch.Explain << "}";
                    }

                    BestHit.Explain = Stream.str();
               }
          }

          if (BestScore > 0.55 || EffectiveSourceScore > 0.0)
          {
               Candidates.push_back(std::move(BestHit));
          }
     }

     std::sort(Candidates.begin(), Candidates.end(),
               [](const SAM::LookupHit& A, const SAM::LookupHit& B)
               {
                    if (A.MatchedScore != B.MatchedScore)
                    {
                         return A.MatchedScore > B.MatchedScore;
                    }

                    if (A.MatchedSignal != B.MatchedSignal)
                    {
                         return A.MatchedSignal > B.MatchedSignal;
                    }

                    if (A.Collection != B.Collection)
                    {
                         return A.Collection < B.Collection;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     for (auto& Candidate : Candidates)
     {
          const std::string DedupKey = Candidate.Collection + "/" + Candidate.DocumentID;

          if (!SeenDocuments.insert(DedupKey).second)
          {
               continue;
          }

          Hits.push_back(std::move(Candidate));

          if (Hits.size() >= Limit)
          {
               break;
          }
     }
}

bool IsBadSamTerm(const std::string& Value)
{
     if (Value.empty() || Value.size() < 3 || Value.size() > 96)
     {
          return true;
     }

     if (Value.find("document id") != std::string::npos ||
         Value.find("collection ") != std::string::npos ||
         Value.find("artist profile") != std::string::npos ||
         Value.find(" biography and discography") != std::string::npos ||
         Value.find(" benchmark") != std::string::npos ||
         Value.find(" fake") != std::string::npos)
     {
          return true;
     }

     return false;
}

bool LooksLikeWeakLLMSuffix(const std::string& Value)
{
     static const std::unordered_set<std::string> WeakSuffixes = {
          "artist", "artists", "music", "songs", "career", "careers", "industry",
          "performance", "performances", "profile", "profiles", "official"};
     return WeakSuffixes.find(Value) != WeakSuffixes.end();
}

bool HasDuplicateTokens(const std::string& Value)
{
     std::unordered_map<std::string, size_t> Counts;

     for (const auto& Token : TokenizeNormalized(Value))
     {
          if (++Counts[Token] > 1)
          {
               return true;
          }
     }

     return false;
}

bool IsWeakLLMTerm(const std::string& Value, const std::string& Subject)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(Value);

     if (Tokens.empty() || Tokens.size() > 5)
     {
          return true;
     }

     if (HasDuplicateTokens(Value))
     {
          return true;
     }

     const std::string NormalizedSubject = NormalizeTerm(Subject);

     if (Value == NormalizedSubject)
     {
          return true;
     }

     for (const auto& Token : Tokens)
     {
          if (IsWeakSamToken(Token))
          {
               return true;
          }
     }

     if (!NormalizedSubject.empty() && Value.rfind(NormalizedSubject + " ", 0) == 0)
     {
          const std::string Suffix = TrimCopy(Value.substr(NormalizedSubject.size()));
          const std::vector<std::string> SuffixTokens = TokenizeNormalized(Suffix);

          if (SuffixTokens.empty())
          {
               return true;
          }

          if (SuffixTokens.size() > 2)
          {
               return true;
          }

          if (SuffixTokens.size() == 1 && LooksLikeWeakLLMSuffix(SuffixTokens.front()))
          {
               return true;
          }
     }

     return false;
}

std::string ClassifyLLMTermKind(const std::string& Value, const std::string& Subject)
{
     const std::string NormalizedSubject = NormalizeTerm(Subject);

     if (!NormalizedSubject.empty() && Value.rfind(NormalizedSubject + " ", 0) == 0)
     {
          return "synonym";
     }

     return "descriptor";
}

double ClassifyLLMTermScore(const std::string& Kind)
{
     return Kind == "synonym" ? 0.82 : 0.67;
}

std::string ResolveSubjectTitle(const Document& Doc)
{
     std::string Title = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);
     const std::string LowerTitle = ToLowerCopy(Title);
     const size_t ColonPos = Title.find(':');

     if (ColonPos != std::string::npos)
     {
          const std::string Prefix = ToLowerCopy(TrimCopy(Title.substr(0, ColonPos)));
          const std::string Suffix = TrimCopy(Title.substr(ColonPos + 1));

          if ((Prefix == "artist profile" || Prefix == "album review" || Prefix == "book profile" ||
               Prefix == "book spotlight" || Prefix == "movie profile" || Prefix == "profile") &&
              !Suffix.empty())
          {
               return Suffix;
          }
     }

     if (LowerTitle.rfind("music_artist_profile_", 0) == 0)
     {
          return TrimCopy(Title.substr(std::string("music_artist_profile_").size()));
     }

     return Title;
}

void AppendScoredTerm(std::vector<SAM::TermEntry>& Target,
                      std::unordered_map<std::string, size_t>& IndexByTerm,
                      const std::string& Value,
                      const std::string& Kind,
                      double Score,
                      const std::string& Source = "",
                      double Signal = 0.0)
{
     const std::string Normalized = NormalizeTerm(Value);

     if (IsBadSamTerm(Normalized))
     {
          return;
     }

     auto ExistingIt = IndexByTerm.find(Normalized);

     if (ExistingIt != IndexByTerm.end())
     {
          SAM::TermEntry& Existing = Target[ExistingIt->second];

          if (Score > Existing.Score || (Score == Existing.Score && Signal > Existing.Signal))
          {
               Existing.Score = Score;
               Existing.Kind = Kind;
               Existing.Source = Source;
               Existing.Signal = Signal;
          }

          return;
     }

     SAM::TermEntry Entry;
     Entry.Text = Normalized;
     Entry.Kind = Kind;
     Entry.Source = Source;
     Entry.Score = Score;
     Entry.Signal = Signal;
     IndexByTerm[Normalized] = Target.size();
     Target.push_back(std::move(Entry));
}

std::vector<std::string> ExtractArrayishValues(const std::string& Raw)
{
     std::vector<std::string> Values;
     const std::string Trimmed = TrimCopy(Raw);

     if (Trimmed.empty())
     {
          return Values;
     }

     try
     {
          if (!Trimmed.empty() && Trimmed.front() == '[')
          {
               nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

               if (Parsed.is_array())
               {
                    for (const auto& Entry : Parsed)
                    {
                         if (Entry.is_string())
                         {
                              const std::string Value = TrimCopy(Entry.get<std::string>());

                              if (!Value.empty())
                              {
                                   Values.push_back(Value);
                              }
                         }
                    }
               }

               return Values;
          }
     }
     catch (...)
     {
     }

     std::string Token;
     std::istringstream In(Trimmed);

     while (std::getline(In, Token, ','))
     {
          Token = TrimCopy(Token);

          if (!Token.empty())
          {
               Values.push_back(Token);
          }
     }

     if (Values.empty() && !Trimmed.empty())
     {
          Values.push_back(Trimmed);
     }

     return Values;
}

std::string ResolveSamDataDir()
{
     std::string SamDataDir = std::string(HLQUERY_SAM_DATA_DIR);
     const char* EnvSamDataDir = std::getenv("HLQUERY_SAM_DATA_DIR");

     if (EnvSamDataDir && *EnvSamDataDir)
     {
          SamDataDir = EnvSamDataDir;
     }

     try
     {
          if (Instance && Instance->Config && Instance->Config->IsValid())
          {
               const std::string& ConfiguredSamDataDir = Instance->Config->GetSamDataDirectory();

               if (!ConfiguredSamDataDir.empty())
               {
                    SamDataDir = ConfiguredSamDataDir;
               }
          }
     }
     catch (...)
     {
     }

     return SamDataDir;
}

std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID)
{
     return "sam:doc:" + Collection + ":" + DocumentID;
}

std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID)
{
     return "sam:term:" + Term + ":" + Collection + ":" + DocumentID;
}

bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry)
{
     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);
          Entry.Collection = Root.value("collection", "");
          Entry.DocumentID = Root.value("id", "");
          Entry.Title = Root.value("title", "");
          Entry.Terms.clear();

          if (Root.contains("terms") && Root["terms"].is_array())
          {
               for (const auto& Term : Root["terms"])
               {
                    if (Term.is_string())
                    {
                         SAM::TermEntry Item;
                         Item.Text = Term.get<std::string>();
                         Item.Kind = "legacy";
                         Item.Score = 0.5;
                         Entry.Terms.push_back(std::move(Item));
                    }
                    else if (Term.is_object())
                    {
                         SAM::TermEntry Item;
                         Item.Text = Term.value("text", "");
                         Item.Kind = Term.value("kind", "");
                         Item.Source = Term.value("source", "");
                         Item.Score = Term.value("score", 0.0);
                         Item.Signal = Term.value("signal", 0.0);

                         if (!Item.Text.empty())
                         {
                              Entry.Terms.push_back(std::move(Item));
                         }
                    }
               }
          }

          std::sort(Entry.Terms.begin(), Entry.Terms.end(),
                    [](const SAM::TermEntry& A, const SAM::TermEntry& B)
                    {
                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         if (A.Signal != B.Signal)
                         {
                              return A.Signal > B.Signal;
                         }

                         return A.Text < B.Text;
                    });

          return !Entry.Collection.empty() && !Entry.DocumentID.empty();
     }
     catch (...)
     {
          return false;
     }
}
SAM::SAM()
{
     OptionsValue.create_if_missing = true;
     OptionsValue.error_if_exists = false;
     OptionsValue.max_open_files = 128;
}

SAM::~SAM()
{
     Shutdown();
}

void SAM::RecordDebugEvent(const std::string& Collection, const std::string& Message) const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);

     DebugEvent Event;
     Event.Sequence = NextDebugSequence++;
     Event.Collection = Collection;
     Event.Message = Message;
     DebugEvents.push_back(std::move(Event));

     constexpr size_t MaxDebugEvents = 512;

     while (DebugEvents.size() > MaxDebugEvents)
     {
          DebugEvents.pop_front();
     }
}

bool SAM::Initialize()
{
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               return true;
          }

          DBPath = ResolveDBPath();

          try
          {
               std::filesystem::create_directories(DBPath);
          }
          catch (const std::exception& E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to create SAM directory '" + DBPath + "': " + E.what() + ".");
               }

               return false;
          }

          std::unique_ptr<rocksdb::DB> RawDB;
          const rocksdb::Status Status = rocksdb::DB::Open(OptionsValue, DBPath, &RawDB);

          if (!Status.ok())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to open SAM database at '" + DBPath + "': " + Status.ToString() + ".");
               }

               return false;
          }

          Database = std::move(RawDB);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "Secondary Assistant Manager opened at " + DBPath + ".");
          }
     }

     StartIndexWorker();

     return true;
}

void SAM::Shutdown()
{
     std::vector<std::thread> ThreadsToJoin;

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);
          ShuttingDown = true;
     }

     QueueCV.notify_all();

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          ThreadsToJoin.swap(WorkerThreads);
     }

     for (auto& Worker : ThreadsToJoin)
     {
          if (Worker.joinable())
          {
               Worker.join();
          }
     }

     std::lock_guard<std::mutex> Lock(DBMutex);
     Database.reset();
}

std::string SAM::BuildPendingIndexKey(const std::string& Collection, const std::string& DocumentID)
{
     return Collection + "\n" + DocumentID;
}

void SAM::StartIndexWorker()
{
     std::lock_guard<std::mutex> Lock(JobMutex);
     ShuttingDown = false;

     if (!WorkerThreads.empty())
     {
          return;
     }

     WorkerThreads.emplace_back([this]()
     {
          RunIndexWorker();
     });
}

void SAM::RunIndexWorker()
{
     while (true)
     {
          PendingIndexJob Job;

          {
               std::unique_lock<std::mutex> Lock(QueueMutex);
               QueueCV.wait(Lock, [this]()
               {
                    return ShuttingDown || !PendingIndexJobs.empty();
               });

               if (ShuttingDown && PendingIndexJobs.empty())
               {
                    return;
               }

               Job = std::move(PendingIndexJobs.front());
               PendingIndexJobs.pop_front();
               PendingIndexKeys.erase(BuildPendingIndexKey(Job.Collection, Job.Doc.ID));
          }

          std::string ErrorMessage;
          const bool Success = IndexDocument(Job.Collection, Job.Doc, &ErrorMessage);

          {
               std::lock_guard<std::mutex> JobLock(JobMutex);
               CollectionJobStatus& Status = CollectionJobs[Job.Collection];

               if (Status.PendingDocuments > 0)
               {
                    --Status.PendingDocuments;
               }

               if (Success)
               {
                    ++Status.IndexedDocuments;
               }
               else
               {
                    ++Status.FailedDocuments;
                    if (!ErrorMessage.empty())
                    {
                         Status.ErrorMessage = ErrorMessage;
                    }
               }

               if (Status.Running && Status.PendingDocuments == 0)
               {
                    Status.Running = false;
                    Status.Completed = true;

                    if (Success)
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments));
                    }
                    else
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete with failures: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments));
                    }
               }
          }

          if (Success)
          {
               RecordDebugEvent(Job.Collection, "background indexed " + Job.Doc.ID);
               continue;
          }

          RecordDebugEvent(Job.Collection,
                           "background indexing failed for " + Job.Doc.ID + ": " +
                                (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage));

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "Failed to background index '" + Job.Collection + "/" + Job.Doc.ID +
                                           "': " + (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage) + ".");
          }
     }
}

bool SAM::IsOpen() const
{
     std::lock_guard<std::mutex> Lock(DBMutex);
     return static_cast<bool>(Database);
}

std::string SAM::ResolveDBPath() const
{
     return ResolveSamDataDir();
}

bool SAM::ClearAll(std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     rocksdb::WriteBatch Batch;
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->SeekToFirst(); Iterator->Valid(); Iterator->Next())
     {
          Batch.Delete(Iterator->key());
     }

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::Recreate(std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!ClearAll(ErrorMessage))
     {
          return false;
     }

     size_t IndexedDocuments = 0;
     size_t FailedDocuments = 0;

     for (const std::string& Collection : HybridStorageManager::GetInstance().ListCollections())
     {
          if (!Instance || !Instance->Database)
          {
               break;
          }

          const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");

          for (const auto& DocKey : DocKeys)
          {
               const size_t LastColon = DocKey.find_last_of(':');

               if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
               {
                    continue;
               }

               const std::string DocumentID = DocKey.substr(LastColon + 1);
               const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

               if (Doc.ID.empty())
               {
                    continue;
               }

               std::string IndexError;

               if (IndexDocumentLocked(Collection, Doc, &IndexError))
               {
                    IndexedDocuments++;
               }
               else
               {
                    FailedDocuments++;

                    if (Instance && Instance->Logs && !IndexError.empty())
                    {
                         Instance->Logs->Normal("sam", "Failed to index '" + Collection + "/" + DocumentID + "' during recreate: " + IndexError + ".");
                    }
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam",
                                 "SAM recreate complete: indexed " + std::to_string(IndexedDocuments) +
                                      " documents, failed " + std::to_string(FailedDocuments) + ".");
     }

     return true;
}

bool SAM::RecreateCollection(const std::string& Collection,
                             size_t* IndexedDocuments,
                             size_t* FailedDocuments,
                             std::string* ErrorMessage)
{
     if (IndexedDocuments)
     {
          *IndexedDocuments = 0;
     }

     if (FailedDocuments)
     {
          *FailedDocuments = 0;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     if (!HybridStorageManager::GetInstance().CollectionExists(Collection))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection not found.";
          }

          return false;
     }

     std::vector<std::string> ExistingDocumentIDs;
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (ManifestIterator->Seek(ManifestPrefix);
               ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
               ManifestIterator->Next())
          {
               const std::string Key = ManifestIterator->key().ToString();

               if (Key.size() > ManifestPrefix.size())
               {
                    ExistingDocumentIDs.push_back(Key.substr(ManifestPrefix.size()));
               }
          }
     }

     for (const auto& DocumentID : ExistingDocumentIDs)
     {
          std::string RemoveError;

          if (!RemoveExistingDocumentTermsLocked(Collection, DocumentID, &RemoveError) && ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = RemoveError;
          }
     }

     size_t IndexedCount = 0;
     size_t FailedCount = 0;
     const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");

     for (const auto& DocKey : DocKeys)
     {
          const size_t LastColon = DocKey.find_last_of(':');

          if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
          {
               continue;
          }

          const std::string DocumentID = DocKey.substr(LastColon + 1);
          const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

          if (Doc.ID.empty())
          {
               continue;
          }

          std::string IndexError;

          if (IndexDocumentLocked(Collection, Doc, &IndexError))
          {
               IndexedCount++;
          }
          else
          {
               FailedCount++;

               if (Instance && Instance->Logs && !IndexError.empty())
               {
                    Instance->Logs->Normal("sam",
                                           "Failed to index '" + Collection + "/" + DocumentID +
                                                "' during collection rebuild: " + IndexError + ".");
               }
          }
     }

     if (IndexedDocuments)
     {
          *IndexedDocuments = IndexedCount;
     }

     if (FailedDocuments)
     {
          *FailedDocuments = FailedCount;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam",
                                 "SAM rebuild for collection '" + Collection + "' complete: indexed " +
                                      std::to_string(IndexedCount) + " documents, failed " +
                                      std::to_string(FailedCount) + ".");
     }

     return true;
}

bool SAM::StartRecreateCollectionAsync(const std::string& Collection,
                                       bool* AlreadyRunning,
                                       std::string* ErrorMessage)
{
     if (AlreadyRunning)
     {
          *AlreadyRunning = false;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }
     }

     if (!HybridStorageManager::GetInstance().CollectionExists(Collection))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection not found.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          const auto ExistingIt = CollectionJobs.find(Collection);

          if (ExistingIt != CollectionJobs.end() && ExistingIt->second.Running)
          {
               if (AlreadyRunning)
               {
                    *AlreadyRunning = true;
               }

               return true;
          }
     }

     std::vector<std::string> ExistingDocumentIDs;
     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (ManifestIterator->Seek(ManifestPrefix);
               ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
               ManifestIterator->Next())
          {
               const std::string Key = ManifestIterator->key().ToString();

               if (Key.size() > ManifestPrefix.size())
               {
                    ExistingDocumentIDs.push_back(Key.substr(ManifestPrefix.size()));
               }
          }
     }

     for (const auto& DocumentID : ExistingDocumentIDs)
     {
          std::string RemoveError;

          if (!DeleteDocument(Collection, DocumentID, &RemoveError) && ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = RemoveError;
          }
     }

     const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");
     std::vector<Document> DocumentsToQueue;
     DocumentsToQueue.reserve(DocKeys.size());

     for (const auto& DocKey : DocKeys)
     {
          const size_t LastColon = DocKey.find_last_of(':');

          if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
          {
               continue;
          }

          const std::string DocumentID = DocKey.substr(LastColon + 1);
          const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

          if (!Doc.ID.empty())
          {
               DocumentsToQueue.push_back(Doc);
          }
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobStatus& JobStatus = CollectionJobs[Collection];
          JobStatus = CollectionJobStatus{};
          JobStatus.Running = true;
          JobStatus.Completed = false;
          JobStatus.PendingDocuments = DocumentsToQueue.size();
          JobStatus.TotalDocuments = DocumentsToQueue.size();
          JobStatus.ErrorMessage.clear();
     }

     RecordDebugEvent(Collection,
                      "queued background rebuild with " + std::to_string(DocumentsToQueue.size()) + " document(s)");

     for (const auto& Doc : DocumentsToQueue)
     {
          std::string QueueError;

          if (!EnqueueIndexDocument(Collection, Doc, &QueueError))
          {
               std::lock_guard<std::mutex> Lock(JobMutex);
               CollectionJobStatus& JobStatus = CollectionJobs[Collection];
               if (JobStatus.PendingDocuments > 0)
               {
                    --JobStatus.PendingDocuments;
               }
               ++JobStatus.FailedDocuments;
               JobStatus.ErrorMessage = QueueError;
          }
     }

     if (DocumentsToQueue.empty())
     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobStatus& JobStatus = CollectionJobs[Collection];
          JobStatus.Running = false;
          JobStatus.Completed = true;
          RecordDebugEvent(Collection, "rebuild complete: indexed 0, failed 0");
     }

     return true;
}

bool SAM::EnqueueIndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage)
{
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }
     }

     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     const std::string PendingKey = BuildPendingIndexKey(Collection, Doc.ID);

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);

          if (!PendingIndexKeys.insert(PendingKey).second)
          {
               for (auto& ExistingJob : PendingIndexJobs)
               {
                    if (ExistingJob.Collection == Collection && ExistingJob.Doc.ID == Doc.ID)
                    {
                         ExistingJob.Doc = Doc;
                         break;
                    }
               }

               return true;
          }

          PendingIndexJobs.push_back(PendingIndexJob{Collection, Doc});
     }

     RecordDebugEvent(Collection, "queued background index for " + Doc.ID);
     QueueCV.notify_one();
     return true;
}

bool SAM::RemoveExistingDocumentTermsLocked(const std::string& Collection,
                                            const std::string& DocumentID,
                                            std::string* ErrorMessage)
{
     const std::string ManifestKey = BuildDocManifestKey(Collection, DocumentID);
     std::string ExistingValue;
     const rocksdb::Status GetStatus = Database->Get(rocksdb::ReadOptions(), ManifestKey, &ExistingValue);

     if (GetStatus.IsNotFound())
     {
          return true;
     }

     if (!GetStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = GetStatus.ToString();
          }

          return false;
     }

     rocksdb::WriteBatch Batch;

     try
     {
          nlohmann::json Root = nlohmann::json::parse(ExistingValue);

          if (Root.contains("terms") && Root["terms"].is_array())
          {
               for (const auto& Entry : Root["terms"])
               {
                    if (Entry.is_string())
                    {
                         Batch.Delete(BuildTermKey(Entry.get<std::string>(), Collection, DocumentID));
                    }
                    else if (Entry.is_object())
                    {
                         const std::string Text = Entry.value("text", "");

                         if (!Text.empty())
                         {
                              Batch.Delete(BuildTermKey(Text, Collection, DocumentID));
                         }
                    }
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
     }

     Batch.Delete(ManifestKey);

     const rocksdb::Status WriteStatus = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::IndexDocumentLocked(const std::string& Collection, const Document& Doc, std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     if (!RemoveExistingDocumentTermsLocked(Collection, Doc.ID, ErrorMessage))
     {
          return false;
     }

     const std::vector<TermEntry> Terms = ExpandDocumentTerms(Collection, Doc);
     if (Terms.empty())
     {
          RecordDebugEvent(Collection, "indexed " + Doc.ID + " with no usable terms");
     }
     else
     {
          std::ostringstream Preview;
          const size_t PreviewCount = std::min<size_t>(Terms.size(), 3);

          for (size_t Index = 0; Index < PreviewCount; ++Index)
          {
               if (Index > 0)
               {
                    Preview << ", ";
               }

               Preview << Terms[Index].Text;
          }

          RecordDebugEvent(Collection,
                           "indexed " + Doc.ID + " with " + std::to_string(Terms.size()) +
                                " term(s): " + Preview.str());
     }
     rocksdb::WriteBatch Batch;
     nlohmann::json Manifest;
     Manifest["collection"] = Collection;
     Manifest["id"] = Doc.ID;
     Manifest["title"] = Doc.Title;
     Manifest["terms"] = nlohmann::json::array();

     for (const auto& Term : Terms)
     {
          Manifest["terms"].push_back({
               {"text", Term.Text},
               {"kind", Term.Kind},
               {"source", Term.Source},
               {"score", Term.Score},
               {"signal", Term.Signal}
          });

          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = Doc.ID;
          Payload["title"] = Doc.Title;
          Payload["term"] = Term.Text;
          Payload["kind"] = Term.Kind;
          Payload["source"] = Term.Source;
          Payload["score"] = Term.Score;
          Payload["signal"] = Term.Signal;
          Batch.Put(BuildTermKey(Term.Text, Collection, Doc.ID), Payload.dump());
     }

     Batch.Put(BuildDocManifestKey(Collection, Doc.ID), Manifest.dump());

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::IndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);
     return IndexDocumentLocked(Collection, Doc, ErrorMessage);
}

bool SAM::DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage)
{
     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexKeys.erase(BuildPendingIndexKey(Collection, DocumentID));
          PendingIndexJobs.erase(
               std::remove_if(PendingIndexJobs.begin(), PendingIndexJobs.end(),
                              [&](const PendingIndexJob& Job)
                              {
                                   return Job.Collection == Collection && Job.Doc.ID == DocumentID;
                              }),
               PendingIndexJobs.end());
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     return RemoveExistingDocumentTermsLocked(Collection, DocumentID, ErrorMessage);
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTerms(const std::string& Collection, const Document& Doc) const
{
     std::vector<TermEntry> Terms;

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("sam",
                                     "GenerateLLMTerms: skipped for '" + Collection + "/" + Doc.ID +
                                          "' because LLM is not configured.");
          }

          return Terms;
     }

     const std::string& Command = Instance->LLM->GetInferenceCommand();

     if (Command.empty())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("sam",
                                     "GenerateLLMTerms: skipped for '" + Collection + "/" + Doc.ID +
                                          "' because inference_command is empty.");
          }

          return Terms;
     }

     nlohmann::json Payload;
     Payload["collection"] = Collection;
     Payload["id"] = Doc.ID;
     Payload["title"] = Doc.Title;
     Payload["content"] = Doc.Content;
     Payload["fields"] = Doc.Fields;

     const auto StartedAt = std::chrono::steady_clock::now();
     std::lock_guard<std::mutex> Lock(InferenceMutex);
     RecordDebugEvent(Collection, "running LLM for " + Doc.ID);

     setenv("HLQUERY_LLM_MODEL", Instance->LLM->GetModelPath().c_str(), 1);
     setenv("HLQUERY_SAM_DOC_JSON", Payload.dump().c_str(), 1);

     FILE* Pipe = popen(Command.c_str(), "r");

     if (!Pipe)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "GenerateLLMTerms: failed to start inference command for '" +
                                           Collection + "/" + Doc.ID + "': " + Command + ".");
          }

          unsetenv("HLQUERY_LLM_MODEL");
          unsetenv("HLQUERY_SAM_DOC_JSON");
          RecordDebugEvent(Collection, "failed to start LLM command for " + Doc.ID);
          return Terms;
     }

     std::unordered_map<std::string, size_t> IndexByTerm;
     std::array<char, 512> Buffer{};
     const std::string Subject = ResolveSubjectTitle(Doc);

     while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe))
     {
          const std::string Normalized = NormalizeTerm(Buffer.data());

          if (IsWeakLLMTerm(Normalized, Subject))
          {
               continue;
          }

          const std::string Kind = ClassifyLLMTermKind(Normalized, Subject);
          AppendScoredTerm(Terms, IndexByTerm, Normalized, Kind, ClassifyLLMTermScore(Kind), "llm", 0.72);

          if (Terms.size() >= 6)
          {
               break;
          }
     }

     const int PipeStatus = pclose(Pipe);
     unsetenv("HLQUERY_LLM_MODEL");
     unsetenv("HLQUERY_SAM_DOC_JSON");

     const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - StartedAt).count();

     if (PipeStatus == -1)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "GenerateLLMTerms: inference command close failed for '" +
                                           Collection + "/" + Doc.ID + "' after " +
                                           std::to_string(ElapsedMs) + " ms.");
          }
          RecordDebugEvent(Collection, "LLM close failed for " + Doc.ID);
     }
     else if (WIFEXITED(PipeStatus) && WEXITSTATUS(PipeStatus) != 0)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "GenerateLLMTerms: inference command exited with status " +
                                           std::to_string(WEXITSTATUS(PipeStatus)) + " for '" +
                                           Collection + "/" + Doc.ID + "' after " +
                                           std::to_string(ElapsedMs) + " ms.");
          }
          RecordDebugEvent(Collection,
                           "LLM exited with status " + std::to_string(WEXITSTATUS(PipeStatus)) +
                                " for " + Doc.ID);
     }
     else if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("sam",
                                "GenerateLLMTerms: produced " + std::to_string(Terms.size()) +
                                     " term(s) for '" + Collection + "/" + Doc.ID + "' in " +
                                     std::to_string(ElapsedMs) + " ms.");
     }

     RecordDebugEvent(Collection,
                      "LLM produced " + std::to_string(Terms.size()) + " term(s) for " + Doc.ID +
                           " in " + std::to_string(ElapsedMs) + " ms");

     return Terms;
}

std::vector<SAM::TermEntry> SAM::ExpandDocumentTerms(const std::string& Collection, const Document& Doc) const
{
     std::vector<TermEntry> Terms = GenerateLLMTerms(Collection, Doc);

     std::sort(Terms.begin(), Terms.end(),
               [](const TermEntry& A, const TermEntry& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Signal != B.Signal)
                    {
                         return A.Signal > B.Signal;
                    }

                    return A.Text < B.Text;
               });

     if (Terms.size() > 14)
     {
          Terms.resize(14);
     }

     return Terms;
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const std::vector<std::string> Variants = BuildQueryVariants(Query);
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);

     if (Variants.empty())
     {
          return Hits;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Hits;
     }

     std::unordered_set<std::string> SeenDocuments;
     std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
          {
               try
               {
                    nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
                    LookupHit Hit;
                    Hit.Collection = Payload.value("collection", "");
                    Hit.DocumentID = Payload.value("id", "");
                    Hit.Title = Payload.value("title", "");
                    Hit.MatchedTerm = Payload.value("term", "");
                    Hit.MatchedKind = Payload.value("kind", "");
                    Hit.MatchedSource = Payload.value("source", "");
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    if (IsSAM25DebugExplainEnabled())
                    {
                         std::ostringstream Stream;
                         Stream << "sam25+ direct score=" << Hit.MatchedScore
                                << " signal=" << Hit.MatchedSignal
                                << " source=" << Hit.MatchedSource;
                         Hit.Explain = Stream.str();
                    }

                    if (!Hit.Collection.empty() && !Hit.DocumentID.empty())
                    {
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

     for (const auto& Hit : Hits)
     {
          SeenDocuments.insert(MakeSAMHitKey(Hit));
     }

     if (Hits.size() < Limit)
     {
          AppendFuzzyFallbackHits(Hits, SeenDocuments, Database.get(), "", Query, Limit);
     }

     EmitSAM25DebugLog(Query, Hits);
     return Hits;
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Collection, const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);

     if (Collection.empty())
     {
          return Lookup(Query, Limit);
     }

     const std::vector<std::string> Variants = BuildQueryVariants(Query);

     if (Variants.empty())
     {
          return Hits;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Hits;
     }

     std::unordered_set<std::string> SeenDocuments;
     std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
          {
               try
               {
                    const nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
                    LookupHit Hit;
                    Hit.Collection = Payload.value("collection", "");
                    Hit.DocumentID = Payload.value("id", "");
                    Hit.Title = Payload.value("title", "");
                    Hit.MatchedTerm = Payload.value("term", "");
                    Hit.MatchedKind = Payload.value("kind", "");
                    Hit.MatchedSource = Payload.value("source", "");
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    if (IsSAM25DebugExplainEnabled())
                    {
                         std::ostringstream Stream;
                         Stream << "sam25+ direct score=" << Hit.MatchedScore
                                << " signal=" << Hit.MatchedSignal
                                << " source=" << Hit.MatchedSource;
                         Hit.Explain = Stream.str();
                    }

                    if (Hit.Collection != Collection)
                    {
                         continue;
                    }

                    if (!Hit.DocumentID.empty())
                    {
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

     for (const auto& Hit : Hits)
     {
          SeenDocuments.insert(MakeSAMHitKey(Hit));
     }

     if (Hits.size() < Limit)
     {
          AppendFuzzyFallbackHits(Hits, SeenDocuments, Database.get(), Collection, Query, Limit);
     }

     EmitSAM25DebugLog(Query, Hits);
     return Hits;
}

std::vector<SAM::DocumentEntry> SAM::ListDocuments(const std::string& Collection, size_t Limit, size_t Offset) const
{
     std::vector<DocumentEntry> Entries;

     if (Collection.empty() || Limit == 0)
     {
          return Entries;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Entries;
     }

     const std::string Prefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     size_t Seen = 0;

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          if (Seen++ < Offset)
          {
               continue;
          }

          DocumentEntry Entry;

          if (ParseManifestValue(Iterator->value().ToString(), Entry))
          {
               Entries.push_back(std::move(Entry));
          }

          if (Entries.size() >= Limit)
          {
               break;
          }
     }

     return Entries;
}

bool SAM::GetCollectionJobStatus(const std::string& Collection, CollectionJobStatus& Status) const
{
     Status = CollectionJobStatus{};

     if (Collection.empty())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(JobMutex);
     const auto It = CollectionJobs.find(Collection);

     if (It == CollectionJobs.end())
     {
          return false;
     }

     Status = It->second;
     return true;
}

std::map<std::string, SAM::CollectionJobStatus> SAM::GetAllCollectionJobStatuses() const
{
     std::lock_guard<std::mutex> Lock(JobMutex);
     return CollectionJobs;
}

bool SAM::GetDocumentEntry(const std::string& Collection,
                           const std::string& DocumentID,
                           DocumentEntry& Entry,
                           std::string* ErrorMessage) const
{
     Entry = DocumentEntry{};

     if (Collection.empty() || DocumentID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name and document ID are required.";
          }

          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     std::string Value;
     const rocksdb::Status Status = Database->Get(rocksdb::ReadOptions(),
                                                  BuildDocManifestKey(Collection, DocumentID),
                                                  &Value);

     if (Status.IsNotFound())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM document not found.";
          }

          return false;
     }

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     if (!ParseManifestValue(Value, Entry))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to parse SAM document entry.";
          }

          return false;
     }

     return true;
}

std::vector<SAM::DebugEvent> SAM::GetDebugEvents(const std::string& Collection,
                                                 uint64_t SinceSequence,
                                                 size_t Limit) const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);
     std::vector<DebugEvent> Events;

     if (Limit == 0)
     {
          return Events;
     }

     for (const auto& Event : DebugEvents)
     {
          if (Event.Sequence <= SinceSequence)
          {
               continue;
          }

          if (!Collection.empty() && Event.Collection != Collection)
          {
               continue;
          }

          Events.push_back(Event);

          if (Events.size() >= Limit)
          {
               break;
          }
     }

     return Events;
}

uint64_t SAM::GetLatestDebugSequence() const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);

     if (DebugEvents.empty())
     {
          return 0;
     }

     return DebugEvents.back().Sequence;
}
