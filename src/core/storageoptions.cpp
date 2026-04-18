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
#include <cctype>

#include "runtime/configreader.h"

#if __has_include("core/hlcore.h")
#include "core/hlcore.h"
#else
#include "core/hlquery.h"
#endif

#include "runtime/serverconfig.h"

/* Loads RocksDB configuration options from the provided config reader instance */

RocksDBOptions RocksDBOptions::LoadFromConfigReader(const ConfigReader &ReaderInstance)
{
     RocksDBOptions OptionsResult = Default();

     auto RocksDBSettingsTag = ReaderInstance.GetTag("database");

     if (!RocksDBSettingsTag)
     {
          RocksDBSettingsTag = ReaderInstance.GetTag("rocksdb");
     }

     if (!RocksDBSettingsTag)
     {
          /* No specific RocksDB configuration found; returning default options */

          return OptionsResult;
     }

     /* Configure directory paths for data persistence */

     OptionsResult.DataDir = RocksDBSettingsTag->GetString("data_dir", OptionsResult.DataDir);

     /* Configure write buffer and merging parameters */

     OptionsResult.WriteBufferSize = RocksDBSettingsTag->GetSize("write_buffer_size", OptionsResult.WriteBufferSize);

     OptionsResult.MaxWriteBufferNumber = RocksDBSettingsTag->GetInt("max_write_buffer_number", OptionsResult.MaxWriteBufferNumber);

     OptionsResult.MinWriteBufferNumberToMerge = RocksDBSettingsTag->GetInt("min_write_buffer_number_to_merge", OptionsResult.MinWriteBufferNumberToMerge);

     /* Configure background job threading, respecting global process limits */

     auto ParseRocksThreads = [&](const std::string &AttributeName, int DefaultVal) -> int
     {
          std::string ValueStr = RocksDBSettingsTag->GetString(AttributeName, "");

          if (ValueStr == "max")
          {
               return 0;
          }

          return RocksDBSettingsTag->GetInt(AttributeName, DefaultVal);
     };

     int ConfiguredMaxBackgroundJobs = ParseRocksThreads("max_background_jobs", OptionsResult.MaxBackgroundJobs);

     int ConfiguredMaxBackgroundFlushes = ParseRocksThreads("max_background_flushes", OptionsResult.MaxBackgroundFlushes);

     int ConfiguredMaxBackgroundCompactions = ParseRocksThreads("max_background_compactions", OptionsResult.MaxBackgroundCompactions);

     /* Retrieve the global thread limit to perform automated resource scaling */

     int GlobalMaxThreadsValue = 0;

     if (Instance && Instance->Config)
     {
          GlobalMaxThreadsValue = Instance->Config->GetMaxThreads();
     }

     /* Resolve 'max' placeholder values into actual thread counts */

     if (GlobalMaxThreadsValue > 0)
     {
          int ReservedForSystemCount = 4;

          int AvailableForRocksDBCount = std::max(1, GlobalMaxThreadsValue - ReservedForSystemCount);

          if (ConfiguredMaxBackgroundJobs == 0)
          {
               ConfiguredMaxBackgroundJobs = std::max(2, AvailableForRocksDBCount / 2);
          }

          if (ConfiguredMaxBackgroundCompactions == 0)
          {
               ConfiguredMaxBackgroundCompactions = std::max(1, ConfiguredMaxBackgroundJobs * 3 / 4);
          }

          if (ConfiguredMaxBackgroundFlushes == 0)
          {
               ConfiguredMaxBackgroundFlushes = std::max(1, ConfiguredMaxBackgroundJobs - ConfiguredMaxBackgroundCompactions);
          }
     }

     /* Apply the validated background job configuration */

     OptionsResult.MaxBackgroundJobs = ConfiguredMaxBackgroundJobs;

     OptionsResult.MaxBackgroundFlushes = ConfiguredMaxBackgroundFlushes;

     OptionsResult.MaxBackgroundCompactions = ConfiguredMaxBackgroundCompactions;

     /* Configure level-based storage and compaction triggers */

     OptionsResult.TargetFileSizeBase = RocksDBSettingsTag->GetSize("target_file_size_base", OptionsResult.TargetFileSizeBase);

     OptionsResult.MaxBytesForLevelBase = RocksDBSettingsTag->GetSize("max_bytes_for_level_base", OptionsResult.MaxBytesForLevelBase);

     OptionsResult.MaxBytesForLevelMultiplier = RocksDBSettingsTag->GetInt("max_bytes_for_level_multiplier", OptionsResult.MaxBytesForLevelMultiplier);

     OptionsResult.Level0SlowdownWritesTrigger = RocksDBSettingsTag->GetInt("level0_slowdown_writes_trigger", OptionsResult.Level0SlowdownWritesTrigger);

     OptionsResult.Level0StopWritesTrigger = RocksDBSettingsTag->GetInt("level0_stop_writes_trigger", OptionsResult.Level0StopWritesTrigger);

     /* Configure data compression algorithms */

     std::string CompressionTypeStr = RocksDBSettingsTag->GetString("compression", OptionsResult.Compression);

     std::transform(CompressionTypeStr.begin(), CompressionTypeStr.end(), CompressionTypeStr.begin(), ::tolower);

     if (CompressionTypeStr == "none" || CompressionTypeStr == "snappy" || CompressionTypeStr == "zlib" ||
         CompressionTypeStr == "lz4" || CompressionTypeStr == "zstd")
     {
          OptionsResult.Compression = CompressionTypeStr;
     }

     std::string BottommostCompressionStr = RocksDBSettingsTag->GetString("bottommost_compression", OptionsResult.BottommostCompression);

     std::transform(BottommostCompressionStr.begin(), BottommostCompressionStr.end(), BottommostCompressionStr.begin(), ::tolower);

     if (BottommostCompressionStr == "none" || BottommostCompressionStr == "snappy" || BottommostCompressionStr == "zlib" ||
         BottommostCompressionStr == "lz4" || BottommostCompressionStr == "zstd")
     {
          OptionsResult.BottommostCompression = BottommostCompressionStr;
     }

     /* Configure block cache and Bloom filter settings */

     OptionsResult.BlockCacheSize = RocksDBSettingsTag->GetSize("block_cache_size", OptionsResult.BlockCacheSize);

     OptionsResult.BloomFilterBitsPerKey = RocksDBSettingsTag->GetInt("bloom_filter_bits_per_key", OptionsResult.BloomFilterBitsPerKey);

     OptionsResult.EnableBloomFilter = RocksDBSettingsTag->GetBool("enable_bloom_filter", OptionsResult.EnableBloomFilter);

     /* Configure Write-Ahead Log (WAL) parameters */

     OptionsResult.DisableWAL = RocksDBSettingsTag->GetBool("disable_wal", OptionsResult.DisableWAL);

     OptionsResult.WALBytesPerSync = RocksDBSettingsTag->GetSize("wal_bytes_per_sync", OptionsResult.WALBytesPerSync);

     OptionsResult.MaxTotalWALSize = RocksDBSettingsTag->GetSize("max_total_wal_size", OptionsResult.MaxTotalWALSize);

     std::string WALSyncModeStr = RocksDBSettingsTag->GetString("wal_sync_mode", OptionsResult.WALSyncMode);

     std::transform(WALSyncModeStr.begin(), WALSyncModeStr.end(), WALSyncModeStr.begin(), ::tolower);

     if (WALSyncModeStr == "none" || WALSyncModeStr == "normal" || WALSyncModeStr == "full")
     {
          OptionsResult.WALSyncMode = WALSyncModeStr;
     }

     /* Configure advanced performance and I/O settings */

     OptionsResult.UseDirectReads = RocksDBSettingsTag->GetBool("use_direct_reads", OptionsResult.UseDirectReads);

     OptionsResult.UseDirectIOForFlushAndCompaction = RocksDBSettingsTag->GetBool("use_direct_io_for_flush_and_compaction", OptionsResult.UseDirectIOForFlushAndCompaction);

     OptionsResult.MaxOpenFiles = RocksDBSettingsTag->GetInt("max_open_files", OptionsResult.MaxOpenFiles);

     /* Configure the preferred compaction strategy */

     std::string CompactionStyleStr = RocksDBSettingsTag->GetString("compaction_style", OptionsResult.CompactionStyle);

     std::transform(CompactionStyleStr.begin(), CompactionStyleStr.end(), CompactionStyleStr.begin(), ::tolower);

     if (CompactionStyleStr == "level" || CompactionStyleStr == "universal" || CompactionStyleStr == "fifo")
     {
          OptionsResult.CompactionStyle = CompactionStyleStr;
     }

     /* Configure internal block data structures */

     OptionsResult.BlockSize = RocksDBSettingsTag->GetSize("block_size", OptionsResult.BlockSize);

     OptionsResult.BlockRestartInterval = RocksDBSettingsTag->GetInt("block_restart_interval", OptionsResult.BlockRestartInterval);

     /* Configure memory-mapped I/O and access patterns */

     OptionsResult.AllowMmapReads = RocksDBSettingsTag->GetBool("allow_mmap_reads", OptionsResult.AllowMmapReads);

     OptionsResult.AllowMmapWrites = RocksDBSettingsTag->GetBool("allow_mmap_writes", OptionsResult.AllowMmapWrites);

     OptionsResult.AdviseRandomOnOpen = RocksDBSettingsTag->GetBool("advise_random_on_open", OptionsResult.AdviseRandomOnOpen);

     /* Configure integrity checks and verification protocols */

     OptionsResult.ParanoidChecks = RocksDBSettingsTag->GetBool("paranoid_checks", OptionsResult.ParanoidChecks);

     OptionsResult.VerifyChecksumsInCompaction = RocksDBSettingsTag->GetBool("verify_checksums_in_compaction", OptionsResult.VerifyChecksumsInCompaction);

     /* Configure internal metrics and statistics collection */

     OptionsResult.EnableStatistics = RocksDBSettingsTag->GetBool("enable_statistics", OptionsResult.EnableStatistics);

     OptionsResult.StatsDumpPeriodSec = RocksDBSettingsTag->GetInt("stats_dump_period_sec", OptionsResult.StatsDumpPeriodSec);

     /* Configure advanced write pipeline optimizations */

     OptionsResult.EnablePipelinedWrite = RocksDBSettingsTag->GetBool("enable_pipelined_write", OptionsResult.EnablePipelinedWrite);

     OptionsResult.UnorderedWrite = RocksDBSettingsTag->GetBool("unordered_write", OptionsResult.UnorderedWrite);

     /* Configure table cache partitioning settings */

     OptionsResult.TableCacheNumShardBits = RocksDBSettingsTag->GetInt("table_cache_numshardbits", OptionsResult.TableCacheNumShardBits);

     /* Configure background file deletion and maintenance intervals */

     OptionsResult.DeleteObsoleteFilesPeriodMicros = RocksDBSettingsTag->GetSize("delete_obsolete_files_period_micros", OptionsResult.DeleteObsoleteFilesPeriodMicros);

     /* Log the resulting RocksDB configuration for diagnostics */

     if (Instance && Instance->Logs)
     {
          std::string ThreadInfoStr = "max_background_jobs=" + std::to_string(OptionsResult.MaxBackgroundJobs) +
                                      " (flushes=" + std::to_string(OptionsResult.MaxBackgroundFlushes) +
                                      ", compactions=" + std::to_string(OptionsResult.MaxBackgroundCompactions) + ")";

          if (GlobalMaxThreadsValue > 0)
          {
               ThreadInfoStr += " [limited by global MaxThreads=" + std::to_string(GlobalMaxThreadsValue) + "]";
          }

          Instance->Logs->Normal("rocksdb_config", "RocksDB options loaded: write_buffer=" + std::to_string(OptionsResult.WriteBufferSize / (1024 * 1024)) + "MB, " + "block_cache=" + std::to_string(OptionsResult.BlockCacheSize / (1024 * 1024)) + "MB, " + ThreadInfoStr + ", compression=" + OptionsResult.Compression + ".");
     }

     return OptionsResult;
}
