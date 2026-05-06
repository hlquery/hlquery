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
#include <string>
#include <vector>

#include <rocksdb/db.h>

#include "sam/sam.h"
#include "vendor/json/json.hpp"

struct SAMSemanticProfile
{
     std::string Subject;
     std::string Summary;
     std::vector<std::string> Aliases;
     std::vector<std::string> Descriptors;
     std::vector<std::string> Queries;
     std::vector<float> Vector;
};

std::string ResolveSamDataDir();
std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID);
std::string BuildCollectionProfileKey(const std::string& Collection);
std::string BuildSearchIdeaPrefix(const std::string& Collection);
std::string BuildCollectionStateKey(const std::string& Collection);
std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID);
std::string BuildSAMSourceDocumentFingerprint(const Document& Doc);
std::string DetectSAMDocumentLabel(const Document& Doc);
std::string DetectSAMDocumentFormat(const Document& Doc);
SAMSemanticProfile BuildSemanticProfile(const std::string& Title,
                                        const std::vector<SAM::TermEntry>& Terms);
void StoreSemanticProfileJSON(nlohmann::json& Manifest, const SAMSemanticProfile& Profile);
bool RebuildCollectionProfileLocked(rocksdb::DB* Database,
                                    const std::string& Collection,
                                    std::string* ErrorMessage = nullptr);
bool WriteCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 uint64_t Version,
                                                 std::string* ErrorMessage = nullptr);
