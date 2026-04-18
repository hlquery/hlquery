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

#include <mutex>
#include <map>
#include <rocksdb/db.h>
#include <deque>
#include <string>
#include <thread>
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

     struct LookupHit
     {
          std::string Collection;
          std::string DocumentID;
          std::string Title;
          std::string MatchedTerm;
          std::string MatchedKind;
          std::string MatchedSource;
          double MatchedScore = 0.0;
          double MatchedSignal = 0.0;
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
          std::string ErrorMessage;
     };

     struct DebugEvent
     {
          uint64_t Sequence = 0;
          std::string Collection;
          std::string Message;
     };

     SAM();
     ~SAM();

     bool Initialize();
     void Shutdown();

     bool IsOpen() const;
     const std::string& GetDBPath() const
     {
          return DBPath;
     }

     bool Recreate(std::string* ErrorMessage = nullptr);
     bool RecreateCollection(const std::string& Collection,
                            size_t* IndexedDocuments = nullptr,
                            size_t* FailedDocuments = nullptr,
                            std::string* ErrorMessage = nullptr);
     bool StartRecreateCollectionAsync(const std::string& Collection,
                                       bool* AlreadyRunning = nullptr,
                                       std::string* ErrorMessage = nullptr);
     bool IndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);
     bool DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);
     std::vector<LookupHit> Lookup(const std::string& Query, size_t Limit = 20) const;
     std::vector<LookupHit> Lookup(const std::string& Collection, const std::string& Query, size_t Limit = 20) const;
     std::vector<DocumentEntry> ListDocuments(const std::string& Collection, size_t Limit = 20, size_t Offset = 0) const;
     bool GetCollectionJobStatus(const std::string& Collection, CollectionJobStatus& Status) const;
     std::map<std::string, CollectionJobStatus> GetAllCollectionJobStatuses() const;
     bool GetDocumentEntry(const std::string& Collection,
                           const std::string& DocumentID,
                           DocumentEntry& Entry,
                           std::string* ErrorMessage = nullptr) const;
     std::vector<DebugEvent> GetDebugEvents(const std::string& Collection = "",
                                            uint64_t SinceSequence = 0,
                                            size_t Limit = 100) const;
     uint64_t GetLatestDebugSequence() const;

   private:

     std::unique_ptr<rocksdb::DB> Database;
     rocksdb::Options OptionsValue;
     std::string DBPath;

     mutable std::mutex DBMutex;
     mutable std::mutex InferenceMutex;
     mutable std::mutex JobMutex;
     mutable std::mutex DebugMutex;
     std::map<std::string, CollectionJobStatus> CollectionJobs;
     mutable std::deque<DebugEvent> DebugEvents;
     mutable uint64_t NextDebugSequence = 1;
     std::vector<std::thread> WorkerThreads;

     std::string ResolveDBPath() const;
     bool ClearAll(std::string* ErrorMessage = nullptr);
     bool RemoveExistingDocumentTermsLocked(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);
     bool IndexDocumentLocked(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);

     std::vector<TermEntry> ExpandDocumentTerms(const std::string& Collection, const Document& Doc) const;
     std::vector<TermEntry> GenerateLLMTerms(const std::string& Collection, const Document& Doc) const;
     std::vector<TermEntry> GenerateHeuristicTerms(const std::string& Collection, const Document& Doc) const;
     void RecordDebugEvent(const std::string& Collection, const std::string& Message) const;
};
