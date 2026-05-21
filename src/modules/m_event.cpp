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
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "core/hlquery.h"
#include "core/modules.h"
#include "search/cstore.h"
#include "vendor/json/json.hpp"

/* Default prefix used for day-bucketed event collections. */

static constexpr const char* DefaultEventPrefix = "events-";

/* Default prefix used for day-bucketed search collections. */

static constexpr const char* DefaultSearchPrefix = "searches-";

/* Default number of queued records flushed in one batch. */

static constexpr unsigned int DefaultBatchSize = 16;

/* Return whether one string starts with another string. */

static bool StartsWith(const std::string& Value, const std::string& Prefix)
{
     return Value.size() >= Prefix.size() && Value.compare(0, Prefix.size(), Prefix) == 0;
}

/* Convert one boolean value into the serialized form stored in event documents. */

static std::string BoolString(bool Value)
{
     return Value ? "true" : "false";
}

/* Runtime module that writes internal event documents into day-bucketed collections. */

class EventRuntimeModule final : public AutoRuntimeModule<EventRuntimeModule>
{
   private:

     /* One queued event or search record waiting for batch flush. */

     struct PendingRecord
     {
          std::string StoragePrefix;
          std::string EventType;
          std::string SubjectCollection;
          nlohmann::json Payload;
     };

     std::string EventPrefix = DefaultEventPrefix;
     std::string SearchPrefix = DefaultSearchPrefix;

     unsigned int BatchSize = DefaultBatchSize;

     std::mutex CollectionCreateMutex;
     std::mutex QueueMutex;

     std::atomic<uint64_t> Sequence{0};
     std::atomic<uint64_t> BatchSequence{0};

     std::vector<PendingRecord> PendingRecords;

     /* Return the current local day in YYYY-MM-DD form. */

     std::string CurrentDayString() const
     {
          const std::time_t Now = Instance ? Instance->Time() : Time();

          std::tm LocalTime{};

#if defined(_WIN32)
          localtime_s(&LocalTime, &Now);
#else
          localtime_r(&Now, &LocalTime);
#endif

          char Buffer[11];

          if (std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%d", &LocalTime) == 0)
          {
               return "unknown-day";
          }

          return Buffer;
     }

     /* Return the current local timestamp in RFC3339-like form. */

     std::string CurrentTimestampString() const
     {
          const auto NowMS = Instance ? Instance->NowMs() : NowMs();
          const auto Now = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
          const auto Millis = std::chrono::duration_cast<std::chrono::milliseconds>(Now.time_since_epoch()) % 1000;
          const std::time_t NowTime = std::chrono::system_clock::to_time_t(Now);

          std::tm LocalTime{};

#if defined(_WIN32)
          localtime_s(&LocalTime, &NowTime);
#else
          localtime_r(&NowTime, &LocalTime);
#endif

          char Buffer[32];

          if (std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%dT%H:%M:%S", &LocalTime) == 0)
          {
               return "unknown-time";
          }

          return std::string(Buffer) + "." + std::to_string(static_cast<long long>(Millis.count()));
     }

     /* Build the internal collection name for one day and storage prefix. */

     std::string BuildStorageCollectionName(const std::string& Prefix, const std::string& Day) const
     {
          return Prefix + Day;
     }

     /* Build one unique document identifier for a queued record. */

     std::string BuildDocumentID(const std::string& Day)
     {
          const uint64_t NowSeconds = static_cast<uint64_t>(Instance ? Instance->Time() : Time());
          const uint64_t LocalSequence = Sequence.fetch_add(1, std::memory_order_relaxed);
          return Day + "-" + std::to_string(NowSeconds) + "-" + std::to_string(LocalSequence);
     }

     /* Build one unique batch tag shared by all records flushed together. */

     std::string BuildBatchTag(const std::string& Day)
     {
          const uint64_t LocalSequence = BatchSequence.fetch_add(1, std::memory_order_relaxed);
          return "batch-" + Day + "-" + std::to_string(LocalSequence);
     }

     /* Return whether one collection belongs to this module's internal storage. */

     bool IsInternalStorageCollection(const std::string& Collection) const
     {
          return StartsWith(Collection, EventPrefix) || StartsWith(Collection, SearchPrefix);
     }

     /* Ensure the target collection exists before inserting one flushed record. */

     bool EnsureStorageCollection(const std::string& CollectionName, const std::string& StoragePrefix)
     {
          if (HybridStorageManagerInstance().CollectionExists(CollectionName))
          {
               return true;
          }

          std::lock_guard<std::mutex> Lock(CollectionCreateMutex);

          if (HybridStorageManagerInstance().CollectionExists(CollectionName))
          {
               return true;
          }

          CollectionConfig Config;

          Config.Name = CollectionName;
          Config.Fields["event_type"] = "string";
          Config.Fields["subject_collection"] = "string";
          Config.Fields["query"] = "string";
          Config.Fields["day"] = "string";
          Config.Fields["at"] = "string";
          Config.Fields["requester_ip"] = "string";
          Config.Fields["requester_user"] = "string";
          Config.Fields["authenticated"] = "string";
          Config.Fields["search_time_ms"] = "int64";
          Config.Fields["found"] = "int64";
          Config.Fields["returned"] = "int64";
          Config.Fields["distributed"] = "string";
          Config.Fields["batch_tag"] = "string";
          Config.Fields["batch_size"] = "int64";
          Config.Fields["batch_index"] = "int64";
          Config.Metadata["_module"] = "m_event";
          Config.Metadata["_internal"] = "true";
          Config.Metadata["_event_day"] = CollectionName.substr(StoragePrefix.size());
          Config.Metadata["_event_stream"] = StoragePrefix == SearchPrefix ? "searches" : "events";

          return HybridStorageManagerInstance().CreateCollection(CollectionName, Config);
     }

     /* Persist one batch of queued records directly into storage. */

     void FlushRecords(std::vector<PendingRecord>& Records)
     {
          if (Records.empty())
          {
               return;
          }

          const std::string Day = CurrentDayString();
          const std::string BatchTag = BuildBatchTag(Day);
          const std::string BatchSizeValue = std::to_string(static_cast<unsigned long long>(Records.size()));

          for (std::size_t Index = 0; Index < Records.size(); ++Index)
          {
               PendingRecord& Record = Records[Index];
               const std::string CollectionName = BuildStorageCollectionName(Record.StoragePrefix, Day);

               if (!EnsureStorageCollection(CollectionName, Record.StoragePrefix))
               {
                    continue;
               }

               Record.Payload["batch_tag"] = BatchTag;
               Record.Payload["batch_size"] = BatchSizeValue;
               Record.Payload["batch_index"] = std::to_string(static_cast<unsigned long long>(Index));

               Document Doc;

               Doc.Timestamp = static_cast<uint64_t>(Instance ? Instance->Time() : Time());
               Doc.ID = BuildDocumentID(Day);
               Doc.Title = Record.EventType;
               Doc.Content = Record.Payload.dump();
               Doc.Fields["event_type"] = Record.EventType;
               Doc.Fields["subject_collection"] = Record.SubjectCollection;
               Doc.Fields["query"] = Record.Payload.value("query", "");
               Doc.Fields["day"] = Day;
               Doc.Fields["at"] = Record.Payload.value("at", "");
               Doc.Fields["requester_ip"] = Record.Payload.value("requester_ip", "");
               Doc.Fields["requester_user"] = Record.Payload.value("requester_user", "");
               Doc.Fields["authenticated"] = Record.Payload.value("authenticated", "");
               Doc.Fields["search_time_ms"] = Record.Payload.value("search_time_ms", "");
               Doc.Fields["found"] = Record.Payload.value("found", "");
               Doc.Fields["returned"] = Record.Payload.value("returned", "");
               Doc.Fields["distributed"] = Record.Payload.value("distributed", "");
               Doc.Fields["batch_tag"] = BatchTag;
               Doc.Fields["batch_size"] = BatchSizeValue;
               Doc.Fields["batch_index"] = std::to_string(static_cast<unsigned long long>(Index));

               (void)HybridStorageManagerInstance().AddDocument(CollectionName, Doc);
          }
     }

     /* Flush queued records when the queue reached the configured threshold. */

     void FlushPendingIfReady()
     {
          std::vector<PendingRecord> Records;

          {
               std::lock_guard<std::mutex> Lock(QueueMutex);

               if (PendingRecords.size() < BatchSize)
               {
                    return;
               }

               const std::size_t FlushCount = std::min<std::size_t>(PendingRecords.size(), static_cast<std::size_t>(BatchSize));

               Records.assign(PendingRecords.begin(), PendingRecords.begin() + static_cast<std::vector<PendingRecord>::difference_type>(FlushCount));
               PendingRecords.erase(PendingRecords.begin(), PendingRecords.begin() + static_cast<std::vector<PendingRecord>::difference_type>(FlushCount));
          }

          FlushRecords(Records);
     }

     /* Flush all queued records regardless of the current queue length. */

     void FlushAllPending()
     {
          std::vector<PendingRecord> Records;

          {
               std::lock_guard<std::mutex> Lock(QueueMutex);

               if (PendingRecords.empty())
               {
                    return;
               }

               Records.swap(PendingRecords);
          }

          FlushRecords(Records);
     }

     /* Queue one record for batched persistence. */

     void QueueRecord(const std::string& StoragePrefix,
                      const std::string& EventType,
                      const std::string& SubjectCollection,
                      const nlohmann::json& Payload)
     {
          {
               std::lock_guard<std::mutex> Lock(QueueMutex);

               PendingRecord Record;

               Record.StoragePrefix = StoragePrefix;
               Record.EventType = EventType;
               Record.SubjectCollection = SubjectCollection;
               Record.Payload = Payload;

               PendingRecords.push_back(std::move(Record));
          }

          FlushPendingIfReady();
     }

     /* Build the common payload fields used by all stored records. */

     nlohmann::json BuildBasePayload(const std::string& EventType,
                                     const std::string& SubjectCollection,
                                     const std::string& RequesterIP,
                                     const std::string& RequesterUser,
                                     bool Authenticated) const
     {
          nlohmann::json Payload = nlohmann::json::object();

          Payload["event_type"] = EventType;
          Payload["subject_collection"] = SubjectCollection;
          Payload["day"] = CurrentDayString();
          Payload["at"] = CurrentTimestampString();
          Payload["requester_ip"] = RequesterIP;
          Payload["requester_user"] = RequesterUser;
          Payload["authenticated"] = BoolString(Authenticated);
          return Payload;
     }

   public:

     /* Construct the event runtime module. */

     EventRuntimeModule()
          : AutoRuntimeModule("event", false)
     {

     }

     /* Load event-module configuration. */

     bool Start(const ServerConfig& Config, std::string&) override
     {
          auto Tag = Config.GetConfigReader().GetTag("event");

          if (Tag)
          {
               const std::string ConfiguredEventPrefix = Tag->GetString("prefix", EventPrefix);
               const std::string ConfiguredSearchPrefix = Tag->GetString("search_prefix", SearchPrefix);
               const int ConfiguredBatchSize = Tag->GetInt("batch_size", static_cast<int>(BatchSize));

               if (!ConfiguredEventPrefix.empty())
               {
                    EventPrefix = ConfiguredEventPrefix;
               }

               if (!ConfiguredSearchPrefix.empty())
               {
                    SearchPrefix = ConfiguredSearchPrefix;
               }

               if (ConfiguredBatchSize > 0)
               {
                    BatchSize = static_cast<unsigned int>(ConfiguredBatchSize);
               }
          }

          return true;
     }

     /* Stop the event runtime module. */

     void Stop() override
     {
          FlushAllPending();
     }

     /* Flush incomplete batches periodically so queued records do not remain buffered indefinitely. */

     void OnEveryOneMinute() override
     {
          FlushAllPending();
     }

     /* Record successful collection-creation events. */

     void OnCreateCollection(const std::string& Collection,
                             const std::string& RequesterIP,
                             const std::string& RequesterUser,
                             bool Authenticated) override
     {
          if (IsInternalStorageCollection(Collection))
          {
               return;
          }

          nlohmann::json Payload = BuildBasePayload("create_collection", Collection, RequesterIP, RequesterUser, Authenticated);

          QueueRecord(EventPrefix, "create_collection", Collection, Payload);
     }

     /* Record successful collection-deletion events. */

     void OnDeleteCollection(const std::string& Collection,
                             const std::string& RequesterIP,
                             const std::string& RequesterUser,
                             bool Authenticated) override
     {
          if (IsInternalStorageCollection(Collection))
          {
               return;
          }

          nlohmann::json Payload = BuildBasePayload("delete_collection", Collection, RequesterIP, RequesterUser, Authenticated);

          QueueRecord(EventPrefix, "delete_collection", Collection, Payload);
     }

     /* Record collection-search events in the search stream. */

     void OnSearchCollection(const SearchEvent& Event) override
     {
          if (IsInternalStorageCollection(Event.Collection))
          {
               return;
          }

          nlohmann::json Payload = BuildBasePayload("search_collection",
                                                    Event.Collection,
                                                    Event.RequesterIP,
                                                    Event.RequesterUser,
                                                    Event.Authenticated);

          Payload["query"] = Event.Query;
          Payload["analytics_tag"] = Event.AnalyticsTag;
          Payload["search_time_ms"] = std::to_string(Event.SearchTimeMS);
          Payload["found"] = std::to_string(Event.Found);
          Payload["returned"] = std::to_string(Event.Returned);
          Payload["distributed"] = BoolString(Event.Distributed);

          QueueRecord(SearchPrefix, "search_collection", Event.Collection, Payload);
     }

     /* Record document-search events in the search stream. */

     void OnSearchDocument(const SearchEvent& Event) override
     {
          if (IsInternalStorageCollection(Event.Collection))
          {
               return;
          }

          nlohmann::json Payload = BuildBasePayload("search_document",
                                                    Event.Collection,
                                                    Event.RequesterIP,
                                                    Event.RequesterUser,
                                                    Event.Authenticated);

          Payload["query"] = Event.Query;
          Payload["analytics_tag"] = Event.AnalyticsTag;
          Payload["search_time_ms"] = std::to_string(Event.SearchTimeMS);
          Payload["found"] = std::to_string(Event.Found);
          Payload["returned"] = std::to_string(Event.Returned);
          Payload["distributed"] = BoolString(Event.Distributed);

          QueueRecord(SearchPrefix, "search_document", Event.Collection, Payload);
     }
};

MODULE_LOAD(EventRuntimeModule)
