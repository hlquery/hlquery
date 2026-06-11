/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include "core/llama_local.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "llama.h"

namespace
{
constexpr uint32_t LocalContextSize = 4096;
constexpr int DefaultThreads = 2;

struct ModelState
{
     llama_model* Model = nullptr;
     std::string Path;

     ~ModelState()
     {
          if (Model)
          {
               llama_model_free(Model);
          }
     }
};

struct ContextDeleter
{
     void operator()(llama_context* Context) const
     {
          llama_free(Context);
     }
};

struct SamplerDeleter
{
     void operator()(llama_sampler* Sampler) const
     {
          llama_sampler_free(Sampler);
     }
};

struct AbortState
{
     std::chrono::steady_clock::time_point Deadline;
};

std::mutex LocalLlamaMutex;
std::unique_ptr<ModelState> CachedModel;
std::once_flag BackendInitFlag;

static bool ShouldAbort(void* Opaque)
{
     const auto* State = static_cast<const AbortState*>(Opaque);
     return State && std::chrono::steady_clock::now() >= State->Deadline;
}

static int ReadThreadCount()
{
     const char* Value = std::getenv("HLQUERY_LLAMA_THREADS");

     if (!Value || !*Value)
     {
          return DefaultThreads;
     }

     char* End = nullptr;
     const long Parsed = std::strtol(Value, &End, 10);

     if (!End || *End != '\0' || Parsed < 1 || Parsed > 256)
     {
          return DefaultThreads;
     }

     return static_cast<int>(Parsed);
}

static int MaxTokensForMode(const std::string& Mode)
{
     if (Mode == "context")
     {
          return 320;
     }

     if (Mode == "search_intent")
     {
          return 820;
     }

     return 192;
}

static std::string SchemaForMode(const std::string& Mode)
{
     if (Mode == "context")
     {
          return R"({"collection_about":"...","document_role":"...","contexts":[{"term":"...","relation":"...","confidence":0.0,"evidence":"...","scope":"document"}]})";
     }

     if (Mode == "anchors")
     {
          return R"({"anchors":[{"text":"...","kind":"anchor","confidence":0.0,"reason":"...","language":"..."}]})";
     }

     return R"({"interpretation":"...","document_questions":[{"question":"...","required":true,"evidence_terms":["..."]}],"evidence_terms":[{"text":"...","weight":0.0}],"conclusion":"...","candidates":[{"text":"...","weight":0.0}],"ranked_terms":[{"text":"...","weight":0.0}]})";
}

static std::string BuildPrompt(const std::string& Mode, const std::string& Payload)
{
     std::string Prompt = "You improve retrieval for a local search engine. ";

     if (Mode == "search_intent")
     {
          Prompt +=
               "Given a query and candidate documents, first decide the concrete evidence questions each document must answer, then infer which candidate titles or terms the query may point to. "
               "For example, a location query should ask whether the candidate evidence actually mentions the requested city, metro, state, or an accepted nearby place. "
               "Use supplied evidence first, but you may also use common alias, spelling, location, institution, or world-knowledge associations when they are likely. "
               "If a query appears misspelled, infer the likely correction. Put required matching words or aliases in evidence_terms. Keep uncertain associations at lower weight. ";
     }
     else
     {
          Prompt += "Analyze the JSON payload using only its supplied evidence. Do not invent unsupported facts. ";
     }

     Prompt +=
          "Return exactly one compact JSON object on one line. Do not emit markdown or explanatory prose. "
          "Keep phrases short, specific, and useful for search.\n"
          "Mode: " + Mode + "\n"
          "Required schema: " + SchemaForMode(Mode) + "\n"
          "Payload:\n";

     constexpr size_t MaxPayloadBytes = 14000;
     Prompt.append(Payload, 0, std::min(Payload.size(), MaxPayloadBytes));
     return Prompt;
}

static bool ApplyChatTemplate(const llama_model* Model,
                              const std::string& UserPrompt,
                              std::string& Prompt)
{
     const llama_chat_message Message = {"user", UserPrompt.c_str()};
     const char* Template = llama_model_chat_template(Model, nullptr);
     const int32_t Required =
          llama_chat_apply_template(Template, &Message, 1, true, nullptr, 0);

     if (Required <= 0)
     {
          Prompt = UserPrompt;
          return true;
     }

     std::vector<char> Buffer(static_cast<size_t>(Required) + 1);
     const int32_t Written =
          llama_chat_apply_template(Template, &Message, 1, true, Buffer.data(), Buffer.size());

     if (Written < 0)
     {
          return false;
     }

     Prompt.assign(Buffer.data(), static_cast<size_t>(Written));
     return true;
}

static bool Tokenize(const llama_vocab* Vocab,
                     std::string& Prompt,
                     std::vector<llama_token>& Tokens,
                     int MaxPromptTokens)
{
     while (!Prompt.empty())
     {
          const int32_t Required =
               -llama_tokenize(Vocab, Prompt.c_str(), Prompt.size(), nullptr, 0, true, true);

          if (Required <= 0)
          {
               return false;
          }

          if (Required <= MaxPromptTokens)
          {
               Tokens.resize(static_cast<size_t>(Required));
               return llama_tokenize(Vocab,
                                     Prompt.c_str(),
                                     Prompt.size(),
                                     Tokens.data(),
                                     Tokens.size(),
                                     true,
                                     true) >= 0;
          }

          Prompt.resize(Prompt.size() * 9 / 10);
     }

     return false;
}

static bool AppendTokenPiece(const llama_vocab* Vocab,
                             llama_token Token,
                             std::string& Output)
{
     std::vector<char> Buffer(256);
     int32_t Written = llama_token_to_piece(Vocab, Token, Buffer.data(), Buffer.size(), 0, true);

     if (Written < 0)
     {
          Buffer.resize(static_cast<size_t>(-Written));
          Written = llama_token_to_piece(Vocab, Token, Buffer.data(), Buffer.size(), 0, true);
     }

     if (Written < 0)
     {
          return false;
     }

     Output.append(Buffer.data(), static_cast<size_t>(Written));
     return true;
}

static bool EnsureModel(const std::string& ModelPath, std::string& Error)
{
     if (CachedModel && CachedModel->Path == ModelPath)
     {
          return true;
     }

     auto NewState = std::make_unique<ModelState>();
     llama_model_params Params = llama_model_default_params();
     NewState->Model = llama_model_load_from_file(ModelPath.c_str(), Params);

     if (!NewState->Model)
     {
          Error = "unable to load local GGUF model: " + ModelPath;
          return false;
     }

     NewState->Path = ModelPath;
     CachedModel = std::move(NewState);
     return true;
}
}

LocalLlamaInferenceResult RunLocalLlamaInference(const std::string& ModelPath,
                                                 const std::string& Mode,
                                                 const std::string& Payload,
                                                 int TimeoutMS)
{
     LocalLlamaInferenceResult Result;
     std::lock_guard<std::mutex> Lock(LocalLlamaMutex);
     std::call_once(BackendInitFlag, llama_backend_init);

     if (!EnsureModel(ModelPath, Result.Error))
     {
          return Result;
     }

     Result.Started = true;
     const int MaxTokens = MaxTokensForMode(Mode);
     AbortState Abort{std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMS)};
     llama_context_params ContextParams = llama_context_default_params();
     ContextParams.n_ctx = LocalContextSize;
     ContextParams.n_batch = LocalContextSize;
     ContextParams.n_threads = ReadThreadCount();
     ContextParams.n_threads_batch = ContextParams.n_threads;
     ContextParams.abort_callback = ShouldAbort;
     ContextParams.abort_callback_data = &Abort;
     std::unique_ptr<llama_context, ContextDeleter> Context(
          llama_init_from_model(CachedModel->Model, ContextParams));

     if (!Context)
     {
          Result.Error = "unable to create local llama context";
          return Result;
     }

     const llama_vocab* Vocab = llama_model_get_vocab(CachedModel->Model);
     std::string Prompt;

     if (!ApplyChatTemplate(CachedModel->Model, BuildPrompt(Mode, Payload), Prompt))
     {
          Result.Error = "unable to apply local llama chat template";
          return Result;
     }

     std::vector<llama_token> PromptTokens;

     if (!Tokenize(Vocab, Prompt, PromptTokens, static_cast<int>(LocalContextSize) - MaxTokens))
     {
          Result.Error = "unable to tokenize local llama prompt";
          return Result;
     }

     llama_sampler_chain_params SamplerParams = llama_sampler_chain_default_params();
     std::unique_ptr<llama_sampler, SamplerDeleter> Sampler(llama_sampler_chain_init(SamplerParams));
     llama_sampler_chain_add(Sampler.get(), llama_sampler_init_top_k(20));
     llama_sampler_chain_add(Sampler.get(), llama_sampler_init_top_p(0.9F, 1));
     llama_sampler_chain_add(Sampler.get(), llama_sampler_init_temp(0.2F));
     llama_sampler_chain_add(Sampler.get(), llama_sampler_init_dist(1));

     llama_batch Batch = llama_batch_get_one(PromptTokens.data(), PromptTokens.size());

     for (int Generated = 0; Generated < MaxTokens; ++Generated)
     {
          if (llama_decode(Context.get(), Batch) != 0)
          {
               Result.TimedOut = ShouldAbort(&Abort);
               Result.Error = Result.TimedOut ? "local llama inference timed out" : "local llama decode failed";
               return Result;
          }

          llama_token Token = llama_sampler_sample(Sampler.get(), Context.get(), -1);

          if (llama_vocab_is_eog(Vocab, Token))
          {
               break;
          }

          if (!AppendTokenPiece(Vocab, Token, Result.Output))
          {
               Result.Error = "unable to convert local llama token to text";
               return Result;
          }

          Batch = llama_batch_get_one(&Token, 1);
     }

     Result.ExitCode = 0;
     return Result;
}
