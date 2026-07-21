/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include "search/segmented_document_router.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>

#include "utils/wildcard.h"

namespace
{
constexpr const char *DocLocationPrefix = "__hlq_docloc:";
constexpr const char *TombstonePrefix = "__hlq_tombstone:";
constexpr const char *ManifestGenerationKey = "__hlq_segment_manifest_generation";

std::string DocKeyFromLocationKey(const std::string &loc_key)
{
     const std::string prefix(DocLocationPrefix);

     if (!loc_key.starts_with(prefix))
     {
          return "";
     }

     return "doc:" + loc_key.substr(prefix.size());
}

std::string LocationPrefixFromDocPrefix(const std::string &doc_prefix)
{
     if (!doc_prefix.starts_with("doc:"))
     {
          return "";
     }

     return std::string(DocLocationPrefix) + doc_prefix.substr(4);
}

uint64_t SegmentOrdinal(const std::string &segment_id)
{
     constexpr const char *prefix = "seg_";

     if (!segment_id.starts_with(prefix))
     {
          return 0;
     }

     try
     {
          return static_cast<uint64_t>(std::stoull(segment_id.substr(4)));
     }
     catch (...)
     {
          return 0;
     }
}
} // namespace

SegmentManager::SegmentManager(const std::string &data_dir,
                               rocksdb::DB *system_db,
                               const rocksdb::Options &options,
                               const rocksdb::WriteOptions &write_options,
                               const RocksDBOptions &storage_options)
    : DataDir(data_dir),
      StorageDir(data_dir + "/storage"),
      SegmentsDir(data_dir + "/storage/segments"),
      ManifestPath(data_dir + "/storage/manifest.json"),
      SystemDB(system_db),
      OptionsValue(options),
      WriteOptionsValue(write_options),
      StorageOptions(storage_options)
{
}

bool SegmentManager::Initialize()
{
     if (!SystemDB)
     {
          return false;
     }

     try
     {
          std::filesystem::create_directories(SegmentsDir);
     }
     catch (...)
     {
          return false;
     }

     bool rebuild_docloc = false;

     {
          std::lock_guard<std::mutex> lock(SegmentMutex);

          if (!LoadOrCreateManifest())
          {
               return false;
          }

          if (!OpenManifestSegments())
          {
               return false;
          }

          if (!ActiveSegment)
          {
               if (!CreateActiveSegmentLocked())
               {
                    return false;
               }
          }

          PublishSnapshotLocked();
     }

     std::string existing_generation;
     if (!GetSystemValue(ManifestGenerationKey, existing_generation) ||
         existing_generation != std::to_string(Manifest.Generation))
     {
          rebuild_docloc = true;
     }

     if (rebuild_docloc)
     {
          if (!RebuildDocLocationMap())
          {
               return false;
          }
     }

     return true;
}

void SegmentManager::Shutdown()
{
     std::lock_guard<std::mutex> lock(SegmentMutex);
     CurrentSnapshot.reset();
     ActiveSegment.reset();
     SealedSegments.clear();
}

bool SegmentManager::IsDocKey(const std::string &key)
{
     return key.starts_with("doc:");
}

bool SegmentManager::ParseDocKey(const std::string &key, std::string &collection, std::string &doc_id)
{
     if (!IsDocKey(key))
     {
          return false;
     }

     const size_t first_sep = key.find(':', 4);

     if (first_sep == std::string::npos || first_sep <= 4 || first_sep + 1 >= key.size())
     {
          return false;
     }

     collection = key.substr(4, first_sep - 4);
     doc_id = key.substr(first_sep + 1);
     return true;
}

std::string SegmentManager::DocLocationKey(const std::string &collection, const std::string &doc_id)
{
     return std::string(DocLocationPrefix) + collection + ":" + doc_id;
}

std::string SegmentManager::TombstoneKey(const std::string &collection, const std::string &doc_id)
{
     return std::string(TombstonePrefix) + collection + ":" + doc_id;
}

uint64_t SegmentManager::NowMs()
{
     const auto now = std::chrono::system_clock::now().time_since_epoch();
     return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t SegmentManager::DirectorySize(const std::string &path, uint64_t *sst_files)
{
     uint64_t total = 0;

     if (sst_files)
     {
          *sst_files = 0;
     }

     try
     {
          if (!std::filesystem::exists(path))
          {
               return 0;
          }

          for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
          {
               if (!entry.is_regular_file())
               {
                    continue;
               }

               total += entry.file_size();

               if (sst_files && entry.path().extension() == ".sst")
               {
                    (*sst_files)++;
               }
          }
     }
     catch (...)
     {
     }

     return total;
}

std::string SegmentManager::FormatSegmentId(uint64_t value)
{
     std::ostringstream stream;
     stream << "seg_" << std::setw(12) << std::setfill('0') << value;
     return stream.str();
}

std::mutex &SegmentManager::GetKeyMutex(const std::string &key)
{
     return KeyMutexes[std::hash<std::string>{}(key) % KeyMutexes.size()];
}

std::shared_ptr<SegmentManager::SegmentSnapshot> SegmentManager::Snapshot() const
{
     std::lock_guard<std::mutex> lock(SegmentMutex);
     return CurrentSnapshot;
}

void SegmentManager::PublishSnapshotLocked()
{
     auto snapshot = std::make_shared<SegmentSnapshot>();
     snapshot->Generation = Manifest.Generation;
     snapshot->ActiveId = ActiveSegment ? ActiveSegment->Metadata.Id : "";

     if (ActiveSegment)
     {
          snapshot->LiveNewestFirst.push_back(ActiveSegment);
     }

     for (auto it = SealedSegments.rbegin(); it != SealedSegments.rend(); ++it)
     {
          snapshot->LiveNewestFirst.push_back(*it);
     }

     CurrentSnapshot = snapshot;
}

bool SegmentManager::LoadOrCreateManifest()
{
     if (SegmentManifest::Load(ManifestPath, Manifest))
     {
          return true;
     }

     std::vector<SegmentMetadata> discovered;

     try
     {
          if (std::filesystem::exists(SegmentsDir))
          {
               for (const auto &entry : std::filesystem::directory_iterator(SegmentsDir))
               {
                    if (!entry.is_directory())
                    {
                         continue;
                    }

                    SegmentMetadata metadata;
                    if (!SegmentMetadata::Load((entry.path() / "segment.json").string(), metadata))
                    {
                         return false;
                    }

                    if (metadata.State != "deleted")
                    {
                         discovered.push_back(metadata);
                    }
               }
          }
     }
     catch (...)
     {
          return false;
     }

     std::sort(discovered.begin(), discovered.end(), [](const SegmentMetadata &left, const SegmentMetadata &right)
               {
                    return left.Id < right.Id;
               });

     Manifest = SegmentManifest();
     Manifest.CreatedAtMs = NowMs();
     Manifest.Generation = 0;

     for (const auto &metadata : discovered)
     {
          Manifest.Generation = std::max(Manifest.Generation, SegmentOrdinal(metadata.Id));

          if (metadata.State == "active")
          {
               if (!Manifest.Active.empty())
               {
                    Manifest.Sealed.push_back(Manifest.Active);
               }

               Manifest.Active = metadata.Id;
          }
          else
          {
               Manifest.Sealed.push_back(metadata.Id);
          }
     }

     if (Manifest.Active.empty() && !Manifest.Sealed.empty())
     {
          Manifest.Active = Manifest.Sealed.back();
          Manifest.Sealed.pop_back();
     }

     return PersistManifestLocked();
}

std::shared_ptr<SegmentManager::SegmentHandle> SegmentManager::OpenSegment(const SegmentMetadata &metadata, bool create_if_missing)
{
     auto handle = std::make_shared<SegmentHandle>();
     handle->Metadata = metadata;
     handle->Directory = SegmentsDir + "/" + metadata.Id;
     handle->RocksDBPath = handle->Directory + "/rocksdb";

     if (create_if_missing)
     {
          try
          {
               std::filesystem::create_directories(handle->RocksDBPath);
          }
          catch (...)
          {
               return nullptr;
          }
     }
     else
     {
          std::error_code error;
          if (!std::filesystem::is_directory(handle->RocksDBPath, error) || error)
          {
               return nullptr;
          }
     }

     rocksdb::Options open_options = OptionsValue;
     open_options.create_if_missing = create_if_missing;
     std::unique_ptr<rocksdb::DB> db_ptr;
     rocksdb::Status status = rocksdb::DB::Open(open_options, handle->RocksDBPath, &db_ptr);

     if (!status.ok() || !db_ptr)
     {
          return nullptr;
     }

     handle->DB = std::shared_ptr<rocksdb::DB>(db_ptr.release());
     return handle;
}

bool SegmentManager::OpenManifestSegments()
{
     ActiveSegment.reset();
     SealedSegments.clear();

     for (const auto &segment_id : Manifest.Sealed)
     {
          SegmentMetadata metadata;
          if (!SegmentMetadata::Load(SegmentsDir + "/" + segment_id + "/segment.json", metadata) ||
              metadata.Id != segment_id)
          {
               return false;
          }

          if (metadata.State != "sealed")
          {
               metadata.State = "sealed";
               if (metadata.SealedAtMs == 0)
               {
                    metadata.SealedAtMs = NowMs();
               }

               if (!metadata.SaveAtomic(SegmentsDir + "/" + segment_id + "/segment.json"))
               {
                    return false;
               }
          }

          auto handle = OpenSegment(metadata);
          if (!handle)
          {
               return false;
          }

          SealedSegments.push_back(handle);
     }

     if (!Manifest.Active.empty())
     {
          SegmentMetadata metadata;
          if (!SegmentMetadata::Load(SegmentsDir + "/" + Manifest.Active + "/segment.json", metadata) ||
              metadata.Id != Manifest.Active)
          {
               return false;
          }

          if (metadata.State != "active")
          {
               metadata.State = "active";
               metadata.SealedAtMs = 0;

               if (!metadata.SaveAtomic(SegmentsDir + "/" + Manifest.Active + "/segment.json"))
               {
                    return false;
               }
          }

          ActiveSegment = OpenSegment(metadata);
          if (!ActiveSegment)
          {
               return false;
          }
     }

     return true;
}

bool SegmentManager::CreateActiveSegmentLocked()
{
     const SegmentManifest previous_manifest = Manifest;
     const std::shared_ptr<SegmentHandle> previous_active = ActiveSegment;
     uint64_t next_id = Manifest.Generation + 1;
     std::set<std::string> used(Manifest.Sealed.begin(), Manifest.Sealed.end());

     if (!Manifest.Active.empty())
     {
          used.insert(Manifest.Active);
     }

     std::string segment_id;
     do
     {
          segment_id = FormatSegmentId(next_id++);
     } while (used.count(segment_id) > 0);

     SegmentMetadata metadata;
     metadata.Id = segment_id;
     metadata.State = "active";
     metadata.CreatedAtMs = NowMs();

     const std::string segment_dir = SegmentsDir + "/" + segment_id;

     try
     {
          std::filesystem::create_directories(segment_dir + "/rocksdb");
     }
     catch (...)
     {
          return false;
     }

     if (!metadata.SaveAtomic(segment_dir + "/segment.json"))
     {
          return false;
     }

     auto handle = OpenSegment(metadata, true);
     if (!handle)
     {
          return false;
     }

     ActiveSegment = handle;
     Manifest.Active = segment_id;
     Manifest.Generation = SegmentOrdinal(segment_id);

     if (!PersistManifestLocked())
     {
          ActiveSegment = previous_active;
          Manifest = previous_manifest;
          return false;
     }

     return true;
}

bool SegmentManager::PersistManifestLocked()
{
     /* Write the rebuild marker first. If the manifest commit does not follow,
      * startup observes a generation mismatch and rebuilds document locations.
      */

     rocksdb::Status status = SystemDB->Put(WriteOptionsValue, ManifestGenerationKey, std::to_string(Manifest.Generation));
     if (!status.ok())
     {
          return false;
     }

     return Manifest.SaveAtomic(ManifestPath);
}

bool SegmentManager::PersistSegmentMetadata(const std::shared_ptr<SegmentHandle> &segment) const
{
     if (!segment)
     {
          return false;
     }

     return segment->Metadata.SaveAtomic(segment->Directory + "/segment.json");
}

bool SegmentManager::ShouldRotateActiveLocked() const
{
     if (!ActiveSegment)
     {
          return true;
     }

     if (StorageOptions.SegmentMaxDocs > 0 &&
         ActiveSegment->Metadata.DocCountEstimate >= StorageOptions.SegmentMaxDocs)
     {
          return true;
     }

     const uint64_t bytes = std::max<uint64_t>(DirectorySize(ActiveSegment->Directory),
                                               ActiveSegment->Metadata.BytesEstimate);
     if (StorageOptions.SegmentMaxBytes > 0 && bytes >= StorageOptions.SegmentMaxBytes)
     {
          return true;
     }

     return false;
}

bool SegmentManager::SealActiveSegmentAndCreateNewLocked()
{
     if (!ActiveSegment)
     {
          return CreateActiveSegmentLocked();
     }

     if (!ActiveSegment->DB->Flush(rocksdb::FlushOptions()).ok() ||
         !ActiveSegment->DB->SyncWAL().ok())
     {
          return false;
     }

     const SegmentManifest previous_manifest = Manifest;
     const SegmentMetadata previous_metadata = ActiveSegment->Metadata;
     const std::shared_ptr<SegmentHandle> previous_active = ActiveSegment;
     const size_t previous_sealed_count = SealedSegments.size();

     ActiveSegment->Metadata.State = "sealed";
     ActiveSegment->Metadata.SealedAtMs = NowMs();
     ActiveSegment->Metadata.BytesEstimate = DirectorySize(ActiveSegment->Directory);

     if (!PersistSegmentMetadata(ActiveSegment))
     {
          ActiveSegment->Metadata = previous_metadata;
          return false;
     }

     Manifest.Sealed.push_back(ActiveSegment->Metadata.Id);
     Manifest.Active.clear();
     SealedSegments.push_back(ActiveSegment);
     ActiveSegment.reset();

     if (!CreateActiveSegmentLocked())
     {
          Manifest = previous_manifest;
          ActiveSegment = previous_active;
          SealedSegments.resize(previous_sealed_count);
          ActiveSegment->Metadata = previous_metadata;
          PersistSegmentMetadata(ActiveSegment);
          return false;
     }

     PublishSnapshotLocked();
     return true;
}

std::shared_ptr<SegmentManager::SegmentHandle> SegmentManager::FindSegment(const std::string &segment_id, const std::shared_ptr<SegmentSnapshot> &snapshot) const
{
     if (!snapshot)
     {
          return nullptr;
     }

     for (const auto &segment : snapshot->LiveNewestFirst)
     {
          if (segment && segment->Metadata.Id == segment_id)
          {
               return segment;
          }
     }

     return nullptr;
}

bool SegmentManager::GetSystemValue(const std::string &key, std::string &value) const
{
     if (!SystemDB)
     {
          return false;
     }

     rocksdb::Status status = SystemDB->Get(rocksdb::ReadOptions(), key, &value);
     return status.ok();
}

bool SegmentManager::PutDocLocation(const std::string &key, const std::string &segment_id, uint64_t timestamp_ms)
{
     std::string collection;
     std::string doc_id;

     if (!ParseDocKey(key, collection, doc_id))
     {
          return false;
     }

     rocksdb::WriteBatch batch;
     batch.Put(DocLocationKey(collection, doc_id), segment_id + ":" + std::to_string(timestamp_ms));
     batch.Delete(TombstoneKey(collection, doc_id));

     return SystemDB->Write(WriteOptionsValue, &batch).ok();
}

bool SegmentManager::RepairDocLocation(const std::string &key, const std::string &segment_id)
{
     return PutDocLocation(key, segment_id, NowMs());
}

bool SegmentManager::IsTombstoned(const std::string &collection, const std::string &doc_id) const
{
     std::string tombstone;
     return GetSystemValue(TombstoneKey(collection, doc_id), tombstone);
}

bool SegmentManager::SetDocument(const std::string &key, const std::string &value)
{
     std::string collection;
     std::string doc_id;

     if (!ParseDocKey(key, collection, doc_id))
     {
          return false;
     }

     std::lock_guard<std::mutex> key_lock(GetKeyMutex(key));
     std::lock_guard<std::mutex> write_lock(WriteMutex);
     std::shared_ptr<SegmentHandle> active;

     {
          std::lock_guard<std::mutex> lock(SegmentMutex);

          if (!ActiveSegment && !CreateActiveSegmentLocked())
          {
               return false;
          }

          active = ActiveSegment;
     }

     if (!active || !active->DB)
     {
          return false;
     }

     rocksdb::Status status = active->DB->Put(WriteOptionsValue, key, value);

     if (!status.ok())
     {
          return false;
     }

     if (!PutDocLocation(key, active->Metadata.Id, NowMs()))
     {
          return false;
     }

     {
          std::lock_guard<std::mutex> lock(SegmentMutex);
          if (ActiveSegment && ActiveSegment->Metadata.Id == active->Metadata.Id)
          {
               ActiveSegment->Metadata.DocCountEstimate++;
               ActiveSegment->Metadata.BytesEstimate += key.size() + value.size();
               if (!PersistSegmentMetadata(ActiveSegment))
               {
                    return false;
               }

               if (ShouldRotateActiveLocked())
               {
                    if (!SealActiveSegmentAndCreateNewLocked())
                    {
                         return false;
                    }
               }
               else
               {
                    PublishSnapshotLocked();
               }
          }
     }

     return true;
}

size_t SegmentManager::BatchSetDocuments(const std::vector<std::pair<std::string, std::string>> &key_values)
{
     if (key_values.empty())
     {
          return 0;
     }

     std::lock_guard<std::mutex> write_lock(WriteMutex);
     std::shared_ptr<SegmentHandle> active;

     {
          std::lock_guard<std::mutex> lock(SegmentMutex);

          if (!ActiveSegment && !CreateActiveSegmentLocked())
          {
               return 0;
          }

          active = ActiveSegment;
     }

     rocksdb::WriteBatch segment_batch;
     uint64_t bytes = 0;

     for (const auto &kv : key_values)
     {
          if (!IsDocKey(kv.first))
          {
               return 0;
          }

          segment_batch.Put(kv.first, kv.second);
          bytes += kv.first.size() + kv.second.size();
     }

     if (!active || !active->DB || !active->DB->Write(WriteOptionsValue, &segment_batch).ok())
     {
          return 0;
     }

     rocksdb::WriteBatch system_batch;
     const uint64_t timestamp_ms = NowMs();

     for (const auto &kv : key_values)
     {
          std::string collection;
          std::string doc_id;

          if (!ParseDocKey(kv.first, collection, doc_id))
          {
               return 0;
          }

          system_batch.Put(DocLocationKey(collection, doc_id), active->Metadata.Id + ":" + std::to_string(timestamp_ms));
          system_batch.Delete(TombstoneKey(collection, doc_id));
     }

     if (!SystemDB->Write(WriteOptionsValue, &system_batch).ok())
     {
          return 0;
     }

     {
          std::lock_guard<std::mutex> lock(SegmentMutex);
          if (ActiveSegment && ActiveSegment->Metadata.Id == active->Metadata.Id)
          {
               ActiveSegment->Metadata.DocCountEstimate += key_values.size();
               ActiveSegment->Metadata.BytesEstimate += bytes;
               if (!PersistSegmentMetadata(ActiveSegment))
               {
                    return 0;
               }

               if (ShouldRotateActiveLocked())
               {
                    if (!SealActiveSegmentAndCreateNewLocked())
                    {
                         return 0;
                    }
               }
               else
               {
                    PublishSnapshotLocked();
               }
          }
     }

     return key_values.size();
}

std::string SegmentManager::GetDocument(const std::string &key)
{
     std::string collection;
     std::string doc_id;

     if (!ParseDocKey(key, collection, doc_id) || IsTombstoned(collection, doc_id))
     {
          return "";
     }

     auto snapshot = Snapshot();
     std::string docloc;

     if (GetSystemValue(DocLocationKey(collection, doc_id), docloc))
     {
          const size_t sep = docloc.find(':');
          const std::string segment_id = sep == std::string::npos ? docloc : docloc.substr(0, sep);
          auto segment = FindSegment(segment_id, snapshot);

          if (segment && segment->DB)
          {
               std::string value;
               if (segment->DB->Get(rocksdb::ReadOptions(), key, &value).ok())
               {
                    return value;
               }
          }
     }

     if (!snapshot)
     {
          return "";
     }

     for (const auto &segment : snapshot->LiveNewestFirst)
     {
          if (!segment || !segment->DB)
          {
               continue;
          }

          std::string value;
          if (segment->DB->Get(rocksdb::ReadOptions(), key, &value).ok())
          {
               RepairDocLocation(key, segment->Metadata.Id);
               return value;
          }
     }

     return "";
}

int SegmentManager::DeleteDocument(const std::string &key)
{
     std::string collection;
     std::string doc_id;

     if (!ParseDocKey(key, collection, doc_id))
     {
          return 0;
     }

     std::lock_guard<std::mutex> key_lock(GetKeyMutex(key));
     std::lock_guard<std::mutex> write_lock(WriteMutex);

     rocksdb::WriteBatch batch;
     batch.Put(TombstoneKey(collection, doc_id), std::to_string(NowMs()));
     batch.Delete(DocLocationKey(collection, doc_id));

     if (!SystemDB->Write(WriteOptionsValue, &batch).ok())
     {
          return 0;
     }

     std::shared_ptr<SegmentHandle> active;
     {
          std::lock_guard<std::mutex> lock(SegmentMutex);
          active = ActiveSegment;
     }

     if (active && active->DB)
     {
          active->DB->Delete(WriteOptionsValue, key);
     }

     return 1;
}

size_t SegmentManager::DeleteDocumentRange(const std::string &start_key, const std::string &end_key)
{
     if (!start_key.starts_with("doc:"))
     {
          return 0;
     }

     std::lock_guard<std::mutex> write_lock(WriteMutex);
     const std::string loc_prefix = LocationPrefixFromDocPrefix(start_key);
     rocksdb::WriteBatch batch;
     size_t deleted = 0;

     std::unique_ptr<rocksdb::Iterator> it(SystemDB->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(loc_prefix); it->Valid() && it->key().starts_with(loc_prefix); it->Next())
     {
          const std::string doc_key = DocKeyFromLocationKey(it->key().ToString());

          if (doc_key.empty() || doc_key < start_key || doc_key >= end_key)
          {
               continue;
          }

          std::string collection;
          std::string doc_id;
          if (!ParseDocKey(doc_key, collection, doc_id))
          {
               continue;
          }

          batch.Put(TombstoneKey(collection, doc_id), std::to_string(NowMs()));
          batch.Delete(DocLocationKey(collection, doc_id));
          deleted++;
     }

     if (deleted == 0)
     {
          return 0;
     }

     return SystemDB->Write(WriteOptionsValue, &batch).ok() ? 1 : 0;
}

bool SegmentManager::ExistsDocument(const std::string &key)
{
     return !GetDocument(key).empty();
}

std::vector<std::string> SegmentManager::Keys(const std::string &pattern)
{
     std::vector<std::string> result;
     const std::string loc_prefix(DocLocationPrefix);
     std::unique_ptr<rocksdb::Iterator> it(SystemDB->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(loc_prefix); it->Valid() && it->key().starts_with(loc_prefix); it->Next())
     {
          const std::string doc_key = DocKeyFromLocationKey(it->key().ToString());
          std::string collection;
          std::string doc_id;

          if (!ParseDocKey(doc_key, collection, doc_id) || IsTombstoned(collection, doc_id))
          {
               continue;
          }

          if (Wildcard::Match(doc_key, pattern))
          {
               result.push_back(doc_key);
          }
     }

     return result;
}

size_t SegmentManager::CountKeys(const std::string &prefix)
{
     return PrefixKeys(prefix, 0, 0).size();
}

std::vector<std::string> SegmentManager::PrefixKeys(const std::string &prefix, size_t offset, size_t limit)
{
     std::vector<std::string> keys;
     const std::string loc_prefix = LocationPrefixFromDocPrefix(prefix);

     if (loc_prefix.empty())
     {
          return keys;
     }

     size_t skipped = 0;
     std::unique_ptr<rocksdb::Iterator> it(SystemDB->NewIterator(rocksdb::ReadOptions()));

     for (it->Seek(loc_prefix); it->Valid() && it->key().starts_with(loc_prefix); it->Next())
     {
          const std::string doc_key = DocKeyFromLocationKey(it->key().ToString());
          std::string collection;
          std::string doc_id;

          if (!ParseDocKey(doc_key, collection, doc_id) || IsTombstoned(collection, doc_id))
          {
               continue;
          }

          if (skipped < offset)
          {
               skipped++;
               continue;
          }

          keys.push_back(doc_key);

          if (limit > 0 && keys.size() >= limit)
          {
               break;
          }
     }

     return keys;
}

bool SegmentManager::ForEachPrefixKeySnapshot(const std::string &prefix, size_t limit, const std::function<bool(const std::string &)> &callback)
{
     if (!callback)
     {
          return false;
     }

     const auto keys = PrefixKeys(prefix, 0, limit);

     for (const auto &key : keys)
     {
          if (!callback(key))
          {
               return false;
          }
     }

     return true;
}

size_t SegmentManager::GetPrefixTotalSize(const std::string &prefix)
{
     size_t total = 0;
     const auto keys = PrefixKeys(prefix, 0, 0);

     for (const auto &key : keys)
     {
          total += GetDocument(key).size();
     }

     return total;
}

bool SegmentManager::Flush()
{
     auto snapshot = Snapshot();
     bool ok = true;

     if (snapshot)
     {
          for (const auto &segment : snapshot->LiveNewestFirst)
          {
               if (segment && segment->DB)
               {
                    ok = segment->DB->Flush(rocksdb::FlushOptions()).ok() && ok;
               }
          }
     }

     return ok;
}

bool SegmentManager::FlushAndSync()
{
     auto snapshot = Snapshot();
     bool ok = true;

     if (snapshot)
     {
          for (const auto &segment : snapshot->LiveNewestFirst)
          {
               if (segment && segment->DB)
               {
                    ok = segment->DB->Flush(rocksdb::FlushOptions()).ok() && ok;
                    ok = segment->DB->SyncWAL().ok() && ok;
               }
          }
     }

     return ok;
}

bool SegmentManager::SyncWAL()
{
     auto snapshot = Snapshot();
     bool ok = true;

     if (snapshot)
     {
          for (const auto &segment : snapshot->LiveNewestFirst)
          {
               if (segment && segment->DB)
               {
                    ok = segment->DB->SyncWAL().ok() && ok;
               }
          }
     }

     return ok;
}

void SegmentManager::Compact()
{
     auto snapshot = Snapshot();

     if (!snapshot)
     {
          return;
     }

     for (const auto &segment : snapshot->LiveNewestFirst)
     {
          if (segment && segment->DB)
          {
               segment->DB->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
          }
     }
}

bool SegmentManager::RebuildDocLocationMap()
{
     auto snapshot = Snapshot();

     if (!snapshot)
     {
          return false;
     }

     rocksdb::WriteBatch batch;
     std::set<std::string> seen;

     for (const auto &segment : snapshot->LiveNewestFirst)
     {
          if (!segment || !segment->DB)
          {
               continue;
          }

          std::unique_ptr<rocksdb::Iterator> it(segment->DB->NewIterator(rocksdb::ReadOptions()));

          for (it->Seek("doc:"); it->Valid() && it->key().starts_with("doc:"); it->Next())
          {
               const std::string doc_key = it->key().ToString();

               if (seen.count(doc_key) > 0)
               {
                    continue;
               }

               seen.insert(doc_key);

               std::string collection;
               std::string doc_id;
               if (!ParseDocKey(doc_key, collection, doc_id) || IsTombstoned(collection, doc_id))
               {
                    continue;
               }

               batch.Put(DocLocationKey(collection, doc_id), segment->Metadata.Id + ":" + std::to_string(NowMs()));
          }
     }

     batch.Put(ManifestGenerationKey, std::to_string(snapshot->Generation));
     return SystemDB->Write(WriteOptionsValue, &batch).ok();
}

SegmentManager::SegmentStats SegmentManager::GetStats() const
{
     SegmentStats stats;
     auto snapshot = Snapshot();

     if (snapshot)
     {
          stats.ManifestGeneration = snapshot->Generation;
          stats.ActiveSegmentId = snapshot->ActiveId;

          for (const auto &segment : snapshot->LiveNewestFirst)
          {
               if (!segment)
               {
                    continue;
               }

               uint64_t sst_files = 0;
               stats.TotalBytes += DirectorySize(segment->Directory, &sst_files);
               stats.NumSstFiles += sst_files;

               if (segment->Metadata.State == "sealed")
               {
                    stats.SealedSegmentCount++;
               }
          }
     }

     try
     {
          std::unique_ptr<rocksdb::Iterator> it(SystemDB->NewIterator(rocksdb::ReadOptions()));

          for (it->Seek(TombstonePrefix); it->Valid() && it->key().starts_with(TombstonePrefix); it->Next())
          {
               stats.TombstoneCountEstimate++;
          }
     }
     catch (...)
     {
     }

     return stats;
}
