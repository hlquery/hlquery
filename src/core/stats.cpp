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

#include "core/hlquery.h"
#include "core/stats.h"

ServerStats::ServerStats()
{
     RestartCount = 0;
     LastRestartTimestamp = 0;
     HealthDegraded.store(false);
     HealthDegradedReason = "";
     DirtyShutdownDetected = false;
     StartupTime = 0;
}

/* Initiates the statistics tracking subsystem and records the primary startup state */

void ServerStats::Start()
{
     if (Instance)
     {
          StartupTime = Instance->Time();

          StartupState InitialState;

          InitialState.StartTime = Instance->Now();
          InitialState.MetadataScanComplete = false;
          InitialState.SyncComplete = false;
          InitialState.CollectionsLoaded = false;
          InitialState.CollectionsLoadFailed = false;
          InitialState.LazyLoadingFallback = false;
          InitialState.CollectionsLoadedCount = 0;
          InitialState.CollectionsExpectedCount = 0;

          SetStartupState(InitialState);
     }
     else
     {
          StartupTime = time(nullptr);
     }
}

/* Returns the total number of times the server instance has been restarted */

uint64_t ServerStats::GetRestartCount() const
{
     std::lock_guard<std::mutex> Lock(RestartStatsMutex);

     return RestartCount;
}

/* Returns the timestamp of the most recent server restart operation */

time_t ServerStats::GetLastRestartTimestamp() const
{
     std::lock_guard<std::mutex> Lock(RestartStatsMutex);

     return LastRestartTimestamp;
}

/* Increments the internal restart counter and updates the last restart timestamp */

void ServerStats::IncrementRestartCount()
{
     std::lock_guard<std::mutex> Lock(RestartStatsMutex);

     RestartCount++;

     LastRestartTimestamp = time(nullptr);
}

/* Sets the global health status and records the reason for any degradation */

void ServerStats::SetHealthDegraded(bool DegradedFlag, const std::string &ReasonStr)
{
     std::lock_guard<std::mutex> Lock(HealthDegradedMutex);

     HealthDegraded.store(DegradedFlag, std::memory_order_release);

     HealthDegradedReason = ReasonStr;
}

/* Evaluates whether the server health is currently considered degraded */

bool ServerStats::IsHealthDegraded() const
{
     return HealthDegraded.load(std::memory_order_acquire);
}

/* Returns the human-readable reason associated with the current health degradation */

std::string ServerStats::GetHealthDegradedReason() const
{
     std::lock_guard<std::mutex> Lock(HealthDegradedMutex);

     return HealthDegradedReason;
}

/* Returns true if a non-graceful shutdown was detected during the last execution */

bool ServerStats::IsDirtyShutdown() const
{
     std::lock_guard<std::mutex> Lock(DirtyShutdownMutex);
     return DirtyShutdownDetected;
}

/* Configures the dirty shutdown flag based on startup recovery checks */

void ServerStats::SetDirtyShutdown(bool IsDirtyFlag)
{
     std::lock_guard<std::mutex> Lock(DirtyShutdownMutex);

     DirtyShutdownDetected = IsDirtyFlag;
}

/* Returns the precise timestamp when the current server instance started */

time_t ServerStats::GetStartupTime() const
{
     return StartupTime;
}

/* Sets the primary startup timestamp for the current execution cycle */

void ServerStats::SetStartupTime(time_t StartupTimestamp)
{
     StartupTime = StartupTimestamp;
}

/* Returns the current startup state snapshot */

StartupState ServerStats::GetStartupState() const
{
     std::lock_guard<std::mutex> Lock(StartupStateMutex);

     return StartupStateInfo;
}

/* Updates the startup state snapshot */

void ServerStats::SetStartupState(const StartupState &StateVal)
{
     std::lock_guard<std::mutex> Lock(StartupStateMutex);

     StartupStateInfo = StateVal;
}

/* Updates startup state collections info */

void ServerStats::UpdateStartupStateCollections(const std::vector<std::string> &FailedCollections,
                                                size_t LoadedCount, size_t ExpectedCount)
{
     std::lock_guard<std::mutex> Lock(StartupStateMutex);

     StartupStateInfo.FailedCollections = FailedCollections;
     StartupStateInfo.CollectionsLoadedCount = LoadedCount;
     StartupStateInfo.CollectionsExpectedCount = ExpectedCount;
}
