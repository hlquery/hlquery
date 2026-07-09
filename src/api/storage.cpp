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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pthread.h>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/storageengine.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides storage API handlers for database and persistence operations. */

/* HandleStorageStatus returns storage status and statistics. */

/*
 * SearchAPI::HandleStorageStatus implementation.
 */

HttpResponse SearchAPI::HandleStorageStatus(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Database)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Database not available\"}";

          return Response;
     }

     try
     {
          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{";

          /* Get document storage stats. */

          auto DocStats = HybridStorageManagerInstance().GetStats();

          std::string db_path;

          if (Instance && Instance->Database)
          {
               db_path = Instance->Database->GetDBPath();
          }

          if (db_path.empty())
          {
               db_path = std::string(HLQUERY_DATA_DIR) + "/storage";
          }

          /* Count SSTables on disk and calculate total size. */

          int SSTableCount = 0;
          size_t SSTableSizeBytes = 0;

          try
          {
               if (std::filesystem::exists(db_path))
               {
                    for (const auto &Entry : std::filesystem::recursive_directory_iterator(db_path))
                    {
                         if (Entry.is_regular_file() && Entry.path().extension() == ".sst")
                         {
                              SSTableCount++;
                              SSTableSizeBytes += Entry.file_size();
                         }
                    }
               }
          }
          catch (...)
          {
          }

          /* Get WAL file size and checkpoint info. */

          uint64_t WALSizeBytes = 0;
          bool CheckpointExists = false;
          uint64_t CheckpointLSN = 0;

          try
          {
               if (std::filesystem::exists(db_path))
               {
                    for (const auto &Entry : std::filesystem::recursive_directory_iterator(db_path))
                    {
                         if (Entry.is_regular_file() && Entry.path().extension() == ".log")
                         {
                              WALSizeBytes += Entry.file_size();
                         }
                    }
               }
          }
          catch (...)
          {
          }

          uint64_t memtable_size = 0;
          DBManager::Stats db_stats;

          if (Instance && Instance->Database)
          {
               db_stats = Instance->Database->GetRocksDBStats();
               memtable_size = db_stats.memtable_size;
               if (db_stats.num_sst_files > 0)
               {
                    SSTableCount = db_stats.num_sst_files;
               }
               if (db_stats.total_db_size > 0)
               {
                    SSTableSizeBytes = db_stats.total_db_size;
               }
          }

          /* Build response. */

          Response.Body += "\"last_durable_lsn\":" + std::to_string(CheckpointLSN) + ",";
          Response.Body += "\"checkpoint_exists\":" + std::string(CheckpointExists ? "true" : "false") + ",";
          Response.Body += "\"wal_size_bytes\":" + std::to_string(WALSizeBytes) + ",";
          Response.Body += "\"active_memtables\":1,";
          Response.Body += "\"memtable_size_bytes\":" + std::to_string(memtable_size) + ",";
          Response.Body += "\"sstable_count\":" + std::to_string(SSTableCount) + ",";
          Response.Body += "\"sstable_size_bytes\":" + std::to_string(SSTableSizeBytes) + ",";
          Response.Body += "\"segmented_storage_enabled\":" + std::string(db_stats.segmented_storage_enabled ? "true" : "false") + ",";
          if (!db_stats.active_segment_id.empty())
          {
               Response.Body += "\"active_segment_id\":\"" + EscapeJSONString(db_stats.active_segment_id) + "\",";
          }
          if (db_stats.segment_manifest_generation > 0)
          {
               Response.Body += "\"sealed_segment_count\":" + std::to_string(db_stats.sealed_segment_count) + ",";
               Response.Body += "\"tombstone_count_estimate\":" + std::to_string(db_stats.tombstone_count_estimate) + ",";
               Response.Body += "\"segment_manifest_generation\":" + std::to_string(db_stats.segment_manifest_generation) + ",";
               Response.Body += "\"segment_max_bytes\":" + std::to_string(db_stats.segment_max_bytes) + ",";
               Response.Body += "\"segment_total_bytes\":" + std::to_string(db_stats.segment_total_bytes) + ",";
               Response.Body += "\"segment_total_sst_files\":" + std::to_string(db_stats.segment_total_sst_files) + ",";
          }
          Response.Body += "\"total_collections\":" + std::to_string(DocStats.total_collections) + ",";
          Response.Body += "\"total_documents\":" + std::to_string(DocStats.total_documents);
          Response.Body += "}";

          return Response;
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Exception getting storage status: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Unknown exception getting storage status\"}";

          return Response;
     }
}
