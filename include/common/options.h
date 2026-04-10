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

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"

/* 
 * Selects how file changes are monitored by the hlog runtime.
 * Auto chooses the best available backend for the platform,
 * while Kernel and Poll force a specific watch strategy.
 */

enum class WatchMode
{
     Auto,
     Kernel,
     Poll
};

/* 
 * Selects where log reading should begin when an input file
 * is opened by the hlog pipeline.
 * Beginning replays existing content, while End tails only
 * new content appended after startup.
 */

enum class StartPosition
{
     Beginning,
     End
};

struct Options
{
     std::string ConfigPath = std::string(HLQUERY_CONFIG_DIR) + "/hlquery.conf";
     std::string HLogConfigPath = "run/conf/hlog.conf";
     std::vector<std::string> ExplicitFiles;
     std::optional<WatchMode> OverrideMode;
     std::optional<int> OverrideIntervalMs;
     std::optional<bool> OverrideFromStart;
     bool Quiet = false;
     bool TestConfig = false;
};

struct HLogInputFile
{
     std::filesystem::path PathValue;
     StartPosition Start = StartPosition::End;
     std::optional<WatchMode> ModeOverride;
     std::optional<int> IntervalMsOverride;
};

WatchMode ParseMode(const std::string& value);
std::string WatchModeToString(WatchMode mode);
StartPosition ParseStartPosition(const std::string& value);
Options ParseArgs(int argc, char** argv);
