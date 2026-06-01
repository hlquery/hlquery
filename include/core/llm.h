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

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "search/cstore.h"

/*
 * Lightweight runtime view of the local LLM configuration.
 * This mirrors the resolved model settings loaded through ServerConfig.
 */

class CoreExport llm
{
   public:

     struct ContextSuggestion
     {
          std::string Text;
          std::string Kind;
          std::string Relation;
          std::string Evidence;
          std::string Scope;
          double Confidence = 0.0;
          double ValidationScore = 0.0;
          bool Provisional = false;
     };

     struct SearchIntentCandidate
     {
          std::string Text;
          double Weight = 0.0;
     };

     struct AnchorSuggestion
     {
          std::string Text;
          std::string Kind;
          double Confidence = 0.0;
          std::string Reason;
          std::string Language;
     };

     struct SearchIntentResolution
     {
          std::string Interpretation;
          std::string Conclusion;
          std::vector<SearchIntentCandidate> Candidates;
          std::vector<SearchIntentCandidate> RankedTerms;
     };

     llm();

     bool Empty() const
     {
          return ModelsDirectory.empty() && ModelName.empty() &&
                 ModelPath.empty() && InferenceCommand.empty();
     }

     bool Configured() const
     {
          return Enabled && !ModelPath.empty();
     }

     bool IsEnabled() const
     {
          return Enabled;
     }

     const std::string& GetModelsDirectory() const
     {
          return ModelsDirectory;
     }

     const std::string& GetModelName() const
     {
          return ModelName;
     }

     const std::string& GetModelPath() const
     {
          return ModelPath;
     }

     const std::string& GetInferenceCommand() const
     {
          return InferenceCommand;
     }

     std::vector<ContextSuggestion> BuildDocumentContext(const std::string& Collection,
                                                         const Document& Doc,
                                                         size_t Limit = 5) const;

     std::vector<AnchorSuggestion> BuildDocumentAnchors(const std::string& Collection,
                                                        const Document& Doc,
                                                        const std::string& Language = "",
                                                        size_t Limit = 8) const;

     SearchIntentResolution ResolveSearchIntent(const std::string& Collection,
                                               const std::string& Query,
                                               const std::vector<Document>& CandidateDocuments,
                                               size_t Limit = 5) const;

     bool EnqueueContextualization(const std::string& Collection, const Document& Doc, bool Force = false);

     size_t ProcessPendingContextJobs(size_t MaxJobs = 1);

     void StoreDocumentContext(const std::string& Collection,
                               const std::string& DocumentID,
                               const std::vector<ContextSuggestion>& Suggestions);

     std::vector<ContextSuggestion> GetDocumentContext(const std::string& Collection,
                                                      const std::string& DocumentID,
                                                      bool* Pending = nullptr) const;

     void RemoveDocumentContext(const std::string& Collection, const std::string& DocumentID);

     void RemoveCollectionContexts(const std::string& Collection);

     size_t GetPendingContextJobs() const;

   private:

     struct ContextJob
     {
          std::string Collection;
          Document Doc;
     };

     struct ContextCacheEntry
     {
          std::vector<ContextSuggestion> Suggestions;
          long long UpdatedAtMs = 0;
          std::string SourceFingerprint;
     };

     static std::string BuildContextKey(const std::string& Collection, const std::string& DocumentID);

     bool Enabled = true;
     std::string ModelsDirectory;
     std::string ModelName;
     std::string ModelPath;
     std::string InferenceCommand;

     mutable std::mutex ContextMutex;
     mutable std::mutex InferenceMutex;
     std::deque<ContextJob> PendingContextJobs;
     std::unordered_set<std::string> PendingContextKeys;
     mutable std::unordered_map<std::string, ContextCacheEntry> ContextCache;
};
