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

#include "api/searchcache.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
     constexpr uint64_t kSearchCacheTTLMS = 3600ULL * 1000ULL;
     constexpr size_t kSearchCacheMaxEntries = 4096;

     struct CacheEntry
     {
          HttpResponse Response;
          std::string Collection;
          uint64_t CreatedMS = 0;
          uint64_t GlobalEpoch = 0;
     };

     std::mutex CacheMutex;
     std::unordered_map<std::string, CacheEntry> Entries;
     std::deque<std::string> EntryOrder;
     std::unordered_map<std::string, std::unordered_set<std::string>> KeysByCollection;
     uint64_t GlobalEpoch = 1;

     uint64_t CurrentMS()
     {
          return static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
     }

     std::string CanonicalParams(const std::map<std::string, std::string>& Params)
     {
          std::string Result;

          for (const auto& Entry : Params)
          {
               if (!Result.empty())
               {
                    Result.push_back('&');
               }

               Result += Entry.first;
               Result.push_back('=');
               Result += Entry.second;
          }

          return Result;
     }

     std::string BuildKey(const std::string& Namespace, const HttpRequest& Request)
     {
          return Namespace + "\n" +
                 Request.Method + "\n" +
                 Request.Path + "\n" +
                 CanonicalParams(Request.QueryParams) + "\n" +
                 Request.Body + "\n" +
                 Request.EmbeddedFilters + "\n" +
                 Request.APIKeyID;
     }

     void RemoveKeyLocked(const std::string& Key)
     {
          const auto It = Entries.find(Key);
          if (It == Entries.end())
          {
               return;
          }

          auto CollectionIt = KeysByCollection.find(It->second.Collection);
          if (CollectionIt != KeysByCollection.end())
          {
               CollectionIt->second.erase(Key);

               if (CollectionIt->second.empty())
               {
                    KeysByCollection.erase(CollectionIt);
               }
          }

          Entries.erase(It);
     }

     void TrimLocked()
     {
          while (Entries.size() > kSearchCacheMaxEntries && !EntryOrder.empty())
          {
               RemoveKeyLocked(EntryOrder.front());
               EntryOrder.pop_front();
          }
     }
}

bool SearchResponseCache::Get(const std::string& Namespace,
                              const HttpRequest& Request,
                              const std::string& Collection,
                              HttpResponse& Response)
{
     const uint64_t NowMS = CurrentMS();
     const std::string Key = BuildKey(Namespace, Request);
     std::lock_guard<std::mutex> Lock(CacheMutex);

     const auto It = Entries.find(Key);
     if (It == Entries.end())
     {
          return false;
     }

     const CacheEntry& Entry = It->second;
     const bool Expired = NowMS < Entry.CreatedMS || (NowMS - Entry.CreatedMS) >= kSearchCacheTTLMS;
     const bool GlobalStale = Entry.GlobalEpoch != GlobalEpoch;

     if (Expired || GlobalStale)
     {
          RemoveKeyLocked(Key);
          return false;
     }

     Response = Entry.Response;
     Response.Headers["X-HLQ-Cache"] = "hit";
     return true;
}

void SearchResponseCache::Put(const std::string& Namespace,
                              const HttpRequest& Request,
                              const std::string& Collection,
                              const HttpResponse& Response)
{
     if (Response.StatusCode != 200)
     {
          return;
     }

     CacheEntry Entry;
     Entry.Response = Response;
     Entry.Response.Headers["X-HLQ-Cache"] = "stored";
     Entry.Collection = Collection;
     Entry.CreatedMS = CurrentMS();

     const std::string Key = BuildKey(Namespace, Request);
     std::lock_guard<std::mutex> Lock(CacheMutex);

     Entry.GlobalEpoch = GlobalEpoch;
     RemoveKeyLocked(Key);
     Entries[Key] = std::move(Entry);
     KeysByCollection[Collection].insert(Key);
     EntryOrder.push_back(Key);
     TrimLocked();
}

void SearchResponseCache::InvalidateCollection(const std::string& Collection)
{
     std::lock_guard<std::mutex> Lock(CacheMutex);

     if (Collection.empty())
     {
          ++GlobalEpoch;
          Entries.clear();
          EntryOrder.clear();
          KeysByCollection.clear();
          return;
     }

     auto EraseGroup = [&](const std::string& Group)
     {
          const auto It = KeysByCollection.find(Group);
          if (It == KeysByCollection.end())
          {
               return;
          }

          std::vector<std::string> Keys(It->second.begin(), It->second.end());

          for (const auto& Key : Keys)
          {
               RemoveKeyLocked(Key);
          }
     };

     EraseGroup(Collection);
     EraseGroup("*");
}

void SearchResponseCache::InvalidateAll()
{
     std::lock_guard<std::mutex> Lock(CacheMutex);
     ++GlobalEpoch;
     Entries.clear();
     EntryOrder.clear();
     KeysByCollection.clear();
}

size_t SearchResponseCache::FlushExpired(uint64_t MaxAgeMS)
{
     const uint64_t NowMS = CurrentMS();
     size_t Removed = 0;
     std::lock_guard<std::mutex> Lock(CacheMutex);

     for (auto It = Entries.begin(); It != Entries.end();)
     {
          const bool Expired = NowMS < It->second.CreatedMS ||
                               (NowMS - It->second.CreatedMS) >= MaxAgeMS;

          if (Expired)
          {
               const std::string Key = It->first;
               ++It;
               RemoveKeyLocked(Key);
               ++Removed;
          }
          else
          {
               ++It;
          }
     }

     if (Entries.empty())
     {
          EntryOrder.clear();
     }

     return Removed;
}
