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

#include <chrono>
#include <fstream>

#include "core/hlquery.h"
#include "vendor/json/json.hpp"

/* Save a summary of collection loading status for diagnostics */

void hlquery::SaveCollectionsLoadSummary()
{
     try
     {
          std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

          /* Compile the summary data into JSON format */

          nlohmann::json SummaryData;

          SummaryData["timestamp"] = StatsVal.GetStartupTime();

          SummaryData["collections_loaded"] = StatsVal.StartupStateInfo.CollectionsLoaded;

          SummaryData["collections_loaded_count"] = StatsVal.StartupStateInfo.CollectionsLoadedCount;

          SummaryData["collections_expected_count"] = StatsVal.StartupStateInfo.CollectionsExpectedCount;

          SummaryData["collections_load_failed"] = StatsVal.StartupStateInfo.CollectionsLoadFailed;

          SummaryData["lazy_loading_fallback"] = StatsVal.StartupStateInfo.LazyLoadingFallback;

          if (!StatsVal.StartupStateInfo.FailedCollections.empty())
          {
               SummaryData["failed_collections"] = StatsVal.StartupStateInfo.FailedCollections;
          }

          /* Calculate and record initialization duration metrics */

          if (StatsVal.StartupStateInfo.ReadyTime.time_since_epoch().count() > 0)
          {
               int64_t TimeToReadyVal = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             StatsVal.StartupStateInfo.ReadyTime - StatsVal.StartupStateInfo.StartTime)
                                             .count();

               SummaryData["time_to_ready_ms"] = TimeToReadyVal;
          }

          /* Commit the summary data to a persistent file */

          std::string SummaryFilePath = std::string(HLQUERY_DATA_DIR) + "/startup_summary.json";

          std::ofstream SummaryFileStream(SummaryFilePath);

          if (SummaryFileStream.is_open())
          {
               SummaryFileStream << SummaryData.dump(2);

               SummaryFileStream.close();

               if (Logs)
               {
                    Logs->Normal("hlquery", "Collections load summary saved to " + SummaryFilePath + ".");
               }
          }
     }
     catch (const std::exception &e)
     {
          if (Logs)
          {
               Logs->Normal("hlquery", "Failed to save collections load summary: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          if (Logs)
          {
               Logs->Normal("hlquery", "Failed to save collections load summary: unknown error.");
          }
     }
}
