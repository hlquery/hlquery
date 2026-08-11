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
#include <rocksdb/rate_limiter.h>
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
#include "search/hybrid_rank_fusion.h"
#include "search/document_collection_store.h"
#include "search/rocksdb_storage_engine.h"
#include "search/lexical_inverted_index.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides storage API handlers for database and persistence operations. */

namespace
{
struct MountDetails
{
     std::string Filesystem;
     std::string MountPoint;
     std::string MountOptions;
     std::string MountSource;
};

std::string CompressionName(rocksdb::CompressionType Type)
{
     switch (Type)
     {
          case rocksdb::kNoCompression: return "none";
          case rocksdb::kSnappyCompression: return "snappy";
          case rocksdb::kZlibCompression: return "zlib";
          case rocksdb::kBZip2Compression: return "bzip2";
          case rocksdb::kLZ4Compression: return "lz4";
          case rocksdb::kLZ4HCCompression: return "lz4hc";
          case rocksdb::kXpressCompression: return "xpress";
          case rocksdb::kZSTD: return "zstd";
          default: return "custom_or_unknown";
     }
}

std::string CompactionStyleName(rocksdb::CompactionStyle Style)
{
     switch (Style)
     {
          case rocksdb::kCompactionStyleLevel: return "level";
          case rocksdb::kCompactionStyleUniversal: return "universal";
          case rocksdb::kCompactionStyleFIFO: return "fifo";
          case rocksdb::kCompactionStyleNone: return "none";
          default: return "unknown";
     }
}

std::string DecodeMountInfoField(const std::string &Value)
{
     std::string Decoded;
     Decoded.reserve(Value.size());

     for (size_t Index = 0; Index < Value.size(); ++Index)
     {
          if (Value[Index] == '\\' && Index + 3 < Value.size() &&
              Value[Index + 1] >= '0' && Value[Index + 1] <= '7' &&
              Value[Index + 2] >= '0' && Value[Index + 2] <= '7' &&
              Value[Index + 3] >= '0' && Value[Index + 3] <= '7')
          {
               const char Character = static_cast<char>((Value[Index + 1] - '0') * 64 +
                                                        (Value[Index + 2] - '0') * 8 +
                                                        (Value[Index + 3] - '0'));
               Decoded.push_back(Character);
               Index += 3;
          }
          else
          {
               Decoded.push_back(Value[Index]);
          }
     }

     return Decoded;
}

bool PathIsWithinMount(const std::string &Path, const std::string &MountPoint)
{
     if (MountPoint == "/")
     {
          return !Path.empty() && Path.front() == '/';
     }

     return Path == MountPoint ||
            (Path.size() > MountPoint.size() && Path.compare(0, MountPoint.size(), MountPoint) == 0 &&
             Path[MountPoint.size()] == '/');
}

MountDetails GetMountDetails(const std::string &DatabasePath)
{
     MountDetails Best;
     std::ifstream MountInfo("/proc/self/mountinfo");
     if (!MountInfo)
     {
          return Best;
     }

     std::error_code Error;
     std::string ResolvedPath = std::filesystem::weakly_canonical(DatabasePath, Error).string();
     if (Error || ResolvedPath.empty())
     {
          ResolvedPath = std::filesystem::absolute(DatabasePath, Error).lexically_normal().string();
     }

     std::string Line;
     while (std::getline(MountInfo, Line))
     {
          std::istringstream Stream(Line);
          std::vector<std::string> Fields;
          std::string Field;
          while (Stream >> Field)
          {
               Fields.push_back(Field);
          }

          const auto Separator = std::find(Fields.begin(), Fields.end(), "-");
          if (Fields.size() < 10 || Separator == Fields.end() ||
              std::distance(Separator, Fields.end()) < 4)
          {
               continue;
          }

          const size_t SeparatorIndex = static_cast<size_t>(std::distance(Fields.begin(), Separator));
          const std::string MountPoint = DecodeMountInfoField(Fields[4]);
          if (!PathIsWithinMount(ResolvedPath, MountPoint) ||
              MountPoint.size() <= Best.MountPoint.size())
          {
               continue;
          }

          Best.MountPoint = MountPoint;
          Best.MountOptions = Fields[5];
          Best.Filesystem = Fields[SeparatorIndex + 1];
          Best.MountSource = DecodeMountInfoField(Fields[SeparatorIndex + 2]);
          const std::string &SuperOptions = Fields[SeparatorIndex + 3];
          if (!SuperOptions.empty() && SuperOptions != Best.MountOptions)
          {
               Best.MountOptions += "," + SuperOptions;
          }
     }

     return Best;
}
} // namespace

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

HttpResponse SearchAPI::HandleStorageDiagnostics(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Database)
     {
          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          Response.Body = "{\"error\":\"Database not available\"}";
          return Response;
     }

     const rocksdb::WriteOptions WriteOptions = Instance->Database->GetEffectiveWriteOptions();
     const rocksdb::Options DBOptions = Instance->Database->GetEffectiveOptions();
     const DBManager::Stats DBStats = Instance->Database->GetRocksDBStats();
     const std::string DatabasePath = Instance->Database->GetDBPath();
     const MountDetails Mount = GetMountDetails(DatabasePath);

     nlohmann::json Body;
     Body["schema_version"] = 1;
     Body["database_path"] = DatabasePath;
     Body["wal_directory"] = DBOptions.wal_dir.empty() ? DatabasePath : DBOptions.wal_dir;
     Body["write_options"] = {
          {"sync", WriteOptions.sync},
          {"disable_wal", WriteOptions.disableWAL},
          {"no_slowdown", WriteOptions.no_slowdown}
     };
     Body["db_options"] = {
          {"manual_wal_flush", DBOptions.manual_wal_flush},
          {"use_fsync", DBOptions.use_fsync},
          {"atomic_flush", DBOptions.atomic_flush},
          {"bytes_per_sync", DBOptions.bytes_per_sync},
          {"wal_bytes_per_sync", DBOptions.wal_bytes_per_sync},
          {"max_total_wal_size", DBOptions.max_total_wal_size},
          {"enable_pipelined_write", DBOptions.enable_pipelined_write},
          {"unordered_write", DBOptions.unordered_write},
          {"two_write_queues", DBOptions.two_write_queues},
          {"allow_concurrent_memtable_write", DBOptions.allow_concurrent_memtable_write},
          {"use_direct_reads", DBOptions.use_direct_reads},
          {"use_direct_io_for_flush_and_compaction", DBOptions.use_direct_io_for_flush_and_compaction},
          {"write_buffer_size", DBOptions.write_buffer_size},
          {"max_write_buffer_number", DBOptions.max_write_buffer_number},
          {"max_background_jobs", DBOptions.max_background_jobs}
     };
     Body["db_options"]["compression"] = CompressionName(DBOptions.compression);
     Body["db_options"]["bottommost_compression"] = CompressionName(DBOptions.bottommost_compression);
     Body["db_options"]["wal_compression"] = CompressionName(DBOptions.wal_compression);
     Body["db_options"]["compaction_style"] = CompactionStyleName(DBOptions.compaction_style);
     Body["db_options"]["rate_limiter_enabled"] = DBOptions.rate_limiter != nullptr;
     Body["db_options"]["rate_limiter_bytes_per_second"] = DBOptions.rate_limiter
                                                                  ? nlohmann::json(DBOptions.rate_limiter->GetBytesPerSecond())
                                                                  : nlohmann::json(nullptr);
     Body["wal_sync_mode"] = Instance->Database->GetWALSyncMode();
     Body["wal_enabled"] = !WriteOptions.disableWAL;
     Body["search_indexing"] = "lazy";
     Body["segmented_storage_enabled"] = DBStats.segmented_storage_enabled;
     Body["database_instances"] = DBStats.segmented_storage_enabled
                                             ? 2 + DBStats.sealed_segment_count
                                             : 1;
     Body["column_families_per_database"] = 1;
     Body["server_ingest_queue"] = "none";
     Body["durability_barrier"] = "DBManager::ExecuteDurabilityBarrier(SyncWAL)";
     Body["metrics"] = {
          {"block_cache_usage", DBStats.block_cache_usage},
          {"memtable_bytes", DBStats.memtable_size},
          {"sst_bytes", DBStats.total_db_size},
          {"pending_compaction_bytes", DBStats.pending_compaction_bytes},
          {"running_flushes", DBStats.running_flushes},
          {"running_compactions", DBStats.running_compactions},
          {"write_stall_micros", DBStats.write_stall_micros},
          {"bytes_written", DBStats.rocksdb_bytes_written}
     };
     Body["filesystem"] = Mount.Filesystem.empty() ? nlohmann::json(nullptr) : nlohmann::json(Mount.Filesystem);
     Body["mount_point"] = Mount.MountPoint.empty() ? nlohmann::json(nullptr) : nlohmann::json(Mount.MountPoint);
     Body["mount_options"] = Mount.MountOptions.empty() ? nlohmann::json(nullptr) : nlohmann::json(Mount.MountOptions);
     Body["mount_source"] = Mount.MountSource.empty() ? nlohmann::json(nullptr) : nlohmann::json(Mount.MountSource);
     Body["storage_is_volatile"] = Mount.Filesystem == "tmpfs" || Mount.Filesystem == "ramfs";
     Body["critical_unknowns"] = nlohmann::json::array();
     if (Mount.Filesystem.empty())
     {
          Body["critical_unknowns"].push_back("filesystem");
     }
     if (Mount.MountPoint.empty())
     {
          Body["critical_unknowns"].push_back("mount_point");
     }
     if (Mount.MountOptions.empty())
     {
          Body["critical_unknowns"].push_back("mount_options");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Body.dump();
     return Response;
}

HttpResponse SearchAPI::HandleDurabilityBarrier(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (!Instance || !Instance->Database)
     {
          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          Response.Body = "{\"success\":false,\"error\":\"Database not available\"}";
          return Response;
     }

     bool SyncWAL = true;
     bool FlushMemtables = false;
     bool WaitForCompaction = false;

     try
     {
          if (!Request.Body.empty())
          {
               const nlohmann::json Body = nlohmann::json::parse(Request.Body);
               SyncWAL = Body.value("sync_wal", true);
               FlushMemtables = Body.value("flush_memtables", false);
               WaitForCompaction = Body.value("wait_for_compaction", false);
          }
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          Response.Body = "{\"success\":false,\"error\":\"Invalid durability barrier request\",\"details\":\"" + EscapeJSONString(E.what()) + "\"}";
          return Response;
     }

     if ((!SyncWAL && !FlushMemtables) || WaitForCompaction)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          Response.Body = WaitForCompaction
                              ? "{\"success\":false,\"error\":\"Compaction waiting is not implemented\"}"
                              : "{\"success\":false,\"error\":\"At least one durability operation must be requested\"}";
          return Response;
     }

     const rocksdb::WriteOptions WriteOptions = Instance->Database->GetEffectiveWriteOptions();
     if (SyncWAL && WriteOptions.disableWAL)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          Response.Body = "{\"success\":false,\"error\":\"Cannot synchronize WAL because WAL is disabled\"}";
          return Response;
     }

     double FlushMS = 0.0;
     double SyncMS = 0.0;
     const auto Barrier = Instance->Database->ExecuteDurabilityBarrier(SyncWAL, FlushMemtables);

     nlohmann::json Instances = nlohmann::json::array();
     std::string FirstFailure;
     for (const auto &Result : Barrier.Instances)
     {
          if (Result.Operation.find("Flush") != std::string::npos)
          {
               FlushMS += Result.ElapsedMS;
          }
          if (Result.Operation.find("SyncWAL") != std::string::npos)
          {
               SyncMS += Result.ElapsedMS;
          }
          Instances.push_back({
               {"name", Result.Name},
               {"operation", Result.Operation},
               {"status", Result.Status},
               {"elapsed_ms", Result.ElapsedMS},
               {"success", Result.Success}
          });
          if (!Result.Success && FirstFailure.empty())
          {
               FirstFailure = Result.Name + ": " + Result.Status;
          }
     }

     const bool Success = Barrier.Success;
     const uint64_t Sequence = Barrier.Sequence;
     const double TotalMS = Barrier.TotalMS;
     const auto NowMS = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

     nlohmann::json Body;
     Body["success"] = Success;
     Body["barrier_id"] = std::to_string(NowMS) + "_" + std::to_string(Sequence);
     Body["sequence"] = Sequence;
     Body["queued_writes_wait_ms"] = Barrier.QueuedWritesWaitMS;
     Body["rocksdb_wal_sync_ms"] = SyncWAL ? nlohmann::json(SyncMS) : nlohmann::json(nullptr);
     Body["memtable_flush_ms"] = FlushMemtables ? nlohmann::json(FlushMS) : nlohmann::json(nullptr);
     Body["compaction_wait_ms"] = nullptr;
     Body["total_ms"] = TotalMS;
     Body["rocksdb_status"] = Success ? "OK" : FirstFailure;
     Body["rocksdb_error_code"] = Success ? "" :
          (Instance->Database->GetLastSyncErrorCode().empty() ? "rocksdb_instance_sync_failed" : Instance->Database->GetLastSyncErrorCode());
     Body["instances"] = std::move(Instances);
     Body["write_boundary"] = "exclusive: all mutations begun before sequence capture completed before synchronization";
     Body["collections_synchronized"] = HybridStorageManagerInstance().ListCollections().size();

     HttpResponse Response(Success ? Status::OK : Status::INTERNAL_SERVER_ERROR,
                           StatusText(Success ? Status::OK : Status::INTERNAL_SERVER_ERROR),
                           "application/json");
     Response.Body = Body.dump();
     return Response;
}
