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
#include <memory>
#include <rocksdb/db.h>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/llm.h"
#include "search/cstore.h"

/*
 * Secondary Assistant Manager.
 * Maintains a second RocksDB database with heuristic/LLM-derived lookup phrases
 * that can be used to discover documents through broader natural-language intent.
 */

class SAM
{
   public:

     /* Forward declarations for the core SAM record types. */

     struct LookupHit;
     struct TermEntry;
     struct DocumentEntry;
     struct CollectionJobStatus;
     struct LexicalSyncInfo;

     /* One debug event emitted by SAM for in-memory inspection. */

     struct DebugEvent
     {
          uint64_t Sequence = 0;
          std::string Collection;
          std::string Message;
     };

     /* One document reference attached to a recorded search idea. */

     struct SearchIdeaDocumentRef
     {
          std::string DocumentID;
          std::string Title;
          double Score = 0.0;
          uint64_t InteractionUses = 0;
          uint64_t LastInteractionMS = 0;
     };

     /* One normalized search idea and its learned intent state. */

     struct SearchIdeaEntry
     {
          std::string Collection;
          std::string Query;
          std::string NormalizedQuery;
          uint64_t FirstSeenMS = 0;
          uint64_t LastSeenMS = 0;
          uint64_t Uses = 0;
          std::vector<float> Vector;
          std::vector<SearchIdeaDocumentRef> Documents;
          std::string ResolvedInterpretation;
          std::string ResolvedConclusion;
          uint64_t ResolvedAtMS = 0;
          uint64_t ResolvedUses = 0;
          uint64_t InteractionUses = 0;
          uint64_t LastInteractionMS = 0;
          std::vector<llm::SearchIntentCandidate> ResolvedCandidates;
          std::vector<llm::SearchIntentCandidate> ResolvedRankedTerms;
     };

     /* One active or recently completed SAM lookup activity snapshot. */

     struct SearchActivityEntry
     {
          uint64_t Sequence = 0;
          std::string Collection;
          std::string Query;
          std::string NormalizedQuery;
          uint64_t StartedMS = 0;
          uint64_t CompletedMS = 0;
          size_t ResultCount = 0;
          bool Running = false;
     };

     struct ImprovementStats
     {
          size_t ImprovedCollections = 0;
          size_t OptimizedIdeas = 0;
          size_t SkippedBusy = 0;
          size_t SkippedStaleIndex = 0;
          size_t SkippedPendingRebuild = 0;
          size_t SkippedCancelled = 0;
          size_t SkippedNotIndexed = 0;
          size_t SkippedLLMUnavailable = 0;
          size_t SkippedThrottled = 0;
          size_t SkippedPaused = 0;
          size_t SkippedFlushInProgress = 0;
          size_t SkippedNoDatabase = 0;

          size_t TotalImproved() const
          {
               return ImprovedCollections + OptimizedIdeas;
          }
     };

     /* Pause automatic background indexing until the given wall-clock timestamp in ms. */

     void SetAutoIndexPauseUntilMS(uint64_t UntilMS);

     /* Pause automatic work for a flush transaction without touching manual pauses. */

     bool BeginFlushPause(uint64_t UntilMS, std::string* ErrorMessage = nullptr);

     /* Release the flush-owned pause and allow automatic work to resume if no manual pause remains. */

     void EndFlushPause();

     /* Clear queued automatic indexing jobs without touching manual rebuild/search work. */

     size_t ClearQueuedAutoIndexJobs();

     /* Returns the current pause-until timestamp in ms (0 means not paused). */

     uint64_t GetAutoIndexPauseUntilMS() const;

     std::string GetAutoIndexPauseReason(uint64_t NowMS = 0) const;

     bool IsFlushInProgress() const;

   private:

     /* One queued background indexing job for a source document. */

     struct PendingIndexJob
     {
          std::string Collection;
          Document Doc;
          bool HasExpectedMutationVersion = false;
          uint64_t ExpectedMutationVersion = 0;
     };

     /* Coordinated pause for auto-index background work (does not block manual rebuild/search). */

     std::atomic<uint64_t> ManualAutoIndexPauseUntilMS {0};
     std::atomic<uint64_t> FlushAutoIndexPauseUntilMS {0};
     std::atomic<bool> FlushInProgress {false};

     struct PendingSearchIdeaJob
     {
          std::string Collection;
          std::string Query;
          std::vector<SearchIdeaDocumentRef> Documents;
          size_t Attempts = 0;
     };

     struct PendingSearchInteractionJob
     {
          std::string Collection;
          std::string Query;
          SearchIdeaDocumentRef Document;
          size_t Attempts = 0;
     };

     std::shared_ptr<rocksdb::DB> Database;
     rocksdb::Options OptionsValue;
     std::string DBPath;
     std::atomic<bool> DatabaseOpen{false};

     mutable std::mutex DBMutex;
     mutable std::mutex InferenceMutex;
     mutable std::mutex JobMutex;
     mutable std::mutex DebugMutex;
     mutable std::mutex SearchActivityMutex;
     mutable std::mutex QueueMutex;
     mutable std::mutex SearchIdeaQueueMutex;
     mutable std::mutex SearchInteractionQueueMutex;
     std::condition_variable QueueCV;
     std::condition_variable JobStateCV;
     std::map<std::string, CollectionJobStatus> CollectionJobs;
     mutable std::deque<DebugEvent> DebugEvents;
     mutable uint64_t NextDebugSequence = 1;
     mutable uint64_t NextSearchActivitySequence = 1;
     mutable std::map<uint64_t, SearchActivityEntry> ActiveSearchActivities;
     mutable std::unordered_map<std::string, SearchActivityEntry> LatestSearchActivityByCollection;
     std::deque<PendingIndexJob> PendingIndexJobs;
     std::unordered_set<std::string> PendingIndexKeys;
     std::deque<PendingSearchIdeaJob> PendingSearchIdeaJobs;
     std::deque<PendingSearchInteractionJob> PendingSearchInteractionJobs;
     size_t DroppedPendingSearchIdeaJobs = 0;
     size_t DroppedPendingSearchInteractionJobs = 0;
     std::unordered_map<std::string, size_t> ActiveCollectionTasks;
     std::unordered_map<std::string, uint64_t> LastBackgroundImprovementMS;
     std::unordered_set<std::string> CancelledCollections;
     bool CancelAllRequested = false;
     bool ShuttingDown = false;
     size_t BackgroundWorkerCount = 1;
     std::vector<std::thread> WorkerThreads;
     std::vector<std::thread> HelperThreads;

     /* Resolve the filesystem path used by the SAM database. */

     std::string ResolveDBPath() const;

     /* Build the unique key used to deduplicate pending index jobs. */

     static std::string BuildPendingIndexKey(const std::string& Collection, const std::string& DocumentID);

     /* Start the background workers that process queued index jobs. */

     void StartIndexWorker();

     /* Track short-lived helper threads so shutdown owns their lifetime. */

     void StartHelperThread(std::thread Thread);

     /* Return the desired number of concurrent SAM background workers. */

     size_t ResolveBackgroundWorkerCount() const;

     /* Run the background loop that drains queued index jobs. */

     void RunIndexWorker();

     /* Run low-priority SAM improvement work while indexing is idle. */

     void RunImprovementWorker();

     /* Try to reserve one idle collection for low-priority improvement. */

     bool TryBeginBackgroundImprovement(const std::string& Collection,
                                        uint64_t NowMS,
                                        bool Force = false,
                                        std::string* SkipReason = nullptr);

     /* Release a collection reserved by the low-priority improvement worker. */

     void FinishBackgroundImprovement(const std::string& Collection);

     /* Remove all indexed SAM data from the database. */

     bool ClearAll(std::string* ErrorMessage = nullptr);

     /* Remove all existing term mappings for one document while holding the database lock. */

     bool RemoveExistingDocumentTermsLocked(const std::string& Collection,
                                            const std::string& DocumentID,
                                            std::string* ErrorMessage = nullptr,
                                            bool ScanAllTermKeys = false);

     /* Index one document into SAM while holding the database lock. */

     bool IndexDocumentLocked(const std::string& Collection,
                              const Document& Doc,
                              std::string* ErrorMessage = nullptr,
                              bool HasExpectedMutationVersion = false,
                              uint64_t ExpectedMutationVersion = 0);

     /* Build the expanded term set for one document before persistence. */

     std::vector<TermEntry> ExpandDocumentTerms(const std::string& Collection,
                                                const Document& Doc,
                                                std::string* ErrorMessage = nullptr) const;

     /* Generate LLM-derived lookup terms for one document. */

     std::vector<TermEntry> GenerateLLMTerms(const std::string& Collection,
                                             const Document& Doc,
                                             std::string* ErrorMessage = nullptr) const;

     /* Generate LLM-derived lookup terms by using a learned profile term set. */

     std::vector<TermEntry> GenerateLLMTermsFromProfile(const std::string& Collection,
                                                        const Document& Doc,
                                                        const std::vector<std::string>& ProfileTerms,
                                                        std::string* ErrorMessage = nullptr) const;

     /* Append a debug event to the in-memory SAM debug log. */

     void RecordDebugEvent(const std::string& Collection, const std::string& Message) const;

     /* Return whether destructive cancellation is currently blocking this collection. */

     bool IsCollectionCancelledLocked(const std::string& Collection) const;

     /* Return the current source mutation version for one collection. */

     uint64_t GetCurrentCollectionMutationVersion(const std::string& Collection) const;

     /* Report whether a queued indexing job is stale against the current collection mutation version. */

     bool ValidateExpectedMutationVersion(const std::string& Collection,
                                          bool HasExpectedMutationVersion,
                                          uint64_t ExpectedMutationVersion,
                                          std::string* ErrorMessage = nullptr) const;

     /* Persist one observed search idea while holding the database lock. */

     bool RecordSearchIdeaLocked(const std::string& Collection,
                                 const std::string& Query,
                                 const std::vector<SearchIdeaDocumentRef>& Documents,
                                 std::string* ErrorMessage = nullptr);

     bool EnqueuePendingSearchIdea(const std::string& Collection,
                                   const std::string& Query,
                                   const std::vector<SearchIdeaDocumentRef>& Documents,
                                   std::string* ErrorMessage = nullptr);

     size_t FlushPendingSearchIdeas(size_t MaxJobs = 1);

     bool RecordSearchInteractionLocked(const std::string& Collection,
                                        const std::string& Query,
                                        const SearchIdeaDocumentRef& Document,
                                        std::string* ErrorMessage = nullptr);

     bool EnqueuePendingSearchInteraction(const std::string& Collection,
                                          const std::string& Query,
                                          const SearchIdeaDocumentRef& Document,
                                          std::string* ErrorMessage = nullptr);

     size_t FlushPendingSearchInteractions(size_t MaxJobs = 1);

     void ScheduleRetryRebuild(const std::string& Collection);

     /* Trim stored search ideas to the configured history budget. */

     bool TrimSearchIdeasLocked(const std::string& Collection,
                                std::string* ErrorMessage = nullptr);

     /* Refresh one stored search idea with optimized intent data. */

     bool OptimizeSearchIdeaIntentLocked(const std::string& Collection,
                                         const std::string& NormalizedQuery,
                                         bool* Updated = nullptr,
                                         std::string* ErrorMessage = nullptr);

     /* Begin tracking one lookup execution for status and introspection. */

     uint64_t BeginLookupActivity(const std::string& Collection,
                                  const std::string& Query) const;

     /* Mark one tracked lookup execution as completed. */

     void FinishLookupActivity(uint64_t Sequence,
                               size_t ResultCount) const;

   public:

     /* One ranked SAM lookup hit with its scoring explanation. */

     struct LookupHit
     {
          /* Component scores used to produce the final ranking value. */

          struct ScoreBreakdown
          {
               double TermScore = 0.0;
               double SourceDocScore = 0.0;
               double SemanticScore = 0.0;
               double SemanticVectorScore = 0.0;
               double EvidenceBonus = 0.0;
               double DocPrior = 0.0;
               double RankPriorScore = 0.0;
               double RankPriorMultiplier = 1.0;
               double SemanticBonus = 0.0;
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

     /* One lookup term extracted or synthesized for a document. */

     struct TermEntry
     {
          std::string Text;
          std::string Kind;
          std::string Source;
          double Score = 0.0;
          double Signal = 0.0;
     };

     /* One stored SAM document projection and its expanded terms. */

     struct DocumentEntry
     {
          std::string Collection;
          std::string DocumentID;
          std::string Title;
          uint64_t SourceTimestamp = 0;
          std::string SourceFingerprint;
          std::string Lang;
          std::string Label;
          std::string Format;
          std::string Subject;
          std::string Summary;
          std::vector<std::string> Aliases;
          std::vector<std::string> Descriptors;
          std::vector<std::string> Queries;
          std::vector<TermEntry> Terms;
     };

     /* One collection-level background indexing status snapshot. */

     struct CollectionJobStatus
     {
          bool Running = false;
          bool Completed = false;
          bool NeedsRetry = false;
          bool RetryScheduled = false;
          size_t IndexedDocuments = 0;
          size_t FailedDocuments = 0;
          size_t PendingDocuments = 0;
          size_t TotalDocuments = 0;
          std::string ErrorMessage;
          std::string Source;
     };

     struct LexicalSyncInfo
     {
          bool CollectionSynonymsSynced = false;
          bool GlobalSynonymsSynced = false;
          bool CollectionStopwordsSynced = false;
          bool GlobalStopwordsSynced = false;
          size_t CollectionSynonymGroups = 0;
          size_t GlobalSynonymGroups = 0;
          size_t CollectionStopwords = 0;
          size_t GlobalStopwords = 0;
          uint64_t CollectionSynonymsSyncedAtMS = 0;
          uint64_t GlobalSynonymsSyncedAtMS = 0;
          uint64_t CollectionStopwordsSyncedAtMS = 0;
          uint64_t GlobalStopwordsSyncedAtMS = 0;
     };

     /* Construct the SAM manager and initialize its runtime state. */

     SAM();

     /* Shut down the SAM manager and release owned resources. */

     ~SAM();

     /* Open the SAM database and prepare background workers. */

     bool Initialize();

     /* Stop background work and close the SAM database. */

     void Shutdown();

     /* Flush SAM RocksDB memtables and sync the WAL to disk. */

     bool FlushAndSync(std::string* ErrorMessage = nullptr);

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
                                       std::string* ErrorMessage = nullptr,
                                       const std::string& Source = "");

     /* Mark a source collection as changed so automatic SAM can rebuild it later. */

     bool NotifyCollectionChanged(const std::string& Collection,
                                  uint64_t MutationVersion = 0,
                                  std::string* ErrorMessage = nullptr);

     /* Queue one document for background indexing. */

     bool EnqueueIndexDocument(const std::string& Collection,
                               const Document& Doc,
                               std::string* ErrorMessage = nullptr,
                               bool HasExpectedMutationVersion = false,
                               uint64_t ExpectedMutationVersion = 0);

     /* Index one document immediately into SAM. */

     bool IndexDocument(const std::string& Collection,
                        const Document& Doc,
                        std::string* ErrorMessage = nullptr,
                        bool HasExpectedMutationVersion = false,
                        uint64_t ExpectedMutationVersion = 0);

     /* Remove one document and its SAM terms from the database. */

     bool DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);

     /* Remove one collection and all of its SAM state from the database. */

     bool DeleteCollection(const std::string& Collection, std::string* ErrorMessage = nullptr);

     /* Record one user query and the matching documents that satisfied it. */

     bool RecordSearchIdea(const std::string& Collection,
                           const std::string& Query,
                           const std::vector<SearchIdeaDocumentRef>& Documents,
                           std::string* ErrorMessage = nullptr);

     /* Record one explicit user interaction against a search idea and document. */

     bool RecordSearchInteraction(const std::string& Collection,
                                  const std::string& Query,
                                  const SearchIdeaDocumentRef& Document,
                                  std::string* ErrorMessage = nullptr);

     /* Recompute one collection profile from accumulated search ideas. */

     bool RefreshCollectionProfileFromSearchIdeas(const std::string& Collection,
                                                  bool* Updated = nullptr,
                                                  std::string* ErrorMessage = nullptr);

     /* Process a bounded batch of pending search-intent optimization work. */

     size_t ProcessPendingSearchIntentOptimizations(size_t MaxCollections = 1);

     /* Run a low-priority improvement pass for idle/current SAM collections. */

     size_t ImproveIdleCollections(size_t MaxCollections = 0, bool Force = false);

     ImprovementStats ImproveIdleCollectionsDetailed(size_t MaxCollections = 0, bool Force = false);

     /* Flush queued interaction-backed search-idea updates. */

     size_t FlushPendingInteractionSignals(size_t MaxJobs = 1);

     /* Cancel all queued and in-flight SAM work for one collection and wait for quiescence. */

     bool CancelCollectionWork(const std::string& Collection, std::string* ErrorMessage = nullptr);

     /* Cancel all queued and in-flight SAM work globally and wait for quiescence. */

     bool CancelAllWork(std::string* ErrorMessage = nullptr);

     /* Search SAM across all collections. */

     std::vector<LookupHit> Lookup(const std::string& Query, size_t Limit = 20) const;

     /* Search SAM within one collection. */

     std::vector<LookupHit> Lookup(const std::string& Collection, const std::string& Query, size_t Limit = 20) const;

     /* Return recorded search-idea history, optionally filtered by collection. */

     std::vector<SearchIdeaEntry> GetSearchIdeaHistory(const std::string& Collection = "",
                                                       size_t Limit = 100) const;

     /* Return active or latest visible lookup activities. */

     std::vector<SearchActivityEntry> GetActiveSearchActivities(const std::string& Collection = "") const;

     /* Load the latest lookup activity snapshot for one collection. */

     bool GetLatestSearchActivity(const std::string& Collection,
                                  SearchActivityEntry& Entry) const;

     /* List indexed SAM documents for one collection. */

     std::vector<DocumentEntry> ListDocuments(const std::string& Collection, size_t Limit = 20, size_t Offset = 0) const;

     /* Retrieve the current background job status for one collection. */

     bool GetCollectionJobStatus(const std::string& Collection, CollectionJobStatus& Status) const;

     /* Return background job status for every tracked collection. */

     std::map<std::string, CollectionJobStatus> GetAllCollectionJobStatuses() const;

     size_t GetBackgroundWorkerCount() const;

     size_t GetRunningCollectionJobCount() const;

     /* Return the last source collection mutation version captured by a completed SAM rebuild. */

     bool GetCollectionIndexedMutationVersion(const std::string& Collection, uint64_t& Version) const;

     bool HasPendingCollectionRebuild(const std::string& Collection,
                                      uint64_t* RequestedVersion = nullptr) const;

     /* Mirror one collection or global lexical resource set into the SAM database. */

     bool SyncLexicalResources(const std::string& Collection,
                               bool* Updated = nullptr,
                               std::string* ErrorMessage = nullptr);

     /* Return lexical sync metadata for one collection plus the global scope. */

     bool GetLexicalSyncInfo(const std::string& Collection,
                             LexicalSyncInfo& Info,
                             std::string* ErrorMessage = nullptr) const;

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

     /* Return the number of search idea jobs dropped by queue pressure. */

     size_t GetDroppedPendingSearchIdeaJobs() const;

     /* Return the number of search interaction jobs dropped by queue pressure. */

     size_t GetDroppedPendingSearchInteractionJobs() const;
};
