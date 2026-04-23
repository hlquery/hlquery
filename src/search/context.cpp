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
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hlquery.h"
#include "search/sam.h"
#include "vendor/json/json.hpp"

static std::string TrimCopy(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

static std::string NormalizeTerm(const std::string& Value)
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

static std::vector<std::string> TokenizeNormalized(const std::string& Value)
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

static std::string SingularizeToken(const std::string& Token)
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

static std::string JoinTokens(const std::vector<std::string>& Tokens)
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

static bool IsWeakSamToken(const std::string& Value)
{
     static const std::unordered_set<std::string> WeakTokens = {
          "article", "articles", "page", "pages", "document", "documents", "content",
          "collection", "collections", "id", "official", "this", "that",
          "these", "those", "their", "there", "here", "brief", "guide"};
     return WeakTokens.find(Value) != WeakTokens.end();
}

static bool IsSamStopword(const std::string& Value)
{
     static const std::unordered_set<std::string> Stopwords = {
          "a", "an", "the",
          "and", "or",
          "of", "to", "for", "in", "on", "at", "by", "with", "from",
          "into", "over", "after", "before", "about", "through", "during",
          "without", "under", "between", "against"};
     return Stopwords.find(Value) != Stopwords.end();
}

static double ClampSAMScore(double Value)
{
     if (Value < 0.0)
     {
          return 0.0;
     }

     if (Value > 1.0)
     {
          return 1.0;
     }

     return Value;
}

static bool IsBadSamTerm(const std::string& Value)
{
     if (Value.empty() || Value.size() < 3 || Value.size() > 96)
     {
          return true;
     }

     if (Value.find("document id") != std::string::npos ||
         Value.find("collection ") != std::string::npos)
     {
          return true;
     }

     return false;
}

static bool IsWeakLLMSuffix(const std::string& Value)
{
     static const std::unordered_set<std::string> WeakSuffixes = {
          "official"};
     return WeakSuffixes.find(Value) != WeakSuffixes.end();
}

static bool HasDuplicateTokens(const std::string& Value)
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

static bool IsGenericDescriptorToken(const std::string& Token)
{
     static const std::unordered_set<std::string> GenericTokens = {
          "feature", "focused", "focus", "example", "examples", "note", "notes"};
     return GenericTokens.find(Token) != GenericTokens.end();
}

static bool IsStrongPhraseToken(const std::string& Token)
{
     return !(Token.empty() || IsSamStopword(Token) || IsWeakSamToken(Token) || IsGenericDescriptorToken(Token));
}

static bool IsLowIntentGenericPhrase(const std::string& Value, const std::string& Subject)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(Value);

     if (Tokens.empty())
     {
          return true;
     }

     size_t GenericCount = 0;
     size_t StrongCount = 0;

     for (const auto& Token : Tokens)
     {
          if (IsGenericDescriptorToken(Token))
          {
               ++GenericCount;
          }

          if (IsStrongPhraseToken(Token))
          {
               ++StrongCount;
          }
     }

     if (StrongCount == 0)
     {
          return true;
     }

     if (Tokens.size() <= 2 && GenericCount == Tokens.size())
     {
          return true;
     }

     const std::string NormalizedSubject = NormalizeTerm(Subject);

     if (!NormalizedSubject.empty() && NormalizeTerm(Value) == NormalizedSubject)
     {
          return true;
     }

     return false;
}

static bool IsWeakLLMTerm(const std::string& Value, const std::string& Subject)
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

          if (SuffixTokens.size() == 1 && IsWeakLLMSuffix(SuffixTokens.front()))
          {
               return true;
          }
     }

     if (IsLowIntentGenericPhrase(Value, Subject))
     {
          return true;
     }

     return false;
}

static std::string ResolveSubjectTitle(const Document& Doc)
{
     std::string Title = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);
     const size_t ColonPos = Title.find(':');

     if (ColonPos != std::string::npos)
     {
          const std::string Prefix = ToLowerCopy(TrimCopy(Title.substr(0, ColonPos)));
          const std::string Suffix = TrimCopy(Title.substr(ColonPos + 1));

          if (!Suffix.empty() && IsLowIntentGenericPhrase(Prefix, ""))
          {
               return Suffix;
          }
     }

     return Title;
}

static std::string WriteJSONPayloadTempFile(const nlohmann::json& Payload)
{
     std::filesystem::path TempTemplate =
          std::filesystem::temp_directory_path() / "hlquery-sam-XXXXXX.json";
     std::string TempPath = TempTemplate.string();

     if (TempPath.size() < 6)
     {
          return "";
     }

     const size_t SuffixLength = 5;
     int FD = mkstemps(TempPath.data(), static_cast<int>(SuffixLength));

     if (FD == -1)
     {
          return "";
     }

     close(FD);

     std::ofstream Output(TempPath, std::ios::out | std::ios::trunc | std::ios::binary);

     if (!Output)
     {
          std::error_code IgnoreError;
          std::filesystem::remove(TempPath, IgnoreError);
          return "";
     }

     Output << Payload.dump();
     Output.close();

     if (!Output)
     {
          std::error_code IgnoreError;
          std::filesystem::remove(TempPath, IgnoreError);
          return "";
     }

     return TempPath;
}

static void AppendScoredTerm(std::vector<SAM::TermEntry>& Target,
                             std::unordered_map<std::string, size_t>& IndexByTerm,
                             const std::string& Value,
                             const std::string& Kind,
                             double Score,
                             const std::string& Source,
                             double Signal)
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

static std::vector<std::string> ExtractArrayishValues(const std::string& Raw)
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

static std::string ClassifyLLMTermKind(const std::string& Value, const std::string& Subject)
{
     const std::string NormalizedSubject = NormalizeTerm(Subject);

     if (!NormalizedSubject.empty() && Value.rfind(NormalizedSubject + " ", 0) == 0)
     {
          return "synonym";
     }

     return "descriptor";
}

static double ClassifyLLMTermScore(const std::string& Kind)
{
     return Kind == "synonym" ? 0.82 : 0.67;
}

static std::string TruncateForContextWindows(const std::string& Value, size_t MaxChars = 360)
{
     if (Value.size() <= MaxChars)
     {
          return Value;
     }

     const std::string Prefix = Value.substr(0, MaxChars);
     const size_t SentenceBreak = Prefix.find_last_of(".!?");

     if (SentenceBreak != std::string::npos && SentenceBreak > MaxChars / 2)
     {
          return Prefix.substr(0, SentenceBreak + 1);
     }

     const size_t SpaceBreak = Prefix.find_last_of(' ');

     if (SpaceBreak != std::string::npos && SpaceBreak > MaxChars / 2)
     {
          return Prefix.substr(0, SpaceBreak);
     }

     return Prefix;
}

static void AppendStructuredPhraseWindows(std::vector<SAM::TermEntry>& Terms,
                                          std::unordered_map<std::string, size_t>& IndexByTerm,
                                          const std::string& RawText,
                                          const std::string& Source,
                                          double BaseScore,
                                          double BaseSignal,
                                          size_t MaxWindow = 4)
{
     std::vector<std::string> Tokens = TokenizeNormalized(RawText);

     if (Tokens.size() < 2)
     {
          return;
     }

     for (auto& Token : Tokens)
     {
          Token = SingularizeToken(Token);
     }

     for (size_t Start = 0; Start < Tokens.size(); ++Start)
     {
          for (size_t Width = 2; Width <= MaxWindow && Start + Width <= Tokens.size(); ++Width)
          {
               std::vector<std::string> Slice(Tokens.begin() + static_cast<long>(Start),
                                              Tokens.begin() + static_cast<long>(Start + Width));

               while (!Slice.empty() && (IsSamStopword(Slice.front()) || IsWeakSamToken(Slice.front())))
               {
                    Slice.erase(Slice.begin());
               }

               while (!Slice.empty() && (IsSamStopword(Slice.back()) || IsWeakSamToken(Slice.back())))
               {
                    Slice.pop_back();
               }

               if (Slice.size() < 2)
               {
                    continue;
               }

               size_t StrongCount = 0;

               for (const auto& Token : Slice)
               {
                    if (IsStrongPhraseToken(Token))
                    {
                         ++StrongCount;
                    }
               }

               if (StrongCount < 2)
               {
                    continue;
               }

               const std::string Candidate = JoinTokens(Slice);

               if (IsLowIntentGenericPhrase(Candidate, ""))
               {
                    continue;
               }

               const double Score = ClampSAMScore(BaseScore + (Slice.size() >= 3 ? 0.04 : 0.0));
               const double Signal = ClampSAMScore(BaseSignal + (Slice.size() >= 3 ? 0.04 : 0.0));
               AppendScoredTerm(Terms, IndexByTerm, Candidate, "descriptor", Score, Source, Signal);
          }
     }
}

static void AppendReorderedAliases(std::vector<SAM::TermEntry>& Terms,
                                   std::unordered_map<std::string, size_t>& IndexByTerm,
                                   const std::string& Collection)
{
     const std::string NormalizedCollection = NormalizeTerm(Collection);
     const size_t OriginalSize = Terms.size();

     for (size_t Index = 0; Index < OriginalSize; ++Index)
     {
          const SAM::TermEntry& Term = Terms[Index];
          const std::vector<std::string> Tokens = TokenizeNormalized(Term.Text);

          if (Tokens.size() < 2 || Tokens.size() > 4 || IsLowIntentGenericPhrase(Term.Text, ""))
          {
               continue;
          }

          if (Tokens.size() == 2)
          {
               std::vector<std::string> Reversed = Tokens;
               std::reverse(Reversed.begin(), Reversed.end());
               AppendScoredTerm(Terms, IndexByTerm, JoinTokens(Reversed), "alias",
                                ClampSAMScore(Term.Score - 0.10), "reordered", ClampSAMScore(Term.Signal - 0.08));
               continue;
          }

          if (!NormalizedCollection.empty() && Tokens.front() == NormalizedCollection)
          {
               std::vector<std::string> Rotated(Tokens.begin() + 1, Tokens.end());
               Rotated.push_back(Tokens.front());
               AppendScoredTerm(Terms, IndexByTerm, JoinTokens(Rotated), "alias",
                                ClampSAMScore(Term.Score - 0.08), "reordered", ClampSAMScore(Term.Signal - 0.06));
          }
     }
}

static void AppendCompressedStopwordVariants(std::vector<SAM::TermEntry>& Terms,
                                             std::unordered_map<std::string, size_t>& IndexByTerm)
{
     const size_t OriginalSize = Terms.size();

     for (size_t Index = 0; Index < OriginalSize; ++Index)
     {
          const SAM::TermEntry& Term = Terms[Index];
          const std::vector<std::string> Tokens = TokenizeNormalized(Term.Text);

          if (Tokens.size() < 3 || Tokens.size() > 5)
          {
               continue;
          }

          std::vector<std::string> Reduced;
          Reduced.reserve(Tokens.size());

          for (const auto& Token : Tokens)
          {
               if (!IsSamStopword(Token))
               {
                    Reduced.push_back(Token);
               }
          }

          if (Reduced.size() >= 2 && Reduced.size() < Tokens.size())
          {
               AppendScoredTerm(Terms, IndexByTerm, JoinTokens(Reduced), "alias",
                                ClampSAMScore(Term.Score - 0.10), "compressed", ClampSAMScore(Term.Signal - 0.08));
          }
     }
}

static void AppendUniqueFact(std::vector<std::string>& Values,
                             std::unordered_set<std::string>& Seen,
                             const std::string& Raw,
                             size_t MaxTokens = 4);

static bool IsContextFieldName(const std::string& FieldName)
{
     const std::string Lower = ToLowerCopy(FieldName);

     static const std::unordered_set<std::string> IgnoredFields = {
          "id", "name", "title", "content", "description", "text", "body", "summary",
          "created_at", "updated_at", "timestamp", "date"};

     if (IgnoredFields.find(Lower) != IgnoredFields.end())
     {
          return false;
     }

     return true;
}

static std::vector<std::string> CollectDynamicFieldFacts(const Document& Doc,
                                                         size_t MaxFacts,
                                                         size_t MaxTokens = 4)
{
     std::vector<std::string> Facts;
     std::unordered_set<std::string> Seen;

     for (const auto& Pair : Doc.Fields)
     {
          if (!IsContextFieldName(Pair.first))
          {
               continue;
          }

          for (const auto& Value : ExtractArrayishValues(Pair.second))
          {
               AppendUniqueFact(Facts, Seen, Value, MaxTokens);

               if (Facts.size() >= MaxFacts)
               {
                    return Facts;
               }
          }
     }

     return Facts;
}

static std::vector<std::string> CollectDynamicContextHints(const Document& Doc,
                                                           size_t MaxHints,
                                                           size_t MaxTokens = 4)
{
     std::vector<std::string> Hints;
     std::unordered_set<std::string> Seen;

     for (const auto& Value : CollectDynamicFieldFacts(Doc, MaxHints, MaxTokens))
     {
          AppendUniqueFact(Hints, Seen, Value, MaxTokens);
     }

     if (Hints.size() >= MaxHints)
     {
          return Hints;
     }

     const std::string ContextText = Doc.Title + " " +
                                     (Doc.Fields.count("description") ? Doc.Fields.at("description") : "") +
                                     " " + TruncateForContextWindows(Doc.Content, 240);
     const std::vector<std::string> Tokens = TokenizeNormalized(ContextText);

     for (size_t Start = 0; Start < Tokens.size() && Hints.size() < MaxHints; ++Start)
     {
          for (size_t Width = 2; Width <= MaxTokens && Start + Width <= Tokens.size(); ++Width)
          {
               std::vector<std::string> Slice(Tokens.begin() + static_cast<long>(Start),
                                              Tokens.begin() + static_cast<long>(Start + Width));

               while (!Slice.empty() && (IsSamStopword(Slice.front()) || IsWeakSamToken(Slice.front())))
               {
                    Slice.erase(Slice.begin());
               }

               while (!Slice.empty() && (IsSamStopword(Slice.back()) || IsWeakSamToken(Slice.back())))
               {
                    Slice.pop_back();
               }

               const std::string Candidate = JoinTokens(Slice);
               AppendUniqueFact(Hints, Seen, Candidate, MaxTokens);

               if (Hints.size() >= MaxHints)
               {
                    break;
               }
          }
     }

     return Hints;
}

static void AppendTemplateQuery(std::vector<SAM::TermEntry>& Terms,
                                std::unordered_map<std::string, size_t>& IndexByTerm,
                                const std::string& Subject,
                                const std::string& Phrase,
                                double Score,
                                double Signal);

static void AppendUniqueFact(std::vector<std::string>& Values,
                             std::unordered_set<std::string>& Seen,
                             const std::string& Raw,
                             size_t MaxTokens)
{
     const std::string Normalized = NormalizeTerm(Raw);

     if (Normalized.empty() || IsLowIntentGenericPhrase(Normalized, ""))
     {
          return;
     }

     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty() || Tokens.size() > MaxTokens)
     {
          return;
     }

     if (Seen.insert(Normalized).second)
     {
          Values.push_back(Normalized);
     }
}

static std::vector<std::string> CollectYearHints(const Document& Doc, size_t MaxYears = 3)
{
     std::vector<std::string> Years;
     std::unordered_set<std::string> Seen;
     const std::string Corpus = Doc.Title + " " + Doc.Content + " " +
                                (Doc.Fields.count("description") ? Doc.Fields.at("description") : "");
     std::string Digits;

     auto TryAddYear = [&](const std::string& Candidate)
     {
          if (Candidate.size() != 4)
          {
               return;
          }

          const int Year = std::atoi(Candidate.c_str());

          if (Year < 1900 || Year > 2099)
          {
               return;
          }

          if (Seen.insert(Candidate).second)
          {
               Years.push_back(Candidate);
          }
     };

     for (char C : Corpus)
     {
          if (std::isdigit(static_cast<unsigned char>(C)))
          {
               Digits.push_back(C);
               continue;
          }

          TryAddYear(Digits);
          Digits.clear();

          if (Years.size() >= MaxYears)
          {
               break;
          }
     }

     if (Years.size() < MaxYears)
     {
          TryAddYear(Digits);
     }

     return Years;
}

static bool LooksAmbiguousSubject(const std::string& Subject)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(Subject);

     if (Tokens.empty())
     {
          return false;
     }

     if (Tokens.size() == 1)
     {
          return true;
     }

     return Tokens.size() == 2 && Tokens.front() == Tokens.back();
}

static std::vector<std::string> CollectDisambiguationFacts(const Document& Doc)
{
     std::vector<std::string> Facts;
     std::unordered_set<std::string> Seen;

     for (const auto& Value : CollectDynamicContextHints(Doc, 6, 4))
     {
          AppendUniqueFact(Facts, Seen, Value, 4);

          if (Facts.size() >= 6)
          {
               break;
          }
     }

     for (const auto& Value : CollectYearHints(Doc, 2))
     {
          AppendUniqueFact(Facts, Seen, Value, 1);
     }

     return Facts;
}

static void AppendCanonicalSnippetQueries(std::vector<SAM::TermEntry>& Terms,
                                          std::unordered_map<std::string, size_t>& IndexByTerm,
                                          const Document& Doc)
{
     const std::string Subject = NormalizeTerm(ResolveSubjectTitle(Doc));

     if (Subject.empty())
     {
          return;
     }

     const std::vector<std::string> Hints = CollectDynamicContextHints(Doc, 6, 4);
     const std::vector<std::string> Years = CollectYearHints(Doc, 2);

     for (const auto& Hint : Hints)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Hint, 0.78, 0.82);
     }

     for (const auto& Year : Years)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Year, 0.68, 0.72);
     }
}

static void AppendDisambiguationQueries(std::vector<SAM::TermEntry>& Terms,
                                        std::unordered_map<std::string, size_t>& IndexByTerm,
                                        const Document& Doc)
{
     const std::string Subject = NormalizeTerm(ResolveSubjectTitle(Doc));

     if (Subject.empty() || !LooksAmbiguousSubject(Subject))
     {
          return;
     }

     const std::vector<std::string> Facts = CollectDisambiguationFacts(Doc);

     for (const auto& Fact : Facts)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Fact, 0.82, 0.86);
     }

     if (Facts.size() >= 2)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Facts[0] + " " + Facts[1], 0.85, 0.89);
     }
}

static void AppendFactDrivenQueries(std::vector<SAM::TermEntry>& Terms,
                                    std::unordered_map<std::string, size_t>& IndexByTerm,
                                    const Document& Doc)
{
     const std::string Subject = NormalizeTerm(ResolveSubjectTitle(Doc));

     if (Subject.empty())
     {
          return;
     }

     const std::vector<std::string> Hints = CollectDynamicContextHints(Doc, 10, 5);
     const std::vector<std::string> Years = CollectYearHints(Doc, 2);

     for (const auto& Hint : Hints)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Hint, 0.74, 0.78);
     }

     for (const auto& Year : Years)
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Year, 0.66, 0.70);
     }
}

static bool IsLikelySeoStyleTerm(const std::string& Value)
{
     static const std::unordered_set<std::string> SeoTokens = {
          "best", "top", "ultimate", "complete", "guide", "guidebook", "explained",
          "overview", "review", "insights", "tips", "learn", "discover"};

     for (const auto& Token : TokenizeNormalized(Value))
     {
          if (SeoTokens.find(Token) != SeoTokens.end())
          {
               return true;
          }
     }

     return false;
}

static double ComputeWebQueryIntentBoost(const SAM::TermEntry& Term, const std::string& Subject)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(Term.Text);
     const std::string NormalizedSubject = NormalizeTerm(Subject);
     double Boost = 0.0;

     if (Term.Text == NormalizedSubject)
     {
          Boost += 0.12;
     }

     if (!NormalizedSubject.empty() && Term.Text.rfind(NormalizedSubject + " ", 0) == 0)
     {
          Boost += 0.08;
     }

     if (Term.Kind == "query")
     {
          Boost += 0.05;
     }

     if (Term.Source == "context_template")
     {
          Boost += 0.03;
     }

     if (Term.Kind == "subject" || Term.Kind == "synonym" || Term.Kind == "alias")
     {
          Boost += 0.04;
     }

     if (Tokens.size() >= 2 && Tokens.size() <= 4)
     {
          Boost += 0.05;
     }
     else if (Tokens.size() >= 5)
     {
          Boost -= 0.05;
     }

     if (IsLikelySeoStyleTerm(Term.Text))
     {
          Boost -= 0.07;
     }

     if (IsLowIntentGenericPhrase(Term.Text, Subject))
     {
          Boost -= 0.08;
     }

     return Boost;
}

static void ApplyWebQueryIntentRerank(std::vector<SAM::TermEntry>& Terms, const std::string& Subject)
{
     for (auto& Term : Terms)
     {
          const double Boost = ComputeWebQueryIntentBoost(Term, Subject);
          Term.Score = ClampSAMScore(Term.Score + Boost);
          Term.Signal = ClampSAMScore(Term.Signal + (Boost * 0.75));
     }
}

static void AppendTemplateQuery(std::vector<SAM::TermEntry>& Terms,
                                std::unordered_map<std::string, size_t>& IndexByTerm,
                                const std::string& Subject,
                                const std::string& Phrase,
                                double Score,
                                double Signal)
{
     if (Subject.empty() || Phrase.empty())
     {
          return;
     }

     AppendScoredTerm(Terms, IndexByTerm, Subject + " " + Phrase, "query", Score, "context_template", Signal);
}

static void AppendSearchContextTemplates(std::vector<SAM::TermEntry>& Terms,
                                         std::unordered_map<std::string, size_t>& IndexByTerm,
                                         const Document& Doc)
{
     const std::string Subject = NormalizeTerm(ResolveSubjectTitle(Doc));

     if (Subject.empty())
     {
          return;
     }

     for (const auto& Hint : CollectDynamicContextHints(Doc, 8, 4))
     {
          AppendTemplateQuery(Terms, IndexByTerm, Subject, Hint, 0.68, 0.72);
     }
}

static std::string BucketValueOrUnknown(const std::string& Value)
{
     return Value.empty() ? "unknown" : Value;
}

static std::vector<SAM::TermEntry> SelectDiversifiedTerms(const std::vector<SAM::TermEntry>& SortedTerms,
                                                          size_t MaxIdeas)
{
     if (SortedTerms.size() <= MaxIdeas)
     {
          return SortedTerms;
     }

     std::vector<SAM::TermEntry> Selected;
     Selected.reserve(MaxIdeas);
     std::vector<bool> Taken(SortedTerms.size(), false);
     std::unordered_map<std::string, size_t> SourceCounts;
     std::unordered_map<std::string, size_t> KindCounts;

     const auto TryTake = [&](size_t Index)
     {
          if (Taken[Index] || Selected.size() >= MaxIdeas)
          {
               return false;
          }

          Taken[Index] = true;
          Selected.push_back(SortedTerms[Index]);
          ++SourceCounts[BucketValueOrUnknown(SortedTerms[Index].Source)];
          ++KindCounts[BucketValueOrUnknown(SortedTerms[Index].Kind)];
          return true;
     };

     for (size_t Index = 0; Index < SortedTerms.size() && Selected.size() < MaxIdeas; ++Index)
     {
          const std::string SourceKey = BucketValueOrUnknown(SortedTerms[Index].Source);

          if (SourceCounts[SourceKey] == 0)
          {
               TryTake(Index);
          }
     }

     for (size_t Index = 0; Index < SortedTerms.size() && Selected.size() < MaxIdeas; ++Index)
     {
          const std::string KindKey = BucketValueOrUnknown(SortedTerms[Index].Kind);

          if (KindCounts[KindKey] == 0)
          {
               TryTake(Index);
          }
     }

     const size_t SourceCap = std::max<size_t>(1, (MaxIdeas + 2) / 3);
     const size_t KindCap = std::max<size_t>(1, (MaxIdeas + 1) / 2);

     for (size_t Index = 0; Index < SortedTerms.size() && Selected.size() < MaxIdeas; ++Index)
     {
          const std::string SourceKey = BucketValueOrUnknown(SortedTerms[Index].Source);
          const std::string KindKey = BucketValueOrUnknown(SortedTerms[Index].Kind);

          if (SourceCounts[SourceKey] >= SourceCap || KindCounts[KindKey] >= KindCap)
          {
               continue;
          }

          TryTake(Index);
     }

     for (size_t Index = 0; Index < SortedTerms.size() && Selected.size() < MaxIdeas; ++Index)
     {
          TryTake(Index);
     }

     return Selected;
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTerms(const std::string& Collection,
                                                  const Document& Doc,
                                                  std::string* ErrorMessage) const
{
     std::vector<TermEntry> Terms;

     if (ErrorMessage)
     {
          ErrorMessage->clear();
     }

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "LLM is not configured for SAM indexing.";
          }

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
          if (ErrorMessage)
          {
               *ErrorMessage = "LLM inference command is empty.";
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("sam",
                                     "GenerateLLMTerms: skipped for '" + Collection + "/" + Doc.ID +
                                          "' because inference_command is empty.");
          }

          return Terms;
     }

     const std::string& ModelPath = Instance->LLM->GetModelPath();

     if (ModelPath.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "LLM model path is empty.";
          }

          RecordDebugEvent(Collection, "LLM model path is empty for " + Doc.ID);
          return Terms;
     }

     std::error_code ModelPathError;

     if (!std::filesystem::exists(ModelPath, ModelPathError))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "LLM model file not found: " + ModelPath;
          }

          RecordDebugEvent(Collection, "LLM model file not found for " + Doc.ID + ": " + ModelPath);
          return Terms;
     }

     nlohmann::json Payload;
     Payload["collection"] = Collection;
     Payload["id"] = Doc.ID;
     Payload["title"] = Doc.Title;
     Payload["content"] = Doc.Content;
     Payload["fields"] = Doc.Fields;
     Payload["instruction"] =
          "Infer the document's domain from its title, content, and fields. Generate concise, multilingual-friendly lookup phrases a user might search to find this exact document. Use important field values, aliases, roles, places, dates, identifiers, and distinctive wording when present. Do not assume any collection-specific domain, gender, language, or topic unless it is supported by the document itself.";
     const int MaxIdeas = (Instance && Instance->Config)
          ? std::max(1, Instance->Config->GetSamLLMMaxIdeas())
          : 6;
     const std::string CreativityMode = (Instance && Instance->Config)
          ? Instance->Config->GetSamLLMCreativityMode()
          : "balanced";

     const auto StartedAt = std::chrono::steady_clock::now();
     std::lock_guard<std::mutex> Lock(InferenceMutex);
     RecordDebugEvent(Collection, "running LLM for " + Doc.ID);
     const std::string PayloadPath = WriteJSONPayloadTempFile(Payload);

     if (PayloadPath.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to create temporary LLM payload file.";
          }

          RecordDebugEvent(Collection, "failed to create LLM payload file for " + Doc.ID);
          return Terms;
     }

     setenv("HLQUERY_LLM_MODEL", ModelPath.c_str(), 1);
     setenv("HLQUERY_SAM_DOC_JSON_FILE", PayloadPath.c_str(), 1);
     setenv("HLQUERY_SAM_TERM_LIMIT", std::to_string(MaxIdeas).c_str(), 1);
     setenv("HLQUERY_SAM_CREATIVITY_MODE", CreativityMode.c_str(), 1);

     FILE* Pipe = popen(Command.c_str(), "r");

     if (!Pipe)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to start inference command: " + Command;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "GenerateLLMTerms: failed to start inference command for '" +
                                           Collection + "/" + Doc.ID + "': " + Command + ".");
          }

          unsetenv("HLQUERY_LLM_MODEL");
          unsetenv("HLQUERY_SAM_DOC_JSON_FILE");
          unsetenv("HLQUERY_SAM_TERM_LIMIT");
          unsetenv("HLQUERY_SAM_CREATIVITY_MODE");
          std::error_code IgnoreError;
          std::filesystem::remove(PayloadPath, IgnoreError);
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

          if (static_cast<int>(Terms.size()) >= MaxIdeas)
          {
               break;
          }
     }

     const int PipeStatus = pclose(Pipe);
     unsetenv("HLQUERY_LLM_MODEL");
     unsetenv("HLQUERY_SAM_DOC_JSON_FILE");
     unsetenv("HLQUERY_SAM_TERM_LIMIT");
     unsetenv("HLQUERY_SAM_CREATIVITY_MODE");
     std::error_code IgnoreError;
     std::filesystem::remove(PayloadPath, IgnoreError);

     const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - StartedAt).count();

     if (PipeStatus == -1)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Inference command close failed: " + Command;
          }

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
          if (ErrorMessage)
          {
               *ErrorMessage = "Inference command exited with status " +
                               std::to_string(WEXITSTATUS(PipeStatus)) + ": " + Command;
          }

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

     if (Terms.empty() && ErrorMessage && ErrorMessage->empty())
     {
          *ErrorMessage = "Inference produced no usable SAM terms.";
     }

     RecordDebugEvent(Collection,
                      "LLM produced " + std::to_string(Terms.size()) + " term(s) for " + Doc.ID +
                           " in " + std::to_string(ElapsedMs) + " ms");

     return Terms;
}

std::vector<SAM::TermEntry> SAM::ExpandDocumentTerms(const std::string& Collection,
                                                     const Document& Doc,
                                                     std::string* ErrorMessage) const
{
     std::vector<TermEntry> Terms = GenerateLLMTerms(Collection, Doc, ErrorMessage);
     std::unordered_map<std::string, size_t> IndexByTerm;

     for (size_t Index = 0; Index < Terms.size(); ++Index)
     {
          IndexByTerm[Terms[Index].Text] = Index;
     }

     const std::string Subject = ResolveSubjectTitle(Doc);

     if (!Subject.empty())
     {
          const std::string NormalizedSubject = NormalizeTerm(Subject);
          AppendScoredTerm(Terms, IndexByTerm, NormalizedSubject, "subject", 0.88, "title", 0.90);

          AppendStructuredPhraseWindows(Terms, IndexByTerm, NormalizedSubject, "subject_window", 0.76, 0.80, 4);
     }

     if (!Doc.Title.empty())
     {
          AppendStructuredPhraseWindows(Terms, IndexByTerm, Doc.Title, "title_window", 0.78, 0.82, 4);
     }

     const auto DescriptionIt = Doc.Fields.find("description");

     if (DescriptionIt != Doc.Fields.end() && !DescriptionIt->second.empty())
     {
          AppendStructuredPhraseWindows(Terms, IndexByTerm, DescriptionIt->second, "description_window", 0.66, 0.70, 4);
     }

     if (!Doc.Content.empty())
     {
          AppendStructuredPhraseWindows(Terms, IndexByTerm, TruncateForContextWindows(Doc.Content), "content_window", 0.60, 0.64, 4);
     }

     const auto LabelsIt = Doc.Fields.find("labels");

     if (LabelsIt != Doc.Fields.end() && !LabelsIt->second.empty())
     {
          for (const auto& Label : ExtractArrayishValues(LabelsIt->second))
          {
               const std::string NormalizedLabel = NormalizeTerm(Label);

               if (IsLowIntentGenericPhrase(NormalizedLabel, ""))
               {
                    continue;
               }

               AppendScoredTerm(Terms, IndexByTerm, NormalizedLabel, "descriptor", 0.72, "label", 0.76);
               AppendStructuredPhraseWindows(Terms, IndexByTerm, Label, "label_window", 0.70, 0.74, 3);
          }
     }

     AppendSearchContextTemplates(Terms, IndexByTerm, Doc);
     AppendCanonicalSnippetQueries(Terms, IndexByTerm, Doc);
     AppendFactDrivenQueries(Terms, IndexByTerm, Doc);
     AppendDisambiguationQueries(Terms, IndexByTerm, Doc);
     AppendCompressedStopwordVariants(Terms, IndexByTerm);
     AppendReorderedAliases(Terms, IndexByTerm, Collection);
     ApplyWebQueryIntentRerank(Terms, Subject);

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

     const size_t MaxIdeas = (Instance && Instance->Config)
          ? static_cast<size_t>(std::max(4, Instance->Config->GetSamContextMaxIdeas()))
          : static_cast<size_t>(20);

     // Preserve score ordering, but reserve space for distinct term families first.
     return SelectDiversifiedTerms(Terms, MaxIdeas);
}
