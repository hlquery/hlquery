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
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <optional>
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

/* Parse one persisted SAM document manifest into the public document entry shape. */

bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry);

/* Trim leading and trailing ASCII whitespace from a value. */

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

/* Return a lowercase ASCII copy without mutating the input caller owns. */

std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

/* Normalize arbitrary text into SAM's term-comparison surface. */

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

/* Split a normalized value into token units. */

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

/* Collapse possessive fragments created by normalization. */

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
     std::unordered_map<std::string, std::vector<std::string>> SynonymGraph;
     std::unordered_set<std::string> Stopwords;
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
     std::optional<size_t> Distance;
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

constexpr const char* kSAMGlobalLexicalCollection = "__global__";

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
void LoadSAMSynonymGraphForCollection(rocksdb::DB* Database,
                                      const std::string& Collection,
                                      std::unordered_map<std::string, std::vector<std::string>>& SynonymGraph);
void LoadSAMStopwordsForCollection(rocksdb::DB* Database,
                                   const std::string& Collection,
                                   std::unordered_set<std::string>& Stopwords);
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

std::vector<std::string> NormalizeSAMTokensWithStopwords(const std::string& Value,
                                                         bool RemoveStopwords,
                                                         const std::unordered_set<std::string>* Stopwords)
{
     std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Value));
     std::vector<std::string> Result;
     Result.reserve(Tokens.size());

     for (const auto& Token : Tokens)
     {
          const bool CustomStopword = (Stopwords && Stopwords->find(Token) != Stopwords->end());

          if (RemoveStopwords && (CustomStopword || IsSamStopword(Token)))
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

std::vector<std::string> GetSAMTokenAlternatives(const SAMQueryTokenViews& QueryViews,
                                                 const std::string& Token)
{
     std::vector<std::string> Alternatives;
     Alternatives.push_back(Token);

     const auto It = QueryViews.SynonymGraph.find(Token);

     if (It != QueryViews.SynonymGraph.end())
     {
          for (const auto& Candidate : It->second)
          {
               if (Candidate != Token)
               {
                    Alternatives.push_back(Candidate);
               }
          }
     }

     return Alternatives;
}

SAMTokenMatchResult MatchSAMQueryTokenToTermToken(const SAMQueryTokenViews& QueryViews,
                                                  const std::string& QueryToken,
                                                  const std::string& TermToken)
{
     SAMTokenMatchResult Result;
     const std::vector<std::string> Alternatives = GetSAMTokenAlternatives(QueryViews, QueryToken);

     for (size_t Index = 0; Index < Alternatives.size(); ++Index)
     {
          const std::string& Candidate = Alternatives[Index];

          if (Candidate == TermToken)
          {
               Result.Matched = true;
               Result.UsedSynonym = Index > 0;
               Result.Distance = 0U;
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

SAMQueryTokenViews NormalizeSAMQueryTokenViews(rocksdb::DB* Database,
                                               const std::string& Collection,
                                               const std::string& Query)
{
     SAMQueryTokenViews Views;
     Views.Quoted = IsQuotedSAMQuery(Query);
     Views.NormalizedQuery = NormalizeTerm(Query);
     Views.NormalizedPhrase = NormalizeTerm(StripSAMQueryQuotes(Query));
     LoadSAMSynonymGraphForCollection(Database, Collection, Views.SynonymGraph);
     LoadSAMStopwordsForCollection(Database, Collection, Views.Stopwords);
     Views.FullTokens = NormalizeSAMTokensWithStopwords(StripSAMQueryQuotes(Query), false, &Views.Stopwords);
     Views.CoreTokens = NormalizeSAMTokensWithStopwords(StripSAMQueryQuotes(Query), true, &Views.Stopwords);
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

std::vector<std::string> BuildQueryVariants(rocksdb::DB* Database,
                                            const std::string& Collection,
                                            const std::string& Query,
                                            const std::unordered_set<std::string>* StrongTokens = nullptr)
{
     std::vector<std::string> Variants;
     std::unordered_set<std::string> Seen;
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Database, Collection, Query);
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
               if (MatchSAMQueryTokenToTermToken(QueryViews, QueryToken, TermToken).Matched)
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
               if (QueryToken == TermToken)
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
                    if (QueryToken == TermTokens[End])
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
               if (QueryTokens[QueryIndex] == TermTokens[End])
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
               const SAMTokenMatchResult Match = MatchSAMQueryTokenToTermToken(QueryViews, QueryToken, FieldToken);

                    if (!Match.Matched)
                    {
                         continue;
                    }

                    if (!BestMatch.Matched || *Match.Distance < *BestMatch.Distance ||
                        (*Match.Distance == *BestMatch.Distance && !Match.UsedSynonym && BestMatch.UsedSynonym))
                    {
                         BestMatch = Match;
                    }
               }

               if (BestMatch.Matched)
               {
                    Matched++;
                    DistancePenalty += BestMatch.Distance.value_or(0U);
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

constexpr double kSAMSearchIdeaInteractionBaseBoost = 1.20;
constexpr double kSAMSearchIdeaInteractionStepBoost = 0.08;
constexpr double kSAMSearchIdeaInteractionSourceBonus = 0.12;
constexpr double kSAMSearchIdeaInteractionSourceBonusStep = 0.04;

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
               Hit.EvidenceCount = static_cast<size_t>(std::max<uint64_t>(1, Idea.Entry.Uses + DocumentRef.InteractionUses));
               Hit.Breakdown.TermScore = Hit.MatchedScore;
               Hit.Breakdown.SemanticScore = Idea.SemanticScore;
               Hit.Breakdown.SemanticVectorScore = Idea.SemanticScore;
               Hit.Breakdown.EvidenceBonus = std::min(0.18, static_cast<double>(Hit.EvidenceCount) * 0.01);
               Hit.Breakdown.SourceDocBonus = std::min(0.28,
                                                       static_cast<double>(DocumentRef.InteractionUses) *
                                                            kSAMSearchIdeaInteractionSourceBonusStep);
               if (DocumentRef.InteractionUses > 0)
               {
                    Hit.Breakdown.SourceDocBonus = std::max(Hit.Breakdown.SourceDocBonus,
                                                            kSAMSearchIdeaInteractionSourceBonus);
               }
               Hit.Breakdown.FinalScore = ClampSAMScore(Hit.MatchedScore +
                                                        Hit.Breakdown.EvidenceBonus +
                                                        Hit.Breakdown.SourceDocBonus);
               AccumulateSAMHit(AggregatedHits, Hit);
          }
     }
}

/* Include the semantic/profile implementation shard. */

#define HLQUERY_SAM_SPLIT_INCLUDE
#include "sam_semantic.cpp"

/* Include the term-expansion and lookup implementation shard. */

#include "sam_terms_lookup.cpp"
#undef HLQUERY_SAM_SPLIT_INCLUDE
