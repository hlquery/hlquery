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
#include <sstream>

#include "core/hlquery.h"
#include "sam/lang.h"
#include "sam/sam.h"
#include "sam/sam_internal.h"
#include "search/storageengine.h"
#include "vendor/json/json.hpp"

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
     return CancelAllRequested || CancelledCollections.find(Collection) != CancelledCollections.end();
}

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

     StartIndexWorker();

     return true;
}

void SAM::Shutdown()
{
     std::vector<std::thread> ThreadsToJoin;
     std::shared_ptr<rocksdb::DB> DatabaseToRelease;

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);
          ShuttingDown = true;
     }

     QueueCV.notify_all();

     {
          std::lock_guard<std::mutex> Lock(JobMutex);
          ThreadsToJoin.swap(WorkerThreads);
     }

     for (auto& Worker : ThreadsToJoin)
     {
          if (Worker.joinable())
          {
               Worker.join();
          }
     }

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseOpen.store(false, std::memory_order_release);
          DatabaseToRelease = std::move(Database);
     }
}

std::string SAM::BuildPendingIndexKey(const std::string& Collection, const std::string& DocumentID)
{
     return Collection + "\n" + DocumentID;
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
}

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
          const bool Success = IndexDocument(Job.Collection,
                                             Job.Doc,
                                             &ErrorMessage,
                                             Job.HasExpectedMutationVersion,
                                             Job.ExpectedMutationVersion);

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
               else
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

          FinishTask();

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
               Instance->Logs->Normal("sam",
                                      "Failed to background index '" + Job.Collection + "/" + Job.Doc.ID +
                                           "': " + (ErrorMessage.empty() ? std::string("unknown error") : ErrorMessage) + ".");
          }
     }
}

size_t SAM::ResolveBackgroundWorkerCount() const
{
     const unsigned int HardwareThreads = std::thread::hardware_concurrency();

     if (HardwareThreads == 0)
     {
          return 2;
     }

     return std::max<size_t>(1, std::min<size_t>(static_cast<size_t>(HardwareThreads), 4));
}

bool SAM::IsOpen() const
{
     return DatabaseOpen.load(std::memory_order_acquire);
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
          Instance->Logs->Normal("sam",
                                 "SAM recreate complete: indexed " + std::to_string(IndexedDocuments) +
                                      " documents, failed " + std::to_string(FailedDocuments) + ".");
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
                    Instance->Logs->Normal("sam",
                                           "Failed to index '" + Collection + "/" + DocumentID +
                                                "' during collection rebuild: " + IndexError + ".");
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
               Instance->Logs->Normal("sam",
                                      "Failed to rebuild SAM collection profile for '" + Collection +
                                           "': " + ProfileError + ".");
          }

          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = ProfileError;
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam",
                                 "SAM rebuild for collection '" + Collection + "' complete: indexed " +
                                      std::to_string(IndexedCount) + " documents, failed " +
                                      std::to_string(FailedCount) + ".");
     }

     return true;
}

bool SAM::StartRecreateCollectionAsync(const std::string& Collection,
                                       bool* AlreadyRunning,
                                       std::string* ErrorMessage)
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
          JobStatus.ErrorMessage.clear();
          CancelledCollections.erase(Collection);
     }

     RecordDebugEvent(Collection,
                      "queued background rebuild setup");

     const bool HasExpectedMutationVersion = Instance && Instance->API;
     const uint64_t ExpectedMutationVersion =
          HasExpectedMutationVersion ? GetCurrentCollectionMutationVersion(Collection) : 0;

     std::thread([this, Collection, HasExpectedMutationVersion, ExpectedMutationVersion]()
     {
          {
               std::lock_guard<std::mutex> Lock(JobMutex);
               ++ActiveCollectionTasks[Collection];
          }

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

                    if (IsCollectionCancelledLocked(Collection))
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

                    if (IsCollectionCancelledLocked(Collection))
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
                           "queued background rebuild with " + std::to_string(DocumentsToQueue.size()) + " document(s)");

          for (const auto& Doc : DocumentsToQueue)
          {
               bool Cancelled = false;
               {
                    std::lock_guard<std::mutex> Lock(JobMutex);

                    if (IsCollectionCancelledLocked(Collection))
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

               std::string QueueError;

               if (!EnqueueIndexDocument(Collection,
                                         Doc,
                                         &QueueError,
                                         HasExpectedMutationVersion,
                                         ExpectedMutationVersion))
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
                    }
               }
          }

          if (DocumentsToQueue.empty())
          {
               std::lock_guard<std::mutex> Lock(JobMutex);
               CollectionJobStatus& JobStatus = CollectionJobs[Collection];
               JobStatus.Running = false;
               std::string StateError;
               if (!ValidateExpectedMutationVersion(Collection,
                                                    HasExpectedMutationVersion,
                                                    ExpectedMutationVersion,
                                                    &StateError))
               {
                    JobStatus.NeedsRetry = true;
                    JobStatus.Completed = false;
                    JobStatus.ErrorMessage = StateError;
                    RecordDebugEvent(Collection, "rebuild invalidated before completion: " + StateError);
               }
               else
               {
                    bool WroteIndexedMutationVersion = false;
                    {
                         std::lock_guard<std::mutex> DBLock(DBMutex);
                         WroteIndexedMutationVersion =
                              WriteCollectionIndexedMutationVersionLocked(Database.get(),
                                                                          Collection,
                                                                          ExpectedMutationVersion,
                                                                          &StateError);
                    }
                    JobStatus.Completed = WroteIndexedMutationVersion;
                    if (!WroteIndexedMutationVersion && !StateError.empty())
                    {
                         JobStatus.ErrorMessage = StateError;
                    }
                    RecordDebugEvent(Collection, "rebuild complete: indexed 0, failed 0");
               }
          }

          FinishTask();
     }).detach();

     return true;
}

bool SAM::EnqueueIndexDocument(const std::string& Collection,
                               const Document& Doc,
                               std::string* ErrorMessage,
                               bool HasExpectedMutationVersion,
                               uint64_t ExpectedMutationVersion)
{
     {
          std::lock_guard<std::mutex> Lock(JobMutex);

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

     {
          std::lock_guard<std::mutex> Lock(QueueMutex);

          if (!PendingIndexKeys.insert(PendingKey).second)
          {
               for (auto& ExistingJob : PendingIndexJobs)
               {
                    if (ExistingJob.Collection == Collection && ExistingJob.Doc.ID == Doc.ID)
                    {
                         ExistingJob.Doc = Doc;
                         ExistingJob.HasExpectedMutationVersion = HasExpectedMutationVersion;
                         ExistingJob.ExpectedMutationVersion = ExpectedMutationVersion;
                         break;
                    }
               }

               return true;
          }

          PendingIndexJobs.push_back(PendingIndexJob{Collection,
                                                     Doc,
                                                     HasExpectedMutationVersion,
                                                     ExpectedMutationVersion});
     }

     RecordDebugEvent(Collection, "queued background index for " + Doc.ID);
     QueueCV.notify_one();
     return true;
}

bool SAM::RemoveExistingDocumentTermsLocked(const std::string& Collection,
                                            const std::string& DocumentID,
                                            std::string* ErrorMessage)
{
     const std::string ManifestKey = BuildDocManifestKey(Collection, DocumentID);
     std::string ExistingValue;
     const rocksdb::Status GetStatus = Database->Get(rocksdb::ReadOptions(), ManifestKey, &ExistingValue);

     if (GetStatus.IsNotFound())
     {
          return true;
     }

     if (!GetStatus.ok())
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
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
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

bool SAM::IndexDocumentLocked(const std::string& Collection,
                              const Document& Doc,
                              std::string* ErrorMessage,
                              bool HasExpectedMutationVersion,
                              uint64_t ExpectedMutationVersion)
{
     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     std::string TermsError;
     const std::vector<TermEntry> Terms = ExpandDocumentTerms(Collection, Doc, &TermsError);
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
     else
     {
          std::ostringstream Preview;
          const size_t PreviewCount = std::min<size_t>(Terms.size(), 3);

          for (size_t Index = 0; Index < PreviewCount; ++Index)
          {
               if (Index > 0)
               {
                    Preview << ", ";
               }

               Preview << Terms[Index].Text;
          }

          RecordDebugEvent(Collection,
                           "indexed " + Doc.ID + " with " + std::to_string(Terms.size()) +
                                " term(s): " + Preview.str());
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

     if (!ValidateExpectedMutationVersion(Collection,
                                          HasExpectedMutationVersion,
                                          ExpectedMutationVersion,
                                          ErrorMessage))
     {
          RecordDebugEvent(Collection, "discarded stale SAM job for " + Doc.ID);
          return false;
     }

     if (!RemoveExistingDocumentTermsLocked(Collection, Doc.ID, ErrorMessage))
     {
          return false;
     }

     rocksdb::WriteBatch Batch;
     nlohmann::json Manifest;
     Manifest["collection"] = Collection;
     Manifest["id"] = Doc.ID;
     Manifest["title"] = Doc.Title;
     Manifest["source_timestamp"] = Doc.Timestamp;
     Manifest["source_fingerprint"] = BuildSAMSourceDocumentFingerprint(Doc);
     Manifest["lang"] = sam::lang::DetectDocumentLanguage(Collection, Doc);
     Manifest["label"] = DetectSAMDocumentLabel(Doc);
     Manifest["format"] = DetectSAMDocumentFormat(Doc);
     Manifest["terms"] = nlohmann::json::array();
     const SAMSemanticProfile SemanticProfile = BuildSemanticProfile(Doc.Title.empty() ? Doc.ID : Doc.Title, Terms);

     for (const auto& Term : Terms)
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
          Payload["id"] = Doc.ID;
          Payload["title"] = Doc.Title;
          Payload["term"] = Term.Text;
          Payload["kind"] = Term.Kind;
          Payload["source"] = Term.Source;
          Payload["score"] = Term.Score;
          Payload["signal"] = Term.Signal;
          Batch.Put(BuildTermKey(Term.Text, Collection, Doc.ID), Payload.dump());
     }

     StoreSemanticProfileJSON(Manifest, SemanticProfile);

     Batch.Put(BuildDocManifestKey(Collection, Doc.ID), Manifest.dump());

     if (!ValidateExpectedMutationVersion(Collection,
                                          HasExpectedMutationVersion,
                                          ExpectedMutationVersion,
                                          ErrorMessage))
     {
          RecordDebugEvent(Collection, "discarded stale SAM job before commit for " + Doc.ID);
          return false;
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
                        uint64_t ExpectedMutationVersion)
{
     return IndexDocumentLocked(Collection,
                                Doc,
                                ErrorMessage,
                                HasExpectedMutationVersion,
                                ExpectedMutationVersion);
}

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

     return RemoveExistingDocumentTermsLocked(Collection, DocumentID, ErrorMessage);
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
               if (It->rfind(Collection + "\n", 0) == 0)
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

          const std::string IdeaPrefix = BuildSearchIdeaPrefix(Collection);
          std::unique_ptr<rocksdb::Iterator> IdeaIterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (IdeaIterator->Seek(IdeaPrefix);
               IdeaIterator->Valid() && IdeaIterator->key().starts_with(IdeaPrefix);
               IdeaIterator->Next())
          {
               Batch.Delete(IdeaIterator->key());
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
          CancelledCollections.erase(Collection);
     }

     return true;
}
