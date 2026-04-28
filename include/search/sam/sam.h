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

#pragma once

#ifndef ROCKSDB_NAMESPACE
#define ROCKSDB_NAMESPACE rocksdb
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <map>
#include <rocksdb/db.h>
#include <deque>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "search/cstore.h"

/*
 * Secondary Assistant Manager.
 * Maintains a second RocksDB database with heuristic/LLM-derived lookup phrases
 * that can be used to discover documents through broader natural-language intent.
 */

class SAM
{
   public:

     struct LookupHit;
     struct TermEntry;
     struct DocumentEntry;
     struct CollectionJobStatus;
     struct DebugEvent
     {
          uint64_t Sequence = 0;
          std::string Collection;
          std::string Message;
     };

   private:

     struct PendingIndexJob
     {
          std::string Collection;
          Document Doc;
     };

     std::unique_ptr<rocksdb::DB> Database;
     rocksdb::Options OptionsValue;
     std::string DBPath;
     std::atomic<bool> DatabaseOpen{false};

     mutable std::mutex DBMutex;
     mutable std::mutex InferenceMutex;
     mutable std::mutex JobMutex;
     mutable std::mutex DebugMutex;
     mutable std::mutex QueueMutex;
     std::condition_variable QueueCV;
     std::map<std::string, CollectionJobStatus> CollectionJobs;
     mutable std::deque<DebugEvent> DebugEvents;
     mutable uint64_t NextDebugSequence = 1;
     std::deque<PendingIndexJob> PendingIndexJobs;
     std::unordered_set<std::string> PendingIndexKeys;
     bool ShuttingDown = false;
     std::vector<std::thread> WorkerThreads;

     /* Resolve the filesystem path used by the SAM database. */

     std::string ResolveDBPath() const;

     /* Build the unique key used to deduplicate pending index jobs. */

     static std::string BuildPendingIndexKey(const std::string& Collection, const std::string& DocumentID);

     /* Start the background workers that process queued index jobs. */

     void StartIndexWorker();

     /* Run the background loop that drains queued index jobs. */

     void RunIndexWorker();

     /* Remove all indexed SAM data from the database. */

     bool ClearAll(std::string* ErrorMessage = nullptr);

     /* Remove all existing term mappings for one document while holding the database lock. */

     bool RemoveExistingDocumentTermsLocked(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);

     /* Index one document into SAM while holding the database lock. */

     bool IndexDocumentLocked(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);

     /* Build the expanded term set for one document before persistence. */

     std::vector<TermEntry> ExpandDocumentTerms(const std::string& Collection,
                                                const Document& Doc,
                                                std::string* ErrorMessage = nullptr) const;

     /* Generate LLM-derived lookup terms for one document. */

     std::vector<TermEntry> GenerateLLMTerms(const std::string& Collection,
                                             const Document& Doc,
                                             std::string* ErrorMessage = nullptr) const;

     std::vector<TermEntry> GenerateLLMTermsFromProfile(const std::string& Collection,
                                                        const Document& Doc,
                                                        const std::vector<std::string>& ProfileTerms,
                                                        std::string* ErrorMessage = nullptr) const;

     /* Append a debug event to the in-memory SAM debug log. */

     void RecordDebugEvent(const std::string& Collection, const std::string& Message) const;

   public:

     struct LookupHit
     {
          struct ScoreBreakdown
          {
               double TermScore = 0.0;
               double SourceDocScore = 0.0;
               double EvidenceBonus = 0.0;
               double DocPrior = 0.0;
               double SourceDocBonus = 0.0;
               double FinalScore = 0.0;
          };

          std::string Collection;
          std::string DocumentID;
          std::string Title;
          std::string MatchedTerm;
          std::string MatchedKind;
          std::string MatchedSource;
          std::string MatchedPath;
          std::string TermOrigin;
          size_t EvidenceCount = 0;
          double MatchedScore = 0.0;
          double MatchedSignal = 0.0;
          ScoreBreakdown Breakdown;
          std::string Explain;
     };

     struct TermEntry
     {
          std::string Text;
          std::string Kind;
          std::string Source;
          double Score = 0.0;
          double Signal = 0.0;
     };

     struct DocumentEntry
     {
          std::string Collection;
          std::string DocumentID;
          std::string Title;
          std::vector<TermEntry> Terms;
     };

     struct CollectionJobStatus
     {
          bool Running = false;
          bool Completed = false;
          size_t IndexedDocuments = 0;
          size_t FailedDocuments = 0;
          size_t PendingDocuments = 0;
          size_t TotalDocuments = 0;
          std::string ErrorMessage;
     };

     /* Construct the SAM manager and initialize its runtime state. */

     SAM();

     /* Shut down the SAM manager and release owned resources. */

     ~SAM();

     /* Open the SAM database and prepare background workers. */

     bool Initialize();

     /* Stop background work and close the SAM database. */

     void Shutdown();

     /* Report whether the SAM database is currently open. */

     bool IsOpen() const;

     /* Return the resolved path of the SAM database directory. */

     const std::string& GetDBPath() const
     {
          return DBPath;
     }

     /* Rebuild the full SAM index from scratch. */

     bool Recreate(std::string* ErrorMessage = nullptr);

     /* Rebuild the SAM index for one collection. */

     bool RecreateCollection(const std::string& Collection,
                             size_t* IndexedDocuments = nullptr,
                             size_t* FailedDocuments = nullptr,
                             std::string* ErrorMessage = nullptr);

     /* Queue an asynchronous rebuild for one collection. */

     bool StartRecreateCollectionAsync(const std::string& Collection,
                                       bool* AlreadyRunning = nullptr,
                                       std::string* ErrorMessage = nullptr);

     /* Queue one document for background indexing. */

     bool EnqueueIndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);

     /* Index one document immediately into SAM. */

     bool IndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);

     /* Remove one document and its SAM terms from the database. */

     bool DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);

     /* Search SAM across all collections. */

     std::vector<LookupHit> Lookup(const std::string& Query, size_t Limit = 20) const;

     /* Search SAM within one collection. */

     std::vector<LookupHit> Lookup(const std::string& Collection, const std::string& Query, size_t Limit = 20) const;

     /* List indexed SAM documents for one collection. */

     std::vector<DocumentEntry> ListDocuments(const std::string& Collection, size_t Limit = 20, size_t Offset = 0) const;

     /* Retrieve the current background job status for one collection. */

     bool GetCollectionJobStatus(const std::string& Collection, CollectionJobStatus& Status) const;

     /* Return background job status for every tracked collection. */

     std::map<std::string, CollectionJobStatus> GetAllCollectionJobStatuses() const;

     /* Load one indexed SAM document entry. */

     bool GetDocumentEntry(const std::string& Collection,
                           const std::string& DocumentID,
                           DocumentEntry& Entry,
                           std::string* ErrorMessage = nullptr) const;

     /* Return collected SAM debug events, optionally filtered by collection and sequence. */

     std::vector<DebugEvent> GetDebugEvents(const std::string& Collection = "",
                                            uint64_t SinceSequence = 0,
                                            size_t Limit = 100) const;

     /* Return the latest emitted SAM debug sequence number. */

     uint64_t GetLatestDebugSequence() const;
};
