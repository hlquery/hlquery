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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <sstream>

#include "core/hlquery.h"
#include "runtime/serverconfig.h"
#include "search/segmented_document_router.h"
#include "search/rocksdb_storage_engine.h"
#include "search/wal_entry_guard.h"
#include "utils/consolewriter.h"
#include "utils/wildcard.h"

namespace
{
/* Serializes durable database syncs and exposes their lifecycle to readiness checks. */

class DatabaseSyncGuard
{
   private:
     std::unique_lock<std::mutex> SyncLock;

   public:
     DatabaseSyncGuard()
     {
          if (Instance)
          {
               SyncLock = std::unique_lock<std::mutex>(Instance->GetSyncMutex());
               Instance->SetSyncInProgress(true);
          }
     }

     ~DatabaseSyncGuard()
     {
          if (Instance)
          {
               Instance->SetSyncInProgress(false);
          }
     }

     DatabaseSyncGuard(const DatabaseSyncGuard &) = delete;
     DatabaseSyncGuard &operator=(const DatabaseSyncGuard &) = delete;
};

bool LooksLikeRocksDBDirectory(const std::string &Path)
{
     try
     {
          if (!std::filesystem::exists(Path) || !std::filesystem::is_directory(Path))
          {
               return false;
          }

          for (const auto &Entry : std::filesystem::directory_iterator(Path))
          {
               const std::string Name = Entry.path().filename().string();

               if (Name == "CURRENT" || Name.starts_with("MANIFEST-") || Name.starts_with("OPTIONS-"))
               {
                    return true;
               }
          }
     }
     catch (...)
     {
     }

     return false;
}
}

/*
 * DBManager::DBManager - Builds a RocksDB configuration with safe defaults before config overrides load.
 */

DBManager::DBManager() : CurrentDB(0), WALSyncMode("normal")
{
     /* Initialize with default path */

     DBPath = std::string(HLQUERY_DATA_DIR) + "/storage";

     /* Configure RocksDB options with safe defaults from config.h */

     OptionsValue.create_if_missing = ROCKSDB_DEFAULT_CREATE_IF_MISSING != 0;
     OptionsValue.create_missing_column_families = ROCKSDB_DEFAULT_CREATE_MISSING_COLUMN_FAMILIES != 0;
     OptionsValue.error_if_exists = ROCKSDB_DEFAULT_ERROR_IF_EXISTS != 0;

     /* Do not call IncreaseParallelism() here without a cap.
      * The no-arg form sizes RocksDB background workers from CPU count,
      * which can spawn far more threads than hlquery's configured budget.
      * We apply a capped parallelism value later, after config has loaded.
      */

     OptionsValue.OptimizeLevelStyleCompaction();
     OptionsValue.max_background_jobs = ROCKSDB_DEFAULT_MAX_BACKGROUND_JOBS;
     OptionsValue.write_buffer_size = ROCKSDB_DEFAULT_WRITE_BUFFER_SIZE;
     OptionsValue.max_write_buffer_number = ROCKSDB_DEFAULT_MAX_WRITE_BUFFER_NUMBER;
     OptionsValue.target_file_size_base = ROCKSDB_DEFAULT_TARGET_FILE_SIZE_BASE;
     OptionsValue.max_bytes_for_level_base = ROCKSDB_DEFAULT_MAX_BYTES_FOR_LEVEL_BASE;

     /* Compression - use no compression by default to avoid library dependency issues */

     OptionsValue.compression = rocksdb::kNoCompression;
     OptionsValue.bottommost_compression = rocksdb::kNoCompression;

     /* Bloom filter and block cache */

     rocksdb::BlockBasedTableOptions table_options;

     table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
     /* 256MB cache */

     table_options.block_cache = rocksdb::NewLRUCache(256 * 1024 * 1024);

     OptionsValue.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

     /* Default to "normal" sync mode for durability */

     WALSyncMode = "normal";
}

/*
 * DBManager::~DBManager - Ensures the database handle is closed during teardown.
 */

DBManager::~DBManager()
{
     Shutdown();
}

/*
 * DBManager::Initialize - Resolves paths, applies configured RocksDB options, and opens the database.
 */

bool DBManager::Initialize()
{
     if (!Instance || !Instance->Logs)
     {
          return false;
     }

     Instance->Logs->Debug("rocksdb", "DBManager::Initialize() starting.");

     /* Get data directory from config, fallback to default */

     std::string base_DataDirValue = std::string(HLQUERY_DATA_DIR);
     RocksDBOptions effective_rocksdb_opts = RocksDBOptions::Default();
     bool segmented_requested = false;
     const char *EnvDataDir = std::getenv("HLQUERY_DATA_DIR");
     if (EnvDataDir && *EnvDataDir)
     {
          base_DataDirValue = EnvDataDir;
     }

     try
     {
          if (Instance && Instance->Config)
          {
               if (Instance->Config->IsValid())
               {
                    const auto &rocksdb_opts = Instance->Config->GetRocksDBOptions();
                    effective_rocksdb_opts = rocksdb_opts;
                    segmented_requested = rocksdb_opts.SegmentedStorageEnabled;

                    Instance->Logs->Debug("rocksdb", "RocksDB options loaded - DataDir='" + rocksdb_opts.DataDir + "'.");

                    if (!rocksdb_opts.DataDir.empty())
                    {
                         base_DataDirValue = rocksdb_opts.DataDir;

                         Instance->Logs->Normal("rocksdb", "Using DataDir from config: " + base_DataDirValue + ".");
                    }
                    else
                    {
                         Instance->Logs->Normal("rocksdb", "DataDir is empty in config, using default: " + base_DataDirValue + ".");
                    }
               }
               else
               {
                    Instance->Logs->Normal("rocksdb", "Config is not valid, using default DataDir: " + base_DataDirValue + ".");
               }
          }
          else
          {
               Instance->Logs->Normal("rocksdb", "Instance or Config is null, using default DataDir: " + base_DataDirValue + ".");
          }
     }
     catch (const std::exception &e)
     {
          Instance->Logs->Normal("rocksdb", "Failed to read DataDir from config, using default: " + std::string(e.what()) + ".");
     }
     catch (...)
     {
          Instance->Logs->Normal("rocksdb", "Failed to read DataDir from config, using default.");
     }

     DBPath = base_DataDirValue + "/storage";
     SegmentedStorageEnabled = false;

     try
     {
          const std::string legacy_db_path = base_DataDirValue + "/rocksdb";
          const std::string storage_path = base_DataDirValue + "/storage";

          if (segmented_requested && LooksLikeRocksDBDirectory(storage_path))
          {
               DBPath = storage_path;
               SegmentedStorageEnabled = false;
               Instance->Logs->Normal("rocksdb", "storage.segmented requested, but existing direct RocksDB layout was detected at " + storage_path + "; opening old layout without migration.");
          }
          else if (segmented_requested)
          {
               DBPath = storage_path + "/system/rocksdb";
               SegmentedStorageEnabled = true;
               Instance->Logs->Normal("rocksdb", "Segmented storage enabled; system RocksDB path: " + DBPath + ".");
          }
          else if (!std::filesystem::exists(DBPath) && std::filesystem::exists(legacy_db_path))
          {
               DBPath = legacy_db_path;

               Instance->Logs->Normal("rocksdb", "Using legacy RocksDB path: " + DBPath + ".");
          }
     }
     catch (...)
     {
     }

     Instance->Logs->Normal("rocksdb", "RocksDB DBPath set to: " + DBPath + ".");

     /* Update RocksDB options from config */

     try
     {
          if (Instance && Instance->Config)
          {
               if (Instance->Config->IsValid())
               {
                    const auto &rocksdb_opts = Instance->Config->GetRocksDBOptions();
                    effective_rocksdb_opts = rocksdb_opts;

                    /* Write buffer settings */

                    OptionsValue.write_buffer_size = rocksdb_opts.WriteBufferSize;
                    OptionsValue.max_write_buffer_number = rocksdb_opts.MaxWriteBufferNumber;
                    OptionsValue.min_write_buffer_number_to_merge = rocksdb_opts.MinWriteBufferNumberToMerge;

                    /* Background jobs */

                    OptionsValue.max_background_jobs = rocksdb_opts.MaxBackgroundJobs;
                    OptionsValue.max_background_flushes = rocksdb_opts.MaxBackgroundFlushes;
                    OptionsValue.max_background_compactions = rocksdb_opts.MaxBackgroundCompactions;
                    OptionsValue.IncreaseParallelism(std::max(1, rocksdb_opts.MaxBackgroundJobs));

                    /* Level and compaction settings */

                    OptionsValue.target_file_size_base = rocksdb_opts.TargetFileSizeBase;
                    OptionsValue.max_bytes_for_level_base = rocksdb_opts.MaxBytesForLevelBase;
                    OptionsValue.max_bytes_for_level_multiplier = rocksdb_opts.MaxBytesForLevelMultiplier;
                    OptionsValue.level0_slowdown_writes_trigger = rocksdb_opts.Level0SlowdownWritesTrigger;
                    OptionsValue.level0_stop_writes_trigger = rocksdb_opts.Level0StopWritesTrigger;
                    OptionsValue.disable_auto_compactions = !rocksdb_opts.EnableCompaction;

                    /* Compression */

                    if (rocksdb_opts.Compression == "none")
                    {
                         OptionsValue.compression = rocksdb::kNoCompression;
                    }
                    else if (rocksdb_opts.Compression == "snappy")
                    {
                         OptionsValue.compression = rocksdb::kSnappyCompression;
                    }
                    else if (rocksdb_opts.Compression == "zlib")
                    {
                         OptionsValue.compression = rocksdb::kZlibCompression;
                    }
                    else if (rocksdb_opts.Compression == "lz4")
                    {
                         OptionsValue.compression = rocksdb::kLZ4Compression;
                    }
                    else if (rocksdb_opts.Compression == "zstd")
                    {
                         OptionsValue.compression = rocksdb::kZSTD;
                    }

                    if (rocksdb_opts.BottommostCompression == "none")
                    {
                         OptionsValue.bottommost_compression = rocksdb::kNoCompression;
                    }
                    else if (rocksdb_opts.BottommostCompression == "snappy")
                    {
                         OptionsValue.bottommost_compression = rocksdb::kSnappyCompression;
                    }
                    else if (rocksdb_opts.BottommostCompression == "zlib")
                    {
                         OptionsValue.bottommost_compression = rocksdb::kZlibCompression;
                    }
                    else if (rocksdb_opts.BottommostCompression == "lz4")
                    {
                         OptionsValue.bottommost_compression = rocksdb::kLZ4Compression;
                    }
                    else if (rocksdb_opts.BottommostCompression == "zstd")
                    {
                         OptionsValue.bottommost_compression = rocksdb::kZSTD;
                    }

                    /* WAL settings */

                    OptionsValue.wal_bytes_per_sync = rocksdb_opts.WALBytesPerSync;
                    OptionsValue.max_total_wal_size = rocksdb_opts.MaxTotalWALSize;
                    WALSyncMode = rocksdb_opts.WALSyncMode;

                    /* Performance options */

                    OptionsValue.use_direct_reads = rocksdb_opts.UseDirectReads;
                    OptionsValue.use_direct_io_for_flush_and_compaction = rocksdb_opts.UseDirectIOForFlushAndCompaction;
                    OptionsValue.max_open_files = rocksdb_opts.MaxOpenFiles;

                    /* Compaction style */

                    if (rocksdb_opts.CompactionStyle == "level")
                    {
                         OptionsValue.compaction_style = rocksdb::kCompactionStyleLevel;
                    }
                    else if (rocksdb_opts.CompactionStyle == "universal")
                    {
                         OptionsValue.compaction_style = rocksdb::kCompactionStyleUniversal;
                    }
                    else if (rocksdb_opts.CompactionStyle == "fifo")
                    {
                         OptionsValue.compaction_style = rocksdb::kCompactionStyleFIFO;
                    }

                    /* Memory mapping options */

                    OptionsValue.allow_mmap_reads = rocksdb_opts.AllowMmapReads;
                    OptionsValue.allow_mmap_writes = rocksdb_opts.AllowMmapWrites;
                    OptionsValue.advise_random_on_open = rocksdb_opts.AdviseRandomOnOpen;

                    /* Safety and verification */

                    OptionsValue.paranoid_checks = rocksdb_opts.ParanoidChecks;

                    /* Statistics */

                    if (rocksdb_opts.EnableStatistics)
                    {
                         OptionsValue.statistics = rocksdb::CreateDBStatistics();
                         OptionsValue.stats_dump_period_sec = rocksdb_opts.StatsDumpPeriodSec;
                    }

                    /* Advanced write options */

                    OptionsValue.enable_pipelined_write = rocksdb_opts.EnablePipelinedWrite;
                    OptionsValue.unordered_write = rocksdb_opts.UnorderedWrite;

                    /* File management */

                    OptionsValue.delete_obsolete_files_period_micros = rocksdb_opts.DeleteObsoleteFilesPeriodMicros;

                    /* Bloom filter and block cache */

                    rocksdb::BlockBasedTableOptions table_options;

                    if (rocksdb_opts.EnableBloomFilter)
                    {
                         if (rocksdb_opts.FilterPolicy == "ribbon")
                         {
                              table_options.filter_policy.reset(rocksdb::NewRibbonFilterPolicy(rocksdb_opts.BloomFilterBitsPerKey, rocksdb_opts.RibbonBloomBeforeLevel));
                         }
                         else
                         {
                              table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(rocksdb_opts.BloomFilterBitsPerKey, false));
                         }
                    }

                    table_options.block_cache = rocksdb::NewLRUCache(rocksdb_opts.BlockCacheSize);
                    table_options.block_size = rocksdb_opts.BlockSize;
                    table_options.block_restart_interval = rocksdb_opts.BlockRestartInterval;
                    table_options.cache_index_and_filter_blocks = true;
                    table_options.cache_index_and_filter_blocks_with_high_priority = true;
                    table_options.pin_l0_filter_and_index_blocks_in_cache = false;

                    OptionsValue.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
               }
          }
     }
     catch (const std::exception &e)
     {
          Instance->Logs->Normal("rocksdb", "Failed to read RocksDB options from config, using defaults: " + std::string(e.what()) + ".");
     }
     catch (...)
     {
          Instance->Logs->Normal("rocksdb", "Failed to read RocksDB options from config, using defaults.");
     }

     /* Create directory if it doesn't exist */

     try
     {
          std::filesystem::create_directories(DBPath);
     }
     catch (const std::exception &e)
     {
          Instance->Logs->Critical("rocksdb", "Failed to create RocksDB directory: " + std::string(e.what()) + ".");
          ConsoleWriter::WriteError("[FATAL] Failed to create RocksDB directory: " + std::string(e.what()) + ".", true);
          return false;
     }

     /* Open database */

     std::unique_ptr<rocksdb::DB> db_ptr;

     rocksdb::Status status = rocksdb::DB::Open(OptionsValue, DBPath, &db_ptr);

     /* If opening failed due to missing compression library, retry with no compression */

     if (!status.ok() && status.ToString().find("Compression type") != std::string::npos && status.ToString().find("is not linked") != std::string::npos)
     {
          Instance->Logs->Normal("rocksdb", "Compression library not available (" + status.ToString() + "), retrying with no compression.");

          auto original_compression = OptionsValue.compression;
          auto original_bottommost = OptionsValue.bottommost_compression;

          OptionsValue.compression = rocksdb::kNoCompression;
          OptionsValue.bottommost_compression = rocksdb::kNoCompression;

          db_ptr.reset();
          status = rocksdb::DB::Open(OptionsValue, DBPath, &db_ptr);

          if (!status.ok())
          {
               OptionsValue.compression = original_compression;
               OptionsValue.bottommost_compression = original_bottommost;

               Instance->Logs->Critical("rocksdb", "Failed to open RocksDB even with no compression: " + status.ToString() + ".");
               ConsoleWriter::WriteError("[FATAL] Failed to open RocksDB (even with no compression): " + status.ToString() + ".", true);
               return false;
          }

          Instance->Logs->Normal("rocksdb", "RocksDB opened successfully with no compression (compression library not available).");
     }

     if (status.ok() && db_ptr)
     {
          std::lock_guard<std::mutex> lock(DBValueMutex);
          DBValue = std::move(db_ptr);
     }
     else if (!status.ok())
     {
          Instance->Logs->Critical("rocksdb", "Failed to open RocksDB: " + status.ToString() + ".");
          ConsoleWriter::WriteError("[FATAL] Failed to open RocksDB: " + status.ToString() + ".", true);

          if (status.IsIOError() && status.ToString().find("lock") != std::string::npos)
          {
               ConsoleWriter::WriteError("[HINT] Another hlquery process might be running or holding the database lock.", true);
          }

          return false;
     }

     if (SegmentedStorageEnabled)
     {
          SegmentManagerValue = std::make_unique<SegmentManager>(base_DataDirValue, DBValue.get(), OptionsValue, GetWriteOptions(), effective_rocksdb_opts);

          if (!SegmentManagerValue->Initialize())
          {
               Instance->Logs->Critical("rocksdb", "Failed to initialize segmented storage.");
               SegmentManagerValue.reset();
               return false;
          }
     }

     Instance->Logs->Normal("rocksdb", "RocksDB initialized successfully at " + DBPath + ".");

     return true;
}

/*
 * DBManager::Shutdown - Releases the RocksDB instance and any cached resources owned by it.
 */

void DBManager::Shutdown()
{
     std::lock_guard<std::mutex> lock(DBValueMutex);

     if (SegmentManagerValue)
     {
          SegmentManagerValue->Shutdown();
          SegmentManagerValue.reset();
     }

     if (DBValue)
     {
          DBValue.reset();
     }
}

/*
 * DBManager::IsOpen - Reports whether the RocksDB handle was successfully created.
 */

bool DBManager::IsOpen() const
{
     std::lock_guard<std::mutex> lock(DBValueMutex);

     return DBValue != nullptr;
}

/*
 * DBManager::StartBackgroundThreads - Reserved hook for future engine-managed workers.
 */

void DBManager::StartBackgroundThreads()
{
}

/*
 * DBManager::RecordWriteValidationFailure - Captures the last rejected WAL-sized write for diagnostics.
 */

void DBManager::RecordWriteValidationFailure(const char *operation, const WALEntryValidationResult &validation, bool is_recovery)
{
     RejectedWALEntriesTotal.fetch_add(1, std::memory_order_relaxed);

     if (is_recovery)
     {
          RejectedWALEntriesRecovery.fetch_add(1, std::memory_order_relaxed);
     }
     else
     {
          RejectedWALEntriesNormal.fetch_add(1, std::memory_order_relaxed);
     }

     /* Store a structured code and a human-readable message so status endpoints can expose both forms. */

     const std::string code = WALEntryValidationErrorCode(validation.Error);
     const std::string message = WALEntryValidationMessage(validation);

     {
          std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
          LastWriteErrorCode = code;
          LastWriteErrorMessage = std::string(operation) + ": " + message;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("rocksdb", std::string(operation) + " rejected oversized WAL entry: " + message + ".");
     }
}

/*
 * DBManager::ClearLastWriteError - Clears the sticky write error after a new operation is allowed to proceed.
 */

void DBManager::ClearLastWriteError()
{
     std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
     LastWriteErrorCode.clear();
     LastWriteErrorMessage.clear();
}

/*
 * DBManager::RecordSyncFailure - Captures the last failed flush or WAL sync for diagnostics.
 */

void DBManager::RecordSyncFailure(const char *operation, const std::string &code, const rocksdb::Status &status)
{
     const std::string message = std::string(operation) + ": " + status.ToString();

     {
          std::lock_guard<std::mutex> lock(LastSyncErrorMutex);
          LastSyncErrorCode = code;
          LastSyncErrorMessage = message;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("rocksdb", message + ".");
     }
}

/*
 * DBManager::ClearLastSyncError - Clears the sticky sync error after a successful flush or sync operation.
 */

void DBManager::ClearLastSyncError()
{
     std::lock_guard<std::mutex> lock(LastSyncErrorMutex);
     LastSyncErrorCode.clear();
     LastSyncErrorMessage.clear();
}

bool DBManager::IsDocumentKey(const std::string &key) const
{
     return key.starts_with("doc:");
}

/*
 * DBManager::Set - Validates write size limits and then writes a single key-value pair.
 */

bool DBManager::Set(const std::string &key, const std::string &value)
{
     if (!DBValue)
     {
          return false;
     }

     const auto validation = ValidateWALEntrySize(key.size(), value.size(), false);

     if (!validation.Valid)
     {
          RecordWriteValidationFailure("Set", validation, false);
          return false;
     }

     ClearLastWriteError();

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(key))
     {
          return SegmentManagerValue->SetDocument(key, value);
     }

     rocksdb::Status status = DBValue->Put(GetWriteOptions(), key, value);

     if (!status.ok())
     {
          const std::string message = "Set: " + status.ToString();
          {
               std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
               LastWriteErrorCode = "rocksdb_write_failed";
               LastWriteErrorMessage = message;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("rocksdb", message + ".");
          }
     }

     return status.ok();
}

/*
 * DBManager::SetIfNotExists - Performs a read-then-put insert only when the key is currently absent.
 */

bool DBManager::SetIfNotExists(const std::string &key, const std::string &value)
{
     if (!DBValue)
     {
          return false;
     }

     const auto validation = ValidateWALEntrySize(key.size(), value.size(), false);

     if (!validation.Valid)
     {
          RecordWriteValidationFailure("SetIfNotExists", validation, false);
          return false;
     }

     ClearLastWriteError();

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(key))
     {
          if (SegmentManagerValue->ExistsDocument(key))
          {
               return false;
          }

          return SegmentManagerValue->SetDocument(key, value);
     }

     std::string existing_value;

     rocksdb::Status status = DBValue->Get(rocksdb::ReadOptions(), key, &existing_value);

     if (status.IsNotFound())
     {
          status = DBValue->Put(GetWriteOptions(), key, value);

          return status.ok();
     }

     return false;
}

/* Batch set multiple key-value pairs */

size_t DBManager::BatchSet(const std::vector<std::pair<std::string, std::string>> &key_values)
{
     if (!DBValue || key_values.empty())
     {
          return 0;
     }

     rocksdb::WriteBatch batch;
     size_t cumulative_batch_size = 0;

     for (const auto &kv : key_values)
     {
          const auto validation = ValidateWALEntrySize(kv.first.size(), kv.second.size(), false);

          if (!validation.Valid)
          {
               RecordWriteValidationFailure("BatchSet.entry", validation, false);
               return 0;
          }

          if (cumulative_batch_size > (std::numeric_limits<size_t>::max() - validation.TotalLength))
          {
               WALEntryValidationResult overflow_validation;
               overflow_validation.Valid = false;
               overflow_validation.Error = WALEntryValidationError::SizeOverflow;
               overflow_validation.KeyLength = 0;
               overflow_validation.ValueLength = validation.TotalLength;
               overflow_validation.TotalLength = std::numeric_limits<size_t>::max();
               overflow_validation.MaxAllowedLength = MAX_WAL_ENTRY_SIZE_FOR_MODE(false);
               overflow_validation.IsRecoveryMode = false;
               RecordWriteValidationFailure("BatchSet.cumulative", overflow_validation, false);
               return 0;
          }

          cumulative_batch_size += validation.TotalLength;

          batch.Put(kv.first, kv.second);
     }

     const auto batch_validation = ValidateWALEntrySize(0, cumulative_batch_size, false);

     if (!batch_validation.Valid)
     {
          RecordWriteValidationFailure("BatchSet.cumulative", batch_validation, false);
          return 0;
     }

     ClearLastWriteError();

     if (SegmentedStorageEnabled && SegmentManagerValue)
     {
          std::vector<std::pair<std::string, std::string>> doc_key_values;
          std::vector<std::pair<std::string, std::string>> system_key_values;

          for (const auto &kv : key_values)
          {
               if (IsDocumentKey(kv.first))
               {
                    doc_key_values.push_back(kv);
               }
               else
               {
                    system_key_values.push_back(kv);
               }
          }

          if (!system_key_values.empty())
          {
               rocksdb::WriteBatch system_batch;

               for (const auto &kv : system_key_values)
               {
                    system_batch.Put(kv.first, kv.second);
               }

               rocksdb::Status system_status = DBValue->Write(GetWriteOptions(), &system_batch);

               if (!system_status.ok())
               {
                    return 0;
               }
          }

          if (!doc_key_values.empty() && SegmentManagerValue->BatchSetDocuments(doc_key_values) != doc_key_values.size())
          {
               return 0;
          }

          return key_values.size();
     }

     rocksdb::Status status = DBValue->Write(GetWriteOptions(), &batch);

     if (!status.ok())
     {
          const std::string message = "BatchSet: " + status.ToString();
          {
               std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
               LastWriteErrorCode = "rocksdb_write_failed";
               LastWriteErrorMessage = message;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("rocksdb", message + ".");
          }
     }

     return status.ok() ? key_values.size() : 0;
}

/* Get value for key */

std::string DBManager::Get(const std::string &key)
{
     if (!DBValue)
     {
          return "";
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(key))
     {
          return SegmentManagerValue->GetDocument(key);
     }

     std::string value;

     rocksdb::Status status = DBValue->Get(rocksdb::ReadOptions(), key, &value);

     return status.ok() ? value : "";
}

/* Delete key */

int DBManager::Del(const std::string &key)
{
     if (!DBValue)
     {
          return 0;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(key))
     {
          return SegmentManagerValue->DeleteDocument(key);
     }

     rocksdb::Status status = DBValue->Delete(GetWriteOptions(), key);

     return status.ok() ? 1 : 0;
}

/* Delete range of keys */

size_t DBManager::DeleteRange(const std::string &start_key, const std::string &end_key)
{
     if (!DBValue)
     {
          return 0;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(start_key))
     {
          return SegmentManagerValue->DeleteDocumentRange(start_key, end_key);
     }

     rocksdb::Status status = DBValue->DeleteRange(GetWriteOptions(), DBValue->DefaultColumnFamily(), start_key, end_key);

     return status.ok() ? 1 : 0;
}

bool DBManager::ClearDocumentStorage()
{
     if (!DBValue)
     {
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue)
     {
          return SegmentManagerValue->ClearAllDocuments();
     }

     rocksdb::Status status = DBValue->DeleteRange(GetWriteOptions(),
                                                   DBValue->DefaultColumnFamily(),
                                                   "doc:",
                                                   "doc;");

     return status.ok();
}

/* Check if key exists */

bool DBManager::Exists(const std::string &key)
{
     if (!DBValue)
     {
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(key))
     {
          return SegmentManagerValue->ExistsDocument(key);
     }

     std::string value;

     rocksdb::Status status = DBValue->Get(rocksdb::ReadOptions(), key, &value);

     return status.ok();
}

/* List keys matching pattern */

std::vector<std::string> DBManager::Keys(const std::string &pattern, bool force_fresh)
{
     (void)force_fresh;

     std::vector<std::string> result;

     if (!DBValue)
     {
          return result;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && pattern.starts_with("doc:"))
     {
          return SegmentManagerValue->Keys(pattern);
     }

     std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(rocksdb::ReadOptions()));

     for (it->SeekToFirst(); it->Valid(); it->Next())
     {
          std::string key = it->key().ToString();

          if (Wildcard::Match(key, pattern))
          {
               result.push_back(key);
          }
     }

     return result;
}

/* Count keys with prefix */

size_t DBManager::CountKeys(const std::string &prefix)
{
     if (!DBValue)
     {
          return 0;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(prefix))
     {
          return SegmentManagerValue->CountKeys(prefix);
     }

     size_t count = 0;

     std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
     {
          count++;
     }

     return count;
}

/* DBManager::PrefixKeys - Lists keys with the requested prefix. */

std::vector<std::string> DBManager::PrefixKeys(const std::string &prefix, size_t offset, size_t limit)
{
     std::vector<std::string> keys;

     if (!DBValue)
     {
          return keys;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(prefix))
     {
          return SegmentManagerValue->PrefixKeys(prefix, offset, limit);
     }

     size_t skipped = 0;
     std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
     {
          if (skipped < offset)
          {
               skipped++;
               continue;
          }

          keys.push_back(it->key().ToString());

          if (limit > 0 && keys.size() >= limit)
          {
               break;
          }
     }

     return keys;
}

/* DBManager::ForEachPrefixKeySnapshot - Visits keys from a consistent prefix snapshot. */

bool DBManager::ForEachPrefixKeySnapshot(const std::string &prefix, size_t limit, const std::function<bool(const std::string &)> &callback)
{
     if (!DBValue || !callback)
     {
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(prefix))
     {
          return SegmentManagerValue->ForEachPrefixKeySnapshot(prefix, limit, callback);
     }

     rocksdb::ReadOptions read_options;
     const rocksdb::Snapshot *snapshot = DBValue->GetSnapshot();

     if (!snapshot)
     {
          return false;
     }

     read_options.snapshot = snapshot;
     size_t processed = 0;
     bool completed = true;

     {
          std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(read_options));

          for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
          {
               if (limit > 0 && processed >= limit)
               {
                    break;
               }

               processed++;

               if (!callback(it->key().ToString()))
               {
                    completed = false;
                    break;
               }
          }

          if (!it->status().ok())
          {
               completed = false;
          }
     }

     DBValue->ReleaseSnapshot(snapshot);
     return completed;
}

/* Get total size of values for keys with prefix */

size_t DBManager::GetPrefixTotalSize(const std::string &prefix)
{
     if (!DBValue)
     {
          return 0;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && IsDocumentKey(prefix))
     {
          return SegmentManagerValue->GetPrefixTotalSize(prefix);
     }

     size_t total_size = 0;

     std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
     {
          total_size += it->value().size();
     }

     return total_size;
}

/* Helper to make a key */

std::string DBManager::MakeKey(const std::string &key)
{
     return key;
}

/* Flush memtables to disk */

bool DBManager::Flush()
{
     std::lock_guard<std::mutex> lock(DBValueMutex);

     if (!DBValue)
     {
          return false;
     }

     rocksdb::Status status = DBValue->Flush(rocksdb::FlushOptions());

     if (!status.ok())
     {
          RecordSyncFailure("Flush", "rocksdb_flush_failed", status);
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && !SegmentManagerValue->Flush())
     {
          return false;
     }

     ClearLastSyncError();
     return true;
}

/* Flush memtables and sync WAL */

bool DBManager::FlushAndSync()
{
     std::lock_guard<std::mutex> lock(DBValueMutex);

     if (!DBValue)
     {
          return false;
     }

     DatabaseSyncGuard SyncGuard;

     rocksdb::FlushOptions flush_opts;

     rocksdb::Status status = DBValue->Flush(flush_opts);

     if (!status.ok())
     {
          RecordSyncFailure("FlushAndSync.Flush", "rocksdb_flush_failed", status);
          return false;
     }

     status = DBValue->SyncWAL();

     if (!status.ok())
     {
          RecordSyncFailure("FlushAndSync.SyncWAL", "rocksdb_wal_sync_failed", status);
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && !SegmentManagerValue->FlushAndSync())
     {
          return false;
     }

     ClearLastSyncError();
     return true;
}

/* Sync WAL */

bool DBManager::SyncWAL()
{
     std::lock_guard<std::mutex> lock(DBValueMutex);

     if (!DBValue)
     {
          return false;
     }

     DatabaseSyncGuard SyncGuard;

     rocksdb::Status status = DBValue->SyncWAL();

     if (!status.ok())
     {
          RecordSyncFailure("SyncWAL", "rocksdb_wal_sync_failed", status);
          return false;
     }

     if (SegmentedStorageEnabled && SegmentManagerValue && !SegmentManagerValue->SyncWAL())
     {
          return false;
     }

     ClearLastSyncError();
     return true;
}

/* Trigger manual compaction */

void DBManager::Compact()
{
     if (DBValue)
     {
          DBValue->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
     }

     if (SegmentedStorageEnabled && SegmentManagerValue)
     {
          SegmentManagerValue->Compact();
     }
}

/* Clear all caches */

void DBManager::ClearAllCaches()
{
}

/* Optimized scan operations */

DBManager::ScanResult DBManager::ScanOptimized(long long cursor, const std::string &pattern, long long count)
{
     ScanResult result;

     result.cursor = 0;
     result.has_more = false;

     if (!DBValue)
     {
          return result;
     }

     std::unique_ptr<rocksdb::Iterator> it(DBValue->NewIterator(rocksdb::ReadOptions()));

     it->SeekToFirst();

     /* Skip elements based on cursor (offset) */

     long long skipped = 0;

     while (it->Valid() && skipped < cursor)
     {
          it->Next();
          skipped++;
     }

     long long processed = 0;

     while (it->Valid() && processed < count)
     {
          std::string key = it->key().ToString();

          if (Wildcard::Match(key, pattern))
          {
               result.keys.push_back(key);
          }

          it->Next();
          processed++;
     }

     if (it->Valid())
     {
          result.has_more = true;
          result.cursor = cursor + processed;
     }

     return result;
}

/* Returns engine information */

std::unordered_map<std::string, std::string> DBManager::Info()
{
     std::unordered_map<std::string, std::string> info;

     if (!DBValue)
     {
          return info;
     }

     std::string value;

     if (DBValue->GetProperty("rocksdb.stats", &value))
     {
          info["stats"] = value;
     }

     info["wal_oversized_rejections_total"] = std::to_string(RejectedWALEntriesTotal.load(std::memory_order_relaxed));
     info["wal_oversized_rejections_normal"] = std::to_string(RejectedWALEntriesNormal.load(std::memory_order_relaxed));
     info["wal_oversized_rejections_recovery"] = std::to_string(RejectedWALEntriesRecovery.load(std::memory_order_relaxed));

     if (SegmentedStorageEnabled && SegmentManagerValue)
     {
          const auto segment_stats = SegmentManagerValue->GetStats();
          info["segmented_storage_enabled"] = "true";
          info["active_segment_id"] = segment_stats.ActiveSegmentId;
          info["sealed_segment_count"] = std::to_string(segment_stats.SealedSegmentCount);
          info["tombstone_count_estimate"] = std::to_string(segment_stats.TombstoneCountEstimate);
          info["segment_manifest_generation"] = std::to_string(segment_stats.ManifestGeneration);
          info["segment_max_bytes"] = std::to_string(Instance && Instance->Config ? Instance->Config->GetRocksDBOptions().SegmentMaxBytes : 0);
          info["segment_total_bytes"] = std::to_string(segment_stats.TotalBytes);
          info["segment_total_sst_files"] = std::to_string(segment_stats.NumSstFiles);
     }
     else
     {
          info["segmented_storage_enabled"] = "false";
     }

     {
          std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
          if (!LastWriteErrorCode.empty())
          {
               info["last_write_error_code"] = LastWriteErrorCode;
               info["last_write_error_message"] = LastWriteErrorMessage;
          }
     }

     {
          std::lock_guard<std::mutex> lock(LastSyncErrorMutex);
          if (!LastSyncErrorCode.empty())
          {
               info["last_sync_error_code"] = LastSyncErrorCode;
               info["last_sync_error_message"] = LastSyncErrorMessage;
          }
     }

     return info;
}

/* Reset after fork */

void DBManager::ResetAfterFork()
{
     if (DBValue)
     {
          /* Close and reopen */

          Shutdown();

          Initialize();
     }
}

/* Create final checkpoint */

bool DBManager::CreateFinalCheckpoint()
{
     return true;
}

/* Get write options based on WALSyncMode */

rocksdb::WriteOptions DBManager::GetWriteOptions() const
{
     rocksdb::WriteOptions write_opts;

     if (WALSyncMode == "none")
     {
          write_opts.sync = false;
          write_opts.disableWAL = true;
     }
     else if (WALSyncMode == "full")
     {
          write_opts.sync = true;
          write_opts.disableWAL = false;
     }
     else
     {
          /* Default: normal */

          write_opts.sync = false;
          write_opts.disableWAL = false;
     }

     return write_opts;
}

/* Returns database statistics */

DBManager::Stats DBManager::GetRocksDBStats() const
{
     Stats stats;

     if (!DBValue)
     {
          return stats;
     }

     /* Simplified statistics extraction */

     try
     {
          uint64_t memtable_size = 0;
          uint64_t block_cache_usage = 0;
          uint64_t index_and_filter_cache_usage = 0;

          DBValue->GetIntProperty("rocksdb.cur-size-all-mem-tables", &memtable_size);
          DBValue->GetIntProperty("rocksdb.block-cache-usage", &block_cache_usage);
          DBValue->GetIntProperty("rocksdb.estimate-table-readers-mem", &index_and_filter_cache_usage);

          stats.memtable_size = memtable_size;
          stats.block_cache_usage = block_cache_usage;
          stats.index_and_filter_cache_usage = index_and_filter_cache_usage;

          std::uint64_t total_size_bytes = 0;
          std::uint64_t sstable_count = 0;

          if (!DBPath.empty() && std::filesystem::exists(DBPath))
          {
               for (const auto &entry : std::filesystem::recursive_directory_iterator(DBPath))
               {
                    if (!entry.is_regular_file())
                    {
                         continue;
                    }

                    total_size_bytes += entry.file_size();

                    if (entry.path().extension() == ".sst")
                    {
                         sstable_count++;
                    }
               }
          }

          stats.total_db_size = total_size_bytes;
          stats.num_sst_files = sstable_count;

          if (SegmentedStorageEnabled && SegmentManagerValue)
          {
               const auto segment_stats = SegmentManagerValue->GetStats();
               stats.segmented_storage_enabled = true;
               stats.total_db_size += segment_stats.TotalBytes;
               stats.num_sst_files += segment_stats.NumSstFiles;
               stats.active_segment_id = segment_stats.ActiveSegmentId;
               stats.sealed_segment_count = segment_stats.SealedSegmentCount;
               stats.tombstone_count_estimate = segment_stats.TombstoneCountEstimate;
               stats.segment_manifest_generation = segment_stats.ManifestGeneration;
               stats.segment_max_bytes = Instance && Instance->Config ? Instance->Config->GetRocksDBOptions().SegmentMaxBytes : 0;
               stats.segment_total_bytes = segment_stats.TotalBytes;
               stats.segment_total_sst_files = segment_stats.NumSstFiles;
          }
     }
     catch (...)
     {
          /* Best-effort stats; ignore filesystem or property errors. */
     }

     return stats;
}

/* DBManager::GetDBPath - Returns the database path. */

std::string DBManager::GetDBPath() const
{
     if (DBValue)
     {
          return DBValue->GetName();
     }

     return DBPath;
}

uint64_t DBManager::GetLatestSequenceNumber() const
{
     std::lock_guard<std::mutex> lock(DBValueMutex);
     return DBValue ? DBValue->GetLatestSequenceNumber() : 0;
}

std::string DBManager::GetWALSyncMode() const
{
     return WALSyncMode;
}

rocksdb::WriteOptions DBManager::GetEffectiveWriteOptions() const
{
     return GetWriteOptions();
}

rocksdb::Options DBManager::GetEffectiveOptions() const
{
     std::lock_guard<std::mutex> lock(DBValueMutex);
     return OptionsValue;
}

std::string DBManager::GetLastSyncErrorCode() const
{
     std::lock_guard<std::mutex> lock(LastSyncErrorMutex);
     return LastSyncErrorCode;
}

std::string DBManager::GetLastSyncErrorMessage() const
{
     std::lock_guard<std::mutex> lock(LastSyncErrorMutex);
     return LastSyncErrorMessage;
}

/* Get actual number of background threads */

int DBManager::GetBackgroundThreadCount() const
{
     return OptionsValue.max_background_jobs;
}

/* DBManager::GetLastWriteErrorCode - Returns the last write error code. */

std::string DBManager::GetLastWriteErrorCode() const
{
     std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
     return LastWriteErrorCode;
}

/* DBManager::GetLastWriteErrorMessage - Returns the last write error message. */

std::string DBManager::GetLastWriteErrorMessage() const
{
     std::lock_guard<std::mutex> lock(LastWriteErrorMutex);
     return LastWriteErrorMessage;
}
