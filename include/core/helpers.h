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

#include <string>
#include <vector>

class ServerConfig;

class CoreHelpers
{
public:
     CoreHelpers() = delete;

/* Helper function to check if the main processing loop should terminate. */

     static bool ShouldExitLoop();

/* Perform periodic database flush with robust error handling. */

     static void SafePeriodicFlush();

/* Process background tasks and management operations. */

     static void ProcessPeriodicTasks();

/* Preflight SSL bind configuration before daemonization. */

     static bool PreflightSSLConfig(ServerConfig *ConfigPtr);

/* Prints a startup section with one module name per line. */

     static void PrintStartupModuleList(const std::string &Heading, const std::vector<std::string> &ModuleNames);

};
