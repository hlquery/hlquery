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
#include "search/sam/sam.h"

namespace
{
     std::mutex StartupSweepMutex;
     bool StartupSweepActive = false;
     std::set<std::string> StartupSweepStartedCollections;

     void RefreshSAMSearchIdeaProfile(const std::string &LogSource)
     {
          if (!Instance || !Instance->Config || !Instance->Sam || !Instance->Sam->IsOpen())
          {
               return;
          }

          const std::string Collection = Instance->Config->GetSamSearchIdeasCollection();

          if (Collection.empty() || !HybridStorageManager::GetInstance().CollectionExists(Collection))
          {
               return;
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

               return;
          }

          if (Updated && Instance->Logs)
          {
               Instance->Logs->Debug(LogSource,
                                     "Refreshed SAM learned search-idea profile for collection '" +
                                          Collection + "'.");
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

          const std::map<std::string, SAM::CollectionJobStatus> AllStatuses =
               Instance->Sam->GetAllCollectionJobStatuses();

          for (const auto &Entry : AllStatuses)
          {
               if (Entry.second.Running)
               {
                    return;
               }
          }

          const std::vector<std::string> Collections =
               HybridStorageManager::GetInstance().ListCollections();

          if (ForceStartupSweep)
          {
               std::lock_guard<std::mutex> Lock(StartupSweepMutex);
               StartupSweepActive = true;
          }

          bool ContinueStartupSweep = false;
          std::string StartupSweepCollection;

          {
               std::lock_guard<std::mutex> Lock(StartupSweepMutex);

               if (StartupSweepActive)
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

                         StartupSweepCollection = Collection;
                         break;
                    }

                    if (!StartupSweepCollection.empty())
                    {
                         ContinueStartupSweep = true;
                    }
                    else
                    {
                         StartupSweepActive = false;
                         StartupSweepStartedCollections.clear();
                    }
               }
          }

          if (ContinueStartupSweep)
          {
               bool AlreadyRunning = false;
               std::string ErrorMessage;

               if (!Instance->Sam->StartRecreateCollectionAsync(StartupSweepCollection, &AlreadyRunning, &ErrorMessage))
               {
                    if (Instance->Logs && !ErrorMessage.empty())
                    {
                         Instance->Logs->Normal(LogSource,
                                                "Failed to queue SAM startup sweep for collection '" +
                                                     StartupSweepCollection + "': " + ErrorMessage + ".");
                    }

                    std::lock_guard<std::mutex> Lock(StartupSweepMutex);
                    StartupSweepStartedCollections.insert(StartupSweepCollection);
                    return;
               }

               if (AlreadyRunning)
               {
                    return;
               }

               {
                    std::lock_guard<std::mutex> Lock(StartupSweepMutex);
                    StartupSweepStartedCollections.insert(StartupSweepCollection);
               }

               if (Instance->Logs)
               {
                    Instance->Logs->Debug(LogSource,
                                          "Queued SAM startup sweep for collection '" +
                                               StartupSweepCollection + "'.");
               }

               return;
          }

          for (const auto &Collection : Collections)
          {
               const size_t DocumentCount =
                    HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);
               uint64_t IndexedMutationVersion = 0;
               const bool HasIndexedVersion =
                    Instance->Sam->GetCollectionIndexedMutationVersion(Collection, IndexedMutationVersion);
               const uint64_t CurrentMutationVersion =
                    Instance->API->GetCollectionMutationVersion(Collection);
               const bool NeedsInitialIndex = !HasIndexedVersion && DocumentCount > 0;
               const bool NeedsRefresh = HasIndexedVersion && CurrentMutationVersion > IndexedMutationVersion;

               if (!NeedsInitialIndex && !NeedsRefresh)
               {
                    continue;
               }

               bool AlreadyRunning = false;
               std::string ErrorMessage;

               if (!Instance->Sam->StartRecreateCollectionAsync(Collection, &AlreadyRunning, &ErrorMessage))
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
                    return;
               }

               if (Instance->Logs)
               {
                    const std::string Reason = NeedsRefresh
                         ? "source mutation version advanced from " + std::to_string(IndexedMutationVersion) +
                              " to " + std::to_string(CurrentMutationVersion)
                         : "collection has documents but no completed SAM index state";
                    Instance->Logs->Debug(LogSource,
                                          "Queued SAM auto-index for collection '" + Collection +
                                               "' because " + Reason + ".");
               }

               return;
          }

          RefreshSAMSearchIdeaProfile(LogSource);
     }
}

class CoreSAMModule final : public AutoRuntimeModule<CoreSAMModule>
{
   public:

     CoreSAMModule() : AutoRuntimeModule("core_sam")
     {

     }

     bool Start(const ServerConfig &, std::string &) override
     {
          return true;
     }

     void Stop() override
     {

     }

     void OnStartup() override
     {
          TriggerSAMAutoIndex("core_sam", true);
     }

     void OnEveryOneMinute() override
     {
          TriggerSAMAutoIndex("core_sam", false);
     }
};

MODULE_LOAD(CoreSAMModule)
