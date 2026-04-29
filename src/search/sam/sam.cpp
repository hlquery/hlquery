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
#include "search/sam/sam.h"
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

bool LooksLikeLanguageCode(const std::string& Value)
{
     if (Value.size() != 2)
     {
          return false;
     }

     return std::all_of(Value.begin(), Value.end(),
                        [](unsigned char C)
                        {
                             return std::isalpha(C);
                        });
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

std::string ResolveSAMDocumentLanguageOverride(const std::string& Collection, const Document& Doc)
{
     static const std::array<std::string, 4> LanguageFieldNames = {"lang", "language", "_lang", "locale"};

     for (const auto& Entry : Doc.Fields)
     {
          const std::string Key = TrimLowerCopy(Entry.first);

          if (std::find(LanguageFieldNames.begin(), LanguageFieldNames.end(), Key) == LanguageFieldNames.end())
          {
               continue;
          }

          const std::string Candidate = TrimLowerCopy(Entry.second);

          if (LooksLikeLanguageCode(Candidate))
          {
               return Candidate;
          }
     }

     CollectionConfig Config;

     if (HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
     {
          const auto MetadataIt = Config.Metadata.find("_lang");

          if (MetadataIt != Config.Metadata.end())
          {
               const std::string Candidate = TrimLowerCopy(MetadataIt->second);

               if (LooksLikeLanguageCode(Candidate))
               {
                    return Candidate;
               }
          }
     }

     return "";
}

std::string DetectSAMDocumentLanguage(const std::string& Collection, const Document& Doc)
{
     const std::string ExplicitLanguage = ResolveSAMDocumentLanguageOverride(Collection, Doc);

     if (!ExplicitLanguage.empty())
     {
          return ExplicitLanguage;
     }

     const std::vector<std::pair<std::string, std::string>> TextFields = CollectDocumentTextFields(Doc);
     std::string Combined;

     for (const auto& Entry : TextFields)
     {
          if (!Combined.empty())
          {
               Combined.push_back(' ');
          }

          Combined += Entry.second;

          if (Combined.size() >= 4096)
          {
               Combined.resize(4096);
               break;
          }
     }

     const std::string Lower = ToLowerASCII(Combined);

     if (Lower.empty())
     {
          return "und";
     }

     int SpanishScore = 0;
     int EnglishScore = 0;

     auto CountNeedles = [&Lower](const std::vector<std::string>& Needles)
     {
          int Score = 0;

          for (const auto& Needle : Needles)
          {
               size_t Position = Lower.find(Needle);

               while (Position != std::string::npos)
               {
                    ++Score;
                    Position = Lower.find(Needle, Position + Needle.size());
               }
          }

          return Score;
     };

     SpanishScore += CountNeedles({" el ", " la ", " los ", " las ", " un ", " una ",
                                   " de ", " del ", " para ", " con ", " por ", " que ",
                                   " como ", " este ", " esta ", " desde ", " sobre "});
     EnglishScore += CountNeedles({" the ", " and ", " for ", " with ", " from ", " this ",
                                   " that ", " into ", " about ", " guide ", " overview ",
                                   " article ", " profile ", " summary "});

     SpanishScore += CountNeedles({"cion", "ciones", "mente", " para ", " años", " está", " será"});
     EnglishScore += CountNeedles({"ing ", "tion", "ions", "ly ", "ed ", "ing\n"});

     if (Combined.find("á") != std::string::npos || Combined.find("é") != std::string::npos ||
         Combined.find("í") != std::string::npos || Combined.find("ó") != std::string::npos ||
         Combined.find("ú") != std::string::npos || Combined.find("ñ") != std::string::npos ||
         Combined.find("¿") != std::string::npos || Combined.find("¡") != std::string::npos)
     {
          SpanishScore += 6;
     }

     if (SpanishScore == 0 && EnglishScore == 0)
     {
          return "und";
     }

     if (SpanishScore >= EnglishScore + 2)
     {
          return "es";
     }

     if (EnglishScore >= SpanishScore + 1)
     {
          return "en";
     }

     return SpanishScore > EnglishScore ? "es" : "en";
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

struct SAMSemanticProfile
{
     std::string Subject;
     std::string Summary;
     std::vector<std::string> Aliases;
     std::vector<std::string> Descriptors;
     std::vector<std::string> Queries;
     std::vector<float> Vector;
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
bool IsUsefulLearnedVariant(const std::string& Value,
                            const std::unordered_set<std::string>& QueryTokens);
double ComputeManifestSeedStrength(const SAMQueryTokenViews& QueryViews,
                                   const SAM::TermEntry& Term);

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
          if (!Root.contains("families") || !Root["families"].is_array())
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

          for (const auto& Family : Root["families"])
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
                         ComputeManifestSeedStrength(QueryViews,
                             SAM::TermEntry{NormalizeTerm(Term), "semantic_rewrite", "profile", 0.72, 0.72}));
               }

               if (Match < 0.56)
               {
                    continue;
               }

               for (const auto& Term : FamilyTerms)
               {
                    AppendVariant(Term);

                    if (Variants.size() >= MaxVariants)
                    {
                         return Variants;
                    }
               }
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

bool IsStrongSAMVariantToken(const std::string& Token)
{
     return !(Token.empty() || IsSamStopword(Token) || IsWeakSamToken(Token));
}

bool IsUsefulLearnedVariant(const std::string& Value,
                            const std::unordered_set<std::string>& QueryTokens)
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
          if (!IsStrongSAMVariantToken(Token))
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

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) || Entry.Terms.empty())
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

               if (!IsUsefulLearnedVariant(Candidate, QueryTokenSet))
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

          if (!ParseManifestValue(ManifestValue, Entry))
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

               if (!IsUsefulLearnedVariant(Candidate, QueryTokenSet))
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

          if (!ParseManifestValue(Iterator->value().ToString(), Entry))
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
               SAMSemanticProfile Profile;

               if (!ParseSemanticProfileJSON(Root, Profile))
               {
                    SAM::DocumentEntry Entry;

                    if (!ParseManifestValue(Iterator->value().ToString(), Entry))
                    {
                         continue;
                    }

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
               Hit.Title = Root.value("title", "");
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

          if (!ParseManifestValue(Iterator->value().ToString(), Entry))
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

std::string BuildCollectionStateKey(const std::string& Collection)
{
     return "sam:state:" + Collection;
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
                                                 std::string* ErrorMessage = nullptr)
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

bool RebuildCollectionProfileLocked(rocksdb::DB* Database,
                                    const std::string& Collection,
                                    std::string* ErrorMessage = nullptr)
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
     const size_t MinTermSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinRelatedSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilySupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyDescriptorSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyAliasSupport = 1;
     const size_t MinFamilyQuerySupport = 1;

     std::vector<SAMProfileEntry> SortedTerms;
     SortedTerms.reserve(RankedTerms.size());

     for (auto& Pair : RankedTerms)
     {
          SAMProfileEntry Entry = Pair.second;

          if (Entry.Support < MinTermSupport)
          {
               continue;
          }

          auto RelatedIt = RelatedCounts.find(Entry.Text);

          if (RelatedIt != RelatedCounts.end())
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

     for (const auto& Entry : SortedTerms)
     {
          Profile["terms"].push_back({
               {"text", Entry.Text},
               {"score", Entry.Score},
               {"support", Entry.Support},
               {"related", Entry.Related}
          });
     }

     std::vector<SAMProfileFamily> SortedFamilies;
     SortedFamilies.reserve(Families.size());

     for (auto& Pair : Families)
     {
          SAMProfileFamily Family = Pair.second;

          if (Family.Support < MinFamilySupport)
          {
               continue;
          }

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

          if (const auto AliasIt = FamilyAliasCounts.find(Family.Subject); AliasIt != FamilyAliasCounts.end())
          {
               AppendRankedValues(AliasIt->second, Family.Aliases, MinFamilyAliasSupport, 6);
          }

          if (const auto DescriptorIt = FamilyDescriptorCounts.find(Family.Subject);
              DescriptorIt != FamilyDescriptorCounts.end())
          {
               AppendRankedValues(DescriptorIt->second, Family.Descriptors, MinFamilyDescriptorSupport, 8);
          }

          if (const auto QueryIt = FamilyQueryCounts.find(Family.Subject); QueryIt != FamilyQueryCounts.end())
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

     for (const auto& Family : SortedFamilies)
     {
          Profile["families"].push_back({
               {"subject", Family.Subject},
               {"score", Family.Score},
               {"support", Family.Support},
               {"aliases", Family.Aliases},
               {"descriptors", Family.Descriptors},
               {"queries", Family.Queries}
          });
     }

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

          if (!Root.contains("terms") || !Root["terms"].is_array())
          {
               return Variants;
          }

          std::unordered_set<std::string> QueryTokens(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
          std::unordered_map<std::string, SAMLearnedVariant> Ranked;

          for (const auto& Item : Root["terms"])
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

               const double EntryScore = ClampSAMScore(Item.value("score", 0.0));
               const size_t Support = static_cast<size_t>(std::max<int64_t>(0, Item.value("support", 0)));
               const double MatchStrength =
                    IsUsefulLearnedVariant(Text, QueryTokens) ? 0.0 :
                    std::max(ComputeManifestSeedStrength(QueryViews, SAM::TermEntry{Text, "collection_profile", "profile", EntryScore, EntryScore}), 0.0);

               if (MatchStrength < 0.58)
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

                         if (!IsUsefulLearnedVariant(Candidate, QueryTokens))
                         {
                              continue;
                         }

                         SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                         RankedEntry.Text = Candidate;
                         RankedEntry.Score += (MatchStrength * 0.60) + (EntryScore * 0.25) +
                                              ClampSAMScore(static_cast<double>(Support) / 8.0) * 0.15;
                         ++RankedEntry.Support;
                    }
               }
          }

          if (Root.contains("families") && Root["families"].is_array())
          {
               for (const auto& FamilyItem : Root["families"])
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

                    const double FamilyScore = ClampSAMScore(FamilyItem.value("score", 0.0));
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

                    if (FamilyMatch < 0.56)
                    {
                         continue;
                    }

                    auto AccumulateVariant = [&](const std::string& CandidateText, double Weight)
                    {
                         const std::string Candidate = NormalizeTerm(CandidateText);

                         if (!IsUsefulLearnedVariant(Candidate, QueryTokens))
                         {
                              return;
                         }

                         SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                         RankedEntry.Text = Candidate;
                         RankedEntry.Score += (FamilyMatch * Weight) +
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
          Entry.Collection = Root.value("collection", "");
          Entry.DocumentID = Root.value("id", "");
          Entry.Title = Root.value("title", "");
          Entry.Lang = Root.value("lang", "und");
          Entry.Label = Root.value("label", "article");
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
          DatabaseOpen.store(true, std::memory_order_release);

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
     DatabaseOpen.store(false, std::memory_order_release);
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
                    std::string ProfileError;

                    {
                         std::lock_guard<std::mutex> DBLock(DBMutex);
                         (void)RebuildCollectionProfileLocked(Database.get(), Job.Collection, &ProfileError);
                         std::string StateError;
                         const uint64_t MutationVersion =
                              (Instance && Instance->API)
                                   ? Instance->API->GetCollectionMutationVersion(Job.Collection)
                                   : 0;
                         (void)WriteCollectionIndexedMutationVersionLocked(Database.get(),
                                                                          Job.Collection,
                                                                          MutationVersion,
                                                                          &StateError);
                    }

                    Status.Running = false;
                    Status.Completed = true;

                    if (Success)
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments) +
                                               (ProfileError.empty() ? "" : ", profile error: " + ProfileError));
                    }
                    else
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete with failures: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments) +
                                               (ProfileError.empty() ? "" : ", profile error: " + ProfileError));
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
     return DatabaseOpen.load(std::memory_order_acquire);
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

     std::string ProfileError;

     if (!RebuildCollectionProfileLocked(Database.get(), Collection, &ProfileError))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "Failed to rebuild SAM collection profile for '" + Collection +
                                           "': " + ProfileError + ".");
          }

          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = ProfileError;
          }
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

          CollectionJobStatus& JobStatus = CollectionJobs[Collection];
          JobStatus = CollectionJobStatus{};
          JobStatus.Running = true;
          JobStatus.Completed = false;
          JobStatus.ErrorMessage.clear();
     }

     RecordDebugEvent(Collection,
                      "queued background rebuild setup");

     std::thread([this, Collection]()
     {
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

               if (!DeleteDocument(Collection, DocumentID, &RemoveError))
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);
                    CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                    if (JobStatus.ErrorMessage.empty())
                    {
                         JobStatus.ErrorMessage = RemoveError;
                    }
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
               JobStatus.PendingDocuments = DocumentsToQueue.size();
               JobStatus.TotalDocuments = DocumentsToQueue.size();
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
               std::string StateError;
               {
                    std::lock_guard<std::mutex> DBLock(DBMutex);
                    const uint64_t MutationVersion =
                         (Instance && Instance->API)
                              ? Instance->API->GetCollectionMutationVersion(Collection)
                              : 0;
                    (void)WriteCollectionIndexedMutationVersionLocked(Database.get(),
                                                                     Collection,
                                                                     MutationVersion,
                                                                     &StateError);
               }

               std::lock_guard<std::mutex> Lock(JobMutex);
               CollectionJobStatus& JobStatus = CollectionJobs[Collection];
               JobStatus.Running = false;
               JobStatus.Completed = true;
               RecordDebugEvent(Collection, "rebuild complete: indexed 0, failed 0");
          }
     }).detach();

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
     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     std::string TermsError;
     const std::vector<TermEntry> Terms = ExpandDocumentTerms(Collection, Doc, &TermsError);
     if (Terms.empty())
     {
          const std::string FailureMessage = TermsError.empty()
               ? std::string("SAM indexing produced no usable terms.")
               : TermsError;

          RecordDebugEvent(Collection, "failed to index " + Doc.ID + ": " + FailureMessage);

          if (ErrorMessage)
          {
               *ErrorMessage = FailureMessage;
          }

          return false;
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

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!RemoveExistingDocumentTermsLocked(Collection, Doc.ID, ErrorMessage))
     {
          return false;
     }

     rocksdb::WriteBatch Batch;
     nlohmann::json Manifest;
     Manifest["collection"] = Collection;
     Manifest["id"] = Doc.ID;
     Manifest["title"] = Doc.Title;
     Manifest["lang"] = DetectSAMDocumentLanguage(Collection, Doc);
     Manifest["label"] = DetectSAMDocumentLabel(Doc);
     Manifest["terms"] = nlohmann::json::array();
     const SAMSemanticProfile SemanticProfile = BuildSemanticProfile(Doc.Title.empty() ? Doc.ID : Doc.Title, Terms);

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

     StoreSemanticProfileJSON(Manifest, SemanticProfile);

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

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Query);

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Hits;
     }

     std::vector<std::string> Variants = BuildQueryVariants(Query);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(Database.get(), "", Query, QueryViews);

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          return Hits;
     }

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
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     AppendFuzzyFallbackHits(AggregatedHits, Database.get(), "", Query, Limit);
     AppendSemanticProfileHits(AggregatedHits, Database.get(), "", SemanticPlan,
                               std::max<size_t>(256, Limit * 24));

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

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

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Hits;
     }

     std::vector<std::string> Variants = BuildQueryVariants(Query);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(Database.get(), Collection, Query, QueryViews);
     const std::vector<std::string> PersistedProfileVariants =
          BuildPersistedCollectionProfileVariants(Database.get(), Collection, QueryViews,
                                                 std::max<size_t>(8, std::min<size_t>(Limit * 3, 16)));
     const std::vector<std::string> LearnedVariants =
          BuildCollectionLearnedVariants(Database.get(), Collection, Query, QueryViews,
                                         std::max<size_t>(8, std::min<size_t>(Limit * 3, 16)));

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

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          return Hits;
     }

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

     AppendFuzzyFallbackHits(AggregatedHits, Database.get(), Collection, Query, Limit);
     AppendSemanticProfileHits(AggregatedHits, Database.get(), Collection, SemanticPlan,
                               std::max<size_t>(256, Limit * 24));
     const std::vector<SAMLearnedVariant> SeededVariants =
          BuildSeededCollectionVariants(Database.get(), Collection, QueryViews, AggregatedHits,
                                        std::max<size_t>(6, std::min<size_t>(Limit * 3, 12)));
     AppendCollectionLearnedHits(AggregatedHits, Database.get(), Collection, SeededVariants);

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, Limit);

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

     std::unique_lock<std::mutex> Lock(JobMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          return false;
     }

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
     std::unique_lock<std::mutex> Lock(JobMutex, std::try_to_lock);

     if (!Lock.owns_lock())
     {
          return {};
     }

     return CollectionJobs;
}

bool SAM::GetCollectionIndexedMutationVersion(const std::string& Collection, uint64_t& Version) const
{
     Version = 0;

     if (Collection.empty())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     return ReadCollectionIndexedMutationVersionLocked(Database.get(), Collection, Version);
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
