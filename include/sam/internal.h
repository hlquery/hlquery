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

/*
 * CODE STYLE AND FORMATTING RULES
 *
 * Use block comments only. Never use line comments.
 * Put opening braces on their own line for functions, loops, conditionals, classes, structs, and namespaces.
 * Indent with spaces only, using exactly five spaces per nested block level.
 * Keep system includes before local includes.
 * Use PascalCase for function names and avoid trailing underscores in variable names.
 * Keep log messages on one physical line and end each message with a period.
 * Do not add comments to namespace closing braces.
 * Place a blank line between a comment and the following code element.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rocksdb/db.h>

#include "sam/sam.h"
#include "vendor/json/json.hpp"

/* Semantic profile persisted for one SAM term/document relationship. */

struct SAMSemanticProfile
{
     std::string Subject;
     std::string Summary;
     std::vector<std::string> Aliases;
     std::vector<std::string> Descriptors;
     std::vector<std::string> Queries;
     std::vector<float> Vector;
};

/* Text payload and semantic category used while scanning profile records. */

struct SAMSemanticIndexEntry
{
     std::string Text;
     std::string Kind;
};

/* Persisted rebuild and mutation tracking state for one collection. */

struct SAMCollectionState
{
     uint64_t IndexedMutationVersion = 0;
     bool HasIndexedMutationVersion = false;
     bool RebuildRequested = false;
     uint64_t RequestedMutationVersion = 0;
};

/* Adaptive lookup calibration derived from recent SAM evaluation samples. */

struct SAMEvaluationCalibration
{
     size_t Samples = 0;
     double EmptyRatio = 0.0;
     double FallbackRatio = 0.0;
     double GraphAssistRatio = 0.0;
     double SemanticAssistRatio = 0.0;
     double HybridRatio = 0.0;
     double AvgTopScore = 0.0;
     size_t AdaptiveVariantBudget = 6;
     size_t AdaptiveGraphBudget = 4;
     size_t AdaptiveSemanticBudget = 8;
};

std::string ResolveSamDataDir();
std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID);
std::string BuildCollectionProfileKey(const std::string& Collection);
std::string BuildSearchIdeaPrefix(const std::string& Collection);
std::string BuildCollectionStateKey(const std::string& Collection);
std::string BuildIntentGraphKey(const std::string& Collection);
std::string BuildLexicalMirrorKey(const std::string& Kind, const std::string& Collection);
std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID);
std::string BuildSemanticProfileKey(const std::string& Term,
                                    const std::string& Collection,
                                    const std::string& DocumentID,
                                    const std::string& Kind);
std::string BuildSemanticProfilePrefix(const std::string& Term, const std::string& Collection);
std::string BuildSAMSourceDocumentFingerprint(const Document& Doc);
std::string DetectSAMDocumentLabel(const Document& Doc);
std::string DetectSAMDocumentFormat(const Document& Doc);
bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry);
SAMSemanticProfile BuildSemanticProfile(const std::string& Title,
                                        const std::vector<SAM::TermEntry>& Terms);
std::vector<SAMSemanticIndexEntry> BuildSemanticIndexEntries(const SAMSemanticProfile& Profile,
                                                            size_t Limit = 32);
void StoreSemanticProfileJSON(nlohmann::json& Manifest, const SAMSemanticProfile& Profile);
bool RebuildCollectionProfileLocked(rocksdb::DB* Database,
                                    const std::string& Collection,
                                    std::string* ErrorMessage = nullptr);
bool WriteCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 uint64_t Version,
                                                 std::string* ErrorMessage = nullptr);
bool ReadCollectionStateLocked(rocksdb::DB* Database,
                               const std::string& Collection,
                               SAMCollectionState& State,
                               nlohmann::json* RootOut = nullptr,
                               std::string* ErrorMessage = nullptr);
bool WriteCollectionStateLocked(rocksdb::DB* Database,
                                const std::string& Collection,
                                const SAMCollectionState& State,
                                std::string* ErrorMessage = nullptr);
bool RebuildIntentGraphLocked(rocksdb::DB* Database,
                              const std::string& Collection,
                              std::string* ErrorMessage = nullptr);
std::vector<std::string> BuildIntentGraphVariants(rocksdb::DB* Database,
                                                  const std::string& Collection,
                                                  const std::string& Query,
                                                  size_t MaxVariants = 8);
std::vector<SAM::LookupHit> BuildIntentGraphHits(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 const std::string& Query,
                                                 size_t Limit = 8);
bool CaptureLookupEvaluation(rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& Query,
                             const std::vector<SAM::LookupHit>& Hits,
                             std::string* ErrorMessage = nullptr);
bool LoadLookupEvaluationCalibration(rocksdb::DB* Database,
                                     const std::string& Collection,
                                     SAMEvaluationCalibration& Calibration,
                                     std::string* ErrorMessage = nullptr);
