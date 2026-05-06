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
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <ctime>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/searchpool.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "search/cstore.h"
#include "search/storageengine.h"
#include "utils/jsonbuilder.h"
#include "vendor/json/json.hpp"

/* Runtime module that purges expired documents and collections. */

class AutoDeleterRuntimeModule final : public AutoRuntimeModule<AutoDeleterRuntimeModule>
{
   private:

     /* Supported purge scopes. */

     enum class DeleteMode
     {
          DocumentsOnly,
          DocumentsAndCollections,
          CollectionsOnly
     };

     /* Protects mutable runtime configuration. */

     std::mutex state_mutex;

     /* Retention threshold in days. */

     uint64_t retention_window_ms = 120ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

     /* Human-readable retention value used for status and logs. */

     std::string retention_label = "120d";

     /* Maximum expired documents deleted per delete batch. */

     int delete_batch_size = 256;

     /* Active purge scope. */

     DeleteMode delete_scope = DeleteMode::DocumentsOnly;

     /* Tracks whether the background worker is active. */

     std::atomic<bool> running{false};

     /* Tracks whether a purge pass has been scheduled or is running. */

     std::atomic<bool> scheduled{false};

     /* Records when the current scheduled run was queued. */

     std::atomic<uint64_t> scheduled_at_ms{0};

     /* Signals the worker to stop as soon as possible. */

     std::atomic<bool> stopping{false};

     /* Rotating cursor to avoid rescanning from collection zero every pass. */

     size_t next_collection_cursor = 0;

     /* Last execution state exposed through module status. */

     uint64_t last_started_at = 0;

     uint64_t last_completed_at = 0;

     uint64_t last_duration_ms = 0;

     size_t last_collections_scanned = 0;

     size_t last_documents_checked = 0;

     size_t last_expired_found = 0;

     size_t last_deleted_count = 0;

     size_t last_zero_timestamp_skipped = 0;

     bool last_pass_hit_time_budget = false;

     bool last_pass_hit_collection_limit = false;

     bool last_run_async = false;

     bool last_more_work_likely = false;

     bool last_run_drained = false;

     size_t last_sweep_passes = 0;

     std::string last_error;

     struct PurgePassStats
     {
          uint64_t StartedAt = 0;
          uint64_t CompletedAt = 0;
          uint64_t DurationMS = 0;
          size_t CollectionsScanned = 0;
          size_t DocumentsChecked = 0;
          size_t ExpiredFound = 0;
          size_t DeletedCount = 0;
          size_t ZeroTimestampSkipped = 0;
          bool HitTimeBudget = false;
          bool HitCollectionLimit = false;
          bool RanAsync = false;
          bool MoreWorkLikely = false;
          bool Drained = false;
          size_t SweepPasses = 1;
          std::string Error;
     };

     uint64_t CurrentUnixTime() const
     {
          return static_cast<uint64_t>(std::max<time_t>(0, std::time(nullptr)));
     }

     uint64_t CurrentSteadyMS() const
     {
          return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
     }

     void ApplyPassStats(const PurgePassStats &stats)
     {
          std::lock_guard<std::mutex> Lock(state_mutex);
          last_started_at = stats.StartedAt;
          last_completed_at = stats.CompletedAt;
          last_duration_ms = stats.DurationMS;
          last_collections_scanned = stats.CollectionsScanned;
          last_documents_checked = stats.DocumentsChecked;
          last_expired_found = stats.ExpiredFound;
          last_deleted_count = stats.DeletedCount;
          last_zero_timestamp_skipped = stats.ZeroTimestampSkipped;
          last_pass_hit_time_budget = stats.HitTimeBudget;
          last_pass_hit_collection_limit = stats.HitCollectionLimit;
          last_run_async = stats.RanAsync;
          last_more_work_likely = stats.MoreWorkLikely;
          last_run_drained = stats.Drained;
          last_sweep_passes = stats.SweepPasses;
          last_error = stats.Error;
     }

     static bool ParseTimestampFieldValue(const nlohmann::json &value, uint64_t &timestamp_out)
     {
          timestamp_out = 0;

          std::string raw_value;
          if (value.is_number_unsigned())
          {
               timestamp_out = value.get<uint64_t>();
               return (timestamp_out > 0);
          }

          if (value.is_number_integer())
          {
               const long long signed_value = value.get<long long>();
               if (signed_value <= 0)
               {
                    return false;
               }

               timestamp_out = static_cast<uint64_t>(signed_value);
               return true;
          }

          if (!value.is_string())
          {
               return false;
          }

          raw_value = value.get<std::string>();
          if (raw_value.empty())
          {
               return false;
          }

          bool digits_only = true;
          for (unsigned char ch : raw_value)
          {
               if (!std::isdigit(ch))
               {
                    digits_only = false;
                    break;
               }
          }

          if (digits_only)
          {
               try
               {
                    timestamp_out = std::stoull(raw_value);
                    if (timestamp_out == 0)
                    {
                         return false;
                    }

                    if (raw_value.size() <= 10)
                    {
                         timestamp_out *= 1000ULL;
                    }

                    return true;
               }
               catch (...)
               {
                    return false;
               }
          }

          std::tm tm_value = {};
          std::istringstream stream(raw_value);
          stream >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
          if (stream.fail())
          {
               return false;
          }

          time_t parsed_time = timegm(&tm_value);
          if (parsed_time < 0)
          {
               return false;
          }

          timestamp_out = static_cast<uint64_t>(parsed_time) * 1000ULL;

          if (stream.peek() == '.')
          {
               stream.get();
               std::string fractional_digits;
               while (std::isdigit(stream.peek()))
               {
                    fractional_digits.push_back(static_cast<char>(stream.get()));
               }

               if (!fractional_digits.empty())
               {
                    while (fractional_digits.size() < 3)
                    {
                         fractional_digits.push_back('0');
                    }

                    if (fractional_digits.size() > 3)
                    {
                         fractional_digits.resize(3);
                    }

                    timestamp_out += static_cast<uint64_t>(std::stoul(fractional_digits));
               }
          }

          if (stream.peek() == 'Z')
          {
               stream.get();
          }

          return stream.eof();
     }

     static bool ParseTimestampFromFields(const std::string &fields_json, uint64_t &timestamp_out)
     {
          timestamp_out = 0;
          if (fields_json.empty())
          {
               return false;
          }

          try
          {
               const nlohmann::json fields = nlohmann::json::parse(fields_json);
               if (fields.contains("timestamp") && ParseTimestampFieldValue(fields["timestamp"], timestamp_out))
               {
                    return true;
               }

               if (fields.contains("created_at") && ParseTimestampFieldValue(fields["created_at"], timestamp_out))
               {
                    return true;
               }
          }
          catch (...)
          {
               return false;
          }

          return false;
     }

     bool ReadStoredTimestamp(const std::string &collection, const std::string &document_id, uint64_t &timestamp_out, bool &missing_timestamp) const
     {
          timestamp_out = 0;
          missing_timestamp = false;

          if (!Instance || !Instance->Database)
          {
               return false;
          }

          const std::string doc_key = "doc:" + collection + ":" + document_id;
          const std::string raw = Instance->Database->Get(doc_key);
          if (raw.empty())
          {
               return false;
          }

          const size_t pos1 = raw.find('|');
          if (pos1 == std::string::npos)
          {
               missing_timestamp = true;
               return true;
          }

          const size_t pos2 = raw.find('|', pos1 + 1);
          if (pos2 == std::string::npos)
          {
               missing_timestamp = true;
               return true;
          }

          const size_t pos3 = raw.find('|', pos2 + 1);
          if (pos3 == std::string::npos)
          {
               missing_timestamp = true;
               return true;
          }

          const size_t pos4 = raw.find('|', pos3 + 1);
          if (pos4 == std::string::npos)
          {
               const std::string fields_json = raw.substr(pos3 + 1);
               if (ParseTimestampFromFields(fields_json, timestamp_out))
               {
                    return true;
               }

               missing_timestamp = true;
               return true;
          }

          const std::string fields_json = raw.substr(pos3 + 1, pos4 - (pos3 + 1));

          const std::string timestamp_str = raw.substr(pos4 + 1);
          if (timestamp_str.empty())
          {
               if (ParseTimestampFromFields(fields_json, timestamp_out))
               {
                    return true;
               }

               missing_timestamp = true;
               return true;
          }

          try
          {
               timestamp_out = std::stoull(timestamp_str);
               if (timestamp_out == 0)
               {
                    missing_timestamp = true;
               }
               return true;
          }
          catch (...)
          {
               timestamp_out = 0;
               if (ParseTimestampFromFields(fields_json, timestamp_out))
               {
                    return true;
               }

               missing_timestamp = true;
               return true;
          }
     }

     nlohmann::json InspectCollections(const std::string &collection_filter = "") const
     {
          nlohmann::json root;
          root["module"] = "autodeleter";
          root["ok"] = true;
          root["action"] = "inspect";
          root["retention_time"] = retention_label;
          root["collections"] = nlohmann::json::array();

          if (!Instance || !Instance->Database)
          {
               root["ok"] = false;
               root["error"] = "Database is unavailable.";
               return root;
          }

          const uint64_t now_ms_value = static_cast<uint64_t>(std::max<long long>(0, Instance->NowMs()));
          const uint64_t cutoff_timestamp_ms = (now_ms_value > retention_window_ms) ? (now_ms_value - retention_window_ms) : 0;

          const std::vector<std::string> collections = HybridStorageManagerInstance().ListCollections();
          for (const auto &collection : collections)
          {
               if (!collection_filter.empty() && collection != collection_filter)
               {
                    continue;
               }

               const std::vector<std::string> keys = Instance->Database->Keys("doc:" + collection + ":*");
               uint64_t oldest_timestamp = std::numeric_limits<uint64_t>::max();
               uint64_t newest_timestamp = 0;
               size_t total_docs = 0;
               size_t missing_timestamp_docs = 0;
               size_t expired_docs = 0;
               size_t live_docs = 0;

               for (const auto &key : keys)
               {
                    const size_t last_colon = key.find_last_of(':');
                 
                    if (last_colon == std::string::npos || last_colon + 1 >= key.size())
                    {
                         continue;
                    }

                    const std::string document_id = key.substr(last_colon + 1);
                    uint64_t stored_timestamp = 0;
                    bool missing_timestamp = false;
                 
                    if (!ReadStoredTimestamp(collection, document_id, stored_timestamp, missing_timestamp))
                    {
                         continue;
                    }

                    ++total_docs;

                    if (missing_timestamp || stored_timestamp == 0)
                    {
                         ++missing_timestamp_docs;
                         ++expired_docs;
                         continue;
                    }

                    oldest_timestamp = std::min(oldest_timestamp, stored_timestamp);
                    newest_timestamp = std::max(newest_timestamp, stored_timestamp);

                    if (stored_timestamp <= cutoff_timestamp_ms)
                    {
                         ++expired_docs;
                    }
                    else
                    {
                         ++live_docs;
                    }
               }

               nlohmann::json item;
               item["collection"] = collection;
               item["total_docs"] = total_docs;
               item["expired_docs"] = expired_docs;
               item["live_docs"] = live_docs;
               item["missing_timestamp_docs"] = missing_timestamp_docs;
               item["fully_expired"] = (total_docs > 0 && (expired_docs + live_docs) == total_docs && live_docs == 0);
               item["empty"] = (total_docs == 0);
               item["oldest_timestamp"] = (oldest_timestamp == std::numeric_limits<uint64_t>::max()) ? 0 : oldest_timestamp;
               item["newest_timestamp"] = newest_timestamp;
               root["collections"].push_back(item);
          }

          if (!collection_filter.empty() && root["collections"].empty())
          {
               root["ok"] = false;
               root["error"] = "Collection not found.";
               root["collection"] = collection_filter;
          }

          return root;
     }

     /* Runs one purge pass using the current configuration snapshot. */

     void RunPurgePass(bool ran_async)
     {
          uint64_t retention_window_ms_snapshot = 120ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
          std::string retention_label_snapshot = "120d";
          int delete_batch_size_snapshot = 256;
          DeleteMode delete_scope_snapshot = DeleteMode::DocumentsOnly;
          size_t collection_cursor_snapshot = 0;

          PurgePassStats stats;
          stats.StartedAt = CurrentUnixTime();
          stats.RanAsync = ran_async;
          const uint64_t started_steady_ms = CurrentSteadyMS();

          {
               std::lock_guard<std::mutex> Lock(state_mutex);

               retention_window_ms_snapshot = retention_window_ms;
               retention_label_snapshot = retention_label;
               delete_batch_size_snapshot = delete_batch_size;
               delete_scope_snapshot = delete_scope;
               collection_cursor_snapshot = next_collection_cursor;
          }

          if (!Instance || !Instance->Database || !Instance->SearchIndex)
          {
               stats.Error = "hlquery instance, database, or search index is unavailable.";
               stats.CompletedAt = CurrentUnixTime();
               stats.DurationMS = CurrentSteadyMS() - started_steady_ms;
               ApplyPassStats(stats);
               return;
          }

          if (Instance->IsShuttingDown() || !HybridStorageManagerInstance().IsInitialized())
          {
               stats.Error = "storage is not initialized or the server is shutting down.";
               stats.CompletedAt = CurrentUnixTime();
               stats.DurationMS = CurrentSteadyMS() - started_steady_ms;
               ApplyPassStats(stats);
               return;
          }

          const uint64_t now_ms_value = static_cast<uint64_t>(std::max<long long>(0, Instance->NowMs()));
          if (now_ms_value <= retention_window_ms_snapshot)
          {
               stats.CompletedAt = CurrentUnixTime();
               stats.DurationMS = CurrentSteadyMS() - started_steady_ms;
               ApplyPassStats(stats);
               return;
          }

          const uint64_t cutoff_timestamp_ms = now_ms_value - retention_window_ms_snapshot;

          size_t deleted_count = 0;

          const std::vector<std::string> collections = HybridStorageManagerInstance().ListCollections();

          if (collections.empty())
          {
               stats.CompletedAt = CurrentUnixTime();
               stats.DurationMS = CurrentSteadyMS() - started_steady_ms;
               ApplyPassStats(stats);
               return;
          }

          const size_t collection_count = collections.size();
          const size_t start_index = collection_cursor_snapshot % collection_count;
          size_t next_cursor_value = start_index;

          for (size_t step = 0; step < collection_count; ++step)
          {
               const size_t index = (start_index + step) % collection_count;
               const std::string &collection = collections[index];

               if (stopping.load(std::memory_order_acquire) || Instance->IsShuttingDown())
               {
                    break;
               }

               if (!HybridStorageManagerInstance().CollectionExists(collection))
               {
                    next_cursor_value = (index + 1) % collection_count;
                    continue;
               }

               std::vector<std::string> keys;

               try
               {
                    keys = Instance->Database->Keys("doc:" + collection + ":*");
               }
               catch (const std::exception &Error)
               {
                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("autodeleter", "Failed to enumerate documents for collection '" + collection + "': " + std::string(Error.what()) + ".");
                    }

                    stats.Error = Error.what();
                    next_cursor_value = (index + 1) % collection_count;
                    continue;
               }
               catch (...)
               {
                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("autodeleter", "Failed to enumerate documents for collection '" + collection + "' due to an unknown error.");
                    }

                    stats.Error = "unknown enumeration error";
                    next_cursor_value = (index + 1) % collection_count;
                    continue;
               }

               bool collection_has_live_documents = false;

               std::vector<std::string> expired_ids;
               expired_ids.reserve(static_cast<size_t>(delete_batch_size_snapshot));
               ++stats.CollectionsScanned;

               for (const auto &key : keys)
               {
                    if (stopping.load(std::memory_order_acquire) || Instance->IsShuttingDown())
                    {
                         break;
                    }

                    const size_t last_colon = key.find_last_of(':');

                    if (last_colon == std::string::npos || last_colon + 1 >= key.size())
                    {
                         continue;
                    }

                    const std::string document_id = key.substr(last_colon + 1);

                    uint64_t stored_timestamp = 0;
                    bool missing_timestamp = false;
                    if (!ReadStoredTimestamp(collection, document_id, stored_timestamp, missing_timestamp))
                    {
                         continue;
                    }

                    ++stats.DocumentsChecked;

                    if (missing_timestamp || stored_timestamp == 0)
                    {
                         ++stats.ZeroTimestampSkipped;
                         expired_ids.push_back(document_id);
                         ++stats.ExpiredFound;

                         if (delete_scope_snapshot != DeleteMode::CollectionsOnly && static_cast<int>(expired_ids.size()) >= delete_batch_size_snapshot)
                         {
                              deleted_count += DeleteExpiredDocuments(collection, expired_ids);
                              expired_ids.clear();
                         }
                         continue;
                    }

                    if (stored_timestamp > cutoff_timestamp_ms)
                    {
                         collection_has_live_documents = true;
                         continue;
                    }

                    expired_ids.push_back(document_id);
                    ++stats.ExpiredFound;

                    if (delete_scope_snapshot != DeleteMode::CollectionsOnly && static_cast<int>(expired_ids.size()) >= delete_batch_size_snapshot)
                    {
                         deleted_count += DeleteExpiredDocuments(collection, expired_ids);
                         expired_ids.clear();
                    }
               }

               if (delete_scope_snapshot != DeleteMode::CollectionsOnly && !expired_ids.empty())
               {
                    deleted_count += DeleteExpiredDocuments(collection, expired_ids);
               }

               if (delete_scope_snapshot == DeleteMode::DocumentsAndCollections && !collection_has_live_documents)
               {
                    deleted_count += DeleteExpiredCollection(collection);
                    continue;
               }

               if (delete_scope_snapshot == DeleteMode::CollectionsOnly && !collection_has_live_documents && !expired_ids.empty())
               {
                    deleted_count += DeleteExpiredCollection(collection);
               }

               next_cursor_value = (index + 1) % collection_count;
          }

          stats.DeletedCount = deleted_count;

          {
               std::lock_guard<std::mutex> Lock(state_mutex);
               next_collection_cursor = next_cursor_value;
          }

          if (deleted_count > 0 && Instance->Logs)
          {
               Instance->Logs->Normal("autodeleter", "Purged " + std::to_string(deleted_count) + " expired documents older than " + retention_label_snapshot + ".");
          }

          stats.CompletedAt = CurrentUnixTime();
          stats.DurationMS = CurrentSteadyMS() - started_steady_ms;
          stats.MoreWorkLikely = (stats.DeletedCount > 0);
          ApplyPassStats(stats);
     }

     void RunDrainSweep(bool ran_async)
     {
          PurgePassStats aggregate;
          aggregate.StartedAt = CurrentUnixTime();
          aggregate.RanAsync = ran_async;
          aggregate.Drained = true;
          aggregate.SweepPasses = 0;
          const uint64_t started_steady_ms = CurrentSteadyMS();

          for (size_t pass = 0; pass < 1024; ++pass)
          {
               RunPurgePass(ran_async);

               PurgePassStats snapshot;
               {
                    std::lock_guard<std::mutex> Lock(state_mutex);
                    snapshot.StartedAt = last_started_at;
                    snapshot.CompletedAt = last_completed_at;
                    snapshot.DurationMS = last_duration_ms;
                    snapshot.CollectionsScanned = last_collections_scanned;
                    snapshot.DocumentsChecked = last_documents_checked;
                    snapshot.ExpiredFound = last_expired_found;
                    snapshot.DeletedCount = last_deleted_count;
                    snapshot.ZeroTimestampSkipped = last_zero_timestamp_skipped;
                    snapshot.HitTimeBudget = last_pass_hit_time_budget;
                    snapshot.HitCollectionLimit = last_pass_hit_collection_limit;
                    snapshot.RanAsync = last_run_async;
                    snapshot.MoreWorkLikely = last_more_work_likely;
                    snapshot.Error = last_error;
               }

               ++aggregate.SweepPasses;
               aggregate.CollectionsScanned += snapshot.CollectionsScanned;
               aggregate.DocumentsChecked += snapshot.DocumentsChecked;
               aggregate.ExpiredFound += snapshot.ExpiredFound;
               aggregate.DeletedCount += snapshot.DeletedCount;
               aggregate.ZeroTimestampSkipped += snapshot.ZeroTimestampSkipped;
               aggregate.HitTimeBudget = snapshot.HitTimeBudget;
               aggregate.HitCollectionLimit = snapshot.HitCollectionLimit;
               aggregate.MoreWorkLikely = snapshot.MoreWorkLikely;
               if (!snapshot.Error.empty())
               {
                    aggregate.Error = snapshot.Error;
               }

               if (stopping.load(std::memory_order_acquire) || (Instance && Instance->IsShuttingDown()))
               {
                    break;
               }

               if (!snapshot.MoreWorkLikely)
               {
                    break;
               }
          }

          aggregate.CompletedAt = CurrentUnixTime();
          aggregate.DurationMS = CurrentSteadyMS() - started_steady_ms;
          aggregate.MoreWorkLikely = false;
          ApplyPassStats(aggregate);
     }

     /* Deletes a batch of expired documents from one collection. */

     size_t DeleteExpiredDocuments(const std::string &Collection, const std::vector<std::string> &DocumentIDs)
     {
          size_t deleted_count = 0;

          for (const auto &document_id : DocumentIDs)
          {
               if (stopping.load(std::memory_order_acquire) || Instance->IsShuttingDown())
               {
                    break;
               }

               if (Instance && Instance->Modules)
               {
                    ModulePreCheckResult pre_check = RUN_MODULE_PRECHECK(OnPreDeleteDocument, Collection, document_id, "127.0.0.1", "autodeleter", true);

                    if (pre_check.Action == ModulePreCheckAction::Deny)
                    {
                         continue;
                    }
               }

               if (HybridStorageManagerInstance().DeleteDocument(Collection, document_id))
               {
                    ++deleted_count;
                    FOREACH_MOD(OnDeleteDocument, Collection, document_id, "127.0.0.1", "autodeleter", true);
               }
          }

          return deleted_count;
     }

     /* Deletes one collection when the selected scope allows it. */

     size_t DeleteExpiredCollection(const std::string &Collection)
     {
          if (!HybridStorageManagerInstance().CollectionExists(Collection))
          {
               return 0;
          }

          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult pre_check = RUN_MODULE_PRECHECK(OnPreDeleteCollection, Collection, "127.0.0.1", "autodeleter", true);

               if (pre_check.Action == ModulePreCheckAction::Deny)
               {
                    return 0;
               }
          }

          if (!HybridStorageManagerInstance().DeleteCollection(Collection))
          {
               return 0;
          }

          FOREACH_MOD(OnDeleteCollection, Collection, "127.0.0.1", "autodeleter", true);

          return 1;
     }

     /* Parses the configured delete scope string. */

     static DeleteMode ParseDeleteScope(const std::string &RawScope)
     {
          std::string scope = RawScope;

          std::transform(scope.begin(), scope.end(), scope.begin(),
                         [](unsigned char Character)
                         {
                              return static_cast<char>(std::tolower(Character));
                         });

          if (scope == "collections" || scope == "collection" || scope == "only_collections")
          {
               return DeleteMode::CollectionsOnly;
          }

          if (scope == "docs_and_collections" || scope == "documents_and_collections" || scope == "both")
          {
               return DeleteMode::DocumentsAndCollections;
          }

          return DeleteMode::DocumentsOnly;
     }

     /* Returns the configured delete scope label. */

     static std::string DeleteScopeToString(DeleteMode Scope)
     {
          if (Scope == DeleteMode::CollectionsOnly)
          {
               return "collections";
          }

          if (Scope == DeleteMode::DocumentsAndCollections)
          {
               return "docs_and_collections";
          }

          return "docs";
     }

     /* Parses a retention value like 10m, 12h, 14d, 2w, or 1y into milliseconds. */

     static uint64_t ParseRetentionSpecToMilliseconds(const std::string &RawValue)
     {
          std::string value;
          value.reserve(RawValue.size());

          for (unsigned char character : RawValue)
          {
               if (!std::isspace(character))
               {
                    value.push_back(static_cast<char>(std::tolower(character)));
               }
          }

          if (value.empty())
          {
               throw std::invalid_argument("retention_time cannot be empty");
          }

          size_t numeric_end = 0;

          while (numeric_end < value.size() && std::isdigit(static_cast<unsigned char>(value[numeric_end])))
          {
               ++numeric_end;
          }

          if (numeric_end == 0)
          {
               throw std::invalid_argument("retention_time must start with a number");
          }

          const uint64_t amount = std::stoull(value.substr(0, numeric_end));

          if (amount == 0)
          {
               throw std::invalid_argument("retention_time must be greater than zero");
          }

          const std::string suffix = value.substr(numeric_end);
          uint64_t multiplier_ms = 24ULL * 60ULL * 60ULL * 1000ULL;

          if (suffix == "s")
          {
               multiplier_ms = 1000ULL;
          }
          else if (suffix.empty() || suffix == "d")
          {
               multiplier_ms = 24ULL * 60ULL * 60ULL * 1000ULL;
          }
          else if (suffix == "m")
          {
               multiplier_ms = 60ULL * 1000ULL;
          }
          else if (suffix == "h")
          {
               multiplier_ms = 60ULL * 60ULL * 1000ULL;
          }
          else if (suffix == "w")
          {
               multiplier_ms = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
          }
          else if (suffix == "y")
          {
               multiplier_ms = 365ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
          }
          else
          {
               throw std::invalid_argument("invalid retention_time suffix; use s=seconds, m=minutes, h=hours, d=days, w=weeks, y=years");
          }

          if (amount > (std::numeric_limits<uint64_t>::max() / multiplier_ms))
          {
               throw std::invalid_argument("retention_time value is too large");
          }

          return amount * multiplier_ms;
     }

     /* Normalizes a retention value for status output and logs. */

     static std::string NormalizeRetentionSpec(const std::string &RawValue)
     {
          std::string value;
          value.reserve(RawValue.size());

          for (unsigned char character : RawValue)
          {
               if (!std::isspace(character))
               {
                    value.push_back(static_cast<char>(std::tolower(character)));
               }
          }

          if (value.empty())
          {
               throw std::invalid_argument("retention_time cannot be empty");
          }

          size_t numeric_end = 0;

          while (numeric_end < value.size() && std::isdigit(static_cast<unsigned char>(value[numeric_end])))
          {
               ++numeric_end;
          }

          if (numeric_end == 0)
          {
               throw std::invalid_argument("retention_time must start with a number");
          }

          const std::string amount = value.substr(0, numeric_end);
          const std::string suffix = value.substr(numeric_end);

          if (suffix.empty())
          {
               return amount + "d";
          }

          if (suffix == "s" || suffix == "m" || suffix == "h" || suffix == "d" || suffix == "w" || suffix == "y")
          {
               return amount + suffix;
          }

          throw std::invalid_argument("invalid retention_time suffix; use s=seconds, m=minutes, h=hours, d=days, w=weeks, y=years");
     }

     /* Runs one purge pass only when the worker is not already active. */

     bool TryRunPurgePass(bool async_preferred, bool *scheduled_async = nullptr, bool drain_until_complete = false)
     {
          static constexpr uint64_t StaleScheduledThresholdMS = 30000;

          if (scheduled_async != nullptr)
          {
               *scheduled_async = false;
          }

          if (stopping.load(std::memory_order_acquire))
          {
               return false;
          }

          if (scheduled.load(std::memory_order_acquire) && !running.load(std::memory_order_acquire))
          {
               const uint64_t queued_at_ms = scheduled_at_ms.load(std::memory_order_acquire);
               const uint64_t now_ms = CurrentSteadyMS();

               if (queued_at_ms > 0 && now_ms > queued_at_ms && (now_ms - queued_at_ms) >= StaleScheduledThresholdMS)
               {
                    bool expected_scheduled = true;
                    if (scheduled.compare_exchange_strong(expected_scheduled, false, std::memory_order_acq_rel))
                    {
                         scheduled_at_ms.store(0, std::memory_order_release);

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("autodeleter", "Recovered a stale scheduled purge pass that never started on the worker pool.");
                         }
                    }
               }
          }

          bool expected = false;
          if (!scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
          {
               return false;
          }

          scheduled_at_ms.store(CurrentSteadyMS(), std::memory_order_release);

          struct WorkerGuard
          {
               std::atomic<bool> &ScheduledFlag;
               std::atomic<bool> &RunningFlag;
               std::atomic<uint64_t> &ScheduledAtMS;

               ~WorkerGuard()
               {
                    RunningFlag.store(false, std::memory_order_release);
                    ScheduledFlag.store(false, std::memory_order_release);
                    ScheduledAtMS.store(0, std::memory_order_release);
               }
          };

          if (async_preferred && ThreadPoolManager::GetInstance().IsInitialized())
          {
               SearchThreadPool::PoolStats MgmtStats = ThreadPoolManager::GetInstance().GetManagementPool().GetStats();

               if (MgmtStats.TotalThreads > 0)
               {
                    auto future = ThreadPoolManager::GetInstance().GetManagementPool().Submit([this, drain_until_complete]()
                    {
                         if (stopping.load(std::memory_order_acquire))
                         {
                              scheduled.store(false, std::memory_order_release);
                              scheduled_at_ms.store(0, std::memory_order_release);
                              return;
                         }

                         running.store(true, std::memory_order_release);
                         WorkerGuard Guard{scheduled, running, scheduled_at_ms};
                         if (drain_until_complete)
                         {
                              RunDrainSweep(true);
                         }
                         else
                         {
                              RunPurgePass(true);
                         }
                    });

                    if (future.valid())
                    {
                         if (scheduled_async != nullptr)
                         {
                              *scheduled_async = true;
                         }
                         return true;
                    }
               }
          }

          running.store(true, std::memory_order_release);
          WorkerGuard Guard{scheduled, running, scheduled_at_ms};
          if (drain_until_complete)
          {
               RunDrainSweep(false);
          }
          else
          {
               RunPurgePass(false);
          }

          return true;
     }

   public:

     /* Initialize the auto-deleter runtime module. */

     AutoDeleterRuntimeModule()
         : AutoRuntimeModule("autodeleter", true)
     {
     }

     /* Start the module and load retention settings. */

     bool Start(const ServerConfig &, std::string &ErrorMessage) override
     {
          if (!Instance || !Instance->Config)
          {
               ErrorMessage = "Autodeleter module requires a live hlquery instance and configuration.";
               return false;
          }

          stopping.store(false, std::memory_order_release);
          running.store(false, std::memory_order_release);
          scheduled.store(false, std::memory_order_release);

          auto tag = Instance->Config->GetConfigReader().GetTag("autodeleter");

          if (!tag)
          {
               return true;
          }

          {
               std::lock_guard<std::mutex> Lock(state_mutex);

               std::string configured_retention = tag->GetString("retention_time", "");
               if (configured_retention.empty())
               {
                    configured_retention = tag->GetString("retention_days", "120");
               }
               retention_window_ms = ParseRetentionSpecToMilliseconds(configured_retention);
               retention_label = NormalizeRetentionSpec(configured_retention);
               delete_batch_size = std::clamp(tag->GetInt("delete_batch_size",
                                                          tag->GetInt("batch_size", 256)),
                                              1, 10000);
               delete_scope = ParseDeleteScope(tag->GetString("delete_scope", "docs"));
          }

          TryRunPurgePass(false);
          return true;
     }

     /* Stop the module and halt background work. */

     void Stop() override
     {
          stopping.store(true, std::memory_order_release);

          while (running.load(std::memory_order_acquire))
          {
               std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
     }

     /* Run one purge pass from the periodic module hook. */

     void OnEveryOneMinute() override
     {
          TryRunPurgePass(true);
     }

     /* Describe the module CLI surface. */

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription description;
          description.Name = "autodeleter";
          description.Summary = "Controls the automatic document and collection expiration worker.";
          description.Syntax = "GET /modules/autodeleter | POST /modules/autodeleter/run | POST /modules/autodeleter/config";
          description.MinParameters = 0;
          description.MaxParameters = 4;
          description.Parameters.push_back({"retention_time", "string", "Retention window. Suffixes: s=seconds, m=minutes, h=hours, d=days, w=weeks, y=years. Legacy alias: retention_days.", false});
          description.Parameters.push_back({"delete_batch_size", "int", "Deletion batch size. Legacy alias: batch_size.", false});
          description.Parameters.push_back({"delete_scope", "string", "One of docs, docs_and_collections, or collections.", false});
          description.Examples.push_back("curl http://localhost:9200/modules/autodeleter");
          description.Examples.push_back("curl -X POST http://localhost:9200/modules/autodeleter/run");
          description.Examples.push_back("curl -X POST http://localhost:9200/modules/autodeleter/config -d '{\"retention_time\":\"2w\",\"delete_batch_size\":128,\"delete_scope\":\"docs\"}'");

          return description;
     }

     /* Describe the supported auto-deleter commands. */

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          std::vector<ModuleCommandSpec> commands;

          ModuleCommandSpec status_command;
          status_command.Route = "status";
          status_command.Summary = "Shows the current autodeleter status and configuration.";
          status_command.Syntax = "module autodeleter status";
          commands.push_back(status_command);

          ModuleCommandSpec run_command;
          run_command.Route = "run";
          run_command.Summary = "Runs one purge pass immediately.";
          run_command.Syntax = "module autodeleter run";
          commands.push_back(run_command);

          ModuleCommandSpec inspect_command;
          inspect_command.Route = "inspect";
          inspect_command.Summary = "Shows per-collection expiration state under the current retention window.";
          inspect_command.Syntax = "module autodeleter inspect [collection]";
          inspect_command.MaxParameters = 1;
          inspect_command.Parameters.push_back({"collection", "string", "Optional collection name to inspect.", false});
          commands.push_back(inspect_command);

          ModuleCommandSpec config_command;
          config_command.Route = "config";
          config_command.Summary = "Updates runtime autodeleter configuration values.";
          config_command.Syntax = "module autodeleter config [--retention_time=2w] [--delete_batch_size=128] [--delete_scope=docs]";
          config_command.MaxParameters = 3;
          config_command.Parameters.push_back({"retention_time", "string", "Retention window. Suffixes: s=seconds, m=minutes, h=hours, d=days, w=weeks, y=years. Legacy alias: retention_days.", false});
          config_command.Parameters.push_back({"delete_batch_size", "int", "Deletion batch size. Legacy alias: batch_size.", false});
          config_command.Parameters.push_back({"delete_scope", "string", "One of docs, docs_and_collections, or collections.", false});
          commands.push_back(config_command);

          return commands;
     }

     /* Execute one auto-deleter module command. */

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string route = Request.Route.empty() ? "status" : Request.Route;

          if (route == "status")
          {
              std::string retention_label_snapshot;
              int delete_batch_size_snapshot = 0;
              DeleteMode delete_scope_snapshot = DeleteMode::DocumentsOnly;
              uint64_t last_started_at_snapshot = 0;
              uint64_t last_completed_at_snapshot = 0;
              uint64_t last_duration_ms_snapshot = 0;
              size_t last_collections_scanned_snapshot = 0;
              size_t last_documents_checked_snapshot = 0;
              size_t last_expired_found_snapshot = 0;
              size_t last_deleted_count_snapshot = 0;
              size_t last_zero_timestamp_skipped_snapshot = 0;
              bool last_pass_hit_time_budget_snapshot = false;
              bool last_pass_hit_collection_limit_snapshot = false;
              bool last_run_async_snapshot = false;
              bool last_more_work_likely_snapshot = false;
              bool last_run_drained_snapshot = false;
              size_t last_sweep_passes_snapshot = 0;
              std::string last_error_snapshot;

               {
                    std::lock_guard<std::mutex> Lock(state_mutex);
                    retention_label_snapshot = retention_label;
                    delete_batch_size_snapshot = delete_batch_size;
                    delete_scope_snapshot = delete_scope;
                    last_started_at_snapshot = last_started_at;
                    last_completed_at_snapshot = last_completed_at;
                    last_duration_ms_snapshot = last_duration_ms;
                    last_collections_scanned_snapshot = last_collections_scanned;
                    last_documents_checked_snapshot = last_documents_checked;
                    last_expired_found_snapshot = last_expired_found;
                    last_deleted_count_snapshot = last_deleted_count;
                    last_zero_timestamp_skipped_snapshot = last_zero_timestamp_skipped;
                    last_pass_hit_time_budget_snapshot = last_pass_hit_time_budget;
                    last_pass_hit_collection_limit_snapshot = last_pass_hit_collection_limit;
                    last_run_async_snapshot = last_run_async;
                    last_more_work_likely_snapshot = last_more_work_likely;
                    last_run_drained_snapshot = last_run_drained;
                    last_sweep_passes_snapshot = last_sweep_passes;
                    last_error_snapshot = last_error;
               }

               const bool running_snapshot = running.load(std::memory_order_acquire);
               const bool scheduled_snapshot = scheduled.load(std::memory_order_acquire);
               const bool stopping_snapshot = stopping.load(std::memory_order_acquire);

               JsonBuilder body;
               body.Add("module", "autodeleter");
               body.Add("retention_time", retention_label_snapshot);
               body.Add("delete_batch_size", static_cast<int>(delete_batch_size_snapshot));
               body.Add("delete_scope", DeleteScopeToString(delete_scope_snapshot));
               body.Add("running", running_snapshot);
               body.Add("scheduled", scheduled_snapshot);
               body.Add("stopping", stopping_snapshot);
               body.Add("worker_mode", ThreadPoolManager::GetInstance().IsInitialized() ? "management_pool" : "inline");
               if (scheduled_snapshot && !running_snapshot)
               {
                    const uint64_t queued_at_ms = scheduled_at_ms.load(std::memory_order_acquire);
                    const uint64_t now_ms = CurrentSteadyMS();
                    body.Add("queued_for_ms", static_cast<unsigned long long>((queued_at_ms > 0 && now_ms >= queued_at_ms) ? (now_ms - queued_at_ms) : 0));
               }
               body.Add("last_started_at", static_cast<unsigned long long>(last_started_at_snapshot));
               body.Add("last_completed_at", static_cast<unsigned long long>(last_completed_at_snapshot));
               body.Add("last_duration_ms", static_cast<unsigned long long>(last_duration_ms_snapshot));
               body.Add("last_collections_scanned", static_cast<unsigned long long>(last_collections_scanned_snapshot));
               body.Add("last_documents_checked", static_cast<unsigned long long>(last_documents_checked_snapshot));
               body.Add("last_expired_found", static_cast<unsigned long long>(last_expired_found_snapshot));
               body.Add("last_deleted_count", static_cast<unsigned long long>(last_deleted_count_snapshot));
               body.Add("last_zero_timestamp_skipped", static_cast<unsigned long long>(last_zero_timestamp_skipped_snapshot));
               body.Add("last_pass_hit_time_budget", last_pass_hit_time_budget_snapshot);
               body.Add("last_pass_hit_collection_limit", last_pass_hit_collection_limit_snapshot);
               body.Add("last_run_async", last_run_async_snapshot);
               body.Add("last_more_work_likely", last_more_work_likely_snapshot);
               body.Add("last_run_drained", last_run_drained_snapshot);
               body.Add("last_sweep_passes", static_cast<unsigned long long>(last_sweep_passes_snapshot));
               body.Add("next_run_in_seconds", static_cast<int>(60 - (CurrentUnixTime() % 60)));
               body.Add("note", "Documents without an internal timestamp fall back to document fields like created_at or timestamp; if still unresolved, autodeleter treats them as expired old-format records.");
               if (!last_error_snapshot.empty())
               {
                    body.Add("last_error", last_error_snapshot);
               }

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = body.ToString();
               return response;
          }

         if (route == "run")
         {
               ModuleCommandResponse response;

               if (!TryRunPurgePass(false, nullptr, true))
               {
                    response.StatusCode = 409;
                    response.Success = false;
                    response.Body = JsonBuilder()
                         .Add("module", "autodeleter")
                         .Add("error", "Purge pass already running or stopping.")
                         .ToString();
                    return response;
               }

               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "autodeleter")
                    .Add("message", "Drain sweep completed.")
                    .ToString();
               return response;
          }

          if (route == "inspect")
          {
               const std::string collection = Request.Parameters.empty() ? "" : Request.Parameters[0];

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = InspectCollections(collection).dump();
               return response;
          }

          if (route == "config")
          {
               try
               {
                    std::lock_guard<std::mutex> Lock(state_mutex);

                    if (Request.NamedParameters.count("retention_time"))
                    {
                         const std::string &requested_retention = Request.NamedParameters.at("retention_time");
                         retention_window_ms = ParseRetentionSpecToMilliseconds(requested_retention);
                         retention_label = NormalizeRetentionSpec(requested_retention);
                    }
                    else if (Request.NamedParameters.count("retention_days"))
                    {
                         const std::string &requested_retention = Request.NamedParameters.at("retention_days");
                         retention_window_ms = ParseRetentionSpecToMilliseconds(requested_retention);
                         retention_label = NormalizeRetentionSpec(requested_retention);
                    }

                    if (Request.NamedParameters.count("delete_batch_size"))
                    {
                         delete_batch_size = std::clamp(std::stoi(Request.NamedParameters.at("delete_batch_size")), 1, 10000);
                    }
                    else if (Request.NamedParameters.count("batch_size"))
                    {
                         delete_batch_size = std::clamp(std::stoi(Request.NamedParameters.at("batch_size")), 1, 10000);
                    }

                    if (Request.NamedParameters.count("delete_scope"))
                    {
                         delete_scope = ParseDeleteScope(Request.NamedParameters.at("delete_scope"));
                    }

                    ModuleCommandResponse response;
                    response.Success = true;
                    response.Body = JsonBuilder()
                         .Add("module", "autodeleter")
                         .Add("retention_time", retention_label)
                         .Add("delete_batch_size", delete_batch_size)
                         .Add("delete_scope", DeleteScopeToString(delete_scope))
                         .Add("message", "Configuration updated.")
                         .ToString();
                    return response;
               }
               catch (const std::exception &Error)
               {
                    ModuleCommandResponse response;
                    response.StatusCode = 400;
                    response.Success = false;
                    response.Body = JsonBuilder()
                         .Add("error", "Invalid module parameters")
                         .Add("message", Error.what())
                         .ToString();
                    return response;
               }
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(AutoDeleterRuntimeModule)
