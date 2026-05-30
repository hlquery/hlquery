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

#include <string>

#include "search/cstore.h"

namespace sam::lang
{
        std::string DetectTextLanguage(const std::string& Text);

        std::string DetectDocumentLanguage(const std::string& Collection, const Document& Doc);

        std::string DetectCollectionLanguage(const std::string& Collection, size_t MaxDocuments = 128);

        bool RefreshCollectionLanguage(const std::string& Collection,
                                 std::string* LanguageOut = nullptr,
                                 size_t MaxDocuments = 128);
}
