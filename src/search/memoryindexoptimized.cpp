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
#include <cstring>

#include "core/hlquery.h"
#include "search/lindex.h"
#include "search/mindex.h"
#include "utils/simdutils.h"

/* ReadMappedTerm - Finds the null terminator for a term stored inside the memory-mapped terms file. */

bool ReadMappedTerm(const char *TermStartPtr, const char *TermsEndPtr, size_t &TermLenOut)
{
     if (!TermStartPtr || TermStartPtr >= TermsEndPtr)
     {
          return false;
     }

     const void *NullTerminator = std::memchr(TermStartPtr, '\0', static_cast<size_t>(TermsEndPtr - TermStartPtr));

     if (!NullTerminator)
     {
          return false;
     }

     TermLenOut = static_cast<const char *>(NullTerminator) - TermStartPtr;
     return true;
}

/*
 * MMapIndex::FindTermOptimized - Looks up a term in the packed term map using binary search.
 */

MMapIndex::TermEntry MMapIndex::FindTermOptimized(const std::string &TermParam) const
{
     TermEntry Ent = {0, 0, 0};

     if (!Valid || TermParam.empty() || !TermMapData || TermMapSize <= sizeof(uint32_t))
     {
          return Ent;
     }

     const uint8_t *Ptr = TermMapData + sizeof(uint32_t);
     const uint8_t *EndPtr = TermMapData + TermMapSize;

     for (uint32_t I = 0; I < TermCount && Ptr < EndPtr; ++I)
     {
          if (Ptr + sizeof(uint16_t) > EndPtr)
          {
               break;
          }

          uint16_t TermLenValue = 0;
          std::memcpy(&TermLenValue, Ptr, sizeof(TermLenValue));
          Ptr += sizeof(uint16_t);

          if (Ptr + TermLenValue + sizeof(uint64_t) + sizeof(uint32_t) > EndPtr)
          {
               break;
          }

          const char *MapTermPtr = reinterpret_cast<const char *>(Ptr);
          const int CmpResultValue = fast_string_compare(TermParam.c_str(), MapTermPtr, TermParam.length(), TermLenValue);

          Ptr += TermLenValue;

          uint64_t PostingsOffsetValue = 0;
          uint32_t PostingsLengthValue = 0;
          std::memcpy(&PostingsOffsetValue, Ptr, sizeof(PostingsOffsetValue));
          Ptr += sizeof(PostingsOffsetValue);
          std::memcpy(&PostingsLengthValue, Ptr, sizeof(PostingsLengthValue));
          Ptr += sizeof(PostingsLengthValue);

          if (CmpResultValue == 0)
          {
               Ent.PostingsOffset = PostingsOffsetValue;
               Ent.PostingsLength = PostingsLengthValue;
               return Ent;
          }

          if (CmpResultValue < 0)
          {
               break;
          }
     }

     return Ent;
}

/*
 * MMapIndex::SearchPrefixOptimized - Finds and aggregates all terms that share the requested prefix.
 */

std::vector<Posting> MMapIndex::SearchPrefixOptimized(const std::string &PrefixValue, int LimitVal) const
{
     std::vector<Posting> ResultsList;

     if (!Valid || PrefixValue.empty() || !TermsData || TermsSize == 0)
     {
          return ResultsList;
     }

     const char *TermsStartPtr = reinterpret_cast<const char *>(TermsData);

     const char *TermsEndPtr = reinterpret_cast<const char *>(TermsData + TermsSize);

     const char *LowerBoundPtr = TermsStartPtr;
     const char *UpperBoundPtr = TermsEndPtr;

     /* First find the lower bound for the prefix so the linear scan only touches the matching slice. */

     while (LowerBoundPtr < UpperBoundPtr)
     {
          const char *MidPtr = LowerBoundPtr + (UpperBoundPtr - LowerBoundPtr) / 2;
          const char *TermStartPtr = MidPtr;

          while (TermStartPtr > TermsStartPtr && TermStartPtr[-1] != '\0')
          {
               TermStartPtr--;
          }

          size_t TermLenVal = 0;

          if (!ReadMappedTerm(TermStartPtr, TermsEndPtr, TermLenVal))
          {
               return ResultsList;
          }

          size_t CmpLenVal = std::min(PrefixValue.length(), TermLenVal);

          int CmpResultValue = fast_string_compare(PrefixValue.c_str(), TermStartPtr, PrefixValue.length(), CmpLenVal);

          if (CmpResultValue <= 0)
          {
               UpperBoundPtr = TermStartPtr;
          }
          else
          {
               LowerBoundPtr = TermStartPtr + TermLenVal + 1;
          }
     }

     std::unordered_map<std::string, Posting> DocScoresMap;

     DocScoresMap.reserve(LimitVal > 0 ? static_cast<size_t>(LimitVal) : 1000);

     const char *CurrentPtr = LowerBoundPtr;

     /* Merge document scores from every term that still matches the prefix. */

     while (CurrentPtr < TermsEndPtr)
     {
          size_t TermLenValue = 0;

          if (!ReadMappedTerm(CurrentPtr, TermsEndPtr, TermLenValue))
          {
               break;
          }

          if (TermLenValue >= PrefixValue.length())
          {
               if (fast_string_equal(CurrentPtr, PrefixValue.c_str(), PrefixValue.length()))
               {
                    std::string TermStrVal(CurrentPtr, TermLenValue);

                    auto PostingsList = SearchTerm(TermStrVal);

                    for (const auto &Post : PostingsList)
                    {
                         auto It = DocScoresMap.find(Post.DocumentID);

                         if (It == DocScoresMap.end())
                         {
                              DocScoresMap[Post.DocumentID] = Post;
                         }
                         else
                         {
                              It->second.Score += Post.Score;
                         }
                    }
               }
               else
               {
                    break;
               }
          }
          else
          {
               break;
          }

          CurrentPtr += TermLenValue + 1;
     }

     ResultsList.reserve(DocScoresMap.size());

     for (const auto &[DocID, Post] : DocScoresMap)
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

/*
 * MMapIndex::DecodePostingsOptimized - Decodes postings with a fast path for short varints.
 */

std::vector<Posting> MMapIndex::DecodePostingsOptimized(const uint8_t *DataParam, size_t LengthParam) const
{
     std::vector<Posting> PostingsList;

     if (!DataParam || LengthParam < sizeof(uint32_t))
     {
          return PostingsList;
     }

     uint32_t DocCountValue = 0;
     std::memcpy(&DocCountValue, DataParam, sizeof(DocCountValue));

     const uint8_t *Ptr = DataParam + sizeof(uint32_t);
     const uint8_t *EndPtr = DataParam + LengthParam;

     PostingsList.reserve(DocCountValue);

     uint32_t PrevDocHash = 0;

     for (uint32_t I = 0; I < DocCountValue && Ptr < EndPtr; ++I)
     {
          uint32_t DeltaValue = 0;

          /* Most deltas fit in one byte, so keep a branch for the common case before entering the slower loop. */

          if (Ptr < EndPtr && (*Ptr & 0x80) == 0)
          {
               DeltaValue = *Ptr++;
          }
          else
          {
               uint32_t ShiftValue = 0;

               while (Ptr < EndPtr && (*Ptr & 0x80) && ShiftValue < 28U)
               {
                    DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;

                    ShiftValue += 7U;
                    Ptr++;
               }

               if (Ptr < EndPtr && ShiftValue < 32U)
               {
                    DeltaValue |= static_cast<uint32_t>(*Ptr & 0x7F) << ShiftValue;
                    Ptr++;
               }
          }

          uint32_t DocHash = (PrevDocHash == 0) ? DeltaValue : PrevDocHash + DeltaValue;

          PrevDocHash = DocHash;

          if (Ptr + sizeof(float) > EndPtr)
          {
               break;
          }

          float ScoreValue = 0.0F;
          std::memcpy(&ScoreValue, Ptr, sizeof(ScoreValue));

          Ptr += sizeof(float);

          if (Ptr + sizeof(uint16_t) > EndPtr)
          {
               break;
          }

          uint16_t DocIDLenVal = 0;
          std::memcpy(&DocIDLenVal, Ptr, sizeof(DocIDLenVal));

          Ptr += sizeof(uint16_t);

          if (Ptr + DocIDLenVal > EndPtr)
          {
               break;
          }

          Posting Post;

          Post.DocumentID.assign(reinterpret_cast<const char *>(Ptr), DocIDLenVal);
          Post.Score = ScoreValue;

          PostingsList.push_back(std::move(Post));

          Ptr += DocIDLenVal;
     }

     return PostingsList;
}
