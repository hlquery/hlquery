#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>

#include "core/hlquery.h"
#include "core/modules.h"
#include "search/cstore.h"
#include "vendor/json/json.hpp"

namespace
{
constexpr const char* kDefaultPrefix = "events-";

bool StartsWith(const std::string& value, const std::string& prefix)
{
     return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string BoolString(bool value)
{
     return value ? "true" : "false";
}
}

/* Runtime module that writes internal event documents into day-bucketed collections. */

class EventRuntimeModule final : public AutoRuntimeModule<EventRuntimeModule>
{
   private:

     std::string EventPrefix = kDefaultPrefix;
     std::mutex CollectionCreateMutex;
     std::atomic<uint64_t> Sequence{0};

     /* Return the current local day in YYYY-MM-DD form. */

     std::string CurrentDayString() const
     {
          const std::time_t now = std::time(nullptr);
          std::tm local_tm{};

#if defined(_WIN32)
          localtime_s(&local_tm, &now);
#else
          localtime_r(&now, &local_tm);
#endif

          char buffer[11];
          if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_tm) == 0)
          {
               return "unknown-day";
          }

          return buffer;
     }

     /* Return the current local timestamp in RFC3339-like form. */

     std::string CurrentTimestampString() const
     {
          const auto now = std::chrono::system_clock::now();
          const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
          const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
          std::tm local_tm{};

#if defined(_WIN32)
          localtime_s(&local_tm, &now_time);
#else
          localtime_r(&now_time, &local_tm);
#endif

          char buffer[32];
          if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local_tm) == 0)
          {
               return "unknown-time";
          }

          return std::string(buffer) + "." + std::to_string(static_cast<long long>(millis.count()));
     }

     /* Build the internal collection name for one day. */

     std::string BuildEventCollectionName(const std::string& day) const
     {
          return EventPrefix + day;
     }

     /* Return whether a collection belongs to this module's internal storage. */

     bool IsInternalEventCollection(const std::string& collection) const
     {
          return StartsWith(collection, EventPrefix);
     }

     /* Ensure the target day collection exists before inserting an event document. */

     bool EnsureEventCollection(const std::string& collection_name)
     {
          if (HybridStorageManagerInstance().CollectionExists(collection_name))
          {
               return true;
          }

          std::lock_guard<std::mutex> lock(CollectionCreateMutex);

          if (HybridStorageManagerInstance().CollectionExists(collection_name))
          {
               return true;
          }

          CollectionConfig config;
          config.Name = collection_name;
          config.Fields["event_type"] = "string";
          config.Fields["subject_collection"] = "string";
          config.Fields["query"] = "string";
          config.Fields["day"] = "string";
          config.Fields["at"] = "string";
          config.Fields["requester_ip"] = "string";
          config.Fields["requester_user"] = "string";
          config.Fields["authenticated"] = "string";
          config.Fields["search_time_ms"] = "int64";
          config.Fields["found"] = "int64";
          config.Fields["returned"] = "int64";
          config.Fields["distributed"] = "string";
          config.Metadata["_module"] = "m_event";
          config.Metadata["_internal"] = "true";
          config.Metadata["_event_day"] = collection_name.substr(EventPrefix.size());

          return HybridStorageManagerInstance().CreateCollection(collection_name, config);
     }

     /* Persist one event document directly into storage. */

     void InsertEvent(const std::string& event_type,
                      const std::string& subject_collection,
                      const nlohmann::json& payload)
     {
          const std::string day = CurrentDayString();
          const std::string event_collection = BuildEventCollectionName(day);

          if (!EnsureEventCollection(event_collection))
          {
               return;
          }

          Document doc;
          doc.Timestamp = static_cast<uint64_t>(std::time(nullptr));
          doc.ID = day + "-" + std::to_string(doc.Timestamp) + "-" + std::to_string(Sequence.fetch_add(1, std::memory_order_relaxed));
          doc.Title = event_type;
          doc.Content = payload.dump();
          doc.Fields["event_type"] = event_type;
          doc.Fields["subject_collection"] = subject_collection;
          doc.Fields["query"] = payload.value("query", "");
          doc.Fields["day"] = day;
          doc.Fields["at"] = payload.value("at", "");
          doc.Fields["requester_ip"] = payload.value("requester_ip", "");
          doc.Fields["requester_user"] = payload.value("requester_user", "");
          doc.Fields["authenticated"] = payload.value("authenticated", "");
          doc.Fields["search_time_ms"] = payload.value("search_time_ms", "");
          doc.Fields["found"] = payload.value("found", "");
          doc.Fields["returned"] = payload.value("returned", "");
          doc.Fields["distributed"] = payload.value("distributed", "");

          (void)HybridStorageManagerInstance().AddDocument(event_collection, doc);
     }

     /* Build the common payload fields used by all event documents. */

     nlohmann::json BuildBasePayload(const std::string& event_type,
                                     const std::string& subject_collection,
                                     const std::string& requester_ip,
                                     const std::string& requester_user,
                                     bool authenticated) const
     {
          nlohmann::json payload = nlohmann::json::object();
          payload["event_type"] = event_type;
          payload["subject_collection"] = subject_collection;
          payload["day"] = CurrentDayString();
          payload["at"] = CurrentTimestampString();
          payload["requester_ip"] = requester_ip;
          payload["requester_user"] = requester_user;
          payload["authenticated"] = BoolString(authenticated);
          return payload;
     }

   public:

     EventRuntimeModule()
         : AutoRuntimeModule("event", false)
     {
     }

     bool Start(const ServerConfig& Config, std::string&) override
     {
          auto tag = Config.GetConfigReader().GetTag("event");
          if (tag)
          {
               const std::string configured_prefix = tag->GetString("prefix", EventPrefix);
               if (!configured_prefix.empty())
               {
                    EventPrefix = configured_prefix;
               }
          }

          return true;
     }

     void Stop() override
     {
     }

     void OnCreateCollection(const std::string& Collection, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated) override
     {
          if (IsInternalEventCollection(Collection))
          {
               return;
          }

          nlohmann::json payload = BuildBasePayload("create_collection", Collection, RequesterIP, RequesterUser, Authenticated);
          InsertEvent("create_collection", Collection, payload);
     }

     void OnDeleteCollection(const std::string& Collection, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated) override
     {
          if (IsInternalEventCollection(Collection))
          {
               return;
          }

          nlohmann::json payload = BuildBasePayload("delete_collection", Collection, RequesterIP, RequesterUser, Authenticated);
          InsertEvent("delete_collection", Collection, payload);
     }

     void OnSearchCollection(const SearchEvent& Event) override
     {
          if (IsInternalEventCollection(Event.Collection))
          {
               return;
          }

          nlohmann::json payload = BuildBasePayload("search_collection",
                                                    Event.Collection,
                                                    Event.RequesterIP,
                                                    Event.RequesterUser,
                                                    Event.Authenticated);
          payload["query"] = Event.Query;
          payload["analytics_tag"] = Event.AnalyticsTag;
          payload["search_time_ms"] = std::to_string(Event.SearchTimeMS);
          payload["found"] = std::to_string(Event.Found);
          payload["returned"] = std::to_string(Event.Returned);
          payload["distributed"] = BoolString(Event.Distributed);
          InsertEvent("search_collection", Event.Collection, payload);
     }

     void OnSearchDocument(const SearchEvent& Event) override
     {
          if (IsInternalEventCollection(Event.Collection))
          {
               return;
          }

          nlohmann::json payload = BuildBasePayload("search_document",
                                                    Event.Collection,
                                                    Event.RequesterIP,
                                                    Event.RequesterUser,
                                                    Event.Authenticated);
          payload["query"] = Event.Query;
          payload["analytics_tag"] = Event.AnalyticsTag;
          payload["search_time_ms"] = std::to_string(Event.SearchTimeMS);
          payload["found"] = std::to_string(Event.Found);
          payload["returned"] = std::to_string(Event.Returned);
          payload["distributed"] = BoolString(Event.Distributed);
          InsertEvent("search_document", Event.Collection, payload);
     }
};

MODULE_LOAD(EventRuntimeModule)
