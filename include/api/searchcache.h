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

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "api/httpserver.h"

namespace SearchResponseCache
{
     bool Get(const std::string& Namespace,
              const HttpRequest& Request,
              const std::string& Collection,
              HttpResponse& Response);

     void Put(const std::string& Namespace,
              const HttpRequest& Request,
              const std::string& Collection,
              const HttpResponse& Response);

     /* Removes cached search and collection responses associated with one collection. */

     void InvalidateCollection(const std::string& Collection);

     void InvalidateAll();

     size_t FlushExpired(uint64_t MaxAgeMS = 3600ULL * 1000ULL);
}
