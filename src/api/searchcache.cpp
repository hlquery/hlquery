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

     constexpr uint64_t kDefaultSearchCacheTTLMS = 3600ULL * 1000ULL;
     constexpr size_t kDefaultSearchCacheMaxSize = 512ULL * 1024ULL * 1024ULL;

     struct CacheEntry
     {
          HttpResponse Response;
          std::string Collection;
          uint64_t CreatedMS = 0;
          uint64_t GlobalEpoch = 0;
          uint64_t CollectionGeneration = 0;
          uint64_t OrderSequence = 0;
          size_t SizeBytes = 0;
     };

     struct CacheOrderEntry
     {
          std::string Key;
          uint64_t Sequence = 0;
     };

     std::mutex CacheMutex;
     std::unordered_map<std::string, CacheEntry> Entries;
     std::deque<CacheOrderEntry> EntryOrder;
     std::unordered_map<std::string, std::unordered_set<std::string>> KeysByCollection;
     uint64_t GlobalEpoch = 1;
     uint64_t MutationClock = 1;
     uint64_t OrderClock = 1;
     std::unordered_map<std::string, uint64_t> CollectionGenerations;
     uint64_t CacheTTLMS = kDefaultSearchCacheTTLMS;
     size_t CacheMaxSizeBytes = kDefaultSearchCacheMaxSize;
     size_t CacheSizeBytes = 0;
     uint64_t CacheHits = 0;
     uint64_t CacheMisses = 0;
     uint64_t CacheExpired = 0;
     uint64_t CacheEvictions = 0;

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

     std::string CanonicalBody(const std::string& Body)
     {
          if (Body.empty())
          {
               return Body;
          }

          try
          {
               return nlohmann::json::parse(Body).dump();
          }
          catch (...)
          {
               return Body;
          }
     }

     std::string BuildKey(const std::string& Namespace, const HttpRequest& Request)
     {
          return Namespace + "\n" +
                 Request.Method + "\n" +
                 Request.Path + "\n" +
                 CanonicalParams(Request.QueryParams) + "\n" +
                 CanonicalBody(Request.Body) + "\n" +
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

          CacheSizeBytes = It->second.SizeBytes > CacheSizeBytes ? 0 : CacheSizeBytes - It->second.SizeBytes;
          Entries.erase(It);
     }

     void TrimLocked()
     {
          while (CacheSizeBytes > CacheMaxSizeBytes && !EntryOrder.empty())
          {
               const CacheOrderEntry Candidate = EntryOrder.front();
               EntryOrder.pop_front();

               const auto It = Entries.find(Candidate.Key);
               if (It == Entries.end() || It->second.OrderSequence != Candidate.Sequence)
               {
                    continue;
               }

               ++CacheEvictions;
               RemoveKeyLocked(Candidate.Key);
          }
     }

     void CompactOrderLocked()
     {
          if (EntryOrder.size() <= (Entries.size() * 2) + 1024)
          {
               return;
          }

          std::deque<CacheOrderEntry> Compacted;

          for (const auto& Candidate : EntryOrder)
          {
               const auto It = Entries.find(Candidate.Key);
               if (It != Entries.end() && It->second.OrderSequence == Candidate.Sequence)
               {
                    Compacted.push_back(Candidate);
               }
          }

          EntryOrder.swap(Compacted);
     }

     size_t EstimateEntrySize(const std::string& Key, const HttpResponse& Response)
     {
          size_t Size = Key.size() + Response.Body.size() + Response.StatusText.size();
          for (const auto& Header : Response.Headers)
          {
               Size += Header.first.size() + Header.second.size();
          }
          return Size + sizeof(CacheEntry);
     }

     uint64_t CurrentGenerationLocked(const std::string& Collection)
     {
          const auto It = CollectionGenerations.find(Collection);
          return std::max(GlobalEpoch, It == CollectionGenerations.end() ? 0 : It->second);
     }

void SearchResponseCache::Configure(uint64_t TTLMS, size_t MaxSizeBytes)
{
     std::lock_guard<std::mutex> Lock(CacheMutex);
     CacheTTLMS = std::max<uint64_t>(1, TTLMS);
     CacheMaxSizeBytes = std::max<size_t>(1, MaxSizeBytes);
     TrimLocked();
}

SearchResponseCache::Stats SearchResponseCache::GetStats()
{
     std::lock_guard<std::mutex> Lock(CacheMutex);
     Stats Result;
     Result.Hits = CacheHits;
     Result.Misses = CacheMisses;
     Result.Expired = CacheExpired;
     Result.Evictions = CacheEvictions;
     Result.Entries = Entries.size();
     Result.SizeBytes = CacheSizeBytes;
     return Result;
}

uint64_t SearchResponseCache::GetGeneration(const std::string& Collection)
{
     std::lock_guard<std::mutex> Lock(CacheMutex);
     return CurrentGenerationLocked(Collection);
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
          ++CacheMisses;
          return false;
     }

     const CacheEntry& Entry = It->second;
     const bool Expired = NowMS < Entry.CreatedMS || (NowMS - Entry.CreatedMS) >= CacheTTLMS;
     const bool GlobalStale = Entry.GlobalEpoch != GlobalEpoch;
     const bool CollectionStale = Entry.CollectionGeneration != CurrentGenerationLocked(Collection);

     if (Expired || GlobalStale || CollectionStale)
     {
          ++CacheMisses;
          if (Expired)
          {
               ++CacheExpired;
          }
          RemoveKeyLocked(Key);
          return false;
     }

     Response = Entry.Response;
     ++CacheHits;
     Response.Headers["X-HLQ-Cache"] = "hit";
     return true;
}

void SearchResponseCache::Put(const std::string& Namespace,
                              const HttpRequest& Request,
                              const std::string& Collection,
                              const HttpResponse& Response,
                              uint64_t ExpectedGeneration)
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
     Entry.SizeBytes = EstimateEntrySize(Key, Entry.Response);
     std::lock_guard<std::mutex> Lock(CacheMutex);

     const uint64_t CurrentGeneration = CurrentGenerationLocked(Collection);
     if (ExpectedGeneration != 0 && ExpectedGeneration != CurrentGeneration)
     {
          return;
     }
     Entry.GlobalEpoch = GlobalEpoch;
     Entry.CollectionGeneration = CurrentGeneration;
     Entry.OrderSequence = ++OrderClock;
     RemoveKeyLocked(Key);
     Entries[Key] = std::move(Entry);
     CacheSizeBytes += Entries[Key].SizeBytes;
     KeysByCollection[Collection].insert(Key);
     EntryOrder.push_back({Key, Entries[Key].OrderSequence});
     TrimLocked();
     CompactOrderLocked();
}

void SearchResponseCache::InvalidateCollection(const std::string& Collection)
{
     std::lock_guard<std::mutex> Lock(CacheMutex);

     if (Collection.empty())
     {
          GlobalEpoch = ++MutationClock;
          Entries.clear();
          EntryOrder.clear();
          KeysByCollection.clear();
          CacheSizeBytes = 0;
          return;
     }

     CollectionGenerations[Collection] = ++MutationClock;

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
     GlobalEpoch = ++MutationClock;
     Entries.clear();
     EntryOrder.clear();
     KeysByCollection.clear();
     CacheSizeBytes = 0;
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
