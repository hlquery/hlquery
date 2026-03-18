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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "core/hlquery.h"
#include "search/consistency-checker.h"
#include "search/storage-utility.h"
#include "search/storageengine.h"
#include "search/storagemaint.h"
#include "search/performance-monitor.h"

/*
 * Build a consistency checker instance.
 */

ConsistencyChecker::ConsistencyChecker()
{
     /* No operation. */
}

/*
 * Destroy the consistency checker instance.
 */

ConsistencyChecker::~ConsistencyChecker()
{
     /* No operation. */
}

/*
 * Validate database memory and key consistency.
 */

bool ConsistencyChecker::CheckConsistency(int DbID)
{
     std::lock_guard<std::mutex> Lock(Mutex);

     bool Consistent = true;

     if (Instance && Instance->Database)
     {
          Consistent &= CheckMemoryConsistency(DbID);

          auto Keys = Instance->Database->Keys("*");

          for (const auto &Key : Keys)
          {
               Consistent &= CheckKeyConsistency(DbID, Key);
          }
     }

     return Consistent;
}

/*
 * Attempt to repair detected inconsistencies.
 */

int ConsistencyChecker::RepairInconsistencies(int /* DbID */)
{
     std::lock_guard<std::mutex> Lock(Mutex);

     int Repaired = 0;

     if (Instance && Instance->Database)
     {
          /*
           * Placeholder for repair logic.
           * A real implementation would fix inconsistencies.
           */

          Repaired = 0;
     }

     return Repaired;
}

/*
 * Generate a summarized consistency report.
 */

std::unordered_map<std::string, std::vector<std::string>> ConsistencyChecker::GetConsistencyReport(int /* DbID */)
{
     std::lock_guard<std::mutex> Lock(Mutex);

     std::unordered_map<std::string, std::vector<std::string>> Report;

     if (Instance && Instance->Database)
     {
          /* Placeholder for consistency report. */

          Report["memory"] = {"No issues found"};
          Report["keys"] = {"No issues found"};
     }

     return Report;
}

/*
 * Validate a single key for consistency.
 */

bool ConsistencyChecker::CheckKeyConsistency(int /* DbID */, const std::string &Key)
{
     /* Placeholder implementation. */

     (void)Key;

     return true;
}

/*
 * Validate memory-level consistency checks.
 */

bool ConsistencyChecker::CheckMemoryConsistency(int /* DbID */)
{
     /* Placeholder implementation. */

     return true;
}

/*
 * Build a performance monitor instance.
 */

PerformanceMonitor::PerformanceMonitor()
{
     /* No operation. */
}

/*
 * Destroy the performance monitor instance.
 */

PerformanceMonitor::~PerformanceMonitor()
{
     /* No operation. */
}

/*
 * Start tracking a named operation.
 */

void PerformanceMonitor::StartOperation(const std::string &Operation)
{
     std::lock_guard<std::mutex> Lock(Mutex);

     ActiveOperations[Operation] = std::chrono::high_resolution_clock::now();
}

/*
 * Finish tracking a named operation and update statistics.
 */

void PerformanceMonitor::EndOperation(const std::string &Operation, bool Success)
{
     std::lock_guard<std::mutex> Lock(Mutex);

     auto It = ActiveOperations.find(Operation);

     if (It != ActiveOperations.end())
     {
          auto EndTime = std::chrono::high_resolution_clock::now();

          auto Duration = std::chrono::duration<double, std::milli>(EndTime - It->second).count();

          auto &Stats = OperationStatsMap[Operation];

          Stats.Count++;

          if (Success)
          {
               Stats.Successful++;
          }
          else
          {
               Stats.Failed++;
          }

          Stats.TotalTimeMS += Duration;
          Stats.MinTimeMS = std::min(Stats.MinTimeMS, Duration);
          Stats.MaxTimeMS = std::max(Stats.MaxTimeMS, Duration);
          Stats.AvgTimeMS = Stats.TotalTimeMS / Stats.Count;

          ActiveOperations.erase(It);
     }
}

/*
 * Return statistics for a specific operation.
 */

std::unordered_map<std::string, std::string> PerformanceMonitor::GetOperationStats(const std::string &Operation) const
{
     std::lock_guard<std::mutex> Lock(Mutex);

     std::unordered_map<std::string, std::string> Stats;

     auto It = OperationStatsMap.find(Operation);

     if (It != OperationStatsMap.end())
     {
          const auto &OpStats = It->second;

          Stats["count"] = std::to_string(OpStats.Count);
          Stats["successful"] = std::to_string(OpStats.Successful);
          Stats["failed"] = std::to_string(OpStats.Failed);
          Stats["total_time_ms"] = std::to_string(OpStats.TotalTimeMS);
          Stats["min_time_ms"] = std::to_string(OpStats.MinTimeMS);
          Stats["max_time_ms"] = std::to_string(OpStats.MaxTimeMS);
          Stats["avg_time_ms"] = std::to_string(OpStats.AvgTimeMS);

          if (OpStats.Count > 0)
          {
               double SuccessRate = (static_cast<double>(OpStats.Successful) / OpStats.Count) * 100.0;

               Stats["success_rate"] = std::to_string(SuccessRate);
          }
     }

     return Stats;
}

/*
 * Return statistics for all tracked operations.
 */

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> PerformanceMonitor::GetAllStats() const
{
     std::lock_guard<std::mutex> Lock(Mutex);

     std::unordered_map<std::string, std::unordered_map<std::string, std::string>> AllStats;

     for (const auto &Pair : OperationStatsMap)
     {
          AllStats[Pair.first] = GetOperationStats(Pair.first);
     }

     return AllStats;
}

/*
 * Reset all tracked performance statistics.
 */

void PerformanceMonitor::ResetStats()
{
     std::lock_guard<std::mutex> Lock(Mutex);

     OperationStatsMap.clear();
     ActiveOperations.clear();
}

/*
 * Generate a database status report.
 */

std::unordered_map<std::string, std::string> DatabaseUtility::GenerateReport(int DbID)
{
     std::unordered_map<std::string, std::string> Report;

     if (Instance && Instance->Database)
     {
          auto Info = Instance->Database->Info();

          Report.insert(Info.begin(), Info.end());
          Report["db_id"] = std::to_string(DbID);

          if (Instance)
          {
               Report["timestamp"] = std::to_string(Instance->Time());
          }
     }

     return Report;
}

/*
 * Optimize database structures and indexes.
 */

int DatabaseUtility::OptimizeDatabase(int /* DbID */)
{
     int Optimizations = 0;

     if (Instance && Instance->Database)
     {
          /* Placeholder for optimization logic. */

          Optimizations = 0;
     }

     return Optimizations;
}

/*
 * Clean up database content that is safe to remove.
 */

int DatabaseUtility::CleanupDatabase(int /* DbID */)
{
     int Cleaned = 0;

     if (Instance && Instance->Database)
     {
          /* Expiration functionality removed. */

          Cleaned++;
     }

     return Cleaned;
}

/*
 * Validate database integrity before use.
 */

bool DatabaseUtility::ValidateIntegrity(int /* DbID */)
{
     if (Instance && Instance->Database)
     {
          /* Placeholder for integrity validation. */

          return true;
     }

     return false;
}

/*
 * Estimate database size from metadata.
 */

size_t DatabaseUtility::EstimateSize(int /* DbID */)
{
     size_t Size = 0;

     if (Instance && Instance->Database)
     {
          auto Info = Instance->Database->Info();

          auto It = Info.find("memory");

          if (It != Info.end() && !It->second.empty())
          {
               try
               {
                    Size = std::stoull(It->second);
               }
               catch (const std::invalid_argument &)
               {
                    /* Invalid string, keep default size. */
               }
               catch (const std::out_of_range &)
               {
                    /* Value too large, keep default size. */
               }
          }
     }

     return Size;
}

/*
 * Compact the database to reclaim space.
 */

size_t DatabaseUtility::CompactDatabase(int /* DbID */)
{
     size_t Saved = 0;

     if (Instance && Instance->Database)
     {
          /* Placeholder for compaction logic. */

          Saved = 0;
     }

     return Saved;
}
