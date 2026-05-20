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
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * Core types defined here for use across the codebase.
 */

struct Document
{
     std::string ID;

     std::string Title;

     std::string Content;

     std::unordered_map<std::string, std::string> Fields;

     double Score = 0.0;

     uint64_t Timestamp = 0;
};

struct StorageDocument
{
     std::string ID;

     std::string Data;
};

struct CollectionConfig
{
     std::string Name;

     std::unordered_map<std::string, std::string> Fields;

     std::unordered_map<std::string, std::string> Metadata;
};

struct CollectionIntegrityStatus
{
     std::string Collection;

     size_t MetadataCount = 0;

     size_t ActualCount = 0;

     size_t IndexedCount = 0;

     size_t ReindexedDocuments = 0;

     bool CollectionExists = false;

     bool MetadataMatch = false;

     bool IndexMatch = false;

     bool IndexPresent = false;

     bool IndexVerified = false;

     bool IndexRebuilt = false;

     std::string Error;
};

struct IntegrityReport
{
     bool Success = true;

     bool RebuildIndex = false;

     size_t CollectionsScanned = 0;

     size_t CollectionsRepaired = 0;

     size_t CounterMismatches = 0;

     size_t IndexMismatches = 0;

     std::vector<CollectionIntegrityStatus> Collections;
};

enum class LazyIndexBuildState
{
     None,
     Building,
     Complete,
     Failed
};

struct LazyIndexState
{
     LazyIndexBuildState State = LazyIndexBuildState::None;

     size_t SourceCount = 0;

     size_t IndexedCount = 0;

     uint64_t Generation = 0;

     std::string Error;
};

/*
      * HybridStorageManager - RocksDB-based storage and search manager.
      * Provides the same interface as the LSM-based version.
      */

class HybridStorageManager
{
   private:

     HybridStorageManager() = default;

     ~HybridStorageManager() = default;

     HybridStorageManager(const HybridStorageManager&) = delete;

     HybridStorageManager& operator=(const HybridStorageManager&) = delete;

     /* Initialized tracks initialization state. */

     std::atomic<bool> Initialized{false};

     /* CollectionsMutex guards collection operations. */

     std::mutex CollectionsMutex;

     /* DocumentsMutex guards document operations. */

     std::mutex DocumentsMutex;

     /* PerKeyMutex guards per-key mutex creation. */

     std::mutex PerKeyMutex;

     /* KeyMutexes stores per-key mutexes. */

     std::unordered_map<std::string, std::unique_ptr<std::mutex>> KeyMutexes;

     /* KeyMutexesMapMutex guards the KeyMutexes map. */

     std::mutex KeyMutexesMapMutex;

     /* Collections stores collection configurations. */

     std::unordered_map<std::string, CollectionConfig> Collections;

     /* CollectionsBeingIndexed tracks collections being indexed. */

     std::unordered_set<std::string> CollectionsBeingIndexed;

     /* LazyIndexStates tracks explicit lazy index build completeness. */

     std::unordered_map<std::string, LazyIndexState> LazyIndexStates;

     /* IndexingMutex guards CollectionsBeingIndexed. */

     std::mutex IndexingMutex;

     /* FlushThreadRunning tracks background flush thread state. */

     std::atomic<bool> FlushThreadRunning{false};

     /* FlushThread runs periodic flush operations. */

     std::thread FlushThread;

     /* FlushThreadRegistered tracks whether the flush thread consumed a global slot. */

     bool FlushThreadRegistered{false};

     /* FlushThreadMutex guards flush thread state. */

     std::mutex FlushThreadMutex;

     /* FlushThreadCV wakes the background flush thread for shutdown. */

     std::condition_variable FlushThreadCV;

     /* PostDeleteCleanupFailures counts deletes where storage succeeded but secondary cleanup drifted. */

     std::atomic<uint64_t> PostDeleteCleanupFailures{0};

     /* CleanupUnusedKeyMutexes removes stale key mutexes. */

     void CleanupUnusedKeyMutexes();

     /* ResolveIndexDir returns the configured on-disk index directory. */

     std::string ResolveIndexDir() const;

     /* FlushIndexesToDisk writes dirty in-memory indexes to the resolved directory. */

     size_t FlushIndexesToDisk(uint64_t min_dirty_age_seconds = 0, size_t max_collections = 0);

     /* PersistStorageState flushes metadata, indexes, and optionally RocksDB. */

     void PersistStorageState(bool update_counters, bool sync_database, bool log_flush_errors);

     /* BackgroundFlushThread runs periodic flush work. */

     void BackgroundFlushThread();

     /* IndexCollectionInBackground builds indexes asynchronously. */

     void IndexCollectionInBackground(const std::string& collection, const std::vector<std::string>& doc_keys);

     /* MarkCollectionIndexDirty invalidates lazy index completion state after writes. */

     void MarkCollectionIndexDirty(const std::string& collection);

   public:

     /* GetInstance returns the singleton manager. */

     static HybridStorageManager& GetInstance();

     /* Initialize initializes the storage manager. */

     bool Initialize();

     /* Start starts the storage manager. */

     bool Start();

     /* IsInitialized reports whether the manager is ready. */

     bool IsInitialized() const
     {
          return Initialized.load();
     }

     /* Shutdown stops the storage manager. */

     void Shutdown();

     /* ResetAfterFork resets state after fork. */

     void ResetAfterFork();

     /* StartMetadataScanThread starts metadata scanning. */

     void StartMetadataScanThread();

     /* StartBackgroundFlushThread starts periodic flushing. */

     void StartBackgroundFlushThread();

     /* UpdateCollectionCounters updates collection counters. */

     void UpdateCollectionCounters(bool force = false);
     void UpdateCollectionCountersPrefix(const std::string &prefix, bool force = false);

     /* SaveDataToDisk persists data to disk. */

     void SaveDataToDisk();

     /* LoadCollectionsFromRocksDB loads collections from storage. */

     bool LoadCollectionsFromRocksDB();

     /* IsMetadataScanComplete reports metadata scan status. */

     bool IsMetadataScanComplete() const;

     /* Stats stores storage statistics. */

     struct Stats
     {
          size_t num_keys_written = 0;
          size_t bytes_written = 0;
          size_t num_keys_read = 0;
          size_t bytes_read = 0;
          size_t memory_usage = 0;
          size_t total_documents = 0;
          size_t total_collections = 0;
          size_t total_db_size = 0;
          size_t memtable_size = 0;
          size_t sstable_count = 0;
          double index_size_mb = 0.0;
     };

     /* GetStats returns current stats. */

     Stats GetStats() const;

     /* CreateCollection creates a collection. */

     bool CreateCollection(const std::string& name, const CollectionConfig& config);

     /* DeleteCollection deletes a collection. */

     bool DeleteCollection(const std::string& name);

     /* CollectionExists checks for collection existence. */

     bool CollectionExists(const std::string& name);

     /* ListCollections lists collections. */

     std::vector<std::string> ListCollections();

     /* GetCollectionConfig returns the stored collection configuration when available. */

     bool GetCollectionConfig(const std::string& name, CollectionConfig& config);

     /* UpdateCollectionMetadata sets one underscore-prefixed metadata key for a collection. */

     bool UpdateCollectionMetadata(const std::string& name, const std::string& key, const std::string& value);

     /* FlushAll removes all collections and documents. */

     bool FlushAll();

     /* Get fetches a value by key. */

     std::string Get(const std::string& key);

     /* Delete removes a value by key. */

     bool Delete(const std::string& key);

     /* AddDocument adds a document to a collection. */

     bool AddDocument(const std::string& collection, const Document& doc);

     /* AddDocumentsBatch inserts documents in bulk. */

     size_t AddDocumentsBatch(const std::string& collection, const std::vector<Document>& documents, bool assume_new_documents = false);

     /* GetDocument retrieves a document by id. */

     Document GetDocument(const std::string& collection, const std::string& document_id);

     /* ListDocuments lists documents in a collection. */

     std::vector<Document> ListDocuments(const std::string& collection, int limit = 10, int offset = 0);

     /* DeleteDocument deletes a document by id. */

     bool DeleteDocument(const std::string& collection, const std::string& document_id);

     /* GetPostDeleteCleanupFailures returns deletes that left secondary cleanup incomplete. */

     uint64_t GetPostDeleteCleanupFailures() const
     {
          return PostDeleteCleanupFailures.load(std::memory_order_relaxed);
     }

     /* UpdateDocument updates a document atomically. */

     bool UpdateDocument(const std::string& collection, const Document& new_doc);

     /* GetCollectionDocumentCount returns document count. */

     size_t GetCollectionDocumentCount(const std::string& collection);

     /* CountStoredDocuments returns the exact number of persisted documents for a collection. */

     size_t CountStoredDocuments(const std::string& collection);

     /* CheckCollectionIntegrity returns the current consistency state for one collection. */

     CollectionIntegrityStatus CheckCollectionIntegrity(const std::string& collection);

     /* CheckIntegrity returns the current consistency state for one or all collections. */

     IntegrityReport CheckIntegrity(const std::string& collection = "");

     /* RebuildCollectionIndex recreates one collection's inverted index from persisted documents. */

     bool RebuildCollectionIndex(const std::string& collection, size_t* reindexed_documents = nullptr, std::string* error_message = nullptr);

     /* RepairCollection repairs metadata counters and optionally rebuilds one collection index. */

     CollectionIntegrityStatus RepairCollection(const std::string& collection, bool rebuild_index);

     /* RepairIntegrity repairs one or all collections and returns a summary report. */

     IntegrityReport RepairIntegrity(const std::string& collection = "", bool rebuild_index = false);

     /* GetCollectionSize returns total collection size. */

     size_t GetCollectionSize(const std::string& collection);

     /* LazyLoadCollectionIndex loads index on demand. */

     bool LazyLoadCollectionIndex(const std::string& collection);

     /* IsCollectionIndexing reports whether a collection is currently being indexed. */

     bool IsCollectionIndexing(const std::string& collection);

     /* IsCollectionIndexComplete reports whether lazy indexing finished for the current collection generation. */

     bool IsCollectionIndexComplete(const std::string& collection, size_t expected_count);

     /* GetCollectionMutex returns the collection mutex. */

     std::mutex& GetCollectionMutex(const std::string& collection)
     {
          return CollectionsMutex;
     }

     /* GetKeyMutex returns or creates a mutex for a key. */

     std::mutex& GetKeyMutex(const std::string& key);
};

/* Free function for backward compatibility */

HybridStorageManager& HybridStorageManagerInstance();
