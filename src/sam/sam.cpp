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
#include "common/cryptoutils.h"
#include "sam/lang.h"
#include "sam/sam.h"
#include "sam/sam_internal.h"
#include "search/storageengine.h"
#include "utils/tools.h"
#include "utils/wildcard.h"
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
          "biography", "discography", "benchmark", "fake", "recognized",
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

std::string JoinValues(const std::vector<std::string>& Values,
                       const std::string& Separator)
{
     std::string Result;

     for (size_t Index = 0; Index < Values.size(); ++Index)
     {
          if (Index > 0)
          {
               Result += Separator;
          }

          Result += Values[Index];
     }

     return Result;
}

std::string ToLowerASCII(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

std::string TrimLowerCopy(const std::string& Value)
{
     return ToLowerASCII(TrimCopy(Value));
}

std::string TruncateSAMDocumentText(const std::string& Value, size_t MaxChars)
{
     if (Value.size() <= MaxChars)
     {
          return Value;
     }

     return Value.substr(0, MaxChars);
}

std::vector<std::pair<std::string, std::string>> CollectDocumentTextFields(const Document& Doc)
{
     std::vector<std::pair<std::string, std::string>> Fields;

     if (!Doc.Title.empty())
     {
          Fields.push_back({"title", Doc.Title});
     }

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

          Fields.push_back({TrimLowerCopy(Entry.first), Entry.second});
     }

     return Fields;
}

std::string BuildSAMSourceDocumentFingerprint(const Document& Doc)
{
     nlohmann::json Root;
     Root["id"] = Doc.ID;
     Root["title"] = Doc.Title;
     Root["content"] = Doc.Content;
     Root["score"] = Doc.Score;
     Root["timestamp"] = Doc.Timestamp;

     nlohmann::json Fields = nlohmann::json::object();

     for (const auto& Entry : Doc.Fields)
     {
          Fields[Entry.first] = Entry.second;
     }

     Root["fields"] = std::move(Fields);
     const std::string Serialized = Root.dump();
     return Hex(SHA256(Serialized.data(), Serialized.size()));
}

bool IsSAMDocumentEntryCurrent(const SAM::DocumentEntry& Entry,
                               std::unordered_map<std::string, bool>* Cache = nullptr)
{
     if (Entry.Collection.empty() || Entry.DocumentID.empty())
     {
          return false;
     }

     const std::string CacheKey = Entry.Collection + "\n" + Entry.DocumentID + "\n" + Entry.SourceFingerprint;

     if (Cache)
     {
          auto It = Cache->find(CacheKey);

          if (It != Cache->end())
          {
               return It->second;
          }
     }

     const Document SourceDoc = HybridStorageManagerInstance().GetDocument(Entry.Collection, Entry.DocumentID);
     bool Current = !SourceDoc.ID.empty();

     if (Current && !Entry.SourceFingerprint.empty())
     {
          Current = (BuildSAMSourceDocumentFingerprint(SourceDoc) == Entry.SourceFingerprint);
     }
     else if (Current && Entry.SourceTimestamp != 0)
     {
          Current = (SourceDoc.Timestamp == Entry.SourceTimestamp);
     }

     if (Cache)
     {
          (*Cache)[CacheKey] = Current;
     }

     return Current;
}

std::string DetectSAMDocumentLabel(const Document& Doc)
{
     const std::vector<std::pair<std::string, std::string>> TextFields = CollectDocumentTextFields(Doc);
     std::string Combined;
     size_t NonEmptyFieldCount = 0;
     size_t ShortFieldCount = 0;
     bool HasProfileSignals = false;
     bool HasReferenceSignals = false;
     bool HasListingSignals = false;

     for (const auto& Entry : TextFields)
     {
          if (Entry.second.empty())
          {
               continue;
          }

          ++NonEmptyFieldCount;

          if (Entry.second.size() <= 80)
          {
               ++ShortFieldCount;
          }

          if (!Combined.empty())
          {
               Combined.push_back(' ');
          }

          Combined += Entry.first;
          Combined.push_back(' ');
          Combined += Entry.second;

          if (Combined.size() >= 4096)
          {
               Combined.resize(4096);
               break;
          }
     }

     const std::string Lower = ToLowerASCII(Combined);

     for (const auto& Entry : Doc.Fields)
     {
          const std::string Key = TrimLowerCopy(Entry.first);
          const std::string Value = TrimLowerCopy(Entry.second);

          if (Key == "author" || Key == "role" || Key == "occupation" || Key == "bio" ||
              Key == "biography" || Key == "artist" || Key == "person")
          {
               HasProfileSignals = true;
          }

          if (Key == "sku" || Key == "isbn" || Key == "version" || Key == "api" ||
              Key == "endpoint" || Key == "spec" || Key == "reference")
          {
               HasReferenceSignals = true;
          }

          if (Key == "tags" || Key == "labels" || Key == "category" || Key == "categories")
          {
               HasListingSignals = HasListingSignals || Value.find(',') != std::string::npos;
          }
     }

     if (Lower.find(" biography ") != std::string::npos || Lower.find(" profile ") != std::string::npos ||
         Lower.find(" born ") != std::string::npos || Lower.find(" founded ") != std::string::npos ||
         Lower.find(" nacido ") != std::string::npos || Lower.find(" biografia ") != std::string::npos)
     {
          HasProfileSignals = true;
     }

     if (Lower.find(" reference ") != std::string::npos || Lower.find(" specification ") != std::string::npos ||
         Lower.find(" api ") != std::string::npos || Lower.find(" endpoint ") != std::string::npos ||
         Lower.find(" schema ") != std::string::npos || Lower.find(" manual ") != std::string::npos)
     {
          HasReferenceSignals = true;
     }

     if (Lower.find(" list of ") != std::string::npos || Lower.find(" top ") != std::string::npos ||
         Lower.find(" collection ") != std::string::npos || Lower.find(" catalog ") != std::string::npos ||
         Lower.find(" listado ") != std::string::npos || Lower.find(" lista de ") != std::string::npos)
     {
          HasListingSignals = true;
     }

     if (HasProfileSignals)
     {
          return "profile";
     }

     if (HasReferenceSignals)
     {
          return "reference";
     }

     if (HasListingSignals || (NonEmptyFieldCount >= 5 && ShortFieldCount + 1 >= NonEmptyFieldCount))
     {
          return "listing";
     }

     return "article";
}

std::string DetectSAMDocumentFormat(const Document& Doc)
{
     std::string Combined = Doc.Title;

     if (!Doc.Content.empty())
     {
          if (!Combined.empty())
          {
               Combined.push_back('\n');
          }

          Combined += TruncateSAMDocumentText(Doc.Content, 2000);
     }

     for (const auto& Entry : Doc.Fields)
     {
          if (Entry.second.empty())
          {
               continue;
          }

          if (!Combined.empty())
          {
               Combined.push_back('\n');
          }

          Combined += Entry.second;
     }

     const std::string Trimmed = TrimCopy(Combined);
     const std::string Lower = ToLowerASCII(Trimmed);

     if (Trimmed.empty())
     {
          return "text";
     }

     if ((Lower.find("<html") != std::string::npos ||
          Lower.find("<body") != std::string::npos ||
          Lower.find("<div") != std::string::npos ||
          Lower.find("<p>") != std::string::npos ||
          Lower.find("<a ") != std::string::npos) &&
         Lower.find('>') != std::string::npos)
     {
          return "html";
     }

     if ((Lower.find("<?xml") != std::string::npos ||
          Lower.find("<rss") != std::string::npos ||
          Lower.find("<feed") != std::string::npos) &&
         Lower.find('>') != std::string::npos)
     {
          return "xml";
     }

     if ((!Trimmed.empty() && (Trimmed.front() == '{' || Trimmed.front() == '[')) ||
         Lower.find("\"id\"") != std::string::npos ||
         Lower.find("\":") != std::string::npos)
     {
          return "json";
     }

     if (Lower.find("```") != std::string::npos ||
         Lower.find("#include") != std::string::npos ||
         Lower.find("function ") != std::string::npos ||
         Lower.find("class ") != std::string::npos)
     {
          return "code";
     }

     if (Lower.find("# ") != std::string::npos ||
         Lower.find("## ") != std::string::npos ||
         Lower.find("- ") != std::string::npos ||
         Lower.find("* ") != std::string::npos ||
         (Lower.find("[") != std::string::npos && Lower.find("](") != std::string::npos))
     {
          return "markdown";
     }

     size_t ListLineCount = 0;
     size_t TotalLineCount = 0;
     std::istringstream Lines(Trimmed);
     std::string Line;

     while (std::getline(Lines, Line))
     {
          const std::string TrimmedLine = TrimCopy(Line);

          if (TrimmedLine.empty())
          {
               continue;
          }

          ++TotalLineCount;

          if (TrimmedLine.rfind("- ", 0) == 0 ||
              TrimmedLine.rfind("* ", 0) == 0 ||
              TrimmedLine.rfind("1. ", 0) == 0 ||
              TrimmedLine.rfind("2. ", 0) == 0 ||
              TrimmedLine.rfind("3. ", 0) == 0)
          {
               ++ListLineCount;
          }
     }

     if (ListLineCount >= 2 && ListLineCount + 1 >= TotalLineCount)
     {
          return "list";
     }

     return "text";
}

bool IsSamStopword(const std::string& Value);
std::string SingularizeToken(const std::string& Token);
double ClampSAMScore(double Value);

std::vector<std::string> UniqueNormalizedPhrases(const std::vector<std::string>& Values,
                                                 size_t MaxItems = 12)
{
     std::vector<std::string> Result;
     std::unordered_set<std::string> Seen;

     for (const auto& Value : Values)
     {
          const std::string Normalized = NormalizeTerm(Value);

          if (Normalized.empty() || !Seen.insert(Normalized).second)
          {
               continue;
          }

          Result.push_back(Normalized);

          if (Result.size() >= MaxItems)
          {
               break;
          }
     }

     return Result;
}

std::vector<float> BuildHashedSemanticVector(const std::vector<std::string>& Phrases,
                                             size_t Dimensions = 64)
{
     std::vector<float> Vector(Dimensions, 0.0f);

     if (Dimensions == 0)
     {
          return Vector;
     }

     for (const auto& Phrase : Phrases)
     {
          std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Phrase));

          for (auto& Token : Tokens)
          {
               Token = SingularizeToken(Token);
          }

          for (const auto& Token : Tokens)
          {
               if (Token.empty() || IsSamStopword(Token))
               {
                    continue;
               }

               const size_t Bucket = std::hash<std::string>{}(Token) % Dimensions;
               const float Weight = Token.size() >= 7 ? 1.35f : 1.0f;
               Vector[Bucket] += Weight;
          }
     }

     float NormSq = 0.0f;

     for (float Value : Vector)
     {
          NormSq += Value * Value;
     }

     if (NormSq <= 0.0f)
     {
          return Vector;
     }

     const float InvNorm = 1.0f / std::sqrt(NormSq);

     for (float& Value : Vector)
     {
          Value *= InvNorm;
     }

     return Vector;
}

double ComputeSemanticVectorSimilarity(const std::vector<float>& Left,
                                       const std::vector<float>& Right)
{
     if (Left.empty() || Right.empty() || Left.size() != Right.size())
     {
          return 0.0;
     }

     double Dot = 0.0;

     for (size_t Index = 0; Index < Left.size(); ++Index)
     {
          Dot += static_cast<double>(Left[Index]) * static_cast<double>(Right[Index]);
     }

     return ClampSAMScore((Dot + 1.0) * 0.5);
}

struct SAMQueryTokenViews
{
     bool Quoted = false;
     std::string NormalizedQuery;
     std::string NormalizedPhrase;
     std::vector<std::string> FullTokens;
     std::vector<std::string> CoreTokens;
};

bool IsSingleTokenSAMIntent(const SAMQueryTokenViews& QueryViews)
{
     const size_t TokenCount = QueryViews.CoreTokens.empty()
          ? QueryViews.FullTokens.size()
          : QueryViews.CoreTokens.size();
     return TokenCount <= 1;
}

struct SAMTokenMatchResult
{
     bool Matched = false;
     bool UsedSynonym = false;
     size_t Distance = static_cast<size_t>(-1);
};

struct SAMLearnedVariant
{
     std::string Text;
     double Score = 0.0;
     size_t Support = 0;
};

struct SAMCollectionProfileHints
{
     std::unordered_set<std::string> StrongTokens;
};

struct SAMProfileEntry
{
     std::string Text;
     double Score = 0.0;
     size_t Support = 0;
     std::vector<std::string> Related;
};

struct SAMProfileFamily
{
     std::string Subject;
     double Score = 0.0;
     size_t Support = 0;
     std::vector<std::string> Aliases;
     std::vector<std::string> Descriptors;
     std::vector<std::string> Queries;
};

struct SAMSemanticQuery
{
     std::vector<std::string> Rewrites;
     std::vector<float> Vector;
};

struct SAMSemanticCandidate
{
     double ProfileScore = 0.0;
     double VectorScore = 0.0;
     std::string MatchedText;
     std::string MatchedSource;
};

size_t EditDistance(const std::string& A, const std::string& B);
double ClampSAMScore(double Value);
std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID);
std::string BuildCollectionProfileKey(const std::string& Collection);
std::string BuildSearchIdeaPrefix(const std::string& Collection);
uint64_t GetSAMCurrentTimeMS();
double GetSAMIdeaFreshness(uint64_t LastSeenMS, uint64_t NowMS);
bool ParseSearchIdeaEntry(const std::string& RawValue, SAM::SearchIdeaEntry& Entry);
std::string BuildStrongSearchIdeaPhrase(const std::string& Value, size_t MaxTokens = 4);
bool IsUsefulLearnedVariant(const std::string& Value,
                            const std::unordered_set<std::string>& QueryTokens,
                            const std::unordered_set<std::string>* StrongTokens = nullptr);
SAMCollectionProfileHints LoadCollectionProfileHints(rocksdb::DB* Database,
                                                     const std::string& Collection);
double ComputeManifestSeedStrength(const SAMQueryTokenViews& QueryViews,
                                   const SAM::TermEntry& Term);
bool SearchIntentCandidateWeightGreater(const llm::SearchIntentCandidate& Left,
                                        const llm::SearchIntentCandidate& Right);

bool IsSubjectLikeTermKind(const std::string& Kind)
{
     return Kind == "subject" || Kind == "alias" || Kind == "synonym";
}

bool IsDescriptorLikeTermKind(const std::string& Kind)
{
     return Kind == "descriptor" || Kind == "query";
}

bool IsCollectionProfileCandidate(const std::string& Value)
{
     return IsUsefulLearnedVariant(Value, std::unordered_set<std::string>{});
}

double GetCollectionProfileKindWeight(const std::string& Kind)
{
     if (Kind == "subject")
     {
          return 1.0;
     }

     if (Kind == "alias" || Kind == "synonym")
     {
          return 0.88;
     }

     if (Kind == "query")
     {
          return 0.76;
     }

     if (Kind == "descriptor")
     {
          return 0.70;
     }

     return 0.62;
}

std::string SelectProfileAnchorSubject(const SAM::DocumentEntry& Entry)
{
     const std::string NormalizedTitle = NormalizeTerm(Entry.Title);
     const SAM::TermEntry* Best = nullptr;

     for (const auto& Term : Entry.Terms)
     {
          const std::string Candidate = NormalizeTerm(Term.Text);

          if (Candidate.empty())
          {
               continue;
          }

          if (IsSubjectLikeTermKind(Term.Kind))
          {
               if (!Best ||
                   GetCollectionProfileKindWeight(Term.Kind) > GetCollectionProfileKindWeight(Best->Kind) ||
                   (GetCollectionProfileKindWeight(Term.Kind) == GetCollectionProfileKindWeight(Best->Kind) &&
                    ClampSAMScore(Term.Score) > ClampSAMScore(Best->Score)))
               {
                    Best = &Term;
               }
          }
     }

     if (Best)
     {
          return NormalizeTerm(Best->Text);
     }

     return NormalizedTitle;
}

SAMSemanticProfile BuildSemanticProfile(const std::string& Title,
                                        const std::vector<SAM::TermEntry>& Terms)
{
     SAMSemanticProfile Profile;
     std::vector<std::string> VectorPhrases;
     const std::string NormalizedTitle = NormalizeTerm(Title);

     Profile.Subject = NormalizedTitle;

     if (!NormalizedTitle.empty())
     {
          VectorPhrases.push_back(NormalizedTitle);
     }

     for (const auto& Term : Terms)
     {
          const std::string Candidate = NormalizeTerm(Term.Text);

          if (Candidate.empty())
          {
               continue;
          }

          if (Profile.Subject.empty() && IsSubjectLikeTermKind(Term.Kind))
          {
               Profile.Subject = Candidate;
          }

          if ((Term.Kind == "alias" || Term.Kind == "synonym") && Profile.Aliases.size() < 8)
          {
               Profile.Aliases.push_back(Candidate);
          }
          else if (Term.Kind == "descriptor" && Profile.Descriptors.size() < 10)
          {
               Profile.Descriptors.push_back(Candidate);
          }
          else if (Term.Kind == "query" && Profile.Queries.size() < 10)
          {
               Profile.Queries.push_back(Candidate);
          }

          if (VectorPhrases.size() < 24)
          {
               VectorPhrases.push_back(Candidate);
          }
     }

     Profile.Aliases = UniqueNormalizedPhrases(Profile.Aliases, 8);
     Profile.Descriptors = UniqueNormalizedPhrases(Profile.Descriptors, 10);
     Profile.Queries = UniqueNormalizedPhrases(Profile.Queries, 10);

     std::vector<std::string> SummaryParts;

     if (!Profile.Subject.empty())
     {
          SummaryParts.push_back(Profile.Subject);
     }

     for (const auto& Alias : Profile.Aliases)
     {
          SummaryParts.push_back(Alias);
     }

     for (const auto& Descriptor : Profile.Descriptors)
     {
          SummaryParts.push_back(Descriptor);
     }

     Profile.Summary = JoinTokens(UniqueNormalizedPhrases(SummaryParts, 10));
     Profile.Vector = BuildHashedSemanticVector(VectorPhrases);
     return Profile;
}

void StoreSemanticProfileJSON(nlohmann::json& Manifest,
                              const SAMSemanticProfile& Profile)
{
     nlohmann::json Semantic;
     Semantic["subject"] = Profile.Subject;
     Semantic["summary"] = Profile.Summary;
     Semantic["aliases"] = Profile.Aliases;
     Semantic["descriptors"] = Profile.Descriptors;
     Semantic["queries"] = Profile.Queries;
     Semantic["vector"] = nlohmann::json::array();

     for (float Value : Profile.Vector)
     {
          Semantic["vector"].push_back(Value);
     }

     Manifest["semantic_profile"] = std::move(Semantic);
}

bool ParseSemanticProfileJSON(const nlohmann::json& Root,
                              SAMSemanticProfile& Profile)
{
     if (!Root.contains("semantic_profile") || !Root["semantic_profile"].is_object())
     {
          return false;
     }

     const nlohmann::json& Semantic = Root["semantic_profile"];
     Profile = SAMSemanticProfile{};
     Profile.Subject = NormalizeTerm(Semantic.value("subject", ""));
     Profile.Summary = NormalizeTerm(Semantic.value("summary", ""));

     auto ParseStringArray = [](const nlohmann::json& Value, std::vector<std::string>& Output, size_t Limit)
     {
          if (!Value.is_array())
          {
               return;
          }

          for (const auto& Item : Value)
          {
               if (!Item.is_string())
               {
                    continue;
               }

               const std::string Candidate = NormalizeTerm(Item.get<std::string>());

               if (Candidate.empty())
               {
                    continue;
               }

               Output.push_back(Candidate);

               if (Output.size() >= Limit)
               {
                    break;
               }
          }
     };

     ParseStringArray(Semantic.value("aliases", nlohmann::json::array()), Profile.Aliases, 8);
     ParseStringArray(Semantic.value("descriptors", nlohmann::json::array()), Profile.Descriptors, 10);
     ParseStringArray(Semantic.value("queries", nlohmann::json::array()), Profile.Queries, 10);

     if (Semantic.contains("vector") && Semantic["vector"].is_array())
     {
          for (const auto& Item : Semantic["vector"])
          {
               if (Item.is_number())
               {
                    Profile.Vector.push_back(Item.get<float>());
               }
          }
     }

     return !Profile.Subject.empty() || !Profile.Summary.empty() ||
            !Profile.Aliases.empty() || !Profile.Descriptors.empty() || !Profile.Queries.empty();
}
bool LooksLikeWeakLLMSuffix(const std::string& Value)
{
     static const std::unordered_set<std::string> WeakSuffixes = {
          "artist", "artists", "music", "songs", "career", "careers", "industry",
          "performance", "performances", "profile", "profiles", "official"};
     return WeakSuffixes.find(Value) != WeakSuffixes.end();
}

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

bool QueryUsesSAMLikePattern(const std::string& Query)
{
     bool EscapeNext = false;

     for (unsigned char C : Query)
     {
          if (EscapeNext)
          {
               EscapeNext = false;
               continue;
          }

          if (C == '\\')
          {
               EscapeNext = true;
               continue;
          }

          if (C == '%' || C == '_')
          {
               return true;
          }
     }

     return false;
}

std::string NormalizeSAMLikePattern(const std::string& Query)
{
     const std::string Raw = StripSAMQueryQuotes(Query);
     std::string Pattern;
     Pattern.reserve(Raw.size());
     bool LastWasSpace = false;
     bool EscapeNext = false;

     for (unsigned char C : Raw)
     {
          if (EscapeNext)
          {
               if (std::isalnum(C))
               {
                    Pattern.push_back(static_cast<char>(std::tolower(C)));
                    LastWasSpace = false;
               }
               else if (C == '%' || C == '_')
               {
                    Pattern.push_back(static_cast<char>(C));
                    LastWasSpace = false;
               }
               else if (std::isspace(C) || C == '-' || C == '/' || C == '.')
               {
                    if (!Pattern.empty() && !LastWasSpace)
                    {
                         Pattern.push_back(' ');
                         LastWasSpace = true;
                    }
               }

               EscapeNext = false;
               continue;
          }

          if (C == '\\')
          {
               EscapeNext = true;
               continue;
          }

          if (std::isalnum(C))
          {
               Pattern.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (C == '%' || C == '_')
          {
               Pattern.push_back(static_cast<char>(C));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '/' || C == '.')
          {
               if (!Pattern.empty() && !LastWasSpace)
               {
                    Pattern.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     std::string Converted;
     Converted.reserve(Pattern.size());
     bool LastWasStar = false;

     for (char C : TrimCopy(Pattern))
     {
          if (C == '%')
          {
               if (!LastWasStar)
               {
                    Converted.push_back('*');
                    LastWasStar = true;
               }

               continue;
          }

          LastWasStar = false;
          Converted.push_back(C == '_' ? '?' : C);
     }

     return TrimCopy(Converted);
}

std::vector<std::string> BuildQueryVariants(const std::string& Query,
                                            const std::unordered_set<std::string>* StrongTokens = nullptr)
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

          if (IsWeakSamToken(Token) &&
              (!StrongTokens || StrongTokens->find(SingularizeToken(Token)) == StrongTokens->end()))
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

std::vector<std::string> BuildSemanticRewriteSeeds(const std::string& Query,
                                                   const SAMQueryTokenViews& QueryViews)
{
     std::vector<std::string> Seeds;
     std::unordered_set<std::string> Seen;

     auto AppendSeed = [&](const std::string& Value)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (Candidate.empty() || !Seen.insert(Candidate).second)
          {
               return;
          }

          Seeds.push_back(Candidate);
     };

     AppendSeed(QueryViews.NormalizedPhrase.empty() ? QueryViews.NormalizedQuery : QueryViews.NormalizedPhrase);
     AppendSeed(JoinTokens(QueryViews.CoreTokens));

     if (QueryViews.CoreTokens.size() >= 2)
     {
          AppendSeed(QueryViews.CoreTokens.front() + " " + QueryViews.CoreTokens.back());
     }

     if (QueryViews.CoreTokens.size() >= 3)
     {
          std::vector<std::string> Slice(QueryViews.CoreTokens.begin(),
                                         QueryViews.CoreTokens.begin() + static_cast<long>(std::min<size_t>(3, QueryViews.CoreTokens.size())));
          AppendSeed(JoinTokens(Slice));
     }

     return Seeds;
}

std::vector<std::string> BuildCollectionSemanticRewrites(rocksdb::DB* Database,
                                                         const std::string& Collection,
                                                         const SAMQueryTokenViews& QueryViews,
                                                         size_t MaxVariants = 10)
{
     std::vector<std::string> Variants;

     if (!Database || Collection.empty() || QueryViews.CoreTokens.empty() || MaxVariants == 0)
     {
          return Variants;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (!Status.ok() || RawProfile.empty())
     {
          return Variants;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawProfile);
          if ((!Root.contains("families") || !Root["families"].is_array()) &&
              (!Root.contains("learned_families") || !Root["learned_families"].is_array()))
          {
               return Variants;
          }

          std::unordered_set<std::string> Seen;
          auto AppendVariant = [&](const std::string& Value)
          {
               const std::string Candidate = NormalizeTerm(Value);

               if (Candidate.empty() || !Seen.insert(Candidate).second)
               {
                    return;
               }

               Variants.push_back(Candidate);
          };

          auto ProcessFamilyArray = [&](const nlohmann::json& FamiliesArray, double LayerWeight)
          {
               for (const auto& Family : FamiliesArray)
               {
                    if (!Family.is_object())
                    {
                         continue;
                    }

                    std::vector<std::string> FamilyTerms;
                    FamilyTerms.push_back(Family.value("subject", ""));

                    for (const auto& Key : {"aliases", "descriptors", "queries"})
                    {
                         if (!Family.contains(Key) || !Family[Key].is_array())
                         {
                              continue;
                         }

                         for (const auto& Item : Family[Key])
                         {
                              if (Item.is_string())
                              {
                                   FamilyTerms.push_back(Item.get<std::string>());
                              }
                         }
                    }

                    double Match = 0.0;

                    for (const auto& Term : FamilyTerms)
                    {
                         Match = std::max(
                              Match,
                              ComputeManifestSeedStrength(
                                   QueryViews,
                                   SAM::TermEntry{NormalizeTerm(Term),
                                                  "semantic_rewrite",
                                                  "profile",
                                                  0.72 * LayerWeight,
                                                  0.72 * LayerWeight}));
                    }

                    if (Match < (LayerWeight < 1.0 ? 0.60 : 0.56))
                    {
                         continue;
                    }

                    for (const auto& Term : FamilyTerms)
                    {
                         AppendVariant(Term);

                         if (Variants.size() >= MaxVariants)
                         {
                              return;
                         }
                    }
               }
          };

          if (Root.contains("families") && Root["families"].is_array())
          {
               ProcessFamilyArray(Root["families"], 1.0);
          }

          if (Variants.size() < MaxVariants &&
              Root.contains("learned_families") && Root["learned_families"].is_array())
          {
               ProcessFamilyArray(Root["learned_families"], 0.82);
          }
     }
     catch (...)
     {
     }

     return Variants;
}

SAMSemanticQuery BuildSemanticQueryPlan(rocksdb::DB* Database,
                                        const std::string& Collection,
                                        const std::string& Query,
                                        const SAMQueryTokenViews& QueryViews)
{
     SAMSemanticQuery Plan;
     Plan.Rewrites = BuildSemanticRewriteSeeds(Query, QueryViews);

     const std::vector<std::string> CollectionRewrites =
          BuildCollectionSemanticRewrites(Database, Collection, QueryViews, 10);

     for (const auto& Rewrite : CollectionRewrites)
     {
          if (std::find(Plan.Rewrites.begin(), Plan.Rewrites.end(), Rewrite) == Plan.Rewrites.end())
          {
               Plan.Rewrites.push_back(Rewrite);
          }
     }

     Plan.Vector = BuildHashedSemanticVector(Plan.Rewrites);
     return Plan;
}

bool IsStrongSAMVariantToken(const std::string& Token,
                             const std::unordered_set<std::string>* StrongTokens = nullptr)
{
     if (Token.empty() || IsSamStopword(Token))
     {
          return false;
     }

     if (StrongTokens && StrongTokens->find(Token) != StrongTokens->end())
     {
          return true;
     }

     return !IsWeakSamToken(Token);
}

bool IsUsefulLearnedVariant(const std::string& Value,
                            const std::unordered_set<std::string>& QueryTokens,
                            const std::unordered_set<std::string>* StrongTokens)
{
     const std::vector<std::string> Tokens = NormalizeSAMTokens(Value, true);

     if (Tokens.empty() || Tokens.size() > 5)
     {
          return false;
     }

     size_t StrongCount = 0;
     size_t NewTokenCount = 0;

     for (const auto& Token : Tokens)
     {
          if (!IsStrongSAMVariantToken(Token, StrongTokens))
          {
               return false;
          }

          ++StrongCount;

          if (QueryTokens.find(Token) == QueryTokens.end())
          {
               ++NewTokenCount;
          }
     }

     return StrongCount > 0 && NewTokenCount > 0;
}

SAMCollectionProfileHints LoadCollectionProfileHints(rocksdb::DB* Database,
                                                     const std::string& Collection)
{
     SAMCollectionProfileHints Hints;

     if (!Database || Collection.empty())
     {
          return Hints;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (!Status.ok() || RawProfile.empty())
     {
          return Hints;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawProfile);

          auto AddStrongTokens = [&](const std::string& Value)
          {
               for (const auto& Token : NormalizeSAMTokens(Value, true))
               {
                    if (!Token.empty() && !IsSamStopword(Token) && Token.size() >= 3)
                    {
                         Hints.StrongTokens.insert(Token);
                    }
               }
          };

          auto ProcessTermArray = [&](const nlohmann::json& TermsArray, size_t MinSupport, double MinScore)
          {
               if (!TermsArray.is_array())
               {
                    return;
               }

               for (const auto& Item : TermsArray)
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    const std::string Text = NormalizeTerm(Item.value("text", ""));
                    const size_t Support =
                         static_cast<size_t>(std::max<int64_t>(0, Item.value("support", 0)));
                    const double Score = ClampSAMScore(Item.value("score", 0.0));

                    if (Text.empty() || Support < MinSupport || Score < MinScore)
                    {
                         continue;
                    }

                    AddStrongTokens(Text);
               }
          };

          auto ProcessFamilyArray = [&](const nlohmann::json& FamiliesArray, size_t MinSupport, double MinScore)
          {
               if (!FamiliesArray.is_array())
               {
                    return;
               }

               for (const auto& Item : FamiliesArray)
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    const size_t Support =
                         static_cast<size_t>(std::max<int64_t>(0, Item.value("support", 0)));
                    const double Score = ClampSAMScore(Item.value("score", 0.0));

                    if (Support < MinSupport || Score < MinScore)
                    {
                         continue;
                    }

                    AddStrongTokens(Item.value("subject", ""));

                    auto ProcessStringArray = [&](const nlohmann::json& Values)
                    {
                         if (!Values.is_array())
                         {
                              return;
                         }

                         for (const auto& Value : Values)
                         {
                              if (Value.is_string())
                              {
                                   AddStrongTokens(Value.get<std::string>());
                              }
                         }
                    };

                    ProcessStringArray(Item.value("aliases", nlohmann::json::array()));
                    ProcessStringArray(Item.value("descriptors", nlohmann::json::array()));
                    ProcessStringArray(Item.value("queries", nlohmann::json::array()));
               }
          };

          ProcessTermArray(Root.value("terms", nlohmann::json::array()), 2, 0.54);
          ProcessTermArray(Root.value("learned_terms", nlohmann::json::array()), 1, 0.60);
          ProcessFamilyArray(Root.value("families", nlohmann::json::array()), 2, 0.54);
          ProcessFamilyArray(Root.value("learned_families", nlohmann::json::array()), 1, 0.60);
     }
     catch (...)
     {
     }

     return Hints;
}

double ComputeManifestSeedStrength(const SAMQueryTokenViews& QueryViews,
                                   const SAM::TermEntry& Term)
{
     if (QueryViews.CoreTokens.empty())
     {
          return 0.0;
     }

     const std::vector<std::string> TermTokens = NormalizeSAMTokens(Term.Text, true);

     if (TermTokens.empty())
     {
          return 0.0;
     }

     size_t MatchedTokens = 0;

     for (const auto& QueryToken : QueryViews.CoreTokens)
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
               ++MatchedTokens;
          }
     }

     const std::string NormalizedPhrase = QueryViews.NormalizedPhrase.empty()
          ? QueryViews.NormalizedQuery
          : QueryViews.NormalizedPhrase;
     const std::string NormalizedTerm = NormalizeTerm(Term.Text);

     if (!NormalizedPhrase.empty() &&
         (NormalizedTerm == NormalizedPhrase ||
          NormalizedTerm.find(NormalizedPhrase) != std::string::npos))
     {
          MatchedTokens = std::max(MatchedTokens, QueryViews.CoreTokens.size());
     }

     if (MatchedTokens == 0)
     {
          return 0.0;
     }

     const double Coverage = static_cast<double>(MatchedTokens) /
                             static_cast<double>(std::max<size_t>(1, QueryViews.CoreTokens.size()));
     const double Base = (Coverage * 0.72) +
                         (ClampSAMScore(Term.Score) * 0.18) +
                         (ClampSAMScore(Term.Signal) * 0.10);
     return ClampSAMScore(Base);
}

double ComputeSingleTokenLiteralBias(const SAMQueryTokenViews& QueryViews,
                                     const std::vector<std::string>& CandidateTokens)
{
     if (!IsSingleTokenSAMIntent(QueryViews) || QueryViews.CoreTokens.empty() || CandidateTokens.empty())
     {
          return 0.0;
     }

     const std::string& QueryToken = QueryViews.CoreTokens.front();
     bool ExactTokenPresent = false;

     for (const auto& Token : CandidateTokens)
     {
          if (Token == QueryToken)
          {
               ExactTokenPresent = true;
               break;
          }
     }

     if (!ExactTokenPresent)
     {
          return 0.0;
     }

     if (CandidateTokens.size() == 1)
     {
          return 0.18;
     }

     return -std::min(0.12, 0.04 * static_cast<double>(CandidateTokens.size() - 1));
}

std::vector<std::string> BuildCollectionLearnedVariants(rocksdb::DB* Database,
                                                        const std::string& Collection,
                                                        const std::string& Query,
                                                        const SAMQueryTokenViews& QueryViews,
                                                        const std::unordered_set<std::string>* StrongTokens = nullptr,
                                                        size_t MaxVariants = 12)
{
     std::vector<std::string> Variants;

     if (!Database || Collection.empty() || QueryViews.CoreTokens.empty() || MaxVariants == 0 ||
         IsSingleTokenSAMIntent(QueryViews))
     {
          return Variants;
     }

     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::unordered_map<std::string, SAMLearnedVariant> Ranked;
     std::unordered_set<std::string> QueryTokenSet(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
     const std::string NormalizedQuery = NormalizeTerm(QueryViews.NormalizedPhrase.empty() ? Query : QueryViews.NormalizedPhrase);
     size_t ScannedDocuments = 0;
     constexpr size_t kMaxManifestScans = 220;

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix) && ScannedDocuments < kMaxManifestScans;
          Iterator->Next(), ++ScannedDocuments)
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry) ||
              Entry.Terms.empty())
          {
               continue;
          }

          double BestSeedStrength = 0.0;

          for (const auto& Term : Entry.Terms)
          {
               BestSeedStrength = std::max(BestSeedStrength, ComputeManifestSeedStrength(QueryViews, Term));
          }

          if (BestSeedStrength < 0.58)
          {
               continue;
          }

          std::unordered_set<std::string> SeenInDocument;

          for (const auto& Term : Entry.Terms)
          {
               const std::string Candidate = NormalizeTerm(Term.Text);

               if (Candidate.empty() || Candidate == NormalizedQuery)
               {
                    continue;
               }

               if (!IsUsefulLearnedVariant(Candidate, QueryTokenSet, StrongTokens))
               {
                    continue;
               }

               if (!SeenInDocument.insert(Candidate).second)
               {
                    continue;
               }

               SAMLearnedVariant& RankedEntry = Ranked[Candidate];
               RankedEntry.Text = Candidate;
               RankedEntry.Score += BestSeedStrength *
                                    ((ClampSAMScore(Term.Score) * 0.55) +
                                     (ClampSAMScore(Term.Signal) * 0.25) +
                                     0.20);
               ++RankedEntry.Support;
          }
     }

     std::vector<SAMLearnedVariant> Sorted;
     Sorted.reserve(Ranked.size());

     for (const auto& Pair : Ranked)
     {
          const SAMLearnedVariant& Candidate = Pair.second;

          if (Candidate.Support == 0 || Candidate.Score < 0.70)
          {
               continue;
          }

          Sorted.push_back(Candidate);
     }

     std::sort(Sorted.begin(), Sorted.end(),
               [](const SAMLearnedVariant& A, const SAMLearnedVariant& B)
               {
                    if (A.Support != B.Support)
                    {
                         return A.Support > B.Support;
                    }

                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Text.size() != B.Text.size())
                    {
                         return A.Text.size() < B.Text.size();
                    }

                    return A.Text < B.Text;
               });

     std::unordered_set<std::string> Seen;

     for (const auto& Candidate : Sorted)
     {
          if (!Seen.insert(Candidate.Text).second)
          {
               continue;
          }

          Variants.push_back(Candidate.Text);

          if (Variants.size() >= MaxVariants)
          {
               break;
          }
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

     if (Source == "field_title" || Source == "title_window")
     {
          return 1.10;
     }

     if (Source == "label_pair")
     {
          return 1.10;
     }

     if (Source == "subject_alias" || Source == "subject_descriptor" || Source == "iterative_pair")
     {
          return 1.08;
     }

     if (Source == "label" || Source == "topic")
     {
          return 1.00;
     }

     if (Source == "taxonomy_field" || Source == "taxonomy_window" ||
         Source == "alias_field" || Source == "alias_window" ||
         Source == "field_pair" || Source == "query_pair" ||
         Source == "query_field" || Source == "query_window" ||
         Source == "seed_query" || Source == "context_pair" ||
         Source == "profile_pair")
     {
          return 1.02;
     }

     if (Source == "label_reduced" || Source == "topic_join" || Source == "category")
     {
          return 0.96;
     }

     if (Source == "summary_field" || Source == "summary_window" ||
         Source == "body_window" || Source == "body_query" ||
         Source == "query_refine" || Source == "query_expand" ||
         Source == "iterative_refine" || Source == "profile_context" ||
         Source == "context_field")
     {
          return 0.98;
     }

     if (Source == "llm_context" || Source == "llm_pair")
     {
          return 0.94;
     }

     if (Source == "llm_anchor" || Source == "llm_alias" ||
         Source == "llm_descriptor" || Source == "llm_query" ||
         Source == "llm_anchor_pair")
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

     if (Source == "field_title" || Source == "title_window")
     {
          return Instance->Config->GetSam25SourcePhraseBoostTitle() * 0.98;
     }

     if (Source == "label_pair")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLabelPair();
     }

     if (Source == "subject_alias" || Source == "subject_descriptor" || Source == "iterative_pair")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLabelPair() * 0.98;
     }

     if (Source == "label" || Source == "label_reduced")
     {
          return Instance->Config->GetSam25SourcePhraseBoostLabel();
     }

     if (Source == "taxonomy_field" || Source == "taxonomy_window" ||
         Source == "alias_field" || Source == "alias_window" ||
         Source == "field_pair" || Source == "query_pair" ||
         Source == "query_field" || Source == "query_window" ||
         Source == "seed_query" || Source == "context_pair" ||
         Source == "profile_pair")
     {
          return 1.02;
     }

     if (Source == "summary_field" || Source == "summary_window" ||
         Source == "body_window" || Source == "body_query" ||
         Source == "query_refine" || Source == "query_expand" ||
         Source == "iterative_refine" || Source == "profile_context" ||
         Source == "context_field")
     {
          return 0.99;
     }

     if (Source == "llm_context" || Source == "llm_pair")
     {
          return std::max(0.92, Instance->Config->GetSam25SourcePhraseBoostLlm() * 0.96);
     }

     if (Source == "llm_anchor" || Source == "llm_alias" ||
         Source == "llm_descriptor" || Source == "llm_query" ||
         Source == "llm_anchor_pair")
     {
          return std::max(0.94, Instance->Config->GetSam25SourcePhraseBoostLlm() * 0.98);
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
     std::unordered_set<std::string> DistinctTerms;
     std::unordered_set<std::string> DistinctSources;
     double BestSemanticScore = 0.0;
     double BestSemanticVectorScore = 0.0;
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

     Score = ClampSAMScore(Score + ComputeSingleTokenLiteralBias(QueryViews, FieldTokens));
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
     if (!Hit.MatchedTerm.empty())
     {
          Aggregate.DistinctTerms.insert(Hit.MatchedTerm);
     }
     if (!Hit.MatchedSource.empty())
     {
          Aggregate.DistinctSources.insert(Hit.MatchedSource);
     }
     Aggregate.BestSemanticScore = std::max(Aggregate.BestSemanticScore, Hit.Breakdown.SemanticScore);
     Aggregate.BestSemanticVectorScore = std::max(Aggregate.BestSemanticVectorScore, Hit.Breakdown.SemanticVectorScore);
}

std::vector<SAMLearnedVariant> BuildSeededCollectionVariants(rocksdb::DB* Database,
                                                             const std::string& Collection,
                                                             const SAMQueryTokenViews& QueryViews,
                                                             const std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                                                             const std::unordered_set<std::string>* StrongTokens = nullptr,
                                                             size_t MaxVariants = 10)
{
     std::vector<SAMLearnedVariant> Variants;

     if (!Database || Collection.empty() || AggregatedHits.empty() || QueryViews.CoreTokens.empty() || MaxVariants == 0 ||
         IsSingleTokenSAMIntent(QueryViews))
     {
          return Variants;
     }

     std::vector<const SAMAggregatedHit*> Seeds;
     Seeds.reserve(AggregatedHits.size());

     for (const auto& Pair : AggregatedHits)
     {
          if (Pair.second.BestHit.Collection == Collection)
          {
               Seeds.push_back(&Pair.second);
          }
     }

     if (Seeds.empty())
     {
          return Variants;
     }

     std::sort(Seeds.begin(), Seeds.end(),
               [](const SAMAggregatedHit* A, const SAMAggregatedHit* B)
               {
                    return A->BestHit.MatchedScore > B->BestHit.MatchedScore;
               });

     std::unordered_map<std::string, SAMLearnedVariant> Ranked;
     const std::unordered_set<std::string> QueryTokenSet(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
     const std::string NormalizedQuery = NormalizeTerm(
          QueryViews.NormalizedPhrase.empty() ? QueryViews.NormalizedQuery : QueryViews.NormalizedPhrase);
     const size_t SeedLimit = std::min<size_t>(4, Seeds.size());

     for (size_t SeedIndex = 0; SeedIndex < SeedLimit; ++SeedIndex)
     {
          const SAMAggregatedHit& Seed = *Seeds[SeedIndex];
          std::string ManifestValue;
          const rocksdb::Status Status = Database->Get(rocksdb::ReadOptions(),
                                                       BuildDocManifestKey(Collection, Seed.BestHit.DocumentID),
                                                       &ManifestValue);

          if (!Status.ok())
          {
               continue;
          }

          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(ManifestValue, Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          std::unordered_set<std::string> SeenInSeed;
          const double SeedWeight = ClampSAMScore(Seed.BestHit.MatchedScore / 2.25);

          for (const auto& Term : Entry.Terms)
          {
               const std::string Candidate = NormalizeTerm(Term.Text);

               if (Candidate.empty() || Candidate == NormalizedQuery)
               {
                    continue;
               }

               if (!IsUsefulLearnedVariant(Candidate, QueryTokenSet, StrongTokens))
               {
                    continue;
               }

               if (!SeenInSeed.insert(Candidate).second)
               {
                    continue;
               }

               SAMLearnedVariant& RankedEntry = Ranked[Candidate];
               RankedEntry.Text = Candidate;
               RankedEntry.Score += (SeedWeight * 0.45) +
                                    (ClampSAMScore(Term.Score) * 0.35) +
                                    (ClampSAMScore(Term.Signal) * 0.20);
               ++RankedEntry.Support;
          }
     }

     for (const auto& Pair : Ranked)
     {
          if (Pair.second.Score >= 0.62)
          {
               Variants.push_back(Pair.second);
          }
     }

     std::sort(Variants.begin(), Variants.end(),
               [](const SAMLearnedVariant& A, const SAMLearnedVariant& B)
               {
                    if (A.Support != B.Support)
                    {
                         return A.Support > B.Support;
                    }

                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Text.size() != B.Text.size())
                    {
                         return A.Text.size() < B.Text.size();
                    }

                    return A.Text < B.Text;
               });

     if (Variants.size() > MaxVariants)
     {
          Variants.resize(MaxVariants);
     }

     return Variants;
}

void AppendCollectionLearnedHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                                 rocksdb::DB* Database,
                                 const std::string& Collection,
                                 const std::vector<SAMLearnedVariant>& Variants)
{
     if (!Database || Collection.empty() || Variants.empty())
     {
          return;
     }

     std::unordered_map<std::string, SAMLearnedVariant> VariantByText;

     for (const auto& Variant : Variants)
     {
          VariantByText[Variant.Text] = Variant;
     }

     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     size_t ScannedDocuments = 0;
     constexpr size_t kMaxManifestScans = 240;

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix) && ScannedDocuments < kMaxManifestScans;
          Iterator->Next(), ++ScannedDocuments)
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          const SAM::TermEntry* BestTerm = nullptr;
          const SAMLearnedVariant* BestVariant = nullptr;

          for (const auto& Term : Entry.Terms)
          {
               const std::string Candidate = NormalizeTerm(Term.Text);
               auto VariantIt = VariantByText.find(Candidate);

               if (VariantIt == VariantByText.end())
               {
                    continue;
               }

               if (!BestTerm ||
                   VariantIt->second.Score > BestVariant->Score ||
                   (VariantIt->second.Score == BestVariant->Score && Term.Score > BestTerm->Score))
               {
                    BestTerm = &Term;
                    BestVariant = &VariantIt->second;
               }
          }

          if (!BestTerm || !BestVariant)
          {
               continue;
          }

          SAM::LookupHit Hit;
          Hit.Collection = Entry.Collection;
          Hit.DocumentID = Entry.DocumentID;
          Hit.Title = Entry.Title;
          Hit.MatchedTerm = BestTerm->Text;
          Hit.MatchedKind = BestTerm->Kind;
          Hit.MatchedSource = BestTerm->Source;
          Hit.TermOrigin = "collection_learned";
          Hit.MatchedPath = "collection_learned";
          Hit.MatchedScore = (ClampSAMScore(BestTerm->Score) * 0.88) +
                             std::min(0.28, BestVariant->Score * 0.18);
          Hit.MatchedSignal = std::max(BestTerm->Signal, std::min(1.0, BestVariant->Score));
          Hit.Breakdown.TermScore = Hit.MatchedScore;
          Hit.Breakdown.FinalScore = Hit.MatchedScore;
          AccumulateSAMHit(AggregatedHits, Hit);
     }
}

struct SAMMatchedSearchIdea
{
     SAM::SearchIdeaEntry Entry;
     double Score = 0.0;
     double SemanticScore = 0.0;
     double CoverageScore = 0.0;
};

std::vector<SAMMatchedSearchIdea> BuildMatchedSearchIdeas(rocksdb::DB* Database,
                                                          const std::string& Collection,
                                                          const std::string& Query,
                                                          const SAMQueryTokenViews& QueryViews,
                                                          size_t MaxIdeas)
{
     std::vector<SAMMatchedSearchIdea> Matches;

     if (!Database || Collection.empty() || QueryViews.CoreTokens.empty() || MaxIdeas == 0)
     {
          return Matches;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     const std::vector<float> QueryVector = BuildHashedSemanticVector({
          NormalizeTerm(QueryViews.NormalizedPhrase.empty() ? Query : QueryViews.NormalizedPhrase)
     });
     const std::unordered_set<std::string> QueryTokenSet(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
     const uint64_t NowMS = GetSAMCurrentTimeMS();

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) || Entry.NormalizedQuery.empty())
          {
               continue;
          }

          const std::vector<std::string> IdeaTokens = NormalizeSAMTokens(Entry.NormalizedQuery, true);

          if (IdeaTokens.empty())
          {
               continue;
          }

          size_t Overlap = 0;

          for (const auto& Token : IdeaTokens)
          {
               if (QueryTokenSet.find(Token) != QueryTokenSet.end())
               {
                    ++Overlap;
               }
          }

          const double CoverageScore = ClampSAMScore(static_cast<double>(Overlap) /
               static_cast<double>(std::max<size_t>(1, QueryViews.CoreTokens.size())));
          const double SemanticScore = ComputeSemanticVectorSimilarity(QueryVector, Entry.Vector);
          const double PopularityScore = ClampSAMScore(std::log1p(static_cast<double>(Entry.Uses)) / std::log(12.0));
          const double FreshnessScore = GetSAMIdeaFreshness(Entry.LastSeenMS, NowMS);
          const double Score = ClampSAMScore((SemanticScore * 0.46) +
                                             (CoverageScore * 0.32) +
                                             (PopularityScore * 0.14) +
                                             (FreshnessScore * 0.08));

          if (Score < 0.58)
          {
               continue;
          }

          Matches.push_back({std::move(Entry), Score, SemanticScore, CoverageScore});
     }

     std::sort(Matches.begin(), Matches.end(),
               [](const SAMMatchedSearchIdea& A, const SAMMatchedSearchIdea& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Entry.Uses != B.Entry.Uses)
                    {
                         return A.Entry.Uses > B.Entry.Uses;
                    }

                    return A.Entry.LastSeenMS > B.Entry.LastSeenMS;
               });

     if (Matches.size() > MaxIdeas)
     {
          Matches.resize(MaxIdeas);
     }

     return Matches;
}

std::vector<std::string> BuildSearchIdeaVariants(const std::vector<SAMMatchedSearchIdea>& Ideas,
                                                 const SAMQueryTokenViews& QueryViews,
                                                 const std::unordered_set<std::string>* StrongTokens,
                                                 size_t MaxVariants)
{
     std::vector<std::string> Variants;

     if (Ideas.empty() || MaxVariants == 0)
     {
          return Variants;
     }

     std::unordered_set<std::string> Seen;
     std::unordered_set<std::string> QueryTokenSet(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
     const std::string QueryText = QueryViews.NormalizedPhrase.empty()
          ? QueryViews.NormalizedQuery
          : QueryViews.NormalizedPhrase;

     for (const auto& Idea : Ideas)
     {
          const std::string Candidate = NormalizeTerm(Idea.Entry.Query);

          if (Candidate.empty() || Candidate == QueryText)
          {
               continue;
          }

          if (!IsUsefulLearnedVariant(Candidate, QueryTokenSet, StrongTokens) || !Seen.insert(Candidate).second)
          {
               continue;
          }

          Variants.push_back(Candidate);

          if (Variants.size() >= MaxVariants)
          {
               break;
          }
     }

     return Variants;
}

void AppendSearchIdeaHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                          rocksdb::DB* Database,
                          const std::string& Collection,
                          const std::vector<SAMMatchedSearchIdea>& Ideas)
{
     if (!Database || Collection.empty() || Ideas.empty())
     {
          return;
     }

     for (const auto& Idea : Ideas)
     {
          for (const auto& DocumentRef : Idea.Entry.Documents)
          {
               if (DocumentRef.DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status Status = Database->Get(rocksdb::ReadOptions(),
                                                            BuildDocManifestKey(Collection, DocumentRef.DocumentID),
                                                            &ManifestValue);

               if (!Status.ok())
               {
                    continue;
               }

               SAM::DocumentEntry Entry;

               if (!ParseManifestValue(ManifestValue, Entry) ||
                   !IsSAMDocumentEntryCurrent(Entry))
               {
                    continue;
               }

               SAM::LookupHit Hit;
               Hit.Collection = Entry.Collection;
               Hit.DocumentID = Entry.DocumentID;
               Hit.Title = Entry.Title.empty() ? DocumentRef.Title : Entry.Title;
               Hit.MatchedTerm = Idea.Entry.Query;
               Hit.MatchedKind = "search_idea";
               Hit.MatchedSource = "search_idea";
               Hit.TermOrigin = "search_idea";
               Hit.MatchedPath = "search_idea";
               Hit.MatchedScore = ClampSAMScore((Idea.Score * 0.74) +
                                                (ClampSAMScore(DocumentRef.Score) * 0.26));
               Hit.MatchedSignal = std::max(Idea.SemanticScore, Idea.CoverageScore);
               Hit.EvidenceCount = static_cast<size_t>(std::max<uint64_t>(1, Idea.Entry.Uses));
               Hit.Breakdown.TermScore = Hit.MatchedScore;
               Hit.Breakdown.SemanticScore = Idea.SemanticScore;
               Hit.Breakdown.SemanticVectorScore = Idea.SemanticScore;
               Hit.Breakdown.EvidenceBonus = std::min(0.18, static_cast<double>(Hit.EvidenceCount) * 0.01);
               Hit.Breakdown.FinalScore = ClampSAMScore(Hit.MatchedScore + Hit.Breakdown.EvidenceBonus);
               AccumulateSAMHit(AggregatedHits, Hit);
          }
     }
}

SAMSemanticCandidate ScoreSemanticProfileMatch(const SAMSemanticQuery& QueryPlan,
                                               const SAMSemanticProfile& Profile)
{
     SAMSemanticCandidate Candidate;

     if (QueryPlan.Rewrites.empty())
     {
          return Candidate;
     }

     std::vector<std::pair<std::string, std::string>> Targets;

     if (!Profile.Subject.empty())
     {
          Targets.push_back({Profile.Subject, "semantic_subject"});
     }

     for (const auto& Alias : Profile.Aliases)
     {
          Targets.push_back({Alias, "semantic_alias"});
     }

     for (const auto& Descriptor : Profile.Descriptors)
     {
          Targets.push_back({Descriptor, "semantic_descriptor"});
     }

     for (const auto& Query : Profile.Queries)
     {
          Targets.push_back({Query, "semantic_query"});
     }

     for (const auto& Rewrite : QueryPlan.Rewrites)
     {
          const SAMQueryTokenViews RewriteViews = NormalizeSAMQueryTokenViews(Rewrite);

          for (const auto& Target : Targets)
          {
               const double Match = ComputeManifestSeedStrength(
                    RewriteViews,
                    SAM::TermEntry{Target.first, "semantic_profile", Target.second, 0.74, 0.74});

               if (Match > Candidate.ProfileScore)
               {
                    Candidate.ProfileScore = Match;
                    Candidate.MatchedText = Target.first;
                    Candidate.MatchedSource = Target.second;
               }
          }
     }

     Candidate.VectorScore = ComputeSemanticVectorSimilarity(QueryPlan.Vector, Profile.Vector);
     return Candidate;
}

void AppendSemanticProfileHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                               rocksdb::DB* Database,
                               const std::string& Collection,
                               const SAMSemanticQuery& QueryPlan,
                               size_t MaxCandidates = 256)
{
     if (!Database || QueryPlan.Rewrites.empty() || QueryPlan.Vector.empty())
     {
          return;
     }

     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     size_t Scanned = 0;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix) && Scanned < MaxCandidates;
          Iterator->Next(), ++Scanned)
     {
          try
          {
               const nlohmann::json Root = nlohmann::json::parse(Iterator->value().ToString());
               SAM::DocumentEntry Entry;

               if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
                   !IsSAMDocumentEntryCurrent(Entry))
               {
                    continue;
               }

               SAMSemanticProfile Profile;

               if (!ParseSemanticProfileJSON(Root, Profile))
               {
                    Profile = BuildSemanticProfile(Entry.Title.empty() ? Entry.DocumentID : Entry.Title,
                                                   Entry.Terms);
               }

               const SAMSemanticCandidate Match = ScoreSemanticProfileMatch(QueryPlan, Profile);
               const double CombinedSemantic = std::max(Match.ProfileScore, Match.VectorScore * 0.92);

               if (CombinedSemantic < 0.52)
               {
                    continue;
               }

               SAM::LookupHit Hit;
               Hit.Collection = Root.value("collection", "");
               Hit.DocumentID = Root.value("id", "");
               Hit.Title = Entry.Title.empty() ? Root.value("title", "") : Entry.Title;
               Hit.MatchedTerm = Match.MatchedText.empty() ? Profile.Subject : Match.MatchedText;
               Hit.MatchedKind = "semantic";
               Hit.MatchedSource = Match.MatchedSource.empty() ? "semantic_vector" : Match.MatchedSource;
               Hit.TermOrigin = "semantic_profile";
               Hit.MatchedPath = "semantic_profile";
               Hit.MatchedScore = CombinedSemantic;
               Hit.MatchedSignal = std::max(Match.ProfileScore, Match.VectorScore);
               Hit.Breakdown.SemanticScore = Match.ProfileScore;
               Hit.Breakdown.SemanticVectorScore = Match.VectorScore;
               Hit.Breakdown.FinalScore = CombinedSemantic;

               if (IsSAM25DebugExplainEnabled())
               {
                    std::ostringstream Stream;
                    Stream << "semantic profile=" << Match.ProfileScore
                           << " vector=" << Match.VectorScore
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

void AppendSAMLikePatternHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                              rocksdb::DB* Database,
                              const std::string& Collection,
                              const std::string& Query)
{
     if (!Database || !QueryUsesSAMLikePattern(Query))
     {
          return;
     }

     const std::string Pattern = NormalizeSAMLikePattern(Query);

     if (Pattern.empty())
     {
          return;
     }

     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) || !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          for (const auto& Term : Entry.Terms)
          {
               const std::string NormalizedTerm = NormalizeTerm(Term.Text);

               if (NormalizedTerm.empty() || !Wildcard::Match(NormalizedTerm, Pattern))
               {
                    continue;
               }

               SAM::LookupHit Hit;
               Hit.Collection = Entry.Collection;
               Hit.DocumentID = Entry.DocumentID;
               Hit.Title = Entry.Title;
               Hit.MatchedTerm = Term.Text;
               Hit.MatchedKind = Term.Kind;
               Hit.MatchedSource = Term.Source;
               Hit.TermOrigin = Term.Source;
               Hit.MatchedPath = "sam_like";
               Hit.MatchedScore = ClampSAMScore((ClampSAMScore(Term.Score) * 0.82) +
                                                (ClampSAMScore(Term.Signal) * 0.18) +
                                                0.12);
               Hit.MatchedSignal = std::max(ClampSAMScore(Term.Signal), 0.60);
               Hit.Breakdown.TermScore = Hit.MatchedScore;
               Hit.Breakdown.FinalScore = Hit.MatchedScore;

               if (IsSAM25DebugExplainEnabled())
               {
                    std::ostringstream Stream;
                    Stream << "sam25+ like pattern=" << Pattern
                           << " term=" << NormalizedTerm
                           << " source=" << Hit.MatchedSource
                           << " score=" << Hit.MatchedScore;
                    Hit.Explain = Stream.str();
               }

               AccumulateSAMHit(AggregatedHits, Hit);
          }
     }
}

std::string ClassifySAMMatchedPath(const SAM::LookupHit& Hit)
{
     const bool HasTermEvidence = Hit.Breakdown.TermScore > 0.0;
     const bool HasSourceEvidence = Hit.Breakdown.SourceDocScore > 0.0 || Hit.Breakdown.SourceDocBonus > 0.0;
     const bool HasSemanticEvidence = Hit.Breakdown.SemanticScore > 0.0 ||
                                      Hit.Breakdown.SemanticVectorScore > 0.0 ||
                                      Hit.Breakdown.SemanticBonus > 0.0;

     if (HasTermEvidence && HasSourceEvidence)
     {
          return "hybrid";
     }

     if (HasTermEvidence && HasSemanticEvidence)
     {
          return "semantic_hybrid";
     }

     if (HasSourceEvidence)
     {
          return "source_doc";
     }

     if (HasSemanticEvidence)
     {
          return "semantic_profile";
     }

     return "sam_term";
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
          const size_t ExtraEvidence = Entry.second.EvidenceCount > 0 ? Entry.second.EvidenceCount - 1 : 0;
          const size_t ExtraTerms = Entry.second.DistinctTerms.size() > 0 ? Entry.second.DistinctTerms.size() - 1 : 0;
          const size_t ExtraSources = Entry.second.DistinctSources.size() > 0 ? Entry.second.DistinctSources.size() - 1 : 0;
          const double EvidenceBonus = std::min(0.18,
               (0.02 * static_cast<double>(ExtraEvidence)) +
               (0.04 * static_cast<double>(ExtraTerms)) +
               (0.02 * static_cast<double>(ExtraSources)));
          const double SemanticBonus = std::min(0.24,
               (Entry.second.BestSemanticScore * 0.16) +
               (Entry.second.BestSemanticVectorScore * 0.08));
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
          FinalHit.EvidenceCount = Entry.second.EvidenceCount;
          FinalHit.Breakdown.EvidenceBonus = EvidenceBonus;
          FinalHit.Breakdown.DocPrior = DocPrior;
          FinalHit.Breakdown.SemanticScore = std::max(FinalHit.Breakdown.SemanticScore, Entry.second.BestSemanticScore);
          FinalHit.Breakdown.SemanticVectorScore = std::max(FinalHit.Breakdown.SemanticVectorScore, Entry.second.BestSemanticVectorScore);
          FinalHit.Breakdown.SemanticBonus = SemanticBonus;
          FinalHit.Breakdown.SourceDocBonus = SourceScoreBonus;
          FinalHit.MatchedScore += EvidenceBonus;
          FinalHit.MatchedScore += SemanticBonus;
          FinalHit.MatchedScore += Instance && Instance->Config
               ? (DocPrior * Instance->Config->GetSam25DocPriorWeight())
               : 0.0;
          FinalHit.MatchedScore += SourceScoreBonus;
          FinalHit.Breakdown.FinalScore = FinalHit.MatchedScore;
          FinalHit.MatchedPath = ClassifySAMMatchedPath(FinalHit);
          if (FinalHit.TermOrigin.empty())
          {
               FinalHit.TermOrigin = FinalHit.MatchedSource;
          }
          if (IsSAM25DebugExplainEnabled() && !FinalHit.Explain.empty())
          {
               std::ostringstream Stream;
               Stream << FinalHit.Explain
                      << " evidence_bonus=" << EvidenceBonus
                      << " semantic_bonus=" << SemanticBonus
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

     Score = ClampSAMScore(Score + ComputeSingleTokenLiteralBias(QueryViews, TermTokens));
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

void AppendFuzzyFallbackHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                             rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& Query,
                             size_t Limit)
{
     if (!Database || Limit == 0)
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

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          if (!Collection.empty() && Entry.Collection != Collection)
          {
               continue;
          }

          const std::string DedupKey = Entry.Collection + "/" + Entry.DocumentID;

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
               BestHit.TermOrigin = Term.Source;
               BestHit.MatchedPath = "sam_term";
               BestHit.MatchedScore = FuzzyScore;
               BestHit.MatchedSignal = Term.Signal;
               BestHit.Breakdown.TermScore = FuzzyScore;
               BestHit.Breakdown.FinalScore = FuzzyScore;
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
               BestHit.TermOrigin = BestHit.MatchedSource;
               BestHit.MatchedPath = "source_doc";
               BestHit.MatchedScore = EffectiveSourceScore;
               BestHit.MatchedSignal = EffectiveSourceScore;
               BestHit.Breakdown.TermScore = 0.0;
               BestHit.Breakdown.SourceDocScore = EffectiveSourceScore;
               BestHit.Breakdown.FinalScore = EffectiveSourceScore;

               if (IsSAM25DebugExplainEnabled())
               {
                    BestHit.Explain = SourceMatch.Explain;
               }
          }
          else if (BestScore > 0.0 && EffectiveSourceScore > 0.0)
          {
               const double MergeBonus = std::min(SourceDocMergeBonus, EffectiveSourceScore);
               BestHit.MatchedScore += MergeBonus;
               BestHit.Breakdown.SourceDocScore = EffectiveSourceScore;
               BestHit.Breakdown.SourceDocBonus += MergeBonus;
               BestHit.Breakdown.FinalScore = BestHit.MatchedScore;
               BestHit.MatchedPath = "hybrid";

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

     for (auto& Candidate : Candidates)
     {
          if (!Candidate.Collection.empty() && !Candidate.DocumentID.empty())
          {
               AccumulateSAMHit(AggregatedHits, Candidate);
          }
     }
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

std::string BuildCollectionProfileKey(const std::string& Collection)
{
     return "sam:profile:" + Collection;
}

std::string BuildSearchIdeaPrefix(const std::string& Collection)
{
     return "sam:idea:" + Collection + ":";
}

std::string BuildSearchIdeaKey(const std::string& Collection, const std::string& NormalizedQuery)
{
     return BuildSearchIdeaPrefix(Collection) + NormalizedQuery;
}

std::string BuildCollectionStateKey(const std::string& Collection)
{
     return "sam:state:" + Collection;
}

constexpr size_t kSAMSearchIdeasMaxEntries = 100;
constexpr size_t kSAMSearchIdeaMaxDocs = 6;
constexpr uint64_t kSAMSearchIdeaRecentWindowMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kSAMSearchIdeaProfileMinUses = 2;
constexpr uint64_t kSAMSearchIdeaProfileSyncCooldownMs = 60ULL * 60ULL * 1000ULL;
constexpr size_t kSAMSearchIdeaProfileForceSyncMinDelta = 4;

uint64_t GetSAMCurrentTimeMS()
{
     return static_cast<uint64_t>(NowMs());
}

bool ShouldTrackSAMSearchIdeas(const std::string& Collection)
{
     if (Collection.empty())
     {
          return false;
     }

     return true;
}

double GetSAMIdeaFreshness(uint64_t LastSeenMS, uint64_t NowMS)
{
     if (LastSeenMS == 0 || NowMS <= LastSeenMS)
     {
          return 1.0;
     }

     const double AgeHours = static_cast<double>(NowMS - LastSeenMS) / (1000.0 * 60.0 * 60.0);
     return ClampSAMScore(1.0 / (1.0 + (AgeHours / 24.0)));
}

nlohmann::json SerializeSearchIdeaEntry(const SAM::SearchIdeaEntry& Entry)
{
     nlohmann::json Root;
     Root["collection"] = Entry.Collection;
     Root["query"] = Entry.Query;
     Root["normalized_query"] = Entry.NormalizedQuery;
     Root["first_seen_ms"] = Entry.FirstSeenMS;
     Root["last_seen_ms"] = Entry.LastSeenMS;
     Root["uses"] = Entry.Uses;
     Root["vector"] = nlohmann::json::array();
     Root["documents"] = nlohmann::json::array();
     Root["resolved_interpretation"] = Entry.ResolvedInterpretation;
     Root["resolved_conclusion"] = Entry.ResolvedConclusion;
     Root["resolved_at_ms"] = Entry.ResolvedAtMS;
     Root["resolved_uses"] = Entry.ResolvedUses;
     Root["resolved_candidates"] = nlohmann::json::array();
     Root["resolved_ranked_terms"] = nlohmann::json::array();

     for (float Value : Entry.Vector)
     {
          Root["vector"].push_back(Value);
     }

     for (const auto& Document : Entry.Documents)
     {
          Root["documents"].push_back({
               {"id", Document.DocumentID},
               {"title", Document.Title},
               {"score", Document.Score}
          });
     }

     for (const auto& Candidate : Entry.ResolvedCandidates)
     {
          Root["resolved_candidates"].push_back({
               {"text", Candidate.Text},
               {"weight", Candidate.Weight}
          });
     }

     for (const auto& RankedTerm : Entry.ResolvedRankedTerms)
     {
          Root["resolved_ranked_terms"].push_back({
               {"text", RankedTerm.Text},
               {"weight", RankedTerm.Weight}
          });
     }

     return Root;
}

bool ParseSearchIdeaEntry(const std::string& RawValue, SAM::SearchIdeaEntry& Entry)
{
     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);
          Entry = SAM::SearchIdeaEntry{};
          Entry.Collection = Root.value("collection", "");
          Entry.Query = TrimCopy(Root.value("query", ""));
          Entry.NormalizedQuery = NormalizeTerm(Root.value("normalized_query", Entry.Query));
          Entry.FirstSeenMS = Root.value("first_seen_ms", static_cast<uint64_t>(0));
          Entry.LastSeenMS = Root.value("last_seen_ms", static_cast<uint64_t>(0));
          Entry.Uses = Root.value("uses", static_cast<uint64_t>(0));
          Entry.ResolvedInterpretation = TrimCopy(Root.value("resolved_interpretation", ""));
          Entry.ResolvedConclusion = TrimCopy(Root.value("resolved_conclusion", ""));
          Entry.ResolvedAtMS = Root.value("resolved_at_ms", static_cast<uint64_t>(0));
          Entry.ResolvedUses = Root.value("resolved_uses", static_cast<uint64_t>(0));

          if (Root.contains("vector") && Root["vector"].is_array())
          {
               for (const auto& Value : Root["vector"])
               {
                    Entry.Vector.push_back(Value.get<float>());
               }
          }

          if (Root.contains("documents") && Root["documents"].is_array())
          {
               for (const auto& Item : Root["documents"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    SAM::SearchIdeaDocumentRef Document;
                    Document.DocumentID = Item.value("id", "");
                    Document.Title = Item.value("title", "");
                    Document.Score = Item.value("score", 0.0);

                    if (!Document.DocumentID.empty())
                    {
                         Entry.Documents.push_back(std::move(Document));
                    }
               }
          }

          if (Root.contains("resolved_candidates") && Root["resolved_candidates"].is_array())
          {
               for (const auto& Item : Root["resolved_candidates"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    llm::SearchIntentCandidate Candidate;
                    Candidate.Text = NormalizeTerm(Item.value("text", ""));
                    Candidate.Weight = ClampSAMScore(Item.value("weight", 0.0));

                    if (!Candidate.Text.empty())
                    {
                         Entry.ResolvedCandidates.push_back(std::move(Candidate));
                    }
               }
          }

          if (Root.contains("resolved_ranked_terms") && Root["resolved_ranked_terms"].is_array())
          {
               for (const auto& Item : Root["resolved_ranked_terms"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    llm::SearchIntentCandidate RankedTerm;
                    RankedTerm.Text = NormalizeTerm(Item.value("text", ""));
                    RankedTerm.Weight = ClampSAMScore(Item.value("weight", 0.0));

                    if (!RankedTerm.Text.empty())
                    {
                         Entry.ResolvedRankedTerms.push_back(std::move(RankedTerm));
                    }
               }
          }

          if (Entry.NormalizedQuery.empty())
          {
               Entry.NormalizedQuery = NormalizeTerm(Entry.Query);
          }

          if (Entry.Query.empty())
          {
               Entry.Query = Entry.NormalizedQuery;
          }

          if (Entry.Vector.empty() && !Entry.NormalizedQuery.empty())
          {
               Entry.Vector = BuildHashedSemanticVector({Entry.NormalizedQuery});
          }

          return !Entry.Collection.empty() && !Entry.NormalizedQuery.empty();
     }
     catch (...)
     {
          return false;
     }
}

std::string BuildStrongSearchIdeaPhrase(const std::string& Value, size_t MaxTokens)
{
     const std::vector<std::string> Tokens = NormalizeSAMTokens(Value, true);
     std::vector<std::string> StrongTokens;
     StrongTokens.reserve(Tokens.size());

     for (const auto& Token : Tokens)
     {
          const std::string Singular = SingularizeToken(Token);

          if (!IsStrongSAMVariantToken(Singular))
          {
               continue;
          }

          StrongTokens.push_back(Singular);

          if (StrongTokens.size() >= MaxTokens)
          {
               break;
          }
     }

     return JoinTokens(StrongTokens);
}

std::string BuildSearchIdeaDescriptorForSubject(const std::string& QueryPhrase,
                                                const std::string& Subject)
{
     const std::vector<std::string> QueryTokens = NormalizeSAMTokens(QueryPhrase, true);
     const std::vector<std::string> SubjectTokens = NormalizeSAMTokens(Subject, true);
     std::unordered_set<std::string> SubjectSet;

     for (const auto& Token : SubjectTokens)
     {
          SubjectSet.insert(SingularizeToken(Token));
     }

     std::vector<std::string> ExtraTokens;

     for (const auto& Token : QueryTokens)
     {
          const std::string Singular = SingularizeToken(Token);

          if (SubjectSet.find(Singular) != SubjectSet.end() || !IsStrongSAMVariantToken(Singular))
          {
               continue;
          }

          ExtraTokens.push_back(Singular);

          if (ExtraTokens.size() >= 3)
          {
               break;
          }
     }

     return JoinTokens(ExtraTokens);
}

double ComputeSearchIdeaProfilePromotionScore(const SAM::SearchIdeaEntry& Entry, uint64_t NowMS)
{
     const double Popularity = ClampSAMScore(std::log1p(static_cast<double>(Entry.Uses)) / std::log(12.0));
     const double Freshness = GetSAMIdeaFreshness(Entry.LastSeenMS, NowMS);
     const double DocConsensus = ClampSAMScore(static_cast<double>(std::min<size_t>(Entry.Documents.size(), 3)) / 3.0);
     return ClampSAMScore((Popularity * 0.46) + (Freshness * 0.34) + (DocConsensus * 0.20));
}

bool SearchIntentCandidateWeightGreater(const llm::SearchIntentCandidate& Left,
                                        const llm::SearchIntentCandidate& Right)
{
     if (Left.Weight != Right.Weight)
     {
          return Left.Weight > Right.Weight;
     }

     return Left.Text < Right.Text;
}

uint64_t GetLatestRecentSearchIdeaTimestampLocked(rocksdb::DB* Database,
                                                  const std::string& Collection,
                                                  size_t* RecentCount = nullptr)
{
     if (RecentCount)
     {
          *RecentCount = 0;
     }

     if (!Database || Collection.empty())
     {
          return 0;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     uint64_t LatestSeenMS = 0;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          if (Entry.LastSeenMS == 0 || NowMS <= Entry.LastSeenMS ||
              (NowMS - Entry.LastSeenMS) > kSAMSearchIdeaRecentWindowMs)
          {
               continue;
          }

          if (Entry.Uses < kSAMSearchIdeaProfileMinUses)
          {
               continue;
          }

          const std::string StrongPhrase = BuildStrongSearchIdeaPhrase(Entry.Query);

          if (StrongPhrase.empty())
          {
               continue;
          }

          LatestSeenMS = std::max(LatestSeenMS, Entry.LastSeenMS);

          if (RecentCount)
          {
               ++(*RecentCount);
          }
     }

     return LatestSeenMS;
}

bool ReadCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                const std::string& Collection,
                                                uint64_t& Version,
                                                std::string* ErrorMessage = nullptr)
{
     Version = 0;

     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection state lookup requires an open database and collection.";
          }

          return false;
     }

     std::string RawValue;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionStateKey(Collection), &RawValue);

     if (Status.IsNotFound())
     {
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

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);

          if (!Root.contains("indexed_mutation_version"))
          {
               return false;
          }

          if (Root["indexed_mutation_version"].is_number_unsigned())
          {
               Version = Root["indexed_mutation_version"].get<uint64_t>();
               return true;
          }

          if (Root["indexed_mutation_version"].is_number_integer())
          {
               const auto SignedVersion = Root["indexed_mutation_version"].get<long long>();

               if (SignedVersion < 0)
               {
                    return false;
               }

               Version = static_cast<uint64_t>(SignedVersion);
               return true;
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }
     }

     return false;
}

bool WriteCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 uint64_t Version,
                                                 std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection state write requires an open database and collection.";
          }

          return false;
     }

     nlohmann::json State;
     State["collection"] = Collection;
     State["indexed_mutation_version"] = Version;
     State["indexed_at_ms"] = Instance ? Instance->NowMs() : 0;

     const rocksdb::Status Status =
          Database->Put(rocksdb::WriteOptions(), BuildCollectionStateKey(Collection), State.dump());

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

std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID)
{
     return "sam:term:" + Term + ":" + Collection + ":" + DocumentID;
}

void MergeRecentSearchIdeasIntoCollectionProfileLocked(
     rocksdb::DB* Database,
     const std::string& Collection,
     std::unordered_map<std::string, SAMProfileEntry>& LearnedRankedTerms,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedRelatedCounts,
     std::unordered_map<std::string, SAMProfileFamily>& LearnedFamilies,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyAliasCounts,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyDescriptorCounts,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyQueryCounts,
     uint64_t* LatestIdeaSeenMS = nullptr,
     size_t* LearnedIdeaCount = nullptr)
{
     if (LatestIdeaSeenMS)
     {
          *LatestIdeaSeenMS = 0;
     }

     if (LearnedIdeaCount)
     {
          *LearnedIdeaCount = 0;
     }

     if (!Database || Collection.empty())
     {
          return;
     }

     struct RankedIdea
     {
          SAM::SearchIdeaEntry Entry;
          std::string QueryPhrase;
          double Score = 0.0;
     };

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<RankedIdea> Ideas;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) ||
              Entry.LastSeenMS == 0 ||
              NowMS <= Entry.LastSeenMS ||
              (NowMS - Entry.LastSeenMS) > kSAMSearchIdeaRecentWindowMs ||
              Entry.Uses < kSAMSearchIdeaProfileMinUses)
          {
               continue;
          }

          const std::string QueryPhrase = BuildStrongSearchIdeaPhrase(Entry.Query);

          if (QueryPhrase.empty() || !IsCollectionProfileCandidate(QueryPhrase))
          {
               continue;
          }

          const double Score = ComputeSearchIdeaProfilePromotionScore(Entry, NowMS);

          if (Score < 0.48)
          {
               continue;
          }

          Ideas.push_back({std::move(Entry), QueryPhrase, Score});
     }

     std::sort(Ideas.begin(), Ideas.end(),
               [](const RankedIdea& A, const RankedIdea& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Entry.Uses != B.Entry.Uses)
                    {
                         return A.Entry.Uses > B.Entry.Uses;
                    }

                    return A.Entry.LastSeenMS > B.Entry.LastSeenMS;
               });

     if (Ideas.size() > 24)
     {
          Ideas.resize(24);
     }

     for (const auto& Idea : Ideas)
     {
          if (LatestIdeaSeenMS)
          {
               *LatestIdeaSeenMS = std::max(*LatestIdeaSeenMS, Idea.Entry.LastSeenMS);
          }

          if (LearnedIdeaCount)
          {
               ++(*LearnedIdeaCount);
          }

          SAMProfileEntry& Ranked = LearnedRankedTerms[Idea.QueryPhrase];
          Ranked.Text = Idea.QueryPhrase;
          Ranked.Score += 0.22 + (Idea.Score * 0.34);
          Ranked.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));

          for (const auto& RankedTerm : Idea.Entry.ResolvedRankedTerms)
          {
               if (RankedTerm.Text.empty() || !IsCollectionProfileCandidate(RankedTerm.Text))
               {
                    continue;
               }

               SAMProfileEntry& ResolvedRanked = LearnedRankedTerms[RankedTerm.Text];
               ResolvedRanked.Text = RankedTerm.Text;
               ResolvedRanked.Score += 0.28 + (Idea.Score * 0.30) + (ClampSAMScore(RankedTerm.Weight) * 0.34);
               ResolvedRanked.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 5));
          }

          std::unordered_set<std::string> SeenSubjects;

          for (const auto& Candidate : Idea.Entry.ResolvedCandidates)
          {
               if (Candidate.Text.empty())
               {
                    continue;
               }

               SAMProfileFamily& CandidateFamily = LearnedFamilies[Candidate.Text];
               CandidateFamily.Subject = Candidate.Text;
               CandidateFamily.Score += 0.20 + (Idea.Score * 0.16) + (ClampSAMScore(Candidate.Weight) * 0.24);
               CandidateFamily.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));
               LearnedFamilyQueryCounts[Candidate.Text][Idea.QueryPhrase] +=
                    static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));
          }

          for (const auto& DocumentRef : Idea.Entry.Documents)
          {
               if (DocumentRef.DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status Status =
                    Database->Get(rocksdb::ReadOptions(),
                                  BuildDocManifestKey(Collection, DocumentRef.DocumentID),
                                  &ManifestValue);

               if (!Status.ok())
               {
                    continue;
               }

               SAM::DocumentEntry DocumentEntry;

               if (!ParseManifestValue(ManifestValue, DocumentEntry))
               {
                    continue;
               }

               const std::string AnchorSubject = SelectProfileAnchorSubject(DocumentEntry);

               if (AnchorSubject.empty())
               {
                    continue;
               }

               LearnedRelatedCounts[Idea.QueryPhrase][AnchorSubject] += 1;

               if (!SeenSubjects.insert(AnchorSubject).second)
               {
                    continue;
               }

               SAMProfileFamily& Family = LearnedFamilies[AnchorSubject];
               Family.Subject = AnchorSubject;
               Family.Score += 0.16 + (Idea.Score * 0.20);
               Family.Support += 1;
               LearnedFamilyQueryCounts[AnchorSubject][Idea.QueryPhrase] +=
                    static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));

               const std::string DescriptorCandidate =
                    BuildSearchIdeaDescriptorForSubject(Idea.QueryPhrase, AnchorSubject);

               if (!DescriptorCandidate.empty() && IsCollectionProfileCandidate(DescriptorCandidate))
               {
                    LearnedFamilyDescriptorCounts[AnchorSubject][DescriptorCandidate] +=
                         static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 3));
                    LearnedRelatedCounts[Idea.QueryPhrase][DescriptorCandidate] += 1;
               }

               const std::string NormalizedTitle = NormalizeTerm(DocumentEntry.Title);

               if (!NormalizedTitle.empty() && NormalizedTitle != AnchorSubject)
               {
                    LearnedFamilyAliasCounts[AnchorSubject][NormalizedTitle] += 1;
               }
          }
     }
}

bool RebuildCollectionProfileLocked(rocksdb::DB* Database,
                                    const std::string& Collection,
                                    std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection profile rebuild requires an open database and collection.";
          }

          return false;
     }

     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::unordered_map<std::string, SAMProfileEntry> RankedTerms;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> RelatedCounts;
     std::unordered_map<std::string, SAMProfileFamily> Families;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyAliasCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyDescriptorCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyQueryCounts;
     std::unordered_map<std::string, SAMProfileEntry> LearnedRankedTerms;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedRelatedCounts;
     std::unordered_map<std::string, SAMProfileFamily> LearnedFamilies;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyAliasCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyDescriptorCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyQueryCounts;
     size_t DocumentCount = 0;

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix);
          Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) || Entry.Terms.empty())
          {
               continue;
          }

          const std::string AnchorSubject = SelectProfileAnchorSubject(Entry);
          std::unordered_set<std::string> AliasTerms;
          std::unordered_set<std::string> DescriptorTerms;
          std::vector<std::pair<std::string, double>> RankedDescriptors;

          if (!AnchorSubject.empty())
          {
               AliasTerms.insert(AnchorSubject);
          }

          const std::string NormalizedTitle = NormalizeTerm(Entry.Title);

          if (!NormalizedTitle.empty())
          {
               AliasTerms.insert(NormalizedTitle);
          }

          ++DocumentCount;
          std::vector<std::string> SeedTerms;
          std::unordered_set<std::string> SeenInDoc;
          size_t AddedSeeds = 0;

          for (const auto& Term : Entry.Terms)
          {
               const std::string Candidate = NormalizeTerm(Term.Text);

               if (Candidate.empty())
               {
                    continue;
               }

               if (IsSubjectLikeTermKind(Term.Kind) && (Candidate == AnchorSubject || IsCollectionProfileCandidate(Candidate)))
               {
                    AliasTerms.insert(Candidate);
               }
               else if (IsDescriptorLikeTermKind(Term.Kind) && IsCollectionProfileCandidate(Candidate))
               {
                    DescriptorTerms.insert(Candidate);
                    RankedDescriptors.emplace_back(
                         Candidate,
                         (ClampSAMScore(Term.Score) * 0.65) +
                         (ClampSAMScore(Term.Signal) * 0.25) +
                         (GetCollectionProfileKindWeight(Term.Kind) * 0.10));
               }

               if (!IsCollectionProfileCandidate(Candidate) || !SeenInDoc.insert(Candidate).second)
               {
                    continue;
               }

               SAMProfileEntry& Ranked = RankedTerms[Candidate];
               Ranked.Text = Candidate;
               Ranked.Score += (ClampSAMScore(Term.Score) * 0.70) + (ClampSAMScore(Term.Signal) * 0.30);
               ++Ranked.Support;
               SeedTerms.push_back(Candidate);
               ++AddedSeeds;

               if (AddedSeeds >= 8)
               {
                    break;
               }
          }

          if (!AnchorSubject.empty())
          {
               SAMProfileFamily& Family = Families[AnchorSubject];
               Family.Subject = AnchorSubject;
               Family.Score += 0.78;
               ++Family.Support;

               for (const auto& Alias : AliasTerms)
               {
                    if (!Alias.empty() && Alias != AnchorSubject)
                    {
                         ++FamilyAliasCounts[AnchorSubject][Alias];
                    }
               }

               std::sort(RankedDescriptors.begin(), RankedDescriptors.end(),
                         [](const auto& A, const auto& B)
                         {
                              if (A.second != B.second)
                              {
                                   return A.second > B.second;
                              }

                              return A.first < B.first;
                         });

               size_t DescriptorLimit = 0;

               for (const auto& RankedDescriptor : RankedDescriptors)
               {
                    if (!DescriptorTerms.contains(RankedDescriptor.first))
                    {
                         continue;
                    }

                    ++FamilyDescriptorCounts[AnchorSubject][RankedDescriptor.first];
                    ++DescriptorLimit;

                    if (DescriptorLimit >= 8)
                    {
                         break;
                    }
               }

               DescriptorLimit = 0;

               for (const auto& RankedDescriptor : RankedDescriptors)
               {
                    if (!DescriptorTerms.contains(RankedDescriptor.first))
                    {
                         continue;
                    }

                    const std::string QueryCandidate = AnchorSubject + " " + RankedDescriptor.first;

                    if (IsCollectionProfileCandidate(QueryCandidate))
                    {
                         ++FamilyQueryCounts[AnchorSubject][QueryCandidate];
                    }

                    ++DescriptorLimit;

                    if (DescriptorLimit >= 5)
                    {
                         break;
                    }
               }
          }

          for (size_t I = 0; I < SeedTerms.size(); ++I)
          {
               for (size_t J = 0; J < SeedTerms.size(); ++J)
               {
                    if (I == J)
                    {
                         continue;
                    }

                    ++RelatedCounts[SeedTerms[I]][SeedTerms[J]];
               }
          }
     }

     nlohmann::json Profile;
     Profile["collection"] = Collection;
     Profile["documents"] = DocumentCount;
     Profile["terms"] = nlohmann::json::array();
     Profile["families"] = nlohmann::json::array();
     Profile["learned_terms"] = nlohmann::json::array();
     Profile["learned_families"] = nlohmann::json::array();
     uint64_t LatestIdeaSeenMS = 0;
     size_t LearnedIdeaCount = 0;
     const uint64_t ProfileSyncedAtMS = GetSAMCurrentTimeMS();
     size_t RecentIdeaCount = 0;
     (void)GetLatestRecentSearchIdeaTimestampLocked(Database, Collection, &RecentIdeaCount);
     MergeRecentSearchIdeasIntoCollectionProfileLocked(Database,
                                                       Collection,
                                                       LearnedRankedTerms,
                                                       LearnedRelatedCounts,
                                                       LearnedFamilies,
                                                       LearnedFamilyAliasCounts,
                                                       LearnedFamilyDescriptorCounts,
                                                       LearnedFamilyQueryCounts,
                                                       &LatestIdeaSeenMS,
                                                       &LearnedIdeaCount);
     Profile["idea_sync_marker_ms"] = LatestIdeaSeenMS;
     Profile["idea_terms_merged"] = LearnedIdeaCount;
     Profile["idea_profile_synced_at_ms"] = ProfileSyncedAtMS;
     Profile["idea_recent_count"] = RecentIdeaCount;
     const size_t MinTermSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinRelatedSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilySupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyDescriptorSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyAliasSupport = 1;
     const size_t MinFamilyQuerySupport = 1;

     auto BuildSortedTerms = [&](std::unordered_map<std::string, SAMProfileEntry>& SourceTerms,
                                 std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceRelated)
     {
          std::vector<SAMProfileEntry> SortedTerms;
          SortedTerms.reserve(SourceTerms.size());

          for (auto& Pair : SourceTerms)
          {
               SAMProfileEntry Entry = Pair.second;

               if (Entry.Support < MinTermSupport)
               {
                    continue;
               }

               auto RelatedIt = SourceRelated.find(Entry.Text);

               if (RelatedIt != SourceRelated.end())
               {
                    std::vector<std::pair<std::string, size_t>> Related(RelatedIt->second.begin(), RelatedIt->second.end());
                    std::sort(Related.begin(), Related.end(),
                              [](const auto& A, const auto& B)
                              {
                                   if (A.second != B.second)
                                   {
                                        return A.second > B.second;
                                   }

                                   return A.first < B.first;
                              });

                    for (const auto& RelatedPair : Related)
                    {
                         if (RelatedPair.second < MinRelatedSupport)
                         {
                              continue;
                         }

                         Entry.Related.push_back(RelatedPair.first);

                         if (Entry.Related.size() >= 6)
                         {
                              break;
                         }
                    }
               }

               Entry.Score = ClampSAMScore(Entry.Score / static_cast<double>(std::max<size_t>(1, Entry.Support)));
               SortedTerms.push_back(std::move(Entry));
          }

          std::sort(SortedTerms.begin(), SortedTerms.end(),
                    [](const SAMProfileEntry& A, const SAMProfileEntry& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         if (A.Text.size() != B.Text.size())
                         {
                              return A.Text.size() < B.Text.size();
                         }

                         return A.Text < B.Text;
                    });

          if (SortedTerms.size() > 48)
          {
               SortedTerms.resize(48);
          }

          return SortedTerms;
     };

     auto AppendRankedValues = [](const auto& Source, std::vector<std::string>& Output, size_t MinSupport, size_t MaxItems)
     {
          std::vector<std::pair<std::string, size_t>> Ranked(Source.begin(), Source.end());
          std::sort(Ranked.begin(), Ranked.end(),
                    [](const auto& A, const auto& B)
                    {
                         if (A.second != B.second)
                         {
                              return A.second > B.second;
                         }

                         return A.first < B.first;
                    });

          for (const auto& Item : Ranked)
          {
               if (Item.second < MinSupport)
               {
                    continue;
               }

               Output.push_back(Item.first);

               if (Output.size() >= MaxItems)
               {
                    break;
               }
          }
     };

     auto BuildSortedFamilies = [&](std::unordered_map<std::string, SAMProfileFamily>& SourceFamilies,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyAliasCounts,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyDescriptorCounts,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyQueryCounts)
     {
          std::vector<SAMProfileFamily> SortedFamilies;
          SortedFamilies.reserve(SourceFamilies.size());

          for (auto& Pair : SourceFamilies)
          {
               SAMProfileFamily Family = Pair.second;

               if (Family.Support < MinFamilySupport)
               {
                    continue;
               }

               if (const auto AliasIt = SourceFamilyAliasCounts.find(Family.Subject); AliasIt != SourceFamilyAliasCounts.end())
               {
                    AppendRankedValues(AliasIt->second, Family.Aliases, MinFamilyAliasSupport, 6);
               }

               if (const auto DescriptorIt = SourceFamilyDescriptorCounts.find(Family.Subject);
                   DescriptorIt != SourceFamilyDescriptorCounts.end())
               {
                    AppendRankedValues(DescriptorIt->second, Family.Descriptors, MinFamilyDescriptorSupport, 8);
               }

               if (const auto QueryIt = SourceFamilyQueryCounts.find(Family.Subject); QueryIt != SourceFamilyQueryCounts.end())
               {
                    AppendRankedValues(QueryIt->second, Family.Queries, MinFamilyQuerySupport, 8);
               }

               Family.Score = ClampSAMScore(Family.Score / static_cast<double>(std::max<size_t>(1, Family.Support)));
               SortedFamilies.push_back(std::move(Family));
          }

          std::sort(SortedFamilies.begin(), SortedFamilies.end(),
                    [](const SAMProfileFamily& A, const SAMProfileFamily& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         return A.Subject < B.Subject;
                    });

          if (SortedFamilies.size() > 24)
          {
               SortedFamilies.resize(24);
          }

          return SortedFamilies;
     };

     auto AppendTermsToProfile = [&](const std::vector<SAMProfileEntry>& SortedTerms, const char* Key)
     {
          for (const auto& Entry : SortedTerms)
          {
               Profile[Key].push_back({
                    {"text", Entry.Text},
                    {"score", Entry.Score},
                    {"support", Entry.Support},
                    {"related", Entry.Related}
               });
          }
     };

     auto AppendFamiliesToProfile = [&](const std::vector<SAMProfileFamily>& SortedFamilies, const char* Key)
     {
          for (const auto& Family : SortedFamilies)
          {
               Profile[Key].push_back({
                    {"subject", Family.Subject},
                    {"score", Family.Score},
                    {"support", Family.Support},
                    {"aliases", Family.Aliases},
                    {"descriptors", Family.Descriptors},
                    {"queries", Family.Queries}
               });
          }
     };

     AppendTermsToProfile(BuildSortedTerms(RankedTerms, RelatedCounts), "terms");
     AppendFamiliesToProfile(BuildSortedFamilies(Families,
                                                FamilyAliasCounts,
                                                FamilyDescriptorCounts,
                                                FamilyQueryCounts),
                             "families");
     AppendTermsToProfile(BuildSortedTerms(LearnedRankedTerms, LearnedRelatedCounts), "learned_terms");
     AppendFamiliesToProfile(BuildSortedFamilies(LearnedFamilies,
                                                LearnedFamilyAliasCounts,
                                                LearnedFamilyDescriptorCounts,
                                                LearnedFamilyQueryCounts),
                             "learned_families");

     const rocksdb::Status Status =
          Database->Put(rocksdb::WriteOptions(), BuildCollectionProfileKey(Collection), Profile.dump());

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

std::vector<std::string> BuildPersistedCollectionProfileVariants(rocksdb::DB* Database,
                                                                 const std::string& Collection,
                                                                 const SAMQueryTokenViews& QueryViews,
                                                                 const std::unordered_set<std::string>* StrongTokens = nullptr,
                                                                 size_t MaxVariants = 12)
{
     std::vector<std::string> Variants;

     if (!Database || Collection.empty() || QueryViews.CoreTokens.empty() || MaxVariants == 0 ||
         IsSingleTokenSAMIntent(QueryViews))
     {
          return Variants;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (!Status.ok() || RawProfile.empty())
     {
          return Variants;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawProfile);

          const bool HasTerms = Root.contains("terms") && Root["terms"].is_array();
          const bool HasLearnedTerms = Root.contains("learned_terms") && Root["learned_terms"].is_array();
          const bool HasFamilies = Root.contains("families") && Root["families"].is_array();
          const bool HasLearnedFamilies = Root.contains("learned_families") && Root["learned_families"].is_array();

          if (!HasTerms && !HasLearnedTerms && !HasFamilies && !HasLearnedFamilies)
          {
               return Variants;
          }

          std::unordered_set<std::string> QueryTokens(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
          std::unordered_map<std::string, SAMLearnedVariant> Ranked;

          auto ProcessTermArray = [&](const nlohmann::json& TermsArray, double LayerWeight)
          {
               for (const auto& Item : TermsArray)
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    const std::string Text = NormalizeTerm(Item.value("text", ""));

                    if (Text.empty())
                    {
                         continue;
                    }

                    const double EntryScore = ClampSAMScore(Item.value("score", 0.0) * LayerWeight);
                    const size_t Support = static_cast<size_t>(std::max<int64_t>(0, Item.value("support", 0)));
                    const double MatchStrength =
                         IsUsefulLearnedVariant(Text, QueryTokens, StrongTokens) ? 0.0 :
                         std::max(ComputeManifestSeedStrength(
                                       QueryViews,
                                       SAM::TermEntry{Text, "collection_profile", "profile", EntryScore, EntryScore}),
                                  0.0);

                    if (MatchStrength < (LayerWeight < 1.0 ? 0.62 : 0.58))
                    {
                         continue;
                    }

                    if (Item.contains("related") && Item["related"].is_array())
                    {
                         for (const auto& RelatedItem : Item["related"])
                         {
                              if (!RelatedItem.is_string())
                              {
                                   continue;
                              }

                              const std::string Candidate = NormalizeTerm(RelatedItem.get<std::string>());

                              if (!IsUsefulLearnedVariant(Candidate, QueryTokens, StrongTokens))
                              {
                                   continue;
                              }

                              SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                              RankedEntry.Text = Candidate;
                              RankedEntry.Score += (MatchStrength * (0.60 * LayerWeight)) + (EntryScore * 0.25) +
                                                   ClampSAMScore(static_cast<double>(Support) / 8.0) * 0.15;
                              ++RankedEntry.Support;
                         }
                    }
               }
          };

          auto ProcessFamilyArray = [&](const nlohmann::json& FamiliesArray, double LayerWeight)
          {
               for (const auto& FamilyItem : FamiliesArray)
               {
                    if (!FamilyItem.is_object())
                    {
                         continue;
                    }

                    const std::string Subject = NormalizeTerm(FamilyItem.value("subject", ""));

                    if (Subject.empty())
                    {
                         continue;
                    }

                    const double FamilyScore = ClampSAMScore(FamilyItem.value("score", 0.0) * LayerWeight);
                    const size_t FamilySupport =
                         static_cast<size_t>(std::max<int64_t>(0, FamilyItem.value("support", 0)));

                    std::vector<std::string> MatchTerms;
                    MatchTerms.push_back(Subject);

                    if (FamilyItem.contains("aliases") && FamilyItem["aliases"].is_array())
                    {
                         for (const auto& AliasItem : FamilyItem["aliases"])
                         {
                              if (AliasItem.is_string())
                              {
                                   MatchTerms.push_back(NormalizeTerm(AliasItem.get<std::string>()));
                              }
                         }
                    }

                    if (FamilyItem.contains("descriptors") && FamilyItem["descriptors"].is_array())
                    {
                         for (const auto& DescriptorItem : FamilyItem["descriptors"])
                         {
                              if (DescriptorItem.is_string())
                              {
                                   MatchTerms.push_back(NormalizeTerm(DescriptorItem.get<std::string>()));
                              }
                         }
                    }

                    double FamilyMatch = 0.0;

                    for (const auto& MatchTerm : MatchTerms)
                    {
                         if (MatchTerm.empty())
                         {
                              continue;
                         }

                         FamilyMatch = std::max(
                              FamilyMatch,
                              std::max(ComputeManifestSeedStrength(
                                            QueryViews,
                                            SAM::TermEntry{MatchTerm, "collection_profile", "profile", FamilyScore, FamilyScore}),
                                       0.0));
                    }

                    if (FamilyMatch < (LayerWeight < 1.0 ? 0.60 : 0.56))
                    {
                         continue;
                    }

                    auto AccumulateVariant = [&](const std::string& CandidateText, double Weight)
                    {
                         const std::string Candidate = NormalizeTerm(CandidateText);

                         if (!IsUsefulLearnedVariant(Candidate, QueryTokens, StrongTokens))
                         {
                              return;
                         }

                         SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                         RankedEntry.Text = Candidate;
                         RankedEntry.Score += (FamilyMatch * (Weight * LayerWeight)) +
                                              (FamilyScore * 0.22) +
                                              ClampSAMScore(static_cast<double>(FamilySupport) / 8.0) * 0.14;
                         ++RankedEntry.Support;
                    };

                    AccumulateVariant(Subject, 0.46);

                    if (FamilyItem.contains("aliases") && FamilyItem["aliases"].is_array())
                    {
                         for (const auto& AliasItem : FamilyItem["aliases"])
                         {
                              if (AliasItem.is_string())
                              {
                                   AccumulateVariant(AliasItem.get<std::string>(), 0.44);
                              }
                         }
                    }

                    if (FamilyItem.contains("descriptors") && FamilyItem["descriptors"].is_array())
                    {
                         for (const auto& DescriptorItem : FamilyItem["descriptors"])
                         {
                              if (DescriptorItem.is_string())
                              {
                                   AccumulateVariant(DescriptorItem.get<std::string>(), 0.52);
                              }
                         }
                    }

                    if (FamilyItem.contains("queries") && FamilyItem["queries"].is_array())
                    {
                         for (const auto& QueryItem : FamilyItem["queries"])
                         {
                              if (QueryItem.is_string())
                              {
                                   AccumulateVariant(QueryItem.get<std::string>(), 0.60);
                              }
                         }
                    }
               }
          };

          if (HasTerms)
          {
               ProcessTermArray(Root["terms"], 1.0);
          }

          if (HasLearnedTerms)
          {
               ProcessTermArray(Root["learned_terms"], 0.78);
          }

          if (HasFamilies)
          {
               ProcessFamilyArray(Root["families"], 1.0);
          }

          if (HasLearnedFamilies)
          {
               ProcessFamilyArray(Root["learned_families"], 0.80);
          }

          std::vector<SAMLearnedVariant> Sorted;
          Sorted.reserve(Ranked.size());

          for (const auto& Pair : Ranked)
          {
               if (Pair.second.Score >= 0.62)
               {
                    Sorted.push_back(Pair.second);
               }
          }

          std::sort(Sorted.begin(), Sorted.end(),
                    [](const SAMLearnedVariant& A, const SAMLearnedVariant& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         return A.Text < B.Text;
                    });

          for (const auto& Entry : Sorted)
          {
               Variants.push_back(Entry.Text);

               if (Variants.size() >= MaxVariants)
               {
                    break;
               }
          }
     }
     catch (...)
     {
     }

     return Variants;
}

bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry)
{
     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);
          SAMSemanticProfile SemanticProfile;
          Entry.Collection = Root.value("collection", "");
          Entry.DocumentID = Root.value("id", "");
          Entry.Title = Root.value("title", "");
          Entry.SourceTimestamp = Root.value("source_timestamp", 0ULL);
          Entry.SourceFingerprint = Root.value("source_fingerprint", "");
          Entry.Lang = Root.value("lang", "und");
          Entry.Label = Root.value("label", "article");
          Entry.Format = Root.value("format", "text");
          Entry.Subject.clear();
          Entry.Summary.clear();
          Entry.Aliases.clear();
          Entry.Descriptors.clear();
          Entry.Queries.clear();
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

          if (ParseSemanticProfileJSON(Root, SemanticProfile))
          {
               Entry.Subject = SemanticProfile.Subject;
               Entry.Summary = SemanticProfile.Summary;
               Entry.Aliases = std::move(SemanticProfile.Aliases);
               Entry.Descriptors = std::move(SemanticProfile.Descriptors);
               Entry.Queries = std::move(SemanticProfile.Queries);
          }

          return !Entry.Collection.empty() && !Entry.DocumentID.empty();
     }
     catch (...)
     {
          return false;
     }
}

namespace
{
bool IsNumericLikeSAMToken(const std::string& Token)
{
     return !Token.empty() &&
            std::all_of(Token.begin(), Token.end(),
                        [](unsigned char C)
                        {
                             return std::isdigit(C);
                        });
}

bool IsIdentifierLikeSAMValue(const std::string& Value)
{
     if (Value.empty())
     {
          return true;
     }

     size_t AlphaCount = 0;
     size_t DigitCount = 0;
     size_t SeparatorCount = 0;

     for (unsigned char C : Value)
     {
          if (std::isalpha(C))
          {
               ++AlphaCount;
          }
          else if (std::isdigit(C))
          {
               ++DigitCount;
          }
          else if (C == '-' || C == '_' || C == '/' || C == '.' || C == ':')
          {
               ++SeparatorCount;
          }
     }

     if (Value.find("://") != std::string::npos)
     {
          return true;
     }

     if (Value.find('@') != std::string::npos)
     {
          return true;
     }

     if (Value.find(' ') == std::string::npos &&
         DigitCount > 0 &&
         SeparatorCount > 0 &&
         AlphaCount <= DigitCount + 2)
     {
          return true;
     }

     return false;
}

bool IsTitleLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "title" || LowerKey == "name" || LowerKey == "headline" ||
            LowerKey == "subject" || LowerKey == "heading";
}

bool IsSummaryLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "summary" || LowerKey == "description" || LowerKey == "excerpt" ||
            LowerKey == "overview" || LowerKey == "abstract" || LowerKey == "subtitle";
}

bool IsBodyLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "content" || LowerKey == "body" || LowerKey == "text" ||
            LowerKey == "article" || LowerKey == "markdown" || LowerKey == "notes";
}

bool IsAliasLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "alias" || LowerKey == "aliases" || LowerKey == "slug" ||
            LowerKey == "handle" || LowerKey == "username" || LowerKey == "short_name" ||
            LowerKey == "nickname";
}

bool IsTaxonomyLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "tag" || LowerKey == "tags" || LowerKey == "label" ||
            LowerKey == "labels" || LowerKey == "category" || LowerKey == "categories" ||
            LowerKey == "topic" || LowerKey == "topics" || LowerKey == "genre" ||
            LowerKey == "genres" || LowerKey == "brand" || LowerKey == "brands" ||
            LowerKey == "author" || LowerKey == "authors" || LowerKey == "type";
}

bool IsQueryLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "query" || LowerKey == "queries" || LowerKey == "keywords" ||
            LowerKey == "search_terms";
}

std::vector<std::string> ExtractArrayishSAMValues(const std::string& RawValue,
                                                  size_t MaxValues = 16)
{
     std::vector<std::string> Values;
     const std::string Trimmed = TrimCopy(RawValue);

     if (Trimmed.empty())
     {
          return Values;
     }

     try
     {
          if (!Trimmed.empty() && Trimmed.front() == '[')
          {
               const nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

               if (Parsed.is_array())
               {
                    for (const auto& Entry : Parsed)
                    {
                         if (!Entry.is_string())
                         {
                              continue;
                         }

                         const std::string Candidate = TrimCopy(Entry.get<std::string>());

                         if (!Candidate.empty())
                         {
                              Values.push_back(Candidate);
                         }

                         if (Values.size() >= MaxValues)
                         {
                              break;
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
     std::istringstream Input(Trimmed);

     while (std::getline(Input, Token, ','))
     {
          size_t Start = 0;

          while (Start < Token.size())
          {
               size_t End = Token.find_first_of(";\n|", Start);
               std::string Candidate = TrimCopy(Token.substr(Start, End == std::string::npos ? std::string::npos : End - Start));

               if (!Candidate.empty())
               {
                    Values.push_back(Candidate);
               }

               if (Values.size() >= MaxValues || End == std::string::npos)
               {
                    break;
               }

               Start = End + 1;
          }

          if (Values.size() >= MaxValues)
          {
               break;
          }
     }

     if (Values.empty())
     {
          Values.push_back(Trimmed);
     }

     return Values;
}

std::vector<std::string> BuildSAMSentenceSamples(const std::string& RawValue,
                                                 size_t MaxSamples = 10)
{
     std::vector<std::string> Samples;
     std::string Current;

     for (unsigned char C : RawValue)
     {
          if (C == '\r')
          {
               continue;
          }

          if (C == '\n' || C == '.' || C == '!' || C == '?' || C == ';')
          {
               const std::string Candidate = TrimCopy(Current);

               if (!Candidate.empty())
               {
                    Samples.push_back(Candidate);
               }

               Current.clear();

               if (Samples.size() >= MaxSamples)
               {
                    break;
               }

               continue;
          }

          Current.push_back(static_cast<char>(C));
     }

     if (Samples.size() < MaxSamples)
     {
          const std::string Candidate = TrimCopy(Current);

          if (!Candidate.empty())
          {
               Samples.push_back(Candidate);
          }
     }

     return Samples;
}

size_t CountStrongSAMTokens(const std::vector<std::string>& Tokens)
{
     size_t Count = 0;

     for (const auto& Token : Tokens)
     {
          if (!Token.empty() && !IsSamStopword(Token) && !IsWeakSamToken(Token) && !IsNumericLikeSAMToken(Token))
          {
               ++Count;
          }
     }

     return Count;
}

bool IsUsefulSAMDocumentPhrase(const std::string& Value)
{
     const std::string Normalized = NormalizeTerm(Value);

     if (Normalized.empty() || Normalized.size() < 2 || Normalized.size() > 96)
     {
          return false;
     }

     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty() || Tokens.size() > 7)
     {
          return false;
     }

     const size_t StrongCount = CountStrongSAMTokens(Tokens);

     if (StrongCount == 0)
     {
          return false;
     }

     if (Tokens.size() == 1)
     {
          return StrongCount == 1 && Tokens.front().size() >= 3;
     }

     if (StrongCount == 1 && Tokens.size() >= 4)
     {
          return false;
     }

     return !IsIdentifierLikeSAMValue(Value);
}

std::string JoinSAMTokenRange(const std::vector<std::string>& Tokens,
                              size_t Start,
                              size_t End)
{
     std::string Result;

     for (size_t Index = Start; Index < End; ++Index)
     {
          if (!Result.empty())
          {
               Result.push_back(' ');
          }

          Result += Tokens[Index];
     }

     return Result;
}

std::vector<std::string> BuildSAMPhraseWindows(const std::string& Value,
                                               size_t MinTokens,
                                               size_t MaxTokens,
                                               size_t MaxPhrases)
{
     std::vector<std::string> Result;
     std::unordered_set<std::string> Seen;
     const std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Value));

     if (Tokens.empty())
     {
          return Result;
     }

     for (size_t Window = MinTokens; Window <= MaxTokens; ++Window)
     {
          if (Window == 0 || Window > Tokens.size())
          {
               continue;
          }

          for (size_t Start = 0; Start + Window <= Tokens.size(); ++Start)
          {
               const std::string Candidate = JoinSAMTokenRange(Tokens, Start, Start + Window);

               if (!IsUsefulSAMDocumentPhrase(Candidate) || !Seen.insert(Candidate).second)
               {
                    continue;
               }

               const std::vector<std::string> CandidateTokens = TokenizeNormalized(Candidate);
               const size_t StrongCount = CountStrongSAMTokens(CandidateTokens);

               if (Window >= 3 && StrongCount < 2)
               {
                    continue;
               }

               Result.push_back(Candidate);

               if (Result.size() >= MaxPhrases)
               {
                    return Result;
               }
          }
     }

     return Result;
}

std::vector<std::string> BuildSAMReducedTitleVariants(const std::string& Title)
{
     std::vector<std::string> Variants;
     const std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Title));

     if (Tokens.size() < 2)
     {
          return Variants;
     }

     std::vector<std::string> Reduced = Tokens;

     while (!Reduced.empty() &&
            (IsSamStopword(Reduced.back()) || IsWeakSamToken(Reduced.back())))
     {
          Reduced.pop_back();
     }

     if (Reduced.size() >= 2 && Reduced.size() < Tokens.size())
     {
          Variants.push_back(JoinTokens(Reduced));
     }

     Reduced = Tokens;

     while (!Reduced.empty() &&
            (IsSamStopword(Reduced.front()) || IsWeakSamToken(Reduced.front())))
     {
          Reduced.erase(Reduced.begin());
     }

     if (Reduced.size() >= 2 && Reduced.size() < Tokens.size())
     {
          Variants.push_back(JoinTokens(Reduced));
     }

     if (Tokens.size() >= 3)
     {
          Variants.push_back(JoinSAMTokenRange(Tokens, 0, Tokens.size() - 1));
          Variants.push_back(JoinSAMTokenRange(Tokens, 1, Tokens.size()));
     }

     return UniqueNormalizedPhrases(Variants, 8);
}

std::vector<std::string> BuildSAMSeedQueries(const std::vector<std::string>& Subjects,
                                             const std::vector<std::string>& Descriptors)
{
     std::vector<std::string> Queries;
     std::unordered_set<std::string> Seen;

     auto Append = [&](const std::string& Value)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (!IsUsefulSAMDocumentPhrase(Candidate) || !Seen.insert(Candidate).second)
          {
               return;
          }

          Queries.push_back(Candidate);
     };

     for (const auto& Subject : Subjects)
     {
          Append(Subject);
          Append(Subject + " guide");
          Append(Subject + " overview");
     }

     for (const auto& Subject : Subjects)
     {
          for (const auto& Descriptor : Descriptors)
          {
               Append(Subject + " " + Descriptor);
               Append(Descriptor + " " + Subject);

               if (Queries.size() >= 24)
               {
                    return Queries;
               }
          }
     }

     return Queries;
}

std::string SpellSAMEnglishUnder100(int Value)
{
     static const std::array<const char*, 20> Units = {
          "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
          "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
          "seventeen", "eighteen", "nineteen"
     };
     static const std::array<const char*, 10> Tens = {
          "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
     };

     if (Value < 0 || Value >= 100)
     {
          return "";
     }

     if (Value < 20)
     {
          return Units[static_cast<size_t>(Value)];
     }

     const int TensValue = Value / 10;
     const int UnitValue = Value % 10;

     if (UnitValue == 0)
     {
          return Tens[static_cast<size_t>(TensValue)];
     }

     return std::string(Tens[static_cast<size_t>(TensValue)]) + " " +
            Units[static_cast<size_t>(UnitValue)];
}

std::string SpellSAMSpanishUnder100(int Value)
{
     static const std::array<const char*, 30> Units = {
          "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve",
          "diez", "once", "doce", "trece", "catorce", "quince", "dieciseis", "diecisiete",
          "dieciocho", "diecinueve", "veinte", "veintiuno", "veintidos", "veintitres",
          "veinticuatro", "veinticinco", "veintiseis", "veintisiete", "veintiocho", "veintinueve"
     };
     static const std::array<const char*, 10> Tens = {
          "", "", "", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta", "ochenta", "noventa"
     };

     if (Value < 0 || Value >= 100)
     {
          return "";
     }

     if (Value < 30)
     {
          return Units[static_cast<size_t>(Value)];
     }

     const int TensValue = Value / 10;
     const int UnitValue = Value % 10;

     if (UnitValue == 0)
     {
          return Tens[static_cast<size_t>(TensValue)];
     }

     return std::string(Tens[static_cast<size_t>(TensValue)]) + " y " +
            Units[static_cast<size_t>(UnitValue)];
}

std::string SpellSAMEnglishNumber(int Value)
{
     if (Value < 0 || Value > 9999)
     {
          return "";
     }

     if (Value < 100)
     {
          return SpellSAMEnglishUnder100(Value);
     }

     if (Value < 1000)
     {
          const int Hundreds = Value / 100;
          const int Remainder = Value % 100;
          std::string Result = SpellSAMEnglishUnder100(Hundreds) + " hundred";

          if (Remainder > 0)
          {
               Result += " " + SpellSAMEnglishUnder100(Remainder);
          }

          return Result;
     }

     const int Thousands = Value / 1000;
     const int Remainder = Value % 1000;
     std::string Result = SpellSAMEnglishUnder100(Thousands) + " thousand";

     if (Remainder >= 100)
     {
          Result += " " + SpellSAMEnglishNumber(Remainder);
     }
     else if (Remainder > 0)
     {
          Result += " " + SpellSAMEnglishUnder100(Remainder);
     }

     return Result;
}

std::string SpellSAMSpanishNumber(int Value)
{
     if (Value < 0 || Value > 9999)
     {
          return "";
     }

     if (Value < 100)
     {
          return SpellSAMSpanishUnder100(Value);
     }

     if (Value == 100)
     {
          return "cien";
     }

     if (Value < 1000)
     {
          static const std::array<const char*, 10> Hundreds = {
               "", "ciento", "doscientos", "trescientos", "cuatrocientos",
               "quinientos", "seiscientos", "setecientos", "ochocientos", "novecientos"
          };

          const int HundredsValue = Value / 100;
          const int Remainder = Value % 100;
          std::string Result = Hundreds[static_cast<size_t>(HundredsValue)];

          if (Remainder > 0)
          {
               Result += " " + SpellSAMSpanishUnder100(Remainder);
          }

          return Result;
     }

     const int Thousands = Value / 1000;
     const int Remainder = Value % 1000;
     std::string Result;

     if (Thousands == 1)
     {
          Result = "mil";
     }
     else
     {
          Result = SpellSAMSpanishUnder100(Thousands) + " mil";
     }

     if (Remainder > 0)
     {
          Result += " " + SpellSAMSpanishNumber(Remainder);
     }

     return Result;
}

std::string SpellSAMNumericToken(const std::string& Token, const std::string& Lang)
{
     if (!IsNumericLikeSAMToken(Token) || Token.empty() || Token.size() > 4)
     {
          return "";
     }

     if (Token.size() > 1 && Token.front() == '0')
     {
          return "";
     }

     int Value = 0;

     try
     {
          Value = std::stoi(Token);
     }
     catch (...)
     {
          return "";
     }

     if (Lang.rfind("es", 0) == 0)
     {
          return SpellSAMSpanishNumber(Value);
     }

     if (Lang.empty() || Lang == "und" || Lang.rfind("en", 0) == 0)
     {
          return SpellSAMEnglishNumber(Value);
     }

     return "";
}

std::vector<std::string> BuildSAMNumericWordVariants(const std::string& Value,
                                                     const std::string& Lang,
                                                     size_t MaxVariants = 3)
{
     std::vector<std::string> Variants;
     const std::string Normalized = NormalizeTerm(Value);
     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty() || Tokens.size() > 6)
     {
          return Variants;
     }

     std::vector<std::string> Rewritten = Tokens;
     size_t NumericTokenCount = 0;
     bool Changed = false;

     for (size_t Index = 0; Index < Tokens.size(); ++Index)
     {
          const std::string Spelled = SpellSAMNumericToken(Tokens[Index], Lang);

          if (Spelled.empty())
          {
               continue;
          }

          Rewritten[Index] = Spelled;
          ++NumericTokenCount;
          Changed = true;
     }

     if (!Changed || NumericTokenCount == 0 || NumericTokenCount > 2)
     {
          return Variants;
     }

     const std::string Candidate = JoinTokens(Rewritten);

     if (Candidate != Normalized && IsUsefulSAMDocumentPhrase(Candidate))
     {
          Variants.push_back(Candidate);
     }

     if (Variants.size() > MaxVariants)
     {
          Variants.resize(MaxVariants);
     }

     return Variants;
}

struct SAMTermCollector
{
     std::unordered_map<std::string, SAM::TermEntry> Entries;
     std::unordered_map<std::string, size_t> Support;

     void Add(const std::string& Text,
              const std::string& Kind,
              const std::string& Source,
              double Score,
              double Signal)
     {
          const std::string Normalized = NormalizeTerm(Text);

          if (!IsUsefulSAMDocumentPhrase(Normalized))
          {
               return;
          }

          SAM::TermEntry& Entry = Entries[Normalized];
          size_t& EntrySupport = Support[Normalized];
          ++EntrySupport;

          if (Entry.Text.empty() ||
              Score > Entry.Score ||
              (Score == Entry.Score && Signal > Entry.Signal))
          {
               Entry.Text = Normalized;
               Entry.Kind = Kind;
               Entry.Source = Source;
          }

          Entry.Score = std::max(Entry.Score, ClampSAMScore(Score));
          Entry.Signal = std::max(Entry.Signal,
                                  ClampSAMScore(Signal + std::min(0.16, 0.03 * static_cast<double>(EntrySupport - 1))));
     }

     std::vector<SAM::TermEntry> Finalize(size_t Limit = 96) const
     {
          std::vector<SAM::TermEntry> Terms;
          Terms.reserve(Entries.size());

          for (const auto& Pair : Entries)
          {
               Terms.push_back(Pair.second);
          }

          std::sort(Terms.begin(), Terms.end(),
                    [](const SAM::TermEntry& Left, const SAM::TermEntry& Right)
                    {
                         if (Left.Score != Right.Score)
                         {
                              return Left.Score > Right.Score;
                         }

                         if (Left.Signal != Right.Signal)
                         {
                              return Left.Signal > Right.Signal;
                         }

                         if (Left.Text.size() != Right.Text.size())
                         {
                              return Left.Text.size() < Right.Text.size();
                         }

                         return Left.Text < Right.Text;
                    });

          if (Terms.size() > Limit)
          {
               Terms.resize(Limit);
          }

          return Terms;
     }
};

std::unordered_set<std::string> BuildSAMDocumentEvidenceTokens(const Document& Doc,
                                                               size_t MaxTokens = 512)
{
     std::unordered_set<std::string> Tokens;

     auto AppendTokens = [&](const std::string& Value)
     {
          for (const auto& Token : NormalizeSAMTokens(Value, true))
          {
               if (Token.empty())
               {
                    continue;
               }

               Tokens.insert(Token);

               if (Tokens.size() >= MaxTokens)
               {
                    return;
               }
          }
     };

     AppendTokens(Doc.Title);
     AppendTokens(Doc.Content);

     for (const auto& Field : CollectDocumentTextFields(Doc))
     {
          if (Tokens.size() >= MaxTokens)
          {
               break;
          }

          AppendTokens(Field.second);
     }

     return Tokens;
}

size_t CountSAMTokenOverlap(const std::vector<std::string>& CandidateTokens,
                            const std::unordered_set<std::string>& EvidenceTokens)
{
     size_t Overlap = 0;

     for (const auto& Token : CandidateTokens)
     {
          if (EvidenceTokens.find(Token) != EvidenceTokens.end())
          {
               ++Overlap;
          }
     }

     return Overlap;
}

bool HasConsistentSAMCandidateLanguage(const std::string& Collection,
                                       const std::string& Candidate,
                                       const std::string& DocumentLang)
{
     if (Candidate.empty() || DocumentLang.empty() || DocumentLang == "und")
     {
          return true;
     }

     Document CandidateDoc;
     CandidateDoc.ID = Candidate;
     CandidateDoc.Title = Candidate;
     const std::string CandidateLang = sam::lang::DetectDocumentLanguage(Collection, CandidateDoc);

     return CandidateLang == "und" || CandidateLang == DocumentLang;
}

bool ValidateSAMLLMCandidate(const std::string& Collection,
                             const Document& Doc,
                             const std::string& Candidate,
                             const std::string& Subject,
                             const std::unordered_set<std::string>& EvidenceTokens,
                             const std::string& DocumentLang)
{
     if (!IsUsefulSAMDocumentPhrase(Candidate) || IsIdentifierLikeSAMValue(Candidate))
     {
          return false;
     }

     if (!HasConsistentSAMCandidateLanguage(Collection, Candidate, DocumentLang))
     {
          return false;
     }

     const std::vector<std::string> CandidateTokens = NormalizeSAMTokens(Candidate, true);

     if (CandidateTokens.empty())
     {
          return false;
     }

     const size_t Overlap = CountSAMTokenOverlap(CandidateTokens, EvidenceTokens);
     const bool MentionsSubject = !Subject.empty() &&
          (Candidate == Subject ||
           Candidate.find(Subject) != std::string::npos ||
           Subject.find(Candidate) != std::string::npos);

     if (Overlap == 0 && !MentionsSubject)
     {
          return false;
     }

     if (CandidateTokens.size() >= 3 && Overlap == 0)
     {
          return false;
     }

     if (CandidateTokens.size() >= 2 && Overlap == 1 && !MentionsSubject)
     {
          return false;
     }

     (void)Doc;
     return true;
}

std::unordered_map<std::string, double> BuildSAMLLMFeedbackBoosts(rocksdb::DB* Database,
                                                                  const std::string& Collection,
                                                                  const Document& Doc,
                                                                  const std::string& Subject,
                                                                  const std::unordered_set<std::string>& EvidenceTokens,
                                                                  const std::string& DocumentLang,
                                                                  size_t MaxIdeas = 64)
{
     std::unordered_map<std::string, double> Boosts;

     if (!Database || Collection.empty() || Doc.ID.empty() || MaxIdeas == 0)
     {
          return Boosts;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     const uint64_t NowMS = GetSAMCurrentTimeMS();
     size_t SeenIdeas = 0;

     auto AddBoost = [&](const std::string& Value, double Boost)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (!ValidateSAMLLMCandidate(Collection,
                                       Doc,
                                       Candidate,
                                       Subject,
                                       EvidenceTokens,
                                       DocumentLang))
          {
               return;
          }

          Boosts[Candidate] = std::max(Boosts[Candidate], ClampSAMScore(Boost));
     };

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix) && SeenIdeas < MaxIdeas;
          Iterator->Next(), ++SeenIdeas)
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) || Entry.Uses == 0)
          {
               continue;
          }

          bool TargetsDocument = false;

          for (const auto& DocumentRef : Entry.Documents)
          {
               if (DocumentRef.DocumentID == Doc.ID)
               {
                    TargetsDocument = true;
                    break;
               }
          }

          if (!TargetsDocument)
          {
               continue;
          }

          const double Popularity = ClampSAMScore(std::log1p(static_cast<double>(Entry.Uses)) / std::log(12.0));
          const double Freshness = GetSAMIdeaFreshness(Entry.LastSeenMS, NowMS);
          const double Resolution = Entry.ResolvedAtMS > 0
               ? ClampSAMScore(std::log1p(static_cast<double>(std::max<uint64_t>(Entry.ResolvedUses, 1))) / std::log(12.0))
               : 0.0;
          const double BaseBoost = ClampSAMScore((Popularity * 0.50) + (Freshness * 0.25) + (Resolution * 0.25));

          AddBoost(Entry.Query, BaseBoost * 0.92);

          for (const auto& Candidate : Entry.ResolvedCandidates)
          {
               AddBoost(Candidate.Text, BaseBoost * (0.84 + (ClampSAMScore(Candidate.Weight) * 0.16)));
          }

          for (const auto& RankedTerm : Entry.ResolvedRankedTerms)
          {
               AddBoost(RankedTerm.Text, BaseBoost * (0.88 + (ClampSAMScore(RankedTerm.Weight) * 0.20)));
          }
     }

     return Boosts;
}
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTerms(const std::string& Collection,
                                                  const Document& Doc,
                                                  std::string* ErrorMessage) const
{
     std::vector<TermEntry> Terms;

     if (!(Instance && Instance->LLM))
     {
          return Terms;
     }

     try
     {
          const std::string DocumentLang = sam::lang::DetectDocumentLanguage(Collection, Doc);
          const std::unordered_set<std::string> EvidenceTokens = BuildSAMDocumentEvidenceTokens(Doc);
          std::unordered_map<std::string, double> FeedbackBoosts;
          const std::vector<llm::AnchorSuggestion> Anchors =
               Instance->LLM->BuildDocumentAnchors(Collection, Doc, DocumentLang, 10);
          const std::vector<llm::ContextSuggestion> Suggestions =
               Instance->LLM->BuildDocumentContext(Collection, Doc, 8);
          const std::string Subject = NormalizeTerm(Doc.Title.empty() ? Doc.ID : Doc.Title);

          {
               std::lock_guard<std::mutex> Lock(DBMutex);

               if (Database)
               {
                    FeedbackBoosts = BuildSAMLLMFeedbackBoosts(Database.get(),
                                                               Collection,
                                                               Doc,
                                                               Subject,
                                                               EvidenceTokens,
                                                               DocumentLang);
               }
          }

          for (const auto& Anchor : Anchors)
          {
               const std::string Candidate = NormalizeTerm(Anchor.Text);

               if (!ValidateSAMLLMCandidate(Collection,
                                            Doc,
                                            Candidate,
                                            Subject,
                                            EvidenceTokens,
                                            DocumentLang))
               {
                    continue;
               }

               std::string Kind = "descriptor";
               std::string Source = "llm_anchor";

               if (Anchor.Kind == "query")
               {
                    Kind = "query";
                    Source = "llm_query";
               }
               else if (Anchor.Kind == "alias")
               {
                    Kind = "alias";
                    Source = "llm_alias";
               }
               else if (Anchor.Kind == "descriptor")
               {
                    Kind = "descriptor";
                    Source = "llm_descriptor";
               }
               else if (Anchor.Kind == "anchor" || Anchor.Kind == "subject")
               {
                    Kind = (!Subject.empty() &&
                            (Candidate == Subject ||
                             Candidate.find(Subject) != std::string::npos ||
                             Subject.find(Candidate) != std::string::npos))
                         ? "alias"
                         : "descriptor";
                    Source = "llm_anchor";
               }

               const double FeedbackBoost = FeedbackBoosts.count(Candidate) > 0
                    ? FeedbackBoosts[Candidate]
                    : 0.0;
               const double Score = ClampSAMScore(0.66 +
                                                  (ClampSAMScore(Anchor.Confidence) * 0.20) +
                                                  std::min(0.16, FeedbackBoost * 0.16));
               const double Signal = ClampSAMScore(0.70 +
                                                   (ClampSAMScore(Anchor.Confidence) * 0.18) +
                                                   std::min(0.18, FeedbackBoost * 0.18));

               Terms.push_back(TermEntry{Candidate, Kind, Source, Score, Signal});

               if (!Subject.empty() && Candidate != Subject && Kind != "query")
               {
                    Terms.push_back(TermEntry{
                        Subject + " " + Candidate,
                        "query",
                        "llm_anchor_pair",
                         std::max(0.68, Score - 0.04),
                         std::max(0.72, Signal - 0.04)
                    });
               }
          }

          for (const auto& Suggestion : Suggestions)
          {
               const std::string Candidate = NormalizeTerm(Suggestion.Text);

               if (!ValidateSAMLLMCandidate(Collection,
                                            Doc,
                                            Candidate,
                                            Subject,
                                            EvidenceTokens,
                                            DocumentLang))
               {
                    continue;
               }

               const bool IsSubjectSuggestion = !Subject.empty() &&
                    (Candidate == Subject ||
                     Candidate.find(Subject) != std::string::npos ||
                     Subject.find(Candidate) != std::string::npos);
               const double FeedbackBoost = FeedbackBoosts.count(Candidate) > 0
                    ? FeedbackBoosts[Candidate]
                    : 0.0;
               const double Score = ClampSAMScore((Suggestion.Kind == "llm" ? 0.78 : 0.70) +
                                                  std::min(0.16, FeedbackBoost * 0.16));
               const double Signal = ClampSAMScore((Suggestion.Kind == "llm" ? 0.82 : 0.72) +
                                                   std::min(0.18, FeedbackBoost * 0.18));

               Terms.push_back(TermEntry{
                    Candidate,
                    IsSubjectSuggestion ? "alias" : (Suggestion.Kind == "llm" ? "query" : "descriptor"),
                    Suggestion.Kind == "llm" ? "llm_context" : "context_field",
                    Score,
                    Signal
               });

               if (!Subject.empty() && Candidate != Subject)
               {
                    Terms.push_back(TermEntry{
                        Subject + " " + Candidate,
                        "query",
                        Suggestion.Kind == "llm" ? "llm_pair" : "context_pair",
                        std::max(0.68, Score - 0.04),
                        std::max(0.70, Signal - 0.04)
                    });
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = E.what();
          }
     }

     return Terms;
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTermsFromProfile(const std::string& Collection,
                                                             const Document& Doc,
                                                             const std::vector<std::string>& ProfileTerms,
                                                             std::string* ErrorMessage) const
{
     (void)Collection;
     std::vector<TermEntry> Terms;

     if (ProfileTerms.empty())
     {
          return Terms;
     }

     try
     {
          const std::string Subject = NormalizeTerm(Doc.Title.empty() ? Doc.ID : Doc.Title);
          std::unordered_set<std::string> DocumentTokens;

          for (const auto& Field : CollectDocumentTextFields(Doc))
          {
               const std::vector<std::string> Tokens = NormalizeSAMTokens(Field.second, true);

               for (const auto& Token : Tokens)
               {
                    if (!Token.empty())
                    {
                         DocumentTokens.insert(Token);
                    }
               }
          }

          for (const auto& ProfileTerm : ProfileTerms)
          {
               const std::string Candidate = NormalizeTerm(ProfileTerm);

               if (!IsUsefulSAMDocumentPhrase(Candidate))
               {
                    continue;
               }

               const std::vector<std::string> CandidateTokens = NormalizeSAMTokens(Candidate, true);
               size_t Overlap = 0;

               for (const auto& Token : CandidateTokens)
               {
                    if (DocumentTokens.find(Token) != DocumentTokens.end())
                    {
                         ++Overlap;
                    }
               }

               if (Overlap == 0 && !Subject.empty() && Candidate.find(Subject) == std::string::npos)
               {
                    continue;
               }

               const double OverlapBoost = std::min(0.18, 0.05 * static_cast<double>(Overlap));

               Terms.push_back(TermEntry{
                    Candidate,
                    Overlap >= 2 ? "alias" : "descriptor",
                    "profile_context",
                    0.72 + OverlapBoost,
                    0.74 + OverlapBoost
               });

               if (!Subject.empty() && Candidate != Subject)
               {
                    Terms.push_back(TermEntry{
                         Subject + " " + Candidate,
                         "query",
                         "profile_pair",
                         0.70 + OverlapBoost,
                         0.76 + OverlapBoost
                    });
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = E.what();
          }
     }

     return Terms;
}

std::vector<SAM::TermEntry> SAM::ExpandDocumentTerms(const std::string& Collection,
                                                     const Document& Doc,
                                                     std::string* ErrorMessage) const
{
     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection and document ID are required.";
          }

          return {};
     }

     SAMTermCollector Collector;
     std::vector<std::string> SubjectSeeds;
     std::vector<std::string> DescriptorSeeds;
     std::vector<std::string> QuerySeeds;
     std::unordered_set<std::string> SubjectSeen;
     std::unordered_set<std::string> DescriptorSeen;
     std::unordered_set<std::string> QuerySeen;
     const std::string DocumentLang = sam::lang::DetectDocumentLanguage(Collection, Doc);

     auto RememberSeed = [](std::vector<std::string>& Output,
                            std::unordered_set<std::string>& Seen,
                            const std::string& Value,
                            size_t Limit)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (Candidate.empty() || !Seen.insert(Candidate).second)
          {
               return;
          }

          Output.push_back(Candidate);

          if (Output.size() > Limit)
          {
               Output.resize(Limit);
          }
     };

     auto AddTerm = [&](const std::string& Text,
                        const std::string& Kind,
                        const std::string& Source,
                        double Score,
                        double Signal)
     {
          Collector.Add(Text, Kind, Source, Score, Signal);

          for (const auto& NumericVariant : BuildSAMNumericWordVariants(Text, DocumentLang))
          {
               Collector.Add(NumericVariant,
                             Kind == "subject" ? "alias" : Kind,
                             Source,
                             std::max(0.54, Score - 0.06),
                             std::max(0.58, Signal - 0.04));
          }

          if (Kind == "subject" || Kind == "alias" || Kind == "synonym")
          {
               RememberSeed(SubjectSeeds, SubjectSeen, Text, 16);
          }
          else if (Kind == "descriptor")
          {
               RememberSeed(DescriptorSeeds, DescriptorSeen, Text, 20);
          }
          else if (Kind == "query")
          {
               RememberSeed(QuerySeeds, QuerySeen, Text, 20);
          }
     };

     const std::string RawTitle = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);
     const std::string Subject = NormalizeTerm(RawTitle);

     if (!Subject.empty())
     {
          AddTerm(Subject, "subject", "title", 0.99, 0.99);

          for (const auto& Variant : BuildSAMReducedTitleVariants(RawTitle))
          {
               AddTerm(Variant, "alias", "title_reduced", 0.93, 0.93);
          }

          for (const auto& Window : BuildSAMPhraseWindows(RawTitle, 1, 4, 12))
          {
               AddTerm(Window,
                       Window == Subject ? "subject" : "alias",
                       Window == Subject ? "title" : "title_window",
                       Window == Subject ? 0.97 : 0.88,
                       Window == Subject ? 0.97 : 0.88);
          }
     }

     std::string CombinedNarrative;

     for (const auto& Field : CollectDocumentTextFields(Doc))
     {
          const std::string& LowerKey = Field.first;
          const std::string& RawValue = Field.second;

          if (RawValue.empty())
          {
               continue;
          }

          const bool TitleLike = IsTitleLikeSAMField(LowerKey);
          const bool SummaryLike = IsSummaryLikeSAMField(LowerKey);
          const bool BodyLike = IsBodyLikeSAMField(LowerKey);
          const bool AliasLike = IsAliasLikeSAMField(LowerKey);
          const bool TaxonomyLike = IsTaxonomyLikeSAMField(LowerKey);
          const bool QueryLike = IsQueryLikeSAMField(LowerKey);

          if ((SummaryLike || BodyLike) && CombinedNarrative.size() < 4000)
          {
               if (!CombinedNarrative.empty())
               {
                    CombinedNarrative.push_back(' ');
               }

               CombinedNarrative += TruncateSAMDocumentText(RawValue, 1800);
          }

          const std::vector<std::string> Values =
               ExtractArrayishSAMValues(RawValue,
                                        TaxonomyLike || AliasLike || QueryLike ? 12 : 4);

          for (const auto& Value : Values)
          {
               if (Value.empty() || IsIdentifierLikeSAMValue(Value))
               {
                    continue;
               }

               if (TitleLike)
               {
                    AddTerm(Value, "alias", "field_title", 0.91, 0.92);
               }
               else if (AliasLike)
               {
                    AddTerm(Value, "alias", "alias_field", 0.89, 0.90);
               }
               else if (TaxonomyLike)
               {
                    AddTerm(Value, "descriptor", "taxonomy_field", 0.84, 0.86);
               }
               else if (QueryLike)
               {
                    AddTerm(Value, "query", "query_field", 0.82, 0.84);
               }
               else if (SummaryLike)
               {
                    AddTerm(Value, "descriptor", "summary_field", 0.76, 0.78);
               }

               const size_t MinWindow = (TaxonomyLike || AliasLike || QueryLike) ? 1 : 2;
               const size_t MaxWindow = (TaxonomyLike || AliasLike || QueryLike) ? 3 : 4;
               const size_t WindowLimit = (TaxonomyLike || AliasLike || QueryLike) ? 8 : 5;

               for (const auto& Window : BuildSAMPhraseWindows(Value, MinWindow, MaxWindow, WindowLimit))
               {
                    const std::string Kind =
                         AliasLike ? "alias" :
                         QueryLike ? "query" :
                         (TaxonomyLike || SummaryLike ? "descriptor" : "alias");
                    const std::string Source =
                         AliasLike ? "alias_window" :
                         QueryLike ? "query_window" :
                         TaxonomyLike ? "taxonomy_window" :
                         SummaryLike ? "summary_window" : "field_window";
                    const double BaseScore =
                         AliasLike ? 0.83 :
                         QueryLike ? 0.80 :
                         TaxonomyLike ? 0.78 :
                         SummaryLike ? 0.74 : 0.72;

                    AddTerm(Window, Kind, Source, BaseScore, BaseScore);
               }

               if (!Subject.empty() && NormalizeTerm(Value) != Subject)
               {
                    const std::string LoweredValue = NormalizeTerm(Value);
                    const std::string Kind =
                         QueryLike ? "query" :
                         TaxonomyLike ? "query" :
                         AliasLike ? "alias" : "descriptor";
                    const std::string Source =
                         QueryLike ? "query_pair" :
                         TaxonomyLike ? "subject_descriptor" :
                         AliasLike ? "subject_alias" : "field_pair";
                    AddTerm(Subject + " " + LoweredValue, Kind, Source, 0.86, 0.87);

                    if (TaxonomyLike || AliasLike)
                    {
                         AddTerm(LoweredValue + " " + Subject, Kind, Source, 0.82, 0.84);
                    }
               }
          }

          if (SummaryLike || BodyLike)
          {
               const std::vector<std::string> Samples =
                    BuildSAMSentenceSamples(RawValue, SummaryLike ? 6 : 10);

               for (const auto& Sample : Samples)
               {
                    for (const auto& Window : BuildSAMPhraseWindows(Sample, 2, 5, SummaryLike ? 6 : 8))
                    {
                         const std::vector<std::string> WindowTokens = NormalizeSAMTokens(Window, true);
                         const size_t StrongCount = CountStrongSAMTokens(WindowTokens);

                         if (StrongCount < 2)
                         {
                              continue;
                         }

                         AddTerm(Window,
                                 StrongCount >= 3 ? "query" : "descriptor",
                                 SummaryLike ? "summary_window" : "body_window",
                                 SummaryLike ? 0.76 : 0.70,
                                 SummaryLike ? 0.78 : 0.72);
                    }
               }
          }
     }

     if (!CombinedNarrative.empty())
     {
          for (const auto& Window : BuildSAMPhraseWindows(CombinedNarrative, 2, 5, 16))
          {
               const std::vector<std::string> Tokens = NormalizeSAMTokens(Window, true);

               if (CountStrongSAMTokens(Tokens) < 2)
               {
                    continue;
               }

               AddTerm(Window, "query", "body_query", 0.68, 0.72);
          }
     }

     const std::vector<std::string> SeedQueries = BuildSAMSeedQueries(SubjectSeeds, DescriptorSeeds);

     for (const auto& Query : SeedQueries)
     {
          AddTerm(Query, "query", "seed_query", 0.79, 0.82);
     }

     std::vector<std::string> ProfileTerms;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               std::string RawProfile;
               const rocksdb::Status Status =
                    Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

               if (Status.ok() && !RawProfile.empty())
               {
                    try
                    {
                        const nlohmann::json Root = nlohmann::json::parse(RawProfile);

                        auto AppendProfileTerms = [&](const nlohmann::json& Array, const char* Field)
                        {
                             if (!Array.is_array())
                             {
                                  return;
                             }

                             for (const auto& Item : Array)
                             {
                                  if (!Item.is_object())
                                  {
                                       continue;
                                  }

                                  if (Field != nullptr)
                                  {
                                       const std::string Text = NormalizeTerm(Item.value(Field, ""));

                                       if (!Text.empty())
                                       {
                                            ProfileTerms.push_back(Text);
                                       }
                                  }

                                  for (const auto& NestedKey : {"aliases", "descriptors", "queries", "related"})
                                  {
                                       if (!Item.contains(NestedKey) || !Item[NestedKey].is_array())
                                       {
                                            continue;
                                       }

                                       for (const auto& Nested : Item[NestedKey])
                                       {
                                            if (Nested.is_string())
                                            {
                                                 const std::string Text = NormalizeTerm(Nested.get<std::string>());

                                                 if (!Text.empty())
                                                 {
                                                      ProfileTerms.push_back(Text);
                                                 }
                                            }
                                       }
                                  }
                             }
                        };

                        AppendProfileTerms(Root.value("terms", nlohmann::json::array()), "text");
                        AppendProfileTerms(Root.value("learned_terms", nlohmann::json::array()), "text");
                        AppendProfileTerms(Root.value("families", nlohmann::json::array()), "subject");
                        AppendProfileTerms(Root.value("learned_families", nlohmann::json::array()), "subject");
                    }
                    catch (...)
                    {
                    }
               }
          }
     }

     ProfileTerms = UniqueNormalizedPhrases(ProfileTerms, 18);

     std::string ContextError;

     for (const auto& Term : GenerateLLMTerms(Collection, Doc, &ContextError))
     {
          AddTerm(Term.Text, Term.Kind, Term.Source, Term.Score, Term.Signal);
     }

     for (const auto& Term : GenerateLLMTermsFromProfile(Collection, Doc, ProfileTerms, &ContextError))
     {
          AddTerm(Term.Text, Term.Kind, Term.Source, Term.Score, Term.Signal);
     }

     for (size_t Iteration = 0; Iteration < 2; ++Iteration)
     {
          const std::vector<std::string> CurrentSubjects = SubjectSeeds;
          const std::vector<std::string> CurrentDescriptors = DescriptorSeeds;
          const std::vector<std::string> CurrentQueries = QuerySeeds;

          for (const auto& SubjectSeed : CurrentSubjects)
          {
               const std::vector<std::string> SubjectTokens = NormalizeSAMTokens(SubjectSeed, true);

               if (SubjectTokens.empty())
               {
                    continue;
               }

               for (const auto& DescriptorSeed : CurrentDescriptors)
               {
                    const std::vector<std::string> DescriptorTokens = NormalizeSAMTokens(DescriptorSeed, true);

                    if (DescriptorTokens.empty())
                    {
                         continue;
                    }

                    size_t Shared = 0;

                    for (const auto& Token : DescriptorTokens)
                    {
                         if (std::find(SubjectTokens.begin(), SubjectTokens.end(), Token) != SubjectTokens.end())
                         {
                              ++Shared;
                         }
                    }

                    if (Shared >= SubjectTokens.size() && Shared >= DescriptorTokens.size())
                    {
                         continue;
                    }

                    AddTerm(SubjectSeed + " " + DescriptorSeed,
                            "query",
                            Iteration == 0 ? "iterative_pair" : "iterative_refine",
                            Iteration == 0 ? 0.80 : 0.76,
                            Iteration == 0 ? 0.84 : 0.80);
               }

               for (const auto& QuerySeed : CurrentQueries)
               {
                    if (QuerySeed == SubjectSeed)
                    {
                         continue;
                    }

                    AddTerm(QuerySeed + " " + SubjectSeed,
                            "query",
                            Iteration == 0 ? "query_refine" : "query_expand",
                            Iteration == 0 ? 0.72 : 0.68,
                            Iteration == 0 ? 0.76 : 0.72);
               }
          }
     }

     std::vector<TermEntry> Terms = Collector.Finalize(96);

     if (Terms.empty())
     {
          if (!Subject.empty())
          {
               Terms.push_back(TermEntry{Subject, "subject", "title", 0.95, 0.95});
          }
          else if (ErrorMessage)
          {
               *ErrorMessage = "No useful document terms could be derived.";
          }
     }

     if (!ContextError.empty() && ErrorMessage && ErrorMessage->empty())
     {
          *ErrorMessage = ContextError;
     }

     return Terms;
}

bool SAM::RecordSearchIdea(const std::string& Collection,
                           const std::string& Query,
                           const std::vector<SearchIdeaDocumentRef>& Documents,
                           std::string* ErrorMessage)
{
     std::unique_lock<std::mutex> Lock(DBMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM history recording skipped because the database is busy.";
          }

          return false;
     }

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     return RecordSearchIdeaLocked(Collection, Query, Documents, ErrorMessage);
}

bool SAM::RecordSearchIdeaLocked(const std::string& Collection,
                                 const std::string& Query,
                                 const std::vector<SearchIdeaDocumentRef>& Documents,
                                 std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!ShouldTrackSAMSearchIdeas(Collection))
     {
          return true;
     }

     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (Collection.empty() || NormalizedQuery.empty() || NormalizedQuery == "*")
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A non-empty collection and query are required.";
          }

          return false;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     SearchIdeaEntry Entry;
     std::string RawValue;
     const std::string IdeaKey = BuildSearchIdeaKey(Collection, NormalizedQuery);
     const rocksdb::Status ReadStatus =
          Database->Get(rocksdb::ReadOptions(), IdeaKey, &RawValue);

     if (ReadStatus.ok())
     {
          ParseSearchIdeaEntry(RawValue, Entry);
     }
     else if (!ReadStatus.IsNotFound() && ErrorMessage)
     {
          *ErrorMessage = ReadStatus.ToString();
          return false;
     }

     Entry.Collection = Collection;
     Entry.Query = TrimCopy(Query);
     Entry.NormalizedQuery = NormalizedQuery;
     Entry.FirstSeenMS = Entry.FirstSeenMS == 0 ? NowMS : Entry.FirstSeenMS;
     Entry.LastSeenMS = NowMS;
     Entry.Uses = Entry.Uses == 0 ? 1 : (Entry.Uses + 1);
     Entry.Vector = BuildHashedSemanticVector({Entry.NormalizedQuery});

     std::unordered_map<std::string, SearchIdeaDocumentRef> RankedDocuments;

     for (const auto& Existing : Entry.Documents)
     {
          if (Existing.DocumentID.empty())
          {
               continue;
          }

          RankedDocuments[Existing.DocumentID] = Existing;
     }

     for (size_t Index = 0; Index < Documents.size(); ++Index)
     {
          const SearchIdeaDocumentRef& Document = Documents[Index];

          if (Document.DocumentID.empty())
          {
               continue;
          }

          SearchIdeaDocumentRef Candidate = Document;

          if (Candidate.Score <= 0.0)
          {
               Candidate.Score = std::max(0.05, 1.0 - (static_cast<double>(Index) * 0.12));
          }

          auto ExistingIt = RankedDocuments.find(Candidate.DocumentID);

          if (ExistingIt == RankedDocuments.end())
          {
               RankedDocuments[Candidate.DocumentID] = Candidate;
               continue;
          }

          ExistingIt->second.Score = std::max(ExistingIt->second.Score, Candidate.Score);

          if (ExistingIt->second.Title.empty() && !Candidate.Title.empty())
          {
               ExistingIt->second.Title = Candidate.Title;
          }
     }

     Entry.Documents.clear();
     Entry.Documents.reserve(RankedDocuments.size());

     for (const auto& Pair : RankedDocuments)
     {
          Entry.Documents.push_back(Pair.second);
     }

     std::sort(Entry.Documents.begin(), Entry.Documents.end(),
               [](const SearchIdeaDocumentRef& A, const SearchIdeaDocumentRef& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Title.size() != B.Title.size())
                    {
                         return A.Title.size() > B.Title.size();
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Entry.Documents.size() > kSAMSearchIdeaMaxDocs)
     {
          Entry.Documents.resize(kSAMSearchIdeaMaxDocs);
     }

     const rocksdb::Status WriteStatus =
          Database->Put(rocksdb::WriteOptions(), IdeaKey, SerializeSearchIdeaEntry(Entry).dump());

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     if (!TrimSearchIdeasLocked(Collection, ErrorMessage))
     {
          return false;
     }

     return true;
}

bool SAM::TrimSearchIdeasLocked(const std::string& Collection, std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Search idea trimming requires an open database and collection.";
          }

          return false;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<std::pair<std::string, SearchIdeaEntry>> Entries;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          Entries.push_back({Iterator->key().ToString(), std::move(Entry)});
     }

     if (Entries.size() <= kSAMSearchIdeasMaxEntries)
     {
          return true;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     std::sort(Entries.begin(), Entries.end(),
               [NowMS](const auto& Left, const auto& Right)
               {
                    const double LeftFreshness = GetSAMIdeaFreshness(Left.second.LastSeenMS, NowMS);
                    const double RightFreshness = GetSAMIdeaFreshness(Right.second.LastSeenMS, NowMS);

                    if (Left.second.Uses != Right.second.Uses)
                    {
                         return Left.second.Uses < Right.second.Uses;
                    }

                    if (LeftFreshness != RightFreshness)
                    {
                         return LeftFreshness < RightFreshness;
                    }

                    if (Left.second.LastSeenMS != Right.second.LastSeenMS)
                    {
                         return Left.second.LastSeenMS < Right.second.LastSeenMS;
                    }

                    return Left.second.FirstSeenMS < Right.second.FirstSeenMS;
               });

     rocksdb::WriteBatch Batch;

     for (size_t Index = 0; Index + kSAMSearchIdeasMaxEntries < Entries.size(); ++Index)
     {
          Batch.Delete(Entries[Index].first);
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

double ComputeSearchIntentTokenOverlap(const std::vector<std::string>& LeftTokens,
                                       const std::vector<std::string>& RightTokens)
{
     if (LeftTokens.empty() || RightTokens.empty())
     {
          return 0.0;
     }

     std::unordered_set<std::string> RightSet(RightTokens.begin(), RightTokens.end());
     size_t Matched = 0;

     for (const auto& Token : LeftTokens)
     {
          if (RightSet.find(Token) != RightSet.end())
          {
               ++Matched;
          }
     }

     return ClampSAMScore(static_cast<double>(Matched) / static_cast<double>(LeftTokens.size()));
}

std::vector<std::string> BuildSearchIntentDocumentPhrases(const SAM::DocumentEntry& Entry)
{
     std::vector<std::string> Phrases;

     if (!Entry.Title.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Title));
     }

     if (!Entry.Subject.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Subject));
     }

     if (!Entry.Summary.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Summary));
     }

     for (const auto& Alias : Entry.Aliases)
     {
          Phrases.push_back(NormalizeTerm(Alias));
     }

     for (const auto& Descriptor : Entry.Descriptors)
     {
          Phrases.push_back(NormalizeTerm(Descriptor));
     }

     for (const auto& Query : Entry.Queries)
     {
          Phrases.push_back(NormalizeTerm(Query));
     }

     for (const auto& Term : Entry.Terms)
     {
          Phrases.push_back(NormalizeTerm(Term.Text));

          if (Phrases.size() >= 32)
          {
               break;
          }
     }

     Phrases.erase(std::remove_if(Phrases.begin(), Phrases.end(),
                                  [](const std::string& Value)
                                  {
                                       return Value.empty();
                                  }),
                   Phrases.end());
     return Phrases;
}

double ScoreSearchIntentDocumentMatch(const SAM::DocumentEntry& Entry,
                                      const std::vector<llm::SearchIntentCandidate>& Candidates,
                                      const std::vector<llm::SearchIntentCandidate>& RankedTerms)
{
     const std::vector<std::string> Phrases = BuildSearchIntentDocumentPhrases(Entry);

     if (Phrases.empty())
     {
          return 0.0;
     }

     const std::string Title = NormalizeTerm(Entry.Title);
     const std::string Subject = NormalizeTerm(Entry.Subject);
     double BestScore = 0.0;

     const auto ScoreOne = [&](const llm::SearchIntentCandidate& Candidate,
                               double BaseWeight) -> double
     {
          const std::string Text = NormalizeTerm(Candidate.Text);

          if (Text.empty())
          {
               return 0.0;
          }

          const std::vector<std::string> CandidateTokens = TokenizeNormalized(Text);
          double Score = 0.0;

          if (!Title.empty() && Title == Text)
          {
               Score = std::max(Score, BaseWeight * 1.45);
          }

          if (!Subject.empty() && Subject == Text)
          {
               Score = std::max(Score, BaseWeight * 1.55);
          }

          for (const auto& Phrase : Phrases)
          {
               if (Phrase == Text)
               {
                    Score = std::max(Score, BaseWeight * 1.30);
                    continue;
               }

               if (Phrase.find(Text) != std::string::npos || Text.find(Phrase) != std::string::npos)
               {
                    Score = std::max(Score, BaseWeight * 1.08);
               }

               const double Overlap = ComputeSearchIntentTokenOverlap(CandidateTokens, TokenizeNormalized(Phrase));

               if (Overlap > 0.0)
               {
                    Score = std::max(Score, BaseWeight * (0.70 + (Overlap * 0.45)));
               }
          }

          return Score;
     };

     for (const auto& Candidate : Candidates)
     {
          BestScore = std::max(BestScore, ScoreOne(Candidate, 0.85 + ClampSAMScore(Candidate.Weight)));
     }

     for (const auto& RankedTerm : RankedTerms)
     {
          BestScore = std::max(BestScore, ScoreOne(RankedTerm, 0.60 + ClampSAMScore(RankedTerm.Weight)));
     }

     return BestScore;
}

Document BuildSearchIntentCandidateDocument(const SAM::DocumentEntry& Entry)
{
     Document Candidate;
     Candidate.ID = Entry.DocumentID;
     Candidate.Title = Entry.Title;
     Candidate.Content = Entry.Summary;

     if (!Entry.Subject.empty())
     {
          Candidate.Fields["subject"] = Entry.Subject;
     }

     if (!Entry.Aliases.empty())
     {
          Candidate.Fields["aliases"] = JoinValues(Entry.Aliases, ", ");
     }

     if (!Entry.Descriptors.empty())
     {
          Candidate.Fields["descriptors"] = JoinValues(Entry.Descriptors, ", ");
     }

     if (!Entry.Queries.empty())
     {
          Candidate.Fields["queries"] = JoinValues(Entry.Queries, ", ");
     }

     std::vector<std::string> RankedTerms;

     for (const auto& Term : Entry.Terms)
     {
          RankedTerms.push_back(Term.Text);

          if (RankedTerms.size() >= 10)
          {
               break;
          }
     }

     if (!RankedTerms.empty())
     {
          Candidate.Fields["terms"] = JoinValues(RankedTerms, ", ");
     }

     return Candidate;
}

bool SAM::OptimizeSearchIdeaIntentLocked(const std::string& Collection,
                                         const std::string& NormalizedQuery,
                                         bool* Updated,
                                         std::string* ErrorMessage)
{
     if (Updated)
     {
          *Updated = false;
     }

     if (Collection.empty() || NormalizedQuery.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection and normalized query are required.";
          }

          return false;
     }

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          return true;
     }

     SearchIdeaEntry Entry;
     std::vector<Document> CandidateDocuments;

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

          std::string RawValue;
          const rocksdb::Status ReadStatus =
               Database->Get(rocksdb::ReadOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             &RawValue);

          if (!ReadStatus.ok())
          {
               if (!ReadStatus.IsNotFound() && ErrorMessage)
               {
                    *ErrorMessage = ReadStatus.ToString();
               }

               return ReadStatus.IsNotFound();
          }

          if (!ParseSearchIdeaEntry(RawValue, Entry))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Failed to parse stored search idea.";
               }

               return false;
          }

          for (const auto& DocumentRef : Entry.Documents)
          {
               if (DocumentRef.DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status ManifestStatus =
                    Database->Get(rocksdb::ReadOptions(),
                                  BuildDocManifestKey(Collection, DocumentRef.DocumentID),
                                  &ManifestValue);

               if (!ManifestStatus.ok())
               {
                    continue;
               }

               SAM::DocumentEntry ManifestEntry;

               if (!ParseManifestValue(ManifestValue, ManifestEntry) || !IsSAMDocumentEntryCurrent(ManifestEntry))
               {
                    continue;
               }

               CandidateDocuments.push_back(BuildSearchIntentCandidateDocument(ManifestEntry));
          }
     }

     if (CandidateDocuments.empty())
     {
          return true;
     }

     const size_t IntentLimit = Instance && Instance->Config
          ? static_cast<size_t>(std::max(1, Instance->Config->GetSamLLMMaxIdeas()))
          : 6;
     const llm::SearchIntentResolution Resolution =
          Instance->LLM->ResolveSearchIntent(Collection, Entry.Query, CandidateDocuments, IntentLimit);

     if (Resolution.Candidates.empty() && Resolution.RankedTerms.empty() &&
         Resolution.Interpretation.empty() && Resolution.Conclusion.empty())
     {
          return true;
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

          std::string RawValue;
          const rocksdb::Status ReadStatus =
               Database->Get(rocksdb::ReadOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             &RawValue);

          if (!ReadStatus.ok())
          {
               if (!ReadStatus.IsNotFound() && ErrorMessage)
               {
                    *ErrorMessage = ReadStatus.ToString();
               }

               return ReadStatus.IsNotFound();
          }

          if (!ParseSearchIdeaEntry(RawValue, Entry))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Failed to parse stored search idea.";
               }

               return false;
          }

          Entry.ResolvedInterpretation = Resolution.Interpretation;
          Entry.ResolvedConclusion = Resolution.Conclusion;
          Entry.ResolvedAtMS = GetSAMCurrentTimeMS();
          Entry.ResolvedUses = Entry.Uses;
          Entry.ResolvedCandidates = Resolution.Candidates;
          Entry.ResolvedRankedTerms = Resolution.RankedTerms;

          std::unordered_map<std::string, SearchIdeaDocumentRef> RankedDocuments;

          for (const auto& Existing : Entry.Documents)
          {
               if (!Existing.DocumentID.empty())
               {
                    RankedDocuments[Existing.DocumentID] = Existing;
               }
          }

          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(ManifestPrefix);
               Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix);
               Iterator->Next())
          {
               SAM::DocumentEntry ManifestEntry;

               if (!ParseManifestValue(Iterator->value().ToString(), ManifestEntry) ||
                   !IsSAMDocumentEntryCurrent(ManifestEntry))
               {
                    continue;
               }

               const double MatchScore =
                    ScoreSearchIntentDocumentMatch(ManifestEntry,
                                                  Entry.ResolvedCandidates,
                                                  Entry.ResolvedRankedTerms);

               if (MatchScore < 0.65)
               {
                    continue;
               }

               SearchIdeaDocumentRef& RankedDocument = RankedDocuments[ManifestEntry.DocumentID];
               RankedDocument.DocumentID = ManifestEntry.DocumentID;
               RankedDocument.Title = ManifestEntry.Title;
               RankedDocument.Score = std::max(RankedDocument.Score, MatchScore);
          }

          Entry.Documents.clear();

          for (const auto& Pair : RankedDocuments)
          {
               Entry.Documents.push_back(Pair.second);
          }

          std::sort(Entry.Documents.begin(), Entry.Documents.end(),
                    [](const SearchIdeaDocumentRef& Left, const SearchIdeaDocumentRef& Right)
                    {
                         if (Left.Score != Right.Score)
                         {
                              return Left.Score > Right.Score;
                         }

                         return Left.Title < Right.Title;
                    });

          if (Entry.Documents.size() > kSAMSearchIdeaMaxDocs)
          {
               Entry.Documents.resize(kSAMSearchIdeaMaxDocs);
          }

          const rocksdb::Status WriteStatus =
               Database->Put(rocksdb::WriteOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             SerializeSearchIdeaEntry(Entry).dump());

          if (!WriteStatus.ok())
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = WriteStatus.ToString();
               }

               return false;
          }
     }

     if (Updated)
     {
          *Updated = true;
     }

     return true;
}

bool SAM::RefreshCollectionProfileFromSearchIdeas(const std::string& Collection,
                                                  bool* Updated,
                                                  std::string* ErrorMessage)
{
     if (Updated)
     {
          *Updated = false;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
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

     size_t RecentIdeaCount = 0;
     const uint64_t LatestRecentIdeaSeenMS =
          GetLatestRecentSearchIdeaTimestampLocked(Database.get(), Collection, &RecentIdeaCount);

     if (LatestRecentIdeaSeenMS == 0 || RecentIdeaCount == 0)
     {
          return true;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (Status.ok() && !RawProfile.empty())
     {
          try
          {
               const nlohmann::json Root = nlohmann::json::parse(RawProfile);
               const uint64_t LastIdeaSyncMarker = Root.value("idea_sync_marker_ms", static_cast<uint64_t>(0));
               const uint64_t LastIdeaProfileSyncMS = Root.value("idea_profile_synced_at_ms", static_cast<uint64_t>(0));
               const size_t LastIdeaRecentCount =
                    static_cast<size_t>(Root.value("idea_recent_count", static_cast<uint64_t>(0)));

               if (LastIdeaSyncMarker >= LatestRecentIdeaSeenMS)
               {
                    return true;
               }

               const uint64_t NowMS = GetSAMCurrentTimeMS();

               if (LastIdeaProfileSyncMS > 0 &&
                   NowMS > LastIdeaProfileSyncMS &&
                   (NowMS - LastIdeaProfileSyncMS) < kSAMSearchIdeaProfileSyncCooldownMs)
               {
                    const size_t IdeaCountDelta = RecentIdeaCount > LastIdeaRecentCount
                         ? (RecentIdeaCount - LastIdeaRecentCount)
                         : 0;

                    if (IdeaCountDelta < kSAMSearchIdeaProfileForceSyncMinDelta)
                    {
                         return true;
                    }
               }
          }
          catch (...)
          {
          }
     }

     if (!RebuildCollectionProfileLocked(Database.get(), Collection, ErrorMessage))
     {
          return false;
     }

     if (Updated)
     {
          *Updated = true;
     }

     return true;
}

size_t SAM::ProcessPendingSearchIntentOptimizations(size_t MaxCollections)
{
     if (MaxCollections == 0 || !Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          return 0;
     }

     std::vector<std::pair<std::string, std::string>> PendingIdeas;
     std::vector<SearchIdeaEntry> RankedIdeas;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               return 0;
          }

          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
          std::unordered_set<std::string> SeenCollections;

          for (Iterator->Seek("sam:idea:");
               Iterator->Valid() && Iterator->key().starts_with("sam:idea:");
               Iterator->Next())
          {
               SearchIdeaEntry Entry;

               if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
               {
                    continue;
               }

               if (Entry.Collection.empty() || Entry.NormalizedQuery.empty() || Entry.Documents.empty())
               {
                    continue;
               }

               if (Entry.ResolvedUses >= Entry.Uses && Entry.ResolvedAtMS >= Entry.LastSeenMS &&
                   !Entry.ResolvedCandidates.empty())
               {
                    continue;
               }

               if (!SeenCollections.insert(Entry.Collection).second)
               {
                    continue;
               }

               RankedIdeas.push_back(std::move(Entry));
          }
     }

     std::sort(RankedIdeas.begin(), RankedIdeas.end(),
               [](const SearchIdeaEntry& Left, const SearchIdeaEntry& Right)
               {
                    if (Left.LastSeenMS != Right.LastSeenMS)
                    {
                         return Left.LastSeenMS > Right.LastSeenMS;
                    }

                    if (Left.Uses != Right.Uses)
                    {
                         return Left.Uses > Right.Uses;
                    }

                    return Left.Query < Right.Query;
               });

     for (const auto& Entry : RankedIdeas)
     {
          PendingIdeas.push_back({Entry.Collection, Entry.NormalizedQuery});

          if (PendingIdeas.size() >= MaxCollections)
          {
               break;
          }
     }

     size_t Processed = 0;

     for (const auto& PendingIdea : PendingIdeas)
     {
          bool Updated = false;
          std::string ErrorMessage;

          if (!OptimizeSearchIdeaIntentLocked(PendingIdea.first, PendingIdea.second, &Updated, &ErrorMessage))
          {
               if (Instance->Logs && !ErrorMessage.empty())
               {
                    Instance->Logs->Normal("sam",
                                           "Failed to optimize SAM search intent for collection '" +
                                                PendingIdea.first + "' and query '" + PendingIdea.second +
                                                "': " + ErrorMessage + ".");
               }

               continue;
          }

          if (!Updated)
          {
               continue;
          }

          bool ProfileUpdated = false;
          ErrorMessage.clear();
          RefreshCollectionProfileFromSearchIdeas(PendingIdea.first, &ProfileUpdated, &ErrorMessage);

          if (Instance->Logs)
          {
               if (!ErrorMessage.empty())
               {
                    Instance->Logs->Normal("sam",
                                           "Optimized SAM search intent for collection '" +
                                                PendingIdea.first +
                                                "' but failed to refresh its profile: " +
                                                ErrorMessage + ".");
               }
               else
               {
                    Instance->Logs->Debug("sam",
                                          "Optimized SAM search intent for collection '" +
                                               PendingIdea.first + "' and refreshed learned terms.");
               }
          }

          ++Processed;
     }

     return Processed;
}

bool SAM::CancelCollectionWork(const std::string& Collection, std::string* ErrorMessage)
{
     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexJobs.erase(
               std::remove_if(PendingIndexJobs.begin(), PendingIndexJobs.end(),
                              [&](const PendingIndexJob& Job)
                              {
                                   return Job.Collection == Collection;
                              }),
               PendingIndexJobs.end());

          for (auto It = PendingIndexKeys.begin(); It != PendingIndexKeys.end(); )
          {
               if (It->rfind(Collection + "\n", 0) == 0)
               {
                    It = PendingIndexKeys.erase(It);
               }
               else
               {
                    ++It;
               }
          }
     }

     std::unique_lock<std::mutex> Lock(JobMutex);
     CancelledCollections.insert(Collection);
     CollectionJobStatus& Status = CollectionJobs[Collection];
     Status.Running = false;
     Status.Completed = false;
     Status.PendingDocuments = 0;
     if (Status.ErrorMessage.empty())
     {
          Status.ErrorMessage = "Cancelled.";
     }

     JobStateCV.wait(Lock, [&]()
     {
          return ActiveCollectionTasks.find(Collection) == ActiveCollectionTasks.end();
     });

     CancelledCollections.erase(Collection);
     return true;
}

bool SAM::CancelAllWork(std::string* ErrorMessage)
{
     (void)ErrorMessage;

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexJobs.clear();
          PendingIndexKeys.clear();
     }

     std::unique_lock<std::mutex> Lock(JobMutex);
     CancelAllRequested = true;

     for (auto& Entry : CollectionJobs)
     {
          Entry.second.Running = false;
          Entry.second.Completed = false;
          Entry.second.PendingDocuments = 0;
          if (Entry.second.ErrorMessage.empty())
          {
               Entry.second.ErrorMessage = "Cancelled.";
          }
     }

     JobStateCV.wait(Lock, [&]()
     {
          return ActiveCollectionTasks.empty();
     });

     CancelledCollections.clear();
     CancelAllRequested = false;
     return true;
}

uint64_t SAM::BeginLookupActivity(const std::string& Collection,
                                  const std::string& Query) const
{
     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (NormalizedQuery.empty())
     {
          return 0;
     }

     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     SearchActivityEntry Entry;
     Entry.Sequence = NextSearchActivitySequence++;
     Entry.Collection = Collection;
     Entry.Query = TrimCopy(Query);
     Entry.NormalizedQuery = NormalizedQuery;
     Entry.StartedMS = GetSAMCurrentTimeMS();
     Entry.Running = true;
     ActiveSearchActivities[Entry.Sequence] = Entry;
     return Entry.Sequence;
}

void SAM::FinishLookupActivity(uint64_t Sequence,
                               size_t ResultCount) const
{
     if (Sequence == 0)
     {
          return;
     }

     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     const auto It = ActiveSearchActivities.find(Sequence);

     if (It == ActiveSearchActivities.end())
     {
          return;
     }

     SearchActivityEntry Entry = It->second;
     Entry.CompletedMS = GetSAMCurrentTimeMS();
     Entry.ResultCount = ResultCount;
     Entry.Running = false;
     LatestSearchActivityByCollection["*"] = Entry;

     if (!Entry.Collection.empty())
     {
          LatestSearchActivityByCollection[Entry.Collection] = Entry;
     }

     ActiveSearchActivities.erase(It);
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);
     const uint64_t ActivitySequence = BeginLookupActivity("", Query);
     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     std::vector<std::string> Variants = BuildQueryVariants(Query);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(DatabaseHandle.get(), "", Query, QueryViews);

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

    std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;
    std::unordered_map<std::string, bool> FreshnessCache;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

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
                    Hit.TermOrigin = Hit.MatchedSource;
                    Hit.MatchedPath = "sam_term";
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    Hit.Breakdown.TermScore = Hit.MatchedScore;
                    Hit.Breakdown.FinalScore = Hit.MatchedScore;
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
                         std::string ManifestValue;
                         const rocksdb::Status ManifestStatus =
                              DatabaseHandle->Get(rocksdb::ReadOptions(),
                                                  BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                                                  &ManifestValue);

                         if (!ManifestStatus.ok())
                         {
                              continue;
                         }

                         DocumentEntry Entry;

                         if (!ParseManifestValue(ManifestValue, Entry) ||
                             !IsSAMDocumentEntryCurrent(Entry, &FreshnessCache))
                         {
                              continue;
                         }

                         Hit.Title = Entry.Title.empty() ? Hit.Title : Entry.Title;
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     AppendFuzzyFallbackHits(AggregatedHits, DatabaseHandle.get(), "", Query, Limit);
     AppendSAMLikePatternHits(AggregatedHits, DatabaseHandle.get(), "", Query);
     AppendSemanticProfileHits(AggregatedHits, DatabaseHandle.get(), "", SemanticPlan,
                               std::max<size_t>(256, Limit * 24));

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

     EmitSAM25DebugLog(Query, Hits);
     FinishLookupActivity(ActivitySequence, Hits.size());
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

     const uint64_t ActivitySequence = BeginLookupActivity(Collection, Query);
     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     const SAMCollectionProfileHints ProfileHints = LoadCollectionProfileHints(DatabaseHandle.get(), Collection);
     std::vector<std::string> Variants = BuildQueryVariants(Query, &ProfileHints.StrongTokens);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(DatabaseHandle.get(), Collection, Query, QueryViews);
     const std::vector<std::string> PersistedProfileVariants =
          BuildPersistedCollectionProfileVariants(DatabaseHandle.get(), Collection, QueryViews,
                                                 &ProfileHints.StrongTokens,
                                                 std::max<size_t>(8, std::min<size_t>(Limit * 3, 16)));
     const std::vector<std::string> LearnedVariants =
          BuildCollectionLearnedVariants(DatabaseHandle.get(), Collection, Query, QueryViews,
                                         &ProfileHints.StrongTokens,
                                         std::max<size_t>(8, std::min<size_t>(Limit * 3, 16)));
     const std::vector<SAMMatchedSearchIdea> SearchIdeas =
          BuildMatchedSearchIdeas(DatabaseHandle.get(), Collection, Query, QueryViews,
                                  std::max<size_t>(6, std::min<size_t>(Limit * 2, 10)));
     const std::vector<std::string> SearchIdeaVariants =
          BuildSearchIdeaVariants(SearchIdeas, QueryViews, &ProfileHints.StrongTokens,
                                  std::max<size_t>(6, std::min<size_t>(Limit * 2, 10)));

     for (const auto& Candidate : PersistedProfileVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : LearnedVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : SearchIdeaVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;
     std::unordered_map<std::string, bool> FreshnessCache;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":" + Collection + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

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
                    Hit.TermOrigin = Hit.MatchedSource;
                    Hit.MatchedPath = "sam_term";
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    Hit.Breakdown.TermScore = Hit.MatchedScore;
                    Hit.Breakdown.FinalScore = Hit.MatchedScore;
                    if (IsSAM25DebugExplainEnabled())
                    {
                         std::ostringstream Stream;
                         Stream << "sam25+ direct score=" << Hit.MatchedScore
                                << " signal=" << Hit.MatchedSignal
                                << " source=" << Hit.MatchedSource;
                         Hit.Explain = Stream.str();
                    }

                    if (!Hit.DocumentID.empty())
                    {
                         std::string ManifestValue;
                         const rocksdb::Status ManifestStatus =
                              DatabaseHandle->Get(rocksdb::ReadOptions(),
                                                  BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                                                  &ManifestValue);

                         if (!ManifestStatus.ok())
                         {
                              continue;
                         }

                         DocumentEntry Entry;

                         if (!ParseManifestValue(ManifestValue, Entry) ||
                             !IsSAMDocumentEntryCurrent(Entry, &FreshnessCache))
                         {
                              continue;
                         }

                         Hit.Title = Entry.Title.empty() ? Hit.Title : Entry.Title;
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     AppendFuzzyFallbackHits(AggregatedHits, DatabaseHandle.get(), Collection, Query, Limit);
     AppendSAMLikePatternHits(AggregatedHits, DatabaseHandle.get(), Collection, Query);
     AppendSemanticProfileHits(AggregatedHits, DatabaseHandle.get(), Collection, SemanticPlan,
                               std::max<size_t>(256, Limit * 24));
     AppendSearchIdeaHits(AggregatedHits, DatabaseHandle.get(), Collection, SearchIdeas);
     const std::vector<SAMLearnedVariant> SeededVariants =
          BuildSeededCollectionVariants(DatabaseHandle.get(), Collection, QueryViews, AggregatedHits,
                                        &ProfileHints.StrongTokens,
                                        std::max<size_t>(6, std::min<size_t>(Limit * 3, 12)));
     AppendCollectionLearnedHits(AggregatedHits, DatabaseHandle.get(), Collection, SeededVariants);

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

     EmitSAM25DebugLog(Query, Hits);
     FinishLookupActivity(ActivitySequence, Hits.size());
     return Hits;
}

std::vector<SAM::SearchIdeaEntry> SAM::GetSearchIdeaHistory(const std::string& Collection,
                                                            size_t Limit) const
{
     std::vector<SearchIdeaEntry> Entries;

     if (Limit == 0)
     {
          return Entries;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return Entries;
     }

     const std::string Prefix = Collection.empty() ? "sam:idea:" : BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          if (!Collection.empty() && Entry.Collection != Collection)
          {
               continue;
          }

          Entries.push_back(std::move(Entry));
     }

     std::sort(Entries.begin(), Entries.end(),
               [](const SearchIdeaEntry& Left, const SearchIdeaEntry& Right)
               {
                    if (Left.LastSeenMS != Right.LastSeenMS)
                    {
                         return Left.LastSeenMS > Right.LastSeenMS;
                    }

                    if (Left.Uses != Right.Uses)
                    {
                         return Left.Uses > Right.Uses;
                    }

                    return Left.Query < Right.Query;
               });

     if (Entries.size() > Limit)
     {
          Entries.resize(Limit);
     }

     return Entries;
}

std::vector<SAM::SearchActivityEntry> SAM::GetActiveSearchActivities(const std::string& Collection) const
{
     std::vector<SearchActivityEntry> Entries;
     std::lock_guard<std::mutex> Lock(SearchActivityMutex);

     for (const auto& Pair : ActiveSearchActivities)
     {
          if (!Collection.empty() && Pair.second.Collection != Collection)
          {
               continue;
          }

          Entries.push_back(Pair.second);
     }

     std::sort(Entries.begin(), Entries.end(),
               [](const SearchActivityEntry& Left, const SearchActivityEntry& Right)
               {
                    if (Left.StartedMS != Right.StartedMS)
                    {
                         return Left.StartedMS > Right.StartedMS;
                    }

                    return Left.Sequence > Right.Sequence;
               });

     return Entries;
}

bool SAM::GetLatestSearchActivity(const std::string& Collection,
                                  SearchActivityEntry& Entry) const
{
     Entry = SearchActivityEntry{};
     const std::string ActivityKey = Collection.empty() ? "*" : Collection;
     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     const auto It = LatestSearchActivityByCollection.find(ActivityKey);

     if (It == LatestSearchActivityByCollection.end())
     {
          return false;
     }

     Entry = It->second;
     return true;
}

std::vector<SAM::DocumentEntry> SAM::ListDocuments(const std::string& Collection, size_t Limit, size_t Offset) const
{
     std::vector<DocumentEntry> Entries;

     if (Collection.empty() || Limit == 0)
     {
          return Entries;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return Entries;
     }

     const std::string Prefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));
     size_t Seen = 0;

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          if (Seen++ < Offset)
          {
               continue;
          }

          Entries.push_back(std::move(Entry));

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

bool SAM::GetCollectionIndexedMutationVersion(const std::string& Collection, uint64_t& Version) const
{
     Version = 0;

     if (Collection.empty())
     {
          return false;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return false;
     }

     return ReadCollectionIndexedMutationVersionLocked(DatabaseHandle.get(), Collection, Version);
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

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     std::string Value;
     const rocksdb::Status Status = DatabaseHandle->Get(rocksdb::ReadOptions(),
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

     if (!IsSAMDocumentEntryCurrent(Entry))
     {
          Entry = DocumentEntry{};

          if (ErrorMessage)
          {
               *ErrorMessage = "SAM document is stale relative to the source table.";
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
