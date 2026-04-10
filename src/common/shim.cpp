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

/*
 * hlog - Live log pipeline for hlquery.
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlog, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include "core/hlcore.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <sstream>
#include <utility>

#include "core/pipeline.h"
#include "core/pipeline_config.h"

namespace
{

std::string JoinPaths(const std::vector<HLogInputFile>& inputs)
{
     std::ostringstream joined;

     for (size_t i = 0; i < inputs.size(); ++i)
     {
          if (i != 0)
          {
               joined << ",";
          }

          joined << std::filesystem::absolute(inputs[i].PathValue).string();
     }

     return joined.str();
}

size_t CountResolvedInputs(const PipelineConfig& config, const Options& options)
{
     if (!config.Inputs.empty())
     {
          return config.Inputs.size();
     }

     for (const auto& module : config.Modules)
     {
          if (module.Name != "filein")
          {
               continue;
          }

          const auto pathIt = module.Attributes.find("path");
          if (pathIt != module.Attributes.end() && !pathIt->second.empty())
          {
               size_t count = 1;
               for (char ch : pathIt->second)
               {
                    if (ch == ',')
                    {
                         ++count;
                    }
               }
               return count;
          }

          try
          {
               return ResolveFilesFromHlqueryConfig(options.ConfigPath).size();
          }
          catch (...)
          {
               return 0;
          }
     }

     return 0;
}

void MaterializeFileInputModule(PipelineConfig& config)
{
     auto existing = std::find_if(config.Modules.begin(), config.Modules.end(), [](const ModuleConfig& module) {
          return module.Name == "filein";
     });

     if (!config.Inputs.empty())
     {
          ModuleConfig materialized;
          materialized.Name = "filein";
          materialized.Attributes["path"] = JoinPaths(config.Inputs);
          materialized.Attributes["start_position"] =
               config.Inputs.front().Start == StartPosition::Beginning ? "beginning" : "end";

          if (existing == config.Modules.end())
          {
               config.Modules.push_back(std::move(materialized));
          }
          else
          {
               existing->Attributes["path"] = materialized.Attributes["path"];
               existing->Attributes["start_position"] = materialized.Attributes["start_position"];
          }

          return;
     }

     (void)existing;
}

}

hlcore::~hlcore()
{
     Cleanup();
}

void hlcore::Cleanup()
{
     HLogPipeline.reset();
     OwnedLogs.reset();
     Logs = nullptr;
}

void hlcore::Run()
{
     if (HLogOptions.TestConfig)
     {
          if (!HLogOptions.Quiet)
          {
               Logs->Normal("config_test", "hlog_config=" + std::filesystem::absolute(HLogOptions.HLogConfigPath).string() + ".");
               Logs->Normal("config_test", "hlquery_config=" + std::filesystem::absolute(HLogOptions.ConfigPath).string() + ".");
               Logs->Normal("config_test", "resolved_inputs=" + std::to_string(CountResolvedInputs(HLogPipelineConfig, HLogOptions)) + ".");
          }

          Logs->Normal("config_test", "Configuration parsed successfully.");
          return;
     }

     HLogPipeline = std::make_unique<Pipeline>(HLogPipelineConfig, Logs);
     EmitStartupLogs();

     std::string sourceError;
     if (HLogPipeline->HasSourceModule() &&
         !HLogPipeline->RunSourceModule(HLogEffectiveMode,
                                        HLogPipelineConfig.PollIntervalMs,
                                        Logs,
                                        sourceError))
     {
          throw std::runtime_error(sourceError.empty() ? "hlog source module failed." : sourceError);
     }

     Logs->Normal("pipeline", "Shutting down.");
}

time_t hlcore::Time() const
{
     try
     {
          auto now = std::chrono::system_clock::now();
          auto duration = now.time_since_epoch();
          auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
          return static_cast<time_t>(seconds.count());
     }
     catch (...)
     {
          return 0;
     }
}

long long hlcore::NowMs() const
{
     try
     {
          auto now = std::chrono::system_clock::now();
          auto duration = now.time_since_epoch();
          auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
          return static_cast<long long>(milliseconds.count());
     }
     catch (...)
     {
          return 0;
     }
}

std::chrono::steady_clock::time_point hlcore::Now() const
{
     try
     {
          return std::chrono::steady_clock::now();
     }
     catch (...)
     {
          return std::chrono::steady_clock::time_point{};
     }
}

void hlcore::InitializeLogs()
{
     OwnedLogs = std::make_unique<LogManager>();

     LogConfig consoleConfig;
     consoleConfig.method = "console";
     consoleConfig.type = "*";
     consoleConfig.level = LogLevel::LOG_NORMAL;
     consoleConfig.target = "console";

     OwnedLogs->Initialize(std::vector<LogConfig>{consoleConfig}, false, true, false);
     Logs = OwnedLogs.get();
}

void hlcore::ResolvePipelineConfig()
{
     HLogPipelineConfig = LoadPipelineConfig(HLogOptions.HLogConfigPath);

     ValidateHlqueryConfig(HLogOptions.ConfigPath);

     if (HLogOptions.OverrideMode.has_value())
     {
          HLogPipelineConfig.Mode = *HLogOptions.OverrideMode;
     }

     if (HLogOptions.OverrideIntervalMs.has_value())
     {
          HLogPipelineConfig.PollIntervalMs = *HLogOptions.OverrideIntervalMs;
     }

     if (!HLogOptions.ExplicitFiles.empty())
     {
          HLogPipelineConfig.Inputs.clear();

          for (const auto& file : HLogOptions.ExplicitFiles)
          {
               HLogInputFile input;
               input.PathValue = std::filesystem::absolute(file);
               input.Start = HLogOptions.OverrideFromStart.value_or(false) ? StartPosition::Beginning : StartPosition::End;
               HLogPipelineConfig.Inputs.push_back(std::move(input));
          }
     }

     if (HLogOptions.OverrideFromStart.has_value())
     {
          for (auto& input : HLogPipelineConfig.Inputs)
          {
               input.Start = *HLogOptions.OverrideFromStart ? StartPosition::Beginning : StartPosition::End;
          }
     }

     for (const auto& input : HLogPipelineConfig.Inputs)
     {
          if (input.ModeOverride.has_value())
          {
               HLogPipelineConfig.Mode = *input.ModeOverride;
          }

          if (input.IntervalMsOverride.has_value())
          {
               HLogPipelineConfig.PollIntervalMs = *input.IntervalMsOverride;
          }
     }

     MaterializeFileInputModule(HLogPipelineConfig);
}
void hlcore::ResolveEffectiveWatchMode()
{
     HLogEffectiveMode = HLogPipelineConfig.Mode;

     if (HLogEffectiveMode == WatchMode::Auto)
     {
#ifdef __linux__
          HLogEffectiveMode = WatchMode::Kernel;
#else
          HLogEffectiveMode = WatchMode::Poll;
#endif
     }
}

void hlcore::EmitStartupLogs() const
{
     if (!Logs || HLogOptions.Quiet)
     {
          return;
     }

     Logs->Normal("pipeline", "mode=" + WatchModeToString(HLogEffectiveMode) +
          " interval_ms=" + std::to_string(HLogPipelineConfig.PollIntervalMs) + ".");
     Logs->Normal("pipeline", "hlog_config=" + std::filesystem::absolute(HLogOptions.HLogConfigPath).string() + ".");

     if (HLogPipeline && HLogPipeline->GetConfig().HlqueryOutput.Enabled)
     {
          Logs->Normal("output_hlquery", "endpoint=" + HLogPipeline->GetConfig().HlqueryOutput.Endpoint +
               " collection=" + HLogPipeline->GetConfig().HlqueryOutput.Collection + ".");
     }
}
