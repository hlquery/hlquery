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
#include <string>
#include <vector>

#include "core/config.h"

class CoreExport StartupState
{
   public:
     /* Start time of the server */

     std::chrono::steady_clock::time_point StartTime;

     /* Start time of the metadata scan */

     std::chrono::steady_clock::time_point MetadataScanStart;

     /* End time of the metadata scan */

     std::chrono::steady_clock::time_point MetadataScanEnd;

     /* Start time of the sync process */

     std::chrono::steady_clock::time_point SyncStart;

     /* End time of the sync process */

     std::chrono::steady_clock::time_point SyncEnd;

     /* Start time of the collection load */

     std::chrono::steady_clock::time_point CollectionLoadStart;

     /* End time of the collection load */

     std::chrono::steady_clock::time_point CollectionLoadEnd;

     /* Time when the server became ready */

     std::chrono::steady_clock::time_point ReadyTime;

     /* Flag indicating metadata scan is complete */

     bool MetadataScanComplete = false;

     /* Flag indicating sync is complete */

     bool SyncComplete = false;

     /* Flag indicating collections are loaded */

     bool CollectionsLoaded = false;

     /* Flag indicating collection load failed */

     bool CollectionsLoadFailed = false;

     /* Flag indicating lazy loading fallback was used */

     bool LazyLoadingFallback = false;

     /* Count of successfully loaded collections */

     size_t CollectionsLoadedCount = 0;

     /* Count of expected collections to be loaded */

     size_t CollectionsExpectedCount = 0;

     /* List of collections that failed to load */

     std::vector<std::string> FailedCollections;

     /* List of collections deferred for lazy loading */

     std::vector<std::string> LazyLoadingDeferred;

     /* Error message from metadata scan */

     std::string MetadataScanError;

     /* Error message from sync process */

     std::string SyncError;

     /* Error message from collection load */

     std::string CollectionLoadError;

     /* Flag for strict startup mode */

     bool StrictStartupMode = false;

     /* Flag for read-only mode */

     bool ReadonlyMode = false;

     /* Flag for validation mode */

     bool ValidationMode = false;
};
