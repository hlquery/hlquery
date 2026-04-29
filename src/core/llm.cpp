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

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "core/hlquery.h"
#include "core/llm.h"
#include "vendor/json/json.hpp"

static std::string TrimCopy(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

static std::string NormalizePhrase(const std::string& Value)
{
     std::string Normalized;
     Normalized.reserve(Value.size());
     bool LastWasSpace = false;

     for (unsigned char C : Value)
     {
          if (std::isalnum(C))
          {
               Normalized.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '_' || C == '/' || C == '.')
          {
               if (!Normalized.empty() && !LastWasSpace)
               {
                    Normalized.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     return TrimCopy(Normalized);
}

static std::vector<std::string> ExtractArrayishValues(const std::string& Raw)
{
     std::vector<std::string> Values;
     const std::string Trimmed = TrimCopy(Raw);

     if (Trimmed.empty())
     {
          return Values;
     }

     try
     {
          if (!Trimmed.empty() && Trimmed.front() == '[')
          {
               nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

               if (Parsed.is_array())
               {
                    for (const auto& Entry : Parsed)
                    {
                         if (Entry.is_string())
                         {
                              const std::string Value = TrimCopy(Entry.get<std::string>());

                              if (!Value.empty())
                              {
                                   Values.push_back(Value);
                              }
                         }
                    }
               }

               return Values;
          }
     }
     catch (...)
     {
     }

     std::string Token;
     std::istringstream In(Trimmed);

     while (std::getline(In, Token, ','))
     {
          Token = TrimCopy(Token);

          if (!Token.empty())
          {
               Values.push_back(Token);
          }
     }

     if (Values.empty())
     {
          Values.push_back(Trimmed);
     }

     return Values;
}

static void AppendSuggestion(std::vector<llm::ContextSuggestion>& Suggestions,
                             std::unordered_set<std::string>& Seen,
                             const std::string& Value,
                             const std::string& Kind,
                             size_t Limit)
{
     if (Suggestions.size() >= Limit)
     {
          return;
     }

     const std::string Normalized = NormalizePhrase(Value);

     if (Normalized.empty() || Normalized.size() < 3 || Normalized.size() > 96)
     {
          return;
     }

     if (!Seen.insert(Normalized).second)
     {
          return;
     }

     llm::ContextSuggestion Entry;
     Entry.Text = Normalized;
     Entry.Kind = Kind;
     Suggestions.push_back(std::move(Entry));
}
std::string llm::BuildContextKey(const std::string& Collection, const std::string& DocumentID)
{
     return Collection + "\n" + DocumentID;
}

std::vector<llm::ContextSuggestion> llm::BuildDocumentContext(const std::string& Collection,
                                                              const Document& Doc,
                                                              size_t Limit) const
{
     std::vector<ContextSuggestion> Suggestions;
     std::unordered_set<std::string> Seen;

     if (!Enabled || Limit == 0)
     {
          return Suggestions;
     }

     const std::string Title = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);

     AppendSuggestion(Suggestions, Seen, Title, "title", Limit);

     for (const auto& Pair : Doc.Fields)
     {
          const std::string LowerKey = ToLowerCopy(Pair.first);

          if (LowerKey == "id" || LowerKey == "name" || LowerKey == "title" ||
              LowerKey == "content" || LowerKey == "description" || LowerKey == "text" ||
              LowerKey == "body" || LowerKey == "summary")
          {
               continue;
          }

          for (const auto& Value : ExtractArrayishValues(Pair.second))
          {
               AppendSuggestion(Suggestions, Seen, Title + " " + Value, "field", Limit);

               if (Suggestions.size() >= Limit)
               {
                    break;
               }
          }
     }

     if (Configured() && !InferenceCommand.empty() && Suggestions.size() < Limit)
     {
          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = Doc.ID;
          Payload["title"] = Doc.Title;
          Payload["content"] = Doc.Content;
          Payload["fields"] = Doc.Fields;
          Payload["mode"] = "context";
          Payload["limit"] = static_cast<unsigned long long>(Limit);

          std::lock_guard<std::mutex> Lock(InferenceMutex);

          setenv("HLQUERY_LLM_MODEL", ModelPath.c_str(), 1);
          setenv("HLQUERY_LLM_CONTEXT_JSON", Payload.dump().c_str(), 1);

          FILE* Pipe = popen(InferenceCommand.c_str(), "r");

          if (Pipe)
          {
               std::array<char, 512> Buffer{};

               while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe))
               {
                    AppendSuggestion(Suggestions, Seen, Buffer.data(), "llm", Limit);

                    if (Suggestions.size() >= Limit)
                    {
                         break;
                    }
               }

               pclose(Pipe);
          }

          unsetenv("HLQUERY_LLM_MODEL");
          unsetenv("HLQUERY_LLM_CONTEXT_JSON");
     }

     if (Suggestions.size() > Limit)
     {
          Suggestions.resize(Limit);
     }

     return Suggestions;
}

void llm::EnqueueContextualization(const std::string& Collection, const Document& Doc)
{
     if (!Enabled || Collection.empty() || Doc.ID.empty())
     {
          return;
     }

     const std::string Key = BuildContextKey(Collection, Doc.ID);
     std::lock_guard<std::mutex> Lock(ContextMutex);

     if (!PendingContextKeys.insert(Key).second)
     {
          return;
     }

     PendingContextJobs.push_back(ContextJob{Collection, Doc});
}

size_t llm::ProcessPendingContextJobs(size_t MaxJobs)
{
     if (!Enabled)
     {
          return 0;
     }

     size_t Processed = 0;

     const bool DebugEnabled = (Instance && Instance->Logs && Instance->Logs->GetDebugMode());

     if (DebugEnabled)
     {
          Instance->Logs->Debug("llm", "ProcessPendingContextJobs: starting with max_jobs=" +
                                         std::to_string(MaxJobs) + ", pending=" +
                                         std::to_string(GetPendingContextJobs()) + ".");
     }

     while (Processed < MaxJobs)
     {
          ContextJob Job;
          std::string Key;

          {
               std::lock_guard<std::mutex> Lock(ContextMutex);

               if (PendingContextJobs.empty())
               {
                    break;
               }

               Job = PendingContextJobs.front();
               PendingContextJobs.pop_front();
               Key = BuildContextKey(Job.Collection, Job.Doc.ID);
               PendingContextKeys.erase(Key);
          }

          if (DebugEnabled)
          {
               Instance->Logs->Debug("llm", "ProcessPendingContextJobs: processing '" +
                                              Job.Collection + "/" + Job.Doc.ID + "'.");
          }

          std::vector<ContextSuggestion> Suggestions =
               BuildDocumentContext(Job.Collection, Job.Doc, 5);

          StoreDocumentContext(Job.Collection, Job.Doc.ID, Suggestions);

          if (DebugEnabled)
          {
               std::string Summary;

               for (size_t I = 0; I < Suggestions.size(); ++I)
               {
                    if (!Summary.empty())
                    {
                         Summary += ", ";
                    }

                    Summary += Suggestions[I].Kind + "=" + Suggestions[I].Text;
               }

               Instance->Logs->Debug("llm", "ProcessPendingContextJobs: stored " +
                                              std::to_string(Suggestions.size()) +
                                              " suggestion(s) for '" + Job.Collection + "/" +
                                              Job.Doc.ID + "'" +
                                              (Summary.empty() ? "." : ": " + Summary + "."));
          }

          ++Processed;
     }

     if (DebugEnabled)
     {
          Instance->Logs->Debug("llm", "ProcessPendingContextJobs: finished processed=" +
                                         std::to_string(Processed) + ", pending=" +
                                         std::to_string(GetPendingContextJobs()) + ".");
     }

     return Processed;
}

void llm::StoreDocumentContext(const std::string& Collection,
                               const std::string& DocumentID,
                               const std::vector<ContextSuggestion>& Suggestions)
{
     if (Collection.empty() || DocumentID.empty())
     {
          return;
     }

     ContextCacheEntry Entry;
     Entry.Suggestions = Suggestions;
     Entry.UpdatedAtMs = (Instance ? Instance->NowMs() : 0);

     std::lock_guard<std::mutex> Lock(ContextMutex);
     ContextCache[BuildContextKey(Collection, DocumentID)] = std::move(Entry);
}

std::vector<llm::ContextSuggestion> llm::GetDocumentContext(const std::string& Collection,
                                                            const std::string& DocumentID,
                                                            bool* Pending) const
{
     if (Pending)
     {
          *Pending = false;
     }

     if (Collection.empty() || DocumentID.empty())
     {
          return {};
     }

     const std::string Key = BuildContextKey(Collection, DocumentID);
     std::lock_guard<std::mutex> Lock(ContextMutex);

     if (Pending)
     {
          *Pending = (PendingContextKeys.find(Key) != PendingContextKeys.end());
     }

     const auto It = ContextCache.find(Key);

     if (It == ContextCache.end())
     {
          return {};
     }

     return It->second.Suggestions;
}

void llm::RemoveDocumentContext(const std::string& Collection, const std::string& DocumentID)
{
     if (Collection.empty() || DocumentID.empty())
     {
          return;
     }

     const std::string Key = BuildContextKey(Collection, DocumentID);
     std::lock_guard<std::mutex> Lock(ContextMutex);
     ContextCache.erase(Key);
     PendingContextKeys.erase(Key);

     PendingContextJobs.erase(
          std::remove_if(PendingContextJobs.begin(), PendingContextJobs.end(),
                         [&](const ContextJob& Job)
                         {
                              return Job.Collection == Collection && Job.Doc.ID == DocumentID;
                         }),
          PendingContextJobs.end());
}

size_t llm::GetPendingContextJobs() const
{
     std::lock_guard<std::mutex> Lock(ContextMutex);
     return PendingContextJobs.size();
}
