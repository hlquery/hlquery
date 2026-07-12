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
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "core/hlquery.h"
#include "core/modules.h"
#include "runtime/timers.h"
#include "search/lang.h"
#include "search/cstore.h"
#include "vendor/json/json.hpp"

/* Periodically refreshes collection-level language metadata. */

class LangUpdaterRuntimeModule final : public AutoRuntimeModule<LangUpdaterRuntimeModule>
{
   private:

     static constexpr uint64_t OneHourMS = 60ULL * 60ULL * 1000ULL;
     static constexpr uint64_t SixHoursMS = 6ULL * OneHourMS;
     static constexpr uint64_t TwelveHoursMS = 12ULL * OneHourMS;
     static constexpr uint64_t OneDayMS = 24ULL * OneHourMS;

     mutable std::mutex StateMutex;

     std::atomic<bool> Stopping{false};
     std::atomic<bool> Running{false};
     std::atomic<uint64_t> TimerGeneration{0};

     bool Enabled = true;
     bool RunOnStartup = false;

     size_t MaxCollectionsPerPass = 0;
     size_t MaxDocumentsPerCollection = 128;

     size_t HourlyThreshold = 100;
     size_t SixHourThreshold = 25;
     size_t TwelveHourThreshold = 5;

     uint64_t CurrentIntervalMS = OneDayMS;
     uint64_t LastStartedAt = 0;
     uint64_t LastCompletedAt = 0;
     uint64_t LastDurationMS = 0;
     uint64_t LastScheduledDelayMS = 0;
     size_t LastCollectionsInsertedPerDay = 0;
     size_t LastCollectionsScanned = 0;
     size_t LastCollectionsUpdated = 0;
     size_t LastCollectionsSkipped = 0;
     std::string LastError;

     static uint64_t NowMS()
     {
          if (Instance)
          {
               return static_cast<uint64_t>(std::max<long long>(0, Instance->NowMs()));
          }

          return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
     }

     static std::string TrimCopy(const std::string& Value)
     {
          const size_t Start = Value.find_first_not_of(" \t\r\n");

          if (Start == std::string::npos)
          {
               return "";
          }

          const size_t End = Value.find_last_not_of(" \t\r\n");
          return Value.substr(Start, End - Start + 1);
     }

     static std::string NormalizeLanguageValue(const std::string& Value)
     {
          std::string Normalized = TrimCopy(Value);
          std::transform(Normalized.begin(),
                         Normalized.end(),
                         Normalized.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });
          return Normalized;
     }

     static bool ParseCollectionCreatedAtSeconds(const std::string& Collection, uint64_t& CreatedAtSeconds)
     {
          CreatedAtSeconds = 0;

          const std::string MetaValue = HybridStorageManagerInstance().Get("collection_meta:" + Collection);

          if (MetaValue.empty())
          {
               return false;
          }

          const size_t Colon = MetaValue.find(':');

          if (Colon == std::string::npos || Colon + 1 >= MetaValue.size())
          {
               return false;
          }

          try
          {
               CreatedAtSeconds = std::stoull(MetaValue.substr(Colon + 1));
               return CreatedAtSeconds > 0;
          }
          catch (...)
          {
               CreatedAtSeconds = 0;
               return false;
          }
     }

     size_t CountCollectionsCreatedInLastDay(const std::vector<std::string>& Collections) const
     {
          const time_t NowSeconds = Instance ? Instance->Time() : std::time(nullptr);

          if (NowSeconds <= 0)
          {
               return 0;
          }

          const uint64_t CutoffSeconds = static_cast<uint64_t>(NowSeconds) > (OneDayMS / 1000ULL)
               ? static_cast<uint64_t>(NowSeconds) - (OneDayMS / 1000ULL)
               : 0;

          size_t Count = 0;

          for (const auto& Collection : Collections)
          {
               uint64_t CreatedAtSeconds = 0;

               if (ParseCollectionCreatedAtSeconds(Collection, CreatedAtSeconds) &&
                   CreatedAtSeconds >= CutoffSeconds)
               {
                    ++Count;
               }
          }

          return Count;
     }

     uint64_t SelectIntervalMS(size_t CollectionsInsertedPerDay) const
     {
          if (CollectionsInsertedPerDay >= HourlyThreshold)
          {
               return OneHourMS;
          }

          if (CollectionsInsertedPerDay >= SixHourThreshold)
          {
               return SixHoursMS;
          }

          if (CollectionsInsertedPerDay >= TwelveHourThreshold)
          {
               return TwelveHoursMS;
          }

          return OneDayMS;
     }

     void ScheduleNext(uint64_t DelayMS)
     {
          if (!Enabled || Stopping.load(std::memory_order_acquire) || !Instance || !Instance->Timers)
          {
               return;
          }

          const uint64_t Generation = TimerGeneration.load(std::memory_order_acquire);

          {
               std::lock_guard<std::mutex> Lock(StateMutex);
               LastScheduledDelayMS = DelayMS;
          }

          Instance->Timers->Add(
               [this, Generation]()
               {
                    if (Stopping.load(std::memory_order_acquire) ||
                        Generation != TimerGeneration.load(std::memory_order_acquire))
                    {
                         return;
                    }

                    RunUpdatePass(true);
               },
               std::chrono::milliseconds(static_cast<int64_t>(DelayMS)),
               false);
     }

     uint64_t GetCurrentIntervalMS() const
     {
          std::lock_guard<std::mutex> Lock(StateMutex);
          return CurrentIntervalMS;
     }

     bool RunUpdatePass(bool ScheduleAfter, bool Force = false)
     {
          if ((!Enabled && !Force) || Stopping.load(std::memory_order_acquire))
          {
               return false;
          }

          bool Expected = false;

          if (!Running.compare_exchange_strong(Expected, true, std::memory_order_acq_rel))
          {
               if (ScheduleAfter)
               {
                    ScheduleNext(GetCurrentIntervalMS());
               }

               return false;
          }

          const uint64_t StartedAt = NowMS();
          size_t CollectionsInsertedPerDay = 0;
          size_t CollectionsScanned = 0;
          size_t CollectionsUpdated = 0;
          size_t CollectionsSkipped = 0;
          std::string Error;

          struct RunningGuard
          {
               std::atomic<bool> &Flag;

               ~RunningGuard()
               {
                    Flag.store(false, std::memory_order_release);
               }
          } Guard{Running};

          try
          {
               std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();
               CollectionsInsertedPerDay = CountCollectionsCreatedInLastDay(Collections);

               if (MaxCollectionsPerPass > 0 && Collections.size() > MaxCollectionsPerPass)
               {
                    Collections.resize(MaxCollectionsPerPass);
               }

               for (const auto& Collection : Collections)
               {
                    if (Stopping.load(std::memory_order_acquire))
                    {
                         break;
                    }

                    CollectionConfig Config;

                    if (!HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
                    {
                         ++CollectionsSkipped;
                         continue;
                    }

                    const auto LangIt = Config.Metadata.find("_lang");
                    const std::string ExistingLanguage =
                         LangIt == Config.Metadata.end() ? "" : NormalizeLanguageValue(LangIt->second);
                    const std::string DetectedLanguage =
                         NormalizeLanguageValue(DetectCollectionLanguage(Collection, MaxDocumentsPerCollection));

                    ++CollectionsScanned;

                    if (DetectedLanguage.empty() || DetectedLanguage == "und")
                    {
                         ++CollectionsSkipped;
                         continue;
                    }

                    if (ExistingLanguage == DetectedLanguage)
                    {
                         continue;
                    }

                    if (HybridStorageManagerInstance().UpdateCollectionMetadata(Collection, "_lang", DetectedLanguage))
                    {
                         ++CollectionsUpdated;
                    }
                    else
                    {
                         ++CollectionsSkipped;
                    }
               }
          }
          catch (const std::bad_alloc&)
          {
               Error = "out of memory";
          }
          catch (const std::exception& Exception)
          {
               Error = Exception.what();
          }
          catch (...)
          {
               Error = "unknown error";
          }

          const uint64_t CompletedAt = NowMS();
          const uint64_t NextIntervalMS = SelectIntervalMS(CollectionsInsertedPerDay);

          {
               std::lock_guard<std::mutex> Lock(StateMutex);
               CurrentIntervalMS = NextIntervalMS;
               LastStartedAt = StartedAt;
               LastCompletedAt = CompletedAt;
               LastDurationMS = CompletedAt >= StartedAt ? CompletedAt - StartedAt : 0;
               LastCollectionsInsertedPerDay = CollectionsInsertedPerDay;
               LastCollectionsScanned = CollectionsScanned;
               LastCollectionsUpdated = CollectionsUpdated;
               LastCollectionsSkipped = CollectionsSkipped;
               LastError = Error;
          }

          if (Instance && Instance->Logs)
          {
               if (Error.empty())
               {
                    Instance->Logs->Debug("lang_updater",
                                          "Language refresh pass scanned " +
                                               std::to_string(CollectionsScanned) + " collection(s), updated " +
                                               std::to_string(CollectionsUpdated) + ", next interval " +
                                               std::to_string(NextIntervalMS / 1000ULL) + "s.");
               }
               else
               {
                    Instance->Logs->Normal("lang_updater",
                                           "Language refresh pass failed: " + Error + ".");
               }
          }

          if (ScheduleAfter)
          {
               ScheduleNext(NextIntervalMS);
          }

          return Error.empty();
     }

     nlohmann::json BuildStatusJSON() const
     {
          std::lock_guard<std::mutex> Lock(StateMutex);

          return {
               {"enabled", Enabled},
               {"running", Running.load(std::memory_order_acquire)},
               {"interval_ms", CurrentIntervalMS},
               {"interval_seconds", CurrentIntervalMS / 1000ULL},
               {"last_scheduled_delay_ms", LastScheduledDelayMS},
               {"last_started_at_ms", LastStartedAt},
               {"last_completed_at_ms", LastCompletedAt},
               {"last_duration_ms", LastDurationMS},
               {"last_collections_inserted_per_day", LastCollectionsInsertedPerDay},
               {"last_collections_scanned", LastCollectionsScanned},
               {"last_collections_updated", LastCollectionsUpdated},
               {"last_collections_skipped", LastCollectionsSkipped},
               {"last_error", LastError}};
     }

   public:

     LangUpdaterRuntimeModule() : AutoRuntimeModule("lang_updater", true)
     {
     }

     bool Start(const ServerConfig& Config, std::string& ErrorMessage) override
     {
          if (!Instance || !Instance->Timers)
          {
               ErrorMessage = "lang_updater module requires the timer manager.";
               return false;
          }

          Stopping.store(false, std::memory_order_release);
          Running.store(false, std::memory_order_release);
          TimerGeneration.fetch_add(1, std::memory_order_acq_rel);

          auto Tag = Config.GetConfigReader().GetTag("lang_updater");

          if (Tag)
          {
               Enabled = Tag->GetBool("enabled", Enabled);
               RunOnStartup = Tag->GetBool("run_on_startup", RunOnStartup);
               MaxCollectionsPerPass = static_cast<size_t>(std::max(0, Tag->GetInt("max_collections_per_pass", 0)));
               MaxDocumentsPerCollection = static_cast<size_t>(
                    std::clamp(Tag->GetInt("max_documents_per_collection", 128), 1, 10000));
               HourlyThreshold = static_cast<size_t>(std::max(1, Tag->GetInt("hourly_threshold", 100)));
               SixHourThreshold = static_cast<size_t>(std::max(1, Tag->GetInt("six_hour_threshold", 25)));
               TwelveHourThreshold = static_cast<size_t>(std::max(1, Tag->GetInt("twelve_hour_threshold", 5)));
          }

          if (SixHourThreshold > HourlyThreshold)
          {
               SixHourThreshold = HourlyThreshold;
          }

          if (TwelveHourThreshold > SixHourThreshold)
          {
               TwelveHourThreshold = SixHourThreshold;
          }

          if (Enabled)
          {
               ScheduleNext(RunOnStartup ? 1000ULL : CurrentIntervalMS);
          }

          return true;
     }

     void Stop() override
     {
          Stopping.store(true, std::memory_order_release);
          TimerGeneration.fetch_add(1, std::memory_order_acq_rel);
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          ModuleCommandSpec Status;
          Status.Route = "status";
          Status.Summary = "Show language updater status.";
          Status.Syntax = "hlquery-cli module lang_updater status";
          Status.MinParameters = 0;
          Status.MaxParameters = 0;

          ModuleCommandSpec Run;
          Run.Route = "run";
          Run.Summary = "Run a language refresh pass now.";
          Run.Syntax = "hlquery-cli module lang_updater run";
          Run.MinParameters = 0;
          Run.MaxParameters = 0;

          ModuleCommandSpec Interval;
          Interval.Route = "interval";
          Interval.Summary = "Show the next refresh interval decision.";
          Interval.Syntax = "hlquery-cli module lang_updater interval";
          Interval.MinParameters = 0;
          Interval.MaxParameters = 0;

          return {Status, Run, Interval};
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest& Request) override
     {
          ModuleCommandResponse Response;
          Response.ContentType = "application/json";

          if (Request.Route == "status" || Request.Route.empty())
          {
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Message = "OK";
               Response.Body = BuildStatusJSON().dump();
               return Response;
          }

          if (Request.Route == "run")
          {
               const bool Completed = RunUpdatePass(false, true);
               Response.Success = Completed;
               Response.StatusCode = Completed ? 200 : 409;
               Response.Message = Completed ? "Language update pass completed." : "Language update pass did not complete.";
               Response.Body = BuildStatusJSON().dump();
               return Response;
          }

          if (Request.Route == "interval")
          {
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Message = "OK";
               Response.Body = BuildStatusJSON().dump();
               return Response;
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(LangUpdaterRuntimeModule)
