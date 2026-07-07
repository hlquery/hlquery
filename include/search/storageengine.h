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

#ifndef ROCKSDB_NAMESPACE
#define ROCKSDB_NAMESPACE rocksdb
#endif

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "search/writeaheadlogvalidator.h"

class SegmentManager;

/*
 * RocksDB wrapper for hlquery storage engine.
 * Provides high-performance key-value storage with RocksDB.
 */

class DBManager
{
   private:

     /* DBValue holds the RocksDB instance. */

     std::unique_ptr<rocksdb::DB> DBValue;

     /* SegmentManagerValue routes logical doc:* keys when segmented storage is enabled. */

     std::unique_ptr<SegmentManager> SegmentManagerValue;

     /* SegmentedStorageEnabled gates all segment routing. */

     bool SegmentedStorageEnabled = false;

     /* DBValueMutex serializes DB handle flush/sync operations with shutdown. */

     mutable std::mutex DBValueMutex;

     /* OptionsValue stores RocksDB options. */

     rocksdb::Options OptionsValue;

     /* DBPath stores the database path. */

     std::string DBPath;

     /* CurrentDB stores the active database id. */

     int CurrentDB;

     /* KeyTypesMutex guards KeyTypes. */

     std::mutex KeyTypesMutex;

     /* KeyTypes stores key type metadata. */

     std::unordered_map<std::string, std::string> KeyTypes;

     /* WALSyncMode stores WAL sync behavior. */

     std::string WALSyncMode;

     /* MatchesPattern checks wildcard pattern matching. */

     bool MatchesPattern(const std::string& key, const std::string& pattern);

     /* IsDocumentKey returns whether a logical key is a document payload key. */

     bool IsDocumentKey(const std::string& key) const;

     /* GetWriteOptions returns write options based on WAL sync mode. */

     rocksdb::WriteOptions GetWriteOptions() const;

     /* Stores details of the most recent write validation failure. */

     mutable std::mutex LastWriteErrorMutex;
     std::string LastWriteErrorCode;
     std::string LastWriteErrorMessage;

     /* Stores details of the most recent flush or WAL sync failure. */

     mutable std::mutex LastSyncErrorMutex;
     std::string LastSyncErrorCode;
     std::string LastSyncErrorMessage;

     /* Counters for oversized WAL entry rejections. */

     std::atomic<uint64_t> RejectedWALEntriesTotal{0};
     std::atomic<uint64_t> RejectedWALEntriesNormal{0};
     std::atomic<uint64_t> RejectedWALEntriesRecovery{0};

     /* Records validation failures for observability and triage. */

     void RecordWriteValidationFailure(const char* operation, const WALEntryValidationResult& validation, bool is_recovery);

     /* Clears stale write error state after successful validation. */

     void ClearLastWriteError();

     /* Records durable sync failures for logs and status endpoints. */

     void RecordSyncFailure(const char* operation, const std::string& code, const rocksdb::Status& status);

     /* Clears stale sync failure state after a successful flush or sync operation. */

     void ClearLastSyncError();

   public:

     /* Constructor. */

     DBManager();

     /* Destructor. */

     ~DBManager();

     /* Initialize initializes the database. */

     bool Initialize();

     /* Shutdown stops and closes the database. */

     void Shutdown();

     /* IsOpen reports whether the database is open. */

     bool IsOpen() const;

     /* StartBackgroundThreads starts RocksDB background threads. */

     void StartBackgroundThreads();

     /* Set writes a key-value pair. */

     bool Set(const std::string& key, const std::string& value);

     /* SetIfNotExists writes a key-value pair if missing. */

     bool SetIfNotExists(const std::string& key, const std::string& value);

     /* BatchSet writes multiple key-value pairs. */

     size_t BatchSet(const std::vector<std::pair<std::string, std::string>>& key_values);

     /* Get retrieves a value by key. */

     std::string Get(const std::string& key);

     /* Del deletes a key and returns deleted count. */

     int Del(const std::string& key);

     /* DeleteRange deletes keys in a key range. */

     size_t DeleteRange(const std::string& start_key, const std::string& end_key);

     /* Exists checks whether a key exists. */

     bool Exists(const std::string& key);

     /* Keys returns keys matching a wildcard pattern. */

     std::vector<std::string> Keys(const std::string& pattern, bool force_fresh = false);

     /* CountKeys counts keys with a prefix. */

     size_t CountKeys(const std::string& prefix);

     /* PrefixKeys returns a bounded page of keys with a prefix. */

     std::vector<std::string> PrefixKeys(const std::string& prefix, size_t offset = 0, size_t limit = 0);

     /* ForEachPrefixKeySnapshot iterates keys with a prefix over one RocksDB snapshot. */

     bool ForEachPrefixKeySnapshot(const std::string& prefix, size_t limit, const std::function<bool(const std::string&)>& callback);

     /* GetPrefixTotalSize returns total size for a prefix. */

     size_t GetPrefixTotalSize(const std::string& prefix);

     /* MakeKey prepares a key for storage. */

     std::string MakeKey(const std::string& key);

     /* Flush flushes pending writes and reports whether RocksDB accepted it. */

     bool Flush();

     /* FlushAndSync flushes and syncs WAL. */

     bool FlushAndSync();

     /* SyncWAL syncs the WAL to disk. */

     bool SyncWAL();

     /* Compact runs RocksDB compaction. */

     void Compact();

     /* ClearAllCaches clears RocksDB caches. */

     void ClearAllCaches();

     /* ScanResult stores scan results. */

     struct ScanResult
     {
          long long cursor;
          std::vector<std::string> keys;
          bool has_more;
     };

     /* ScanOptimized scans keys with a cursor. */

     ScanResult ScanOptimized(long long cursor, const std::string& pattern = "*", long long count = 10);

     /* Info returns database information. */

     std::unordered_map<std::string, std::string> Info();

     /* ResetAfterFork resets internal state after fork. */

     void ResetAfterFork();

     /* CreateFinalCheckpoint creates a final checkpoint. */

     bool CreateFinalCheckpoint();

     /* Stats stores database statistics. */

     struct Stats
     {
          size_t num_keys_written = 0;
          size_t bytes_written = 0;
          size_t num_keys_read = 0;
          size_t bytes_read = 0;
          uint64_t block_cache_hits = 0;
          uint64_t block_cache_misses = 0;
          uint64_t block_cache_usage = 0;
          uint64_t index_and_filter_cache_hits = 0;
          uint64_t index_and_filter_cache_misses = 0;
          uint64_t index_and_filter_cache_usage = 0;
          uint64_t total_db_size = 0;
          uint64_t memtable_size = 0;
          uint64_t num_sst_files = 0;
          bool segmented_storage_enabled = false;
          std::string active_segment_id;
          uint64_t sealed_segment_count = 0;
          uint64_t tombstone_count_estimate = 0;
          uint64_t segment_manifest_generation = 0;
          uint64_t segment_max_bytes = 0;
          uint64_t segment_total_bytes = 0;
          uint64_t segment_total_sst_files = 0;
     };

     /* GetRocksDBStats returns RocksDB statistics. */

     Stats GetRocksDBStats() const;

     /* GetDBPath returns the database path. */

     std::string GetDBPath() const;

     /* GetBackgroundThreadCount returns background thread count. */

     int GetBackgroundThreadCount() const;

     /* Returns the last structured write error code for failed writes. */

     std::string GetLastWriteErrorCode() const;

     /* Returns the last structured write error message for failed writes. */

     std::string GetLastWriteErrorMessage() const;

     /* GetLSMEngine returns the LSM engine pointer for legacy use. */

     void* GetLSMEngine() const
     {
          return nullptr;
     }
};
