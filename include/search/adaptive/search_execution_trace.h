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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct SearchExecutionStage
{
     int Order = 0;
     std::string Name;
     std::string Category;
     std::string Status;
     std::string Algorithm;
     std::string Reason;
     uint64_t DurationUS = 0;
     std::size_t CandidatesIn = 0;
     std::size_t CandidatesOut = 0;
     std::string QueryForm;
     std::map<std::string, std::string> Details;
};

struct SearchExecutionTrace
{
     int SchemaVersion = 1;
     bool Enabled = false;
     bool Adaptive = false;
     bool FromCache = false;
     bool Detailed = false;
     bool IncludeQueryText = false;
     std::string Planner = "legacy";
     std::string Intent = "not_evaluated";
     std::string Difficulty = "not_evaluated";
     std::string FinalStrategy;
     std::vector<SearchExecutionStage> Stages;
};

class SearchExecutionRecorder
{
   private:

     static uint64_t MeasureDurationUS(const std::chrono::steady_clock::time_point &StartTime);
     static std::string BoundedText(const std::string &Value, std::size_t MaximumBytes);

   public:

     static std::chrono::steady_clock::time_point Start(const SearchExecutionTrace &Trace);

     static void Append(SearchExecutionTrace *Trace,
                        const std::string &Name,
                        const std::string &Category,
                        const std::string &Status,
                        const std::string &Algorithm,
                        const std::string &Reason,
                        const std::chrono::steady_clock::time_point &StartTime,
                        std::size_t CandidatesIn = 0,
                        std::size_t CandidatesOut = 0,
                        const std::string &QueryForm = "",
                        const std::map<std::string, std::string> &Details = {});

     static void AppendSkipped(SearchExecutionTrace *Trace,
                               const std::string &Name,
                               const std::string &Category,
                               const std::string &Reason);

     static std::string Serialize(const SearchExecutionTrace &Trace);
     static bool Inject(std::string *ResponseBody, const SearchExecutionTrace &Trace);
     static bool MarkCached(std::string *ResponseBody, uint64_t CacheLookupDurationUS);
};
