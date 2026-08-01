/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <shared_mutex>
#include <string>
#include <vector>

#include "runtime/serverconfig.h"
#include "search/segment_catalog.h"

class SegmentManager
{
   public:
     struct SegmentStats
     {
          uint64_t TotalBytes = 0;
          uint64_t NumSstFiles = 0;
          std::string ActiveSegmentId;
          uint64_t SealedSegmentCount = 0;
          uint64_t TombstoneCountEstimate = 0;
          uint64_t ManifestGeneration = 0;
     };

     SegmentManager(const std::string &data_dir,
                    rocksdb::DB *system_db,
                    const rocksdb::Options &options,
                    const rocksdb::WriteOptions &write_options,
                    const RocksDBOptions &storage_options);

     bool Initialize();
     void Shutdown();

     bool SetDocument(const std::string &key, const std::string &value);
     size_t BatchSetDocuments(const std::vector<std::pair<std::string, std::string>> &key_values);
     std::string GetDocument(const std::string &key);
     int DeleteDocument(const std::string &key);
     size_t DeleteDocumentRange(const std::string &start_key, const std::string &end_key);
     bool ExistsDocument(const std::string &key);
     std::vector<std::string> Keys(const std::string &pattern);
     size_t CountKeys(const std::string &prefix);
     std::vector<std::string> PrefixKeys(const std::string &prefix, size_t offset, size_t limit);
     bool ForEachPrefixKeySnapshot(const std::string &prefix, size_t limit, const std::function<bool(const std::string &)> &callback);
     size_t GetPrefixTotalSize(const std::string &prefix);

     bool Flush();
     bool FlushAndSync();
     bool SyncWAL();
     void Compact();
     bool ClearAllDocuments();

     bool RebuildDocLocationMap();
     SegmentStats GetStats() const;

   private:
     struct SegmentHandle
     {
          SegmentMetadata Metadata;
          std::string Directory;
          std::string RocksDBPath;
          std::shared_ptr<rocksdb::DB> DB;
     };

     struct SegmentSnapshot
     {
          uint64_t Generation = 0;
          std::string ActiveId;
          std::vector<std::shared_ptr<SegmentHandle>> LiveNewestFirst;
     };

     std::string DataDir;
     std::string StorageDir;
     std::string SegmentsDir;
     std::string ManifestPath;
     rocksdb::DB *SystemDB;
     rocksdb::Options OptionsValue;
     rocksdb::WriteOptions WriteOptionsValue;
     RocksDBOptions StorageOptions;

     /*
      * Normal document writes share the active segment. Rollover, flush and
      * range deletion take this mutex exclusively.
      */
     std::shared_mutex WriteMutex;
     mutable std::mutex SegmentMutex;
     std::shared_ptr<SegmentHandle> ActiveSegment;
     std::vector<std::shared_ptr<SegmentHandle>> SealedSegments;
     SegmentManifest Manifest;
     std::shared_ptr<SegmentSnapshot> CurrentSnapshot;
     std::array<std::mutex, 4096> KeyMutexes;
     uint64_t ActiveReservedDocuments = 0;
     uint64_t ActiveReservedBytes = 0;
     uint64_t ActiveMetadataDirtyDocuments = 0;
     uint64_t ActiveMetadataDirtyBytes = 0;
     uint64_t LastMetadataPersistMs = 0;

     static bool IsDocKey(const std::string &key);
     static bool ParseDocKey(const std::string &key, std::string &collection, std::string &doc_id);
     static std::string DocLocationKey(const std::string &collection, const std::string &doc_id);
     static std::string TombstoneKey(const std::string &collection, const std::string &doc_id);
     static uint64_t NowMs();
     static uint64_t DirectorySize(const std::string &path, uint64_t *sst_files = nullptr);
     static std::string FormatSegmentId(uint64_t value);

     std::mutex &GetKeyMutex(const std::string &key);
     std::shared_ptr<SegmentSnapshot> Snapshot() const;
     void PublishSnapshotLocked();
     bool LoadOrCreateManifest();
     bool OpenManifestSegments();
     std::shared_ptr<SegmentHandle> OpenSegment(const SegmentMetadata &metadata, bool create_if_missing = false);
     bool CreateActiveSegmentLocked();
     bool PersistManifestLocked();
     bool PersistSegmentMetadata(const std::shared_ptr<SegmentHandle> &segment) const;
     bool PersistActiveMetadataIfNeededLocked(bool force);
     bool ActiveSegmentHasCapacityLocked(uint64_t documents, uint64_t bytes) const;
     bool EnsureActiveSegmentCapacity(uint64_t documents, uint64_t bytes);
     bool ShouldRotateActiveLocked() const;
     bool SealActiveSegmentAndCreateNewLocked();
     bool RotateActiveSegmentIfNeeded();
     std::shared_ptr<SegmentHandle> FindSegment(const std::string &segment_id, const std::shared_ptr<SegmentSnapshot> &snapshot) const;
     bool GetSystemValue(const std::string &key, std::string &value) const;
     bool PutDocLocation(const std::string &key, const std::string &segment_id, uint64_t timestamp_ms);
     bool RepairDocLocation(const std::string &key, const std::string &segment_id);
     bool IsTombstoned(const std::string &collection, const std::string &doc_id) const;
};
