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
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/hlquery.h"
#include "runtime/threadlimit.h"

/* High-resolution performance counters for tracking application metrics */

class PerformanceCounters
{
   private:

     struct Counter
     {
          std::atomic<uint64_t> count{0};
          std::atomic<uint64_t> total_time_us{0};
          std::atomic<uint64_t> min_time_us{UINT64_MAX};
          std::atomic<uint64_t> max_time_us{0};
          std::atomic<uint64_t> last_time_us{0};
     };

     std::unordered_map<std::string, Counter> Counters;

     mutable std::mutex CountersMutex;

   public:

     /* Increments a specific performance counter by its name */

     void IncrementCounter(const std::string &Name)
     {
          std::lock_guard<std::mutex> Lock(CountersMutex);
          Counters[Name].count.fetch_add(1);
     }

     /* Records a timing duration for a named performance counter */

     void RecordTiming(const std::string &Name, uint64_t TimeUS)
     {
          std::lock_guard<std::mutex> Lock(CountersMutex);

          auto &CounterRef = Counters[Name];
          CounterRef.count.fetch_add(1);
          CounterRef.total_time_us.fetch_add(TimeUS);
          CounterRef.last_time_us.store(TimeUS);

          /* Update minimum and maximum timing values using atomic compare-and-swap */

          uint64_t CurrentMin = CounterRef.min_time_us.load();

          while (TimeUS < CurrentMin &&
                 !CounterRef.min_time_us.compare_exchange_weak(CurrentMin, TimeUS))
          {
               /* Retry until the atomic update succeeds */
          }

          uint64_t CurrentMax = CounterRef.max_time_us.load();

          while (TimeUS > CurrentMax &&
                 !CounterRef.max_time_us.compare_exchange_weak(CurrentMax, TimeUS))
          {
               /* Retry until the atomic update succeeds */
          }
     }

     /* Aggregated statistics for a specific performance counter */

     struct CounterStats
     {
          uint64_t count;
          uint64_t total_time_us;
          uint64_t min_time_us;
          uint64_t max_time_us;
          uint64_t last_time_us;
          double avg_time_us;
     };

     /* Retrieves the current statistics for a named counter */

     CounterStats GetCounterStats(const std::string &Name) const
     {
          std::lock_guard<std::mutex> Lock(CountersMutex);

          auto CounterIterator = Counters.find(Name);

          if (CounterIterator == Counters.end())
          {
               return CounterStats{};
          }

          const auto &CounterInstance = CounterIterator->second;

          CounterStats Stats;

          Stats.count = CounterInstance.count.load();
          Stats.total_time_us = CounterInstance.total_time_us.load();
          Stats.min_time_us = CounterInstance.min_time_us.load();
          Stats.max_time_us = CounterInstance.max_time_us.load();
          Stats.last_time_us = CounterInstance.last_time_us.load();

          if (Stats.count > 0)
          {
               Stats.avg_time_us = static_cast<double>(Stats.total_time_us) / Stats.count;
          }
          else
          {
               Stats.avg_time_us = 0.0;
          }

          return Stats;
     }

     /* Returns a list of all registered performance counter names */

     std::vector<std::string> GetCounterNames() const
     {
          std::lock_guard<std::mutex> Lock(CountersMutex);

          std::vector<std::string> CounterNamesList;

          CounterNamesList.reserve(Counters.size());

          for (const auto &[Name, CounterInstance] : Counters)
          {
               CounterNamesList.push_back(Name);
          }

          return CounterNamesList;
     }

     /* Resets all performance counters to their initial values */

     void ResetCounters()
     {
          std::lock_guard<std::mutex> Lock(CountersMutex);

          Counters.clear();
     }
};

/* Monitors system-level resource utilization like CPU and Memory */

class SystemResourceMonitor
{
   private:

     std::atomic<uint64_t> CPUUsagePercent{0};

     std::atomic<uint64_t> MemoryUsageBytes{0};

     std::atomic<uint64_t> DiskIOBytes{0};

     std::atomic<uint64_t> NetworkIOBytes{0};

     std::thread MonitorThread;

     std::atomic<bool> ShutdownValue{false};

   public:

     /* Constructor that initiates the background resource monitoring thread */

     SystemResourceMonitor()
     {
          bool RegisteredFlag = ThreadLimit::TryAcquireThreadSlot();

          if (RegisteredFlag)
          {
               MonitorThread = std::thread([this]()
                                           {
                                                ThreadLimit::SetThreadName("hlquery:perfmon");
                                                MonitorResources();
                                           });
          }
     }

     /* Destructor that handles graceful termination of the monitoring thread */

     ~SystemResourceMonitor()
     {
          ShutdownValue.store(true);

          if (MonitorThread.joinable())
          {
               MonitorThread.join();

               ThreadLimit::DecrementThreadCount();
          }
     }

     /* The primary background monitoring loop that periodically samples resource state */

     void MonitorResources()
     {
          while (!ShutdownValue.load())
          {
               try
               {
                    MonitorCPUUsage();

                    MonitorMemoryUsage();

                    MonitorDiskIO();

                    MonitorNetworkIO();

                    std::this_thread::sleep_for(std::chrono::seconds(1));
               }
               catch (const std::exception &e)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("performance_monitor", "Resource monitoring error: " + std::string(e.what()) + ".");
                    }
               }
          }
     }

   private:

     /* Samples CPU utilization by parsing the /proc/stat system file */

     void MonitorCPUUsage()
     {
          std::ifstream StatFileStream("/proc/stat");

          std::string LineContent;

          if (std::getline(StatFileStream, LineContent))
          {
               std::istringstream Iss(LineContent);

               std::string CPULabel;

               uint64_t UserVal, NiceVal, SystemVal, IdleVal, IOWaitVal, IRQVal, SoftIRQVal, StealVal;

               Iss >> CPULabel >> UserVal >> NiceVal >> SystemVal >> IdleVal >> IOWaitVal >> IRQVal >> SoftIRQVal >> StealVal;

               if (CPULabel == "cpu")
               {
                    uint64_t TotalIdleTime = IdleVal + IOWaitVal;

                    uint64_t TotalNonIdleTime = UserVal + NiceVal + SystemVal + IRQVal + SoftIRQVal + StealVal;

                    uint64_t TotalTime = TotalIdleTime + TotalNonIdleTime;

                    static uint64_t PrevTotalTime = 0;

                    static uint64_t PrevIdleTime = 0;

                    if (PrevTotalTime > 0)
                    {
                         uint64_t TotalDelta = TotalTime - PrevTotalTime;

                         uint64_t IdleDelta = TotalIdleTime - PrevIdleTime;

                         if (TotalDelta > 0)
                         {
                              uint64_t CPUPercentResult = 100 - (IdleDelta * 100 / TotalDelta);

                              CPUUsagePercent.store(CPUPercentResult);
                         }
                    }

                    PrevTotalTime = TotalTime;

                    PrevIdleTime = TotalIdleTime;
               }
          }
     }

     /* Monitors system memory utilization by parsing the /proc/meminfo file */

     void MonitorMemoryUsage()
     {
          std::ifstream MemInfoFileStream("/proc/meminfo");

          std::string LineContent;

          uint64_t TotalMemoryValue = 0;

          uint64_t AvailableMemoryValue = 0;

          while (std::getline(MemInfoFileStream, LineContent))
          {
               if (LineContent.find("MemTotal:") == 0)
               {
                    std::istringstream Iss(LineContent);

                    std::string LabelStr, ValueStr, UnitStr;

                    Iss >> LabelStr >> ValueStr >> UnitStr;

                    TotalMemoryValue = std::stoull(ValueStr) * 1024;
               }
               else if (LineContent.find("MemAvailable:") == 0)
               {
                    std::istringstream Iss(LineContent);

                    std::string LabelStr, ValueStr, UnitStr;

                    Iss >> LabelStr >> ValueStr >> UnitStr;

                    AvailableMemoryValue = std::stoull(ValueStr) * 1024;
               }
          }

          if (TotalMemoryValue > 0)
          {
               uint64_t UsedMemoryBytes = TotalMemoryValue - AvailableMemoryValue;

               MemoryUsageBytes.store(UsedMemoryBytes);
          }
     }

     /* Monitors disk I/O throughput by parsing the /proc/diskstats file */

     void MonitorDiskIO()
     {
          std::ifstream DiskStatsFileStream("/proc/diskstats");

          std::string LineContent;

          uint64_t TotalReadBytesValue = 0;

          uint64_t TotalWriteBytesValue = 0;

          while (std::getline(DiskStatsFileStream, LineContent))
          {
               std::istringstream Iss(LineContent);

               unsigned int MajorNumber = 0;

               unsigned int MinorNumber = 0;

               std::string DeviceName;

               uint64_t ReadsCount, ReadMergesCount, ReadSectorsCount, ReadTicksCount;

               uint64_t WritesCount, WriteMergesCount, WriteSectorsCount, WriteTicksCount;

               if (Iss >> MajorNumber >> MinorNumber >> DeviceName >> ReadsCount >> ReadMergesCount >>
                   ReadSectorsCount >> ReadTicksCount >> WritesCount >> WriteMergesCount >>
                   WriteSectorsCount >> WriteTicksCount)
               {
                    /* Only aggregate data for primary disk devices */

                    const bool IsLetterSuffixedDisk =
                         (DeviceName.rfind("sd", 0) == 0 || DeviceName.rfind("hd", 0) == 0) &&
                         DeviceName.size() > 2 &&
                         std::all_of(DeviceName.begin() + 2, DeviceName.end(),
                                     [](unsigned char CharacterValue)
                                     {
                                          return std::isalpha(CharacterValue) != 0;
                                     });

                    const bool IsNVMeNamespace = DeviceName.rfind("nvme", 0) == 0 &&
                                                  DeviceName.find('n', 4) != std::string::npos &&
                                                  DeviceName.find('p', 4) == std::string::npos;

                    if (IsLetterSuffixedDisk || IsNVMeNamespace)
                    {
                         TotalReadBytesValue += ReadSectorsCount * 512;

                         TotalWriteBytesValue += WriteSectorsCount * 512;
                    }
               }
          }

          DiskIOBytes.store(TotalReadBytesValue + TotalWriteBytesValue);
     }

     /* Monitors network I/O throughput by parsing the /proc/net/dev file */

     void MonitorNetworkIO()
     {
          std::ifstream NetDevFileStream("/proc/net/dev");

          std::string LineContent;

          uint64_t TotalRxBytesValue = 0;

          uint64_t TotalTxBytesValue = 0;

          /* Skip the header lines in the net/dev file */

          std::getline(NetDevFileStream, LineContent);

          std::getline(NetDevFileStream, LineContent);

          while (std::getline(NetDevFileStream, LineContent))
          {
               std::istringstream Iss(LineContent);

               std::string InterfaceName;

               std::array<uint64_t, 16> InterfaceCounters{};

               if (!(Iss >> InterfaceName))
               {
                    continue;
               }

               bool ParsedAllCounters = true;

               for (auto &CounterValue : InterfaceCounters)
               {
                    if (!(Iss >> CounterValue))
                    {
                         ParsedAllCounters = false;
                         break;
                    }
               }

               if (ParsedAllCounters)
               {
                    if (InterfaceName.back() == ':')
                    {
                         InterfaceName.pop_back();
                    }

                    /* Filter out loopback and virtual virtualization interfaces */

                    if (InterfaceName != "lo" && InterfaceName.find("veth") == std::string::npos &&
                        InterfaceName.find("docker") == std::string::npos)
                    {
                         TotalRxBytesValue += InterfaceCounters[0];

                         TotalTxBytesValue += InterfaceCounters[8];
                    }
               }
          }

          NetworkIOBytes.store(TotalRxBytesValue + TotalTxBytesValue);
     }

   public:

     /* Aggregated resource statistics structure */

     struct ResourceStats
     {
          uint64_t cpu_usage_percent;
          uint64_t memory_usage_bytes;
          uint64_t disk_io_bytes;
          uint64_t network_io_bytes;
     };

     /* Returns a snapshot of current system resource utilization */

     ResourceStats GetStats() const
     {
          ResourceStats FinalStats;

          FinalStats.cpu_usage_percent = CPUUsagePercent.load();

          FinalStats.memory_usage_bytes = MemoryUsageBytes.load();

          FinalStats.disk_io_bytes = DiskIOBytes.load();

          FinalStats.network_io_bytes = NetworkIOBytes.load();

          return FinalStats;
     }
};

/* Component for measuring and profiling function execution performance */

struct PerformanceProfileEntry
{
     std::string function_name;
     uint64_t total_time_us;
     uint64_t call_count;
     uint64_t min_time_us;
     uint64_t max_time_us;
};

class PerformanceProfiler
{
   private:

     std::unordered_map<std::string, PerformanceProfileEntry> Profiles;

     mutable std::mutex ProfilesMutex;

   public:

     using ProfileEntry = PerformanceProfileEntry;

     /* Begins a profiling session for a named function */

     void StartProfile(const std::string & /*FunctionName*/)
     {
          /* Placeholder for future profiling logic */
     }

     /* Ends a profiling session and records the measured duration */

     void EndProfile(const std::string & /*FunctionName*/, uint64_t /*TimeUS*/)
     {
          /* Placeholder for future profiling logic */
     }

     /* Retrieves profiling data for a specific function */

     ProfileEntry GetProfile(const std::string & /*FunctionName*/) const
     {
          return ProfileEntry{};
     }

     /* Clears all accumulated profiling data */

     void ResetProfiles()
     {
          std::lock_guard<std::mutex> Lock(ProfilesMutex);

          Profiles.clear();
     }
};

/* Global performance monitoring subsystem components */

static std::unique_ptr<PerformanceCounters> GlobalPerformanceCounters;

static std::unique_ptr<SystemResourceMonitor> GlobalResourceMonitor;

/* Initializes the global performance monitoring infrastructure */

void InitializePerformanceMonitoring()
{
     GlobalPerformanceCounters = std::make_unique<PerformanceCounters>();

     GlobalResourceMonitor = std::make_unique<SystemResourceMonitor>();
}

/* Gracefully terminates the performance monitoring infrastructure */

void ShutdownPerformanceMonitoring()
{
     GlobalResourceMonitor.reset();

     GlobalPerformanceCounters.reset();
}

/* Accessor for the global high-resolution performance counters */

PerformanceCounters *GetPerformanceCounters()
{
     return GlobalPerformanceCounters.get();
}

/* Returns current system-wide resource utilization statistics */

SystemResourceMonitor::ResourceStats GetResourceStats()
{
     if (GlobalResourceMonitor)
     {
          return GlobalResourceMonitor->GetStats();
     }

     return SystemResourceMonitor::ResourceStats{};
}
