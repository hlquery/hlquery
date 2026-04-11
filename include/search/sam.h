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

#ifndef ROCKSDB_NAMESPACE
#define ROCKSDB_NAMESPACE rocksdb
#endif

#include <mutex>
#include <rocksdb/db.h>
#include <string>
#include <vector>

#include "search/cstore.h"

/*
 * Secondary Assistant Manager.
 * Maintains a second RocksDB database with heuristic/LLM-derived lookup phrases
 * that can be used to discover documents through broader natural-language intent.
 */

class SAM
{
   public:

     struct LookupHit
     {
          std::string Collection;
          std::string DocumentID;
          std::string Title;
          std::string MatchedTerm;
     };

     SAM();
     ~SAM();

     bool Initialize();
     void Shutdown();

     bool IsOpen() const;
     const std::string& GetDBPath() const
     {
          return DBPath;
     }

     bool Recreate(std::string* ErrorMessage = nullptr);
     bool IndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);
     bool DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);
     std::vector<LookupHit> Lookup(const std::string& Query, size_t Limit = 20) const;

   private:

     std::unique_ptr<rocksdb::DB> Database;
     rocksdb::Options OptionsValue;
     std::string DBPath;

     mutable std::mutex DBMutex;
     mutable std::mutex InferenceMutex;

     std::string ResolveDBPath() const;
     bool ClearAll(std::string* ErrorMessage = nullptr);
     bool RemoveExistingDocumentTermsLocked(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage = nullptr);
     bool IndexDocumentLocked(const std::string& Collection, const Document& Doc, std::string* ErrorMessage = nullptr);

     std::vector<std::string> ExpandDocumentTerms(const std::string& Collection, const Document& Doc) const;
     std::vector<std::string> GenerateLLMTerms(const std::string& Collection, const Document& Doc) const;
     std::vector<std::string> GenerateHeuristicTerms(const std::string& Collection, const Document& Doc) const;
};
