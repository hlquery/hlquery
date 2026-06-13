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

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "search/cstore.h"

class SAM
{
   public:

     struct TermEntry
     {
          std::string Text;
          std::string Kind;
          std::string Source;
          double Score = 0.0;
          double Signal = 0.0;
     };

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

     struct LookupHit
     {
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

     struct SearchIdeaDocumentRef
     {
          std::string DocumentID;
          std::string Title;
          double Score = 0.0;
          size_t InteractionUses = 0;
          uint64_t LastInteractionMS = 0;
     };

     struct WeightedTerm
     {
          std::string Text;
          double Weight = 0.0;
     };

     struct SearchIdeaEntry
     {
          std::string Collection;
          std::string Query;
          std::string NormalizedQuery;
          uint64_t FirstSeenMS = 0;
          std::vector<SearchIdeaDocumentRef> Documents;
          size_t Uses = 0;
          size_t InteractionUses = 0;
          uint64_t LastInteractionMS = 0;
          uint64_t LastSeenMS = 0;
          std::string ResolvedInterpretation;
          std::string ResolvedConclusion;
          uint64_t ResolvedAtMS = 0;
          size_t ResolvedUses = 0;
          std::vector<WeightedTerm> ResolvedCandidates;
          std::vector<WeightedTerm> ResolvedRankedTerms;
     };

     struct CollectionJobStatus
     {
          bool Running = false;
          bool Completed = false;
          bool NeedsRetry = false;
          bool RetryScheduled = false;
          size_t PendingDocuments = 0;
          size_t IndexedDocuments = 0;
          size_t FailedDocuments = 0;
          size_t TotalDocuments = 0;
          std::string ErrorMessage;
          std::string Source;
     };

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

     struct LexicalSyncInfo
     {
          bool Running = false;
          uint64_t StartedMS = 0;
          uint64_t CompletedMS = 0;
          size_t SynonymGroups = 0;
          size_t Stopwords = 0;
          bool CollectionSynonymsSynced = false;
          size_t CollectionSynonymGroups = 0;
          uint64_t CollectionSynonymsSyncedAtMS = 0;
          bool GlobalSynonymsSynced = false;
          size_t GlobalSynonymGroups = 0;
          uint64_t GlobalSynonymsSyncedAtMS = 0;
          bool CollectionStopwordsSynced = false;
          size_t CollectionStopwords = 0;
          uint64_t CollectionStopwordsSyncedAtMS = 0;
          bool GlobalStopwordsSynced = false;
          size_t GlobalStopwords = 0;
          uint64_t GlobalStopwordsSyncedAtMS = 0;
          std::string ErrorMessage;
     };

     struct ImprovementStats
     {
          std::vector<std::string> ImprovedCollections;
          size_t QueuedContextAudits = 0;
          size_t LearnedSynonymGroups = 0;
          size_t LearnedStopwords = 0;
          size_t OptimizedIdeas = 0;
          size_t PrunedIdeas = 0;
          size_t PrunedTerms = 0;
          size_t SkippedBusy = 0;
          size_t SkippedStaleIndex = 0;
          size_t SkippedPendingRebuild = 0;
          size_t SkippedCancelled = 0;
          size_t SkippedNotIndexed = 0;
          size_t SkippedUnavailable = 0;
          size_t SkippedThrottled = 0;
          size_t SkippedPaused = 0;
          size_t SkippedFlushInProgress = 0;
          size_t SkippedNoDatabase = 0;

          size_t TotalImproved() const
          {
               return ImprovedCollections.size();
          }
     };

     struct DebugEvent
     {
          uint64_t Sequence = 0;
          std::string Collection;
          std::string Message;
     };

     struct DocumentEntry
     {
          std::string Collection;
          std::string DocumentID;
          std::string Title;
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

     bool Initialize() { return false; }
     void Shutdown() {}
     bool IsOpen() const { return false; }

     bool NotifyCollectionChanged(const std::string&, uint64_t = 0, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool CancelCollectionWork(const std::string&, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool DeleteCollection(const std::string&, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool DeleteDocument(const std::string&, const std::string&, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool CancelAllWork(std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool BeginFlushPause(uint64_t, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     void EndFlushPause() {}
     size_t ClearQueuedAutoIndexJobs() { return 0; }
     bool Recreate(std::string* Error = nullptr) { if (Error) { *Error = "SAM is disabled"; } return false; }
     bool FlushAndSync(std::string* Error = nullptr) { if (Error) { *Error = "SAM is disabled"; } return false; }

     bool StartRecreateCollectionAsync(const std::string&, bool* AlreadyRunning = nullptr,
                                       std::string* Error = nullptr,
                                       const std::string& = "")
     {
          if (AlreadyRunning) { *AlreadyRunning = false; }
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool SyncLexicalResources(const std::string&, bool* Updated = nullptr, std::string* Error = nullptr)
     {
          if (Updated) { *Updated = false; }
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     bool GetCollectionJobStatus(const std::string&, CollectionJobStatus&) const { return false; }
     std::map<std::string, CollectionJobStatus> GetAllCollectionJobStatuses() const { return {}; }
     size_t GetRunningCollectionJobCount() const { return 0; }
     bool HasPendingCollectionRebuild(const std::string&) const { return false; }
     std::vector<SearchActivityEntry> GetActiveSearchActivities(const std::string& = "") const { return {}; }
     bool GetLatestSearchActivity(const std::string&, SearchActivityEntry&) const { return false; }
     bool GetLexicalSyncInfo(const std::string&, LexicalSyncInfo&) const { return false; }
     size_t GetDroppedPendingSearchIdeaJobs() const { return 0; }
     size_t GetDroppedPendingSearchInteractionJobs() const { return 0; }
     uint64_t GetLatestDebugSequence() const { return 0; }
     std::vector<DebugEvent> GetDebugEvents(const std::string&, uint64_t = 0, size_t = 100) const { return {}; }
     std::vector<DocumentEntry> ListDocuments(const std::string&, size_t = 20, size_t = 0) const { return {}; }
     bool GetDocumentEntry(const std::string&, const std::string&, DocumentEntry&, std::string* Error = nullptr) const
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     std::vector<SearchIdeaEntry> GetSearchIdeaHistory(const std::string&, size_t = 100) const { return {}; }
     ImprovementStats ImproveIdleCollectionsDetailed(size_t = 0, bool = false) { return {}; }
     bool LoadDocumentContext(const std::string&, const std::string&, nlohmann::json&) const { return false; }
     bool StoreDocumentContext(const std::string&, const std::string&, const nlohmann::json&) { return false; }
     bool RemoveDocumentContext(const std::string&, const std::string&) { return false; }
     bool LoadCollectionPromptProfile(const std::string&, nlohmann::json&) const { return false; }
     bool StoreCollectionPromptSummary(const std::string&, const nlohmann::json&) { return false; }
     bool EnqueueIndexDocument(const std::string&, const Document&, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     void AddDebugEvent(const std::string&, const std::string&) {}
     std::vector<LookupHit> Lookup(const std::string&, const std::string&, size_t = 20, bool = false) const { return {}; }
     bool RecordSearchIdea(const std::string&, const std::string&, const std::vector<SearchIdeaDocumentRef>& = {}, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }
     bool RecordSearchInteraction(const std::string&, const std::string&, const SearchIdeaDocumentRef&, std::string* Error = nullptr)
     {
          if (Error) { *Error = "SAM is disabled"; }
          return false;
     }

     void SetAutoIndexPauseUntilMS(uint64_t) {}
     uint64_t GetAutoIndexPauseUntilMS() const { return 0; }
     std::string GetAutoIndexPauseReason(uint64_t = 0) const { return ""; }
     bool IsFlushInProgress() const { return false; }

     std::vector<TermEntry> GenerateContextTermsFromProfile(const std::string&, const Document&, const std::vector<std::string>&, std::string* = nullptr) const;
     std::vector<TermEntry> GenerateContextTerms(const std::string&, const Document&, std::string* = nullptr) const;
     std::vector<TermEntry> ExpandDocumentTerms(const std::string&, const Document&, std::string* = nullptr) const;
};
