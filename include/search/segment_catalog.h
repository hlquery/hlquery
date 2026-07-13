/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SegmentManifest
{
     int Version = 1;
     uint64_t Generation = 0;
     std::string Active;
     std::vector<std::string> Sealed;
     std::vector<std::string> Deleted;
     uint64_t CreatedAtMs = 0;

     static bool Load(const std::string &path, SegmentManifest &out_manifest);
     bool SaveAtomic(const std::string &path) const;
};

struct SegmentMetadata
{
     std::string Id;
     std::string State = "active";
     uint64_t CreatedAtMs = 0;
     uint64_t SealedAtMs = 0;
     uint64_t DocCountEstimate = 0;
     uint64_t BytesEstimate = 0;

     static bool Load(const std::string &path, SegmentMetadata &out_metadata);
     bool SaveAtomic(const std::string &path) const;
};
