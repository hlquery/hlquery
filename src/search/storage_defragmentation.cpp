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

#include <unordered_map>
#include "search/storage_defragmentation.h"

/*
 * DefragmentationManager::DefragmentationManager - Initializes counters for the placeholder defragmentation API.
 */

DefragmentationManager::DefragmentationManager()
    : Enabled(true),
      TotalDefragmented(0), TotalBytesFreed(0)
{
}

/*
 * DefragmentationManager::~DefragmentationManager - Leaves cleanup to owned members and atomics.
 */

DefragmentationManager::~DefragmentationManager()
{
}

/*
 * DefragmentationManager::DefragmentDatabase - Reports success because RocksDB manages on-disk layout internally.
 */

int DefragmentationManager::DefragmentDatabase(int /*db_id*/)
{
     /* Keep the legacy interface callable even though RocksDB owns compaction and file layout decisions. */

     return 0;
}

/*
 * DefragmentationManager::DefragmentKey - Declines per-key defragmentation because the backend does not expose it.
 */

bool DefragmentationManager::DefragmentKey(int /*db_id*/, const std::string & /*key*/)
{
     /* Preserve the API surface without pretending that a single key can be physically compacted on demand. */

     return false;
}

/*
 * DefragmentationManager::GetStats - Exposes counters in string form for admin and diagnostic endpoints.
 */

std::unordered_map<std::string, std::string> DefragmentationManager::GetStats() const
{
     /* Return string values so the result can be forwarded directly to generic reporting code. */

     std::unordered_map<std::string, std::string> stats;

     stats["enabled"] = Enabled.load() ? "true" : "false";
     stats["total_defragmented"] = std::to_string(TotalDefragmented.load());
     stats["total_bytes_freed"] = std::to_string(TotalBytesFreed.load());
     return stats;
}

/*
 * DefragmentationManager::ShouldDefragment - Always returns false because RocksDB decides when compaction is needed.
 */

bool DefragmentationManager::ShouldDefragment(const std::string & /*key*/, size_t /*value_size*/) const
{
     /* Signal that manual defragmentation should not be scheduled on top of RocksDB's own maintenance. */

     return false;
}
