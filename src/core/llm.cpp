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
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "core/hlquery.h"
#include "core/llm.h"
#include "sam/internal.h"
#include "sam/lang.h"
#include "sam/sam.h"
#include "vendor/json/json.hpp"

struct LLMInferenceResult
{
     bool Started = false;
     bool TimedOut = false;
     int ExitCode = -1;
     std::string Stdout;
     std::string Stderr;
};

llm::llm()
{
     if (!Instance || !Instance->HasConfig())
     {
          return;
     }

     const ServerConfig& ConfigValue = Instance->GetConfig();

     Enabled = ConfigValue.GetAIEnabled();
     ModelsDirectory = ConfigValue.GetAIModelsDirectory();
     ModelName = ConfigValue.GetAIModelName();
     ModelPath = ConfigValue.GetAIModelPath();
     InferenceCommand = ConfigValue.GetAIInferenceCommand();
}

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

static std::string DetectDocumentType(const Document& Doc)
{
     size_t TaxonomyFields = 0;
     size_t AliasFields = 0;
     size_t BodyChars = Doc.Content.size();

     for (const auto& Pair : Doc.Fields)
     {
          const std::string LowerKey = ToLowerCopy(Pair.first);

          if (LowerKey == "tag" || LowerKey == "tags" || LowerKey == "label" ||
              LowerKey == "labels" || LowerKey == "category" || LowerKey == "categories" ||
              LowerKey == "topic" || LowerKey == "topics" || LowerKey == "brand" ||
              LowerKey == "brands" || LowerKey == "author" || LowerKey == "authors")
          {
               ++TaxonomyFields;
          }

          if (LowerKey == "alias" || LowerKey == "aliases" || LowerKey == "slug" ||
              LowerKey == "nickname" || LowerKey == "handle")
          {
               ++AliasFields;
          }

          if (LowerKey == "content" || LowerKey == "body" || LowerKey == "text" ||
              LowerKey == "article" || LowerKey == "markdown" || LowerKey == "notes")
          {
               BodyChars += Pair.second.size();
          }
     }

     if (TaxonomyFields >= 3)
     {
          return "listing";
     }

     if (AliasFields >= 2)
     {
          return "profile";
     }

     if (BodyChars >= 1200)
     {
          return "article";
     }

     return "reference";
}

static nlohmann::json BuildCollectionPromptMetadata(const std::string& Collection)
{
     nlohmann::json Meta;
     Meta["name"] = Collection;
     Meta["suggested_relationships"] = {
          "alias",
          "metro_area",
          "location",
          "category",
          "topic",
          "related_query"
     };

     CollectionConfig Config;

     if (HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
     {
          Meta["metadata"] = Config.Metadata;
     }

     return Meta;
}

static std::vector<std::string> BuildPromptConstraints(const std::string& Language,
                                                       const std::string& Objective)
{
     std::vector<std::string> Constraints = {
          "Do not invent entities, brands, products, or people that are not grounded in the document.",
          "Keep phrases short, indexable, and useful as anchors.",
          "Prefer exact or near-exact wording supported by the title, fields, or body.",
          "Avoid ids, urls, handles, timestamps, and noisy boilerplate."
     };

     if (Objective == "anchors")
     {
          Constraints.push_back("Return anchor, alias, descriptor, and query candidates only when they would be natural internal-link phrases.");
     }
     else if (Objective == "context")
     {
          Constraints.push_back("Return short contextual phrases that improve retrieval recall without drifting away from the document.");
     }
     else if (Objective == "search_intent")
     {
          Constraints.push_back("Resolve likely user intent only from the query and supplied candidates.");
     }

     if (!Language.empty() && Language != "und")
     {
          Constraints.push_back("Use the document language `" + Language + "` for generated phrases.");
     }

     return Constraints;
}

static std::string BuildPromptInstructionText(const std::string& Objective,
                                              const std::string& Language,
                                              const std::string& DocumentType)
{
     std::string Instruction = "Objective: " + Objective + ". ";
     Instruction += "Document type: " + DocumentType + ". ";

     if (!Language.empty())
     {
          Instruction += "Language: " + Language + ". ";
     }

     if (Objective == "anchors")
     {
          Instruction += "Generate precise anchor/alias/query/descriptor suggestions for indexing.";
     }
     else if (Objective == "context")
     {
          Instruction += "Generate short context phrases that broaden retrieval safely.";
     }
     else if (Objective == "search_intent")
     {
          Instruction += "Interpret the query and rank candidate documents and terms.";
     }

     return Instruction;
}

static void AddPromptEnvelope(nlohmann::json& Payload,
                              const std::string& Collection,
                              const Document& Doc,
                              const std::string& Objective,
                              size_t Limit,
                              const std::string& Language = "")
{
     const std::string EffectiveLanguage =
          Language.empty() ? sam::lang::DetectDocumentLanguage(Collection, Doc) : Language;
     const std::string DocumentType = DetectDocumentType(Doc);

     Payload["collection_profile"] = BuildCollectionPromptMetadata(Collection);
     Payload["language"] = EffectiveLanguage;
     Payload["document_type"] = DocumentType;
     Payload["objective"] = Objective;
     Payload["limit"] = static_cast<unsigned long long>(Limit);
     Payload["prompt"] = {
          {"version", 1},
          {"instruction", BuildPromptInstructionText(Objective, EffectiveLanguage, DocumentType)},
          {"constraints", BuildPromptConstraints(EffectiveLanguage, Objective)},
          {"output_contract",
               Objective == "search_intent"
                    ? "Return JSON with interpretation, conclusion, candidates[], ranked_terms[]."
                    : (Objective == "anchors"
                         ? "Return JSON array or {anchors:[...]} with items {text,kind,confidence,reason,language}."
                         : "Return {contexts:[...]} with items {term,relation,confidence,evidence,scope}. Use relation types such as alias, metro_area, topic, category, or related_query.")}
     };
}

static void AppendPipeOutput(int FD, std::string& Output)
{
     std::array<char, 4096> Buffer{};

     while (true)
     {
          const ssize_t ReadCount = read(FD, Buffer.data(), Buffer.size());

          if (ReadCount > 0)
          {
               Output.append(Buffer.data(), static_cast<size_t>(ReadCount));
               continue;
          }

          if (ReadCount == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
          {
               break;
          }

          if (errno == EINTR)
          {
               continue;
          }

          break;
     }
}

static bool WritePayloadFile(const std::string& Payload, std::string& PathOut)
{
     std::array<char, 64> Template{};
     const std::string Prefix = "/tmp/hlquery-llm-XXXXXX";

     std::copy(Prefix.begin(), Prefix.end(), Template.begin());
     int FD = mkstemp(Template.data());

     if (FD < 0)
     {
          return false;
     }

     size_t Written = 0;

     while (Written < Payload.size())
     {
          const ssize_t Result = write(FD,
                                       Payload.data() + Written,
                                       Payload.size() - Written);

          if (Result > 0)
          {
               Written += static_cast<size_t>(Result);
               continue;
          }

          if (Result < 0 && errno == EINTR)
          {
               continue;
          }

          close(FD);
          unlink(Template.data());
          return false;
     }

     close(FD);
     PathOut = Template.data();
     return true;
}

static void LogLLMInferenceFailure(const std::string& Mode, const LLMInferenceResult& Result)
{
     if (!Instance || !Instance->Logs)
     {
          return;
     }

     std::string Message = "LLM inference failed for mode '" + Mode + "'";

     if (Result.TimedOut)
     {
          Message += ": timed out";
     }
     else if (!Result.Started)
     {
          Message += ": process did not start";
     }
     else
     {
          Message += ": exit_code=" + std::to_string(Result.ExitCode);
     }

     const std::string Stderr = TrimCopy(Result.Stderr);

     if (!Stderr.empty())
     {
          Message += ", stderr=" + Stderr.substr(0, 512);
     }

     Instance->Logs->Normal("llm", Message + ".");
}

static LLMInferenceResult RunLLMInferenceCommand(const std::string& Command,
                                                 const std::string& ModelPath,
                                                 const std::string& Mode,
                                                 const nlohmann::json& Payload,
                                                 int TimeoutMS = 60000)
{
     LLMInferenceResult Result;
     std::string PayloadPath;

     if (Command.empty() || !WritePayloadFile(Payload.dump(), PayloadPath))
     {
          return Result;
     }

     int StdoutPipe[2] = {-1, -1};
     int StderrPipe[2] = {-1, -1};

     if (pipe(StdoutPipe) != 0 || pipe(StderrPipe) != 0)
     {
          if (StdoutPipe[0] >= 0)
          {
               close(StdoutPipe[0]);
               close(StdoutPipe[1]);
          }

          if (StderrPipe[0] >= 0)
          {
               close(StderrPipe[0]);
               close(StderrPipe[1]);
          }

          unlink(PayloadPath.c_str());
          return Result;
     }

     const pid_t PID = fork();

     if (PID == 0)
     {
          dup2(StdoutPipe[1], STDOUT_FILENO);
          dup2(StderrPipe[1], STDERR_FILENO);
          close(StdoutPipe[0]);
          close(StdoutPipe[1]);
          close(StderrPipe[0]);
          close(StderrPipe[1]);

          setenv("HLQUERY_LLM_MODEL", ModelPath.c_str(), 1);
          setenv("HLQUERY_LLM_PAYLOAD_MODE", Mode.c_str(), 1);
          setenv("HLQUERY_LLM_PAYLOAD_JSON_FILE", PayloadPath.c_str(), 1);

          if (Mode == "context")
          {
               setenv("HLQUERY_LLM_CONTEXT_JSON_FILE", PayloadPath.c_str(), 1);
          }
          else if (Mode == "anchors")
          {
               setenv("HLQUERY_LLM_ANCHOR_JSON_FILE", PayloadPath.c_str(), 1);
          }
          else if (Mode == "search_intent")
          {
               setenv("HLQUERY_LLM_SEARCH_JSON_FILE", PayloadPath.c_str(), 1);
          }

          execl("/bin/sh", "sh", "-c", Command.c_str(), static_cast<char*>(nullptr));
          _exit(127);
     }

     close(StdoutPipe[1]);
     close(StderrPipe[1]);

     if (PID < 0)
     {
          close(StdoutPipe[0]);
          close(StderrPipe[0]);
          unlink(PayloadPath.c_str());
          return Result;
     }

     Result.Started = true;
     fcntl(StdoutPipe[0], F_SETFL, fcntl(StdoutPipe[0], F_GETFL, 0) | O_NONBLOCK);
     fcntl(StderrPipe[0], F_SETFL, fcntl(StderrPipe[0], F_GETFL, 0) | O_NONBLOCK);

     const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMS);
     int Status = 0;

     while (true)
     {
          AppendPipeOutput(StdoutPipe[0], Result.Stdout);
          AppendPipeOutput(StderrPipe[0], Result.Stderr);

          const pid_t WaitResult = waitpid(PID, &Status, WNOHANG);

          if (WaitResult == PID)
          {
               if (WIFEXITED(Status))
               {
                    Result.ExitCode = WEXITSTATUS(Status);
               }
               else if (WIFSIGNALED(Status))
               {
                    Result.ExitCode = 128 + WTERMSIG(Status);
               }

               break;
          }

          if (WaitResult < 0 && errno != EINTR)
          {
               break;
          }

          if (std::chrono::steady_clock::now() >= Deadline)
          {
               Result.TimedOut = true;
               kill(PID, SIGKILL);
               waitpid(PID, &Status, 0);
               break;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(10));
     }

     AppendPipeOutput(StdoutPipe[0], Result.Stdout);
     AppendPipeOutput(StderrPipe[0], Result.Stderr);
     close(StdoutPipe[0]);
     close(StderrPipe[0]);
     unlink(PayloadPath.c_str());
     return Result;
}

static void AppendSuggestion(std::vector<llm::ContextSuggestion>& Suggestions,
                             std::unordered_set<std::string>& Seen,
                             const std::string& Value,
                             const std::string& Kind,
                             size_t Limit,
                             const std::string& Relation = "",
                             double Confidence = 0.70,
                             const std::string& Evidence = "",
                             const std::string& Scope = "document")
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
     Entry.Relation = Relation.empty() ? Kind : Relation;
     Entry.Confidence = std::max(0.0, std::min(1.0, Confidence));
     Entry.Evidence = TrimCopy(Evidence);
     Entry.Scope = Scope.empty() ? "document" : Scope;
     Entry.Provisional = Entry.Kind == "llm" && Entry.Confidence < 0.78;
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

static void AppendAnchorSuggestion(std::vector<llm::AnchorSuggestion>& Suggestions,
                                   std::unordered_set<std::string>& Seen,
                                   const std::string& Value,
                                   const std::string& Kind,
                                   double Confidence,
                                   const std::string& Reason,
                                   const std::string& Language,
                                   size_t Limit)
{
     if (Suggestions.size() >= Limit)
     {
          return;
     }

     const std::string Normalized = NormalizePhrase(Value);

     if (Normalized.empty() || Normalized.size() < 2 || Normalized.size() > 96)
     {
          return;
     }

     if (!Seen.insert(Kind + "\n" + Normalized).second)
     {
          return;
     }

     llm::AnchorSuggestion Suggestion;
     Suggestion.Text = Normalized;
     Suggestion.Kind = Kind.empty() ? "anchor" : Kind;
     Suggestion.Confidence = std::max(0.0, std::min(1.0, Confidence));
     Suggestion.Reason = Reason;
     Suggestion.Language = Language;
     Suggestions.push_back(std::move(Suggestion));
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

static void SortAnchorSuggestions(std::vector<llm::AnchorSuggestion>& Suggestions)
{
     std::sort(Suggestions.begin(), Suggestions.end(),
               [](const llm::AnchorSuggestion& Left,
                  const llm::AnchorSuggestion& Right)
               {
                    if (std::fabs(Left.Confidence - Right.Confidence) > 0.00001)
                    {
                         return Left.Confidence > Right.Confidence;
                    }

                    if (Left.Kind != Right.Kind)
                    {
                         return Left.Kind < Right.Kind;
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
     const bool UseInference = Configured() && !InferenceCommand.empty();
     const size_t HeuristicLimit = UseInference ? std::max<size_t>(1, Limit / 2) : Limit;

     AppendSuggestion(Suggestions, Seen, Title, "title", HeuristicLimit);

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
               AppendSuggestion(Suggestions, Seen, Title + " " + Value, "field", HeuristicLimit);

               if (Suggestions.size() >= HeuristicLimit)
               {
                    break;
               }
          }
     }

     if (UseInference && Suggestions.size() < Limit)
     {
          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = Doc.ID;
          Payload["title"] = Doc.Title;
          Payload["content"] = Doc.Content;
          Payload["fields"] = Doc.Fields;
          Payload["mode"] = "context";
          AddPromptEnvelope(Payload, Collection, Doc, "context", Limit);

          std::lock_guard<std::mutex> Lock(InferenceMutex);
          const LLMInferenceResult Result =
               RunLLMInferenceCommand(InferenceCommand, ModelPath, "context", Payload);

          if (Result.ExitCode == 0)
          {
               const std::string TrimmedOutput = TrimCopy(Result.Stdout);

               try
               {
                    const nlohmann::json Root = nlohmann::json::parse(TrimmedOutput);
                    const nlohmann::json* Contexts = Root.is_array()
                         ? &Root
                         : (Root.is_object() && Root.contains("contexts") && Root["contexts"].is_array()
                              ? &Root["contexts"]
                              : nullptr);

                    if (Contexts)
                    {
                         for (const auto& Item : *Contexts)
                         {
                              if (!Item.is_object())
                              {
                                   continue;
                              }

                              AppendSuggestion(Suggestions,
                                               Seen,
                                               Item.value("term", Item.value("text", "")),
                                               "llm",
                                               Limit,
                                               Item.value("relation", "context"),
                                               Item.value("confidence", 0.70),
                                               Item.value("evidence", ""),
                                               Item.value("scope", "document"));
                         }
                    }
               }
               catch (...)
               {
                    std::istringstream Input(TrimmedOutput);
                    std::string Line;

                    while (std::getline(Input, Line))
                    {
                         AppendSuggestion(Suggestions, Seen, Line, "llm", Limit);
                    }
               }
          }
          else
          {
               LogLLMInferenceFailure("context", Result);
          }
     }

     if (Suggestions.size() > Limit)
     {
          Suggestions.resize(Limit);
     }

     return Suggestions;
}

std::vector<llm::AnchorSuggestion> llm::BuildDocumentAnchors(const std::string& Collection,
                                                             const Document& Doc,
                                                             const std::string& Language,
                                                             size_t Limit) const
{
     std::vector<AnchorSuggestion> Suggestions;
     std::unordered_set<std::string> Seen;

     if (!Enabled || Limit == 0)
     {
          return Suggestions;
     }

     const std::string Title = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);

     if (!Title.empty())
     {
          AppendAnchorSuggestion(Suggestions, Seen, Title, "anchor", 0.96, "title", Language, Limit);
     }

     for (const auto& Pair : Doc.Fields)
     {
          const std::string LowerKey = ToLowerCopy(Pair.first);
          std::string Kind;
          double Confidence = 0.0;

          if (LowerKey == "alias" || LowerKey == "aliases" || LowerKey == "slug" ||
              LowerKey == "nickname" || LowerKey == "handle")
          {
               Kind = "alias";
               Confidence = 0.86;
          }
          else if (LowerKey == "query" || LowerKey == "queries" || LowerKey == "keywords" ||
                   LowerKey == "search_terms")
          {
               Kind = "query";
               Confidence = 0.82;
          }
          else if (LowerKey == "tag" || LowerKey == "tags" || LowerKey == "label" ||
                   LowerKey == "labels" || LowerKey == "category" || LowerKey == "categories" ||
                   LowerKey == "topic" || LowerKey == "topics" || LowerKey == "author" ||
                   LowerKey == "authors" || LowerKey == "brand" || LowerKey == "brands")
          {
               Kind = "descriptor";
               Confidence = 0.78;
          }
          else
          {
               continue;
          }

          for (const auto& Value : ExtractArrayishValues(Pair.second))
          {
               AppendAnchorSuggestion(Suggestions, Seen, Value, Kind, Confidence, LowerKey, Language, Limit);

               if (!Title.empty() && Kind != "alias")
               {
                    AppendAnchorSuggestion(Suggestions,
                                           Seen,
                                           Title + " " + Value,
                                           "query",
                                           Confidence - 0.04,
                                           LowerKey + "_pair",
                                           Language,
                                           Limit);
               }

               if (Suggestions.size() >= Limit)
               {
                    break;
               }
          }

          if (Suggestions.size() >= Limit)
          {
               break;
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
          Payload["mode"] = "anchors";
          AddPromptEnvelope(Payload, Collection, Doc, "anchors", Limit, Language);

          std::lock_guard<std::mutex> Lock(InferenceMutex);
          const LLMInferenceResult Result =
               RunLLMInferenceCommand(InferenceCommand, ModelPath, "anchors", Payload);

          if (Result.ExitCode != 0)
          {
               LogLLMInferenceFailure("anchors", Result);
          }

          const std::string TrimmedOutput = TrimCopy(Result.Stdout);

          if (Result.ExitCode == 0 && !TrimmedOutput.empty())
          {
               try
               {
                    const nlohmann::json Root = nlohmann::json::parse(TrimmedOutput);
                    const nlohmann::json* AnchorArray = nullptr;

                    if (Root.is_array())
                    {
                         AnchorArray = &Root;
                    }
                    else if (Root.is_object() && Root.contains("anchors") && Root["anchors"].is_array())
                    {
                         AnchorArray = &Root["anchors"];
                    }

                    if (AnchorArray)
                    {
                         for (const auto& Item : *AnchorArray)
                         {
                              if (!Item.is_object())
                              {
                                   continue;
                              }

                              AppendAnchorSuggestion(Suggestions,
                                                     Seen,
                                                     Item.value("text", ""),
                                                     Item.value("kind", "anchor"),
                                                     Item.value("confidence", 0.72),
                                                     Item.value("reason", "llm"),
                                                     Item.value("language", Language),
                                                     Limit);

                              if (Suggestions.size() >= Limit)
                              {
                                   break;
                              }
                         }
                    }
               }
               catch (...)
               {
                    std::istringstream Input(TrimmedOutput);
                    std::string Line;

                    while (std::getline(Input, Line))
                    {
                         Line = TrimCopy(Line);

                         if (Line.empty())
                         {
                              continue;
                         }

                         AppendAnchorSuggestion(Suggestions, Seen, Line, "anchor", 0.70, "llm", Language, Limit);

                         if (Suggestions.size() >= Limit)
                         {
                              break;
                         }
                    }
               }
          }
     }

     SortAnchorSuggestions(Suggestions);

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

     const SearchIntentResolution HeuristicResolution =
          BuildHeuristicSearchIntentResolution(Query, CandidateDocuments, Limit);

     if (!Configured() || InferenceCommand.empty())
     {
          return HeuristicResolution;
     }

     nlohmann::json Payload;
     Payload["mode"] = "search_intent";
     Payload["collection"] = Collection;
     Payload["query"] = Query;
     Payload["candidates"] = nlohmann::json::array();

     Document QueryDoc;
     QueryDoc.ID = Query;
     QueryDoc.Title = Query;
     AddPromptEnvelope(Payload, Collection, QueryDoc, "search_intent", Limit);

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
     const LLMInferenceResult Result =
          RunLLMInferenceCommand(InferenceCommand, ModelPath, "search_intent", Payload);

     if (Result.ExitCode != 0)
     {
          LogLLMInferenceFailure("search_intent", Result);
          return {};
     }

     const std::string TrimmedOutput = TrimCopy(Result.Stdout);

     if (TrimmedOutput.empty())
     {
          return {};
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
               Parsed.Interpretation = NormalizePhrase(Query);
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
          return {};
     }
}

bool llm::EnqueueContextualization(const std::string& Collection, const Document& Doc, bool Force)
{
     if (!Enabled || Collection.empty() || Doc.ID.empty())
     {
          return false;
     }

     const std::string Key = BuildContextKey(Collection, Doc.ID);
     (void)GetDocumentContext(Collection, Doc.ID);
     std::lock_guard<std::mutex> Lock(ContextMutex);

     const auto Cached = ContextCache.find(Key);
     if (!Force &&
         Cached != ContextCache.end() &&
         Cached->second.SourceFingerprint == BuildSAMSourceDocumentFingerprint(Doc) &&
         Cached->second.UpdatedAtMs > 0 &&
         Instance && (Instance->NowMs() - Cached->second.UpdatedAtMs) < (24LL * 60LL * 60LL * 1000LL))
     {
          return false;
     }

     if (!PendingContextKeys.insert(Key).second)
     {
          return false;
     }

     PendingContextJobs.push_back(ContextJob{Collection, Doc});
     return true;
}

/* Processes queued context-generation jobs while respecting the configured batch size. */

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
          const std::string StartMessage = "ProcessPendingContextJobs: starting with max_jobs=" + std::to_string(MaxJobs) + ", pending=" + std::to_string(GetPendingContextJobs()) + ".";

          Instance->Logs->Debug("llm", StartMessage);
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
               const std::string ProcessingMessage = "ProcessPendingContextJobs: processing '" + Job.Collection + "/" + Job.Doc.ID + "'.";

               Instance->Logs->Debug("llm", ProcessingMessage);
          }

          const Document SourceDoc =
               HybridStorageManager::GetInstance().GetDocument(Job.Collection, Job.Doc.ID);

          if (SourceDoc.ID.empty())
          {
               RemoveDocumentContext(Job.Collection, Job.Doc.ID);
               ++Processed;
               continue;
          }

          const size_t ContextLimit = Instance && Instance->Config
               ? static_cast<size_t>(std::max(1, Instance->Config->GetSamContextMaxIdeas()))
               : 5;
          std::vector<ContextSuggestion> Suggestions =
               BuildDocumentContext(Job.Collection, SourceDoc, ContextLimit);
          const Document LatestDoc =
               HybridStorageManager::GetInstance().GetDocument(Job.Collection, SourceDoc.ID);

          if (LatestDoc.ID.empty())
          {
               RemoveDocumentContext(Job.Collection, SourceDoc.ID);
               ++Processed;
               continue;
          }

          if (BuildSAMSourceDocumentFingerprint(LatestDoc) !=
              BuildSAMSourceDocumentFingerprint(SourceDoc))
          {
               (void)EnqueueContextualization(Job.Collection, LatestDoc);

               if (DebugEnabled)
               {
                    Instance->Logs->Debug("llm",
                                          "ProcessPendingContextJobs: discarded stale context for '" +
                                               Job.Collection + "/" + SourceDoc.ID +
                                               "' because the source changed during inference.");
               }

               ++Processed;
               continue;
          }

          StoreDocumentContext(Job.Collection, LatestDoc.ID, Suggestions);

          if (Instance && Instance->Sam && Instance->Sam->IsOpen())
          {
               std::string IndexError;

               if (!Instance->Sam->EnqueueIndexDocument(Job.Collection, LatestDoc, &IndexError) &&
                   DebugEnabled && !IndexError.empty())
               {
                    Instance->Logs->Debug("llm",
                                          "ProcessPendingContextJobs: failed to queue SAM refresh for '" +
                                               Job.Collection + "/" + LatestDoc.ID + "': " + IndexError + ".");
               }
          }

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

               const std::string StoredMessage = "ProcessPendingContextJobs: stored " + std::to_string(Suggestions.size()) + " suggestion(s) for '" + Job.Collection + "/" + LatestDoc.ID + "'" + (Summary.empty() ? "." : ": " + Summary + ".");

               Instance->Logs->Debug("llm", StoredMessage);
          }

          ++Processed;
     }

     if (DebugEnabled)
     {
          const std::string FinishedMessage = "ProcessPendingContextJobs: finished processed=" + std::to_string(Processed) + ", pending=" + std::to_string(GetPendingContextJobs()) + ".";

          Instance->Logs->Debug("llm", FinishedMessage);
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
     const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);
     Entry.SourceFingerprint = Doc.ID.empty() ? "" : BuildSAMSourceDocumentFingerprint(Doc);

     nlohmann::json PersistedSuggestions = nlohmann::json::array();
     for (const auto& Suggestion : Suggestions)
     {
          PersistedSuggestions.push_back({
               {"term", Suggestion.Text},
               {"kind", Suggestion.Kind},
               {"relation", Suggestion.Relation},
               {"confidence", Suggestion.Confidence},
               {"evidence", Suggestion.Evidence},
               {"scope", Suggestion.Scope},
               {"provisional", Suggestion.Provisional}
          });
     }

     if (Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          (void)Instance->Sam->StoreDocumentContext(Collection,
                                                    DocumentID,
                                                    Entry.SourceFingerprint,
                                                    static_cast<uint64_t>(Entry.UpdatedAtMs),
                                                    PersistedSuggestions);
     }

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

     auto It = ContextCache.find(Key);

     if (It == ContextCache.end())
     {
          nlohmann::json Root;

          if (!(Instance && Instance->Sam && Instance->Sam->IsOpen() &&
                Instance->Sam->LoadDocumentContext(Collection, DocumentID, Root) &&
                Root.contains("suggestions") && Root["suggestions"].is_array()))
          {
               return {};
          }

          ContextCacheEntry Entry;
          Entry.UpdatedAtMs = Root.value("updated_at_ms", static_cast<long long>(0));
          Entry.SourceFingerprint = Root.value("source_fingerprint", "");

          for (const auto& Item : Root["suggestions"])
          {
               if (!Item.is_object())
               {
                    continue;
               }

               ContextSuggestion Suggestion;
               Suggestion.Text = Item.value("term", "");
               Suggestion.Kind = Item.value("kind", "llm");
               Suggestion.Relation = Item.value("relation", "context");
               Suggestion.Confidence = Item.value("confidence", 0.70);
               Suggestion.Evidence = Item.value("evidence", "");
               Suggestion.Scope = Item.value("scope", "document");
               Suggestion.Provisional = Item.value("provisional", Suggestion.Confidence < 0.78);

               if (!Suggestion.Text.empty())
               {
                    Entry.Suggestions.push_back(std::move(Suggestion));
               }
          }

          It = ContextCache.emplace(Key, std::move(Entry)).first;
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

     if (Instance && Instance->Sam && Instance->Sam->IsOpen())
     {
          (void)Instance->Sam->RemoveDocumentContext(Collection, DocumentID);
     }
}

void llm::RemoveCollectionContexts(const std::string& Collection)
{
     if (Collection.empty())
     {
          return;
     }

     const std::string Prefix = Collection + "\n";
     std::lock_guard<std::mutex> Lock(ContextMutex);

     for (auto It = ContextCache.begin(); It != ContextCache.end(); )
     {
          It = It->first.rfind(Prefix, 0) == 0 ? ContextCache.erase(It) : std::next(It);
     }

     for (auto It = PendingContextKeys.begin(); It != PendingContextKeys.end(); )
     {
          It = It->rfind(Prefix, 0) == 0 ? PendingContextKeys.erase(It) : std::next(It);
     }

     PendingContextJobs.erase(
          std::remove_if(PendingContextJobs.begin(), PendingContextJobs.end(),
                         [&](const ContextJob& Job)
                         {
                              return Job.Collection == Collection;
                         }),
          PendingContextJobs.end());
}

size_t llm::GetPendingContextJobs() const
{
     std::lock_guard<std::mutex> Lock(ContextMutex);
     return PendingContextJobs.size();
}
