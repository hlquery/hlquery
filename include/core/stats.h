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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/startup.h"

/*
 * ServerStats - Tracks server statistics and state information.
 * Thread-safe statistics tracking for restart counts, health status, and shutdown state.
 */

class ServerStats
{
   private:

     /* Restart count */

     uint64_t RestartCount;

     /* Last restart timestamp */

     time_t LastRestartTimestamp;

     /* Mutex for restart statistics */

     mutable std::mutex RestartStatsMutex;

     /* Health degradation status */

     std::atomic<bool> HealthDegraded;

     /* Reason for health degradation */

     std::string HealthDegradedReason;

     /* Mutex for health status */

     mutable std::mutex HealthDegradedMutex;

     /* Flag for dirty shutdown detection */

     bool DirtyShutdownDetected;

     /* Mutex for dirty shutdown status */

     mutable std::mutex DirtyShutdownMutex;

     /* Server startup time */

     std::atomic<time_t> StartupTime;

   public:

     struct HealthStatus
     {
          bool Degraded = false;
          std::string Reason;
     };

     /* Constructor */

     ServerStats();

     /* Destructor */

     ~ServerStats() = default;

     /* Starts statistics tracking */

     void Start();

     /* Returns the restart count */

     uint64_t GetRestartCount() const;

     /* Returns the last restart timestamp */

     time_t GetLastRestartTimestamp() const;

     /* Increments the restart count */

     void IncrementRestartCount();

     /* Sets the health status */

     void SetHealthDegraded(bool degraded, const std::string &reason = "");

     /* Returns true if health is degraded */

     bool IsHealthDegraded() const;

     /* Returns a consistent health status and reason snapshot */

     HealthStatus GetHealthStatus() const;

     /* Returns the reason for degraded health */

     std::string GetHealthDegradedReason() const;

     /* Returns true if a dirty shutdown was detected */

     bool IsDirtyShutdown() const;

     /* Sets the dirty shutdown flag */

     void SetDirtyShutdown(bool dirty);

     /* Returns the startup time */

     time_t GetStartupTime() const;

     /* Sets the startup time */

     void SetStartupTime(time_t time);

     /* Returns the current startup state snapshot */

     StartupState GetStartupState() const;

     /* Updates the startup state snapshot */

     void SetStartupState(const StartupState &state);

     /* Updates startup state collections info */

     void UpdateStartupStateCollections(const std::vector<std::string> &failed_collections,
                                        size_t loaded_count, size_t expected_count);

     /* Startup state snapshot */

     StartupState StartupStateInfo;

     /* Mutex for startup state */

     mutable std::mutex StartupStateMutex;
};
