#pragma once

#include <string>

#include "search/cstore.h"

namespace sam::lang
{
inline std::string DetectDocumentLanguage(const std::string&, const Document&)
{
     return "und";
}

inline std::string DetectCollectionLanguage(const std::string&, size_t = 128)
{
     return "und";
}
}
