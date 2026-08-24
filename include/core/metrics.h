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
#include <mutex>
#include <vector>

#include "common/searchpool.h"
#include "runtime/threadlimit.h"

class HLQueryMetrics
{
   public:

     struct MetricPoint
     {
          std::chrono::system_clock::time_point Timestamp;
          double Value;

          /* Initializes an empty metric point. */

          MetricPoint() : Value(0.0)
          {

          }

          /* Initializes a metric point with a timestamp and value. */

          MetricPoint(std::chrono::system_clock::time_point ts, double val)
              : Timestamp(ts), Value(val)
          {

          }
     };

     struct MetricWindowSummary
     {
          bool HasPoints = false;
          std::size_t Count = 0;

          double Min = 0.0;
          double Max = 0.0;

          double Average = 0.0;
          double Latest = 0.0;

          std::chrono::system_clock::time_point Start;
          std::chrono::system_clock::time_point End;
     };

     enum class RetentionMode
     {
          None,
          Daily,
          Monthly
     };

     struct RetentionStats
     {
          bool HasRun = false;
          RetentionMode Mode = RetentionMode::None;

          std::size_t PointsBefore = 0;
          std::size_t PointsAfter = 0;

          std::size_t PointsDropped = 0;
          std::chrono::system_clock::time_point ExecutedAt;
     };

     class MetricHistory
     {
        private:

          mutable std::mutex PointsMutex;

          std::vector<MetricPoint> Points;

          std::chrono::system_clock::time_point LastStorageTime;

          std::chrono::system_clock::time_point LastRetentionCheck;

          RetentionStats LastRetentionStats;

          static constexpr int STORAGE_INTERVAL_MINUTES = 30;

          static constexpr int RETENTION_CHECK_INTERVAL_MINUTES = 60;

          static constexpr int DAYS_BEFORE_DAILY_RETENTION = 30;

          /* Returns the current wall-clock time. */

          std::chrono::system_clock::time_point GetCurrentTime() const;

          /* Returns whether another point should be stored. */

          bool ShouldStore(std::chrono::system_clock::time_point now) const;

          /* Returns whether another retention pass should run. */

          bool ShouldPerformRetention(std::chrono::system_clock::time_point now) const;

          /* Performs retention without acquiring the history lock. */

          void PerformRetentionUnlocked(std::chrono::system_clock::time_point now);

          /* Compacts old points into daily samples. */

          void PerformDailyRetention();

          /* Compacts old points into monthly samples. */

          void PerformMonthlyRetention();

          /* Converts a timestamp into a local day number. */

          int64_t GetDayNumber(std::chrono::system_clock::time_point tp) const;

          /* Returns whether two timestamps share a local calendar day. */

          bool IsSameDay(
               std::chrono::system_clock::time_point tp1,
               std::chrono::system_clock::time_point tp2) const;

        public:

          /* Initializes an empty metric history. */

          MetricHistory();

          /* Destroys the metric history. */

          ~MetricHistory() = default;

          /* Adds one metric point when the storage interval allows it. */

          void AddPoint(double value);

          /* Returns all retained points. */

          std::vector<MetricPoint> GetPoints() const;

          /* Returns retained points within a time range. */

          std::vector<MetricPoint> GetPointsInRange(
               std::chrono::system_clock::time_point start,
               std::chrono::system_clock::time_point end) const;

          /* Returns the latest retained point. */

          MetricPoint GetLatest() const;

          /* Returns the latest retained value. */

          double GetLatestValue() const;

          /* Returns the retained point count. */

          std::size_t GetCount() const;

          /* Returns the minimum retained value. */

          double GetMin() const;

          /* Returns the maximum retained value. */

          double GetMax() const;

          /* Returns the average retained value. */

          double GetAverage() const;

          /* Summarizes retained points within a time range. */

          MetricWindowSummary GetWindowSummary(
               std::chrono::system_clock::time_point start,
               std::chrono::system_clock::time_point end) const;

          /* Returns statistics from the latest retention pass. */

          RetentionStats GetLastRetentionStats() const;

          /* Clears retained points and resets retention timestamps. */

          void Clear();

          /* Performs a retention pass. */

          void PerformRetention();
     };

     MetricHistory CPUMetrics;

     MetricHistory MemoryMetrics;

     CPUAffinityManager Affinity;
};
