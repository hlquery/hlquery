/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
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
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

/* OperationStats captures performance metrics for an operation. */

struct OperationStats
{
     size_t Count = 0;

     size_t Successful = 0;

     size_t Failed = 0;

     double TotalTimeMS = 0.0;

     /* MinTimeMS stores the minimum observed duration. */

     double MinTimeMS = std::numeric_limits<double>::max();

     double MaxTimeMS = 0.0;

     double AvgTimeMS = 0.0;
};

/* PerformanceMonitor tracks operation timings and success rates. */

class PerformanceMonitor
{
   private:

     /* Mutex guards stats access. */

     mutable std::mutex Mutex;

     /* ActiveOperations tracks operation start times. */

     std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> ActiveOperations;

     /* OperationStatsMap stores stats per operation name. */

     std::unordered_map<std::string, OperationStats> OperationStatsMap;

   public:

     /* Constructor. */

     PerformanceMonitor();

     /* Destructor. */

     ~PerformanceMonitor();

     /* StartOperation records a start time for an operation. */

     void StartOperation(const std::string& Operation);

     /* EndOperation records the completion of an operation. */

     void EndOperation(const std::string& Operation, bool Success);

     /* GetOperationStats returns stats for a single operation. */

     std::unordered_map<std::string, std::string> GetOperationStats(const std::string& Operation) const;

     /* GetAllStats returns stats for all operations. */

     std::unordered_map<std::string, std::unordered_map<std::string, std::string>> GetAllStats() const;

     /* ResetStats clears all collected stats. */

     void ResetStats();
};
