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
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sam/lang.h"

namespace
{
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
     if (Value.size() < 2 || Value.size() > 3)
     {
          return false;
     }

     return std::all_of(Value.begin(), Value.end(),
                        [](unsigned char C)
                        {
                             return std::isalpha(C);
                        });
}

std::string NormalizeLanguageHint(const std::string& Value)
{
     std::string Candidate = TrimLowerCopy(Value);

     if (Candidate.empty())
     {
          return "";
     }

     std::replace(Candidate.begin(), Candidate.end(), '_', '-');

     static const std::unordered_map<std::string, std::string> LanguageAliases = {
          {"english", "en"},
          {"ingles", "en"},
          {"inglés", "en"},
          {"spanish", "es"},
          {"espanol", "es"},
          {"español", "es"},
          {"castellano", "es"},
          {"french", "fr"},
          {"francais", "fr"},
          {"français", "fr"},
          {"portuguese", "pt"},
          {"portugues", "pt"},
          {"português", "pt"},
          {"german", "de"},
          {"deutsch", "de"},
          {"italian", "it"},
          {"italiano", "it"}};

     const auto AliasIt = LanguageAliases.find(Candidate);

     if (AliasIt != LanguageAliases.end())
     {
          return AliasIt->second;
     }

     if (LooksLikeLanguageCode(Candidate))
     {
          return Candidate;
     }

     const size_t Separator = Candidate.find('-');

     if (Separator != std::string::npos)
     {
          const std::string Prefix = Candidate.substr(0, Separator);

          if (LooksLikeLanguageCode(Prefix))
          {
               return Prefix;
          }
     }

     return "";
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

std::string ResolveDocumentLanguageOverride(const std::string& Collection, const Document& Doc)
{
     static const std::array<std::string, 4> LanguageFieldNames = {"lang", "language", "_lang", "locale"};

     for (const auto& Entry : Doc.Fields)
     {
          const std::string Key = TrimLowerCopy(Entry.first);

          if (std::find(LanguageFieldNames.begin(), LanguageFieldNames.end(), Key) == LanguageFieldNames.end())
          {
               continue;
          }

          const std::string Candidate = NormalizeLanguageHint(Entry.second);

          if (!Candidate.empty())
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
               const std::string Candidate = NormalizeLanguageHint(MetadataIt->second);

               if (!Candidate.empty())
               {
                    return Candidate;
               }
          }
     }

     return "";
}

std::string NormalizeTextForLanguageDetection(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         if (std::isalnum(C) || std::isspace(C))
                         {
                              return static_cast<char>(std::tolower(C));
                         }

                         switch (C)
                         {
                              case '-':
                              case '_':
                              case '/':
                              case '.':
                              case ',':
                              case ';':
                              case ':':
                              case '!':
                              case '?':
                              case '(':
                              case ')':
                              case '[':
                              case ']':
                              case '{':
                              case '}':
                              case '"':
                              case '\'':
                                   return ' ';
                              default:
                                   return static_cast<char>(C);
                         }
                    });
     return Value;
}

std::vector<std::string> TokenizeLanguageSample(const std::string& Value)
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

struct LanguageProfile
{
     const char* Code = "und";
     std::unordered_set<std::string> FunctionWords;
     std::vector<std::string> Fragments;
     std::vector<std::string> StrongMarkers;
};

int CountNeedleMatches(const std::string& Haystack, const std::vector<std::string>& Needles)
{
     int Score = 0;

     for (const auto& Needle : Needles)
     {
          size_t Position = Haystack.find(Needle);

          while (Position != std::string::npos)
          {
               ++Score;
               Position = Haystack.find(Needle, Position + Needle.size());
          }
     }

     return Score;
}

int ScoreLanguageProfile(const LanguageProfile& Profile,
                         const std::vector<std::string>& Tokens,
                         const std::string& NormalizedText,
                         const std::string& OriginalText)
{
     int Score = 0;

     for (const auto& Token : Tokens)
     {
          if (Profile.FunctionWords.find(Token) != Profile.FunctionWords.end())
          {
               Score += 2;
          }
     }

     Score += CountNeedleMatches(NormalizedText, Profile.Fragments);
     Score += CountNeedleMatches(OriginalText, Profile.StrongMarkers) * 3;
     return Score;
}
}

namespace sam::lang
{
std::string DetectDocumentLanguage(const std::string& Collection, const Document& Doc)
{
     const std::string ExplicitLanguage = ResolveDocumentLanguageOverride(Collection, Doc);

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

     const std::string Normalized = NormalizeTextForLanguageDetection(Combined);

     if (Normalized.empty())
     {
          return "und";
     }

     const std::vector<std::string> Tokens = TokenizeLanguageSample(Normalized);

     if (Tokens.empty())
     {
          return "und";
     }

     static const std::array<LanguageProfile, 6> Profiles = {{
          {"en",
           {"the", "and", "for", "with", "from", "this", "that", "into", "about", "your", "guide", "overview", "summary"},
           {"tion", "ions", "ing ", "ed ", "ly "},
           {}},
          {"es",
           {"el", "la", "los", "las", "un", "una", "de", "del", "para", "con", "por", "que", "como", "desde", "sobre"},
           {"ción", "ciones", "mente", "años", "está", "será", " para "},
           {"á", "é", "í", "ó", "ú", "ñ", "¿", "¡"}},
          {"fr",
           {"le", "la", "les", "des", "une", "pour", "avec", "dans", "sur", "est", "pas", "par", "qui"},
           {"tion", "ement", "aire", "ique", " pour ", " avec "},
           {"à", "â", "ç", "è", "é", "ê", "ë", "î", "ï", "ô", "ù", "û", "œ"}},
          {"pt",
           {"de", "do", "da", "dos", "das", "para", "com", "uma", "que", "por", "sobre", "não", "mais"},
           {"ção", "ções", "mente", " para ", " com ", " não "},
           {"ã", "á", "â", "à", "ç", "é", "ê", "í", "ó", "ô", "õ", "ú"}},
          {"de",
           {"der", "die", "das", "und", "mit", "für", "ist", "nicht", "von", "ein", "eine", "auf", "über"},
           {"ung", "keit", "lich", "sch", " für ", " und "},
           {"ä", "ö", "ü", "ß"}},
          {"it",
           {"il", "lo", "la", "gli", "le", "per", "con", "che", "una", "del", "della", "sono", "come"},
           {"zione", "zioni", "mente", " per ", " con "},
           {"à", "è", "é", "ì", "ò", "ù"}}
     }};

     std::string BestCode = "und";
     int BestScore = 0;
     int SecondBestScore = 0;

     for (const auto& Profile : Profiles)
     {
          const int Score = ScoreLanguageProfile(Profile, Tokens, Normalized, Combined);

          if (Score > BestScore)
          {
               SecondBestScore = BestScore;
               BestScore = Score;
               BestCode = Profile.Code;
               continue;
          }

          if (Score > SecondBestScore)
          {
               SecondBestScore = Score;
          }
     }

     if (BestScore <= 1)
     {
          return "und";
     }

     const int Margin = BestScore - SecondBestScore;

     if (BestScore < 4 && Margin <= 1)
     {
          return "und";
     }

     return BestCode;
}
}
