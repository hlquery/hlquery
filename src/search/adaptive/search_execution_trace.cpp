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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "search/adaptive/search_execution_trace.h"
#include "vendor/json/json.hpp"

uint64_t SearchExecutionRecorder::MeasureDurationUS(const std::chrono::steady_clock::time_point &StartTime)
{
     if (StartTime == std::chrono::steady_clock::time_point())
     {
          return 0;
     }

     const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - StartTime);

     return Elapsed.count() < 0 ? 0 : static_cast<uint64_t>(Elapsed.count());
}

std::string SearchExecutionRecorder::BoundedText(const std::string &Value, std::size_t MaximumBytes)
{
     if (Value.size() <= MaximumBytes)
     {
          return Value;
     }

     return Value.substr(0, MaximumBytes);
}

std::chrono::steady_clock::time_point SearchExecutionRecorder::Start(const SearchExecutionTrace &Trace)
{
     if (!Trace.Enabled)
     {
          return std::chrono::steady_clock::time_point();
     }

     return std::chrono::steady_clock::now();
}

void SearchExecutionRecorder::Append(SearchExecutionTrace *Trace,
                                     const std::string &Name,
                                     const std::string &Category,
                                     const std::string &Status,
                                     const std::string &Algorithm,
                                     const std::string &Reason,
                                     const std::chrono::steady_clock::time_point &StartTime,
                                     std::size_t CandidatesIn,
                                     std::size_t CandidatesOut,
                                     const std::string &QueryForm,
                                     const std::map<std::string, std::string> &Details)
{
     if (!Trace || !Trace->Enabled)
     {
          return;
     }

     SearchExecutionStage Stage;
     Stage.Order = static_cast<int>(Trace->Stages.size()) + 1;
     Stage.Name = BoundedText(Name, 64);
     Stage.Category = BoundedText(Category, 64);
     Stage.Status = BoundedText(Status, 32);
     Stage.Algorithm = BoundedText(Algorithm, 64);
     Stage.Reason = BoundedText(Reason, 256);
     Stage.DurationUS = MeasureDurationUS(StartTime);
     Stage.CandidatesIn = CandidatesIn;
     Stage.CandidatesOut = CandidatesOut;

     if (Trace->IncludeQueryText)
     {
          Stage.QueryForm = BoundedText(QueryForm, 512);
     }

     std::size_t DetailCount = 0;
     for (const auto &Detail : Details)
     {
          if (DetailCount >= 32)
          {
               break;
          }

          Stage.Details[BoundedText(Detail.first, 64)] = BoundedText(Detail.second, 256);
          ++DetailCount;
     }

     Trace->Stages.push_back(std::move(Stage));
}

void SearchExecutionRecorder::AppendSkipped(SearchExecutionTrace *Trace,
                                            const std::string &Name,
                                            const std::string &Category,
                                            const std::string &Reason)
{
     if (!Trace || !Trace->Enabled || !Trace->Detailed)
     {
          return;
     }

     Append(Trace,
            Name,
            Category,
            "skipped",
            "",
            Reason,
            std::chrono::steady_clock::time_point());
}

std::string SearchExecutionRecorder::Serialize(const SearchExecutionTrace &Trace)
{
     if (!Trace.Enabled)
     {
          return "{}";
     }

     nlohmann::json TraceJSON;
     TraceJSON["schema_version"] = Trace.SchemaVersion;
     TraceJSON["adaptive"] = Trace.Adaptive;
     TraceJSON["from_cache"] = Trace.FromCache;
     TraceJSON["planner"] = Trace.Planner;
     TraceJSON["intent"] = Trace.Intent;
     TraceJSON["difficulty"] = Trace.Difficulty;
     TraceJSON["final_strategy"] = Trace.FinalStrategy;
     TraceJSON["stages"] = nlohmann::json::array();

     for (const auto &Stage : Trace.Stages)
     {
          nlohmann::json StageJSON;
          StageJSON["order"] = Stage.Order;
          StageJSON["name"] = Stage.Name;
          StageJSON["category"] = Stage.Category;
          StageJSON["status"] = Stage.Status;
          StageJSON["duration_us"] = Stage.DurationUS;
          StageJSON["candidates_in"] = Stage.CandidatesIn;
          StageJSON["candidates_out"] = Stage.CandidatesOut;

          if (!Stage.Algorithm.empty())
          {
               StageJSON["algorithm"] = Stage.Algorithm;
          }

          if (!Stage.Reason.empty())
          {
               StageJSON["reason"] = Stage.Reason;
          }

          if (!Stage.QueryForm.empty())
          {
               StageJSON["query_form"] = Stage.QueryForm;
          }

          if (!Stage.Details.empty())
          {
               StageJSON["details"] = Stage.Details;
          }

          TraceJSON["stages"].push_back(std::move(StageJSON));
     }

     return TraceJSON.dump();
}

bool SearchExecutionRecorder::Inject(std::string *ResponseBody, const SearchExecutionTrace &Trace)
{
     if (!ResponseBody || !Trace.Enabled)
     {
          return false;
     }

     try
     {
          nlohmann::json ResponseJSON = nlohmann::json::parse(*ResponseBody);
          ResponseJSON["search_execution"] = nlohmann::json::parse(Serialize(Trace));
          *ResponseBody = ResponseJSON.dump();
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool SearchExecutionRecorder::MarkCached(std::string *ResponseBody, uint64_t CacheLookupDurationUS)
{
     if (!ResponseBody)
     {
          return false;
     }

     try
     {
          nlohmann::json ResponseJSON = nlohmann::json::parse(*ResponseBody);

          if (!ResponseJSON.contains("search_execution") ||
              !ResponseJSON["search_execution"].is_object())
          {
               return false;
          }

          nlohmann::json &TraceJSON = ResponseJSON["search_execution"];
          TraceJSON["from_cache"] = true;

          bool UpdatedCacheStage = false;

          if (TraceJSON.contains("stages") && TraceJSON["stages"].is_array())
          {
               for (auto &StageJSON : TraceJSON["stages"])
               {
                    if (!StageJSON.is_object())
                    {
                         continue;
                    }

                    if (StageJSON.value("name", "") == "cache_lookup")
                    {
                         StageJSON["status"] = "cached";
                         StageJSON["duration_us"] = CacheLookupDurationUS;
                         StageJSON["details"]["timing_scope"] = "current_request";
                         UpdatedCacheStage = true;
                    }
                    else
                    {
                         StageJSON["details"]["timing_scope"] = "original_execution";
                         StageJSON["details"]["cached_snapshot"] = "true";
                    }
               }
          }

          if (!UpdatedCacheStage)
          {
               nlohmann::json CacheStage = {
                    {"order", 1},
                    {"name", "cache_lookup"},
                    {"category", "cache"},
                    {"status", "cached"},
                    {"algorithm", "response_cache"},
                    {"duration_us", CacheLookupDurationUS},
                    {"candidates_in", 0},
                    {"candidates_out", 0},
                    {"details", {{"timing_scope", "current_request"}}}};

               if (!TraceJSON.contains("stages") || !TraceJSON["stages"].is_array())
               {
                    TraceJSON["stages"] = nlohmann::json::array();
               }

               TraceJSON["stages"].insert(TraceJSON["stages"].begin(), std::move(CacheStage));

               int Order = 1;
               for (auto &StageJSON : TraceJSON["stages"])
               {
                    if (StageJSON.is_object())
                    {
                         StageJSON["order"] = Order++;
                    }
               }
          }

          *ResponseBody = ResponseJSON.dump();
          return true;
     }
     catch (...)
     {
          return false;
     }
}
