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
#include <cmath>
#include <ctime>
#include <limits>

#include "core/hlquery.h"
#include "core/metrics.h"

HLQueryMetrics::MetricHistory::MetricHistory()
    : LastStorageTime(std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()))),
      LastRetentionCheck(std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs())))
{
}

std::chrono::system_clock::time_point HLQueryMetrics::MetricHistory::GetCurrentTime() const
{
     if (Instance)
     {
          return std::chrono::system_clock::time_point(std::chrono::milliseconds(Instance->NowMs()));
     }

     return std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()));
}

void HLQueryMetrics::MetricHistory::AddPoint(double Value)
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     const auto NowTime = GetCurrentTime();

     if (!ShouldStore(NowTime))
     {
          return;
     }

     Points.emplace_back(NowTime, Value);

     LastStorageTime = NowTime;

     if (ShouldPerformRetention(NowTime))
     {
          PerformRetentionUnlocked(NowTime);

          LastRetentionCheck = NowTime;
     }
}

std::vector<HLQueryMetrics::MetricPoint> HLQueryMetrics::MetricHistory::GetPoints() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     return Points;
}

std::vector<HLQueryMetrics::MetricPoint> HLQueryMetrics::MetricHistory::GetPointsInRange(
     std::chrono::system_clock::time_point StartTime,
     std::chrono::system_clock::time_point EndTime) const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     std::vector<MetricPoint> ResultList;

     const auto FirstPoint = std::lower_bound(Points.begin(), Points.end(), StartTime,
                                              [](const MetricPoint &PointItem, const auto &Timestamp)
                                              {
                                                   return PointItem.Timestamp < Timestamp;
                                              });

     const auto LastPoint = std::upper_bound(FirstPoint, Points.end(), EndTime,
                                             [](const auto &Timestamp, const MetricPoint &PointItem)
                                             {
                                                  return Timestamp < PointItem.Timestamp;
                                             });

     ResultList.reserve(static_cast<std::size_t>(std::distance(FirstPoint, LastPoint)));

     for (auto PointIt = FirstPoint; PointIt != LastPoint; ++PointIt)
     {
          ResultList.push_back(*PointIt);
     }

     return ResultList;
}

HLQueryMetrics::MetricPoint HLQueryMetrics::MetricHistory::GetLatest() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     if (Points.empty())
     {
          return MetricPoint();
     }

     return Points.back();
}

double HLQueryMetrics::MetricHistory::GetLatestValue() const
{
     return GetLatest().Value;
}

std::size_t HLQueryMetrics::MetricHistory::GetCount() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     return Points.size();
}

double HLQueryMetrics::MetricHistory::GetMin() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     if (Points.empty())
     {
          return 0.0;
     }

     double MinValue = std::numeric_limits<double>::max();

     for (const auto &PointItem : Points)
     {
          MinValue = std::min(MinValue, PointItem.Value);
     }

     return MinValue;
}

double HLQueryMetrics::MetricHistory::GetMax() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     if (Points.empty())
     {
          return 0.0;
     }

     double MaxValue = std::numeric_limits<double>::lowest();

     for (const auto &PointItem : Points)
     {
          MaxValue = std::max(MaxValue, PointItem.Value);
     }

     return MaxValue;
}

double HLQueryMetrics::MetricHistory::GetAverage() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     if (Points.empty())
     {
          return 0.0;
     }

     double TotalValue = 0.0;

     for (const auto &PointItem : Points)
     {
          TotalValue += PointItem.Value;
     }

     return TotalValue / static_cast<double>(Points.size());
}

HLQueryMetrics::MetricWindowSummary HLQueryMetrics::MetricHistory::GetWindowSummary(
     std::chrono::system_clock::time_point StartTime,
     std::chrono::system_clock::time_point EndTime) const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     MetricWindowSummary Summary;

     if (EndTime < StartTime)
     {
          return Summary;
     }

     const auto FirstPoint = std::lower_bound(Points.begin(), Points.end(), StartTime,
                                              [](const MetricPoint &PointItem, const auto &Timestamp)
                                              {
                                                   return PointItem.Timestamp < Timestamp;
                                              });

     const auto LastPoint = std::upper_bound(FirstPoint, Points.end(), EndTime,
                                             [](const auto &Timestamp, const MetricPoint &PointItem)
                                             {
                                                  return Timestamp < PointItem.Timestamp;
                                             });

     double TotalValue = 0.0;

     for (auto PointIt = FirstPoint; PointIt != LastPoint; ++PointIt)
     {
          const auto &PointItem = *PointIt;

          if (!Summary.HasPoints)
          {
               Summary.HasPoints = true;
               Summary.Start = PointItem.Timestamp;
               Summary.End = PointItem.Timestamp;
               Summary.Min = PointItem.Value;
               Summary.Max = PointItem.Value;
          }
          else
          {
               if (PointItem.Timestamp < Summary.Start)
               {
                    Summary.Start = PointItem.Timestamp;
               }

               if (PointItem.Timestamp > Summary.End)
               {
                    Summary.End = PointItem.Timestamp;
               }

               Summary.Min = std::min(Summary.Min, PointItem.Value);
               Summary.Max = std::max(Summary.Max, PointItem.Value);
          }

          Summary.Latest = PointItem.Value;
          ++Summary.Count;
          TotalValue += PointItem.Value;
     }

     if (Summary.HasPoints && Summary.Count > 0)
     {
          Summary.Average = TotalValue / static_cast<double>(Summary.Count);
     }

     return Summary;
}

HLQueryMetrics::RetentionStats HLQueryMetrics::MetricHistory::GetLastRetentionStats() const
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     return LastRetentionStats;
}

void HLQueryMetrics::MetricHistory::Clear()
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     Points.clear();

     const auto NowTime = GetCurrentTime();

     LastStorageTime = NowTime;
     LastRetentionCheck = NowTime;
     LastRetentionStats = RetentionStats();
}

void HLQueryMetrics::MetricHistory::PerformRetention()
{
     std::lock_guard<std::mutex> Lock(PointsMutex);

     const auto NowTime = GetCurrentTime();
     PerformRetentionUnlocked(NowTime);
     LastRetentionCheck = NowTime;
}

void HLQueryMetrics::MetricHistory::PerformRetentionUnlocked(std::chrono::system_clock::time_point NowTime)
{
     if (Points.empty())
     {
          return;
     }

     auto OldestPointTimestamp = Points.front().Timestamp;

     auto DurationVal = std::chrono::duration_cast<std::chrono::hours>(NowTime - OldestPointTimestamp);

     int DaysCount = DurationVal.count() / 24;

     const std::size_t PointsBefore = Points.size();
     RetentionMode Mode = RetentionMode::None;

     if (DaysCount >= DAYS_BEFORE_DAILY_RETENTION)
     {
          Mode = RetentionMode::Monthly;
          PerformMonthlyRetention();
     }
     else
     {
          Mode = RetentionMode::Daily;
          PerformDailyRetention();
     }

     LastRetentionStats.HasRun = true;
     LastRetentionStats.Mode = Mode;
     LastRetentionStats.PointsBefore = PointsBefore;
     LastRetentionStats.PointsAfter = Points.size();
     LastRetentionStats.PointsDropped = (PointsBefore >= Points.size()) ? (PointsBefore - Points.size()) : 0;
     LastRetentionStats.ExecutedAt = NowTime;
}

bool HLQueryMetrics::MetricHistory::ShouldStore(std::chrono::system_clock::time_point NowTime) const
{
     if (Points.empty())
     {
          return true;
     }

     auto ElapsedTime = std::chrono::duration_cast<std::chrono::minutes>(
          NowTime - LastStorageTime);

     return ElapsedTime.count() >= STORAGE_INTERVAL_MINUTES;
}

bool HLQueryMetrics::MetricHistory::ShouldPerformRetention(std::chrono::system_clock::time_point NowTime) const
{
     auto ElapsedTime = std::chrono::duration_cast<std::chrono::minutes>(
          NowTime - LastRetentionCheck);

     if (ElapsedTime.count() >= RETENTION_CHECK_INTERVAL_MINUTES)
     {
          if (!IsSameDay(NowTime, LastRetentionCheck))
          {
               return true;
          }
     }

     return false;
}

void HLQueryMetrics::MetricHistory::PerformDailyRetention()
{
     if (Points.size() < 2)
     {
          return;
     }

     const auto NowTime = GetCurrentTime();

     auto TimeValue = std::chrono::system_clock::to_time_t(NowTime);

     struct tm TmBuf;

     std::tm *TmPtr = localtime_r(&TimeValue, &TmBuf);

     if (!TmPtr)
     {
          return;
     }

     TmPtr->tm_hour = 0;
     TmPtr->tm_min = 0;
     TmPtr->tm_sec = 0;

     auto TodayStartTimestamp = std::chrono::system_clock::from_time_t(std::mktime(TmPtr));

     std::vector<MetricPoint> TodayPointsList;
     std::vector<MetricPoint> OlderPointsList;

     for (const auto &PointItem : Points)
     {
          if (PointItem.Timestamp >= TodayStartTimestamp)
          {
               TodayPointsList.push_back(PointItem);
          }
          else
          {
               OlderPointsList.push_back(PointItem);
          }
     }

     if (TodayPointsList.size() > 48)
     {
          std::vector<MetricPoint> ReducedTodayList;

          for (size_t i = 0; i < TodayPointsList.size(); i += 2)
          {
               ReducedTodayList.push_back(TodayPointsList[i]);
          }

          TodayPointsList = std::move(ReducedTodayList);
     }

     std::vector<MetricPoint> ReducedOlderList;

     for (size_t i = 0; i < OlderPointsList.size(); i += 2)
     {
          ReducedOlderList.push_back(OlderPointsList[i]);
     }

     Points.clear();

     Points.insert(Points.end(), ReducedOlderList.begin(), ReducedOlderList.end());
     Points.insert(Points.end(), TodayPointsList.begin(), TodayPointsList.end());
}

void HLQueryMetrics::MetricHistory::PerformMonthlyRetention()
{
     if (Points.empty())
     {
          return;
     }

     std::vector<MetricPoint> RetainedPointsList;

     int64_t CurrentDayValue = -1;

     MetricPoint BestPointForDay;

     bool HasPointForDayFlag = false;

     for (const auto &PointItem : Points)
     {
          int64_t PointDayNum = GetDayNumber(PointItem.Timestamp);

          if (PointDayNum != CurrentDayValue)
          {
               if (HasPointForDayFlag)
               {
                    RetainedPointsList.push_back(BestPointForDay);
               }

               CurrentDayValue = PointDayNum;

               BestPointForDay = PointItem;

               HasPointForDayFlag = true;
          }
          else
          {
               auto PointTimeVal = std::chrono::system_clock::to_time_t(PointItem.Timestamp);

               struct tm TmBufPoint;

               std::tm *TmPtrPoint = localtime_r(&PointTimeVal, &TmBufPoint);

               if (!TmPtrPoint)
               {
                    continue;
               }

               int PointHourVal = TmPtrPoint->tm_hour;

               auto BestTimeVal = std::chrono::system_clock::to_time_t(BestPointForDay.Timestamp);

               struct tm TmBufBest;

               std::tm *TmPtrBest = localtime_r(&BestTimeVal, &TmBufBest);

               if (!TmPtrBest)
               {
                    BestPointForDay = PointItem;
                    continue;
               }

               int BestHourVal = TmPtrBest->tm_hour;

               int PointDistanceVal = std::abs(PointHourVal - 12);

               int BestDistanceVal = std::abs(BestHourVal - 12);

               if (PointDistanceVal < BestDistanceVal ||
                   (PointDistanceVal == BestDistanceVal && PointItem.Timestamp > BestPointForDay.Timestamp))
               {
                    BestPointForDay = PointItem;
               }
          }
     }

     if (HasPointForDayFlag)
     {
          RetainedPointsList.push_back(BestPointForDay);
     }

     Points = std::move(RetainedPointsList);
}

int64_t HLQueryMetrics::MetricHistory::GetDayNumber(std::chrono::system_clock::time_point TimestampPoint) const
{
     auto TimeValue = std::chrono::system_clock::to_time_t(TimestampPoint);

     struct tm TmBuf;

     const std::tm *TmPtr = localtime_r(&TimeValue, &TmBuf);

     if (!TmPtr)
     {
          return TimeValue / 86400;
     }

     return static_cast<int64_t>(TmPtr->tm_year) * 366 + TmPtr->tm_yday;
}

bool HLQueryMetrics::MetricHistory::IsSameDay(
     std::chrono::system_clock::time_point FirstTimestamp,
     std::chrono::system_clock::time_point SecondTimestamp) const
{
     return GetDayNumber(FirstTimestamp) == GetDayNumber(SecondTimestamp);
}
