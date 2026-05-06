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
#include <cmath>
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

static void AppendIntentCandidate(std::vector<llm::SearchIntentCandidate>& Candidates,
                                  std::unordered_set<std::string>& Seen,
                                  const std::string& Value,
                                  double Weight,
                                  size_t Limit)
{
     if (Candidates.size() >= Limit)
     {
          return;
     }

     const std::string Normalized = NormalizePhrase(Value);

     if (Normalized.empty() || Normalized.size() < 3)
     {
          return;
     }

     if (!Seen.insert(Normalized).second)
     {
          return;
     }

     llm::SearchIntentCandidate Candidate;
     Candidate.Text = Normalized;
     Candidate.Weight = std::max(0.0, std::min(1.0, Weight));
     Candidates.push_back(std::move(Candidate));
}

static void SortIntentCandidates(std::vector<llm::SearchIntentCandidate>& Candidates)
{
     std::sort(Candidates.begin(), Candidates.end(),
               [](const llm::SearchIntentCandidate& Left,
                  const llm::SearchIntentCandidate& Right)
               {
                    if (std::fabs(Left.Weight - Right.Weight) > 0.00001)
                    {
                         return Left.Weight > Right.Weight;
                    }

                    return Left.Text < Right.Text;
               });
}

static llm::SearchIntentResolution BuildHeuristicSearchIntentResolution(const std::string& Query,
                                                                        const std::vector<Document>& CandidateDocuments,
                                                                        size_t Limit)
{
     llm::SearchIntentResolution Resolution;
     Resolution.Interpretation = NormalizePhrase(Query);
     std::unordered_set<std::string> CandidateSeen;
     std::unordered_set<std::string> RankedSeen;

     for (size_t Index = 0; Index < CandidateDocuments.size() && Index < Limit; ++Index)
     {
          const Document& CandidateDocument = CandidateDocuments[Index];
          const std::string Title = TrimCopy(CandidateDocument.Title.empty() ? CandidateDocument.ID : CandidateDocument.Title);

          if (Title.empty())
          {
               continue;
          }

          const double Weight = std::max(0.10, 1.0 - (static_cast<double>(Index) * 0.15));
          AppendIntentCandidate(Resolution.Candidates, CandidateSeen, Title, Weight, Limit);
          AppendIntentCandidate(Resolution.RankedTerms, RankedSeen, Title, Weight, Limit * 2);

          for (const auto& Pair : CandidateDocument.Fields)
          {
               if (Resolution.RankedTerms.size() >= Limit * 2)
               {
                    break;
               }

               for (const auto& Value : ExtractArrayishValues(Pair.second))
               {
                    AppendIntentCandidate(Resolution.RankedTerms, RankedSeen, Title + " " + Value, Weight * 0.90, Limit * 2);

                    if (Resolution.RankedTerms.size() >= Limit * 2)
                    {
                         break;
                    }
               }
          }
     }

     if (!Resolution.Candidates.empty())
     {
          Resolution.Conclusion = "Likely intent points to " + Resolution.Candidates.front().Text;
     }

     SortIntentCandidates(Resolution.Candidates);
     SortIntentCandidates(Resolution.RankedTerms);
     return Resolution;
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

llm::SearchIntentResolution llm::ResolveSearchIntent(const std::string& Collection,
                                                     const std::string& Query,
                                                     const std::vector<Document>& CandidateDocuments,
                                                     size_t Limit) const
{
     if (!Enabled || Query.empty() || Limit == 0)
     {
          return {};
     }

     SearchIntentResolution Resolution =
          BuildHeuristicSearchIntentResolution(Query, CandidateDocuments, Limit);

     if (!Configured() || InferenceCommand.empty())
     {
          return Resolution;
     }

     nlohmann::json Payload;
     Payload["mode"] = "search_intent";
     Payload["collection"] = Collection;
     Payload["query"] = Query;
     Payload["limit"] = static_cast<unsigned long long>(Limit);
     Payload["candidates"] = nlohmann::json::array();

     for (const auto& CandidateDocument : CandidateDocuments)
     {
          nlohmann::json Candidate;
          Candidate["id"] = CandidateDocument.ID;
          Candidate["title"] = CandidateDocument.Title;
          Candidate["content"] = CandidateDocument.Content;
          Candidate["fields"] = CandidateDocument.Fields;
          Payload["candidates"].push_back(std::move(Candidate));
     }

     std::lock_guard<std::mutex> Lock(InferenceMutex);
     setenv("HLQUERY_LLM_MODEL", ModelPath.c_str(), 1);
     setenv("HLQUERY_LLM_SEARCH_JSON", Payload.dump().c_str(), 1);

     FILE* Pipe = popen(InferenceCommand.c_str(), "r");
     std::string RawOutput;

     if (Pipe)
     {
          std::array<char, 1024> Buffer{};

          while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe))
          {
               RawOutput += Buffer.data();
          }

          pclose(Pipe);
     }

     unsetenv("HLQUERY_LLM_MODEL");
     unsetenv("HLQUERY_LLM_SEARCH_JSON");

     const std::string TrimmedOutput = TrimCopy(RawOutput);

     if (TrimmedOutput.empty())
     {
          return Resolution;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(TrimmedOutput);
          SearchIntentResolution Parsed;
          Parsed.Interpretation = TrimCopy(Root.value("interpretation", ""));
          Parsed.Conclusion = TrimCopy(Root.value("conclusion", ""));
          std::unordered_set<std::string> CandidateSeen;
          std::unordered_set<std::string> RankedSeen;

          if (Root.contains("candidates") && Root["candidates"].is_array())
          {
               for (const auto& Item : Root["candidates"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    AppendIntentCandidate(Parsed.Candidates,
                                          CandidateSeen,
                                          Item.value("text", ""),
                                          Item.value("weight", 0.0),
                                          Limit);
               }
          }

          if (Root.contains("ranked_terms") && Root["ranked_terms"].is_array())
          {
               for (const auto& Item : Root["ranked_terms"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    AppendIntentCandidate(Parsed.RankedTerms,
                                          RankedSeen,
                                          Item.value("text", ""),
                                          Item.value("weight", 0.0),
                                          Limit * 2);
               }
          }

          if (Parsed.Interpretation.empty())
          {
               Parsed.Interpretation = Resolution.Interpretation;
          }

          if (Parsed.Candidates.empty())
          {
               Parsed.Candidates = Resolution.Candidates;
          }

          if (Parsed.RankedTerms.empty())
          {
               Parsed.RankedTerms = Resolution.RankedTerms;
          }

          if (Parsed.Conclusion.empty() && !Parsed.Candidates.empty())
          {
               Parsed.Conclusion = "Likely intent points to " + Parsed.Candidates.front().Text;
          }

          SortIntentCandidates(Parsed.Candidates);
          SortIntentCandidates(Parsed.RankedTerms);
          return Parsed;
     }
     catch (...)
     {
          return Resolution;
     }
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
