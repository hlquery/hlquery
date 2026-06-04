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

/*
 * hlog option parsing.
 */

#include "common/options.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void PrintUsageAndExit()
{
     std::cout
          << "hlog - log pipeline\n"
          << "Usage: hlog [options]\n"
          << "  --config <path>      Read watched hlquery log targets from hlquery.conf\n"
          << "  --hlog-config <path> Read hlog pipeline config\n"
          << "  --file <path>        Add explicit file input\n"
          << "  --mode <auto|inotify|refresh>\n"
          << "  --interval <ms>      Poll interval in milliseconds\n"
          << "  --from-start         Read current file contents before follow\n"
          << "  --nofork            Keep stdout output attached to the terminal\n"
          << "  --test-config        Validate hlog.conf and hlquery.conf then exit\n"
          << "  --quiet              Suppress startup banner\n";
     std::exit(0);
}

static std::string RequireValue(int argc, char** argv, int& index, const std::string& option)
{
     if (index + 1 >= argc)
     {
          throw std::runtime_error(option + " requires a value");
     }

     ++index;
     return argv[index];
}

WatchMode ParseMode(const std::string& value)
{
     if (value == "auto")
     {
          return WatchMode::Auto;
     }

     if (value == "kernel" || value == "notify" || value == "notification" || value == "inotify")
     {
          return WatchMode::Kernel;
     }

     if (value == "poll" || value == "refresh" || value == "timer")
     {
          return WatchMode::Poll;
     }

     throw std::runtime_error("Invalid mode '" + value + "'. Use auto, inotify, or refresh");
}

std::string WatchModeToString(WatchMode mode)
{
     switch (mode)
     {
          case WatchMode::Kernel:
               return "inotify";
          case WatchMode::Poll:
               return "poll";
          default:
               return "auto";
     }
}

StartPosition ParseStartPosition(const std::string& value)
{
     if (value == "beginning" || value == "start")
     {
          return StartPosition::Beginning;
     }

     if (value == "end" || value == "tail")
     {
          return StartPosition::End;
     }

     throw std::runtime_error("Invalid start_position '" + value + "'. Use beginning or end");
}

Options ParseArgs(int argc, char** argv)
{
     Options options;

     for (int i = 1; i < argc; ++i)
     {
          const std::string arg = argv[i];

          if (arg == "--config" || arg == "-c")
          {
               options.ConfigPath = RequireValue(argc, argv, i, arg);
               continue;
          }

          if (arg == "--hlog-config" || arg == "-g")
          {
               options.HLogConfigPath = RequireValue(argc, argv, i, arg);
               continue;
          }

          if (arg == "--file" || arg == "-f")
          {
               options.ExplicitFiles.push_back(RequireValue(argc, argv, i, arg));
               continue;
          }

          if (arg == "--mode" || arg == "-m")
          {
               options.OverrideMode = ParseMode(RequireValue(argc, argv, i, arg));
               continue;
          }

          if (arg == "--interval" || arg == "-i")
          {
               options.OverrideIntervalMs = std::max(100, std::stoi(RequireValue(argc, argv, i, arg)));
               continue;
          }

          if (arg == "--from-start" || arg == "-s")
          {
               options.OverrideFromStart = true;
               continue;
          }

          if (arg == "--nofork")
          {
               continue;
          }

          if (arg == "--quiet" || arg == "-q")
          {
               options.Quiet = true;
               continue;
          }

          if (arg == "--test-config" || arg == "-t")
          {
               options.TestConfig = true;
               continue;
          }

          if (arg == "--help" || arg == "-h")
          {
               PrintUsageAndExit();
          }

          throw std::runtime_error("Unknown argument: " + arg);
     }

     return options;
}
