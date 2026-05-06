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
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

#include "core/hlquery.h"
#include "core/modules.h"
#include "search/storageengine.h"

class DumpModule final : public AutoRuntimeModule<DumpModule>
{
   public:

     DumpModule()
         : AutoRuntimeModule("dump", true)
     {

     }

     void Stop() override
     {
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          ModuleCommandResponse Response;
          const auto StartTime = std::chrono::steady_clock::now();

          Response.Success = true;
          Response.StatusCode = 200;
          Response.Message = "Storage dump completed.";

          if (!Instance || !Instance->Database)
          {
               Response.Success = false;
               Response.StatusCode = 503;
               Response.Body = "Storage engine unavailable.";
               return Response;
          }

          auto parseInteger = [](const std::string &Value, long long DefaultValue) -> long long
          {
               try
               {
                    return std::stoll(Value);
               }
               catch (...)
               {
                    return DefaultValue;
               }
          };

          std::string pattern = "*";
          if (auto it = Request.NamedParameters.find("pattern"); it != Request.NamedParameters.end() && !it->second.empty())
          {
               pattern = it->second;
          }
          else if (auto prefix_it = Request.NamedParameters.find("prefix"); prefix_it != Request.NamedParameters.end() && !prefix_it->second.empty())
          {
               pattern = prefix_it->second + "*";
          }

          long long cursor = 0;
          if (auto it = Request.NamedParameters.find("cursor"); it != Request.NamedParameters.end())
          {
               cursor = parseInteger(it->second, 0);
          }

          long long limit = 0;
          if (auto it = Request.NamedParameters.find("limit"); it != Request.NamedParameters.end())
          {
               limit = parseInteger(it->second, limit);
          }
          if (auto it = Request.NamedParameters.find("count"); it != Request.NamedParameters.end())
          {
               limit = parseInteger(it->second, limit);
          }

          if (limit < 0)
          {
               limit = 0;
          }

          std::ostringstream BodyStream;

          const long long chunk_size = 1000;
          long long processed = 0;
          long long next_cursor = cursor;
          bool dumped_any = false;
          bool has_more_entries = false;

          while (true)
          {
               long long fetch_count = chunk_size;
               if (limit > 0)
               {
                    const long long remaining = limit - processed;
                    if (remaining <= 0)
                    {
                         break;
                    }
                    fetch_count = std::min(fetch_count, remaining);
               }

               const auto chunk = Instance->Database->ScanOptimized(next_cursor, pattern, fetch_count);

               if (chunk.keys.empty())
               {
                    if (!chunk.has_more)
                    {
                         break;
                    }

                    has_more_entries = true;
                    next_cursor = chunk.cursor;
                    continue;
               }

               for (const auto &Key : chunk.keys)
               {
                    const std::string Value = Instance->Database->Get(Key);
                    if (processed > 0 || dumped_any)
                    {
                         BodyStream << "========================================\n";
                    }
                    BodyStream << "Key\n";
                    BodyStream << "----------------------------------------\n";
                    BodyStream << Key << "\n";
                    BodyStream << "Value\n";
                    BodyStream << "========================================\n";
                    BodyStream << Value << "\n";
                    ++processed;
               }
               dumped_any = true;

               has_more_entries = chunk.has_more;
               next_cursor = chunk.cursor;

               if (!chunk.has_more)
               {
                    has_more_entries = false;
                    break;
               }

               if (limit > 0 && processed >= limit)
               {
                    break;
               }
           }

          if (!dumped_any)
          {
               BodyStream << "No keys matched pattern '" << pattern << "'.";
          }
          else
          {
               BodyStream << "Dumped " << processed << " entries";
               if (has_more_entries)
               {
                    BodyStream << " (next cursor: " << next_cursor << ")";
               }

               const auto EndTime = std::chrono::steady_clock::now();
               const double ElapsedSeconds = std::chrono::duration<double>(EndTime - StartTime).count();

               BodyStream << "\nTime it took to dump: " << std::fixed << std::setprecision(2) << ElapsedSeconds << "s";
          }

          Response.Body = BodyStream.str();

          return Response;
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          ModuleCommandSpec command;

          command.Route = "dump";
          command.Summary = "Print storage key/value pairs.";
          command.Syntax = "dump [pattern=<pattern>] [prefix=<prefix>] [limit=<count>] [cursor=<offset>]";
          command.MaxParameters = 4;

          ModuleCommandParameterSpec pattern_param;
          pattern_param.Name = "pattern";
          pattern_param.Type = "string";
          pattern_param.Description = "Wildcard pattern of keys to dump (default '*').";

          ModuleCommandParameterSpec prefix_param;
          prefix_param.Name = "prefix";
          prefix_param.Type = "string";
          prefix_param.Description = "Prefix to match (converted to prefix*).";

          ModuleCommandParameterSpec limit_param;
          limit_param.Name = "limit";
          limit_param.Type = "int";
          limit_param.Description = "Maximum number of entries to print (0=all, default 0).";

          ModuleCommandParameterSpec cursor_param;
          cursor_param.Name = "cursor";
          cursor_param.Type = "int";
          cursor_param.Description = "Cursor offset for pagination.";

          command.Parameters = {pattern_param, prefix_param, limit_param, cursor_param};

          return {command};
     }
};

MODULE_LOAD(DumpModule)
