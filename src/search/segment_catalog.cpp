/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <utility>

#include "search/segment_catalog.h"
#include "vendor/json/json.hpp"

bool FsyncPath(const std::filesystem::path &path, bool directory)
{
     int flags = O_RDONLY;

#ifdef O_DIRECTORY

     if (directory)
     {
          flags |= O_DIRECTORY;
     }

#else

     (void)directory;

#endif

     const int fd = ::open(path.c_str(), flags);
     if (fd < 0)
     {
          return false;
     }

     bool ok = false;
     do
     {
          ok = (::fsync(fd) == 0);
     } while (!ok && errno == EINTR);

     const int close_result = ::close(fd);
     return ok && close_result == 0;
}

bool SaveJsonAtomic(const std::string &path, const nlohmann::json &json_value)
{
     const std::filesystem::path target(path);
     std::filesystem::path tmp_path(target);
     tmp_path += ".tmp";

     try
     {
          if (!target.parent_path().empty())
          {
               std::filesystem::create_directories(target.parent_path());
          }

          {
               std::ofstream output(tmp_path, std::ios::trunc);

               if (!output.is_open())
               {
                    return false;
               }

               output << json_value.dump(2) << '\n';
               output.flush();

               if (!output.good())
               {
                    return false;
               }
          }

          if (!FsyncPath(tmp_path, false))
          {
               return false;
          }

          std::filesystem::rename(tmp_path, target);

          const std::filesystem::path parent = target.parent_path().empty()
                                                    ? std::filesystem::path(".")
                                                    : target.parent_path();
          return FsyncPath(parent, true);
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentManifest::Load(const std::string &path, SegmentManifest &out_manifest)
{
     std::ifstream input(path);

     if (!input.is_open())
     {
          return false;
     }

     try
     {
          nlohmann::json json_value = nlohmann::json::parse(input);

          SegmentManifest loaded_manifest;
          loaded_manifest.Version = json_value.value("version", 1);
          loaded_manifest.Generation = json_value.value("generation", 0ULL);
          loaded_manifest.Active = json_value.value("active", std::string());
          loaded_manifest.Sealed = json_value.value("sealed", std::vector<std::string>());
          loaded_manifest.Deleted = json_value.value("deleted", std::vector<std::string>());
          loaded_manifest.CreatedAtMs = json_value.value("created_at_ms", 0ULL);
          out_manifest = std::move(loaded_manifest);
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentManifest::SaveAtomic(const std::string &path) const
{
     nlohmann::json json_value;
     json_value["version"] = Version;
     json_value["generation"] = Generation;
     json_value["active"] = Active;
     json_value["sealed"] = Sealed;
     json_value["deleted"] = Deleted;
     json_value["created_at_ms"] = CreatedAtMs;

     return SaveJsonAtomic(path, json_value);
}

bool SegmentMetadata::Load(const std::string &path, SegmentMetadata &out_metadata)
{
     std::ifstream input(path);

     if (!input.is_open())
     {
          return false;
     }

     try
     {
          nlohmann::json json_value = nlohmann::json::parse(input);

          SegmentMetadata loaded_metadata;
          loaded_metadata.Id = json_value.value("id", std::string());
          loaded_metadata.State = json_value.value("state", std::string("active"));
          loaded_metadata.CreatedAtMs = json_value.value("created_at_ms", 0ULL);
          loaded_metadata.SealedAtMs = json_value.value("sealed_at_ms", 0ULL);
          loaded_metadata.DocCountEstimate = json_value.value("doc_count_estimate", 0ULL);
          loaded_metadata.BytesEstimate = json_value.value("bytes_estimate", 0ULL);

          if (loaded_metadata.Id.empty())
          {
               return false;
          }

          out_metadata = std::move(loaded_metadata);
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentMetadata::SaveAtomic(const std::string &path) const
{
     if (Id.empty())
     {
          return false;
     }

     nlohmann::json json_value;
     json_value["id"] = Id;
     json_value["state"] = State;
     json_value["created_at_ms"] = CreatedAtMs;
     json_value["sealed_at_ms"] = SealedAtMs;
     json_value["doc_count_estimate"] = DocCountEstimate;
     json_value["bytes_estimate"] = BytesEstimate;

     return SaveJsonAtomic(path, json_value);
}
