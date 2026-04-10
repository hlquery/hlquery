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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/threadlimit.h"

class HLQueryMetrics
{
   public:

     struct MetricPoint
     {
          std::chrono::system_clock::time_point Timestamp;
          double Value;

          MetricPoint() : Value(0.0)
          {
          }

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
        public:

          MetricHistory();

          ~MetricHistory();

          void AddPoint(double value);

          std::vector<MetricPoint> GetPoints() const;

          std::vector<MetricPoint> GetPointsInRange(
               std::chrono::system_clock::time_point start,
               std::chrono::system_clock::time_point end) const;

          MetricPoint GetLatest() const;

          double GetLatestValue() const;

          std::size_t GetCount() const;

          double GetMin() const;

          double GetMax() const;

          double GetAverage() const;

          MetricWindowSummary GetWindowSummary(
               std::chrono::system_clock::time_point start,
               std::chrono::system_clock::time_point end) const;

          RetentionStats GetLastRetentionStats() const;

          void Clear();

          void PerformRetention();

        private:

          mutable std::mutex PointsMutex;

          std::vector<MetricPoint> Points;

          std::chrono::system_clock::time_point LastStorageTime;

          std::chrono::system_clock::time_point LastRetentionCheck;

          RetentionStats LastRetentionStats;

          static constexpr int STORAGE_INTERVAL_MINUTES = 30;

          static constexpr int RETENTION_CHECK_INTERVAL_MINUTES = 60;

          static constexpr int DAYS_BEFORE_DAILY_RETENTION = 30;

          std::chrono::system_clock::time_point GetCurrentTime() const;

          bool ShouldStore() const;

          bool ShouldPerformRetention() const;

          void PerformDailyRetention();

          void PerformMonthlyRetention();

          int64_t GetDayNumber(std::chrono::system_clock::time_point tp) const;

          bool IsSameDay(
               std::chrono::system_clock::time_point tp1,
               std::chrono::system_clock::time_point tp2) const;
     };

     MetricHistory CPUMetrics;

     MetricHistory MemoryMetrics;

     CPUAffinityManager Affinity;
};
