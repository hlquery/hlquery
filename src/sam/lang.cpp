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
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sam/lang.h"
#include "cld2/public/compact_lang_det.h"

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

std::string NormalizeCLD2LanguageCode(const CLD2::Language Language)
{
     if (Language == CLD2::UNKNOWN_LANGUAGE)
     {
          return "und";
     }

     const char* Code = CLD2::LanguageCode(Language);

     if (Code == nullptr || Code[0] == '\0')
     {
          return "und";
     }

     std::string Normalized = ToLowerASCII(Code);

     if (Normalized == "un" || Normalized == "unknown")
     {
          return "und";
     }

     const size_t Dash = Normalized.find('-');

     if (Dash != std::string::npos && Dash > 0)
     {
          Normalized.resize(Dash);
     }

     return Normalized;
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

std::string NormalizeTextForCLD2(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         if (std::iscntrl(C) && !std::isspace(C))
                         {
                              return ' ';
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
                                   return ' ';
                              default:
                                   return static_cast<char>(C);
                         }
                    });
     return Value;
}

std::string BuildDocumentLanguageSample(const Document& Doc, size_t MaxBytes = 8192)
{
     const std::vector<std::pair<std::string, std::string>> TextFields = CollectDocumentTextFields(Doc);
     std::string Combined;

     for (const auto& Entry : TextFields)
     {
          if (!Combined.empty())
          {
               Combined.push_back(' ');
          }

          Combined += Entry.second;

          if (Combined.size() >= MaxBytes)
          {
               Combined.resize(MaxBytes);
               break;
          }
     }

     return NormalizeTextForCLD2(Combined);
}

bool IsUsefulCLD2Result(const CLD2::Language Language,
                        const int Percent,
                        const double NormalizedScore,
                        const int TextBytes,
                        const bool IsReliable)
{
     if (Language == CLD2::UNKNOWN_LANGUAGE || TextBytes < 16 || Percent < 45)
     {
          return false;
     }

     if (IsReliable)
     {
          return Percent >= 50 && NormalizedScore >= 0.10;
     }

     if (TextBytes >= 96 && Percent >= 75 && NormalizedScore >= 0.25)
     {
          return true;
     }

     if (TextBytes >= 256 && Percent >= 65 && NormalizedScore >= 0.18)
     {
          return true;
     }

     return false;
}

std::string DetectLanguageWithCld2(const std::string& Text)
{
     const std::string Normalized = NormalizeTextForCLD2(Text);

     if (Normalized.empty() || Normalized.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
     {
          return "und";
     }

     CLD2::Language Languages[3] = {
          CLD2::UNKNOWN_LANGUAGE,
          CLD2::UNKNOWN_LANGUAGE,
          CLD2::UNKNOWN_LANGUAGE};
     int Percent[3] = {0, 0, 0};
     double NormalizedScore[3] = {0.0, 0.0, 0.0};
     CLD2::ResultChunkVector Chunks;
     int TextBytes = 0;
     bool IsReliable = false;
     int ValidPrefixBytes = 0;
     const CLD2::CLDHints Hints = {nullptr, nullptr, 0, CLD2::UNKNOWN_LANGUAGE};

     const CLD2::Language Summary = CLD2::ExtDetectLanguageSummaryCheckUTF8(
          Normalized.data(),
          static_cast<int>(Normalized.size()),
          true,
          &Hints,
          0,
          Languages,
          Percent,
          NormalizedScore,
          &Chunks,
          &TextBytes,
          &IsReliable,
          &ValidPrefixBytes);

     if (ValidPrefixBytes != static_cast<int>(Normalized.size()))
     {
          return "und";
     }

     if (IsUsefulCLD2Result(Summary, Percent[0], NormalizedScore[0], TextBytes, IsReliable))
     {
          return NormalizeCLD2LanguageCode(Summary);
     }

     for (size_t Index = 0; Index < 3; ++Index)
     {
          if (IsUsefulCLD2Result(Languages[Index], Percent[Index], NormalizedScore[Index], TextBytes, IsReliable))
          {
               return NormalizeCLD2LanguageCode(Languages[Index]);
          }
     }

     return "und";
}

std::string DetectDocumentLanguageInternal(const std::string& Collection,
                                           const Document& Doc,
                                           bool AllowCollectionOverride)
{
     if (AllowCollectionOverride)
     {
          const std::string ExplicitLanguage = ResolveDocumentLanguageOverride(Collection, Doc);

          if (!ExplicitLanguage.empty())
          {
               return ExplicitLanguage;
          }
     }
     else
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
     }

     return DetectLanguageWithCld2(BuildDocumentLanguageSample(Doc));
}
}

namespace sam::lang
{
std::string DetectTextLanguage(const std::string& Text)
{
     return DetectLanguageWithCld2(Text);
}

std::string DetectDocumentLanguage(const std::string& Collection, const Document& Doc)
{
     return DetectDocumentLanguageInternal(Collection, Doc, true);
}

std::string DetectCollectionLanguage(const std::string& Collection, size_t MaxDocuments)
{
     if (Collection.empty() || MaxDocuments == 0)
     {
          return "und";
     }

     constexpr int BatchSize = 128;
     size_t SeenDocuments = 0;
     int Offset = 0;
     std::map<std::string, double> WeightedVotes;
     std::string Combined;
     Combined.reserve(65536);

     while (SeenDocuments < MaxDocuments)
     {
          const int Limit = static_cast<int>(std::min<size_t>(BatchSize, MaxDocuments - SeenDocuments));
          const std::vector<Document> Documents = HybridStorageManagerInstance().ListDocuments(Collection, Limit, Offset);

          if (Documents.empty())
          {
               break;
          }

          for (const auto& Doc : Documents)
          {
               const std::string DocumentLang = DetectDocumentLanguageInternal(Collection, Doc, false);

               if (!DocumentLang.empty() && DocumentLang != "und")
               {
                    WeightedVotes[DocumentLang] += 1.0;
               }

               if (Combined.size() < 262144)
               {
                    if (!Combined.empty())
                    {
                         Combined.push_back('\n');
                    }

                    Combined += BuildDocumentLanguageSample(Doc, 4096);
               }
          }

          SeenDocuments += Documents.size();
          Offset += static_cast<int>(Documents.size());

          if (Documents.size() < static_cast<size_t>(Limit))
          {
               break;
          }
     }

     const std::string CorpusLanguage = DetectLanguageWithCld2(Combined);

     if (!CorpusLanguage.empty() && CorpusLanguage != "und")
     {
          WeightedVotes[CorpusLanguage] += std::max<double>(3.0, static_cast<double>(SeenDocuments) * 0.35);
     }

     std::string BestLanguage = "und";
     double BestScore = 0.0;
     double SecondScore = 0.0;

     for (const auto& Vote : WeightedVotes)
     {
          if (Vote.second > BestScore)
          {
               SecondScore = BestScore;
               BestScore = Vote.second;
               BestLanguage = Vote.first;
          }
          else if (Vote.second > SecondScore)
          {
               SecondScore = Vote.second;
          }
     }

     if (BestScore <= 0.0)
     {
          return "und";
     }

     if (SecondScore > 0.0 && BestScore < SecondScore * 1.25)
     {
          return "und";
     }

     return BestLanguage;
}

bool RefreshCollectionLanguage(const std::string& Collection,
                               std::string* LanguageOut,
                               size_t MaxDocuments)
{
     const std::string Language = DetectCollectionLanguage(Collection, MaxDocuments);

     if (LanguageOut)
     {
          *LanguageOut = Language;
     }

     if (Collection.empty() || Language.empty() || Language == "und")
     {
          return false;
     }

     return HybridStorageManagerInstance().UpdateCollectionMetadata(Collection, "_lang", Language);
}
}
