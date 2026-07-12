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
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "core/hlquery.h"
#include "search/lindex.h"
#include "search/mindex.h"
#include "utils/wildcard.h"

namespace
{
     /* The mmap index is split into several small files instead of one monolithic
      * blob so each reader can map only the structures it needs. Every file uses
      * the same fixed header, followed by a type-specific payload.
      */

     constexpr uint32_t kMMapIndexVersion = 1;
     constexpr uint32_t kMMapIndexEndianMarker = 0x01020304U;
     constexpr size_t kMMapFileHeaderSize = 40;
     constexpr uint64_t kFnv64Offset = 14695981039346656037ULL;
     constexpr uint64_t kFnv64Prime = 1099511628211ULL;
     constexpr uint32_t kFnv32Offset = 2166136261U;
     constexpr uint32_t kFnv32Prime = 16777619U;

     const std::array<uint8_t, 8> kTermsMagic = {'H', 'L', 'Q', 'T', 'E', 'R', 'M', '1'};
     const std::array<uint8_t, 8> kTermMapMagic = {'H', 'L', 'Q', 'T', 'M', 'A', 'P', '1'};
     const std::array<uint8_t, 8> kTermIndexMagic = {'H', 'L', 'Q', 'T', 'I', 'D', 'X', '1'};
     const std::array<uint8_t, 8> kPostingsMagic = {'H', 'L', 'Q', 'P', 'O', 'S', 'T', '1'};

     /* StableChecksum64 computes a deterministic checksum over serialized bytes.
      * The checksum is intentionally simple and portable because its purpose is
      * corruption detection, not cryptographic validation.
      */

     uint64_t StableChecksum64(const uint8_t *Data, size_t Length)
     {
          uint64_t Hash = kFnv64Offset;

          for (size_t I = 0; I < Length; ++I)
          {
               Hash ^= Data[I];
               Hash *= kFnv64Prime;
          }

          return Hash;
     }

     /* StableHash32 produces the document hash used by postings delta encoding.
      * The writer stores full document IDs too, so hash collisions only affect
      * compression efficiency and not lookup correctness.
      */

     uint32_t StableHash32(const std::string &Value)
     {
          uint32_t Hash = kFnv32Offset;

          for (unsigned char C : Value)
          {
               Hash ^= C;
               Hash *= kFnv32Prime;
          }

          return Hash;
     }

     /* AppendBytes appends raw binary data to a serialization buffer. Callers are
      * responsible for choosing values with stable sizes and explicit widths.
      */

     void AppendBytes(std::vector<uint8_t> &Buffer, const void *Data, size_t Length)
     {
          const uint8_t *Bytes = static_cast<const uint8_t *>(Data);
          Buffer.insert(Buffer.end(), Bytes, Bytes + Length);
     }

     /* AppendValue stores a trivially copyable scalar in the native on-disk layout
      * used by this index version.
      */

     template <typename T>
     void AppendValue(std::vector<uint8_t> &Buffer, const T &Value)
     {
          AppendBytes(Buffer, &Value, sizeof(Value));
     }

     /* WriteMMapFile writes the common file envelope:
      * magic, version, header size, endian marker, reserved bytes, payload size,
      * checksum, then the payload itself.
      */

     bool WriteMMapFile(const std::string &Path, const std::array<uint8_t, 8> &Magic, const std::vector<uint8_t> &Payload)
     {
          std::ofstream Out(Path, std::ios::binary);

          if (!Out)
          {
               return false;
          }

          const uint32_t Version = kMMapIndexVersion;
          const uint32_t HeaderSize = static_cast<uint32_t>(kMMapFileHeaderSize);
          const uint32_t Endian = kMMapIndexEndianMarker;
          const uint32_t Reserved = 0;
          const uint64_t PayloadSize = static_cast<uint64_t>(Payload.size());
          const uint64_t Checksum = StableChecksum64(Payload.data(), Payload.size());

          Out.write(reinterpret_cast<const char *>(Magic.data()), Magic.size());
          Out.write(reinterpret_cast<const char *>(&Version), sizeof(Version));
          Out.write(reinterpret_cast<const char *>(&HeaderSize), sizeof(HeaderSize));
          Out.write(reinterpret_cast<const char *>(&Endian), sizeof(Endian));
          Out.write(reinterpret_cast<const char *>(&Reserved), sizeof(Reserved));
          Out.write(reinterpret_cast<const char *>(&PayloadSize), sizeof(PayloadSize));
          Out.write(reinterpret_cast<const char *>(&Checksum), sizeof(Checksum));

          if (!Payload.empty())
          {
               Out.write(reinterpret_cast<const char *>(Payload.data()), Payload.size());
          }

          return Out.good();
     }

     /* ReadValue copies a scalar from a mapped file only after checking that the
      * requested byte range is fully contained in the mapped region.
      */

     template <typename T>
     bool ReadValue(const uint8_t *Base, size_t Size, size_t Offset, T &Out)
     {
          if (Offset > Size || sizeof(T) > Size - Offset)
          {
               return false;
          }

          std::memcpy(&Out, Base + Offset, sizeof(T));
          return true;
     }

     /* ReadMappedTerm finds the next null terminator without reading past the
      * mapped terms payload.
      */

     bool ReadMappedTerm(const char *TermStartPtr, const char *TermsEndPtr, std::string &TermOut)
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

          const char *TermEndPtr = static_cast<const char *>(NullTerminator);

          TermOut.assign(TermStartPtr, static_cast<size_t>(TermEndPtr - TermStartPtr));
          return true;
     }

     /* ValidateMMapFile verifies the shared header before the reader trusts any
      * payload pointer. This keeps malformed or partial files from being decoded
      * as valid term or postings data.
      */

     bool ValidateMMapFile(const uint8_t *Base,
                           size_t FileSize,
                           const std::array<uint8_t, 8> &Magic,
                           const uint8_t *&Payload,
                           size_t &PayloadSize,
                           const std::string &Path)
     {
          if (!Base || FileSize < kMMapFileHeaderSize)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("mmap_index", "Index file too small or missing header: " + Path + ".");
               }
               return false;
          }

          if (!std::equal(Magic.begin(), Magic.end(), Base))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("mmap_index", "Invalid index file magic: " + Path + ".");
               }
               return false;
          }

          uint32_t Version = 0;
          uint32_t HeaderSize = 0;
          uint32_t Endian = 0;
          uint64_t StoredPayloadSize = 0;
          uint64_t StoredChecksum = 0;

          /* Header fields live at fixed byte offsets so mmap readers can validate
           * files without constructing any temporary parser state.
           */

          if (!ReadValue(Base, FileSize, 8, Version) ||
              !ReadValue(Base, FileSize, 12, HeaderSize) ||
              !ReadValue(Base, FileSize, 16, Endian) ||
              !ReadValue(Base, FileSize, 24, StoredPayloadSize) ||
              !ReadValue(Base, FileSize, 32, StoredChecksum))
          {
               return false;
          }

          if (Version != kMMapIndexVersion || HeaderSize != kMMapFileHeaderSize || Endian != kMMapIndexEndianMarker)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("mmap_index", "Unsupported index file header: " + Path + ".");
               }
               return false;
          }

          if (StoredPayloadSize > FileSize - HeaderSize)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("mmap_index", "Invalid index payload size: " + Path + ".");
               }
               return false;
          }

          Payload = Base + HeaderSize;
          PayloadSize = static_cast<size_t>(StoredPayloadSize);

          /* The checksum covers only the payload. Header mutations are caught by
           * explicit magic, version, size, and endian checks above.
           */

          if (StableChecksum64(Payload, PayloadSize) != StoredChecksum)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("mmap_index", "Index checksum mismatch: " + Path + ".");
               }
               return false;
          }

          return true;
     }
}

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

     for (const auto &Post : Postings)
     {
          HashedPosting Entry;

          Entry.Hash = StableHash32(Post.DocumentID);
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

          if (Entry.Post.DocumentID.length() > std::numeric_limits<uint16_t>::max())
          {
               throw std::runtime_error("Document ID is too long for mmap postings format");
          }

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
          /* The writer receives the process-local inverted index grouped by
           * collection and term. It flattens only the requested collection into
           * mmap-friendly files.
           */

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

          const auto &CollectionTermMap = CollectionIt->second;

          for (const std::string &TermValue : Terms)
          {
               if (TermValue.length() > std::numeric_limits<uint16_t>::max())
               {
                    Instance->Logs->Normal("mmap_index", "Term is too long for mmap term map: " + TermValue + ".");

                    return false;
               }

               auto TermIt = CollectionTermMap.find(TermValue);

               if (TermIt == CollectionTermMap.end())
               {
                    continue;
               }

               for (const auto &Post : TermIt->second)
               {
                    if (Post.DocumentID.length() > std::numeric_limits<uint16_t>::max())
                    {
                         Instance->Logs->Normal("mmap_index", "Document ID is too long for mmap postings format: " + Post.DocumentID + ".");

                         return false;
                    }
               }
          }

          /* Write sorted terms to terms.bin as null-terminated strings. */

          std::string TermsFile = IndexDir + "/" + Collection + "/terms.bin";

          std::filesystem::create_directories(std::filesystem::path(TermsFile).parent_path());

          std::vector<uint8_t> TermsBuffer;

          /* Each term is stored as a null-terminated string. Keeping the terms
           * sorted allows prefix and wildcard scans to walk a compact byte range.
           */

          for (const auto &TermValue : Terms)
          {
               TermsBuffer.insert(TermsBuffer.end(), TermValue.begin(), TermValue.end());
               TermsBuffer.push_back('\0');
          }

          if (!WriteMMapFile(TermsFile, kTermsMagic, TermsBuffer))
          {
               Instance->Logs->Normal("mmap_index", "Failed to write terms.bin.");

               return false;
          }

          /* Write postings to postings.bin and track offsets per term. */

          std::string PostingsFile = IndexDir + "/" + Collection + "/postings.bin";

          std::vector<uint8_t> PostingsBuffer;

          std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> TermOffsets;

          uint64_t CurrentOffset = 0;
          const uint32_t IndexIntervalConst = 16;

          /* The postings file stores every term's postings back-to-back. The
           * separate term map records where each term starts and how many bytes
           * its encoded postings occupy.
           */

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

          if (!WriteMMapFile(PostingsFile, kPostingsMagic, PostingsBuffer))
          {
               Instance->Logs->Normal("mmap_index", "Failed to write postings.bin.");

               return false;
          }

          /* Write complete term->offset map for fast direct lookup. */

          std::string TermMapFile = IndexDir + "/" + Collection + "/term_map.bin";

          std::vector<uint8_t> TermMapBuffer;

          uint32_t TermCountValue = static_cast<uint32_t>(Terms.size());

          AppendValue(TermMapBuffer, TermCountValue);

          /* The term map is the direct lookup structure:
           * term length, term bytes, postings offset, and postings length.
           */

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

               AppendValue(TermMapBuffer, TermLenValue);
               AppendBytes(TermMapBuffer, TermValue.c_str(), TermLenValue);
               AppendValue(TermMapBuffer, PostingsOffsetValue);
               AppendValue(TermMapBuffer, PostingsLengthValue);
          }

          if (!WriteMMapFile(TermMapFile, kTermMapMagic, TermMapBuffer))
          {
               Instance->Logs->Normal("mmap_index", "Failed to write term_map.bin.");

               return false;
          }

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

          std::vector<uint8_t> TermIndexBuffer;

          uint32_t TermCountTotal = static_cast<uint32_t>(Terms.size());

          AppendValue(TermIndexBuffer, TermCountTotal);
          AppendValue(TermIndexBuffer, IndexIntervalConst);

          /* The sparse term index samples every Nth term. It is smaller than the
           * full term map and gives prefix search a future place to jump into the
           * sorted term stream without scanning from the beginning.
           */

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

               AppendValue(TermIndexBuffer, TermsFileOffsetValue);

               uint32_t TermLenVal = static_cast<uint32_t>(TermValue.length());

               AppendValue(TermIndexBuffer, TermLenVal);
               AppendValue(TermIndexBuffer, PostingsOffsetValue);
               AppendValue(TermIndexBuffer, PostingsLengthValue);
          }

          if (!WriteMMapFile(TermIndexFileValue, kTermIndexMagic, TermIndexBuffer))
          {
               Instance->Logs->Normal("mmap_index", "Failed to write term_index.bin.");

               return false;
          }

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
          munmap(TermsMMap, TermsMappedSize);

          TermsMMap = nullptr;
          TermsMappedSize = 0;
          TermsSize = 0;
     }

     if (TermMapMMap && TermMapMMap != MAP_FAILED)
     {
          munmap(TermMapMMap, TermMapMappedSize);

          TermMapMMap = nullptr;
          TermMapMappedSize = 0;
          TermMapSize = 0;
     }

     if (TermIndexMMap && TermIndexMMap != MAP_FAILED)
     {
          munmap(TermIndexMMap, TermIndexMappedSize);

          TermIndexMMap = nullptr;
          TermIndexMappedSize = 0;
          TermIndexSize = 0;
     }

     if (PostingsMMap && PostingsMMap != MAP_FAILED)
     {
          munmap(PostingsMMap, PostingsMappedSize);

          PostingsMMap = nullptr;
          PostingsMappedSize = 0;
          PostingsSize = 0;
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
          /* Loading maps files into the process address space and keeps raw
           * payload pointers after each file header has been validated.
           */

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

          TermsMappedSize = std::filesystem::file_size(TermsFile);

          /* terms.bin is mapped first because term-oriented operations use it as
           * the canonical sorted list of indexed terms.
           */

          TermsMMap = mmap(nullptr, TermsMappedSize, PROT_READ, MAP_PRIVATE, TermsFd, 0);

          close(TermsFd);

          if (TermsMMap == MAP_FAILED)
          {
               return false;
          }

          if (!ValidateMMapFile(reinterpret_cast<const uint8_t *>(TermsMMap), TermsMappedSize, kTermsMagic, TermsData, TermsSize, TermsFile))
          {
               Unmap();

               return false;
          }

          int TermMapFd = open(TermMapFile.c_str(), O_RDONLY);

          /* term_map.bin carries the exact postings offsets. Without it the
           * reader cannot turn a term into a postings slice safely.
           */

          if (TermMapFd >= 0)
          {
               TermMapMappedSize = std::filesystem::file_size(TermMapFile);

               TermMapMMap = mmap(nullptr, TermMapMappedSize, PROT_READ, MAP_PRIVATE, TermMapFd, 0);

               close(TermMapFd);

               if (TermMapMMap != MAP_FAILED)
               {
                    if (!ValidateMMapFile(reinterpret_cast<const uint8_t *>(TermMapMMap), TermMapMappedSize, kTermMapMagic, TermMapData, TermMapSize, TermMapFile) ||
                        TermMapSize < sizeof(uint32_t))
                    {
                         Unmap();

                         return false;
                    }

                    std::memcpy(&TermCount, TermMapData, sizeof(TermCount));
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
               /* term_index.bin is optional. The index remains usable without the
                * sparse accelerator because exact term lookup relies on term_map.bin.
                */

               int TermIndexFd = open(TermIndexFile.c_str(), O_RDONLY);

               if (TermIndexFd >= 0)
               {
                    TermIndexMappedSize = std::filesystem::file_size(TermIndexFile);

                    TermIndexMMap = mmap(nullptr, TermIndexMappedSize, PROT_READ, MAP_PRIVATE, TermIndexFd, 0);

                    close(TermIndexFd);

                    if (TermIndexMMap != MAP_FAILED)
                    {
                         if (!ValidateMMapFile(reinterpret_cast<const uint8_t *>(TermIndexMMap), TermIndexMappedSize, kTermIndexMagic, TermIndexData, TermIndexSize, TermIndexFile) ||
                             TermIndexSize < sizeof(uint32_t) * 2)
                         {
                              Unmap();

                              return false;
                         }

                         std::memcpy(&IndexInterval, TermIndexData + sizeof(uint32_t), sizeof(IndexInterval));
                    }
               }
          }

          int PostingsFd = open(PostingsFile.c_str(), O_RDONLY);

          if (PostingsFd < 0)
          {
               Unmap();

               return false;
          }

          PostingsMappedSize = std::filesystem::file_size(PostingsFile);

          /* postings.bin is mapped last because all previous structures only
           * point into offsets inside this payload.
           */

          PostingsMMap = mmap(nullptr, PostingsMappedSize, PROT_READ, MAP_PRIVATE, PostingsFd, 0);

          close(PostingsFd);

          if (PostingsMMap == MAP_FAILED)
          {
               Unmap();

               return false;
          }

          if (!ValidateMMapFile(reinterpret_cast<const uint8_t *>(PostingsMMap), PostingsMappedSize, kPostingsMagic, PostingsData, PostingsSize, PostingsFile))
          {
               Unmap();

               return false;
          }

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

     uint32_t DocCountValue = 0;
     std::memcpy(&DocCountValue, DataParam, sizeof(DocCountValue));

     /* The postings payload starts with the number of encoded postings. Each
      * following entry contains a delta-encoded document hash, score, document
      * ID length, and document ID bytes.
      */

     const uint8_t *Ptr = DataParam + sizeof(uint32_t);
     const uint8_t *EndPtr = DataParam + LengthParam;

     uint32_t PrevDocHash = 0;

     for (uint32_t I = 0; I < DocCountValue && Ptr < EndPtr; ++I)
     {
          uint32_t DeltaValue = 0;
          uint32_t ShiftValue = 0;

          /* Decode one unsigned varint. A malformed varint stops growing after
           * 32 bits so corrupted input cannot shift indefinitely.
           */

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

          /* The full document ID is stored below, so the reconstructed hash is
           * used only to advance the delta stream consistently.
           */

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

          /* The document ID is copied out of the mmap region because callers own
           * Posting values independently of the mapped file lifetime.
           */

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

     if (!PostingsData || Ent.PostingsOffset > PostingsSize || Ent.PostingsLength > PostingsSize - Ent.PostingsOffset)
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

     /* Prefix search walks terms.bin and aggregates scores for documents that
      * appear under multiple matching terms.
      */

     while (Ptr < End)
     {
          std::string TermVal;

          if (!ReadMappedTerm(Ptr, End, TermVal))
          {
               break;
          }

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

     /* Results are sorted after aggregation so the strongest combined document
      * score wins, not just the strongest single term posting.
      */

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

     /* The mmap format does not store a standalone document table, so the unique
      * document count is derived once from all postings and then cached.
      */

     while (Ptr < End)
     {
          std::string TermVal;

          if (!ReadMappedTerm(Ptr, End, TermVal))
          {
               break;
          }

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

     /* Wildcard search must inspect candidate terms directly because wildcard
      * patterns are not guaranteed to share a fixed searchable prefix.
      */

     while (Ptr < End)
     {
          std::string TermVal;

          if (!ReadMappedTerm(Ptr, End, TermVal))
          {
               break;
          }

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
