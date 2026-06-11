/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include "core/llm.h"

#include "core/hlquery.h"

llm::llm()
{
     if (!Instance || !Instance->HasConfig())
     {
          Enabled = false;
          return;
     }

     const ServerConfig& ConfigValue = Instance->GetConfig();
     Enabled = ConfigValue.GetAIEnabled();
     ModelsDirectory = ConfigValue.GetAIModelsDirectory();
     ModelName = ConfigValue.GetAIModelName();
     ModelPath = ConfigValue.GetAIModelPath();
}

std::string llm::BuildContextKey(const std::string& Collection, const std::string& DocumentID)
{
     return Collection + "\n" + DocumentID;
}

std::vector<llm::ContextSuggestion> llm::BuildDocumentContext(const std::string& Collection,
                                                              const Document& Doc,
                                                              size_t Limit) const
{
     (void)Collection;
     (void)Doc;
     (void)Limit;
     return {};
}

std::vector<llm::AnchorSuggestion> llm::BuildDocumentAnchors(const std::string& Collection,
                                                             const Document& Doc,
                                                             const std::string& Language,
                                                             size_t Limit) const
{
     (void)Collection;
     (void)Doc;
     (void)Language;
     (void)Limit;
     return {};
}

llm::SearchIntentResolution llm::ResolveSearchIntent(const std::string& Collection,
                                                     const std::string& Query,
                                                     const std::vector<Document>& CandidateDocuments,
                                                     size_t Limit) const
{
     (void)Collection;
     (void)CandidateDocuments;
     (void)Limit;

     SearchIntentResolution Resolution;
     Resolution.Interpretation = Query;
     Resolution.Conclusion = Query;

     if (!Query.empty())
     {
          Resolution.Candidates.push_back({Query, 1.0});
          Resolution.RankedTerms.push_back({Query, 1.0});
     }

     return Resolution;
}

bool llm::EnqueueContextualization(const std::string& Collection, const Document& Doc, bool Force)
{
     (void)Collection;
     (void)Doc;
     (void)Force;
     return false;
}

size_t llm::ProcessPendingContextJobs(size_t MaxJobs)
{
     (void)MaxJobs;
     return 0;
}

void llm::StoreDocumentContext(const std::string& Collection,
                               const std::string& DocumentID,
                               const std::vector<ContextSuggestion>& Suggestions)
{
     std::lock_guard<std::mutex> Lock(ContextMutex);
     ContextCache[BuildContextKey(Collection, DocumentID)] = {Suggestions, 0, ""};
}

std::vector<llm::ContextSuggestion> llm::GetDocumentContext(const std::string& Collection,
                                                           const std::string& DocumentID,
                                                           bool* Pending) const
{
     if (Pending)
     {
          *Pending = false;
     }

     std::lock_guard<std::mutex> Lock(ContextMutex);
     const auto It = ContextCache.find(BuildContextKey(Collection, DocumentID));

     if (It == ContextCache.end())
     {
          return {};
     }

     return It->second.Suggestions;
}

void llm::RemoveDocumentContext(const std::string& Collection, const std::string& DocumentID)
{
     std::lock_guard<std::mutex> Lock(ContextMutex);
     ContextCache.erase(BuildContextKey(Collection, DocumentID));
}

void llm::RemoveCollectionContexts(const std::string& Collection)
{
     std::lock_guard<std::mutex> Lock(ContextMutex);

     for (auto It = ContextCache.begin(); It != ContextCache.end();)
     {
          if (It->first.rfind(Collection + "\n", 0) == 0)
          {
               It = ContextCache.erase(It);
          }
          else
          {
               ++It;
          }
     }
}

size_t llm::GetPendingContextJobs() const
{
     return 0;
}
