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

     /* CleanupUnusedKeyMutexes removes stale key mutexes. */

     void CleanupUnusedKeyMutexes();

     /* ResolveIndexDir returns the configured on-disk index directory. */

     std::string ResolveIndexDir() const;

     /* FlushIndexesToDisk writes in-memory indexes to the resolved directory. */

     void FlushIndexesToDisk();

     /* PersistStorageState flushes metadata, indexes, and optionally RocksDB. */

     void PersistStorageState(bool update_counters, bool sync_database, bool log_flush_errors);

     /* BackgroundFlushThread runs periodic flush work. */

     void BackgroundFlushThread();

     /* IndexCollectionInBackground builds indexes asynchronously. */

     void IndexCollectionInBackground(const std::string& collection, const std::vector<std::string>& doc_keys);

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

     /* UpdateDocument updates a document atomically. */

     bool UpdateDocument(const std::string& collection, const Document& new_doc);

     /* GetCollectionDocumentCount returns document count. */

     size_t GetCollectionDocumentCount(const std::string& collection);

     /* GetCollectionSize returns total collection size. */

     size_t GetCollectionSize(const std::string& collection);

     /* LazyLoadCollectionIndex loads index on demand. */

     bool LazyLoadCollectionIndex(const std::string& collection);

     /* IsCollectionIndexing reports whether a collection is currently being indexed. */

     bool IsCollectionIndexing(const std::string& collection);

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
