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

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "search/cstore.h"
#include "vendor/cld2/public/compact_lang_det.h"
#include "vendor/cld2/internal/lang_script.h"

namespace hlquery::lang
{
inline std::string NormalizeLanguageCode(const char* code)
{
     if (!code || !*code)
     {
          return "und";
     }

     std::string out(code);
     std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
     {
          return static_cast<char>(std::tolower(c));
     });

     return out;
}

inline std::string DetectTextLanguage(const std::string& text)
{
     if (text.empty())
     {
          return "und";
     }

     bool reliable = false;
     const CLD2::Language lang = CLD2::DetectLanguage(text.c_str(),
                                                     static_cast<int>(text.size()),
                                                     true,
                                                     &reliable);
     const char* code = CLD2::LanguageCode(lang);
     const std::string normalized = NormalizeLanguageCode(code);

     if (normalized == "unknown" || normalized == "un")
     {
          return "und";
     }

     return normalized;
}

inline std::string DetectDocumentLanguage(const std::string&, const Document& doc)
{
     std::string combined;
     combined.reserve(doc.Title.size() + doc.Content.size() + 64);
     combined.append(doc.Title);
     combined.push_back(' ');
     combined.append(doc.Content);

     for (const auto& [key, value] : doc.Fields)
     {
          combined.push_back(' ');
          combined.append(key);
          combined.push_back(' ');
          combined.append(value);
     }

     return DetectTextLanguage(combined);
}

inline std::string DetectCollectionLanguage(const std::string& collection, size_t max_documents = 128)
{
     std::string sample;
     sample.reserve(max_documents * 256);

     const std::vector<Document> documents = HybridStorageManagerInstance().ListDocuments(
          collection,
          static_cast<int>(max_documents),
          0);

     for (const auto& doc : documents)
     {
          sample.append(doc.Title);
          sample.push_back(' ');
          sample.append(doc.Content);
          sample.push_back(' ');

          for (const auto& [key, value] : doc.Fields)
          {
               sample.append(key);
               sample.push_back(' ');
               sample.append(value);
               sample.push_back(' ');
          }

          if (sample.size() >= 16384)
          {
               break;
          }
     }

     return DetectTextLanguage(sample);
}
}
