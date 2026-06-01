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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "core/hlquery.h"
#include "search/lindex.h"
#include "search/mindex.h"
#include "utils/wildcard.h"

/*
 * IndexWriter - Writes the in-memory index to disk layout.
 */

IndexWriter::IndexWriter(const std::string &IndexDirParam, const std::string &CollectionParam) : IndexDir(IndexDirParam), Collection(CollectionParam)
{
     /* Ensure the index directory exists before writing files. */

     std::filesystem::create_directories(IndexDir);
}

/*
 * IndexWriter::~IndexWriter - Default cleanup for the writer.
 */

IndexWriter::~IndexWriter() = default;

/*
 * WriteVarint - Writes a 32-bit value using varint encoding.
 */

void IndexWriter::WriteVarint(std::vector<uint8_t> &Buffer, uint32_t Value) const
{
     /* Varint encoding stores 7 bits per byte and uses MSB as a continuation bit. */

     while (Value >= 0x80)
     {
          Buffer.push_back(static_cast<uint8_t>(Value | 0x80));

          Value >>= 7;
     }

     Buffer.push_back(static_cast<uint8_t>(Value));
}

/*
 * WritePostings - Serializes postings as delta-encoded hashes with payloads.
 */

void IndexWriter::WritePostings(std::vector<uint8_t> &PostingsBuffer, const std::vector<Posting> &Postings) const
{
     if (Postings.empty())
     {
          return;
     }

     /* Persist document count so the reader can pre-size and stop correctly. */

     uint32_t DocCountValue = static_cast<uint32_t>(Postings.size());

     const uint8_t *DocCountBytes = reinterpret_cast<const uint8_t *>(&DocCountValue);

     PostingsBuffer.insert(PostingsBuffer.end(), DocCountBytes, DocCountBytes + sizeof(DocCountValue));

     /* Sort by document hash to improve delta compression effectiveness. */

     struct HashedPosting
     {
          uint32_t Hash;
          Posting Post;
     };

     std::vector<HashedPosting> SortedPostings;

     SortedPostings.reserve(Postings.size());

     std::hash<std::string> Hasher;

     for (const auto &Post : Postings)
     {
          HashedPosting Entry;

          Entry.Hash = static_cast<uint32_t>(Hasher(Post.DocumentID));
          Entry.Post = Post;

          SortedPostings.push_back(std::move(Entry));
     }

     std::sort(SortedPostings.begin(), SortedPostings.end(), [](const HashedPosting &a, const HashedPosting &b)
               {
                    if (a.Hash == b.Hash)
                    {
                         return a.Post.DocumentID < b.Post.DocumentID;
                    }

                    return a.Hash < b.Hash;
               });

     uint32_t PrevDocHash = 0;

     for (const auto &Entry : SortedPostings)
     {
          uint32_t DocHash = Entry.Hash;

          /* Delta encode hash values to reduce varint size. */

          uint32_t DeltaValue = (PrevDocHash == 0) ? DocHash : DocHash - PrevDocHash;

          WriteVarint(PostingsBuffer, DeltaValue);

          /* Write score as a 32-bit float. */

          float ScoreValue = static_cast<float>(Entry.Post.Score);

          PostingsBuffer.insert(PostingsBuffer.end(), reinterpret_cast<const uint8_t *>(&ScoreValue), reinterpret_cast<const uint8_t *>(&ScoreValue) + sizeof(ScoreValue));

          /* Store document ID length and bytes for retrieval. */

          uint16_t DocIDLen = static_cast<uint16_t>(Entry.Post.DocumentID.length());

          PostingsBuffer.insert(PostingsBuffer.end(), reinterpret_cast<const uint8_t *>(&DocIDLen), reinterpret_cast<const uint8_t *>(&DocIDLen) + sizeof(DocIDLen));

          PostingsBuffer.insert(PostingsBuffer.end(), Entry.Post.DocumentID.begin(), Entry.Post.DocumentID.end());

          PrevDocHash = DocHash;
     }
}

/*
 * WriteIndex - Writes the complete index files for a collection.
 */

bool IndexWriter::WriteIndex(const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>> &IndexData)
{
     try
     {
          /* Collect and sort all terms for the target collection. */

          std::vector<std::string> Terms;

          auto CollectionIt = IndexData.find(Collection);

          if (CollectionIt == IndexData.end())
          {
               Instance->Logs->Debug("mmap_index", "WriteIndex: Collection '" + Collection + "' not found in index.");

               return false;
          }

          for (const auto &[TermValue, Postings] : CollectionIt->second)
          {
               Terms.push_back(TermValue);
          }

          std::sort(Terms.begin(), Terms.end());

          /* Write sorted terms to terms.bin as null-terminated strings. */

          std::string TermsFile = IndexDir + "/" + Collection + "/terms.bin";

          std::filesystem::create_directories(std::filesystem::path(TermsFile).parent_path());

          std::ofstream TermsOut(TermsFile, std::ios::binary);

          if (!TermsOut)
          {
               Instance->Logs->Normal("mmap_index", "Failed to open terms.bin for writing.");

               return false;
          }

          for (const auto &TermValue : Terms)
          {
               TermsOut.write(TermValue.c_str(), TermValue.length());

               TermsOut.put('\0');
          }

          TermsOut.close();

          /* Write postings to postings.bin and track offsets per term. */

          std::string PostingsFile = IndexDir + "/" + Collection + "/postings.bin";

          std::ofstream PostingsOut(PostingsFile, std::ios::binary);

          if (!PostingsOut)
          {
               Instance->Logs->Normal("mmap_index", "Failed to open postings.bin for writing.");

               return false;
          }

          std::vector<uint8_t> PostingsBuffer;

          std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> TermOffsets;

          uint64_t CurrentOffset = 0;
          const uint32_t IndexIntervalConst = 16;

          const auto &CollectionTermMap = CollectionIt->second;

          for (const std::string &TermValue : Terms)
          {
               auto TermIt = CollectionTermMap.find(TermValue);

               if (TermIt == CollectionTermMap.end())
               {
                    continue;
               }

               const auto &Postings = TermIt->second;

               size_t PostingsStart = PostingsBuffer.size();

               WritePostings(PostingsBuffer, Postings);

               size_t PostingsLength = PostingsBuffer.size() - PostingsStart;

               TermOffsets[TermValue] = {CurrentOffset, static_cast<uint32_t>(PostingsLength)};

               CurrentOffset += PostingsLength;
          }

          PostingsOut.write(reinterpret_cast<const char *>(PostingsBuffer.data()), PostingsBuffer.size());

          PostingsOut.close();

          /* Write complete term->offset map for fast direct lookup. */

          std::string TermMapFile = IndexDir + "/" + Collection + "/term_map.bin";

          std::ofstream TermMapOut(TermMapFile, std::ios::binary);

          if (!TermMapOut)
          {
               Instance->Logs->Normal("mmap_index", "Failed to open term_map.bin for writing.");

               return false;
          }

          uint32_t TermCountValue = static_cast<uint32_t>(Terms.size());

          TermMapOut.write(reinterpret_cast<const char *>(&TermCountValue), sizeof(TermCountValue));

          for (const std::string &TermValue : Terms)
          {
               auto OffsetIt = TermOffsets.find(TermValue);

               if (OffsetIt == TermOffsets.end())
               {
                    continue;
               }

               uint16_t TermLenValue = static_cast<uint16_t>(TermValue.length());
               uint64_t PostingsOffsetValue = OffsetIt->second.first;
               uint32_t PostingsLengthValue = OffsetIt->second.second;

               TermMapOut.write(reinterpret_cast<const char *>(&TermLenValue), sizeof(TermLenValue));

               TermMapOut.write(TermValue.c_str(), TermLenValue);

               TermMapOut.write(reinterpret_cast<const char *>(&PostingsOffsetValue), sizeof(PostingsOffsetValue));

               TermMapOut.write(reinterpret_cast<const char *>(&PostingsLengthValue), sizeof(PostingsLengthValue));
          }

          TermMapOut.close();

          /* Precompute byte offsets within terms.bin for each term. */

          std::vector<uint64_t> TermOffsetsByIndex;

          TermOffsetsByIndex.reserve(Terms.size());

          uint64_t TermFileOffset = 0;

          for (const auto &TermValue : Terms)
          {
               TermOffsetsByIndex.push_back(TermFileOffset);
               TermFileOffset += TermValue.length() + 1;
          }

          /* Write sparse term index for prefix search acceleration. */

          std::string TermIndexFileValue = IndexDir + "/" + Collection + "/term_index.bin";

          std::ofstream TermIndexOut(TermIndexFileValue, std::ios::binary);

          if (!TermIndexOut)
          {
               Instance->Logs->Normal("mmap_index", "Failed to open term_index.bin for writing.");

               return false;
          }

          uint32_t TermCountTotal = static_cast<uint32_t>(Terms.size());

          TermIndexOut.write(reinterpret_cast<const char *>(&TermCountTotal), sizeof(TermCountTotal));

          TermIndexOut.write(reinterpret_cast<const char *>(&IndexIntervalConst), sizeof(IndexIntervalConst));

          for (size_t I = 0; I < Terms.size(); I += IndexIntervalConst)
          {
               const std::string &TermValue = Terms[I];
               uint64_t TermsFileOffsetValue = TermOffsetsByIndex[I];

               uint64_t PostingsOffsetValue = 0;
               uint32_t PostingsLengthValue = 0;

               auto OffsetIt = TermOffsets.find(TermValue);

               if (OffsetIt != TermOffsets.end())
               {
                    PostingsOffsetValue = OffsetIt->second.first;
                    PostingsLengthValue = OffsetIt->second.second;
               }

               TermIndexOut.write(reinterpret_cast<const char *>(&TermsFileOffsetValue), sizeof(TermsFileOffsetValue));

               uint32_t TermLenVal = static_cast<uint32_t>(TermValue.length());

               TermIndexOut.write(reinterpret_cast<const char *>(&TermLenVal), sizeof(TermLenVal));

               TermIndexOut.write(reinterpret_cast<const char *>(&PostingsOffsetValue), sizeof(PostingsOffsetValue));

               TermIndexOut.write(reinterpret_cast<const char *>(&PostingsLengthValue), sizeof(PostingsLengthValue));
          }

          TermIndexOut.close();

          Instance->Logs->Normal("mmap_index", "Wrote index for collection '" + Collection + "': " + std::to_string(Terms.size()) + " terms, " + std::to_string(PostingsBuffer.size()) + " bytes postings.");

          return true;
     }
     catch (const std::exception &e)
     {
          Instance->Logs->Normal("mmap_index", "Exception writing index: " + std::string(e.what()) + ".");

          return false;
     }
}

/*
 * MMapIndex::~MMapIndex - Ensures all mappings are released.
 */

MMapIndex::~MMapIndex()
{
     Unmap();
}

/*
 * Unmap - Unmaps all memory-mapped regions.
 */

void MMapIndex::Unmap()
{
     if (TermsMMap && TermsMMap != MAP_FAILED)
     {
          munmap(TermsMMap, TermsSize);

          TermsMMap = nullptr;
     }

     if (TermMapMMap && TermMapMMap != MAP_FAILED)
     {
          munmap(TermMapMMap, TermMapSize);

          TermMapMMap = nullptr;
     }

     if (TermIndexMMap && TermIndexMMap != MAP_FAILED)
     {
          munmap(TermIndexMMap, TermIndexSize);

          TermIndexMMap = nullptr;
     }

     if (PostingsMMap && PostingsMMap != MAP_FAILED)
     {
          munmap(PostingsMMap, PostingsSize);

          PostingsMMap = nullptr;
     }
}

/*
 * Open - Opens an existing mmap index from disk.
 */

std::unique_ptr<MMapIndex> MMapIndex::Open(const std::string &IndexDirParam, const std::string &CollectionParam)
{
     auto Idx = std::make_unique<MMapIndex>();

     if (Idx->LoadIndex(IndexDirParam, CollectionParam))
     {
          return Idx;
     }

     return nullptr;
}

/*
 * LoadIndex - Loads the index into memory using mmap.
 */

bool MMapIndex::LoadIndex(const std::string &IndexDirParam, const std::string &CollectionParam)
{
     try
     {
          /* Resolve file paths for the collection. */

          std::string CollectionDir = IndexDirParam + "/" + CollectionParam;
          std::string TermsFile = CollectionDir + "/terms.bin";
          std::string TermMapFile = CollectionDir + "/term_map.bin";
          std::string TermIndexFile = CollectionDir + "/term_index.bin";
          std::string PostingsFile = CollectionDir + "/postings.bin";

          bool HasTermMap = std::filesystem::exists(TermMapFile);

          if (!std::filesystem::exists(TermsFile) || !std::filesystem::exists(PostingsFile))
          {
               return false;
          }

          /* term_map.bin is required to resolve postings offsets for all terms. */

          if (!HasTermMap)
          {
               Instance->Logs->Normal("mmap_index", "Missing term_map.bin for collection '" + CollectionParam + "'.");

               return false;
          }

          bool HasTermIndex = std::filesystem::exists(TermIndexFile);

          int TermsFd = open(TermsFile.c_str(), O_RDONLY);

          if (TermsFd < 0)
          {
               return false;
          }

          TermsSize = std::filesystem::file_size(TermsFile);

          TermsMMap = mmap(nullptr, TermsSize, PROT_READ, MAP_PRIVATE, TermsFd, 0);

          close(TermsFd);

          if (TermsMMap == MAP_FAILED)
          {
               return false;
          }

          TermsData = reinterpret_cast<const uint8_t *>(TermsMMap);

          int TermMapFd = open(TermMapFile.c_str(), O_RDONLY);

          if (TermMapFd >= 0)
          {
               TermMapSize = std::filesystem::file_size(TermMapFile);

               TermMapMMap = mmap(nullptr, TermMapSize, PROT_READ, MAP_PRIVATE, TermMapFd, 0);

               close(TermMapFd);

               if (TermMapMMap != MAP_FAILED)
               {
                    TermMapData = reinterpret_cast<const uint8_t *>(TermMapMMap);

                    TermCount = *reinterpret_cast<const uint32_t *>(TermMapData);
               }
               else
               {
                    Unmap();

                    return false;
               }
          }
          else
          {
               Unmap();

               return false;
          }

          if (HasTermIndex)
          {
               int TermIndexFd = open(TermIndexFile.c_str(), O_RDONLY);

               if (TermIndexFd >= 0)
               {
                    TermIndexSize = std::filesystem::file_size(TermIndexFile);

                    TermIndexMMap = mmap(nullptr, TermIndexSize, PROT_READ, MAP_PRIVATE, TermIndexFd, 0);

                    close(TermIndexFd);

                    if (TermIndexMMap != MAP_FAILED)
                    {
                         TermIndexData = reinterpret_cast<const uint8_t *>(TermIndexMMap);

                         const uint32_t *Header = reinterpret_cast<const uint32_t *>(TermIndexData);

                         IndexInterval = Header[1];
                    }
               }
          }

          int PostingsFd = open(PostingsFile.c_str(), O_RDONLY);

          if (PostingsFd < 0)
          {
               Unmap();

               return false;
          }

          PostingsSize = std::filesystem::file_size(PostingsFile);

          PostingsMMap = mmap(nullptr, PostingsSize, PROT_READ, MAP_PRIVATE, PostingsFd, 0);

          close(PostingsFd);

          if (PostingsMMap == MAP_FAILED)
          {
               Unmap();

               return false;
          }

          PostingsData = reinterpret_cast<const uint8_t *>(PostingsMMap);

          Valid = true;

          Instance->Logs->Normal("mmap_index", "Loaded mmap index for collection '" + CollectionParam + "': " + std::to_string(TermCount) + " terms.");

          return true;
     }
     catch (const std::exception &e)
     {
          Instance->Logs->Normal("mmap_index", "Exception loading index: " + std::string(e.what()) + ".");

          Unmap();

          return false;
     }
}

/*
      * FindTerm - Finds a term entry in the memory-mapped structures.
      */

MMapIndex::TermEntry MMapIndex::FindTerm(const std::string &TermValue) const
{
     TermEntry Ent = {0, 0, 0};

     if (!Valid || TermValue.empty())
     {
          return Ent;
     }

     /* Use optimized binary search on the term map for lookups. */

     if (TermMapData && TermMapSize > sizeof(uint32_t))
     {
          return FindTermOptimized(TermValue);
     }

     return Ent;
}

/*
      * DecodePostings - Decodes postings from the memory-mapped buffer.
      */

std::vector<Posting> MMapIndex::DecodePostings(const uint8_t *DataParam, size_t LengthParam) const
{
     std::vector<Posting> PostingsList;

     if (!DataParam || LengthParam < sizeof(uint32_t))
     {
          return PostingsList;
     }

     uint32_t DocCountValue = *reinterpret_cast<const uint32_t *>(DataParam);

     const uint8_t *Ptr = DataParam + sizeof(uint32_t);
     const uint8_t *EndPtr = DataParam + LengthParam;

     uint32_t PrevDocHash = 0;

     for (uint32_t I = 0; I < DocCountValue && Ptr < EndPtr; ++I)
     {
          uint32_t DeltaValue = 0;
          uint32_t ShiftValue = 0;

          while (Ptr < EndPtr && (*Ptr & 0x80))
          {
               DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;

               ShiftValue += 7U;
               Ptr++;

               if (ShiftValue >= 32U)
               {
                    break;
               }
          }

          if (Ptr < EndPtr && ShiftValue < 32U)
          {
               DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;
               Ptr++;
          }

          uint32_t DocHash = (PrevDocHash == 0) ? DeltaValue : PrevDocHash + DeltaValue;

          PrevDocHash = DocHash;

          if (Ptr + sizeof(float) > EndPtr)
          {
               break;
          }

          float ScoreValue = *reinterpret_cast<const float *>(Ptr);

          Ptr += sizeof(float);

          if (Ptr + sizeof(uint16_t) > EndPtr)
          {
               break;
          }

          uint16_t DocIDLenVal = *reinterpret_cast<const uint16_t *>(Ptr);

          Ptr += sizeof(uint16_t);

          if (Ptr + DocIDLenVal > EndPtr)
          {
               break;
          }

          std::string DocIDStr(reinterpret_cast<const char *>(Ptr), DocIDLenVal);
          Ptr += DocIDLenVal;

          Posting Post;

          Post.DocumentID = DocIDStr;
          Post.Score = ScoreValue;

          PostingsList.push_back(Post);
     }

     return PostingsList;
}

/*
 * SearchTerm - Searches for a term and returns its postings list.
 */

std::vector<Posting> MMapIndex::SearchTerm(const std::string &TermValue) const
{
     if (!Valid)
     {
          return {};
     }

     TermEntry Ent = FindTerm(TermValue);

     if (Ent.PostingsLength == 0)
     {
          return {};
     }

     return DecodePostings(PostingsData + Ent.PostingsOffset, Ent.PostingsLength);
}

/*
 * SearchPrefix - Searches for terms with a given prefix.
 */

std::vector<Posting> MMapIndex::SearchPrefix(const std::string &PrefixValue, int LimitVal) const
{
     std::vector<Posting> ResultsList;

     if (!Valid || PrefixValue.empty())
     {
          return ResultsList;
     }

     const char *Ptr = reinterpret_cast<const char *>(TermsData);
     const char *End = reinterpret_cast<const char *>(TermsData + TermsSize);

     std::unordered_map<std::string, Posting> DocScores;

     while (Ptr < End)
     {
          std::string TermVal(Ptr);

          if (TermVal.length() >= PrefixValue.length() && TermVal.substr(0, PrefixValue.length()) == PrefixValue)
          {
               auto Postings = SearchTerm(TermVal);

               for (const auto &Post : Postings)
               {
                    auto It = DocScores.find(Post.DocumentID);

                    if (It == DocScores.end())
                    {
                         DocScores[Post.DocumentID] = Post;
                    }
                    else
                    {
                         It->second.Score += Post.Score;
                    }
               }
          }

          Ptr += TermVal.length() + 1;
     }

     for (const auto &[DocID, Post] : DocScores)
     {
          ResultsList.push_back(Post);
     }

     std::sort(ResultsList.begin(), ResultsList.end(), [](const Posting &a, const Posting &b)
               {
                    return a.Score > b.Score;
               });

     if (LimitVal > 0)
     {
          const std::size_t LimitSize = static_cast<std::size_t>(LimitVal);
          if (ResultsList.size() > LimitSize)
          {
               ResultsList.resize(LimitSize);
          }
     }

     return ResultsList;
}

/* MMapIndex::GetDocumentCount - Returns the document count. */

size_t MMapIndex::GetDocumentCount() const
{
     if (!Valid)
     {
          return 0;
     }

     std::lock_guard<std::mutex> Lock(DocumentCountMutex);

     if (DocumentCountCached)
     {
          return DocumentCount;
     }

     std::unordered_set<std::string> DocumentIDs;
     const char *Ptr = reinterpret_cast<const char *>(TermsData);
     const char *End = reinterpret_cast<const char *>(TermsData + TermsSize);

     while (Ptr < End)
     {
          std::string TermVal(Ptr);

          if (TermVal.empty())
          {
               break;
          }

          auto Postings = SearchTerm(TermVal);

          for (const auto &Post : Postings)
          {
               if (!Post.DocumentID.empty())
               {
                    DocumentIDs.insert(Post.DocumentID);
               }
          }

          Ptr += TermVal.length() + 1;
     }

     DocumentCount = DocumentIDs.size();
     DocumentCountCached = true;
     return DocumentCount;
}

/* MMapIndex::SearchWildcard - Searches terms that match a wildcard pattern. */

std::vector<Posting> MMapIndex::SearchWildcard(const std::string &PatternValue, int LimitVal) const
{
     std::vector<Posting> ResultsList;

     if (!Valid || PatternValue.empty())
     {
          return ResultsList;
     }

     const char *Ptr = reinterpret_cast<const char *>(TermsData);
     const char *End = reinterpret_cast<const char *>(TermsData + TermsSize);
     std::unordered_map<std::string, Posting> DocScores;

     while (Ptr < End)
     {
          std::string TermVal(Ptr);

          if (Wildcard::Match(TermVal, PatternValue))
          {
               auto Postings = SearchTerm(TermVal);

               for (const auto &Post : Postings)
               {
                    auto It = DocScores.find(Post.DocumentID);

                    if (It == DocScores.end())
                    {
                         DocScores[Post.DocumentID] = Post;
                    }
                    else
                    {
                         It->second.Score += Post.Score;
                    }
               }
          }

          Ptr += TermVal.length() + 1;
     }

     for (const auto &[DocID, Post] : DocScores)
     {
          ResultsList.push_back(Post);
     }

     std::sort(ResultsList.begin(), ResultsList.end(), [](const Posting &A, const Posting &B)
     {
          return A.Score > B.Score;
     });

     if (LimitVal > 0)
     {
          const std::size_t LimitSize = static_cast<std::size_t>(LimitVal);

          if (ResultsList.size() > LimitSize)
          {
               ResultsList.resize(LimitSize);
          }
     }

     return ResultsList;
}
