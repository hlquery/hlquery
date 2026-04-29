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
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hlquery.h"
#include "search/sam/sam.h"
#include "vendor/json/json.hpp"

static std::string TruncateForContextWindows(const std::string& Value, size_t MaxChars = 360);
static std::vector<std::string> ExtractArrayishValues(const std::string& Raw);
static bool IsContextFieldName(const std::string& FieldName);
static bool IsLowIntentGenericPhrase(const std::string& Value, const std::string& Subject);
static void AppendScoredTerm(std::vector<SAM::TermEntry>& Target,
                             std::unordered_map<std::string, size_t>& IndexByTerm,
                             const std::string& Value,
                             const std::string& Kind,
                             double Score,
                             const std::string& Source,
                             double Signal);
static void AppendStructuredPhraseWindows(std::vector<SAM::TermEntry>& Terms,
                                          std::unordered_map<std::string, size_t>& IndexByTerm,
                                          const std::string& RawText,
                                          const std::string& Source,
                                          double BaseScore,
                                          double BaseSignal,
                                          size_t MaxWindow);

static bool ShouldLogSAMContext()
{
     return Instance && Instance->Config && Instance->Config->GetSamLogContext() &&
            Instance->Logs;
}

static void LogSAMContext(const std::string& Collection,
                          const std::string& DocumentID,
                          const std::string& Message)
{
     if (!ShouldLogSAMContext())
     {
          return;
     }

     Instance->Logs->Debug("sam",
                           "context [" + Collection + "/" + DocumentID + "] " + Message);
}

static std::string FormatSAMTermsForLog(const std::vector<SAM::TermEntry>& Terms)
{
     nlohmann::json Root = nlohmann::json::array();

     for (const auto& Term : Terms)
     {
          Root.push_back({
               {"text", Term.Text},
               {"kind", Term.Kind},
               {"source", Term.Source},
               {"score", Term.Score},
               {"signal", Term.Signal}
          });
     }

     return Root.dump();
}

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
          "these", "those", "their", "there", "here", "brief", "guide",
          "benchmark", "fake", "dummy", "sample", "placeholder", "mock", "test"};
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

static bool IsNarrativeDriftToken(const std::string& Token)
{
     static const std::unordered_set<std::string> DriftTokens = {
          "additional", "commentary", "follow", "follows",
          "flow", "finished", "how", "look", "looks", "material",
          "move", "moves", "moving", "piece", "related", "shape", "tradeoff",
          "tradeoffs", "understanding", "working", "writing"};
     return DriftTokens.find(Token) != DriftTokens.end();
}

static bool IsStrongPhraseToken(const std::string& Token)
{
     return !(Token.empty() || IsSamStopword(Token) || IsWeakSamToken(Token) || IsGenericDescriptorToken(Token));
}

static bool IsPhraseWindowCandidate(const std::vector<std::string>& Tokens)
{
     if (Tokens.size() < 2)
     {
          return false;
     }

     size_t AnchorCount = 0;
     size_t DriftCount = 0;

     for (const auto& Token : Tokens)
     {
          if (IsStrongPhraseToken(Token) && !IsNarrativeDriftToken(Token))
          {
               ++AnchorCount;
          }

          if (IsNarrativeDriftToken(Token))
          {
               ++DriftCount;
          }
     }

     if (AnchorCount < 2)
     {
          return false;
     }

     if (DriftCount >= Tokens.size() / 2 + 1)
     {
          return false;
     }

     if (IsNarrativeDriftToken(Tokens.front()) || IsNarrativeDriftToken(Tokens.back()))
     {
          return false;
     }

     return true;
}

struct CollectionProfileCandidate
{
     std::string Text;
     size_t DocFrequency = 0;
     size_t TermFrequency = 0;
};

struct CollectionProfile
{
     std::vector<std::string> Terms;
};

static bool IsCollectionProfilePhrase(const std::string& Value)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(Value);

     if (Tokens.empty() || Tokens.size() > 3)
     {
          return false;
     }

     size_t StrongCount = 0;

     for (const auto& Token : Tokens)
     {
          if (Token.size() < 3)
          {
               return false;
          }

          if (IsStrongPhraseToken(Token))
          {
               ++StrongCount;
          }
     }

     return StrongCount == Tokens.size();
}

static void AccumulateCollectionProfileText(const std::string& Text,
                                           std::unordered_map<std::string, CollectionProfileCandidate>& Ranked,
                                           std::unordered_set<std::string>& SeenInDoc)
{
     const std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Text));

     if (Tokens.empty())
     {
          return;
     }

     auto AddCandidate = [&](const std::string& Candidate)
     {
          if (!IsCollectionProfilePhrase(Candidate))
          {
               return;
          }

          CollectionProfileCandidate& Entry = Ranked[Candidate];
          Entry.Text = Candidate;
          ++Entry.TermFrequency;

          if (SeenInDoc.insert(Candidate).second)
          {
               ++Entry.DocFrequency;
          }
     };

     for (size_t Index = 0; Index < Tokens.size(); ++Index)
     {
          if (!IsStrongPhraseToken(Tokens[Index]))
          {
               continue;
          }

          AddCandidate(Tokens[Index]);

          if (Index + 1 < Tokens.size() &&
              IsStrongPhraseToken(Tokens[Index + 1]))
          {
               AddCandidate(Tokens[Index] + " " + Tokens[Index + 1]);
          }

          if (Index + 2 < Tokens.size() &&
              IsStrongPhraseToken(Tokens[Index + 1]) &&
              IsStrongPhraseToken(Tokens[Index + 2]))
          {
               AddCandidate(Tokens[Index] + " " + Tokens[Index + 1] + " " + Tokens[Index + 2]);
          }
     }
}

static std::string BuildDocumentProfileEvidence(const Document& Doc)
{
     std::string Evidence = Doc.Title;

     if (!Doc.Content.empty())
     {
          if (!Evidence.empty())
          {
               Evidence.push_back(' ');
          }

          Evidence += TruncateForContextWindows(Doc.Content, 720);
     }

     for (const auto& Field : Doc.Fields)
     {
          if (Field.second.empty())
          {
               continue;
          }

          if (!Evidence.empty())
          {
               Evidence.push_back(' ');
          }

          Evidence += Field.second;
     }

     return NormalizeTerm(Evidence);
}

static void AppendFieldFactTerms(std::vector<SAM::TermEntry>& Terms,
                                 std::unordered_map<std::string, size_t>& IndexByTerm,
                                 const Document& Doc)
{
     for (const auto& Pair : Doc.Fields)
     {
          if (!IsContextFieldName(Pair.first))
          {
               continue;
          }

          for (const auto& Value : ExtractArrayishValues(Pair.second))
          {
               const std::string NormalizedValue = NormalizeTerm(Value);

               if (IsLowIntentGenericPhrase(NormalizedValue, ""))
               {
                    continue;
               }

               AppendScoredTerm(Terms, IndexByTerm, NormalizedValue, "descriptor", 0.74, "field", 0.78);
               AppendStructuredPhraseWindows(Terms, IndexByTerm, Value, "field_window", 0.72, 0.76, 5);
          }
     }
}

static CollectionProfile BuildCollectionProfile(const std::string& Collection,
                                                const std::string& CurrentDocumentID)
{
     CollectionProfile Profile;
     std::unordered_map<std::string, CollectionProfileCandidate> Ranked;
     const size_t DocumentCount = HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);
     const int SampleSize = static_cast<int>(std::min<size_t>(24, std::max<size_t>(6, DocumentCount)));

     if (SampleSize <= 0)
     {
          return Profile;
     }

     const std::vector<Document> Docs = HybridStorageManager::GetInstance().ListDocuments(Collection, SampleSize, 0);

     for (const auto& SampleDoc : Docs)
     {
          if (!CurrentDocumentID.empty() && SampleDoc.ID == CurrentDocumentID)
          {
               continue;
          }

          std::unordered_set<std::string> SeenInDoc;
          AccumulateCollectionProfileText(SampleDoc.Title, Ranked, SeenInDoc);
          AccumulateCollectionProfileText(TruncateForContextWindows(SampleDoc.Content, 520), Ranked, SeenInDoc);

          for (const auto& Field : SampleDoc.Fields)
          {
               AccumulateCollectionProfileText(Field.second, Ranked, SeenInDoc);
          }
     }

     std::vector<CollectionProfileCandidate> Candidates;
     Candidates.reserve(Ranked.size());

     for (const auto& Pair : Ranked)
     {
          const CollectionProfileCandidate& Candidate = Pair.second;

          if (Candidate.DocFrequency < 2)
          {
               continue;
          }

          Candidates.push_back(Candidate);
     }

     std::sort(Candidates.begin(), Candidates.end(),
               [](const CollectionProfileCandidate& A, const CollectionProfileCandidate& B)
               {
                    if (A.DocFrequency != B.DocFrequency)
                    {
                         return A.DocFrequency > B.DocFrequency;
                    }

                    if (A.TermFrequency != B.TermFrequency)
                    {
                         return A.TermFrequency > B.TermFrequency;
                    }

                    if (A.Text.size() != B.Text.size())
                    {
                         return A.Text.size() < B.Text.size();
                    }

                    return A.Text < B.Text;
               });

     for (const auto& Candidate : Candidates)
     {
          bool Covered = false;

          for (const auto& Existing : Profile.Terms)
          {
               if (Existing == Candidate.Text ||
                   Existing.find(Candidate.Text) != std::string::npos ||
                   Candidate.Text.find(Existing) != std::string::npos)
               {
                    Covered = true;
                    break;
               }
          }

          if (Covered)
          {
               continue;
          }

          Profile.Terms.push_back(Candidate.Text);

          if (Profile.Terms.size() >= 8)
          {
               break;
          }
     }

     return Profile;
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

     size_t DriftCount = 0;

     for (const auto& Token : Tokens)
     {
          if (IsNarrativeDriftToken(Token))
          {
               ++DriftCount;
          }
     }

     if (DriftCount >= Tokens.size() / 2 + 1)
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

static std::string TruncateForContextWindows(const std::string& Value, size_t MaxChars)
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

               if (!IsPhraseWindowCandidate(Slice))
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

static bool TokenLooksNumericLike(const std::string& Token)
{
     return !Token.empty() &&
            std::all_of(Token.begin(), Token.end(),
                        [](unsigned char C)
                        {
                             return std::isdigit(C) != 0;
                        });
}

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

     const std::vector<std::string> SubjectTokens = TokenizeNormalized(Subject);
     const std::vector<std::string> PhraseTokens = TokenizeNormalized(Phrase);

     if (PhraseTokens.empty())
     {
          return;
     }

     std::unordered_set<std::string> SubjectTokenSet(SubjectTokens.begin(), SubjectTokens.end());
     std::vector<std::string> NewTokens;

     for (const auto& Token : PhraseTokens)
     {
          if (SubjectTokenSet.find(Token) == SubjectTokenSet.end())
          {
               NewTokens.push_back(Token);
          }
     }

     size_t StrongNewTokens = 0;
     bool HasNumericAnchor = false;

     for (const auto& Token : NewTokens)
     {
          if (IsStrongPhraseToken(Token))
          {
               ++StrongNewTokens;
          }

          if (TokenLooksNumericLike(Token))
          {
               HasNumericAnchor = true;
          }
     }

     if (StrongNewTokens == 0 && !HasNumericAnchor)
     {
          return;
     }

     if (StrongNewTokens < 2 && !HasNumericAnchor)
     {
          return;
     }

     const std::string Combined = Subject + " " + Phrase;

     if (HasDuplicateTokens(Combined) || IsLowIntentGenericPhrase(Combined, Subject))
     {
          return;
     }

     AppendScoredTerm(Terms, IndexByTerm, Combined, "query", Score, "context_template", Signal);
}

static void PruneRedundantSAMTerms(std::vector<SAM::TermEntry>& Terms)
{
     if (Terms.empty())
     {
          return;
     }

     std::vector<SAM::TermEntry> Pruned;
     Pruned.reserve(Terms.size());

     for (const auto& Term : Terms)
     {
          bool Redundant = false;
          const std::vector<std::string> CandidateTokens = TokenizeNormalized(Term.Text);

          for (const auto& Kept : Pruned)
          {
               const std::vector<std::string> KeptTokens = TokenizeNormalized(Kept.Text);
               size_t Shared = 0;

               for (const auto& Token : CandidateTokens)
               {
                    if (std::find(KeptTokens.begin(), KeptTokens.end(), Token) != KeptTokens.end())
                    {
                         ++Shared;
                    }
               }

               const bool SamePhraseFamily =
                    !CandidateTokens.empty() &&
                    Shared >= CandidateTokens.size() &&
                    Kept.Score >= Term.Score;
               const bool TemplateShadowed =
                    Term.Source == "context_template" &&
                    Shared >= std::min(CandidateTokens.size(), KeptTokens.size()) &&
                    Kept.Score >= (Term.Score - 0.03);

               if (SamePhraseFamily || TemplateShadowed)
               {
                    Redundant = true;
                    break;
               }
          }

          if (!Redundant)
          {
               Pruned.push_back(Term);
          }
     }

     Terms.swap(Pruned);
}

static std::unordered_set<std::string> BuildTokenSet(const std::string& Value)
{
     std::unordered_set<std::string> Tokens;

     for (const auto& Token : TokenizeNormalized(Value))
     {
          if (!Token.empty())
          {
               Tokens.insert(Token);
          }
     }

     return Tokens;
}

static double ComputeCandidateEvidenceSupport(const SAM::TermEntry& Term,
                                             const std::unordered_set<std::string>& EvidenceTokens,
                                             const std::unordered_set<std::string>& ProfileTokens,
                                             const std::string& Subject)
{
     const std::vector<std::string> CandidateTokens = TokenizeNormalized(Term.Text);

     if (CandidateTokens.empty())
     {
          return -0.18;
     }

     size_t EvidenceMatches = 0;
     size_t ProfileMatches = 0;
     size_t StrongTokens = 0;
     bool HasNumericAnchor = false;

     for (const auto& Token : CandidateTokens)
     {
          if (EvidenceTokens.find(Token) != EvidenceTokens.end())
          {
               ++EvidenceMatches;
          }

          if (ProfileTokens.find(Token) != ProfileTokens.end())
          {
               ++ProfileMatches;
          }

          if (IsStrongPhraseToken(Token))
          {
               ++StrongTokens;
          }

          if (TokenLooksNumericLike(Token))
          {
               HasNumericAnchor = true;
          }
     }

     double Support = 0.0;
     Support += std::min(0.22, static_cast<double>(EvidenceMatches) * 0.06);
     Support += std::min(0.10, static_cast<double>(ProfileMatches) * 0.03);

     if (Term.Kind == "query")
     {
          Support += 0.03;
     }

     if (Term.Kind == "subject" || Term.Kind == "synonym" || Term.Kind == "alias")
     {
          Support += 0.04;
     }

     if (StrongTokens == 0 && !HasNumericAnchor)
     {
          Support -= 0.16;
     }
     else if (StrongTokens == 1 && CandidateTokens.size() >= 3 && !HasNumericAnchor)
     {
          Support -= 0.08;
     }

     if (CandidateTokens.size() >= 6)
     {
          Support -= 0.05;
     }

     if (IsLowIntentGenericPhrase(Term.Text, Subject))
     {
          Support -= 0.12;
     }

     return Support;
}

static std::vector<std::string> BuildInternalImprovementQuestions(const Document& Doc,
                                                                  const std::string& Subject,
                                                                  const std::vector<std::string>& ProfileTerms)
{
     std::vector<std::string> Questions;
     Questions.push_back("Are the current descriptors too close to the title or description wording?");
     Questions.push_back("Which outside-in discovery phrases would help someone find this document without knowing its title?");

     const auto LabelsIt = Doc.Fields.find("labels");
     if (LabelsIt != Doc.Fields.end() && !LabelsIt->second.empty())
     {
          Questions.push_back("Did we use the strongest non-generic labels and ignore synthetic benchmark-like labels?");
     }
     else
     {
          Questions.push_back("Which field facts or structured metadata are still underused?");
     }

     if (!Subject.empty() && LooksAmbiguousSubject(NormalizeTerm(Subject)))
     {
          Questions.push_back("Is the subject ambiguous enough that we should add stronger disambiguation facts?");
     }
     else if (!ProfileTerms.empty())
     {
          Questions.push_back("Which collection-supported terms are genuinely backed by this document and worth keeping?");
     }

     return Questions;
}

static void RefineInternalSAMTerms(std::vector<SAM::TermEntry>& Terms,
                                   const std::string& DocumentEvidence,
                                   const std::vector<std::string>& ProfileTerms,
                                   const std::string& Subject)
{
     const std::unordered_set<std::string> EvidenceTokens = BuildTokenSet(DocumentEvidence);
     std::unordered_set<std::string> ProfileTokens;

     for (const auto& Term : ProfileTerms)
     {
          const std::vector<std::string> Tokens = TokenizeNormalized(Term);
          ProfileTokens.insert(Tokens.begin(), Tokens.end());
     }

     for (auto& Term : Terms)
     {
          const double Support = ComputeCandidateEvidenceSupport(Term, EvidenceTokens, ProfileTokens, Subject);
          Term.Score = ClampSAMScore(Term.Score + Support);
          Term.Signal = ClampSAMScore(Term.Signal + (Support * 0.85));
     }

     Terms.erase(std::remove_if(Terms.begin(), Terms.end(),
                                [&](const SAM::TermEntry& Term)
                                {
                                     if (Term.Score < 0.40 || Term.Signal < 0.38)
                                     {
                                          return true;
                                     }

                                     if (Term.Kind == "descriptor" &&
                                         IsLowIntentGenericPhrase(Term.Text, Subject) &&
                                         Term.Score < 0.62)
                                     {
                                          return true;
                                     }

                                     return false;
                                }),
                 Terms.end());
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

std::vector<SAM::TermEntry> SAM::GenerateLLMTermsFromProfile(const std::string& Collection,
                                                             const Document& Doc,
                                                             const std::vector<std::string>& ProfileTerms,
                                                             std::string* ErrorMessage) const
{
     std::vector<TermEntry> Terms;

     if (ErrorMessage)
     {
          ErrorMessage->clear();
     }

     const int MaxIdeas = (Instance && Instance->Config)
          ? std::max(4, Instance->Config->GetSamLLMMaxIdeas())
          : 8;
     const auto StartedAt = std::chrono::steady_clock::now();
     std::unordered_map<std::string, size_t> IndexByTerm;
     const std::string Subject = ResolveSubjectTitle(Doc);
     const std::string DocumentEvidence = BuildDocumentProfileEvidence(Doc);

     if (ShouldLogSAMContext())
     {
          nlohmann::json ContextSummary;
          ContextSummary["collection"] = Collection;
          ContextSummary["id"] = Doc.ID;
          ContextSummary["title"] = Doc.Title;
          ContextSummary["collection_profile"] = ProfileTerms;
          LogSAMContext(Collection, Doc.ID, "internal_reasoning_context=" + ContextSummary.dump());
     }

     const std::vector<std::string> ImprovementQuestions =
          BuildInternalImprovementQuestions(Doc, Subject, ProfileTerms);

     if (!Subject.empty())
     {
          AppendScoredTerm(Terms, IndexByTerm, NormalizeTerm(Subject), "subject", 0.84, "internal_subject", 0.88);
          AppendStructuredPhraseWindows(Terms, IndexByTerm, Subject, "internal_subject_window", 0.76, 0.80, 4);
     }

     if (!Doc.Title.empty())
     {
          AppendStructuredPhraseWindows(Terms, IndexByTerm, Doc.Title, "internal_title_window", 0.74, 0.78, 4);
     }

     const auto DescriptionIt = Doc.Fields.find("description");

     if (DescriptionIt != Doc.Fields.end() && !DescriptionIt->second.empty())
     {
          AppendStructuredPhraseWindows(Terms, IndexByTerm, DescriptionIt->second, "internal_description_window", 0.68, 0.72, 4);
     }

     const auto LabelsIt = Doc.Fields.find("labels");

     if (LabelsIt != Doc.Fields.end() && !LabelsIt->second.empty())
     {
          for (const auto& Label : ExtractArrayishValues(LabelsIt->second))
          {
               const std::string NormalizedLabel = NormalizeTerm(Label);

               if (IsLowIntentGenericPhrase(NormalizedLabel, Subject))
               {
                    continue;
               }

               AppendScoredTerm(Terms, IndexByTerm, NormalizedLabel, "descriptor", 0.74, "internal_label", 0.78);
               AppendStructuredPhraseWindows(Terms, IndexByTerm, Label, "internal_label_window", 0.70, 0.74, 3);
          }
     }

     for (const auto& ProfileTerm : ProfileTerms)
     {
          const std::string NormalizedProfileTerm = NormalizeTerm(ProfileTerm);
          const std::string Needle = " " + NormalizedProfileTerm + " ";
          const std::string Haystack = " " + DocumentEvidence + " ";

          if (NormalizedProfileTerm.empty() || Needle.size() <= 2 || Haystack.find(Needle) == std::string::npos)
          {
               continue;
          }

          AppendScoredTerm(Terms, IndexByTerm, NormalizedProfileTerm, "collection_context", 0.66, "internal_collection_profile", 0.70);
     }

     AppendFieldFactTerms(Terms, IndexByTerm, Doc);
     AppendSearchContextTemplates(Terms, IndexByTerm, Doc);
     AppendCanonicalSnippetQueries(Terms, IndexByTerm, Doc);
     AppendFactDrivenQueries(Terms, IndexByTerm, Doc);
     AppendDisambiguationQueries(Terms, IndexByTerm, Doc);
     RefineInternalSAMTerms(Terms, DocumentEvidence, ProfileTerms, Subject);

     if (Terms.size() < static_cast<size_t>(std::max(3, MaxIdeas / 2)))
     {
          AppendSearchContextTemplates(Terms, IndexByTerm, Doc);
          AppendFactDrivenQueries(Terms, IndexByTerm, Doc);
          AppendCanonicalSnippetQueries(Terms, IndexByTerm, Doc);
          RefineInternalSAMTerms(Terms, DocumentEvidence, ProfileTerms, Subject);
     }

     PruneRedundantSAMTerms(Terms);

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

     Terms = SelectDiversifiedTerms(Terms, static_cast<size_t>(MaxIdeas));

     const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - StartedAt).count();

     if (ShouldLogSAMContext())
     {
          nlohmann::json ReasoningSummary;
          ReasoningSummary["mode"] = "internal";
          ReasoningSummary["improvement_questions"] = ImprovementQuestions;
          LogSAMContext(Collection, Doc.ID, "internal_reasoning=" + ReasoningSummary.dump());
     }

     LogSAMContext(Collection, Doc.ID, "internal_terms=" + FormatSAMTermsForLog(Terms));
     RecordDebugEvent(Collection,
                      "Internal SAM reasoning produced " + std::to_string(Terms.size()) +
                           " term(s) for " + Doc.ID + " in " + std::to_string(ElapsedMs) + " ms");

     return Terms;
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTerms(const std::string& Collection,
                                                  const Document& Doc,
                                                  std::string* ErrorMessage) const
{
     const CollectionProfile Profile = BuildCollectionProfile(Collection, Doc.ID);
     return GenerateLLMTermsFromProfile(Collection, Doc, Profile.Terms, ErrorMessage);
}

std::vector<SAM::TermEntry> SAM::ExpandDocumentTerms(const std::string& Collection,
                                                     const Document& Doc,
                                                     std::string* ErrorMessage) const
{
     const CollectionProfile Profile = BuildCollectionProfile(Collection, Doc.ID);
     std::vector<TermEntry> Terms = GenerateLLMTermsFromProfile(Collection, Doc, Profile.Terms, ErrorMessage);

     if (ShouldLogSAMContext())
     {
          nlohmann::json DocContext;
          DocContext["collection"] = Collection;
          DocContext["id"] = Doc.ID;
          DocContext["title"] = Doc.Title;
          DocContext["content"] = Doc.Content;
          DocContext["fields"] = Doc.Fields;
          DocContext["collection_profile"] = Profile.Terms;
          LogSAMContext(Collection, Doc.ID, "document_context=" + DocContext.dump());
     }

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

     AppendFieldFactTerms(Terms, IndexByTerm, Doc);

     const std::string DocumentEvidence = BuildDocumentProfileEvidence(Doc);

     for (const auto& ProfileTerm : Profile.Terms)
     {
          const std::string Needle = " " + NormalizeTerm(ProfileTerm) + " ";
          const std::string Haystack = " " + DocumentEvidence + " ";

          if (Needle.size() <= 2 || Haystack.find(Needle) == std::string::npos)
          {
               continue;
          }

          AppendScoredTerm(Terms, IndexByTerm, ProfileTerm, "collection_context", 0.61, "collection_profile", 0.66);
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
     PruneRedundantSAMTerms(Terms);

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
     std::vector<TermEntry> SelectedTerms = SelectDiversifiedTerms(Terms, MaxIdeas);
     LogSAMContext(Collection, Doc.ID, "expanded_terms=" + FormatSAMTermsForLog(SelectedTerms));
     return SelectedTerms;
}
