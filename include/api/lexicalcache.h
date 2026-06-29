/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 */

#pragma once

#include <cstdint>
#include <string>

namespace LexicalQueryCache
{
     struct Stats
     {
          uint64_t ResourceHits = 0;
          uint64_t ResourceMisses = 0;
          uint64_t ExpansionHits = 0;
          uint64_t ExpansionMisses = 0;
     };

     void InvalidateCollection(const std::string& Collection);
     void InvalidateAll();
     Stats GetStats();
}
