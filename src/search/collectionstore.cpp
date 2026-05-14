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

#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <set>
#include <sstream>
#include <thread>
#include <vendor/json/json.hpp>

#include "core/hlquery.h"
#include "runtime/threadlimit.h"
#include "search/storageengine.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "sam/sam.h"
#include "sam/lang.h"
#include "search/writeaheadlogvalidator.h"
#include "utils/consolewriter.h"

/* Static container for background indexing threads */

static std::vector<std::thread> IndexingThreads;
static std::mutex IndexingThreadsMutex;

static std::string GetCollectionConfigKey(const std::string &Name)
{
     return "collection_config:" + Name;
}

static bool CollectionNeedsLanguageDetection(const std::string &Name)
{
     CollectionConfig config;

     if (!HybridStorageManagerInstance().GetCollectionConfig(Name, config))
     {
          return false;
     }

     const auto it = config.Metadata.find("_lang");

     if (it == config.Metadata.end())
     {
          return true;
     }

     const std::string value = it->second;
     return value.empty() || value == "auto" || value == "und";
}

static void RefreshCollectionLanguageIfNeeded(const std::string &Collection,
                                              const Document *SeedDocument = nullptr)
{
     if (Collection.empty() || !CollectionNeedsLanguageDetection(Collection))
     {
          return;
     }

     std::string language = "und";

     if (SeedDocument)
     {
          language = sam::lang::DetectDocumentLanguage(Collection, *SeedDocument);
     }

     if (language.empty() || language == "und")
     {
          language = sam::lang::DetectCollectionLanguage(Collection, 128);
     }

     if (!language.empty() && language != "und")
     {
          HybridStorageManagerInstance().UpdateCollectionMetadata(Collection, "_lang", language);
     }
}

static std::string SerializeCollectionConfig(const CollectionConfig &Config)
{
     nlohmann::json config_json;

     config_json["name"] = Config.Name;
     config_json["fields"] = Config.Fields;
     config_json["metadata"] = Config.Metadata;

     return config_json.dump();
}

static bool ParseCollectionMetaValue(const std::string &MetaValue, size_t *OutCount, time_t *OutTimestamp)
{
     if (OutCount)
     {
          *OutCount = 0;
     }

     if (OutTimestamp)
     {
          *OutTimestamp = time(nullptr);
     }

     if (MetaValue.empty())
     {
          return false;
     }

     const size_t colon_pos = MetaValue.find(':');

     if (colon_pos == std::string::npos)
     {
          return false;
     }

     try
     {
          if (OutCount)
          {
               *OutCount = std::stoull(MetaValue.substr(0, colon_pos));
          }

          if (OutTimestamp && colon_pos + 1 < MetaValue.size())
          {
               const time_t parsed = std::stoull(MetaValue.substr(colon_pos + 1));

               if (parsed > 0)
               {
                    *OutTimestamp = parsed;
               }
          }

          return true;
     }
     catch (...)
     {
          return false;
     }
}

static std::string BuildCollectionMetaValue(size_t Count, time_t Timestamp)
{
     return std::to_string(Count) + ":" + std::to_string(Timestamp);
}

static std::string ExtractDocumentIDFromKey(const std::string &DocKey)
{
     const size_t last_colon = DocKey.find_last_of(':');

     if (last_colon == std::string::npos || last_colon + 1 >= DocKey.size())
     {
          return "";
     }

     return DocKey.substr(last_colon + 1);
}

static bool DeserializeCollectionConfig(const std::string &Value, CollectionConfig &Config)
{
     if (Value.empty())
     {
          return false;
     }

     try
     {
          nlohmann::json config_json = nlohmann::json::parse(Value);

          if (config_json.contains("name") && config_json["name"].is_string())
          {
               Config.Name = config_json["name"].get<std::string>();
          }

          if (config_json.contains("fields") && config_json["fields"].is_object())
          {
               Config.Fields.clear();

               for (auto it = config_json["fields"].begin(); it != config_json["fields"].end(); ++it)
               {
                    if (it.value().is_string())
                    {
                         Config.Fields[it.key()] = it.value().get<std::string>();
                    }
                    else if (!it.value().is_null())
                    {
                         Config.Fields[it.key()] = it.value().dump();
                    }
               }
          }

          if (config_json.contains("metadata") && config_json["metadata"].is_object())
          {
               Config.Metadata.clear();

               for (auto it = config_json["metadata"].begin(); it != config_json["metadata"].end(); ++it)
               {
                    if (it.value().is_string())
                    {
                         Config.Metadata[it.key()] = it.value().get<std::string>();
                    }
                    else if (!it.value().is_null())
                    {
                         Config.Metadata[it.key()] = it.value().dump();
                    }
               }
          }

          return !Config.Name.empty();
     }
     catch (...)
     {
          return false;
     }
}

static std::string ResolveStorageRootDir()
{
     std::string base_DataDirValue = std::string(HLQUERY_DATA_DIR);

     const char *EnvDataDir = std::getenv("HLQUERY_DATA_DIR");

     if (EnvDataDir && *EnvDataDir)
     {
          base_DataDirValue = EnvDataDir;
     }

     try
     {
          if (Instance && Instance->Config && Instance->Config->IsValid())
          {
               const auto &rocksdb_opts = Instance->Config->GetRocksDBOptions();

               if (!rocksdb_opts.DataDir.empty())
               {
                    base_DataDirValue = rocksdb_opts.DataDir;
               }
          }
     }
     catch (...)
     {
     }

     std::string storage_root = base_DataDirValue + "/storage";

     try
     {
          const std::string legacy_storage_root = base_DataDirValue + "/rocksdb";

          if (!std::filesystem::exists(storage_root) && std::filesystem::exists(legacy_storage_root))
          {
               storage_root = legacy_storage_root;
          }
     }
     catch (...)
     {

     }

     return storage_root;
}

std::string HybridStorageManager::ResolveIndexDir() const
{
     return ResolveStorageRootDir() + "/indices";
}

void HybridStorageManager::FlushIndexesToDisk()
{
     if (Instance && Instance->SearchIndex)
     {
          Instance->SearchIndex->FlushToDisk(ResolveIndexDir());
     }
}

void HybridStorageManager::PersistStorageState(bool update_counters, bool sync_database, bool log_flush_errors)
{
     if (update_counters)
     {
          UpdateCollectionCounters(true);
     }

     if (sync_database && Instance && Instance->Database)
     {
          try
          {
               if (!Instance->Database->FlushAndSync() && log_flush_errors && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "PersistStorageState: WARNING - FlushAndSync failed, metadata may not be persisted.");
               }
          }
          catch (...)
          {
               if (log_flush_errors && Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "PersistStorageState: WARNING - FlushAndSync failed, metadata may not be persisted.");
               }
          }
     }

     FlushIndexesToDisk();
}
/* Implement the static GetInstance() method declared in the header */

HybridStorageManager &HybridStorageManager::GetInstance()
{
     static HybridStorageManager instance;
     return instance;
}

/* Free function for backward compatibility */

HybridStorageManager &HybridStorageManagerInstance()
{
     return HybridStorageManager::GetInstance();
}

/* GetKeyMutex - Returns a striped mutex for the given key. */

std::mutex &HybridStorageManager::GetKeyMutex(const std::string &key)
{
     static constexpr size_t NumMutexes = 256;
     static std::array<std::mutex, NumMutexes> key_mutex_pool;

     size_t hash = 0;

     for (char c : key)
     {
          hash = hash * 31 + static_cast<unsigned char>(c);
     }

     size_t mutex_index = hash % NumMutexes;

     return key_mutex_pool[mutex_index];
}

/* CleanupUnusedKeyMutexes - Prunes key mutex tracking when the map grows too large. */

void HybridStorageManager::CleanupUnusedKeyMutexes()
{
     std::lock_guard<std::mutex> map_lock(KeyMutexesMapMutex);

     /*
           * Conservative approach: keep all mutexes to avoid race conditions.
           * Memory usage is bounded by number of unique keys processed.
           */

     if (KeyMutexes.size() > 50000)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "Key mutex map is very large: " + std::to_string(KeyMutexes.size()) + ".");
          }
     }
}

/* Start - Starts the hybrid storage manager and initializes dependencies. */

bool HybridStorageManager::Start()
{
     /* Database Initialization */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "Creating DBManager with RocksDB.");
     }

     if (!Instance)
     {
          ConsoleWriter::WriteError("[FATAL] HybridStorageManager::Start() failed: Global instance is null.", true);
          return false;
     }

     Instance->Database = std::make_unique<DBManager>();
     Instance->Engine = Instance->Database.get();

     if (Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "DBManager created, initializing.");
     }

     if (!Instance->Database->Initialize())
     {
          Instance->Engine = nullptr;

          if (Instance->Logs)
          {
               Instance->Logs->Critical("hlquery", "Failed to initialize DBManager.");
          }
          /* Specific error already printed by DBManager::Initialize() */

          return false;
     }

     if (Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "DBManager initialized successfully.");
     }

     /* Verify that the database pointer is valid after initialization */

     if (!Instance->Database)
     {
          if (Instance->Logs)
          {
               Instance->Logs->Critical("hlquery", "Database is null.");
          }
          ConsoleWriter::WriteError("[FATAL] HybridStorageManager::Start() failed: Database is null after initialization.", true);
          return false;
     }

     if (Instance->Logs)
     {
          Instance->Logs->Normal("hlquery", "Database verified and working.");
     }

     /* Initialize InvertedIndex */

     if (Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "Creating InvertedIndex.");
     }

     Instance->SearchIndex = std::make_unique<InvertedIndex>();

     if (Instance->Logs)
     {
          Instance->Logs->Normal("hlquery", "InvertedIndex created successfully.");
     }

     /* Initialize thread pools */

     if (Instance->Logs)
     {
          Instance->Logs->Debug("hlquery", "Thread pool initialization.");
     }

     /* Initialize HybridStorageManager */

     if (!Initialize())
     {
          if (Instance->Logs)
          {
               Instance->Logs->Critical("hlquery", "Failed to initialize HybridStorageManager.");
          }
          ConsoleWriter::WriteError("[FATAL] Failed to initialize HybridStorageManager background services.", true);
          return false;
     }

     return true;
}

/* Initialize - Initializes the hybrid storage manager state. */

bool HybridStorageManager::Initialize()
{
     if (Initialized.load())
     {
          return true;
     }

     if (!Instance || !Instance->Database)
     {
          return false;
     }

     Initialized.store(true);

     /* Start background flush thread for periodic index persistence */

     StartBackgroundFlushThread();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "HybridStorageManager initialized with RocksDB.");
     }

     return true;
}

/* Shutdown - Stops background tasks and releases resources. */

void HybridStorageManager::Shutdown()
{
     if (!Initialized.load())
     {
          return;
     }

     /* Stop background flush thread */

     FlushThreadRunning.store(false);
     FlushThreadCV.notify_all();

     if (FlushThread.joinable())
     {
          FlushThread.join();

          if (FlushThreadRegistered)
          {
               ThreadLimit::DecrementThreadCount();
               FlushThreadRegistered = false;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "Background flush thread stopped.");
          }
     }

     /* Join all indexing threads */

     {
          std::lock_guard<std::mutex> lock(IndexingThreadsMutex);

          for (auto &thread : IndexingThreads)
          {
               if (thread.joinable())
               {
                    thread.join();
               }
          }

          IndexingThreads.clear();
     }

     /* Final flush on shutdown */

     SaveDataToDisk();

     Initialized.store(false);
}

/* ResetAfterFork - Resets runtime state after a process fork. */

void HybridStorageManager::ResetAfterFork()
{
     /* No-op: daemon mode and forking removed */
}

/* StartMetadataScanThread - Starts the metadata scan thread. */

void HybridStorageManager::StartMetadataScanThread()
{
     /* No-op: metadata scanning not implemented */
}

/* UpdateCollectionCounters - Refreshes persisted collection counters. */

void HybridStorageManager::UpdateCollectionCounters(bool force)
{
     /* Update collection document counts from RocksDB */

     if (!Instance || !Instance->Database)
     {
          return;
     }

     /* Get all collections */

     auto collections = ListCollections();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCounters: Updating counts for " + std::to_string(collections.size()) + ".");
     }

     /*
           * Update count for each collection.
           * PERFORMANCE: Use CountKeys instead of Keys().size() to avoid loading all keys into memory.
           */

     for (const auto &collection : collections)
     {
          /* Count actual documents in RocksDB using efficient iterator-based counting */

          std::string prefix = "doc:" + collection + ":";

          size_t actual_count = Instance->Database->CountKeys(prefix);

          /* Get current metadata */

          std::string meta_key = "collection_meta:" + collection;

          std::string meta_value = Instance->Database->Get(meta_key);

          size_t stored_count = 0;

          time_t timestamp = time(nullptr);

          /* Parse existing metadata if present */

          if (!meta_value.empty())
          {
               size_t colon_pos = meta_value.find(':');

               if (colon_pos != std::string::npos)
               {
                    stored_count = std::stoull(meta_value.substr(0, colon_pos));

                    if (colon_pos + 1 < meta_value.size())
                    {
                         time_t parsed_ts = std::stoull(meta_value.substr(colon_pos + 1));

                         if (parsed_ts > 0)
                         {
                              timestamp = parsed_ts;
                         }
                    }
               }
          }

          /* Update metadata only if count changed or if it was missing */

          if (actual_count != stored_count || meta_value.empty())
          {
               std::string new_meta_value = std::to_string(actual_count) + ":" + std::to_string(timestamp);

               Instance->Database->Set(meta_key, new_meta_value);

               if (Instance && Instance->Logs && actual_count != stored_count)
               {
                    Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCounters: Updated '" + collection + "' count from " + std::to_string(stored_count) + " to " + std::to_string(actual_count) + ".");
               }
          }
     }

     /*
           * Flush database only if we are forced or if it's necessary.
           * PERFORMANCE: Avoid unnecessary FlushAndSync if no metadata was updated.
           */

     if (force && Instance && Instance->Database)
     {
          try
          {
               Instance->Database->FlushAndSync();
          }
          catch (...)
          {
               /* If flush fails, log but don't fail - data should still be persisted eventually */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCounters: WARNING - FlushAndSync failed, metadata may not be persisted.");
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCounters: Completed.");
     }
}

void HybridStorageManager::UpdateCollectionCountersPrefix(const std::string &prefix, bool force)
{
     if (prefix.empty())
     {
          UpdateCollectionCounters(force);
          return;
     }

     if (!Instance || !Instance->Database)
     {
          return;
     }

     auto collections = ListCollections();

     std::vector<std::string> filtered;
     filtered.reserve(collections.size());

     for (const auto &collection : collections)
     {
          if (collection.rfind(prefix, 0) == 0)
          {
               filtered.push_back(collection);
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCountersPrefix: Updating counts for " + std::to_string(filtered.size()) + " collection(s) with prefix '" + prefix + "'.");
     }

     for (const auto &collection : filtered)
     {
          std::string doc_prefix = "doc:" + collection + ":";

          size_t actual_count = Instance->Database->CountKeys(doc_prefix);

          std::string meta_key = "collection_meta:" + collection;

          std::string meta_value = Instance->Database->Get(meta_key);

          size_t stored_count = 0;

          time_t timestamp = time(nullptr);

          if (!meta_value.empty())
          {
               size_t colon_pos = meta_value.find(':');

               if (colon_pos != std::string::npos)
               {
                    stored_count = std::stoull(meta_value.substr(0, colon_pos));

                    if (colon_pos + 1 < meta_value.size())
                    {
                         time_t parsed_ts = std::stoull(meta_value.substr(colon_pos + 1));

                         if (parsed_ts > 0)
                         {
                              timestamp = parsed_ts;
                         }
                    }
               }
          }

          if (actual_count != stored_count || meta_value.empty())
          {
               std::string new_meta_value = std::to_string(actual_count) + ":" + std::to_string(timestamp);

               Instance->Database->Set(meta_key, new_meta_value);

               if (Instance && Instance->Logs && actual_count != stored_count)
               {
                    Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCountersPrefix: Updated '" + collection + "' count from " + std::to_string(stored_count) + " to " + std::to_string(actual_count) + ".");
               }
          }
     }

     if (force && Instance && Instance->Database)
     {
          try
          {
               Instance->Database->FlushAndSync();
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCountersPrefix: WARNING - FlushAndSync failed, metadata may not be persisted.");
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "UpdateCollectionCountersPrefix: Completed.");
     }
}

/* CreateCollection - Creates a new collection and stores metadata. */

bool HybridStorageManager::CreateCollection(const std::string &name, const CollectionConfig &config)
{
     /* Check if collection already exists BEFORE creating */

     std::string meta_key = "collection_meta:" + name;

     if (Instance && Instance->Database)
     {
          /* Check if collection metadata already exists */

          std::string existing_meta = Instance->Database->Get(meta_key);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_CHECK] Database->Get() for key: " + meta_key + " returned: " + (existing_meta.empty() ? "NOT_EXISTS" : "EXISTS (len: " + std::to_string(existing_meta.length()) + ").") + ".");
          }

          if (!existing_meta.empty())
          {
               /* Collection already exists - reject duplicate */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "[COLLECTION_DUPLICATE_REJECTED] Collection with name '" + name + "' already exists (key: " + meta_key + ", meta_len: " + std::to_string(existing_meta.length()) + ") - REJECTING.");
               }
               return false;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_CHECK_PASS] Collection does not exist, proceeding with creation - name: " + name + ".");
          }
     }

     std::lock_guard<std::mutex> lock(CollectionsMutex);

     /*
           * Add collection to in-memory map FIRST before RocksDB operations.
           * This ensures ListCollections() will see it immediately, even if RocksDB operations are slow.
           * We hold the lock, so this is safe and atomic.
           */

     Collections[name] = config;

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("hybrid_storage", "[COLLECTION_CREATE] Added collection '" + name + "' to in-memory map (map size now: " + std::to_string(Collections.size()) + ".");
     }

     /* Store collection metadata in RocksDB */

     if (Instance && Instance->Database)
     {
          /* Store current timestamp in metadata (count:timestamp) */

          time_t now = time(nullptr);

          std::string meta_value = BuildCollectionMetaValue(0, now);
          const std::string config_key = GetCollectionConfigKey(name);
          const std::string config_value = SerializeCollectionConfig(config);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_INSERT] Calling Database->Set() for collection metadata - key: " + meta_key + ", value: " + meta_value + ".");
          }

          bool success = false;
          DBManager *db_engine = dynamic_cast<DBManager *>(Instance->Database.get());

          if (db_engine)
          {
               std::vector<std::pair<std::string, std::string>> metadata_batch;
               metadata_batch.reserve(2);
               metadata_batch.push_back({meta_key, meta_value});
               metadata_batch.push_back({config_key, config_value});
               success = (db_engine->BatchSet(metadata_batch) == metadata_batch.size());
          }
          else
          {
               success = Instance->Database->Set(meta_key, meta_value);

               if (success)
               {
                    success = Instance->Database->Set(config_key, config_value);
               }
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_INSERT_RESULT] Database->Set() returned: " + std::string(success ? "SUCCESS" : "FAILURE") + " for key: " + meta_key + ".");
          }

          /*
                * If RocksDB write failed, remove from in-memory map to maintain consistency.
                * The collection should only exist if it's in both places.
                */

          if (!success)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_ERROR] RocksDB write failed for collection '" + name + "' - removing from in-memory map.");
               }

               Collections.erase(name);
               return false;
          }

          /*
                * Verify collection is still in map after RocksDB operations.
                * This ensures it wasn't accidentally removed (we still hold the lock from above).
                */

          if (Collections.find(name) == Collections.end())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_ERROR] Collection '" + name + "' was removed from in-memory map after RocksDB write.");
               }

               /* Re-add it to ensure consistency */

               Collections[name] = config;
          }

          /* Collection created successfully */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_SUCCESS] Collection '" + name + "' created successfully.");
          }

          /*
                * Final verification - ensure collection is in map before returning.
                * This guarantees ListCollections() will see it immediately.
                * Double-check that collection is definitely in the map.
                */

          auto final_check = Collections.find(name);

          if (final_check == Collections.end())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_ERROR] Collection '" + name + "' not in map before return - re-adding.");
               }
               Collections[name] = config;
          }
          else
          {
               /* Verify the collection name matches (sanity check) */

               if (final_check->first != name)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_ERROR] Collection name mismatch in map: expected '" + name + "', found '" + final_check->first + "'.");
                    }
                    Collections[name] = config;
               }
          }

          /*
                * Final log to confirm collection is in map.
                * Use NORMAL log level so it's always visible (not just in debug mode).
                */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_VERIFY] Collection '" + name + "' verified in map before return (map size: " + std::to_string(Collections.size()) + ".");
          }

          /*
                * Double-check collection is actually in the map by iterating.
                * This ensures it's not just a map entry issue.
                * We still hold the lock, so this is safe.
                */

          bool found_in_iteration = false;

          for (const auto &pair : Collections)
          {
               if (pair.first == name)
               {
                    found_in_iteration = true;

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("hybrid_storage", "[COLLECTION_VERIFY] Collection '" + name + "' confirmed in map iteration.");
                    }
                    break;
               }
          }

          if (!found_in_iteration)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_CRITICAL] Collection '" + name + "' not found in map iteration - re-adding immediately.");
               }
               Collections[name] = config;

               /* Verify it was added - we still hold the lock so this should work */

               auto verify_add = Collections.find(name);

               if (verify_add == Collections.end())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_CRITICAL] Failed to add collection '" + name + "' to map even after re-adding! This should never happen.");
                    }

                    /* Last resort: try one more time */

                    Collections[name] = config;
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("hybrid_storage", "[COLLECTION_VERIFY] Collection '" + name + "' successfully re-added to map.");
                    }
               }
          }

          /*
           * FINAL CHECK: Before releasing the lock, verify collection is definitely in map.
           * This is the last chance to ensure it's there before ListCollections() can see it.
           */

          auto absolute_final_check = Collections.find(name);

          if (absolute_final_check == Collections.end() || absolute_final_check->first != name)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_CRITICAL] ABSOLUTE FINAL CHECK FAILED - collection '" + name + "' not in map! Re-adding one last time.");
               }
               Collections[name] = config;
          }

          /* Log final state before releasing lock */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_FINAL] Collection '" + name + "' confirmed in map (size: " + std::to_string(Collections.size()) + ") - releasing lock.");
          }

          return true;
     }

     /*
      * If Database is null, collection is still in in-memory map, which is fine for testing
      * but in production this should not happen.
      */

     if (!Instance || !Instance->Database)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[COLLECTION_SUCCESS] Collection '" + name + "' added to in-memory map (Database not available).");
          }

          /*
           * Verify collection is in map even when Database is null.
           * This ensures it's visible to ListCollections() immediately.
           */

          auto check = Collections.find(name);

          if (check == Collections.end())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[COLLECTION_CREATE_ERROR] Collection '" + name + "' not in map when Database is null - re-adding.");
               }
               Collections[name] = config;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "[COLLECTION_VERIFY] Collection '" + name + "' verified in map (Database null, map size: " + std::to_string(Collections.size()) + ".");
          }
     }

     return true;
}

/* DeleteCollection - Deletes a collection and all associated data. */

bool HybridStorageManager::DeleteCollection(const std::string &name)
{
     if (Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          std::string SAMCancelError;

          if (!Instance->Sam->CancelCollectionWork(name, &SAMCancelError) &&
              Instance->Logs && !SAMCancelError.empty())
          {
               Instance->Logs->Normal("hybrid_storage",
                                      "DeleteCollection: Failed to cancel SAM work for '" + name +
                                           "': " + SAMCancelError + ".");
          }
     }

     /*
      * Check if collection exists first (before acquiring lock to avoid deadlock).
      * Use a non-locking check by directly checking the in-memory map.
      */

     {
          std::lock_guard<std::mutex> check_lock(CollectionsMutex);

          if (Collections.find(name) == Collections.end())
          {
               /* Not in memory, check RocksDB */

               if (Instance && Instance->Database)
               {
                    std::string meta_key = "collection_meta:" + name;

                    std::string meta_value = Instance->Database->Get(meta_key);

                    if (meta_value.empty())
                    {
                         /* Collection doesn't exist */

                         return false;
                    }
               }
               else
               {
                    /* No database instance */

                    return false;
               }
          }
     }

     /* Now acquire lock for deletion */

     std::lock_guard<std::mutex> lock(CollectionsMutex);

     /* Verify collection still exists (could have been deleted by another thread) */

     if (Collections.find(name) == Collections.end())
     {
          /* Double-check in RocksDB */

          if (Instance && Instance->Database)
          {
               std::string meta_key = "collection_meta:" + name;

               std::string meta_value = Instance->Database->Get(meta_key);

               if (meta_value.empty())
               {
                    /* Collection was deleted by another thread */

                    return false;
               }
          }
          else
          {
               /* Collection doesn't exist */

               return false;
          }
     }

     /* Delete all documents in the collection before deleting metadata */

     /* This prevents document accumulation when collections are deleted and recreated */

     /* PERFORMANCE: Use DeleteRange for bulk deletion - much faster than Keys() + Del() loops */

     if (Instance && Instance->Database)
     {
          size_t deleted_count = 0;

          /* Use DeleteRange for efficient bulk deletion of documents */

          /* Range: "doc:name:" to "doc:name;" (semicolon is after colon in ASCII, covers all doc:name:* keys) */

          try
          {
               std::string doc_start = "doc:" + name + ":";
               std::string doc_end = "doc:" + name + ";";

               size_t delete_result = Instance->Database->DeleteRange(doc_start, doc_end);

               /*
                     * DeleteRange returns 1 on success, not the actual count.
                     * We can estimate count from metadata if needed, but deletion is what matters.
                     */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Deleted documents from collection '" + name + "' using DeleteRange (result: " + std::to_string(delete_result) + ").");
               }
          }
          catch (const std::bad_alloc &e)
          {
               /* Memory error - collection too large, use fallback with batching */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Out of memory using DeleteRange, using batched deletion: " + std::string(e.what()) + ".");
               }

               /* Fallback: Delete in batches to avoid memory issues */

               const size_t BatchSize = 1000;
               std::string pattern = "doc:" + name + ":*";

               /*
                * Fallback: Try to delete using iterator-based approach to avoid loading all keys.
                * If that also fails, log error but continue - collection metadata will be deleted anyway.
                */

               try
               {
                    /* Try to get count first to see if collection is manageable */

                    std::string prefix = "doc:" + name + ":";

                    size_t estimated_count = Instance->Database->CountKeys(prefix);

                    if (estimated_count > 100000)
                    {
                         /*
                          * Very large collection - warn but try to delete anyway using DeleteRange again.
                          * Sometimes DeleteRange works on retry.
                          */

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Collection '" + name + "' has " + std::to_string(estimated_count) + ".");
                         }

                         try
                         {
                              std::string doc_start = "doc:" + name + ":";
                              std::string doc_end = "doc:" + name + ";";

                              Instance->Database->DeleteRange(doc_start, doc_end);

                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Successfully deleted large collection '" + name + "' using DeleteRange (retry).");
                              }
                         }
                         catch (...)
                         {
                              /* If retry also fails, log critical error but continue */

                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Failed to delete very large collection '" + name + "' - documents may remain in database.");
                              }
                         }
                    }
                    else
                    {
                         /* Smaller collection - use batched deletion */

                         std::vector<std::string> doc_keys = Instance->Database->Keys(pattern);

                         for (size_t i = 0; i < doc_keys.size(); i += BatchSize)
                         {
                              size_t end = std::min(i + BatchSize, doc_keys.size());

                              for (size_t j = i; j < end; ++j)
                              {
                                   if (Instance->Database->Del(doc_keys[j]) > 0)
                                   {
                                        deleted_count++;
                                   }
                              }

                              /* Flush periodically to prevent memory buildup */

                              if (i % (BatchSize * 10) == 0)
                              {
                                   try
                                   {
                                        Instance->Database->Flush();
                                   }
                                   catch (...)
                                   {
                                        /* Continue even if flush fails */
                                   }
                              }
                         }

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Deleted " + std::to_string(deleted_count) + ".");
                         }
                    }
               }
               catch (const std::bad_alloc &e2)
               {
                    /* Even fallback failed with memory error - collection is too large */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Out of memory deleting collection '" + name + "' - collection may be too large. Documents may remain in database.");
                    }
               }
               catch (const std::exception &e2)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Failed to delete documents from collection '" + name + "': " + std::string(e2.what()) + ".");
                    }
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Unknown exception deleting documents from collection '" + name + "'.");
                    }
               }
          }
          catch (const std::exception &e)
          {
               /* Fallback to slower method if DeleteRange fails */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "DeleteCollection: DeleteRange failed, falling back to individual deletion: " + std::string(e.what()) + ".");
               }

               try
               {
                    std::string pattern = "doc:" + name + ":*";

                    std::vector<std::string> doc_keys = Instance->Database->Keys(pattern);

                    for (const auto &doc_key : doc_keys)
                    {
                         if (Instance->Database->Del(doc_key) > 0)
                         {
                              deleted_count++;
                         }
                    }
               }
               catch (const std::exception &e2)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Failed to delete documents (fallback method): " + std::string(e2.what()) + ".");
                    }
               }
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "DeleteCollection: Unknown exception during document deletion for collection '" + name + "'.");
               }
          }

          /* Use DeleteRange for efficient bulk deletion of index keys */

          try
          {
               std::string idx_start = "idx:" + name + ":";
               std::string idx_end = "idx:" + name + ";";

               Instance->Database->DeleteRange(idx_start, idx_end);
          }
          catch (const std::exception &e)
          {
               /* Fallback to slower method if DeleteRange fails */

               try
               {
                    std::string index_pattern = "idx:" + name + ":*";

                    std::vector<std::string> index_keys = Instance->Database->Keys(index_pattern);

                    for (const auto &idx_key : index_keys)
                    {
                         Instance->Database->Del(idx_key);
                    }
               }
               catch (...)
               {
                    /* Log but continue - index keys are less critical */

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("hybrid_storage", "DeleteCollection: Failed to delete index keys for collection '" + name + "'.");
                    }
               }
          }
          catch (...)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "DeleteCollection: Unknown exception deleting index keys for collection '" + name + "'.");
               }
          }

          /* Delete synonyms for this collection */

          try
          {
               std::string synonyms_key = "synonyms:" + name;

               Instance->Database->Del(synonyms_key);
          }
          catch (...)
          {
               /* Log but continue - synonyms are less critical */
          }

          /* Delete stopwords for this collection */

          try
          {
               std::string stopwords_key = "stopwords:" + name;

               Instance->Database->Del(stopwords_key);
          }
          catch (...)
          {
               /* Log but continue - stopwords are less critical */
          }

          /* Delete collection metadata from RocksDB */

          try
          {
               std::string meta_key = "collection_meta:" + name;

               Instance->Database->Del(meta_key);
               Instance->Database->Del(GetCollectionConfigKey(name));
          }
          catch (...)
          {
               /* Log but continue - metadata deletion failure is less critical */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "DeleteCollection: Failed to delete metadata for collection '" + name + "'.");
               }
          }

          /*
                * Flush to ensure deletions are persisted immediately.
                * This prevents documents from reappearing if collection is recreated quickly.
                */

          try
          {
               Instance->Database->Flush();
          }
          catch (...)
          {
               /* If flush fails, continue anyway - deletions should still be applied */
          }
     }

     /*
           * Remove collection from inverted index.
           * Do this AFTER deleting documents to ensure index is cleaned up properly.
           */

     try
     {
          Instance->SearchIndex->DeleteCollection(name);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "[INDEX_UPDATE] Removed collection '" + name + "' from inverted index.");
          }
     }
     catch (const std::exception &e)
     {
          /* Log error but don't fail collection deletion */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[INDEX_ERROR] Failed to remove collection '" + name + "' from inverted index: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          /* Unknown exception - log but continue */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "[INDEX_ERROR] Unknown exception removing collection '" + name + "' from inverted index.");
          }
     }

     /*
           * Remove mmap index files on disk for this collection to prevent stale indexes
           * from being reused if the collection is recreated.
           */

     try
     {
          std::string index_dir = Instance && Instance->SearchIndex
                                       ? Instance->SearchIndex->GetIndexDir()
                                       : (std::string(HLQUERY_DATA_DIR) + "/storage/indices");

          std::filesystem::path collection_index_dir = std::filesystem::path(index_dir) / name;

          if (std::filesystem::exists(collection_index_dir))
          {
               std::filesystem::remove_all(collection_index_dir);

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "DeleteCollection: Removed mmap index directory '" + collection_index_dir.string() + "'.");
               }
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "DeleteCollection: Error removing mmap index directory for '" + name + "': " + std::string(e.what()) + ".");
          }
     }

     if (Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          std::string SAMDeleteError;

          if (!Instance->Sam->DeleteCollection(name, &SAMDeleteError) &&
              Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage",
                                      "DeleteCollection: Failed to purge SAM state for '" + name +
                                           "': " +
                                           (SAMDeleteError.empty() ? std::string("unknown error") : SAMDeleteError) + ".");
          }
     }

     /* Remove from in-memory map */

     Collections.erase(name);

     return true;
}

/* CollectionExists - Checks whether a collection exists. */

bool HybridStorageManager::CollectionExists(const std::string &name)
{
     /* Check in-memory map first (fast path) */

     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          if (Collections.find(name) != Collections.end())
          {
               return true;
          }
     }

     /* If not in memory, check RocksDB directly */

     if (Instance && Instance->Database)
     {
          std::string meta_key = "collection_meta:" + name;

          std::string meta_value = Instance->Database->Get(meta_key);

          if (!meta_value.empty())
          {
               /* Found in RocksDB - add to in-memory map */

               std::lock_guard<std::mutex> lock(CollectionsMutex);

               if (Collections.find(name) == Collections.end())
               {
                    CollectionConfig config;

                    config.Name = name;
                    Collections[name] = config;
               }
               return true;
          }
     }

     return false;
}

/* ListCollections - Returns the list of collection names. */

std::vector<std::string> HybridStorageManager::ListCollections()
{
     /*
           * CRITICAL FIX: Check in-memory map FIRST to catch newly created collections
           * that might not be visible in RocksDB iterator yet (even after flush).
           * Then merge with RocksDB results to ensure consistency.
           */

     std::set<std::string> collection_set;

     /*
           * First, get collections from in-memory map (includes newly created ones).
           * This MUST be done first to ensure newly created collections are visible.
           * The in-memory map is the source of truth for recently created collections.
           */

     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          size_t map_size_before = collection_set.size();

          for (const auto &pair : Collections)
          {
               collection_set.insert(pair.first);
          }

          size_t map_size_after = collection_set.size();

          /* Log if we added any collections from in-memory map */

          if (map_size_after > map_size_before || Collections.size() > 0)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "[ListCollections] Added " + std::to_string(map_size_after - map_size_before) + ".");
               }
          }
     }

     /*
           * Then query RocksDB to get any collections that might be in DB but not in memory.
           * This ensures we return collections even if they weren't loaded into memory.
           * Query RocksDB AFTER checking in-memory map to ensure newly created
           * collections are visible immediately (they're in memory but might not be in RocksDB yet).
           */

     if (Instance && Instance->Database)
     {
          std::vector<std::string> keys = Instance->Database->Keys("collection_meta:*");

          /* Extract collection names from keys (format: "collection_meta:name") */

          for (const auto &key : keys)
          {
               size_t colon_pos = key.find(':');

               if (colon_pos != std::string::npos && colon_pos + 1 < key.size())
               {
                    std::string collection_name = key.substr(colon_pos + 1);

                    /* Insert will handle duplicates */

                    collection_set.insert(collection_name);
               }
          }

          /* Update in-memory map to keep it in sync with RocksDB */

          {
               std::lock_guard<std::mutex> lock(CollectionsMutex);

               for (const auto &name : collection_set)
               {
                    if (Collections.find(name) == Collections.end())
                    {
                         /* Create a basic collection config for collections found in RocksDB */

                         CollectionConfig config;

                         config.Name = name;
                         Collections[name] = config;
                    }
               }
          }
     }

     /* Return collections from set */

     std::vector<std::string> result;

     result.reserve(collection_set.size());

     for (const auto &name : collection_set)
     {
          result.push_back(name);
     }

     return result;
}

bool HybridStorageManager::GetCollectionConfig(const std::string &name, CollectionConfig &config)
{
     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          auto it = Collections.find(name);

          if (it != Collections.end())
          {
               config = it->second;
               return true;
          }
     }

     if (!Instance || !Instance->Database)
     {
          return false;
     }

     CollectionConfig loaded_config;
     loaded_config.Name = name;

     std::string raw_config = Instance->Database->Get(GetCollectionConfigKey(name));

     if (!raw_config.empty())
     {
          if (!DeserializeCollectionConfig(raw_config, loaded_config))
          {
               loaded_config.Name = name;
          }
     }

     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          auto it = Collections.find(name);

          if (it != Collections.end())
          {
               config = it->second;
               return true;
          }

          Collections[name] = loaded_config;
          config = loaded_config;
     }

     return true;
}

bool HybridStorageManager::UpdateCollectionMetadata(const std::string &name, const std::string &key, const std::string &value)
{
     if (key.empty() || key[0] != '_')
     {
          return false;
     }

     CollectionConfig config;

     if (!GetCollectionConfig(name, config))
     {
          return false;
     }

     config.Name = name;
     config.Metadata[key] = value;

     if (Instance && Instance->Database)
     {
          if (!Instance->Database->Set(GetCollectionConfigKey(name), SerializeCollectionConfig(config)))
          {
               return false;
          }
     }

     std::lock_guard<std::mutex> lock(CollectionsMutex);
     Collections[name] = config;

     return true;
}

/* AddDocument - Adds a document to storage and indexes. */

bool HybridStorageManager::AddDocument(const std::string &collection, const Document &doc)
{
     /* REMOVED: Verbose logging from hot path - use Debug mode if needed */

     if (!Instance || !Instance->Database)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "[STORAGE_ERROR] AddDocument: Instance or Database is null - REJECTING.");
          }
          return false;
     }

     /*
           * Check if document already exists - prevent duplicates.
           * Validate inputs first.
           */

     if (collection.empty() || doc.ID.empty())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "[STORAGE_VALIDATION_FAIL] AddDocument: Invalid input - collection='" + collection + "' doc.ID='" + doc.ID + "' - REJECTING.");
          }
          return false;
     }

     std::string doc_key = "doc:" + collection + ":" + doc.ID;

     /*
           * PERFORMANCE FIX: Use only per-key mutex - no global lock needed.
           * RocksDB operations are already thread-safe, and per-key mutex prevents
           * concurrent inserts of the same document. This eliminates global lock contention.
           */

     std::mutex &key_mutex = GetKeyMutex(doc_key);

     std::lock_guard<std::mutex> key_lock(key_mutex);

     /*
           * PERFORMANCE: Skip Exists() check - just call Set() directly (upsert behavior).
           * RocksDB Set() efficiently overwrites existing keys, so no need to check first.
           * This is fastest for benchmarks that reinsert documents - no duplicate check overhead.
           * FIX: Implement upsert behavior as comment suggests - allow overwriting existing documents.
           */

     /* Store document in RocksDB */

     /* Serialize document: id|title|content|fields_json|timestamp|score */

     /* Serialize fields map as JSON */

     nlohmann::json fields_json;

     for (const auto &[key, value] : doc.Fields)
     {
          fields_json[key] = value;
     }

     std::string fields_str = fields_json.dump();

     /* Use document timestamp if provided, otherwise use current time */

     uint64_t timestamp_to_store = doc.Timestamp;

     if (timestamp_to_store == 0)
     {
          /* Generate timestamp in milliseconds since epoch if not provided */

          if (Instance)
          {
               timestamp_to_store = Instance->NowMs();
          }
          else
          {
               timestamp_to_store = NowMs();
          }
     }

     std::string doc_data = doc.ID + "|" + doc.Title + "|" + doc.Content + "|" + fields_str + "|" + std::to_string(timestamp_to_store) + "|" + std::to_string(doc.Score);

     /* Check if document already exists to determine if this is a new document or update */

     bool document_existed = Instance->Database->Exists(doc_key);

     /* Upsert: Set() will overwrite if exists, insert if new */

     bool success = Instance->Database->Set(doc_key, doc_data);

     if (!success)
     {
          /* Set failed - log error and return false */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "[STORAGE_INSERT_FAIL] Database->Set() failed for document ID '" + doc.ID + "' in collection '" + collection + "' key: " + doc_key + ".");
          }
          return false;
     }

     /*
           * Update collection metadata counter after insert to ensure accuracy.
           * Only increment counter for new documents (not updates).
           * Use collection mutex to prevent race conditions when multiple threads
           * add different documents to the same collection concurrently.
           */

     if (success && Instance && Instance->Database && !document_existed)
     {
          /* Use collection mutex to protect counter update from race conditions */

          std::lock_guard<std::mutex> collection_lock(GetCollectionMutex(collection));

          std::string meta_key = "collection_meta:" + collection;

          std::string meta_value = Instance->Database->Get(meta_key);

          size_t current_count = 0;

          time_t timestamp = time(nullptr);

          /* Parse existing metadata if present */

          if (!meta_value.empty())
          {
               size_t colon_pos = meta_value.find(':');

               if (colon_pos != std::string::npos)
               {
                    current_count = std::stoull(meta_value.substr(0, colon_pos));

                    if (colon_pos + 1 < meta_value.size())
                    {
                         time_t parsed_ts = std::stoull(meta_value.substr(colon_pos + 1));

                         if (parsed_ts > 0)
                         {
                              timestamp = parsed_ts;
                         }
                    }
               }
          }

          /* Increment count for new documents (upsert: overwrites don't change count) */

          current_count++;

          /* Update metadata with accurate count */

          std::string new_meta_value = std::to_string(current_count) + ":" + std::to_string(timestamp);

          Instance->Database->Set(meta_key, new_meta_value);
     }

     /* If document existed, count stays the same (overwrite, not new document) */

     if (success)
     {
          /*
                * Index new document immediately so it's searchable right away.
                * This ensures synonyms work with newly added documents.
                * OPTIMIZATION: Skip immediate indexing for bulk operations - use lazy indexing instead.
                * Documents are stored and will be indexed on-demand via LazyLoadCollectionIndex.
                * This prevents blocking HTTP responses during bulk document insertion.
                * For single document inserts, immediate indexing is fast enough.
                */

          try
          {
               /*
                     * Only index immediately if collection is small (likely single document insert).
                     * For bulk operations, rely on lazy indexing.
                     */

               size_t doc_count = GetCollectionDocumentCount(collection);

               if (doc_count < 100)
               {
                    /* Small collection - index immediately (fast) */

                    Instance->SearchIndex->AddDocument(collection, doc);
               }
               else
               {
                    /*
                          * Large collection - skip immediate indexing to avoid blocking.
                          * Document will be indexed on next search via LazyLoadCollectionIndex.
                          */

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("hybrid_storage", "AddDocument: Skipping immediate indexing for collection '" + collection + "' with " + std::to_string(doc_count) + ".");
                    }
               }
          }
          catch (const std::exception &e)
          {
               /* Don't fail document addition if indexing fails - document is still stored */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "AddDocument: Failed to index document '" + doc.ID + "': " + e.what() + ".");
               }
          }

          if (Instance && Instance->Sam && Instance->Sam->IsOpen())
          {
               std::string sam_error;

               if (!Instance->Sam->EnqueueIndexDocument(collection, doc, &sam_error) &&
                   Instance->Logs)
               {
                    Instance->Logs->Normal("sam",
                                           "Failed to queue incremental SAM index for '" +
                                                collection + "/" + doc.ID + "': " +
                                                (sam_error.empty() ? std::string("unknown error") : sam_error) + ".");
               }
          }

          RefreshCollectionLanguageIfNeeded(collection, &doc);
     }

     /* Document write complete */

     return success;
}

/*
      * PERFORMANCE: Batch insert using RocksDB WriteBatch - MUCH faster than calling AddDocument() one-by-one.
      */

size_t HybridStorageManager::AddDocumentsBatch(const std::string &collection, const std::vector<Document> &documents, bool assume_new_documents)
{
     if (!Instance || !Instance->Database)
     {
          return 0;
     }

     if (collection.empty() || documents.empty())
     {
          return 0;
     }

     std::lock_guard<std::mutex> collection_lock(GetCollectionMutex(collection));

     /* Prepare all documents for batch write (upsert: includes both new and existing) */

     std::vector<std::pair<std::string, std::string>> batch_data;

     batch_data.reserve(documents.size());

     for (const auto &doc : documents)
     {
          if (doc.ID.empty())
          {
               /* Skip documents without IDs */

               continue;
          }

          std::string doc_key = "doc:" + collection + ":" + doc.ID;

          /* Serialize document: id|title|content|fields_json|timestamp|score */

          nlohmann::json fields_json;

          for (const auto &[key, value] : doc.Fields)
          {
               fields_json[key] = value;
          }

          std::string fields_str = fields_json.dump();

          /* Use document timestamp if provided, otherwise use current time */

          uint64_t timestamp_to_store = doc.Timestamp;

          if (timestamp_to_store == 0)
          {
               /* Generate timestamp in milliseconds since epoch if not provided */

               if (Instance)
               {
                    timestamp_to_store = Instance->NowMs();
               }
               else
               {
                    timestamp_to_store = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         Now().time_since_epoch()).count());
               }
          }

          std::string doc_data = doc.ID + "|" + doc.Title + "|" + doc.Content + "|" + fields_str + "|" + std::to_string(timestamp_to_store) + "|" + std::to_string(doc.Score);

          batch_data.push_back({doc_key, doc_data});
     }

     if (batch_data.empty())
     {
          /* All documents were invalid (no IDs) */

          return 0;
     }

     /*
      * Use batch write API for atomic multi-write - MUCH faster!
      * This writes all documents in one RocksDB WriteBatch operation instead of individual Set() calls.
      * Upsert behavior: overwrites existing documents, inserts new ones.
      */

     DBManager *db_engine = dynamic_cast<DBManager *>(Instance->Database.get());
     size_t count = 0;
     const size_t document_write_count = batch_data.size();

     /*
      * Update collection metadata counter after batch insert to ensure accuracy.
      * Count actual documents in the collection to get the true count (accounts for overwrites).
      * This is more accurate than incrementing because it handles edge cases correctly.
      * PERFORMANCE: Use CountKeys instead of Keys().size() to avoid loading all keys into memory.
      */

     if (Instance && Instance->Database)
     {
          /*
           * Keep collection counters crash-safe without rescanning the whole collection.
           * We estimate the delta by checking existence for keys in this batch while
           * holding the per-collection mutex, then persist the new metadata value in
           * the same write batch as the documents whenever the RocksDB engine is used.
           */

          std::string meta_key = "collection_meta:" + collection;
          std::string meta_value = Instance->Database->Get(meta_key);
          size_t current_count = 0;
          time_t timestamp = time(nullptr);
          ParseCollectionMetaValue(meta_value, &current_count, &timestamp);

          size_t inserted_count = 0;

          if (assume_new_documents)
          {
               inserted_count = document_write_count;
          }
          else
          {
               for (const auto &doc : documents)
               {
                    if (doc.ID.empty())
                    {
                         continue;
                    }

                    const std::string doc_key = "doc:" + collection + ":" + doc.ID;

                    if (!Instance->Database->Exists(doc_key))
                    {
                         inserted_count++;
                    }
               }
          }

          const size_t new_count = current_count + inserted_count;
          const std::string new_meta_value = BuildCollectionMetaValue(new_count, timestamp);

          if (db_engine)
          {
               batch_data.push_back({meta_key, new_meta_value});
               const size_t written = db_engine->BatchSet(batch_data);
               count = (written == batch_data.size()) ? document_write_count : 0;
          }
          else
          {
               /* Fallback path keeps metadata aligned even without batched RocksDB writes. */

               count = 0;

               for (const auto &[doc_key, doc_data] : batch_data)
               {
                    if (Instance->Database->Set(doc_key, doc_data))
                    {
                         count++;
                    }
               }

               if (count == document_write_count && !Instance->Database->Set(meta_key, new_meta_value))
               {
                    count = 0;
               }
          }
     }

     if (count > 0 && Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          for (const auto &doc : documents)
          {
               if (doc.ID.empty())
               {
                    continue;
               }

               Document stored_doc = GetDocument(collection, doc.ID);

               if (stored_doc.ID.empty())
               {
                    continue;
               }

               std::string sam_error;

               if (!Instance->Sam->EnqueueIndexDocument(collection, stored_doc, &sam_error) &&
                   Instance->Logs)
               {
                    Instance->Logs->Normal("sam",
                                           "Failed to queue batch SAM index for '" +
                                                collection + "/" + stored_doc.ID + "': " +
                                                (sam_error.empty() ? std::string("unknown error") : sam_error) + ".");
               }
          }
     }

     if (count > 0)
     {
          RefreshCollectionLanguageIfNeeded(collection, nullptr);
     }

     /* Batch write complete */

     /* Return total number of documents written (both new and updated) */

     return count;
}

/* GetDocument - Retrieves a document by collection and id. */

Document HybridStorageManager::GetDocument(const std::string &collection, const std::string &document_id)
{
     Document doc;

     if (!Instance || !Instance->Database)
     {
          return doc;
     }

     std::string doc_key = "doc:" + collection + ":" + document_id;

     std::string doc_data = Instance->Database->Get(doc_key);

     if (!doc_data.empty())
     {
          const auto validation = ValidateWALEntrySize(doc_key.size(), doc_data.size(), true);

          if (!validation.Valid)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "GetDocument skipped oversized persisted entry during decode: " + WALEntryValidationMessage(validation) + ".");
               }

               return Document();
          }

          /*
                * Parse document: id|title|content|fields_json|timestamp|score (new format).
                * Backward compatible with: id|title|content|fields_json|timestamp.
                * Backward compatible with: id|title|content|fields_json (old format without timestamp).
                * Backward compatible with: id|title|content (very old format without fields).
                */

          size_t pos1 = doc_data.find('|');

          if (pos1 != std::string::npos)
          {
               doc.ID = doc_data.substr(0, pos1);

               size_t pos2 = doc_data.find('|', pos1 + 1);

               if (pos2 != std::string::npos)
               {
                    doc.Title = doc_data.substr(pos1 + 1, pos2 - pos1 - 1);

                    size_t pos3 = doc_data.find('|', pos2 + 1);

                    if (pos3 != std::string::npos)
                    {
                         doc.Content = doc_data.substr(pos2 + 1, pos3 - pos2 - 1);

                         /* Parse fields JSON */

                         size_t pos4 = doc_data.find('|', pos3 + 1);

                         if (pos4 != std::string::npos)
                         {
                              /* New format with timestamp and optional score metadata. */

                              std::string fields_str = doc_data.substr(pos3 + 1, pos4 - pos3 - 1);
                              size_t pos5 = doc_data.find('|', pos4 + 1);
                              std::string timestamp_str = (pos5 == std::string::npos)
                                                              ? doc_data.substr(pos4 + 1)
                                                              : doc_data.substr(pos4 + 1, pos5 - pos4 - 1);
                              std::string score_str = (pos5 == std::string::npos) ? "" : doc_data.substr(pos5 + 1);

                              /* Parse fields JSON */

                              if (!fields_str.empty())
                              {
                                   try
                                   {
                                        nlohmann::json fields_json = nlohmann::json::parse(fields_str);

                                        for (const auto &[key, value] : fields_json.items())
                                        {
                                             if (value.is_string())
                                             {
                                                  doc.Fields[key] = value.get<std::string>();
                                             }
                                             else
                                             {
                                                  doc.Fields[key] = value.dump();
                                             }
                                        }
                                   }
                                   catch (const std::exception &e)
                                   {
                                        /* If JSON parsing fails, fields remain empty */

                                        if (Instance && Instance->Logs)
                                        {
                                             Instance->Logs->Normal("hybrid_storage", "Failed to parse fields JSON for document " + doc.ID + ": " + e.what() + ".");
                                        }
                                   }
                              }

                              /* Parse timestamp */

                              if (!timestamp_str.empty())
                              {
                                   try
                                   {
                                        doc.Timestamp = std::stoull(timestamp_str);
                                   }
                                   catch (const std::exception &e)
                                   {
                                        /*
* If timestamp parsing fails, use 0 (will be set to current time on next update).
*/

                                        if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                        {
                                             Instance->Logs->Debug("hybrid_storage", "Failed to parse timestamp for document " + doc.ID + ": " + e.what() + ".");
                                        }
                                        doc.Timestamp = 0;
                                   }
                              }

                              if (!score_str.empty())
                              {
                                   try
                                   {
                                        doc.Score = std::stod(score_str);
                                   }
                                   catch (const std::exception &e)
                                   {
                                        if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                        {
                                             Instance->Logs->Debug("hybrid_storage", "Failed to parse score for document " + doc.ID + ": " + e.what() + ".");
                                        }
                                        doc.Score = 0.0;
                                   }
                              }
                         }
                         else
                         {
                              /* Old format without timestamp: id|title|content|fields_json */

                              std::string fields_str = doc_data.substr(pos3 + 1);

                              if (!fields_str.empty())
                              {
                                   try
                                   {
                                        nlohmann::json fields_json = nlohmann::json::parse(fields_str);

                                        for (const auto &[key, value] : fields_json.items())
                                        {
                                             if (value.is_string())
                                             {
                                                  doc.Fields[key] = value.get<std::string>();
                                             }
                                             else
                                             {
                                                  doc.Fields[key] = value.dump();
                                             }
                                        }
                                   }
                                   catch (const std::exception &e)
                                   {
                                        /* If JSON parsing fails, fields remain empty */

                                        if (Instance && Instance->Logs)
                                        {
                                             Instance->Logs->Normal("hybrid_storage", "Failed to parse fields JSON for document " + doc.ID + ": " + e.what() + ".");
                                        }
                                   }
                              }

                              /* No timestamp in old format - will be set to current time on next update */

                              doc.Timestamp = 0;
                         }
                    }
                    else
                    {
                         /* Very old format without fields: id|title|content */

                         doc.Content = doc_data.substr(pos2 + 1);
                         doc.Timestamp = 0;
                    }
               }
          }
     }

     /*
           * LAZY MIGRATION: If document has no timestamp (old format), update it with current time.
           * This ensures old documents get timestamps on first access after code update.
           */

     if (!doc.ID.empty() && doc.Timestamp == 0 && Instance && Instance->Database)
     {
          /* Use per-key mutex to prevent race conditions during migration */

          std::mutex &key_mutex = GetKeyMutex(doc_key);

          std::lock_guard<std::mutex> key_lock(key_mutex);

          /* Double-check: another thread might have migrated it already */

          std::string recheck_data = Instance->Database->Get(doc_key);

          if (!recheck_data.empty())
          {
               /* Check if it still needs migration (parse just the timestamp part) */

               size_t last_pipe = recheck_data.find_last_of('|');

               if (last_pipe != std::string::npos)
               {
                    std::string last_part = recheck_data.substr(last_pipe + 1);

                    try
                    {
                         uint64_t existing_timestamp = std::stoull(last_part);

                         if (existing_timestamp > 0)
                         {
                              /* Already migrated by another thread */

                              doc.Timestamp = existing_timestamp;
                         }
                    }
                    catch (...)
                    {
                         /* Not a timestamp, needs migration */
                    }
               }
          }

          /* If still needs migration, update the document */

          if (doc.Timestamp == 0)
          {
               /* Generate current timestamp */

               if (Instance)
               {
                    doc.Timestamp = Instance->NowMs();
               }
               else
               {
                    doc.Timestamp = NowMs();
               }

               /* Re-serialize document with new timestamp */

               nlohmann::json fields_json;

               for (const auto &[key, value] : doc.Fields)
               {
                    fields_json[key] = value;
               }

               std::string fields_str = fields_json.dump();

               std::string updated_doc_data = doc.ID + "|" + doc.Title + "|" + doc.Content + "|" + fields_str + "|" + std::to_string(doc.Timestamp);

               /* Update in storage (this is a one-time migration per document) */

               Instance->Database->Set(doc_key, updated_doc_data);

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "Migrated old document " + doc.ID + " with timestamp " + std::to_string(doc.Timestamp) + ".");
               }
          }
     }

     return doc;
}

/* ListDocuments - Lists documents in a collection with pagination. */

std::vector<Document> HybridStorageManager::ListDocuments(const std::string &collection, int limit, int offset)
{
     std::vector<Document> result;

     if (!Instance || !Instance->Database)
     {
          return result;
     }

     /* PERFORMANCE: For large collections, use iterator-based approach to avoid loading all keys */

     /* However, for small collections or when offset is small, Keys() is acceptable */

     try
     {
          std::string pattern = "doc:" + collection + ":*";

          std::vector<std::string> keys = Instance->Database->Keys(pattern);

          /* Apply offset and limit */

          int count = 0;

          for (size_t i = offset; i < keys.size() && count < limit; ++i)
          {
               std::string doc_id = keys[i].substr(keys[i].find_last_of(':') + 1);

               Document doc = GetDocument(collection, doc_id);

               if (!doc.ID.empty())
               {
                    result.push_back(doc);
                    count++;
               }
          }
     }
     catch (const std::bad_alloc &e)
     {
          /* Memory error - log and return empty result */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "ListDocuments: Out of memory listing documents for collection '" + collection + "': " + std::string(e.what()) + ".");
          }
          return result;
     }
     catch (const std::exception &e)
     {
          /* Other errors - log and return empty result */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "ListDocuments: Exception listing documents for collection '" + collection + "': " + std::string(e.what()) + ".");
          }
          return result;
     }
     catch (...)
     {
          /* Unknown errors - log and return empty result */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "ListDocuments: Unknown exception listing documents for collection '" + collection + "'.");
          }
          return result;
     }

     return result;
}

/* DeleteDocument - Deletes a document and updates indexes. */

bool HybridStorageManager::DeleteDocument(const std::string &collection, const std::string &document_id)
{
     if (!Instance || !Instance->Database)
     {
          return false;
     }

     std::string doc_key = "doc:" + collection + ":" + document_id;

     /*
           * Use per-key mutex to prevent concurrent access.
           * RocksDB operations are thread-safe, but this ensures consistency
           * with AddDocument and prevents race conditions during deletion.
           */

     std::mutex &key_mutex = GetKeyMutex(doc_key);

     std::lock_guard<std::mutex> key_lock(key_mutex);

     /* Check if document exists before deleting to return accurate success/fail */

     if (!Instance->Database->Exists(doc_key))
     {
          return false;
     }

     bool deleted = Instance->Database->Del(doc_key) > 0;

     /* Ensure index and counter are updated atomically with storage deletion */

     if (deleted)
     {
          bool partial_cleanup_failed = false;

          try
          {
               Instance->SearchIndex->DeleteDocument(collection, document_id);

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "[INDEX_UPDATE] Removed document '" + document_id + "' from inverted index.");
               }
          }
          catch (const std::exception &e)
          {
               /*
                     * If index deletion fails, we can't rollback storage deletion.
                     * But we should log it as a consistency issue.
                     */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("hybrid_storage", "[INDEX_ERROR] Failed to remove document '" + document_id + "' from inverted index (storage already deleted): " + std::string(e.what()) + ".");
               }

               partial_cleanup_failed = true;
          }

          if (Instance && Instance->Sam && Instance->Sam->IsOpen())
          {
               std::string sam_error;

               if (!Instance->Sam->DeleteDocument(collection, document_id, &sam_error))
               {
                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("sam",
                                                "Failed to remove SAM terms for '" + collection + "/" +
                                                     document_id + "': " +
                                                     (sam_error.empty() ? std::string("unknown error") : sam_error) + ".");
                    }

                    partial_cleanup_failed = true;
               }
          }

          /*
                * Update collection metadata counter after delete to ensure accuracy.
                * Use collection mutex to prevent race conditions.
                */

          if (Instance && Instance->Database)
          {
               std::lock_guard<std::mutex> collection_lock(GetCollectionMutex(collection));

               std::string meta_key = "collection_meta:" + collection;

               std::string meta_value = Instance->Database->Get(meta_key);

               size_t current_count = 0;

               time_t timestamp = time(nullptr);

               if (!meta_value.empty())
               {
                    size_t colon_pos = meta_value.find(':');

                    if (colon_pos != std::string::npos)
                    {
                         current_count = std::stoull(meta_value.substr(0, colon_pos));

                         if (colon_pos + 1 < meta_value.size())
                         {
                              time_t parsed_ts = std::stoull(meta_value.substr(colon_pos + 1));

                              if (parsed_ts > 0)
                              {
                                   timestamp = parsed_ts;
                              }
                         }
                    }
               }

               if (current_count > 0)
               {
                    current_count--;

                    std::string new_meta_value = std::to_string(current_count) + ":" + std::to_string(timestamp);

                    Instance->Database->Set(meta_key, new_meta_value);
               }
          }

          if (partial_cleanup_failed)
          {
               const uint64_t FailureCount = PostDeleteCleanupFailures.fetch_add(1, std::memory_order_relaxed) + 1;

               if (Instance)
               {
                    Instance->StatsVal.SetHealthDegraded(true, "Post-delete cleanup drift detected after storage removal");

                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("hybrid_storage",
                                                "[CONSISTENCY_WARNING] Document '" + document_id +
                                                     "' was deleted from storage but secondary cleanup was incomplete (total failures: " +
                                                     std::to_string(FailureCount) + ").");
                    }
               }
          }
     }

     return deleted;
}

/* UpdateDocument - Updates a document with rollback on failure. */

bool HybridStorageManager::UpdateDocument(const std::string &collection, const Document &new_doc)
{
     /*
           * Atomic update with rollback - if update fails, restore old document.
           * Use per-key mutex to prevent concurrent update race conditions.
           */

     if (!Instance || !Instance->Database)
     {
          return false;
     }

     if (collection.empty() || new_doc.ID.empty())
     {
          return false;
     }

     std::string doc_key = "doc:" + collection + ":" + new_doc.ID;

     /* Use per-key mutex to prevent concurrent updates */

     std::mutex &key_mutex = GetKeyMutex(doc_key);

     std::unique_lock<std::mutex> key_lock(key_mutex);

     /* Get old document for rollback */

     Document old_doc = GetDocument(collection, new_doc.ID);

     if (old_doc.ID.empty())
     {
          /* Avoid re-entering AddDocument() while holding the same per-key mutex. */

          key_lock.unlock();
          return AddDocument(collection, new_doc);
     }

     /* Serialize new document */

     nlohmann::json fields_json;

     for (const auto &[key, value] : new_doc.Fields)
     {
          fields_json[key] = value;
     }

     std::string fields_str = fields_json.dump();

     /*
           * Preserve old timestamp if new document doesn't have one, otherwise use new timestamp.
           */

     uint64_t timestamp_to_store = new_doc.Timestamp;

     if (timestamp_to_store == 0)
     {
          /* Preserve old document's timestamp if it exists */

          timestamp_to_store = old_doc.Timestamp;

          if (timestamp_to_store == 0)
          {
               /* Neither has timestamp, use current time */

               if (Instance)
               {
                    timestamp_to_store = Instance->NowMs();
               }
               else
               {
                    timestamp_to_store = NowMs();
               }
          }
     }

     std::string new_doc_data = new_doc.ID + "|" + new_doc.Title + "|" + new_doc.Content + "|" + fields_str + "|" + std::to_string(timestamp_to_store) + "|" + std::to_string(new_doc.Score);

     /* Update storage first */

     bool storage_success = Instance->Database->Set(doc_key, new_doc_data);

     if (!storage_success)
     {
          return false;
     }

     /* Update inverted index - if this fails, rollback storage */

     bool index_success = false;

     try
     {
          Instance->SearchIndex->UpdateDocument(collection, old_doc, new_doc);
          index_success = true;
     }
     catch (const std::exception &e)
     {
          /* Rollback storage if index update fails */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "[UPDATE_ROLLBACK] Index update failed, rolling back storage: " + std::string(e.what()) + ".");
          }

          /* Rollback: restore old document */

          nlohmann::json old_fields_json;

          for (const auto &[key, value] : old_doc.Fields)
          {
               old_fields_json[key] = value;
          }

          std::string old_fields_str = old_fields_json.dump();

          /* Preserve old timestamp or use current time if not set */

          uint64_t old_timestamp = old_doc.Timestamp;

          if (old_timestamp == 0)
          {
               if (Instance)
               {
                    old_timestamp = Instance->NowMs();
               }
               else
               {
                    old_timestamp = NowMs();
               }
          }

          std::string old_doc_data = old_doc.ID + "|" + old_doc.Title + "|" + old_doc.Content + "|" + old_fields_str + "|" + std::to_string(old_timestamp) + "|" + std::to_string(old_doc.Score);

          Instance->Database->Set(doc_key, old_doc_data);

          return false;
     }

     if (index_success && Instance && Instance->Logs)
     {
          Instance->Logs->Debug("hybrid_storage", "[UPDATE_SUCCESS] Updated document '" + new_doc.ID + "' in collection '" + collection + "'.");
     }

     if (index_success && Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          std::string sam_error;

          if (!Instance->Sam->EnqueueIndexDocument(collection, new_doc, &sam_error) &&
              Instance->Logs)
          {
               Instance->Logs->Normal("sam",
                                      "Failed to queue incremental SAM update for '" +
                                           collection + "/" + new_doc.ID + "': " +
                                           (sam_error.empty() ? std::string("unknown error") : sam_error) + ".");
          }
     }

     /*
           * Invalidate cache after successful update.
           * This ensures search results reflect updated document content and scores.
           */

     if (index_success)
     {
          try
          {
               Instance->SearchIndex->InvalidateDocumentCache(collection, new_doc.ID);
          }
          catch (const std::exception &e)
          {
               /* Log but don't fail - cache invalidation is best-effort */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "Failed to invalidate cache for document '" + new_doc.ID + "': " + std::string(e.what()) + ".");
               }
          }
     }

     return index_success;
}

/* GetCollectionDocumentCount - Returns the document count for a collection. */

size_t HybridStorageManager::GetCollectionDocumentCount(const std::string &collection)
{
     if (!Instance || !Instance->Database)
     {
          return 0;
     }

     /* Get count from metadata */

     std::string meta_key = "collection_meta:" + collection;

     std::string meta_value = Instance->Database->Get(meta_key);

     size_t metadata_count = 0;

     if (!meta_value.empty())
     {
          size_t colon_pos = meta_value.find(':');

          if (colon_pos != std::string::npos)
          {
               metadata_count = std::stoull(meta_value.substr(0, colon_pos));
          }
     }

     /*
           * FIX: Don't auto-update metadata during reads - this causes counter bugs after restart.
           * Only UpdateCollectionCounters() should update metadata, not GetCollectionDocumentCount().
           * If metadata is 0, return 0 - don't count and update (that's what UpdateCollectionCounters is for).
           */

     return metadata_count;
}

size_t HybridStorageManager::CountStoredDocuments(const std::string &collection)
{
     if (!Instance || !Instance->Database)
     {
          return 0;
     }

     return Instance->Database->CountKeys("doc:" + collection + ":");
}

CollectionIntegrityStatus HybridStorageManager::CheckCollectionIntegrity(const std::string &collection)
{
     CollectionIntegrityStatus status;
     status.Collection = collection;
     status.CollectionExists = CollectionExists(collection);

     if (!status.CollectionExists)
     {
          status.Error = "Collection not found.";
          return status;
     }

     status.MetadataCount = GetCollectionDocumentCount(collection);
     status.ActualCount = CountStoredDocuments(collection);
     status.MetadataMatch = (status.MetadataCount == status.ActualCount);

     if (Instance && Instance->SearchIndex)
     {
          const bool has_in_memory = Instance->SearchIndex->HasInMemoryIndex(collection);
          const bool has_mmap = Instance->SearchIndex->HasMMapIndex(collection);

          status.IndexPresent = has_in_memory || has_mmap;
          status.IndexVerified = has_in_memory;

          if (status.IndexPresent)
          {
               status.IndexedCount = Instance->SearchIndex->GetDocumentCount(collection);
          }

          if (status.IndexVerified)
          {
               status.IndexMatch = (status.IndexedCount == status.ActualCount);
          }
          else if (has_mmap)
          {
               status.Error = "Persisted mmap index is present but cannot be fully verified without rebuilding it in memory.";
          }
     }

     return status;
}

IntegrityReport HybridStorageManager::CheckIntegrity(const std::string &collection)
{
     IntegrityReport report;
     std::vector<std::string> target_collections;

     if (!collection.empty())
     {
          target_collections.push_back(collection);
     }
     else
     {
          target_collections = ListCollections();
     }

     for (const auto &name : target_collections)
     {
          CollectionIntegrityStatus status = CheckCollectionIntegrity(name);

          if (!status.CollectionExists)
          {
               report.Success = false;
          }

          if (!status.MetadataMatch)
          {
               report.CounterMismatches++;
          }

          if (status.IndexVerified && !status.IndexMatch)
          {
               report.IndexMismatches++;
          }

          report.Collections.push_back(std::move(status));
     }

     report.CollectionsScanned = report.Collections.size();
     return report;
}

bool HybridStorageManager::RebuildCollectionIndex(const std::string &collection, size_t *reindexed_documents, std::string *error_message)
{
     if (reindexed_documents)
     {
          *reindexed_documents = 0;
     }

     if (error_message)
     {
          error_message->clear();
     }

     if (!Instance || !Instance->Database || !Instance->SearchIndex)
     {
          if (error_message)
          {
               *error_message = "Storage manager is not initialized.";
          }

          return false;
     }

     if (!CollectionExists(collection))
     {
          if (error_message)
          {
               *error_message = "Collection not found.";
          }

          return false;
     }

     if (IsCollectionIndexing(collection))
     {
          if (error_message)
          {
               *error_message = "Collection is already being indexed.";
          }

          return false;
     }

     std::lock_guard<std::mutex> collection_lock(GetCollectionMutex(collection));

     const std::vector<std::string> doc_keys = Instance->Database->Keys("doc:" + collection + ":*");
     size_t indexed_documents = 0;

     try
     {
          Instance->SearchIndex->DeleteCollection(collection);

          for (size_t i = 0; i < doc_keys.size(); ++i)
          {
               const std::string doc_id = ExtractDocumentIDFromKey(doc_keys[i]);

               if (doc_id.empty())
               {
                    continue;
               }

               const Document doc = GetDocument(collection, doc_id);

               if (doc.ID.empty())
               {
                    continue;
               }

               if (!Instance->SearchIndex->AddDocument(collection, doc))
               {
                    if (error_message)
                    {
                         *error_message = "Inverted index rejected a document while rebuilding '" + collection + "'.";
                    }

                    return false;
               }

               indexed_documents++;

               if (i > 0 && (i % 256) == 0)
               {
                    std::this_thread::yield();
               }
          }

          Instance->SearchIndex->FlushToDisk(ResolveIndexDir());
     }
     catch (const std::exception &e)
     {
          if (error_message)
          {
               *error_message = e.what();
          }

          return false;
     }
     catch (...)
     {
          if (error_message)
          {
               *error_message = "Unknown error while rebuilding index.";
          }

          return false;
     }

     if (reindexed_documents)
     {
          *reindexed_documents = indexed_documents;
     }

     return true;
}

CollectionIntegrityStatus HybridStorageManager::RepairCollection(const std::string &collection, bool rebuild_index)
{
     CollectionIntegrityStatus status = CheckCollectionIntegrity(collection);

     if (!status.CollectionExists)
     {
          return status;
     }

     {
          std::lock_guard<std::mutex> collection_lock(GetCollectionMutex(collection));

          time_t timestamp = time(nullptr);
          ParseCollectionMetaValue(Instance->Database->Get("collection_meta:" + collection), nullptr, &timestamp);
          Instance->Database->Set("collection_meta:" + collection, BuildCollectionMetaValue(status.ActualCount, timestamp));
     }

     status.MetadataCount = status.ActualCount;
     status.MetadataMatch = true;

     if (rebuild_index)
     {
          std::string rebuild_error;

          if (!RebuildCollectionIndex(collection, &status.ReindexedDocuments, &rebuild_error))
          {
               status.Error = rebuild_error;
               return status;
          }

          status.IndexRebuilt = true;
          status.IndexPresent = true;
          status.IndexVerified = true;
          status.IndexedCount = Instance->SearchIndex->GetDocumentCount(collection);
          status.IndexMatch = (status.IndexedCount == status.ActualCount);
     }

     return status;
}

IntegrityReport HybridStorageManager::RepairIntegrity(const std::string &collection, bool rebuild_index)
{
     IntegrityReport report;
     report.RebuildIndex = rebuild_index;

     std::vector<std::string> target_collections;

     if (!collection.empty())
     {
          target_collections.push_back(collection);
     }
     else
     {
          target_collections = ListCollections();
     }

     for (const auto &name : target_collections)
     {
          CollectionIntegrityStatus before = CheckCollectionIntegrity(name);

          if (!before.MetadataMatch)
          {
               report.CounterMismatches++;
          }

          if (before.IndexVerified && !before.IndexMatch)
          {
               report.IndexMismatches++;
          }

          CollectionIntegrityStatus repaired = RepairCollection(name, rebuild_index);

          if (!repaired.CollectionExists || !repaired.Error.empty() || !repaired.MetadataMatch || (rebuild_index && !repaired.IndexMatch))
          {
               report.Success = false;
          }

          if (!before.MetadataMatch || (rebuild_index && repaired.IndexRebuilt))
          {
               report.CollectionsRepaired++;
          }

          report.Collections.push_back(std::move(repaired));
     }

     report.CollectionsScanned = report.Collections.size();
     return report;
}

/* GetCollectionSize - Returns the total size of a collection. */

size_t HybridStorageManager::GetCollectionSize(const std::string &collection)
{
     if (!Instance || !Instance->Database)
     {
          return 0;
     }

     size_t total_size = 0;

     try
     {
          /* Get total size of all documents for this collection */

          std::string doc_prefix = "doc:" + collection + ":";

          total_size += Instance->Database->GetPrefixTotalSize(doc_prefix);

          /* Also include index size if available */

          std::string idx_prefix = "idx:" + collection + ":";

          total_size += Instance->Database->GetPrefixTotalSize(idx_prefix);
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "GetCollectionSize: Error calculating size for collection '" + collection + "': " + e.what() + ".");
          }
          return 0;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "GetCollectionSize: Unknown error calculating size for collection '" + collection + "'.");
          }
          return 0;
     }

     return total_size;
}

/* SaveDataToDisk - Flushes metadata and indexes to disk. */

void HybridStorageManager::SaveDataToDisk()
{
     PersistStorageState(true, true, true);
}

/* StartBackgroundFlushThread - Starts the background flush thread. */

void HybridStorageManager::StartBackgroundFlushThread()
{
     if (FlushThreadRunning.load())
     {
          /* Already running */

          return;
     }

     if (!ThreadLimit::TryAcquireThreadSlot())
     {
          FlushThreadRunning.store(false);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "Skipping background index flush thread because max_threads has been reached.");
          }

          return;
     }

     FlushThreadRegistered = true;
     FlushThreadRunning.store(true);
     FlushThread = std::thread([this]()

                               {
                                    ThreadLimit::SetThreadName("hlquery:flush");
                                    BackgroundFlushThread();
                               });

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "Started background index flush thread.");
     }
}

/* BackgroundFlushThread - Runs periodic background flush work. */

void HybridStorageManager::BackgroundFlushThread()
{
     /* Flush every 5 minutes */

     const int FlushIntervalSeconds = 300;

     while (FlushThreadRunning.load())
     {
          std::unique_lock<std::mutex> lock(FlushThreadMutex);
          FlushThreadCV.wait_for(lock, std::chrono::seconds(FlushIntervalSeconds), [this]()
                                 {
                                      return !FlushThreadRunning.load();
                                 });
          lock.unlock();

          if (!FlushThreadRunning.load())
          {
               break;
          }

          try
          {
               FlushIndexesToDisk();

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "Background flush: Indexes flushed to disk.");
               }
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "Background flush error: " + std::string(e.what()) + ".");
               }
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("hybrid_storage", "Background flush: Unknown error.");
               }
          }
     }
}

/* LoadCollectionsFromRocksDB - Loads collection metadata from RocksDB. */

bool HybridStorageManager::LoadCollectionsFromRocksDB()
{
     if (!Instance || !Instance->Database)
     {
          return false;
     }

     std::vector<std::string> keys = Instance->Database->Keys("collection_meta:*");

     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          for (const auto &key : keys)
          {
               /* Extract collection name from key (collection_meta:name) */

               size_t colon_pos = key.find(':');

               if (colon_pos != std::string::npos && colon_pos + 1 < key.size())
               {
                    std::string collection_name = key.substr(colon_pos + 1);

                    CollectionConfig config;
                    config.Name = collection_name;
                    const std::string raw_config = Instance->Database->Get(GetCollectionConfigKey(collection_name));

                    if (!raw_config.empty())
                    {
                         if (!DeserializeCollectionConfig(raw_config, config))
                         {
                              config = CollectionConfig();
                              config.Name = collection_name;
                         }
                    }

                    Collections[collection_name] = config;
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "LoadCollectionsFromRocksDB: Loaded " + std::to_string(keys.size()) + " collection metadata (lazy loading enabled - indexes will be built on-demand).");
     }

     /* Load mmap indexes from disk (if available) */

     Instance->SearchIndex->LoadFromDisk(ResolveIndexDir());

     /*
           * Update collection document counters to ensure metadata is accurate.
           * This counts actual documents in RocksDB and updates collection_meta:* counters.
           * This is essential since we no longer rebuild indexes at startup.
           * NOTE: Lock is released above, so UpdateCollectionCounters() can safely call ListCollections().
           */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "LoadCollectionsFromRocksDB: Updating collection document counters.");
     }

     /* force=true to update all counters */

     UpdateCollectionCounters(true);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "LoadCollectionsFromRocksDB: Collection counters updated successfully.");
     }

     return true;
}

/* IsMetadataScanComplete - Reports whether metadata scan is finished. */

bool HybridStorageManager::IsMetadataScanComplete() const
{
     /* For RocksDB, metadata scan is immediate (no async scan needed) */

     return true;
}

/* Helper function to perform actual indexing work in background thread */

void HybridStorageManager::IndexCollectionInBackground(const std::string &collection, const std::vector<std::string> &doc_keys)
{
     /* RAII guard to ensure cleanup on all exit paths */

     class IndexingGuard
     {
        private:
          std::unordered_set<std::string> &set_ref;
          std::mutex &mutex;
          const std::string &Collection;
          bool should_remove;

        public:
          /* IndexingGuard - Initializes RAII guard state. */

          IndexingGuard(std::unordered_set<std::string> &s, std::mutex &m, const std::string &coll)

              : set_ref(s), mutex(m), Collection(coll), should_remove(false)
          {
          }

          /* IndexingGuard::~IndexingGuard - Cleans up indexing guard state. */

          ~IndexingGuard()
          {
               if (should_remove)
               {
                    std::lock_guard<std::mutex> lock(mutex);

                    set_ref.erase(Collection);
               }
          }

          /* try_acquire - Attempts to reserve a collection for indexing. */

          bool try_acquire()
          {
               std::lock_guard<std::mutex> lock(mutex);
               if (set_ref.find(Collection) != set_ref.end())
               {
                    return false;
               }

               set_ref.insert(Collection);
               should_remove = true;
               return true;
          }

          /* release - Releases a previously reserved collection. */

          void release()
          {
               if (should_remove)
               {
                    std::lock_guard<std::mutex> lock(mutex);

                    set_ref.erase(Collection);
                    should_remove = false;
               }
          }
     };

     IndexingGuard guard(CollectionsBeingIndexed, IndexingMutex, collection);

     if (!guard.try_acquire())
     {
          /* Another thread is already indexing - exit */

          return;
     }

     /* Always use the main global Instance - check it's still valid */

     if (!Instance || !Instance->Database)
     {
          guard.release();
          return;
     }

     if (doc_keys.empty())
     {
          guard.release();
          return;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "LazyLoadCollectionIndex: Building index for collection '" + collection + "' with " + std::to_string(doc_keys.size()) + ".");
     }

     /* MEMORY SAFETY: Limit indexing to prevent OOM crashes with large datasets */

     const size_t MaxIndexDocuments = 1000000;

     size_t documents_to_index = std::min(doc_keys.size(), MaxIndexDocuments);

     if (doc_keys.size() > MaxIndexDocuments)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "LazyLoadCollectionIndex: Collection '" + collection + "' has " + std::to_string(doc_keys.size()) + ".");
          }
     }

     size_t indexed = 0;
     const size_t BatchSize = 100;
     bool memory_limit_reached = false;

     /* Comprehensive exception handling and early exit on memory pressure */

     try
     {
          for (size_t i = 0; i < documents_to_index && !memory_limit_reached; i += BatchSize)
          {
               size_t batch_end = std::min(i + BatchSize, documents_to_index);

               for (size_t j = i; j < batch_end && !memory_limit_reached; j++)
               {
                    const auto &doc_key = doc_keys[j];

                    /* Extract document ID */

                    size_t last_colon = doc_key.find_last_of(':');

                    if (last_colon != std::string::npos && last_colon + 1 < doc_key.size())
                    {
                         std::string doc_id = doc_key.substr(last_colon + 1);

                         /* Catch all exceptions from GetDocument */

                         Document doc;

                         try
                         {
                              doc = GetDocument(collection, doc_id);
                         }
                         catch (const std::exception &e)
                         {
                              if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                              {
                                   Instance->Logs->Debug("hybrid_storage", "LazyLoadCollectionIndex: Exception getting document '" + doc_id + "' in collection '" + collection + "': " + e.what() + ".");
                              }
                              continue;
                         }
                         catch (...)
                         {
                              if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                              {
                                   Instance->Logs->Debug("hybrid_storage", "LazyLoadCollectionIndex: Unknown exception getting document '" + doc_id + "' in collection '" + collection + "'.");
                              }
                              continue;
                         }

                         if (!doc.ID.empty())
                         {
                              try
                              {
                                   if (Instance->SearchIndex->AddDocument(collection, doc))
                                   {
                                        indexed++;
                                   }
                                   else
                                   {
                                        memory_limit_reached = true;
                                        if (Instance && Instance->Logs)
                                        {
                                             Instance->Logs->Normal("hybrid_storage", "LazyLoadCollectionIndex: Memory limit reached for collection '" + collection + "', indexed " + std::to_string(indexed) + ".");
                                        }
                                        break;
                                   }
                              }
                              catch (const std::bad_alloc &e)
                              {
                                   memory_limit_reached = true;
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Out of memory while indexing collection '" + collection + "', indexed " + std::to_string(indexed) + ".");
                                   }
                                   break;
                              }
                              catch (const std::exception &e)
                              {
                                   if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                   {
                                        Instance->Logs->Debug("hybrid_storage", "LazyLoadCollectionIndex: Exception indexing document '" + doc.ID + "' in collection '" + collection + "': " + e.what() + ".");
                                   }
                              }
                              catch (...)
                              {
                                   if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                                   {
                                        Instance->Logs->Debug("hybrid_storage", "LazyLoadCollectionIndex: Unknown exception indexing document '" + doc.ID + "' in collection '" + collection + "'.");
                                   }
                              }
                         }
                    }
               }

               /* Early exit if memory limit reached */

               if (memory_limit_reached)
               {
                    break;
               }

               if (indexed >= MaxIndexDocuments)
               {
                    break;
               }

               /* Yield periodically to avoid blocking other operations */

               if (i % (BatchSize * 10) == 0)
               {
                    std::this_thread::yield();
               }
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Exception during indexing loop for collection '" + collection + "': " + e.what() + ".");
          }

          guard.release();
          return;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Unknown exception during indexing loop for collection '" + collection + "'.");
          }

          guard.release();
          return;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "LazyLoadCollectionIndex: Indexed " + std::to_string(indexed) + " documents for collection '" + collection + "'.");
     }

     /* Guard destructor will automatically remove from indexing set */
}

/* LazyLoadCollectionIndex - Builds an index lazily for a collection. */

bool HybridStorageManager::LazyLoadCollectionIndex(const std::string &collection)
{
     /*
           * Lazy loading: Build index on-demand when collection is first searched.
           * This avoids loading all indexes at startup.
           * NON-BLOCKING: Starts indexing in background thread and returns immediately.
           */

     /* Always use the main global Instance - never cache or create new instances */

     if (!Instance || !Instance->Database)
     {
          return false;
     }

     /* Check if already indexed first (fast path) */

     size_t indexed_count = Instance->SearchIndex->GetDocumentCount(collection);

     if (indexed_count > 0)
     {
          return true;
     }

     /* Check if collection exists */

     if (!CollectionExists(collection))
     {
          return false;
     }

     /* Check if already being indexed - return immediately (non-blocking) */

     {
          std::lock_guard<std::mutex> lock(IndexingMutex);

          if (CollectionsBeingIndexed.find(collection) != CollectionsBeingIndexed.end())
          {
               /* Already being indexed in background - return immediately to allow search to proceed */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "LazyLoadCollectionIndex: Collection '" + collection + "' is already being indexed in background.");
               }
               return true;
          }
     }

     /*
           * Get all documents in this collection.
           * PERFORMANCE: For very large collections, this could be slow, but necessary for indexing.
           * Exception handling prevents crashes if Keys() fails.
           */

     std::string pattern = "doc:" + collection + ":*";
     std::vector<std::string> doc_keys;

     /* Catch all exceptions from Database operations */

     try
     {
          doc_keys = Instance->Database->Keys(pattern);
     }
     catch (const std::bad_alloc &e)
     {
          /* Memory error - collection too large to load all keys */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Out of memory getting document keys for collection '" + collection + "': " + e.what() + " - collection may be too large.");
          }
          return false;
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Exception getting document keys for collection '" + collection + "': " + e.what() + ".");
          }
          return false;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "LazyLoadCollectionIndex: Unknown exception getting document keys for collection '" + collection + "'.");
          }
          return false;
     }

     if (doc_keys.empty())
     {
          return true;
     }

     /* For small collections, index synchronously to avoid empty search results. */

     const size_t SyncIndexMaxDocs = 10000;

     if (doc_keys.size() <= SyncIndexMaxDocs)
     {
          IndexCollectionInBackground(collection, doc_keys);
          return true;
     }

     /* Start indexing in background thread - return immediately */

     std::thread indexing_thread([this, collection, doc_keys]()

                                 {
                                      this->IndexCollectionInBackground(collection, doc_keys);
                                 });

     /* Store thread for cleanup at shutdown */

     {
          std::lock_guard<std::mutex> lock(IndexingThreadsMutex);

          IndexingThreads.push_back(std::move(indexing_thread));
     }

     /* Return true immediately - search can proceed with partial index or fallback to RocksDB scan */

     return true;
}

/* IsCollectionIndexing - Reports whether a collection is currently being indexed. */

bool HybridStorageManager::IsCollectionIndexing(const std::string &collection)
{
     std::lock_guard<std::mutex> lock(IndexingMutex);

     return CollectionsBeingIndexed.find(collection) != CollectionsBeingIndexed.end();
}

/* GetStats - Returns storage and index statistics. */

HybridStorageManager::Stats HybridStorageManager::GetStats() const
{
     Stats stats;

     /* Get stats from RocksDB if available */

     if (Instance && Instance->Database)
     {
          auto db_stats = Instance->Database->GetRocksDBStats();

          stats.num_keys_written = db_stats.num_keys_written;
          stats.bytes_written = db_stats.bytes_written;
          stats.num_keys_read = db_stats.num_keys_read;
          stats.bytes_read = db_stats.bytes_read;
          stats.memtable_size = db_stats.memtable_size;
          stats.total_db_size = db_stats.total_db_size;
          stats.sstable_count = db_stats.num_sst_files;
          stats.memory_usage = db_stats.block_cache_usage + db_stats.index_and_filter_cache_usage;
     }

     /* Calculate document and collection counts (need const_cast for const method) */

     auto collections = const_cast<HybridStorageManager *>(this)->ListCollections();

     stats.total_collections = collections.size();

     for (const auto &coll : collections)
     {
          stats.total_documents += const_cast<HybridStorageManager *>(this)->GetCollectionDocumentCount(coll);
     }

     return stats;
}

/* Get - Retrieves a raw value by key. */

std::string HybridStorageManager::Get(const std::string &key)
{
     if (!Instance || !Instance->Database)
     {
          return "";
     }

     return Instance->Database->Get(key);
}

/* Delete - Deletes a raw key from storage. */

bool HybridStorageManager::Delete(const std::string &key)
{
     if (!Instance || !Instance->Database)
     {
          return false;
     }
     return Instance->Database->Del(key) > 0;
}

/* FlushAll - Clears all storage, indexes, and caches. */

bool HybridStorageManager::FlushAll()
{
     if (!Instance || !Instance->Database)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("hybrid_storage", "FlushAll: Instance or Database is null.");
          }
          return false;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "FlushAll: Starting complete flush - removing all data, indexes, caches, and mmap files.");
     }

     if (Instance->Sam && Instance->Sam->IsOpen())
     {
          std::string SAMCancelError;

          if (!Instance->Sam->CancelAllWork(&SAMCancelError) &&
              Instance->Logs && !SAMCancelError.empty())
          {
               Instance->Logs->Normal("hybrid_storage",
                                      "FlushAll: Failed to cancel SAM work before destructive flush: " +
                                           SAMCancelError + ".");
          }
     }

     /*
           * PERFORMANCE OPTIMIZATION: Use bulk deletion instead of deleting keys one by one.
           * This is MUCH faster - DeleteRange is optimized by RocksDB for bulk operations.
           */

     /* Step 1: Clear inverted index completely (in-memory maps and mmap indexes) */

     try
     {
          Instance->SearchIndex->Clear();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "FlushAll: Cleared inverted index (in-memory and mmap).");
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error clearing inverted index: " + std::string(e.what()) + ".");
          }
     }

     /* Step 2: Clear all caches (RocksDB block cache, index cache, etc.) */

     try
     {
          Instance->Database->ClearAllCaches();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "FlushAll: Cleared all database caches.");
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error clearing caches: " + std::string(e.what()) + ".");
          }
     }

     /* Step 3: Clear collections map (fast - in-memory) */

     size_t collection_count = 0;

     {
          std::lock_guard<std::mutex> lock(CollectionsMutex);

          collection_count = Collections.size();

          Collections.clear();
     }

     /*
      * Step 4: Use DeleteRange for efficient bulk deletion of all data.
      * DeleteRange is MUCH faster than Keys() + Del() loops.
      */

     try
     {
          const std::vector<std::pair<std::string, std::string>> ranges = {
               {"doc:", "doc;"},
               {"idx:", "idx;"},
               {"collection_meta:", "collection_meta;"},
               {"collection_config:", "collection_config;"},
               {"synonyms:", "synonyms;"},
               {"stopwords:", "stopwords;"},
               {"override:", "override;"},
               {"alias:", "alias;"},
               {"module_data:", "module_data;"},
               {"session:", "session;"},
               {"spotlight:", "spotlight;"},
               {"trends:", "trends;"},
               {"flush_pending:", "flush_pending;"}};

          for (const auto &range : ranges)
          {
               Instance->Database->DeleteRange(range.first, range.second);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "FlushAll: Used DeleteRange to delete known storage prefixes, including collection_config:, module_data:, synonyms:, and stopwords:.");
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error during DeleteRange: " + std::string(e.what()) + ".");
          }
     }

     /*
      * Step 4b: Remove any residual keys that do not fall under the known prefixes.
      * Flush is expected to leave RocksDB empty, so do a final sweep.
      */

     try
     {
          const std::vector<std::string> all_keys = Instance->Database->Keys("*");

          for (const auto &key : all_keys)
          {
               Instance->Database->Del(key);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "FlushAll: Final sweep removed " + std::to_string(all_keys.size()) + " residual key(s).");
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error during residual key sweep: " + std::string(e.what()) + ".");
          }
     }

     /* Step 5: Clear mmap index files from disk */

     try
     {
          std::string index_dir = std::string(HLQUERY_DATA_DIR) + "/storage/indices";

          if (std::filesystem::exists(index_dir))
          {
               for (const auto &entry : std::filesystem::directory_iterator(index_dir))
               {
                    if (entry.is_regular_file() && (entry.path().extension() == ".mmap" || entry.path().extension() == ".IDx"))
                    {
                         std::filesystem::remove(entry.path());
                    }
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("hybrid_storage", "FlushAll: Removed mmap index files from disk.");
               }
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error removing mmap index files: " + std::string(e.what()) + ".");
          }
     }

     /* Step 6: Flush and sync database to ensure all deletions are persisted */

     try
     {
          /* Use FlushAndSync() to ensure all deletions are written to disk */

          Instance->Database->FlushAndSync();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("hybrid_storage", "FlushAll: Database flushed and synced to disk.");
          }
     }
     catch (const std::exception &e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("hybrid_storage", "FlushAll: Error flushing database: " + std::string(e.what()) + ".");
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("hybrid_storage", "FlushAll: Completed - cleared " + std::to_string(collection_count) + " collections, all indexes, caches, and data. System is now empty and ready for fresh start.");
     }

     return true;
}
