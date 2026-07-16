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

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

class DefragmentationManager
{
   private:
     /* Enabled controls whether defragmentation runs. */

     std::atomic<bool> Enabled;

     /* TotalDefragmented tracks total keys defragmented. */

     std::atomic<size_t> TotalDefragmented;

     /* TotalBytesFreed tracks total bytes freed. */

     std::atomic<size_t> TotalBytesFreed;

     /* Mutex guards stats operations. */

     mutable std::mutex mutex;

   public:
     /* Constructor. */

     DefragmentationManager();

     /* Destructor. */

     ~DefragmentationManager();

     /* DefragmentDatabase defragments the database. */

     int DefragmentDatabase(int db_id);

     /* DefragmentKey defragments a single key. */

     bool DefragmentKey(int db_id, const std::string &key);

     /* IsEnabled reports whether defragmentation is enabled. */

     bool IsEnabled() const
     {
          return Enabled.load();
     }

     /* SetEnabled enables or disables defragmentation. */

     void SetEnabled(bool enabled)
     {
          Enabled.store(enabled);
     }

     /* GetTotalDefragmented returns the total defragmented count. */

     size_t GetTotalDefragmented() const
     {
          return TotalDefragmented.load();
     }

     /* GetTotalBytesFreed returns the total bytes freed. */

     size_t GetTotalBytesFreed() const
     {
          return TotalBytesFreed.load();
     }

     /* GetStats returns a summary of defragmentation stats. */

     std::unordered_map<std::string, std::string> GetStats() const;

     /* ShouldDefragment checks whether a key should be defragmented. */

     bool ShouldDefragment(const std::string &key, size_t value_size) const;
};
