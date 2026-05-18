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

#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct Posting;

/*
      * MMapIndex - Memory-mapped inverted index.
      */

class MMapIndex
{
   private:

     /* TermEntry stores a term postings entry. */

     struct TermEntry
     {
          uint64_t PostingsOffset;
          uint32_t PostingsLength;
          uint32_t DocFreq;
     };

     /* LoadIndex loads mmap index data from disk. */

     bool LoadIndex(const std::string& IndexDir, const std::string& Collection);

     /* Unmap releases mmap regions and resets state. */

     void Unmap();

     /* FindTerm locates a term entry in the term map. */

     TermEntry FindTerm(const std::string& Term) const;

     /* DecodePostings decodes postings from the mmap buffer. */

     std::vector<Posting> DecodePostings(const uint8_t* Data, size_t Length) const;

     /* FindTermOptimized locates a term entry using optimized lookup. */

     TermEntry FindTermOptimized(const std::string& Term) const;

     /* SearchPrefixOptimized searches prefix terms using optimized lookup. */

     std::vector<Posting> SearchPrefixOptimized(const std::string& Prefix, int Limit = 100) const;

     /* DecodePostingsOptimized decodes postings using optimized layout. */

     std::vector<Posting> DecodePostingsOptimized(const uint8_t* Data, size_t Length) const;

     /* TermsMMap stores the memory-mapped terms buffer. */

     void* TermsMMap = nullptr;

     /* TermsSize stores the terms mmap size. */

     size_t TermsSize = 0;

     /* TermMapMMap stores the term map mmap buffer. */

     void* TermMapMMap = nullptr;

     /* TermMapSize stores the term map mmap size. */

     size_t TermMapSize = 0;

     /* TermIndexMMap stores the term index mmap buffer. */

     void* TermIndexMMap = nullptr;

     /* TermIndexSize stores the term index mmap size. */

     size_t TermIndexSize = 0;

     /* PostingsMMap stores the postings mmap buffer. */

     void* PostingsMMap = nullptr;

     /* PostingsSize stores the postings mmap size. */

     size_t PostingsSize = 0;

     /* TermCount stores the number of terms in the index. */

     uint32_t TermCount = 0;

     /* Cached unique document count for the mmap index. */

     mutable bool DocumentCountCached = false;

     mutable size_t DocumentCount = 0;

     mutable std::mutex DocumentCountMutex;

     /* IndexInterval stores the term index interval. */

     uint32_t IndexInterval = 16;

     /* TermsData points to the terms buffer. */

     const uint8_t* TermsData = nullptr;

     /* TermMapData points to the term map buffer. */

     const uint8_t* TermMapData = nullptr;

     /* TermIndexData points to the term index buffer. */

     const uint8_t* TermIndexData = nullptr;

     /* PostingsData points to the postings buffer. */

     const uint8_t* PostingsData = nullptr;

     /* Valid indicates whether the index is usable. */

     bool Valid = false;

   public:

     /* Open opens an existing index from disk (mmap). */

     static std::unique_ptr<MMapIndex> Open(const std::string& IndexDir, const std::string& Collection);

     /* SearchTerm searches for a term and returns its postings list. */

     std::vector<Posting> SearchTerm(const std::string& Term) const;

     /* SearchPrefix searches for a prefix and returns matching postings. */

     std::vector<Posting> SearchPrefix(const std::string& Prefix, int Limit = 100) const;

     /* SearchWildcard searches for a wildcard pattern and returns matching postings. */

     std::vector<Posting> SearchWildcard(const std::string& Pattern, int Limit = 100) const;

     /* GetTermCount returns the number of indexed terms. */

     size_t GetTermCount() const
     {
          return TermCount;
     }

     /* GetDocumentCount returns the number of unique indexed documents. */

     size_t GetDocumentCount() const;

     /* IsValid reports whether the index is valid. */

     bool IsValid() const
     {
          return Valid;
     }

     /* Destructor. */

     ~MMapIndex();

     /* Constructor. */

     MMapIndex() = default;
};

/*
      * IndexWriter - Write in-memory index to disk format.
      */

class IndexWriter
{
   private:

     /* IndexDir stores the index output directory. */

     std::string IndexDir;

     /* Collection stores the collection name for output. */

     std::string Collection;

     /* WriteVarint writes a varint-encoded value. */

     void WriteVarint(std::vector<uint8_t>& Buffer, uint32_t Value) const;

     /* WritePostings writes delta-encoded postings data. */

     void WritePostings(std::vector<uint8_t>& PostingsBuffer, const std::vector<Posting>& Postings) const;

   public:

     /* Constructor. */

     IndexWriter(const std::string& IndexDir, const std::string& Collection);

     /* Destructor. */

     ~IndexWriter();

     /* WriteIndex writes the in-memory index to disk. */

     bool WriteIndex(const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>>& Index);
};
