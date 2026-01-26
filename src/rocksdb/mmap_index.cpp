/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <unordered_map>

#include "rocksdb/mmap_index.h"
#include "rocksdb/inverted_index.h"
#include "core/hlquery.h"

namespace hlquery_storage
{
     /*
      * IndexWriter - Write in-memory index to disk format.
      */

     IndexWriter::IndexWriter(const std::string& IndexDirParam, const std::string& CollectionParam) : IndexDir(IndexDirParam), Collection(CollectionParam)
     {
          /* Create directory if it doesn't exist. */

          std::filesystem::create_directories(IndexDir);
     }

     IndexWriter::~IndexWriter() = default;

     /*
      * WriteVarint - Write varint (variable-length integer encoding).
      */

     void IndexWriter::WriteVarint(std::vector<uint8_t>& Buffer, uint32_t Value) const
     {
          /* Varint encoding: 7 bits per byte, MSB indicates continuation. */

          while (Value >= 0x80)
          {
               Buffer.push_back(static_cast<uint8_t>(Value | 0x80));

               Value >>= 7;
          }

          Buffer.push_back(static_cast<uint8_t>(Value));
     }

     /*
      * WritePostings - Write delta-encoded docIDs.
      */

     void IndexWriter::WritePostings(std::vector<uint8_t>& PostingsBuffer, const std::vector<Posting>& Postings) const
     {
          if (Postings.empty())
          {
               return;
          }

          /* Write document count. */

          uint32_t DocCountValue = static_cast<uint32_t>(Postings.size());
          const uint8_t* DocCountBytes = reinterpret_cast<const uint8_t*>(&DocCountValue);

          PostingsBuffer.insert(PostingsBuffer.end(), DocCountBytes, DocCountBytes + sizeof(DocCountValue));

          /* Sort postings by docID for delta encoding. */

          std::vector<Posting> SortedPostings = Postings;

          std::sort(SortedPostings.begin(), SortedPostings.end(), [](const Posting& a, const Posting& b)
          {
               return a.DocumentID < b.DocumentID;
          });

          uint32_t PrevDocHash = 0;

          for (const auto& Post : SortedPostings)
          {
               std::hash<std::string> Hasher;
               uint32_t DocHash = static_cast<uint32_t>(Hasher(Post.DocumentID));

               /* Delta encode. */

               uint32_t DeltaValue = (PrevDocHash == 0) ? DocHash : DocHash - PrevDocHash;

               WriteVarint(PostingsBuffer, DeltaValue);

               /* Write score (float). */

               float ScoreValue = static_cast<float>(Post.Score);

               PostingsBuffer.insert(PostingsBuffer.end(), reinterpret_cast<const uint8_t*>(&ScoreValue), reinterpret_cast<const uint8_t*>(&ScoreValue) + sizeof(ScoreValue));

               /* Write docID length + docID string (for retrieval). */

               uint16_t DocIDLen = static_cast<uint16_t>(Post.DocumentID.length());

               PostingsBuffer.insert(PostingsBuffer.end(), reinterpret_cast<const uint8_t*>(&DocIDLen), reinterpret_cast<const uint8_t*>(&DocIDLen) + sizeof(DocIDLen));

               PostingsBuffer.insert(PostingsBuffer.end(), Post.DocumentID.begin(), Post.DocumentID.end());

               PrevDocHash = DocHash;
          }
     }

     /*
      * WriteIndex - Writes the complete index to disk.
      */

     bool IndexWriter::WriteIndex(const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>>& IndexData)
     {
          try
          {
               /* Step 1: Collect and sort all terms for this collection. */

               std::vector<std::string> Terms;
               auto CollectionIt = IndexData.find(Collection);

               if (CollectionIt == IndexData.end())
               {
                    Instance->Logs->Debug("mmap_index", "WriteIndex: Collection '" + Collection + "' not found in index.");

                    return false;
               }

               for (const auto& [TermValue, Postings] : CollectionIt->second)
               {
                    Terms.push_back(TermValue);
               }

               std::sort(Terms.begin(), Terms.end());

               /* Step 2: Write terms.bin (sorted, null-terminated). */

               std::string TermsFile = IndexDir + "/" + Collection + "/terms.bin";

               std::filesystem::create_directories(std::filesystem::path(TermsFile).parent_path());

               std::ofstream TermsOut(TermsFile, std::ios::binary);

               if (!TermsOut)
               {
                    Instance->Logs->Normal("mmap_index", "Failed to open terms.bin for writing.");

                    return false;
               }

               for (const auto& TermValue : Terms)
               {
                    TermsOut.write(TermValue.c_str(), TermValue.length());

                    TermsOut.put('\0');
               }

               TermsOut.close();

               /* Step 3: Write postings.bin and build term index. */

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

               const auto& CollectionTermMap = CollectionIt->second;

               for (const std::string& TermValue : Terms)
               {
                    auto TermIt = CollectionTermMap.find(TermValue);

                    if (TermIt == CollectionTermMap.end())
                    {
                         continue;
                    }

                    const auto& Postings = TermIt->second;
                    size_t PostingsStart = PostingsBuffer.size();

                    WritePostings(PostingsBuffer, Postings);

                    size_t PostingsLength = PostingsBuffer.size() - PostingsStart;

                    TermOffsets[TermValue] = {CurrentOffset, static_cast<uint32_t>(PostingsLength)};

                    CurrentOffset += PostingsLength;
               }

               PostingsOut.write(reinterpret_cast<const char*>(PostingsBuffer.data()), PostingsBuffer.size());

               PostingsOut.close();

               /* Step 4: Write term_map.bin (complete term->offset map for fast lookup). */

               std::string TermMapFile = IndexDir + "/" + Collection + "/term_map.bin";
               std::ofstream TermMapOut(TermMapFile, std::ios::binary);

               if (!TermMapOut)
               {
                    Instance->Logs->Normal("mmap_index", "Failed to open term_map.bin for writing.");

                    return false;
               }

               uint32_t TermCountValue = static_cast<uint32_t>(Terms.size());

               TermMapOut.write(reinterpret_cast<const char*>(&TermCountValue), sizeof(TermCountValue));

               for (const std::string& TermValue : Terms)
               {
                    auto OffsetIt = TermOffsets.find(TermValue);

                    if (OffsetIt == TermOffsets.end())
                    {
                         continue;
                    }

                    uint16_t TermLenValue = static_cast<uint16_t>(TermValue.length());
                    uint64_t PostingsOffsetValue = OffsetIt->second.first;
                    uint32_t PostingsLengthValue = OffsetIt->second.second;

                    TermMapOut.write(reinterpret_cast<const char*>(&TermLenValue), sizeof(TermLenValue));

                    TermMapOut.write(TermValue.c_str(), TermLenValue);

                    TermMapOut.write(reinterpret_cast<const char*>(&PostingsOffsetValue), sizeof(PostingsOffsetValue));

                    TermMapOut.write(reinterpret_cast<const char*>(&PostingsLengthValue), sizeof(PostingsLengthValue));
               }

               TermMapOut.close();

               /* Step 5: Write term_index.bin (sparse index for prefix search). */

               std::string TermIndexFileValue = IndexDir + "/" + Collection + "/term_index.bin";
               std::ofstream TermIndexOut(TermIndexFileValue, std::ios::binary);

               if (!TermIndexOut)
               {
                    Instance->Logs->Normal("mmap_index", "Failed to open term_index.bin for writing.");

                    return false;
               }

               uint32_t TermCountTotal = static_cast<uint32_t>(Terms.size());

               TermIndexOut.write(reinterpret_cast<const char*>(&TermCountTotal), sizeof(TermCountTotal));

               TermIndexOut.write(reinterpret_cast<const char*>(&IndexIntervalConst), sizeof(IndexIntervalConst));

               uint64_t TermsFileOffsetValue = 0;

               for (size_t I = 0; I < Terms.size(); I += IndexIntervalConst)
               {
                    const std::string& TermValue = Terms[I];
                    TermsFileOffsetValue = 0;

                    for (size_t J = 0; J < I; ++J)
                    {
                         TermsFileOffsetValue += Terms[J].length() + 1;
                    }

                    uint64_t PostingsOffsetValue = 0;
                    uint32_t PostingsLengthValue = 0;

                    auto OffsetIt = TermOffsets.find(TermValue);

                    if (OffsetIt != TermOffsets.end())
                    {
                         PostingsOffsetValue = OffsetIt->second.first;
                         PostingsLengthValue = OffsetIt->second.second;
                    }

                    TermIndexOut.write(reinterpret_cast<const char*>(&TermsFileOffsetValue), sizeof(TermsFileOffsetValue));

                    uint32_t TermLenVal = static_cast<uint32_t>(TermValue.length());

                    TermIndexOut.write(reinterpret_cast<const char*>(&TermLenVal), sizeof(TermLenVal));

                    TermIndexOut.write(reinterpret_cast<const char*>(&PostingsOffsetValue), sizeof(PostingsOffsetValue));

                    TermIndexOut.write(reinterpret_cast<const char*>(&PostingsLengthValue), sizeof(PostingsLengthValue));
               }

               TermIndexOut.close();

               Instance->Logs->Normal("mmap_index", "Wrote index for collection '" + Collection + "': " + std::to_string(Terms.size()) + " terms, " + std::to_string(PostingsBuffer.size()) + " bytes postings.");

               return true;
          }
          catch (const std::exception& e)
          {
               Instance->Logs->Normal("mmap_index", "Exception writing index: " + std::string(e.what()) + ".");

               return false;
          }
     }

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

     std::unique_ptr<MMapIndex> MMapIndex::Open(const std::string& IndexDirParam, const std::string& CollectionParam)
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

     bool MMapIndex::LoadIndex(const std::string& IndexDirParam, const std::string& CollectionParam)
     {
          try
          {
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

               TermsData = reinterpret_cast<const uint8_t*>(TermsMMap);

               if (HasTermMap)
               {
                    int TermMapFd = open(TermMapFile.c_str(), O_RDONLY);

                    if (TermMapFd >= 0)
                    {
                         TermMapSize = std::filesystem::file_size(TermMapFile);
                         TermMapMMap = mmap(nullptr, TermMapSize, PROT_READ, MAP_PRIVATE, TermMapFd, 0);

                         close(TermMapFd);

                         if (TermMapMMap != MAP_FAILED)
                         {
                              TermMapData = reinterpret_cast<const uint8_t*>(TermMapMMap);
                              TermCount = *reinterpret_cast<const uint32_t*>(TermMapData);
                         }
                         else
                         {
                              Unmap();

                              return false;
                         }
                    }
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
                              TermIndexData = reinterpret_cast<const uint8_t*>(TermIndexMMap);
                              const uint32_t* Header = reinterpret_cast<const uint32_t*>(TermIndexData);

                              if (!HasTermMap)
                              {
                                   TermCount = Header[0];
                              }

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

               PostingsData = reinterpret_cast<const uint8_t*>(PostingsMMap);

               Valid = true;

               Instance->Logs->Normal("mmap_index", "Loaded mmap index for collection '" + CollectionParam + "': " + std::to_string(TermCount) + " terms.");

               return true;
          }
          catch (const std::exception& e)
          {
               Instance->Logs->Normal("mmap_index", "Exception loading index: " + std::string(e.what()) + ".");

               Unmap();

               return false;
          }
     }

     /*
      * FindTerm - Finds a term in the memory-mapped index.
      */

     MMapIndex::TermEntry MMapIndex::FindTerm(const std::string& TermValue) const
     {
          TermEntry Ent = {0, 0, 0};

          if (!Valid || TermValue.empty())
          {
               return Ent;
          }

          if (TermMapData && TermMapSize > sizeof(uint32_t))
          {
               const uint8_t* Ptr = TermMapData + sizeof(uint32_t);
               const uint8_t* End = TermMapData + TermMapSize;

               while (Ptr < End)
               {
                    if (Ptr + sizeof(uint16_t) > End)
                    {
                         break;
                    }

                    uint16_t TermLenVal = *reinterpret_cast<const uint16_t*>(Ptr);

                    Ptr += sizeof(uint16_t);

                    if (Ptr + TermLenVal > End)
                    {
                         break;
                    }

                    std::string MapTerm(reinterpret_cast<const char*>(Ptr), TermLenVal);

                    Ptr += TermLenVal;

                    if (Ptr + sizeof(uint64_t) + sizeof(uint32_t) > End)
                    {
                         break;
                    }

                    uint64_t PostingsOffsetVal = *reinterpret_cast<const uint64_t*>(Ptr);
                    Ptr += sizeof(uint64_t);

                    uint32_t PostingsLengthVal = *reinterpret_cast<const uint32_t*>(Ptr);
                    Ptr += sizeof(uint32_t);

                    if (MapTerm == TermValue)
                    {
                         Ent.PostingsOffset = PostingsOffsetVal;
                         Ent.PostingsLength = PostingsLengthVal;

                         return Ent;
                    }

                    if (MapTerm > TermValue)
                    {
                         break;
                    }
               }
          }

          if (TermIndexData && TermIndexSize > 8)
          {
               const uint8_t* IndexPtr = TermIndexData + 8;
               size_t IndexEntries = (TermIndexSize - 8) / (sizeof(uint64_t) + sizeof(uint32_t) * 3);

               size_t Left = 0;
               size_t Right = IndexEntries;
               size_t SparseIdx = 0;

               while (Left < Right)
               {
                    size_t Mid = (Left + Right) / 2;
                    const uint64_t* EntryPtr = reinterpret_cast<const uint64_t*>(IndexPtr + Mid * (sizeof(uint64_t) + sizeof(uint32_t) * 3));

                    uint64_t TermsOffsetVal = EntryPtr[0];
                    uint32_t TermLenVal = reinterpret_cast<const uint32_t*>(EntryPtr + 1)[0];

                    if (TermsOffsetVal >= TermsSize)
                    {
                         break;
                    }

                    const char* TermStr = reinterpret_cast<const char*>(TermsData + TermsOffsetVal);
                    std::string SparseTerm(TermStr, TermLenVal);

                    if (SparseTerm < TermValue)
                    {
                         Left = Mid + 1;
                         SparseIdx = Mid;
                    }
                    else
                    {
                         Right = Mid;

                         if (SparseTerm == TermValue)
                         {
                              SparseIdx = Mid;

                              break;
                         }
                    }
               }

               if (SparseIdx < IndexEntries)
               {
                    const uint64_t* SparseEntry = reinterpret_cast<const uint64_t*>(IndexPtr + SparseIdx * (sizeof(uint64_t) + sizeof(uint32_t) * 3));

                    uint64_t SearchStartOffset = SparseEntry[0];
                    uint32_t SearchStartLen = reinterpret_cast<const uint32_t*>(SparseEntry + 1)[0];
                    uint64_t StartPostingsOffset = reinterpret_cast<const uint64_t*>(SparseEntry + 1)[1];
                    uint32_t StartPostingsLength = reinterpret_cast<const uint32_t*>(SparseEntry + 2)[1];

                    if (SearchStartOffset < TermsSize)
                    {
                         const char* SparseTermStr = reinterpret_cast<const char*>(TermsData + SearchStartOffset);
                         size_t ActualLen = strlen(SparseTermStr);

                         if (ActualLen == SearchStartLen)
                         {
                              std::string SparseTermVal(SparseTermStr, SearchStartLen);

                              if (SparseTermVal == TermValue)
                              {
                                   Ent.PostingsOffset = StartPostingsOffset;
                                   Ent.PostingsLength = StartPostingsLength;

                                   return Ent;
                              }
                         }
                    }
               }
          }

          return Ent;
     }

     /*
      * DecodePostings - Decodes postings from the memory-mapped buffer.
      */

     std::vector<Posting> MMapIndex::DecodePostings(const uint8_t* DataParam, size_t LengthParam) const
     {
          std::vector<Posting> PostingsList;

          if (!DataParam || LengthParam < sizeof(uint32_t))
          {
               return PostingsList;
          }

          uint32_t DocCountValue = *reinterpret_cast<const uint32_t*>(DataParam);
          const uint8_t* Ptr = DataParam + sizeof(uint32_t);

          uint32_t PrevDocHash = 0;

          for (uint32_t I = 0; I < DocCountValue && Ptr < DataParam + LengthParam; ++I)
          {
               uint32_t DeltaValue = 0;
               int ShiftValue = 0;

               while (Ptr < DataParam + LengthParam && (*Ptr & 0x80))
               {
                    DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;

                    ShiftValue += 7;
                    Ptr++;

                    if (ShiftValue >= 32)
                    {
                         break;
                    }
               }

               if (Ptr < DataParam + LengthParam && ShiftValue < 32)
               {
                    DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;
                    Ptr++;
               }

               uint32_t DocHash = (PrevDocHash == 0) ? DeltaValue : PrevDocHash + DeltaValue;
               PrevDocHash = DocHash;

               if (Ptr + sizeof(float) > DataParam + LengthParam)
               {
                    break;
               }

               float ScoreValue = *reinterpret_cast<const float*>(Ptr);
               Ptr += sizeof(float);

               if (Ptr + sizeof(uint16_t) > DataParam + LengthParam)
               {
                    break;
               }

               uint16_t DocIDLenVal = *reinterpret_cast<const uint16_t*>(Ptr);
               Ptr += sizeof(uint16_t);

               if (Ptr + DocIDLenVal > DataParam + LengthParam)
               {
                    break;
               }

               std::string DocIDStr(reinterpret_cast<const char*>(Ptr), DocIDLenVal);
               Ptr += DocIDLenVal;

               Posting Post;
               Post.DocumentID = DocIDStr;
               Post.Score = ScoreValue;

               PostingsList.push_back(Post);
          }

          return PostingsList;
     }

     /*
      * SearchTerm - Searches for a term in the memory-mapped index.
      */

     std::vector<Posting> MMapIndex::SearchTerm(const std::string& TermValue) const
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
      * SearchPrefix - Searches for terms with a given prefix in the index.
      */

     std::vector<Posting> MMapIndex::SearchPrefix(const std::string& PrefixValue, int LimitVal) const
     {
          std::vector<Posting> ResultsList;

          if (!Valid || PrefixValue.empty())
          {
               return ResultsList;
          }

          const char* Ptr = reinterpret_cast<const char*>(TermsData);
          const char* End = reinterpret_cast<const char*>(TermsData + TermsSize);

          std::unordered_map<std::string, Posting> DocScores;

          while (Ptr < End && ResultsList.size() < static_cast<size_t>(LimitVal))
          {
               std::string TermVal(Ptr);

               if (TermVal.length() >= PrefixValue.length() && TermVal.substr(0, PrefixValue.length()) == PrefixValue)
               {
                    auto Postings = SearchTerm(TermVal);

                    for (const auto& Post : Postings)
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

          for (const auto& [DocID, Post] : DocScores)
          {
               ResultsList.push_back(Post);
          }

          std::sort(ResultsList.begin(), ResultsList.end(), [](const Posting& a, const Posting& b)
          {
               return a.Score > b.Score;
          });

          if (LimitVal > 0 && static_cast<int>(ResultsList.size()) > LimitVal)
          {
               ResultsList.resize(LimitVal);
          }

          return ResultsList;
     }

}
