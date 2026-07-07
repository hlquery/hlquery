/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include "search/segmentmanifest.h"

#include <filesystem>
#include <fstream>

#include "vendor/json/json.hpp"

namespace
{
     bool FsyncFileBestEffort(const std::string& path)
     {
          (void)path;
          return true;
     }
}

bool SegmentManifest::Load(const std::string& path, SegmentManifest& out_manifest)
{
     std::ifstream input(path);

     if (!input.is_open())
     {
          return false;
     }

     try
     {
          nlohmann::json json_value = nlohmann::json::parse(input);

          out_manifest.Version = json_value.value("version", 1);
          out_manifest.Generation = json_value.value("generation", 0ULL);
          out_manifest.Active = json_value.value("active", std::string());
          out_manifest.Sealed = json_value.value("sealed", std::vector<std::string>());
          out_manifest.Deleted = json_value.value("deleted", std::vector<std::string>());
          out_manifest.CreatedAtMs = json_value.value("created_at_ms", 0ULL);
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentManifest::SaveAtomic(const std::string& path) const
{
     const std::filesystem::path target(path);
     const std::filesystem::path tmp_path = target.parent_path() / "manifest.tmp";

     try
     {
          std::filesystem::create_directories(target.parent_path());

          nlohmann::json json_value;
          json_value["version"] = Version;
          json_value["generation"] = Generation;
          json_value["active"] = Active;
          json_value["sealed"] = Sealed;
          json_value["deleted"] = Deleted;
          json_value["created_at_ms"] = CreatedAtMs;

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

          FsyncFileBestEffort(tmp_path.string());
          std::filesystem::rename(tmp_path, target);
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentMetadata::Load(const std::string& path, SegmentMetadata& out_metadata)
{
     std::ifstream input(path);

     if (!input.is_open())
     {
          return false;
     }

     try
     {
          nlohmann::json json_value = nlohmann::json::parse(input);

          out_metadata.Id = json_value.value("id", std::string());
          out_metadata.State = json_value.value("state", std::string("active"));
          out_metadata.CreatedAtMs = json_value.value("created_at_ms", 0ULL);
          out_metadata.SealedAtMs = json_value.value("sealed_at_ms", 0ULL);
          out_metadata.DocCountEstimate = json_value.value("doc_count_estimate", 0ULL);
          out_metadata.BytesEstimate = json_value.value("bytes_estimate", 0ULL);
          return !out_metadata.Id.empty();
     }
     catch (...)
     {
          return false;
     }
}

bool SegmentMetadata::SaveAtomic(const std::string& path) const
{
     const std::filesystem::path target(path);
     const std::filesystem::path tmp_path = target.parent_path() / "segment.tmp";

     try
     {
          std::filesystem::create_directories(target.parent_path());

          nlohmann::json json_value;
          json_value["id"] = Id;
          json_value["state"] = State;
          json_value["created_at_ms"] = CreatedAtMs;
          json_value["sealed_at_ms"] = SealedAtMs;
          json_value["doc_count_estimate"] = DocCountEstimate;
          json_value["bytes_estimate"] = BytesEstimate;

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

          FsyncFileBestEffort(tmp_path.string());
          std::filesystem::rename(tmp_path, target);
          return true;
     }
     catch (...)
     {
          return false;
     }
}
