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
#include <cstdint>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "api/searchapi.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "search/cstore.h"
#include "sam/sam.h"

     constexpr const char* kSAMGlobalLexicalScope = "__global__";
     constexpr uint64_t kSAMInteractionFlushIntervalMs = 60ULL * 60ULL * 1000ULL;
     std::mutex StartupSweepMutex;
     bool StartupSweepActive = false;
     std::set<std::string> StartupSweepStartedCollections;
     uint64_t LastInteractionFlushMS = 0;

     void RefreshSAMSearchIdeaProfiles(const std::string &LogSource)
     {
          if (!Instance || !Instance->Config || !Instance->Sam || !Instance->Sam->IsOpen())
          {
               return;
          }

          size_t UpdatedCollections = 0;

          for (const auto& Collection : HybridStorageManager::GetInstance().ListCollections())
          {
               if (!HybridStorageManager::GetInstance().CollectionExists(Collection))
               {
                    continue;
               }

               bool Updated = false;
               std::string ErrorMessage;

               if (!Instance->Sam->RefreshCollectionProfileFromSearchIdeas(Collection, &Updated, &ErrorMessage))
               {
                    if (Instance->Logs && !ErrorMessage.empty())
                    {
                         Instance->Logs->Normal(LogSource,
                                                "Failed to refresh SAM search-idea profile for collection '" +
                                                     Collection + "': " + ErrorMessage + ".");
                    }

                    continue;
               }

               if (Updated)
               {
                    ++UpdatedCollections;

                    if (Instance->Logs)
                    {
                         Instance->Logs->Debug(LogSource,
                                               "Refreshed SAM learned search-idea profile for collection '" +
                                                    Collection + "'.");
                    }
               }
          }

          if (UpdatedCollections > 1 && Instance->Logs)
          {
               Instance->Logs->Debug(LogSource,
                                     "Refreshed SAM learned search-idea profiles for " +
                                          std::to_string(UpdatedCollections) + " collection(s).");
          }
     }

     void TriggerSAMAutoIndex(const std::string &LogSource, bool ForceStartupSweep)
     {
          if (!Instance || !Instance->Config || !Instance->Config->GetSamEnabled() ||
              !Instance->Config->GetSamIndexAll() || !Instance->Sam || !Instance->Sam->IsOpen() ||
              !Instance->API)
          {
               return;
          }

          const uint64_t NowMS = static_cast<uint64_t>(NowMs());
          const uint64_t PauseUntilMS = Instance->Sam->GetAutoIndexPauseUntilMS();

          if (PauseUntilMS > 0 && NowMS < PauseUntilMS)
          {
               return;
          }

          const size_t MaxParallelJobs = 1;
          size_t AvailableSlots = MaxParallelJobs;
          const size_t RunningJobs = Instance->Sam->GetRunningCollectionJobCount();

          if (RunningJobs >= MaxParallelJobs)
          {
               AvailableSlots = 0;
          }
          else
          {
               AvailableSlots = MaxParallelJobs - RunningJobs;
          }

          const std::vector<std::string> Collections =
               HybridStorageManager::GetInstance().ListCollections();

          if (ForceStartupSweep)
          {
               std::lock_guard<std::mutex> Lock(StartupSweepMutex);
               StartupSweepActive = true;
          }

          std::vector<std::string> StartupSweepCollections;

          {
               std::lock_guard<std::mutex> Lock(StartupSweepMutex);

               if (StartupSweepActive && AvailableSlots > 0)
               {
                    for (const auto &Collection : Collections)
                    {
                         if (StartupSweepStartedCollections.count(Collection) > 0)
                         {
                              continue;
                         }

                         if (HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection) == 0)
                         {
                              StartupSweepStartedCollections.insert(Collection);
                              continue;
                         }

                         StartupSweepCollections.push_back(Collection);

                         if (StartupSweepCollections.size() >= AvailableSlots)
                         {
                              break;
                         }
                    }

                    if (StartupSweepCollections.empty())
                    {
                         StartupSweepActive = false;
                         StartupSweepStartedCollections.clear();
                    }
               }
          }

          if (!StartupSweepCollections.empty())
          {
               size_t QueuedCount = 0;

               for (const auto& StartupSweepCollection : StartupSweepCollections)
               {
                    bool AlreadyRunning = false;
                    std::string ErrorMessage;

                    if (!Instance->Sam->StartRecreateCollectionAsync(StartupSweepCollection, &AlreadyRunning, &ErrorMessage, "auto-index startup", false))
                    {
                         if (Instance->Logs && !ErrorMessage.empty())
                         {
                              Instance->Logs->Normal(LogSource,
                                                     "Failed to queue SAM startup sweep for collection '" +
                                                          StartupSweepCollection + "': " + ErrorMessage + ".");
                         }

                         std::lock_guard<std::mutex> Lock(StartupSweepMutex);
                         StartupSweepStartedCollections.insert(StartupSweepCollection);
                         continue;
                    }

                    if (AlreadyRunning)
                    {
                         std::lock_guard<std::mutex> Lock(StartupSweepMutex);
                         StartupSweepStartedCollections.insert(StartupSweepCollection);
                         continue;
                    }

                    {
                         std::lock_guard<std::mutex> Lock(StartupSweepMutex);
                         StartupSweepStartedCollections.insert(StartupSweepCollection);
                    }

                    ++QueuedCount;

                    if (Instance->Logs)
                    {
                         Instance->Logs->Debug(LogSource,
                                               "Queued SAM startup sweep for collection '" +
                                                    StartupSweepCollection + "'.");
                    }
               }

               if (QueuedCount > 0)
               {
                    return;
               }

               return;
          }

          if (AvailableSlots == 0)
          {
               return;
          }

          size_t QueuedAutoIndexes = 0;

          for (const auto &Collection : Collections)
          {
          const size_t DocumentCount =
                    HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);
               uint64_t IndexedMutationVersion = 0;
               const bool HasIndexedVersion =
                    Instance->Sam->GetCollectionIndexedMutationVersion(Collection, IndexedMutationVersion);
               uint64_t RequestedMutationVersion = 0;
               const bool HasPendingRebuild =
                    Instance->Sam->HasPendingCollectionRebuild(Collection, &RequestedMutationVersion);
               const uint64_t CurrentMutationVersion =
                    Instance->API->GetCollectionMutationVersion(Collection);
               const bool NeedsInitialIndex = !HasIndexedVersion && DocumentCount > 0;
               const bool NeedsRefresh = HasIndexedVersion && CurrentMutationVersion > IndexedMutationVersion;
               const bool NeedsRequestedRetry =
                    HasPendingRebuild &&
                    (DocumentCount > 0 || RequestedMutationVersion > 0 || !HasIndexedVersion);

               if (!NeedsInitialIndex && !NeedsRefresh && !NeedsRequestedRetry)
               {
                    continue;
               }

               bool AlreadyRunning = false;
               std::string ErrorMessage;

               if (!Instance->Sam->StartRecreateCollectionAsync(Collection, &AlreadyRunning, &ErrorMessage, "auto-index", false))
               {
                    if (Instance->Logs && !ErrorMessage.empty())
                    {
                         Instance->Logs->Normal(LogSource,
                                                "Failed to queue SAM auto-index for collection '" +
                                                     Collection + "': " + ErrorMessage + ".");
                    }

                    continue;
               }

               if (AlreadyRunning)
               {
                    continue;
                }

                if (Instance->Logs)
                {
                    const std::string Reason = NeedsRequestedRetry
                         ? "a pending rebuild request is recorded"
                         : NeedsRefresh
                         ? "source mutation version advanced from " + std::to_string(IndexedMutationVersion) +
                              " to " + std::to_string(CurrentMutationVersion)
                         : "collection has documents but no completed SAM index state";
                    Instance->Logs->Debug(LogSource,
                                          "Queued SAM auto-index for collection '" + Collection +
                                               "' because " + Reason + ".");
                }

               ++QueuedAutoIndexes;

               if (QueuedAutoIndexes >= AvailableSlots)
               {
                    return;
               }
          }

          RefreshSAMSearchIdeaProfiles(LogSource);
     }

     void FlushSAMInteractionSignals(const std::string &LogSource, bool Force)
     {
          if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
          {
               return;
          }

          const uint64_t PauseUntilMS = Instance->Sam->GetAutoIndexPauseUntilMS();

          if (!Force && PauseUntilMS > 0)
          {
               const uint64_t NowMS = static_cast<uint64_t>(NowMs());

               if (NowMS < PauseUntilMS)
               {
                    return;
               }
          }

          const uint64_t NowMS = static_cast<uint64_t>(NowMs());

          if (!Force && LastInteractionFlushMS > 0 &&
              NowMS > LastInteractionFlushMS &&
              (NowMS - LastInteractionFlushMS) < kSAMInteractionFlushIntervalMs)
          {
               return;
          }

          const size_t Flushed = Instance->Sam->FlushPendingInteractionSignals(512);

          if (Flushed == 0 && !Force)
          {
               return;
          }

          LastInteractionFlushMS = NowMS;

          if (Flushed > 0 && Instance->Logs)
          {
               Instance->Logs->Debug(LogSource,
                                     "Flushed " + std::to_string(Flushed) +
                                          " pending SAM interaction signal(s).");
          }
     }

     void SyncSAMLexicalScope(const std::string& Collection,
                              bool GlobalScope,
                              const std::string& LogSource)
     {
          if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
          {
               return;
          }

          bool Updated = false;
          std::string ErrorMessage;
          const std::string Scope = GlobalScope ? std::string(kSAMGlobalLexicalScope) : Collection;

          if (!Instance->Sam->SyncLexicalResources(Scope, &Updated, &ErrorMessage))
          {
               if (Instance->Logs && !ErrorMessage.empty())
               {
                    Instance->Logs->Normal(LogSource,
                                           "Failed to sync SAM lexical resources for '" +
                                                Scope + "': " + ErrorMessage + ".");
               }

               return;
          }

          if (Instance->Logs && Updated)
          {
               Instance->Logs->Debug(LogSource,
                                     "Synced SAM lexical resources for '" + Scope + "'.");
          }
     }

     void QueueSAMLexicalRefresh(const std::string& Collection,
                                 bool GlobalScope,
                                 const std::string& LogSource)
     {
          if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen())
          {
               return;
          }

          SyncSAMLexicalScope(Collection, GlobalScope, LogSource);

          std::vector<std::string> Targets;

          if (GlobalScope)
          {
               Targets = HybridStorageManager::GetInstance().ListCollections();
          }
          else if (!Collection.empty())
          {
               Targets.push_back(Collection);
          }

          for (const auto& Target : Targets)
          {
               bool AlreadyRunning = false;
               std::string ErrorMessage;

               if (!Instance->Sam->StartRecreateCollectionAsync(Target, &AlreadyRunning, &ErrorMessage, "lexical refresh", false))
               {
                    if (Instance->Logs && !ErrorMessage.empty())
                    {
                         Instance->Logs->Normal(LogSource,
                                                "Failed to queue SAM lexical refresh for collection '" +
                                                     Target + "': " + ErrorMessage + ".");
                    }
                    continue;
               }
          }
     }

class CoreSAMModule final : public AutoRuntimeModule<CoreSAMModule>
{
   public:

     CoreSAMModule() : AutoRuntimeModule("core_sam")
     {

     }

     void Stop() override
     {
          FlushSAMInteractionSignals("core_sam", true);
     }

     void OnStartup() override
     {
          LastInteractionFlushMS = static_cast<uint64_t>(NowMs());
          SyncSAMLexicalScope(kSAMGlobalLexicalScope, false, "core_sam");

          for (const auto& Collection : HybridStorageManager::GetInstance().ListCollections())
          {
               SyncSAMLexicalScope(Collection, false, "core_sam");
          }

          TriggerSAMAutoIndex("core_sam", true);
     }

     void OnEveryOneMinute() override
     {
          SyncSAMLexicalScope(kSAMGlobalLexicalScope, false, "core_sam");

          FlushSAMInteractionSignals("core_sam", false);
          TriggerSAMAutoIndex("core_sam", false);
     }

     void OnUpsertSynonym(const std::string& Collection,
                          const std::string&,
                          bool GlobalScope,
                          const std::string&,
                          const std::string&,
                          bool) override
     {
          QueueSAMLexicalRefresh(Collection, GlobalScope, "core_sam");
     }

     void OnDeleteSynonym(const std::string& Collection,
                          const std::string&,
                          bool GlobalScope,
                          const std::string&,
                          const std::string&,
                          bool) override
     {
          QueueSAMLexicalRefresh(Collection, GlobalScope, "core_sam");
     }

     void OnCreateStopword(const std::string& Collection,
                           uint64_t,
                           bool GlobalScope,
                           const std::string&,
                           const std::string&,
                           bool) override
     {
          QueueSAMLexicalRefresh(Collection, GlobalScope, "core_sam");
     }

     void OnDeleteStopword(const std::string& Collection,
                           const std::string&,
                           bool GlobalScope,
                           const std::string&,
                           const std::string&,
                           bool) override
     {
          QueueSAMLexicalRefresh(Collection, GlobalScope, "core_sam");
     }
};

MODULE_LOAD(CoreSAMModule)
