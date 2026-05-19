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

/*
 * CODE STYLE AND FORMATTING RULES
 *
 * Use block comments only. Never use line comments.
 * Put opening braces on their own line for functions, loops, conditionals, classes, structs, and namespaces.
 * Indent with spaces only, using exactly five spaces per nested block level.
 * Keep system includes before local includes.
 * Use PascalCase for function names and avoid trailing underscores in variable names.
 * Keep log messages on one physical line and end each message with a period.
 * Do not add comments to namespace closing braces.
 * Place a blank line between a comment and the following code element.
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
