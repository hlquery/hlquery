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
#include <filesystem>
#include <rocksdb/write_batch.h>
#include <chrono>
#include <limits>
#include <sstream>
#include <thread>

#include "api/searchapi.h"
#include "core/hlquery.h"
#include "sam/lang.h"
#include "sam/sam.h"
#include "sam/internal.h"
#include "search/storageengine.h"
#include "vendor/json/json.hpp"

constexpr const char* kPendingIndexQueuePrefix = "sam:queue:index:";
constexpr const char* kDocumentContextPrefix = "sam:context:";
constexpr uint64_t kDefaultBackgroundImprovementIntervalMS = 60000;
constexpr uint64_t kDefaultBackgroundImprovementPollMS = 15000;
constexpr size_t kMaxPendingIndexJobs = 4096;
constexpr size_t kDocumentContextAuditBatchSize = 10;
constexpr uint64_t kDocumentContextWeakRetryMS = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kDocumentContextHealthyRetryMS = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

/* Build the persisted queue key for one pending SAM index job. */

std::string BuildPendingIndexQueueKey(const std::string& Collection, const std::string& DocumentID)
{
     std::string Key;
     Key.reserve(std::char_traits<char>::length(kPendingIndexQueuePrefix) + Collection.size() + 1 + DocumentID.size());
     Key.append(kPendingIndexQueuePrefix);
     Key.append(Collection);
     Key.push_back('\0');
     Key.append(DocumentID);
     return Key;
}

std::string BuildDocumentContextKey(const std::string& Collection, const std::string& DocumentID)
{
     return std::string(kDocumentContextPrefix) + Collection + ":" + DocumentID;
}

double ScoreDocumentContextSuggestions(const nlohmann::json& Suggestions)
{
     if (!Suggestions.is_array() || Suggestions.empty())
     {
          return 0.0;
     }

     size_t UsefulSuggestions = 0;
     double ConfidenceTotal = 0.0;

     for (const auto& Suggestion : Suggestions)
     {
          if (!Suggestion.is_object() ||
              Suggestion.value("provisional", false) ||
              Suggestion.value("kind", "") == "title")
          {
               continue;
          }

          const double Confidence = std::clamp(Suggestion.value("confidence", 0.0), 0.0, 1.0);
          const double ValidationScore =
               std::clamp(Suggestion.value("validation_score", Confidence), 0.0, 1.0);

          if (Confidence < 0.78 || ValidationScore < 0.70)
          {
               continue;
          }

          ++UsefulSuggestions;
          ConfidenceTotal += (Confidence * 0.65) + (ValidationScore * 0.35);
     }

     if (UsefulSuggestions == 0)
     {
          return 0.0;
     }

     const double Coverage = std::min(1.0, static_cast<double>(UsefulSuggestions) / 3.0);
     return std::clamp(Coverage * (ConfidenceTotal / static_cast<double>(UsefulSuggestions)), 0.0, 1.0);
}

/* Parse a persisted queue key back into collection and document identifiers. */

bool ParsePendingIndexQueueKey(const std::string& Key, std::string& Collection, std::string& DocumentID)
{
     const size_t PrefixLen = std::char_traits<char>::length(kPendingIndexQueuePrefix);
     if (Key.size() <= PrefixLen || Key.compare(0, PrefixLen, kPendingIndexQueuePrefix) != 0)
     {
          return false;
     }

     const std::string_view Remainder(Key.data() + PrefixLen, Key.size() - PrefixLen);
     const size_t Sep = Remainder.find('\0');
     if (Sep == std::string::npos)
     {
          return false;
     }

     Collection.assign(Remainder.substr(0, Sep));
     DocumentID.assign(Remainder.substr(Sep + 1));
     return !Collection.empty() && !DocumentID.empty();
}

bool SmartSAMBackgroundEnabled()
{
     return !Instance || !Instance->Config || Instance->Config->GetSamSmartBackground();
}

bool SAMAutoRebuildEnabled()
{
     return Instance && Instance->Config && Instance->Config->GetSamIndexAll();
}

bool SAMAutomaticPauseActive()
{
     if (!Instance || !Instance->Sam)
     {
          return false;
     }

     const uint64_t NowMS = static_cast<uint64_t>(NowMs());
     const uint64_t PauseUntilMS = Instance->Sam->GetAutoIndexPauseUntilMS();
     return PauseUntilMS > 0 && NowMS < PauseUntilMS;
}

uint64_t ResolveBackgroundImprovementIntervalMS()
{
     if (!Instance || !Instance->Config)
     {
          return kDefaultBackgroundImprovementIntervalMS;
     }

     return static_cast<uint64_t>(
          std::max(5000, Instance->Config->GetSamBackgroundImprovementIntervalMs()));
}

std::chrono::milliseconds ResolveBackgroundImprovementPollInterval()
{
     if (!Instance || !Instance->Config)
     {
          return std::chrono::milliseconds(kDefaultBackgroundImprovementPollMS);
     }

     return std::chrono::milliseconds(
          std::max(1000, Instance->Config->GetSamBackgroundImprovementPollMs()));
}

/* Initialize SAM runtime defaults. */

SAM::SAM()
{
     OptionsValue.create_if_missing = true;
     OptionsValue.error_if_exists = false;
     OptionsValue.max_open_files = 128;
     BackgroundWorkerCount = ResolveBackgroundWorkerCount();
}

SAM::~SAM()
{
     Shutdown();
}

void SAM::RecordDebugEvent(const std::string& Collection, const std::string& Message) const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);

     DebugEvent Event;
     Event.Sequence = NextDebugSequence++;
     Event.Collection = Collection;
     Event.Message = Message;
     DebugEvents.push_back(std::move(Event));

     constexpr size_t MaxDebugEvents = 512;

     while (DebugEvents.size() > MaxDebugEvents)
     {
          DebugEvents.pop_front();
     }
}

bool SAM::IsCollectionCancelledLocked(const std::string& Collection) const
{
     return ShuttingDown || CancelAllRequested || CancelledCollections.find(Collection) != CancelledCollections.end();
}

size_t SAM::ClearQueuedAutoIndexJobs()
{
     size_t ClearedJobs = 0;

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          ClearedJobs = PendingIndexJobs.size();
          PendingIndexJobs.clear();
          PendingIndexKeys.clear();
     }

     {
          std::lock_guard<std::mutex> DBLock(DBMutex);
          if (Database)
          {
               std::vector<std::string> QueueKeys;
               std::unique_ptr<rocksdb::Iterator> It(Database->NewIterator(rocksdb::ReadOptions()));

               for (It->Seek(kPendingIndexQueuePrefix);
                    It->Valid() && It->key().starts_with(kPendingIndexQueuePrefix);
                    It->Next())
               {
                    QueueKeys.push_back(It->key().ToString());
               }

               if (!QueueKeys.empty())
               {
                    rocksdb::WriteBatch Batch;

                    for (const std::string& Key : QueueKeys)
                    {
                         Batch.Delete(Key);
                    }

                    (void)Database->Write(rocksdb::WriteOptions(), &Batch);
                    ClearedJobs = std::max(ClearedJobs, QueueKeys.size());
               }
          }
     }

     return ClearedJobs;
}

/* Open the SAM database and restore durable queue state. */

bool SAM::Initialize()
{
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               return true;
          }

          DBPath = ResolveDBPath();

          try
          {
               std::filesystem::create_directories(DBPath);
          }
          catch (const std::exception& E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to create SAM directory '" + DBPath + "': " + E.what() + ".");
               }

               return false;
          }

          std::unique_ptr<rocksdb::DB> RawDB;
          const rocksdb::Status Status = rocksdb::DB::Open(OptionsValue, DBPath, &RawDB);

          if (!Status.ok())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to open SAM database at '" + DBPath + "': " + Status.ToString() + ".");
               }

               return false;
          }

          Database = std::shared_ptr<rocksdb::DB>(RawDB.release());
          DatabaseOpen.store(true, std::memory_order_release);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "Secondary Assistant Manager opened at " + DBPath + ".");
          }
     }

     {
          std::lock_guard<std::mutex> DBLock(DBMutex);
          if (Database)
          {
               std::vector<std::string> StaleQueueKeys;
               std::unique_ptr<rocksdb::Iterator> It(Database->NewIterator(rocksdb::ReadOptions()));
               for (It->Seek(kPendingIndexQueuePrefix);
                    It->Valid() && It->key().starts_with(kPendingIndexQueuePrefix);
                    It->Next())
               {
                    std::string Collection;
                    std::string DocumentID;
                    if (!ParsePendingIndexQueueKey(It->key().ToString(), Collection, DocumentID))
                    {
                         continue;
                    }

                    bool HasExpectedMutationVersion = false;
                    uint64_t ExpectedMutationVersion = 0;

                    try
                    {
                         const nlohmann::json Root = nlohmann::json::parse(It->value().ToString());
                         HasExpectedMutationVersion = Root.value("has_expected_mutation_version", false);
                         ExpectedMutationVersion = Root.value("expected_mutation_version", 0);
                    }
                    catch (...)
                    {
                    }

                    if (HasExpectedMutationVersion &&
                        ExpectedMutationVersion != GetCurrentCollectionMutationVersion(Collection))
                    {
                         StaleQueueKeys.push_back(It->key().ToString());
                         continue;
                    }

                    const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);
                    if (Doc.ID.empty())
                    {
                         StaleQueueKeys.push_back(It->key().ToString());
                         continue;
                    }

                    {
                         std::lock_guard<std::mutex> QueueLock(QueueMutex);
                         const std::string PendingKey = BuildPendingIndexKey(Collection, DocumentID);
                         if (!PendingIndexKeys.insert(PendingKey).second)
                         {
                              continue;
                         }

                         PendingIndexJobs.push_back(PendingIndexJob{Collection, Doc, HasExpectedMutationVersion, ExpectedMutationVersion});
                    }
               }

               if (!StaleQueueKeys.empty())
               {
                    rocksdb::WriteBatch Batch;
                    for (const auto& Key : StaleQueueKeys)
                    {
                         Batch.Delete(Key);
                    }
                    (void)Database->Write(rocksdb::WriteOptions(), &Batch);
               }
          }
     }

     StartIndexWorker();

     return true;
}

/* Stop workers and release the SAM database handle. */

void SAM::Shutdown()
{
     std::vector<std::thread> ThreadsToJoin;
     std::vector<std::thread> HelperThreadsToJoin;
     std::shared_ptr<rocksdb::DB> DatabaseToRelease;

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM shutdown begin.");
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          ShuttingDown = true;
          CancelAllRequested = true;

          for (auto& Entry : CollectionJobs)
          {
               if (Entry.second.Running)
               {
                    Entry.second.Running = false;
                    Entry.second.Completed = false;
                    Entry.second.PendingDocuments = 0;

                    if (Entry.second.ErrorMessage.empty())
                    {
                         Entry.second.ErrorMessage = "Cancelled during shutdown.";
                    }
               }
          }
     }

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);
          ShuttingDown = true;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "SAM shutdown clearing " + std::to_string(PendingIndexJobs.size()) + " queued index job(s).");
          }

          PendingIndexJobs.clear();
          PendingIndexKeys.clear();
     }

     QueueCV.notify_all();
     JobStateCV.notify_all();

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          ThreadsToJoin.swap(WorkerThreads);
          HelperThreadsToJoin.swap(HelperThreads);
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM shutdown joining " + std::to_string(ThreadsToJoin.size()) + " worker thread(s).");
     }

     for (auto& Worker : ThreadsToJoin)
     {
          if (Worker.joinable())
          {
               if (Worker.get_id() == std::this_thread::get_id())
               {
                    continue;
               }

               Worker.join();
          }
     }

     for (auto& Helper : HelperThreadsToJoin)
     {
          if (Helper.joinable())
          {
               if (Helper.get_id() == std::this_thread::get_id())
               {
                    continue;
               }

               Helper.join();
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM shutdown workers joined.");
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseOpen.store(false, std::memory_order_release);
          DatabaseToRelease = std::move(Database);
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM shutdown complete.");
     }
}

void SAM::StartHelperThread(std::thread Thread)
{
     if (!Thread.joinable())
     {
          return;
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          if (!ShuttingDown)
          {
               HelperThreads.emplace_back(std::move(Thread));
               return;
          }
     }

     Thread.join();
}

std::string SAM::BuildPendingIndexKey(const std::string& Collection, const std::string& DocumentID)
{
     std::string Key;
     Key.reserve(Collection.size() + 1 + DocumentID.size());
     Key.append(Collection);
     Key.push_back('\0');
     Key.append(DocumentID);
     return Key;
}

uint64_t SAM::GetCurrentCollectionMutationVersion(const std::string& Collection) const
{
     if (Collection.empty() || !Instance || !Instance->API)
     {
          return 0;
     }

     return Instance->API->GetCollectionMutationVersion(Collection);
}

bool SAM::ValidateExpectedMutationVersion(const std::string& Collection,
                                          bool HasExpectedMutationVersion,
                                          uint64_t ExpectedMutationVersion,
                                          std::string* ErrorMessage) const
{
     if (!HasExpectedMutationVersion)
     {
          return true;
     }

     const uint64_t CurrentMutationVersion = GetCurrentCollectionMutationVersion(Collection);

     if (CurrentMutationVersion == ExpectedMutationVersion)
     {
          return true;
     }

     if (ErrorMessage)
     {
          *ErrorMessage = "Source collection changed during SAM indexing; queued mutation version " +
                          std::to_string(ExpectedMutationVersion) + ", current mutation version " +
                          std::to_string(CurrentMutationVersion) + ".";
     }

     return false;
}

void SAM::ScheduleRetryRebuild(const std::string& Collection)
{
     if (Collection.empty())
     {
          return;
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobStatus& Status = CollectionJobs[Collection];

          if (ShuttingDown)
          {
               return;
          }

          if (Status.RetryScheduled)
          {
               return;
          }

          Status.RetryScheduled = true;
     }

     std::string RetrySource;
     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          const auto It = CollectionJobs.find(Collection);
          if (It != CollectionJobs.end())
          {
               RetrySource = It->second.Source.empty() ? "retry rebuild" : It->second.Source;
          }
     }

     try
     {
          StartHelperThread(std::thread([this, Collection, RetrySource]()
          {
               {
                    std::unique_lock<std::mutex> Lock(JobMutex);
                    const bool Cancelled = JobStateCV.wait_for(Lock,
                                                               std::chrono::seconds(5),
                                                               [this, &Collection]()
                                                               {
                                                                    return ShuttingDown ||
                                                                           IsCollectionCancelledLocked(Collection);
                                                               });
                    if (Cancelled)
                    {
                         auto It = CollectionJobs.find(Collection);
                         if (It != CollectionJobs.end())
                         {
                              It->second.RetryScheduled = false;
                         }
                         return;
                    }
               }

               {
                    std::lock_guard<std::mutex> Lock(JobMutex);

                    if (ShuttingDown || IsCollectionCancelledLocked(Collection))
                    {
                         auto It = CollectionJobs.find(Collection);
                         if (It != CollectionJobs.end())
                         {
                              It->second.RetryScheduled = false;
                         }
                         return;
                    }
               }

               bool AlreadyRunning = false;
               std::string ErrorMessage;
               const bool Started = StartRecreateCollectionAsync(Collection, &AlreadyRunning, &ErrorMessage, RetrySource, false);
               bool RescheduleAfterDrain = false;

               {
                    std::lock_guard<std::mutex> Lock(JobMutex);
                    auto It = CollectionJobs.find(Collection);
                    if (It != CollectionJobs.end())
                    {
                         It->second.RetryScheduled = false;

                         if (Started && !AlreadyRunning)
                         {
                              It->second.NeedsRetry = false;
                              if (It->second.ErrorMessage.find("Source collection changed during SAM indexing;") == 0)
                              {
                                   It->second.ErrorMessage = "Automatic SAM retry queued after source mutations.";
                              }
                         }
                         else if (AlreadyRunning)
                         {
                              RescheduleAfterDrain = It->second.NeedsRetry;
                         }
                         else if (!ErrorMessage.empty())
                         {
                              It->second.ErrorMessage = ErrorMessage;
                         }
                    }
                }

               if (AlreadyRunning && RescheduleAfterDrain)
               {
                    if (SAMAutoRebuildEnabled())
                    {
                         ScheduleRetryRebuild(Collection);
                    }
               }
               else if (Started && !AlreadyRunning)
               {
                    RecordDebugEvent(Collection, "scheduled automatic rebuild retry after concurrent source mutation");
               }
               else if (!AlreadyRunning)
               {
                    RecordDebugEvent(Collection,
                                     "automatic rebuild retry could not be queued: " +
                                          (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage));
               }
          }));
     }
     catch (const std::exception& E)
     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobStatus& Status = CollectionJobs[Collection];
          Status.RetryScheduled = false;
          Status.ErrorMessage = E.what();
     }
}

void SAM::StartIndexWorker()
{
     std::lock_guard<std::mutex> Lock(JobMutex);
     ShuttingDown = false;

     if (!WorkerThreads.empty())
     {
          return;
     }

     if (BackgroundWorkerCount == 0)
     {
          BackgroundWorkerCount = 1;
     }

     for (size_t Index = 0; Index < BackgroundWorkerCount; ++Index)
     {
          WorkerThreads.emplace_back([this]()
          {
               RunIndexWorker();
          });
     }

     WorkerThreads.emplace_back([this]()
     {
          RunImprovementWorker();
     });
}

bool SAM::TryBeginBackgroundImprovement(const std::string& Collection,
                                        uint64_t NowMS,
                                        bool Force,
                                        std::string* SkipReason)
{
     if (SkipReason)
     {
          SkipReason->clear();
     }

     if (Collection.empty())
     {
          if (SkipReason)
          {
               *SkipReason = "not_indexed";
          }

          return false;
     }

     if (FlushInProgress.load(std::memory_order_acquire))
     {
          if (SkipReason)
          {
               *SkipReason = "flush";
          }

          return false;
     }

     if (!Force && SAMAutomaticPauseActive())
     {
          if (SkipReason)
          {
               *SkipReason = "paused";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);

          if (!PendingIndexJobs.empty())
          {
               if (SkipReason)
               {
                    *SkipReason = "busy";
               }

               return false;
          }
     }

     std::lock_guard<std::mutex> JobLock(JobMutex);

     if (IsCollectionCancelledLocked(Collection))
     {
          if (SkipReason)
          {
               *SkipReason = "cancelled";
          }

          return false;
     }

     if (ActiveCollectionTasks.find(Collection) != ActiveCollectionTasks.end())
     {
          if (SkipReason)
          {
               *SkipReason = "busy";
          }

          return false;
     }

     const auto StatusIt = CollectionJobs.find(Collection);

     if (StatusIt != CollectionJobs.end() && StatusIt->second.Running)
     {
          if (SkipReason)
          {
               *SkipReason = "busy";
          }

          return false;
     }

     const uint64_t LastImprovementMS = LastBackgroundImprovementMS[Collection];
     const uint64_t ImprovementIntervalMS = ResolveBackgroundImprovementIntervalMS();

     if (!Force &&
         LastImprovementMS > 0 &&
         NowMS > LastImprovementMS &&
         (NowMS - LastImprovementMS) < ImprovementIntervalMS)
     {
          if (SkipReason)
          {
               *SkipReason = "throttled";
          }

          return false;
     }

     ++ActiveCollectionTasks[Collection];
     LastBackgroundImprovementMS[Collection] = NowMS;
     return true;
}

void SAM::FinishBackgroundImprovement(const std::string& Collection)
{
     std::lock_guard<std::mutex> JobLock(JobMutex);
     auto ActiveIt = ActiveCollectionTasks.find(Collection);

     if (ActiveIt != ActiveCollectionTasks.end())
     {
          if (ActiveIt->second > 0)
          {
               --ActiveIt->second;
          }

          if (ActiveIt->second == 0)
          {
               ActiveCollectionTasks.erase(ActiveIt);
          }
     }

     JobStateCV.notify_all();
     QueueCV.notify_all();
}

std::vector<std::string> SAM::CollectDocumentContextAuditCandidates(const std::string& Collection,
                                                                    uint64_t NowMS,
                                                                    size_t Limit)
{
     std::vector<std::string> DocumentIDs;

     if (Collection.empty() || Limit == 0)
     {
          return DocumentIDs;
     }

     const size_t DocumentCount =
          HybridStorageManager::GetInstance().GetCollectionDocumentCount(Collection);

     if (DocumentCount == 0)
     {
          return DocumentIDs;
     }

     size_t Offset = 0;

     {
          std::lock_guard<std::mutex> JobLock(JobMutex);
          Offset = NextContextAuditOffset[Collection] % DocumentCount;
          NextContextAuditOffset[Collection] = (Offset + Limit) % DocumentCount;
     }

     std::vector<Document> Documents =
          HybridStorageManager::GetInstance().ListDocuments(Collection,
                                                            static_cast<int>(Limit),
                                                            static_cast<int>(Offset));

     if (Documents.size() < Limit && Offset > 0)
     {
          const std::vector<Document> WrappedDocuments =
               HybridStorageManager::GetInstance().ListDocuments(Collection,
                                                                 static_cast<int>(Limit - Documents.size()),
                                                                 0);
          Documents.insert(Documents.end(), WrappedDocuments.begin(), WrappedDocuments.end());
     }

     for (const auto& Doc : Documents)
     {
          nlohmann::json Context;
          const bool HasContext = LoadDocumentContext(Collection, Doc.ID, Context);
          const uint64_t RevisitAfterMS =
               HasContext ? Context.value("revisit_after_ms", static_cast<uint64_t>(0)) : 0;
          const bool Changed =
               !HasContext ||
               Context.value("source_fingerprint", "") != BuildSAMSourceDocumentFingerprint(Doc);
          const bool Due =
               !HasContext ||
               RevisitAfterMS == 0 ||
               (NowMS > 0 && NowMS >= RevisitAfterMS);

          if (Changed || Due)
          {
               DocumentIDs.push_back(Doc.ID);
          }
     }

     return DocumentIDs;
}

SAM::ImprovementStats SAM::ImproveIdleCollectionsDetailed(size_t MaxCollections, bool Force)
{
     ImprovementStats Stats;
     FlushPendingSearchInteractions(4);
     FlushPendingSearchIdeas(2);
     size_t OptimizationLimit =
          std::max<size_t>(1, std::min<size_t>(BackgroundWorkerCount, 4));

     if (Force)
     {
          constexpr size_t kDefaultForcedIntentOptimizationJobs = 16;
          constexpr size_t kMaxForcedIntentOptimizationJobs = 64;

          OptimizationLimit = MaxCollections > 0
               ? std::min(MaxCollections, kMaxForcedIntentOptimizationJobs)
               : kDefaultForcedIntentOptimizationJobs;
     }

     Stats.OptimizedIdeas =
          ProcessPendingSearchIntentOptimizations(OptimizationLimit, Force);

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          Stats.SkippedLLMUnavailable = 1;
     }

     const std::vector<std::string> Collections =
          HybridStorageManager::GetInstance().ListCollections();
     const uint64_t NowMS = Instance ? Instance->NowMs() : 0;

     for (const auto& Collection : Collections)
     {
          if (MaxCollections > 0 && Stats.ImprovedCollections >= MaxCollections)
          {
               break;
          }

          std::string SkipReason;
          if (!TryBeginBackgroundImprovement(Collection, NowMS, Force, &SkipReason))
          {
               if (SkipReason == "busy")
               {
                    ++Stats.SkippedBusy;
               }
               else if (SkipReason == "cancelled")
               {
                    ++Stats.SkippedCancelled;
               }
               else if (SkipReason == "throttled")
               {
                    ++Stats.SkippedThrottled;
               }
               else if (SkipReason == "paused")
               {
                    ++Stats.SkippedPaused;
               }
               else if (SkipReason == "flush")
               {
                    ++Stats.SkippedFlushInProgress;
               }
               else
               {
                    ++Stats.SkippedNotIndexed;
               }

               continue;
          }

          bool Improved = false;
          size_t PrunedTerms = 0;
          std::string ErrorMessage;
          std::vector<std::string> ContextRefreshDocumentIDs;
          bool ContextAuditEligible = false;

          {
               std::lock_guard<std::mutex> DBLock(DBMutex);

               if (Database)
               {
                    SAMCollectionState State;
                    std::string StateError;
                    const bool LoadedState =
                         ReadCollectionStateLocked(Database.get(), Collection, State, nullptr, &StateError);
                    const uint64_t CurrentMutationVersion =
                         GetCurrentCollectionMutationVersion(Collection);
                    const bool CurrentIndex =
                         LoadedState &&
                         State.HasIndexedMutationVersion &&
                         !State.RebuildRequested &&
                         (CurrentMutationVersion == 0 ||
                          State.IndexedMutationVersion == CurrentMutationVersion);

                    if (CurrentIndex)
                    {
                         size_t PrunedIdeas = 0;
                         if (!TrimSearchIdeasLocked(Collection, &PrunedIdeas, &ErrorMessage))
                         {
                              RecordDebugEvent(Collection, "background improvement failed to prune stale search ideas: " + ErrorMessage);
                         }
                         Stats.PrunedIdeas += PrunedIdeas;

                         if (!PruneUnusedSAMTermsLocked(Database.get(), Collection, &PrunedTerms, &ErrorMessage))
                         {
                              RecordDebugEvent(Collection, "background improvement failed to prune unused generated terms: " + ErrorMessage);
                         }
                         Stats.PrunedTerms += PrunedTerms;

                         const bool RebuiltProfile =
                              RebuildCollectionProfileLocked(Database.get(), Collection, &ErrorMessage);
                         const bool RebuiltGraph =
                              RebuildIntentGraphLocked(Database.get(), Collection, &ErrorMessage);
                         ContextRefreshDocumentIDs =
                              CollectSearchReinforcedDocumentIDsLocked(Database.get(), Collection, NowMS, 4);
                         ContextAuditEligible = true;
                         Improved = RebuiltProfile || RebuiltGraph;
                    }
                    else if (LoadedState && State.RebuildRequested)
                    {
                         ErrorMessage = "skipped because a rebuild is already pending";
                         ++Stats.SkippedPendingRebuild;
                    }
                    else if (!LoadedState)
                    {
                         ErrorMessage = StateError;
                         ++Stats.SkippedNotIndexed;
                    }
                    else
                    {
                         ErrorMessage = "skipped because indexed mutation version is stale";
                         if (!State.HasIndexedMutationVersion)
                         {
                              ++Stats.SkippedNotIndexed;
                         }
                         else
                         {
                              ++Stats.SkippedStaleIndex;
                         }
                    }
               }
               else
               {
                    ++Stats.SkippedNoDatabase;
               }
          }

          FinishBackgroundImprovement(Collection);

          if (Instance && Instance->LLM)
          {
               std::unordered_set<std::string> AuditDocumentIDs;

               if (ContextAuditEligible)
               {
                    const std::vector<std::string> SelectedAuditDocumentIDs =
                         CollectDocumentContextAuditCandidates(Collection,
                                                               NowMS,
                                                               kDocumentContextAuditBatchSize);
                    ContextRefreshDocumentIDs.insert(ContextRefreshDocumentIDs.end(),
                                                     SelectedAuditDocumentIDs.begin(),
                                                     SelectedAuditDocumentIDs.end());
                    AuditDocumentIDs.insert(SelectedAuditDocumentIDs.begin(),
                                            SelectedAuditDocumentIDs.end());
               }

               size_t QueuedContextRefreshes = 0;
               std::unordered_set<std::string> SeenDocumentIDs;

               for (const auto& DocumentID : ContextRefreshDocumentIDs)
               {
                    if (!SeenDocumentIDs.insert(DocumentID).second)
                    {
                         continue;
                    }

                    const Document Doc =
                         HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

                    if (Doc.ID.empty())
                    {
                         continue;
                    }

                    if (Instance->LLM->EnqueueContextualization(Collection, Doc, true))
                    {
                         ++QueuedContextRefreshes;

                         if (AuditDocumentIDs.count(DocumentID) > 0)
                         {
                              ++Stats.QueuedContextAudits;
                         }
                    }
               }

               if (QueuedContextRefreshes > 0)
               {
                    RecordDebugEvent(Collection,
                                     "background improvement queued " +
                                          std::to_string(QueuedContextRefreshes) +
                                          " document context audit(s)");
               }
          }

          if (ContextAuditEligible && Instance && Instance->API)
          {
               size_t LearnedSynonymGroups = 0;
               size_t LearnedStopwords = 0;
               std::string LexicalError;

               if (Instance->API->ImproveSAMLexicalResources(Collection,
                                                             &LearnedSynonymGroups,
                                                             &LearnedStopwords,
                                                             &LexicalError))
               {
                    Stats.LearnedSynonymGroups += LearnedSynonymGroups;
                    Stats.LearnedStopwords += LearnedStopwords;

                    if (LearnedSynonymGroups > 0 || LearnedStopwords > 0)
                    {
                         RecordDebugEvent(Collection,
                                          "background improvement learned " +
                                               std::to_string(LearnedSynonymGroups) +
                                               " synonym group(s) and " +
                                               std::to_string(LearnedStopwords) +
                                               " stopword(s)");
                    }
               }
               else if (!LexicalError.empty())
               {
                    RecordDebugEvent(Collection,
                                     "background lexical improvement failed: " + LexicalError);
               }
          }

          if (Improved)
          {
               ++Stats.ImprovedCollections;
               RecordDebugEvent(Collection,
                                "background improvement refreshed learned profile and intent graph; pruned " +
                                     std::to_string(PrunedTerms) + " unused generated term(s)");
          }
          else if (!ErrorMessage.empty())
          {
               RecordDebugEvent(Collection, "background improvement " + ErrorMessage);
          }
     }

     return Stats;
}

size_t SAM::ImproveIdleCollections(size_t MaxCollections, bool Force)
{
     return ImproveIdleCollectionsDetailed(MaxCollections, Force).TotalImproved();
}

void SAM::RunImprovementWorker()
{
     while (true)
     {
          const auto PollInterval = ResolveBackgroundImprovementPollInterval();

          {
               std::unique_lock<std::mutex> Lock(QueueMutex);
               QueueCV.wait_for(Lock, PollInterval, [this]()
               {
                    return ShuttingDown;
               });

               if (ShuttingDown)
               {
                    return;
               }

               if (!PendingIndexJobs.empty())
               {
                    continue;
               }
          }

          if (!SmartSAMBackgroundEnabled())
          {
               continue;
          }

          if (SAMAutomaticPauseActive())
          {
               continue;
          }

          ImproveIdleCollections(1, false);
     }
}

/* Drain queued document indexing jobs until shutdown. */

void SAM::RunIndexWorker()
{
     while (true)
     {
          PendingIndexJob Job;

          {
               std::unique_lock<std::mutex> Lock(QueueMutex);
               while (true)
               {
                    QueueCV.wait(Lock, [this]()
                    {
                         return ShuttingDown || !PendingIndexJobs.empty();
                    });

                    if (ShuttingDown && PendingIndexJobs.empty())
                    {
                         return;
                    }

                    if (FlushInProgress.load(std::memory_order_acquire))
                    {
                         QueueCV.wait_for(Lock, std::chrono::milliseconds(250), [this]()
                         {
                              return ShuttingDown ||
                                     !FlushInProgress.load(std::memory_order_acquire);
                         });
                         continue;
                    }

                    size_t SelectedIndex = PendingIndexJobs.size();

                    {
                         std::lock_guard<std::mutex> JobLock(JobMutex);

                         for (size_t Index = 0; Index < PendingIndexJobs.size(); ++Index)
                         {
                              if (ActiveCollectionTasks.find(PendingIndexJobs[Index].Collection) ==
                                  ActiveCollectionTasks.end())
                              {
                                   ++ActiveCollectionTasks[PendingIndexJobs[Index].Collection];
                                   SelectedIndex = Index;
                                   break;
                              }
                         }
                    }

                    if (SelectedIndex < PendingIndexJobs.size())
                    {
                         Job = std::move(PendingIndexJobs[SelectedIndex]);
                         PendingIndexJobs.erase(PendingIndexJobs.begin() + static_cast<long>(SelectedIndex));
                         PendingIndexKeys.erase(BuildPendingIndexKey(Job.Collection, Job.Doc.ID));
                         {
                              std::lock_guard<std::mutex> DBLock(DBMutex);
                              if (Database)
                              {
                                   (void)Database->Delete(rocksdb::WriteOptions(),
                                                          BuildPendingIndexQueueKey(Job.Collection, Job.Doc.ID));
                              }
                         }
                         QueueCV.notify_all();
                         break;
                    }

                    QueueCV.wait(Lock);

                    if (ShuttingDown && PendingIndexJobs.empty())
                    {
                         return;
                    }
               }
          }

          auto FinishTask = [this, &Job]()
          {
               std::lock_guard<std::mutex> JobLock(JobMutex);
               auto ActiveIt = ActiveCollectionTasks.find(Job.Collection);

               if (ActiveIt != ActiveCollectionTasks.end())
               {
                    if (ActiveIt->second > 0)
                    {
                         --ActiveIt->second;
                    }

                    if (ActiveIt->second == 0)
                    {
                         ActiveCollectionTasks.erase(ActiveIt);
                    }
               }

               JobStateCV.notify_all();
               QueueCV.notify_all();
          };

          {
               bool Cancelled = false;
               {
                    std::lock_guard<std::mutex> JobLock(JobMutex);

                    if (IsCollectionCancelledLocked(Job.Collection))
                    {
                         CollectionJobStatus& Status = CollectionJobs[Job.Collection];

                         if (Status.PendingDocuments > 0)
                         {
                              --Status.PendingDocuments;
                         }

                         if (Status.Running && Status.PendingDocuments == 0)
                         {
                              Status.Running = false;
                              Status.Completed = false;
                              if (Status.ErrorMessage.empty())
                              {
                                   Status.ErrorMessage = "Cancelled.";
                              }
                         }

                         RecordDebugEvent(Job.Collection, "skipped queued background index for " + Job.Doc.ID + " because collection work was cancelled");
                         Cancelled = true;
                    }
               }

               if (Cancelled)
               {
                    FinishTask();
                    continue;
               }
          }

          std::string ErrorMessage;
          bool SkipInvalidatedRebuildJob = false;

          if (Job.HasExpectedMutationVersion)
          {
               std::lock_guard<std::mutex> JobLock(JobMutex);
               SkipInvalidatedRebuildJob =
                    InvalidatedRebuildCollections.find(Job.Collection) != InvalidatedRebuildCollections.end();
          }

          const bool Success = SkipInvalidatedRebuildJob
               ? false
               : IndexDocument(Job.Collection,
                               Job.Doc,
                               &ErrorMessage,
                               Job.HasExpectedMutationVersion,
                               Job.ExpectedMutationVersion,
                               Job.AllowLLMExpansion);

          bool RetryRequested = false;

          {
               std::lock_guard<std::mutex> JobLock(JobMutex);
               CollectionJobStatus& Status = CollectionJobs[Job.Collection];

               if (Status.PendingDocuments > 0)
               {
                    --Status.PendingDocuments;
               }

               if (Success)
               {
                    ++Status.IndexedDocuments;
               }
               else if (!SkipInvalidatedRebuildJob)
               {
                    ++Status.FailedDocuments;
                    if (!ErrorMessage.empty())
                    {
                         Status.ErrorMessage = ErrorMessage;
                    }

                    if (Job.HasExpectedMutationVersion &&
                        ErrorMessage.find("Source collection changed during SAM indexing;") == 0)
                    {
                         Status.NeedsRetry = true;
                         InvalidatedRebuildCollections.insert(Job.Collection);
                         RetryRequested = true;
                    }
               }

               if (Status.Running && Status.PendingDocuments == 0)
               {
                    std::string ProfileError;
                    bool WroteIndexedMutationVersion = false;

                    if (!Status.NeedsRetry)
                    {
                         std::lock_guard<std::mutex> DBLock(DBMutex);
                         (void)RebuildCollectionProfileLocked(Database.get(), Job.Collection, &ProfileError);
                         std::string StateError;
                         const uint64_t MutationVersion = GetCurrentCollectionMutationVersion(Job.Collection);
                         WroteIndexedMutationVersion =
                              WriteCollectionIndexedMutationVersionLocked(Database.get(),
                                                                          Job.Collection,
                                                                          MutationVersion,
                                                                          &StateError);
                         if (!WroteIndexedMutationVersion && Status.ErrorMessage.empty() && !StateError.empty())
                         {
                              Status.ErrorMessage = StateError;
                         }
                    }

                    Status.Running = false;
                    Status.Completed = !Status.NeedsRetry && WroteIndexedMutationVersion;
                    InvalidatedRebuildCollections.erase(Job.Collection);

                    if (Status.NeedsRetry)
                    {
                         if (Status.ErrorMessage.empty())
                         {
                              Status.ErrorMessage = "Source collection changed during SAM indexing; retry required.";
                         }

                         RecordDebugEvent(Job.Collection,
                                          "rebuild invalidated by concurrent source mutation: indexed " +
                                               std::to_string(Status.IndexedDocuments) + ", failed " +
                                               std::to_string(Status.FailedDocuments));
                    }
                    else if (Success)
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments) +
                                               (ProfileError.empty() ? "" : ", profile error: " + ProfileError));
                    }
                    else
                    {
                         RecordDebugEvent(Job.Collection,
                                          "rebuild complete with failures: indexed " + std::to_string(Status.IndexedDocuments) +
                                               ", failed " + std::to_string(Status.FailedDocuments) +
                                               (ProfileError.empty() ? "" : ", profile error: " + ProfileError));
                    }
               }
          }

          if (RetryRequested)
          {
               std::lock_guard<std::mutex> DBLock(DBMutex);

               if (Database)
               {
                    SAMCollectionState State;

                    std::string StateError;

                    if (ReadCollectionStateLocked(Database.get(), Job.Collection, State, nullptr, &StateError))
                    {
                         State.RebuildRequested = true;
                         State.RequestedMutationVersion =
                              GetCurrentCollectionMutationVersion(Job.Collection);

                         if (!WriteCollectionStateLocked(Database.get(), Job.Collection, State, &StateError))
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Normal("sam", "Failed to persist rebuild retry state for '" + Job.Collection + "': " + (StateError.empty() ? std::string("unknown error") : StateError) + ".");
                              }
                         }
                    }
                    else if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("sam", "Failed to read collection state for '" + Job.Collection + "' while scheduling rebuild retry: " + (StateError.empty() ? std::string("unknown error") : StateError) + ".");
                    }
               }

               if (SAMAutoRebuildEnabled())
               {
                    ScheduleRetryRebuild(Job.Collection);
               }
               else
               {
                    RecordDebugEvent(Job.Collection,
                                     "dropped stale automatic rebuild retry because SAM auto-index is disabled");
               }
          }

          FlushPendingSearchIdeas(8);
          FinishTask();

          if (SkipInvalidatedRebuildJob)
          {
               continue;
          }

          if (Success)
          {
               RecordDebugEvent(Job.Collection, "background indexed " + Job.Doc.ID);
               continue;
          }

          RecordDebugEvent(Job.Collection,
                           "background indexing failed for " + Job.Doc.ID + ": " +
                                (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage));

          if (Instance && Instance->Logs)
          {
               if (Job.HasExpectedMutationVersion &&
                   ErrorMessage.find("Source collection changed during SAM indexing;") == 0)
               {
                    Instance->Logs->Debug("sam",
                                          "Deferred stale background rebuild for '" +
                                               Job.Collection + "/" + Job.Doc.ID +
                                               "' until the source collection is stable.");
               }
               else
               {
                    Instance->Logs->Normal("sam", "Failed to background index '" + Job.Collection + "/" + Job.Doc.ID + "': " + (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage) + ".");
               }
          }
     }
}

size_t SAM::ResolveBackgroundWorkerCount() const
{
     return 1;
}

bool SAM::IsOpen() const
{
     return DatabaseOpen.load(std::memory_order_acquire);
}

bool SAM::FlushAndSync(std::string* ErrorMessage)
{
     if (ErrorMessage)
     {
          ErrorMessage->clear();
     }

     constexpr size_t kMaxQueueDrainAttempts = 8;

     for (size_t Attempt = 0; Attempt < kMaxQueueDrainAttempts; ++Attempt)
     {
          FlushPendingSearchInteractions(std::numeric_limits<size_t>::max());
          FlushPendingSearchIdeas(std::numeric_limits<size_t>::max());

          std::unique_lock<std::mutex> IdeaQueueLock(SearchIdeaQueueMutex);
          std::unique_lock<std::mutex> InteractionQueueLock(SearchInteractionQueueMutex);

          if (!PendingSearchIdeaJobs.empty() || !PendingSearchInteractionJobs.empty())
          {
               InteractionQueueLock.unlock();
               IdeaQueueLock.unlock();
               std::this_thread::yield();
               continue;
          }

          /*
           * Keep feedback queue locks held through the database flush.
           * Enqueues after this point belong to the next sync boundary.
           */

          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          rocksdb::FlushOptions FlushOptions;
          FlushOptions.wait = true;
          const rocksdb::Status Status = Database->Flush(FlushOptions);

          if (!Status.ok())
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = Status.ToString();
               }

               return false;
          }

          const rocksdb::Status SyncStatus = Database->SyncWAL();

          if (!SyncStatus.ok())
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = SyncStatus.ToString();
               }

               return false;
          }

          return true;
     }

     if (ErrorMessage)
     {
          *ErrorMessage = "SAM feedback queues remained busy during flush.";
     }

     return false;
}

std::string SAM::ResolveDBPath() const
{
     return ResolveSamDataDir();
}

bool SAM::ClearAll(std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     rocksdb::WriteBatch Batch;
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->SeekToFirst(); Iterator->Valid(); Iterator->Next())
     {
          Batch.Delete(Iterator->key());
     }

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::Recreate(std::string* ErrorMessage)
{
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          if (!ClearAll(ErrorMessage))
          {
               return false;
          }
     }

     size_t IndexedDocuments = 0;
     size_t FailedDocuments = 0;

     for (const std::string& Collection : HybridStorageManager::GetInstance().ListCollections())
     {
          if (!Instance || !Instance->Database)
          {
               break;
          }

          const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");

          for (const auto& DocKey : DocKeys)
          {
               const size_t LastColon = DocKey.find_last_of(':');

               if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
               {
                    continue;
               }

               const std::string DocumentID = DocKey.substr(LastColon + 1);
               const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

               if (Doc.ID.empty())
               {
                    continue;
               }

               std::string IndexError;

               if (IndexDocument(Collection, Doc, &IndexError))
               {
                    IndexedDocuments++;
               }
               else
               {
                    FailedDocuments++;

                    if (Instance && Instance->Logs && !IndexError.empty())
                    {
                         Instance->Logs->Normal("sam", "Failed to index '" + Collection + "/" + DocumentID + "' during recreate: " + IndexError + ".");
                    }
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM recreate complete: indexed " + std::to_string(IndexedDocuments) + " documents, failed " + std::to_string(FailedDocuments) + ".");
     }

     return true;
}

bool SAM::RecreateCollection(const std::string& Collection,
                             size_t* IndexedDocuments,
                             size_t* FailedDocuments,
                             std::string* ErrorMessage)
{
     if (IndexedDocuments)
     {
          *IndexedDocuments = 0;
     }

     if (FailedDocuments)
     {
          *FailedDocuments = 0;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     if (!HybridStorageManager::GetInstance().CollectionExists(Collection))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection not found.";
          }

          return false;
     }

     std::vector<std::string> ExistingDocumentIDs;
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (ManifestIterator->Seek(ManifestPrefix);
               ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
               ManifestIterator->Next())
          {
               const std::string Key = ManifestIterator->key().ToString();

               if (Key.size() > ManifestPrefix.size())
               {
                    ExistingDocumentIDs.push_back(Key.substr(ManifestPrefix.size()));
               }
          }
     }

     for (const auto& DocumentID : ExistingDocumentIDs)
     {
          std::string RemoveError;
          bool Removed = false;

          {
               std::lock_guard<std::mutex> Lock(DBMutex);

               if (Database)
               {
                    Removed = RemoveExistingDocumentTermsLocked(Collection, DocumentID, &RemoveError);
               }
          }

          if (!Removed && ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = RemoveError;
          }
     }

     size_t IndexedCount = 0;
     size_t FailedCount = 0;
     const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");

     for (const auto& DocKey : DocKeys)
     {
          const size_t LastColon = DocKey.find_last_of(':');

          if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
          {
               continue;
          }

          const std::string DocumentID = DocKey.substr(LastColon + 1);
          const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

          if (Doc.ID.empty())
          {
               continue;
          }

          std::string IndexError;

          if (IndexDocumentLocked(Collection, Doc, &IndexError))
          {
               IndexedCount++;
          }
          else
          {
               FailedCount++;

               if (Instance && Instance->Logs && !IndexError.empty())
               {
                    Instance->Logs->Normal("sam", "Failed to index '" + Collection + "/" + DocumentID + "' during collection rebuild: " + IndexError + ".");
               }
          }
     }

     if (IndexedDocuments)
     {
          *IndexedDocuments = IndexedCount;
     }

     if (FailedDocuments)
     {
          *FailedDocuments = FailedCount;
     }

     std::string ProfileError;
     bool RebuiltProfile = false;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               RebuiltProfile = RebuildCollectionProfileLocked(Database.get(), Collection, &ProfileError);
          }
     }

     if (!RebuiltProfile)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "Failed to rebuild SAM collection profile for '" + Collection + "': " + ProfileError + ".");
          }

          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = ProfileError;
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam", "SAM rebuild for collection '" + Collection + "' complete: indexed " + std::to_string(IndexedCount) + " documents, failed " + std::to_string(FailedCount) + ".");
     }

     return true;
}

bool SAM::StartRecreateCollectionAsync(const std::string& Collection,
                                       bool* AlreadyRunning,
                                       std::string* ErrorMessage,
                                       const std::string& Source,
                                       bool AllowLLMExpansion)
{
     if (AlreadyRunning)
     {
          *AlreadyRunning = false;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          SAMCollectionState State;
          std::string StateError;

          if (!ReadCollectionStateLocked(Database.get(), Collection, State, nullptr, &StateError))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = StateError;
               }

               return false;
          }

          State.RebuildRequested = true;
          State.RequestedMutationVersion = GetCurrentCollectionMutationVersion(Collection);

          if (!WriteCollectionStateLocked(Database.get(), Collection, State, &StateError))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = StateError;
               }

               return false;
          }
     }

     if (!HybridStorageManager::GetInstance().CollectionExists(Collection))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection not found.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          const auto ExistingIt = CollectionJobs.find(Collection);

          if (ShuttingDown)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM shutdown is in progress.";
               }

               return false;
          }

          if (FlushInProgress.load(std::memory_order_acquire))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM flush is in progress.";
               }

               return false;
          }

          if (IsCollectionCancelledLocked(Collection))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Collection SAM work is being cancelled.";
               }

               return false;
          }

          if (ExistingIt != CollectionJobs.end() && ExistingIt->second.Running)
          {
               if (!Source.empty() && ExistingIt->second.Source.empty())
               {
                    ExistingIt->second.Source = Source;
               }

               if (AlreadyRunning)
               {
                    *AlreadyRunning = true;
               }

               return true;
          }

          CollectionJobStatus& JobStatus = CollectionJobs[Collection];
          JobStatus = CollectionJobStatus{};
          JobStatus.Running = true;
          JobStatus.Completed = false;
          JobStatus.RetryScheduled = false;
          JobStatus.ErrorMessage.clear();
          JobStatus.Source = Source;
          InvalidatedRebuildCollections.erase(Collection);
          CancelledCollections.erase(Collection);
          ++ActiveCollectionTasks[Collection];
     }

     RecordDebugEvent(Collection,
                      Source.empty() ? "queued background rebuild setup"
                                     : "queued background rebuild setup from " + Source);

     const bool HasExpectedMutationVersion = Instance && Instance->API;
     const uint64_t ExpectedMutationVersion =
          HasExpectedMutationVersion ? GetCurrentCollectionMutationVersion(Collection) : 0;

     try
     {
          StartHelperThread(std::thread([this, Collection, HasExpectedMutationVersion, ExpectedMutationVersion, Source, AllowLLMExpansion]()
          {
          auto FinishTask = [this, &Collection]()
          {
               std::lock_guard<std::mutex> Lock(JobMutex);
               auto ActiveIt = ActiveCollectionTasks.find(Collection);

               if (ActiveIt != ActiveCollectionTasks.end())
               {
                    if (ActiveIt->second > 0)
                    {
                         --ActiveIt->second;
                    }

                    if (ActiveIt->second == 0)
                    {
                         ActiveCollectionTasks.erase(ActiveIt);
                    }
               }

               JobStateCV.notify_all();
               QueueCV.notify_all();
          };

          auto CancelRebuildSetup = [this, &Collection](const std::string& Reason)
          {
               std::lock_guard<std::mutex> Lock(JobMutex);
               CollectionJobStatus& JobStatus = CollectionJobs[Collection];
               JobStatus.Running = false;
               JobStatus.Completed = false;
               JobStatus.PendingDocuments = 0;
               if (JobStatus.ErrorMessage.empty())
               {
                    JobStatus.ErrorMessage = "Cancelled.";
               }
               RecordDebugEvent(Collection, Reason);
          };

          auto WaitForQueueCapacity = [this, &Collection, &CancelRebuildSetup]() -> bool
          {
               while (true)
               {
                    {
                         std::lock_guard<std::mutex> QueueLock(QueueMutex);

                         if (PendingIndexJobs.size() < kMaxPendingIndexJobs)
                         {
                              return true;
                         }
                    }

                    bool Cancelled = false;

                    {
                         std::lock_guard<std::mutex> JobLock(JobMutex);
                         Cancelled = ShuttingDown ||
                                     IsCollectionCancelledLocked(Collection) ||
                                     FlushInProgress.load(std::memory_order_acquire);
                    }

                    if (Cancelled)
                    {
                         CancelRebuildSetup("cancelled rebuild setup while waiting for SAM queue capacity");
                         return false;
                    }

                    std::unique_lock<std::mutex> QueueLock(QueueMutex);
                    QueueCV.wait_for(QueueLock, std::chrono::milliseconds(100));
               }
          };

          std::vector<std::string> ExistingDocumentIDs;
          {
               std::lock_guard<std::mutex> Lock(DBMutex);
               const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
               std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));

               for (ManifestIterator->Seek(ManifestPrefix);
                    ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
                    ManifestIterator->Next())
               {
                    const std::string Key = ManifestIterator->key().ToString();

                    if (Key.size() > ManifestPrefix.size())
                    {
                         ExistingDocumentIDs.push_back(Key.substr(ManifestPrefix.size()));
                    }
               }
          }

          for (const auto& DocumentID : ExistingDocumentIDs)
          {
               bool Cancelled = false;
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);

                    if (IsCollectionCancelledLocked(Collection) ||
                        FlushInProgress.load(std::memory_order_acquire))
                    {
                         CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                         JobStatus.Running = false;
                         JobStatus.Completed = false;
                         if (JobStatus.ErrorMessage.empty())
                         {
                              JobStatus.ErrorMessage = "Cancelled.";
                         }
                         RecordDebugEvent(Collection, "cancelled rebuild setup while removing existing SAM terms");
                         Cancelled = true;
                    }
               }

               if (Cancelled)
               {
                    FinishTask();
                    return;
               }

               std::string RemoveError;

               if (!DeleteDocument(Collection, DocumentID, &RemoveError))
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);
                    CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                    if (JobStatus.ErrorMessage.empty())
                    {
                         JobStatus.ErrorMessage = RemoveError;
                    }
               }
          }

          const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");
          std::vector<Document> DocumentsToQueue;
          DocumentsToQueue.reserve(DocKeys.size());

          for (const auto& DocKey : DocKeys)
          {
               const size_t LastColon = DocKey.find_last_of(':');

               if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
               {
                    continue;
               }

               const std::string DocumentID = DocKey.substr(LastColon + 1);
               const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

               if (!Doc.ID.empty())
               {
                    DocumentsToQueue.push_back(Doc);
               }
          }

          {
               bool Cancelled = false;
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);
                    CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                    JobStatus.PendingDocuments = DocumentsToQueue.size();
                    JobStatus.TotalDocuments = DocumentsToQueue.size();

                    if (IsCollectionCancelledLocked(Collection) ||
                        FlushInProgress.load(std::memory_order_acquire))
                    {
                         JobStatus.Running = false;
                         JobStatus.Completed = false;
                         if (JobStatus.ErrorMessage.empty())
                         {
                              JobStatus.ErrorMessage = "Cancelled.";
                         }
                         RecordDebugEvent(Collection, "cancelled rebuild setup before queueing documents");
                         Cancelled = true;
                    }
               }

               if (Cancelled)
               {
                    FinishTask();
                    return;
               }
          }

          RecordDebugEvent(Collection,
                           Source.empty()
                                ? "queued background rebuild with " + std::to_string(DocumentsToQueue.size()) + " document(s)"
                                : "queued background rebuild from " + Source + " with " + std::to_string(DocumentsToQueue.size()) + " document(s)");

          for (const auto& Doc : DocumentsToQueue)
          {
               bool Cancelled = false;
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);

                    if (IsCollectionCancelledLocked(Collection) ||
                        FlushInProgress.load(std::memory_order_acquire))
                    {
                         CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                         JobStatus.Running = false;
                         JobStatus.Completed = false;
                         JobStatus.PendingDocuments = 0;
                         if (JobStatus.ErrorMessage.empty())
                         {
                              JobStatus.ErrorMessage = "Cancelled.";
                         }
                         RecordDebugEvent(Collection, "cancelled rebuild setup while queueing documents");
                         Cancelled = true;
                    }
               }

               if (Cancelled)
               {
                    FinishTask();
                    return;
               }

               if (!WaitForQueueCapacity())
               {
                    FinishTask();
                    return;
               }

               std::string QueueError;

               if (!EnqueueIndexDocument(Collection,
                                         Doc,
                                         &QueueError,
                                         HasExpectedMutationVersion,
                                         ExpectedMutationVersion,
                                         AllowLLMExpansion))
               {
                    bool QueueNeedsRetry = false;
                    {
                         std::lock_guard<std::mutex> Lock(JobMutex);
                         CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                         if (JobStatus.PendingDocuments > 0)
                         {
                              --JobStatus.PendingDocuments;
                         }
                         ++JobStatus.FailedDocuments;
                         JobStatus.ErrorMessage = QueueError;
                         if (HasExpectedMutationVersion &&
                             QueueError.find("Source collection changed during SAM indexing;") == 0)
                         {
                              JobStatus.NeedsRetry = true;
                              QueueNeedsRetry = true;
                         }
                    }

                    if (QueueNeedsRetry)
                    {
                         std::lock_guard<std::mutex> DBLock(DBMutex);

                         if (Database)
                         {
                              SAMCollectionState State;

                              std::string StateError;

                              if (ReadCollectionStateLocked(Database.get(), Collection, State, nullptr, &StateError))
                              {
                                   State.RebuildRequested = true;
                                   State.RequestedMutationVersion =
                                        GetCurrentCollectionMutationVersion(Collection);

                                   if (!WriteCollectionStateLocked(Database.get(), Collection, State, &StateError))
                                   {
                                        if (Instance && Instance->Logs)
                                        {
                                             Instance->Logs->Normal("sam", "Failed to persist rebuild retry state for '" + Collection + "': " + (StateError.empty() ? std::string("unknown error") : StateError) + ".");
                                        }
                                   }
                              }
                              else if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Normal("sam", "Failed to read collection state for '" + Collection + "' while scheduling rebuild retry: " + (StateError.empty() ? std::string("unknown error") : StateError) + ".");
                              }
                         }

                         if (SAMAutoRebuildEnabled())
                         {
                              ScheduleRetryRebuild(Collection);
                         }
                         else
                         {
                              RecordDebugEvent(Collection,
                                               "dropped stale automatic rebuild retry because SAM auto-index is disabled");
                         }
                    }
               }
          }

          if (DocumentsToQueue.empty())
          {
               std::string StateError;
               const bool ValidMutationVersion =
                    ValidateExpectedMutationVersion(Collection,
                                                   HasExpectedMutationVersion,
                                                   ExpectedMutationVersion,
                                                   &StateError);
               bool WroteIndexedMutationVersion = false;

               if (ValidMutationVersion)
               {
                    std::lock_guard<std::mutex> DBLock(DBMutex);
                    WroteIndexedMutationVersion =
                         WriteCollectionIndexedMutationVersionLocked(Database.get(),
                                                                     Collection,
                                                                     ExpectedMutationVersion,
                                                                     &StateError);
               }

               {
                    std::lock_guard<std::mutex> Lock(JobMutex);
                    CollectionJobStatus& JobStatus = CollectionJobs[Collection];
                    JobStatus.Running = false;

                    if (!ValidMutationVersion)
                    {
                         JobStatus.NeedsRetry = true;
                         JobStatus.Completed = false;
                         JobStatus.ErrorMessage = StateError;
                    }
                    else
                    {
                         JobStatus.Completed = WroteIndexedMutationVersion;
                         if (!WroteIndexedMutationVersion && !StateError.empty())
                         {
                              JobStatus.ErrorMessage = StateError;
                         }
                    }
               }

               if (!ValidMutationVersion)
               {
                    std::lock_guard<std::mutex> DBLock(DBMutex);

                    if (Database)
                    {
                         SAMCollectionState State;

                         std::string StateIOError;

                         if (ReadCollectionStateLocked(Database.get(), Collection, State, nullptr, &StateIOError))
                         {
                              State.RebuildRequested = true;
                              State.RequestedMutationVersion = GetCurrentCollectionMutationVersion(Collection);

                              if (!WriteCollectionStateLocked(Database.get(), Collection, State, &StateIOError))
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Normal("sam", "Failed to persist rebuild retry state for '" + Collection + "': " + (StateIOError.empty() ? std::string("unknown error") : StateIOError) + ".");
                                   }
                              }
                         }
                         else if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("sam", "Failed to read collection state for '" + Collection + "' while scheduling rebuild retry: " + (StateIOError.empty() ? std::string("unknown error") : StateIOError) + ".");
                         }
                    }

                    RecordDebugEvent(Collection, "rebuild invalidated before completion: " + StateError);
                    if (SAMAutoRebuildEnabled())
                    {
                         ScheduleRetryRebuild(Collection);
                    }
                    else
                    {
                         RecordDebugEvent(Collection,
                                          "dropped stale automatic rebuild retry because SAM auto-index is disabled");
                    }
               }
               else
               {
                    RecordDebugEvent(Collection, "rebuild complete: indexed 0, failed 0");
               }
          }

               FinishTask();
          }));
     }
     catch (const std::exception& E)
     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobStatus& JobStatus = CollectionJobs[Collection];
          JobStatus.Running = false;
          JobStatus.Completed = false;
          JobStatus.PendingDocuments = 0;
          JobStatus.ErrorMessage = E.what();
          auto ActiveIt = ActiveCollectionTasks.find(Collection);
          if (ActiveIt != ActiveCollectionTasks.end())
          {
               if (ActiveIt->second > 0)
               {
                    --ActiveIt->second;
               }

               if (ActiveIt->second == 0)
               {
                    ActiveCollectionTasks.erase(ActiveIt);
               }
          }
          JobStateCV.notify_all();

          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
     }

     return true;
}

bool SAM::NotifyCollectionChanged(const std::string& Collection,
                                  uint64_t MutationVersion,
                                  std::string* ErrorMessage)
{
     if (Collection.empty() || Collection == "*")
     {
          return true;
     }

     if (MutationVersion == 0)
     {
          MutationVersion = GetCurrentCollectionMutationVersion(Collection);
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     SAMCollectionState State;
     std::string StateError;

     if (!ReadCollectionStateLocked(Database.get(), Collection, State, nullptr, &StateError))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = StateError;
          }

          return false;
     }

     if (State.RebuildRequested && State.RequestedMutationVersion >= MutationVersion)
     {
          return true;
     }

     State.RebuildRequested = true;
     State.RequestedMutationVersion = std::max(State.RequestedMutationVersion, MutationVersion);

     if (!WriteCollectionStateLocked(Database.get(), Collection, State, &StateError))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = StateError;
          }

          return false;
     }

     RecordDebugEvent(Collection,
                      "marked collection dirty for automatic rebuild at mutation version " +
                           std::to_string(State.RequestedMutationVersion));
     return true;
}

bool SAM::EnqueueIndexDocument(const std::string& Collection,
                               const Document& Doc,
                               std::string* ErrorMessage,
                               bool HasExpectedMutationVersion,
                               uint64_t ExpectedMutationVersion,
                               bool AllowLLMExpansion)
{
     {
          std::lock_guard<std::mutex> Lock(JobMutex);

          if (FlushInProgress.load(std::memory_order_acquire))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM flush is in progress.";
               }

               return false;
          }

          if (IsCollectionCancelledLocked(Collection))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Collection SAM work is being cancelled.";
               }

               return false;
          }
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }
     }

     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     const std::string PendingKey = BuildPendingIndexKey(Collection, Doc.ID);
     bool PersistQueueItem = false;
     bool PersistHasExpectedMutationVersion = HasExpectedMutationVersion;
     uint64_t PersistExpectedMutationVersion = ExpectedMutationVersion;

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);

          if (!PendingIndexKeys.insert(PendingKey).second)
          {
	               for (auto& ExistingJob : PendingIndexJobs)
	               {
	                    if (ExistingJob.Collection == Collection && ExistingJob.Doc.ID == Doc.ID)
	                    {
	                         ExistingJob.Doc = Doc;
	                         ExistingJob.AllowLLMExpansion =
	                              ExistingJob.AllowLLMExpansion || AllowLLMExpansion;

	                         if (HasExpectedMutationVersion || !ExistingJob.HasExpectedMutationVersion)
	                         {
	                              ExistingJob.HasExpectedMutationVersion = HasExpectedMutationVersion;
	                              ExistingJob.ExpectedMutationVersion = ExpectedMutationVersion;
	                         }
                         PersistHasExpectedMutationVersion = ExistingJob.HasExpectedMutationVersion;
                         PersistExpectedMutationVersion = ExistingJob.ExpectedMutationVersion;
                         PersistQueueItem = true;
                         break;
                    }
               }

               /* Fall through to persist the refreshed job payload. */
          }
          else
          {
               PendingIndexJobs.push_back(PendingIndexJob{Collection,
                                                          Doc,
                                                          HasExpectedMutationVersion,
                                                          ExpectedMutationVersion,
                                                          AllowLLMExpansion});
               PersistQueueItem = true;
          }
     }

     if (PersistQueueItem)
     {
          std::lock_guard<std::mutex> DBLock(DBMutex);
          if (Database)
          {
               nlohmann::json Root;
               Root["has_expected_mutation_version"] = PersistHasExpectedMutationVersion;
               Root["expected_mutation_version"] = PersistExpectedMutationVersion;
               (void)Database->Put(rocksdb::WriteOptions(),
                                   BuildPendingIndexQueueKey(Collection, Doc.ID),
                                   Root.dump());
          }
     }

     RecordDebugEvent(Collection, "queued background index for " + Doc.ID);
     QueueCV.notify_one();
     return true;
}

/* Remove persisted SAM term and manifest data for one source document. */

bool SAM::RemoveExistingDocumentTermsLocked(const std::string& Collection,
                                            const std::string& DocumentID,
                                            std::string* ErrorMessage,
                                            bool ScanAllTermKeys)
{
     const std::string ManifestKey = BuildDocManifestKey(Collection, DocumentID);
     std::string ExistingValue;
     const rocksdb::Status GetStatus = Database->Get(rocksdb::ReadOptions(), ManifestKey, &ExistingValue);

     if (!GetStatus.IsNotFound() && !GetStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = GetStatus.ToString();
          }

          return false;
     }

     rocksdb::WriteBatch Batch;

     try
     {
          if (!GetStatus.IsNotFound())
          {
               nlohmann::json Root = nlohmann::json::parse(ExistingValue);

               if (Root.contains("terms") && Root["terms"].is_array())
               {
                    for (const auto& Entry : Root["terms"])
                    {
                         if (Entry.is_string())
                         {
                              Batch.Delete(BuildTermKey(Entry.get<std::string>(), Collection, DocumentID));
                         }
                         else if (Entry.is_object())
                         {
                              const std::string Text = Entry.value("text", "");

                              if (!Text.empty())
                              {
                                   Batch.Delete(BuildTermKey(Text, Collection, DocumentID));
                              }
                         }
                    }
               }

               if (Root.contains("semantic_index") && Root["semantic_index"].is_array())
               {
                    for (const auto& Entry : Root["semantic_index"])
                    {
                         if (!Entry.is_object())
                         {
                              continue;
                         }

                         const std::string Text = Entry.value("text", "");
                         const std::string Kind = Entry.value("kind", "semantic");

                         if (!Text.empty())
                         {
                              Batch.Delete(BuildSemanticProfileKey(Text, Collection, DocumentID, Kind));
                         }
                    }
               }
          }
     }
     catch (const std::exception& E)
     {
          if (!ScanAllTermKeys)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = E.what();
               }

               return false;
          }
     }

     if (ScanAllTermKeys)
     {
          const std::string TermPrefix = "sam:term:";
          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(TermPrefix);
               Iterator->Valid() && Iterator->key().starts_with(TermPrefix);
               Iterator->Next())
          {
               try
               {
                    const nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());

                    if (Payload.value("collection", "") == Collection &&
                        Payload.value("id", "") == DocumentID)
                    {
                         Batch.Delete(Iterator->key());
                    }
               }
               catch (...)
               {
               }
          }

          const std::string SemanticPrefix = "sam:semantic:";
          std::unique_ptr<rocksdb::Iterator> SemanticIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (SemanticIterator->Seek(SemanticPrefix);
               SemanticIterator->Valid() && SemanticIterator->key().starts_with(SemanticPrefix);
               SemanticIterator->Next())
          {
               try
               {
                    const nlohmann::json Payload = nlohmann::json::parse(SemanticIterator->value().ToString());

                    if (Payload.value("collection", "") == Collection &&
                        Payload.value("id", "") == DocumentID)
                    {
                         Batch.Delete(SemanticIterator->key());
                    }
               }
               catch (...)
               {
               }
          }
     }

     Batch.Delete(ManifestKey);

     const rocksdb::Status WriteStatus = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     return true;
}

/* Index one source document into SAM after validating the main store state. */

bool SAM::IndexDocumentLocked(const std::string& Collection,
                              const Document& Doc,
                              std::string* ErrorMessage,
                              bool HasExpectedMutationVersion,
                              uint64_t ExpectedMutationVersion,
                              bool AllowLLMExpansion)
{
     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     if (!ValidateExpectedMutationVersion(Collection,
                                          HasExpectedMutationVersion,
                                          ExpectedMutationVersion,
                                          ErrorMessage))
     {
          RecordDebugEvent(Collection, "discarded stale SAM job for " + Doc.ID + " before term expansion");
          return false;
     }

     Document SourceDoc = HybridStorageManager::GetInstance().GetDocument(Collection, Doc.ID);

     if (SourceDoc.ID.empty())
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          if (!RemoveExistingDocumentTermsLocked(Collection, Doc.ID, ErrorMessage, true))
          {
               return false;
          }

          RecordDebugEvent(Collection, "removed stale SAM index for deleted source document " + Doc.ID);
          return true;
     }

     if (BuildSAMSourceDocumentFingerprint(SourceDoc) != BuildSAMSourceDocumentFingerprint(Doc))
     {
          RecordDebugEvent(Collection, "refreshing stale queued SAM job for " + Doc.ID + " from main document store");
     }

     std::string TermsError;
     const std::vector<TermEntry> Terms = ExpandDocumentTerms(Collection, SourceDoc, &TermsError, AllowLLMExpansion);
     if (Terms.empty())
     {
          const std::string FailureMessage = TermsError.empty()
               ? std::string("SAM indexing produced no usable terms.")
               : TermsError;

          RecordDebugEvent(Collection, "failed to index " + Doc.ID + ": " + FailureMessage);

          if (ErrorMessage)
          {
               *ErrorMessage = FailureMessage;
          }

          return false;
     }

     /* 
      * If a collection has not yet built a collection profile, avoid persisting
      * large synthetic expansion terms (LLM/context/iterative) into the doc manifest.
      * This keeps "ranked terms" from existing until the collection has been iterated.
      */
      
     bool HasCollectionProfile = false;
     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               std::string RawProfile;
               const rocksdb::Status Status = Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);
               HasCollectionProfile = Status.ok() && !RawProfile.empty();

               if (HasCollectionProfile)
               {
                    try
                    {
                         const nlohmann::json Root = nlohmann::json::parse(RawProfile);
                         const int64_t DocCount = std::max<int64_t>(0, Root.value("document_count", static_cast<int64_t>(0)));
                         HasCollectionProfile = (DocCount > 0);
                    }
                    catch (...)
                    {
                         HasCollectionProfile = false;
                    }
               }
          }
     }

     std::vector<TermEntry> PersistedTerms;
     PersistedTerms.reserve(Terms.size());

     if (HasCollectionProfile)
     {
          PersistedTerms = Terms;
     }
     else
     {
          for (const auto& Term : Terms)
          {
               const std::string& Source = Term.Source;

               if (Source.rfind("llm_", 0) == 0 ||
                   Source.rfind("context_", 0) == 0 ||
                   Source.rfind("iterative_", 0) == 0 ||
                   Source.rfind("query_", 0) == 0 ||
                   Source == "profile_context")
               {
                    continue;
               }

               PersistedTerms.push_back(Term);
          }
     }

     {
          std::ostringstream Preview;
          const size_t PreviewCount = std::min<size_t>(PersistedTerms.size(), 3);

          for (size_t Index = 0; Index < PreviewCount; ++Index)
          {
               if (Index > 0)
               {
                    Preview << ", ";
               }

               Preview << PersistedTerms[Index].Text;
          }

          RecordDebugEvent(Collection,
                           "indexed " + SourceDoc.ID + " with " + std::to_string(PersistedTerms.size()) +
                                " term(s): " + Preview.str());
     }

     std::unique_lock<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!RemoveExistingDocumentTermsLocked(Collection, SourceDoc.ID, ErrorMessage))
     {
          return false;
     }

     rocksdb::WriteBatch Batch;
     nlohmann::json Manifest;
     Manifest["collection"] = Collection;
     Manifest["id"] = SourceDoc.ID;
     Manifest["title"] = SourceDoc.Title;
     Manifest["source_timestamp"] = SourceDoc.Timestamp;
     Manifest["source_fingerprint"] = BuildSAMSourceDocumentFingerprint(SourceDoc);
     Manifest["lang"] = sam::lang::DetectDocumentLanguage(Collection, SourceDoc);
     Manifest["label"] = DetectSAMDocumentLabel(SourceDoc);
     Manifest["format"] = DetectSAMDocumentFormat(SourceDoc);
     Manifest["terms"] = nlohmann::json::array();
     const SAMSemanticProfile SemanticProfile = BuildSemanticProfile(SourceDoc.Title.empty() ? SourceDoc.ID : SourceDoc.Title, PersistedTerms);
     const std::vector<SAMSemanticIndexEntry> SemanticIndex = BuildSemanticIndexEntries(SemanticProfile, 32);

     for (const auto& Term : PersistedTerms)
     {
          Manifest["terms"].push_back({
               {"text", Term.Text},
               {"kind", Term.Kind},
               {"source", Term.Source},
               {"score", Term.Score},
               {"signal", Term.Signal}
          });

          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = SourceDoc.ID;
          Payload["title"] = SourceDoc.Title;
          Payload["term"] = Term.Text;
          Payload["kind"] = Term.Kind;
          Payload["source"] = Term.Source;
          Payload["score"] = Term.Score;
          Payload["signal"] = Term.Signal;
          Batch.Put(BuildTermKey(Term.Text, Collection, SourceDoc.ID), Payload.dump());
     }

     StoreSemanticProfileJSON(Manifest, SemanticProfile);
     Manifest["semantic_index"] = nlohmann::json::array();

     for (const auto& Entry : SemanticIndex)
     {
          Manifest["semantic_index"].push_back({
               {"text", Entry.Text},
               {"kind", Entry.Kind}
          });

          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = SourceDoc.ID;
          Payload["title"] = SourceDoc.Title;
          Payload["term"] = Entry.Text;
          Payload["kind"] = Entry.Kind;
          Batch.Put(BuildSemanticProfileKey(Entry.Text, Collection, SourceDoc.ID, Entry.Kind), Payload.dump());
     }

     Batch.Put(BuildDocManifestKey(Collection, SourceDoc.ID), Manifest.dump());

     if (!ValidateExpectedMutationVersion(Collection,
                                          HasExpectedMutationVersion,
                                          ExpectedMutationVersion,
                                          ErrorMessage))
     {
          RecordDebugEvent(Collection, "discarded stale SAM job before commit for " + Doc.ID);
          return false;
     }

     const Document LatestDoc = HybridStorageManager::GetInstance().GetDocument(Collection, SourceDoc.ID);

     if (LatestDoc.ID.empty())
     {
          RecordDebugEvent(Collection, "discarded SAM commit for deleted source document " + SourceDoc.ID);
          return true;
     }

     if (BuildSAMSourceDocumentFingerprint(LatestDoc) != BuildSAMSourceDocumentFingerprint(SourceDoc))
     {
          RecordDebugEvent(Collection, "restarting SAM index for " + SourceDoc.ID + " because source changed before commit");
          Lock.unlock();
          return IndexDocumentLocked(Collection,
                                     LatestDoc,
                                     ErrorMessage,
                                     HasExpectedMutationVersion,
                                     ExpectedMutationVersion,
                                     AllowLLMExpansion);
     }

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::IndexDocument(const std::string& Collection,
                        const Document& Doc,
                        std::string* ErrorMessage,
                        bool HasExpectedMutationVersion,
                        uint64_t ExpectedMutationVersion,
                        bool AllowLLMExpansion)
{
     return IndexDocumentLocked(Collection,
                                Doc,
                                ErrorMessage,
                                HasExpectedMutationVersion,
                                ExpectedMutationVersion,
                                AllowLLMExpansion);
}

bool SAM::StoreDocumentContext(const std::string& Collection,
                               const std::string& DocumentID,
                               const std::string& SourceFingerprint,
                               uint64_t UpdatedAtMS,
                               const nlohmann::json& Suggestions,
                               std::string* ErrorMessage)
{
     if (Collection.empty() || DocumentID.empty() || !Suggestions.is_array())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     nlohmann::json EnrichedSuggestions = Suggestions;
     std::string PreviousRawValue;
     const rocksdb::Status PreviousStatus =
          Database->Get(rocksdb::ReadOptions(),
                        BuildDocumentContextKey(Collection, DocumentID),
                        &PreviousRawValue);

     if (PreviousStatus.ok() && !PreviousRawValue.empty())
     {
          try
          {
               const nlohmann::json Previous = nlohmann::json::parse(PreviousRawValue);
               std::unordered_map<std::string, nlohmann::json> PreviousByTerm;

               for (const auto& Item : Previous.value("suggestions", nlohmann::json::array()))
               {
                    if (Item.is_object() && !Item.value("term", "").empty())
                    {
                         PreviousByTerm[Item.value("term", "")] = Item;
                    }
               }

               for (auto& Item : EnrichedSuggestions)
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    const auto Existing = PreviousByTerm.find(Item.value("term", ""));

                    if (Existing == PreviousByTerm.end())
                    {
                         continue;
                    }

                    const nlohmann::json& Old = Existing->second;
                    Item["feedback_uses"] = Old.value("feedback_uses", 0U);
                    Item["interaction_uses"] = Old.value("interaction_uses", 0U);
                    Item["last_used_ms"] = Old.value("last_used_ms", static_cast<uint64_t>(0));
                    Item["misses"] = Old.value("misses", 0U);
                    Item["confidence"] = std::max(Item.value("confidence", 0.0),
                                                  Old.value("confidence", 0.0));
                    Item["provisional"] =
                         Item.value("confidence", 0.0) < 0.78 ||
                         Item.value("validation_score", 0.0) < 0.70;
               }
          }
          catch (...)
          {
          }
     }

     const double AuditScore = ScoreDocumentContextSuggestions(EnrichedSuggestions);
     const bool WellCovered = AuditScore >= 0.72;
     nlohmann::json Root = {
          {"collection", Collection},
          {"id", DocumentID},
          {"source_fingerprint", SourceFingerprint},
          {"updated_at_ms", UpdatedAtMS},
          {"audit_status", WellCovered ? "well_covered" : "low_confidence"},
          {"audit_score", AuditScore},
          {"revisit_after_ms", UpdatedAtMS + (WellCovered
               ? kDocumentContextHealthyRetryMS
               : kDocumentContextWeakRetryMS)},
          {"suggestions", std::move(EnrichedSuggestions)}
     };
     const rocksdb::Status Status =
          Database->Put(rocksdb::WriteOptions(), BuildDocumentContextKey(Collection, DocumentID), Root.dump());

     if (!Status.ok() && ErrorMessage)
     {
          *ErrorMessage = Status.ToString();
     }

     return Status.ok();
}

bool SAM::LoadDocumentContext(const std::string& Collection,
                              const std::string& DocumentID,
                              nlohmann::json& Root,
                              std::string* ErrorMessage) const
{
     Root = nlohmann::json::object();
     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     std::string RawValue;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildDocumentContextKey(Collection, DocumentID), &RawValue);

     if (!Status.ok())
     {
          if (!Status.IsNotFound() && ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }
          return false;
     }

     try
     {
          Root = nlohmann::json::parse(RawValue);
          return true;
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }
          return false;
     }
}

bool SAM::LoadCollectionPromptProfile(const std::string& Collection,
                                      nlohmann::json& Root,
                                      std::string* ErrorMessage) const
{
     Root = nlohmann::json::object();

     if (Collection.empty())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     std::string RawValue;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawValue);

     if (!Status.ok())
     {
          if (!Status.IsNotFound() && ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     try
     {
          const nlohmann::json Profile = nlohmann::json::parse(RawValue);
          Root["summary"] = Profile.value("llm_summary", Profile.value("summary", ""));
          Root["sampled_summary"] = Profile.value("summary", "");
          Root["summary_terms"] = Profile.value("summary_terms", nlohmann::json::array());
          Root["negative_terms"] = Profile.value("negative_terms", nlohmann::json::array());
          Root["documents"] = Profile.value("documents", 0U);
          Root["profile_synced_at_ms"] = Profile.value("profile_synced_at_ms", static_cast<uint64_t>(0));
          return true;
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
     }
}

bool SAM::StoreCollectionPromptSummary(const std::string& Collection,
                                       const std::string& Summary,
                                       std::string* ErrorMessage)
{
     if (Collection.empty() || Summary.empty())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     const std::string ProfileKey = BuildCollectionProfileKey(Collection);
     std::string RawValue;
     nlohmann::json Profile = nlohmann::json::object();
     const rocksdb::Status ReadStatus =
          Database->Get(rocksdb::ReadOptions(), ProfileKey, &RawValue);

     if (ReadStatus.ok() && !RawValue.empty())
     {
          try
          {
               Profile = nlohmann::json::parse(RawValue);
          }
          catch (...)
          {
               Profile = nlohmann::json::object();
          }
     }
     else if (!ReadStatus.IsNotFound())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = ReadStatus.ToString();
          }

          return false;
     }

     Profile["collection"] = Collection;
     Profile["llm_summary"] = Summary.substr(0, 512);
     Profile["llm_summary_updated_at_ms"] = static_cast<uint64_t>(Instance ? Instance->NowMs() : 0);
     const rocksdb::Status WriteStatus =
          Database->Put(rocksdb::WriteOptions(), ProfileKey, Profile.dump());

     if (!WriteStatus.ok() && ErrorMessage)
     {
          *ErrorMessage = WriteStatus.ToString();
     }

     return WriteStatus.ok();
}

bool SAM::RemoveDocumentContext(const std::string& Collection,
                                const std::string& DocumentID,
                                std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return false;
     }

     const rocksdb::Status Status =
          Database->Delete(rocksdb::WriteOptions(), BuildDocumentContextKey(Collection, DocumentID));

     if (!Status.ok() && ErrorMessage)
     {
          *ErrorMessage = Status.ToString();
     }

     return Status.ok();
}

/* Delete SAM state for one source document and clear queued work. */

bool SAM::DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage)
{
     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexKeys.erase(BuildPendingIndexKey(Collection, DocumentID));
          PendingIndexJobs.erase(
               std::remove_if(PendingIndexJobs.begin(), PendingIndexJobs.end(),
                              [&](const PendingIndexJob& Job)
                              {
                                   return Job.Collection == Collection && Job.Doc.ID == DocumentID;
                              }),
               PendingIndexJobs.end());
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     (void)Database->Delete(rocksdb::WriteOptions(), BuildPendingIndexQueueKey(Collection, DocumentID));
     (void)Database->Delete(rocksdb::WriteOptions(), BuildDocumentContextKey(Collection, DocumentID));
     return RemoveExistingDocumentTermsLocked(Collection, DocumentID, ErrorMessage, true);
}

bool SAM::DeleteCollection(const std::string& Collection, std::string* ErrorMessage)
{
     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexJobs.erase(
               std::remove_if(PendingIndexJobs.begin(), PendingIndexJobs.end(),
                              [&](const PendingIndexJob& Job)
                              {
                                   return Job.Collection == Collection;
                              }),
               PendingIndexJobs.end());

          for (auto It = PendingIndexKeys.begin(); It != PendingIndexKeys.end(); )
          {
               const std::string Prefix = Collection + std::string(1, '\0');
               if (It->rfind(Prefix, 0) == 0)
               {
                    It = PendingIndexKeys.erase(It);
               }
               else
               {
                    ++It;
               }
          }
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "SAM database is not open.";
               }

               return false;
          }

          std::unique_ptr<rocksdb::Iterator> QueueIterator(Database->NewIterator(rocksdb::ReadOptions()));
          const std::string QueuePrefix = std::string(kPendingIndexQueuePrefix) + Collection + std::string(1, '\0');
          std::vector<std::string> QueueKeysToDelete;
         
          for (QueueIterator->Seek(QueuePrefix);
               QueueIterator->Valid() && QueueIterator->key().starts_with(QueuePrefix);
               QueueIterator->Next())
          {
               QueueKeysToDelete.push_back(QueueIterator->key().ToString());
          }

          if (!QueueKeysToDelete.empty())
          {
               rocksdb::WriteBatch Batch;
         
               for (const auto& Key : QueueKeysToDelete)
               {
                    Batch.Delete(Key);
               }
         
               (void)Database->Write(rocksdb::WriteOptions(), &Batch);
          }

          std::vector<std::string> ExistingDocumentIDs;
          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (ManifestIterator->Seek(ManifestPrefix);
               ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
               ManifestIterator->Next())
          {
               const std::string Key = ManifestIterator->key().ToString();

               if (Key.size() > ManifestPrefix.size())
               {
                    ExistingDocumentIDs.push_back(Key.substr(ManifestPrefix.size()));
               }
          }

          for (const auto& DocumentID : ExistingDocumentIDs)
          {
               if (!RemoveExistingDocumentTermsLocked(Collection, DocumentID, ErrorMessage))
               {
                    return false;
               }
          }

          rocksdb::WriteBatch Batch;
          Batch.Delete(BuildCollectionProfileKey(Collection));
          Batch.Delete(BuildCollectionStateKey(Collection));
          Batch.Delete(BuildLexicalMirrorKey("synonyms", Collection));
          Batch.Delete(BuildLexicalMirrorKey("stopwords", Collection));

          const std::string IdeaPrefix = BuildSearchIdeaPrefix(Collection);
          std::unique_ptr<rocksdb::Iterator> IdeaIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (IdeaIterator->Seek(IdeaPrefix);
               IdeaIterator->Valid() && IdeaIterator->key().starts_with(IdeaPrefix);
               IdeaIterator->Next())
          {
               Batch.Delete(IdeaIterator->key());
          }

          const std::string ContextPrefix = std::string(kDocumentContextPrefix) + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ContextIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (ContextIterator->Seek(ContextPrefix);
               ContextIterator->Valid() && ContextIterator->key().starts_with(ContextPrefix);
               ContextIterator->Next())
          {
               Batch.Delete(ContextIterator->key());
          }

          const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

          if (!Status.ok())
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = Status.ToString();
               }

               return false;
          }
     }

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          CollectionJobs.erase(Collection);
          ActiveCollectionTasks.erase(Collection);
          NextContextAuditOffset.erase(Collection);
          CancelledCollections.erase(Collection);
     }

     return true;
}
