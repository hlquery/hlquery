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
#include "sam/sam.h"

namespace
{
     constexpr const char* kSAMGlobalLexicalScope = "__global__";
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

     void OptimizeSAMSearchIntent(const std::string &LogSource)
     {
          if (!Instance || !Instance->Sam || !Instance->Sam->IsOpen() || !Instance->LLM || !Instance->LLM->Configured())
          {
               return;
          }

          const size_t Processed = Instance->Sam->ProcessPendingSearchIntentOptimizations(1);

          if (Processed > 0 && Instance->Logs)
          {
               Instance->Logs->Debug(LogSource,
                                     "Processed " + std::to_string(Processed) +
                                          " pending SAM search-intent optimization job(s).");
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

               if (!Instance->Sam->StartRecreateCollectionAsync(Target, &AlreadyRunning, &ErrorMessage))
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
}

class CoreSAMModule final : public AutoRuntimeModule<CoreSAMModule>
{
   public:

     CoreSAMModule() : AutoRuntimeModule("core_sam")
     {

     }

     void Stop() override
     {

     }

     void OnStartup() override
     {
          SyncSAMLexicalScope(kSAMGlobalLexicalScope, false, "core_sam");

          for (const auto& Collection : HybridStorageManager::GetInstance().ListCollections())
          {
               SyncSAMLexicalScope(Collection, false, "core_sam");
          }

          TriggerSAMAutoIndex("core_sam", true);
          OptimizeSAMSearchIntent("core_sam");
     }

     void OnEveryOneMinute() override
     {
          SyncSAMLexicalScope(kSAMGlobalLexicalScope, false, "core_sam");

          TriggerSAMAutoIndex("core_sam", false);
          OptimizeSAMSearchIntent("core_sam");
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
