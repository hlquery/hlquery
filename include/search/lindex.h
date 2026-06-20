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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "search/mindex.h"

/* Forward declarations */

struct Document;

/*
 * Posting - Represents a document occurrence in a posting list.
 */

struct Posting
{
     std::string DocumentID;

     std::string Collection;

     double Score = 1.0;

     std::vector<size_t> Positions;

     bool operator<(const Posting& other) const
     {
          if (Score != other.Score)
          {
               return Score > other.Score;
          }

          return DocumentID < other.DocumentID;
     }
};

/*
 * InvertedIndex - RocksDB-based search index.
 * Maintains term -> document mappings for fast search.
 */

class InvertedIndex
{
   private:

     InvertedIndex(const InvertedIndex&) = delete;

     InvertedIndex& operator=(const InvertedIndex&) = delete;

     /* ExtractTerms splits input text into normalized terms. */

     std::vector<std::string> ExtractTerms(const std::string& Text);

     /* NormalizeTerm normalizes a term for indexing. */

     std::string NormalizeTerm(const std::string& Term);

     /* Index maps collection -> term -> postings list. */

     std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>> Index;

     /* DocumentTerms maps collection -> document_id -> term set. */

     std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_set<std::string>>> DocumentTerms;

     /* DocumentLengths stores document lengths for scoring. */

     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> DocumentLengths;

     /* AvgDocLengths stores average document length per collection. */

     std::unordered_map<std::string, double> AvgDocLengths;

     /* DocCounts stores document count per collection. */

     std::unordered_map<std::string, size_t> DocCounts;

     /* MMapIndexes stores mmap index handles by collection. */

     std::unordered_map<std::string, std::unique_ptr<MMapIndex>> MMapIndexes;

     /* DirtyCollections tracks collections with unflushed in-memory mutations. */

     std::unordered_set<std::string> DirtyCollections;

     /* CollectionLastMutation tracks the last in-memory mutation time per collection. */

     std::unordered_map<std::string, std::chrono::steady_clock::time_point> CollectionLastMutation;

     /* CollectionLastFlush tracks the last successful flush time per collection. */

     std::unordered_map<std::string, std::chrono::steady_clock::time_point> CollectionLastFlush;

     /* IndexMutex guards index state. */

     mutable std::mutex IndexMutex;

     /* MarkCollectionDirtyLocked records a collection mutation while IndexMutex is held. */

     void MarkCollectionDirtyLocked(const std::string& Collection);

     /* SelectFlushCollectionsLocked chooses dirty collections old enough to flush. */

     std::vector<std::string> SelectFlushCollectionsLocked(uint64_t MinDirtyAgeSeconds, size_t MaxCollections) const;

     /* FlushCollectionToDiskLocked writes one collection while IndexMutex is held. */

     bool FlushCollectionToDiskLocked(const std::string& IndexDir, const std::string& Collection);

     /* RemoveDocumentFromIndex removes a document from term postings. */

     void RemoveDocumentFromIndex(const std::string& Collection, const std::string& DocID);

     /* CalculateBM25PlusScore computes BM25+ score. */

     double CalculateBM25PlusScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1 = 1.2, double B = 0.75, double Delta = 1.0) const;

     /* CalculateBM25LScore computes BM25L score. */

     double CalculateBM25LScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1 = 1.2, double B = 0.75, double Delta = 0.5) const;

     /* CalculateTFIDFScore computes TF-IDF with optional length normalization. */

     double CalculateTFIDFScore(double TermFreq, double DocFreq, double DocLength, double CollectionSize, double IdfSmooth = 1.0, bool Normalize = true) const;

     /* CalculatePivotNormScore computes pivoted normalization score. */

     double CalculatePivotNormScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double Pivot = 0.25) const;

     /* CalculateTermFrequency computes a term frequency for a doc. */

     size_t CalculateTermFrequency(const std::string& Collection, const std::string& DocID, const std::string& Term) const;

     /* CalculateDocumentFrequency computes document frequency. */

     size_t CalculateDocumentFrequency(const std::string& Collection, const std::string& Term) const;

     /* UpdateCollectionStatistics refreshes collection stats. */

     void UpdateCollectionStatistics(const std::string& Collection);

     /* CalculateProximityBoost computes proximity boost for query terms. */

     double CalculateProximityBoost(const std::vector<std::string>& QueryTerms, const std::vector<Posting>& Postings, const std::string& DocID) const;

   public:

     /* AddDocument indexes a document. */

     bool AddDocument(const std::string& Collection, const Document& Doc);

     /* DeleteDocument removes a document from the index. */

     bool DeleteDocument(const std::string& Collection, const std::string& DocID);

     /* UpdateDocument updates index entries for a document. */

     bool UpdateDocument(const std::string& Collection, const Document& OldDoc, const Document& NewDoc);

     /* Search performs a search query over a collection. */

     std::vector<Posting> Search(const std::string& Collection, const std::string& Query, int Limit = 100, const std::vector<std::string>& QueryFields = {});

     /* SearchTerm searches for a single term. */

     std::vector<Posting> SearchTerm(const std::string& Collection, const std::string& Term);

     /* SearchPrefix searches for a prefix. */

     std::vector<Posting> SearchPrefix(const std::string& Collection, const std::string& Prefix, int Limit = 100);

     /* DeleteCollection removes all index data for a collection. */

     void DeleteCollection(const std::string& Collection);

     /* Clear clears all index data. */

     void Clear();

     /* InvalidateDocumentCache removes cached doc data. */

     void InvalidateDocumentCache(const std::string& Collection, const std::string& DocID);

     /* InvalidateCollectionCache removes cached collection data. */

     void InvalidateCollectionCache(const std::string& Collection);

     /* GetTermCount returns number of terms in a collection. */

     size_t GetTermCount(const std::string& Collection) const;

    /* GetDocumentCount returns number of documents in a collection. */

    size_t GetDocumentCount(const std::string& Collection) const;

    /* FlushToDisk writes dirty mmap index data to disk. */

    size_t FlushToDisk(const std::string& IndexDir, uint64_t MinDirtyAgeSeconds = 0, size_t MaxCollections = 0);

    /* Returns true if the in-memory index contains documents for the collection. */

    bool HasInMemoryIndex(const std::string& Collection) const;

    /* LoadFromDisk loads mmap index data from disk. */

    void LoadFromDisk(const std::string& IndexDir);

     /* HasMMapIndex checks whether an mmap index exists. */

     bool HasMMapIndex(const std::string& Collection) const;

     /* GetIndexDir returns the current index directory. */

     std::string GetIndexDir() const;

     /* Constructor. */

     InvertedIndex() = default;

     /* Destructor. */

     ~InvertedIndex() = default;
};
