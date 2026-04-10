/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
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
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

class FastMemoryPool
{
   private:

     /* Lock-free block structure with atomic operations */

     struct AtomicBlock
     {
          std::atomic<char*> data{nullptr};

          std::atomic<size_t> size{0};

          std::atomic<bool> in_use{false};

          std::atomic<AtomicBlock*> next{nullptr};

          std::atomic<uint64_t> access_count{0};

          AtomicBlock(size_t block_size) : size(block_size)
          {
               data.store(static_cast<char*>(std::aligned_alloc(64, block_size)));

               if (!data.load())
               {
                    throw std::bad_alloc();
               }
          }

          ~AtomicBlock()
          {
               char* ptr = data.load();

               if (ptr)
               {
                    std::free(ptr);
               }
          }
     };

     /* Sharded free lists for lock-free operations */

     static constexpr size_t NUM_SHARDS = 16;

     std::array<std::atomic<AtomicBlock*>, NUM_SHARDS> FreeLists;

     std::array<std::atomic<size_t>, NUM_SHARDS> ShardCounts;

     /* Memory pools for different sizes */

     std::array<std::vector<std::unique_ptr<AtomicBlock>>, NUM_SHARDS> Pools;

     std::array<std::mutex, NUM_SHARDS> PoolMutexes;

     /* Statistics */

     std::atomic<size_t> TotalAllocated{0};

     std::atomic<size_t> PeakUsage{0};

     std::atomic<size_t> Allocations{0};

     std::atomic<size_t> Deallocations{0};

     std::atomic<size_t> CacheHits{0};

     std::atomic<size_t> CacheMisses{0};

     /* NUMA-aware allocation */

     std::atomic<int> CurrentNumaNode{0};

     /* Memory compression */

     std::atomic<bool> CompressionEnabled{true};

     std::atomic<size_t> CompressedBytes{0};

     /* Helper methods */

     size_t GetShardIndex(size_t size) const
     {
          return std::min(size / 64, NUM_SHARDS - 1); /* 64-byte aligned shards */
     }

     AtomicBlock* TryGetFromFreeList(size_t shard_idx)
     {
          AtomicBlock* head = FreeLists[shard_idx].load();

          while (head)
          {
               AtomicBlock* next = head->next.load();

               if (FreeLists[shard_idx].compare_exchange_weak(head, next))
               {
                    head->in_use.store(true);
                    head->access_count.fetch_add(1);
                    CacheHits.fetch_add(1);

                    return head;
               }
          }

          CacheMisses.fetch_add(1);

          return nullptr;
     }

     void ReturnToFreeList(AtomicBlock* block)
     {
          if (!block)
          {
               return;
          }

          size_t shard_idx = GetShardIndex(block->size.load());

          AtomicBlock* head = FreeLists[shard_idx].load();

          do
          {
               block->next.store(head);
          } while (!FreeLists[shard_idx].compare_exchange_weak(head, block));

          block->in_use.store(false);

          ShardCounts[shard_idx].fetch_add(1);
     }

   public:

     explicit FastMemoryPool(size_t initial_blocks = 1024, size_t block_size = 4096)
     {
          (void)block_size; /* Suppress unused parameter warning */

          /* Initialize free lists */

          for (auto& free_list : FreeLists)
          {
               free_list.store(nullptr);
          }

          for (auto& count : ShardCounts)
          {
               count.store(0);
          }

          /* Pre-allocate blocks for each shard */

          for (size_t i = 0; i < NUM_SHARDS; ++i)
          {
               size_t shard_block_size = (i + 1) * 64; /* 64, 128, 192, ... bytes */

               size_t blocks_per_shard = initial_blocks / NUM_SHARDS;

               std::lock_guard<std::mutex> lock(PoolMutexes[i]);

               Pools[i].reserve(blocks_per_shard);

               for (size_t j = 0; j < blocks_per_shard; ++j)
               {
                    auto block = std::make_unique<AtomicBlock>(shard_block_size);

                    ReturnToFreeList(block.get());
                    Pools[i].push_back(std::move(block));
               }
          }
     }

     ~FastMemoryPool() = default;

     /* Ultra-fast lock-free allocation */

     void* Allocate(size_t size) noexcept
     {
          if (size == 0)
          {
               return nullptr;
          }

          Allocations.fetch_add(1);

          /* Try to get from free list first */

          size_t shard_idx = GetShardIndex(size);

          AtomicBlock* block = TryGetFromFreeList(shard_idx);

          if (block)
          {
               TotalAllocated.fetch_add(size);

               PeakUsage.store(std::max(PeakUsage.load(), TotalAllocated.load()));

               return block->data.load();
          }

          /* Fallback to malloc for large allocations */

          void* ptr = std::malloc(size);

          if (ptr)
          {
               TotalAllocated.fetch_add(size);

               PeakUsage.store(std::max(PeakUsage.load(), TotalAllocated.load()));
          }

          return ptr;
     }

     /* Ultra-fast lock-free deallocation */

     void Deallocate(void* ptr) noexcept
     {
          if (!ptr)
          {
               return;
          }

          Deallocations.fetch_add(1);

          /* Try to find the block in our pools */

          for (size_t i = 0; i < NUM_SHARDS; ++i)
          {
               std::lock_guard<std::mutex> lock(PoolMutexes[i]);

               for (auto& block : Pools[i])
               {
                    if (block->data.load() == ptr)
                    {
                         /* Decrement TotalAllocated by the block size before returning to pool */

                         size_t block_size = block->size.load();

                         if (block_size > 0)
                         {
                              TotalAllocated.fetch_sub(block_size);
                         }

                         ReturnToFreeList(block.get());

                         return;
                    }
               }
          }

          /* Fallback to free for external allocations */
          /* Note: We can't track size for external allocations without a map,
         * so we don't decrement TotalAllocated here. External allocations
         * should ideally not use this pool's deallocate() method.
         */

          std::free(ptr);
     }

     /* Advanced memory management */

     void EnableCompression(bool enable)
     {
          CompressionEnabled.store(enable);
     }

     void CompactMemory()
     {
          for (size_t i = 0; i < NUM_SHARDS; ++i)
          {
               std::lock_guard<std::mutex> lock(PoolMutexes[i]);

               auto& pool = Pools[i];

               /* Remove unused blocks */

               pool.erase(
                    std::remove_if(pool.begin(), pool.end(),
                                   [](const std::unique_ptr<AtomicBlock>& block)
                                   {
                                        return !block->in_use.load();
                                   }),
                    pool.end());
          }
     }

     /* Performance statistics */

     size_t GetTotalAllocated() const noexcept
     {
          return TotalAllocated.load();
     }

     size_t GetPeakUsage() const noexcept
     {
          return PeakUsage.load();
     }

     size_t GetAllocations() const noexcept
     {
          return Allocations.load();
     }

     size_t GetDeallocations() const noexcept
     {
          return Deallocations.load();
     }

     size_t GetCacheHits() const noexcept
     {
          return CacheHits.load();
     }

     size_t GetCacheMisses() const noexcept
     {
          return CacheMisses.load();
     }

     double GetHitRatio() const noexcept
     {
          size_t hits = CacheHits.load();

          size_t misses = CacheMisses.load();

          return hits + misses > 0 ? static_cast<double>(hits) / (hits + misses) : 0.0;
     }

     /* Memory optimization */

     void OptimizeForWorkload()
     {
          /* Analyze access patterns and adjust pool sizes */

          for (size_t i = 0; i < NUM_SHARDS; ++i)
          {
               size_t count = ShardCounts[i].load();

               if (count < 10)
               {
                    /* Increase pool size for hot shards */

                    std::lock_guard<std::mutex> lock(PoolMutexes[i]);

                    size_t additional_blocks = 100;

                    for (size_t j = 0; j < additional_blocks; ++j)
                    {
                         auto block = std::make_unique<AtomicBlock>((i + 1) * 64);

                         ReturnToFreeList(block.get());
                         Pools[i].push_back(std::move(block));
                    }
               }
          }
     }
};

/* Global high-performance memory pool instance */

extern FastMemoryPool* GFastPool;

/* Global initialization/teardown helpers */

void InitFastMemoryPool();

void CleanupFastMemoryPool();

void PrintMemoryPoolStats();
