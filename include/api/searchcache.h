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

     void InvalidateCollection(const std::string& Collection);

     void InvalidateAll();

     size_t FlushExpired(uint64_t MaxAgeMS = 3600ULL * 1000ULL);
}
