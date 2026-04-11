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

#include <ctime>
#include <string>

#include "api/ipfilter.h"
#include "common/health.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "utils/consolewriter.h"

namespace
{
     static constexpr time_t HEALTH_SNAPSHOT_INTERVAL_SEC = 15;

     std::string FormatFriendlyTime(time_t Timestamp)
     {
          if (Timestamp <= 0)
          {
               return "unknown time";
          }

          std::tm LocalTime{};

          localtime_r(&Timestamp, &LocalTime);

          char Buffer[64] = {};

          if (std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%d %H:%M:%S", &LocalTime) == 0)
          {
               return "unknown time";
          }

          return std::string(Buffer);
     }
}

class CoreTimersModule final : public RuntimeModule
{
   private:

     time_t LastSnapshot = 0;

     time_t LastFlush = 0;

   public:

     CoreTimersModule() : RuntimeModule("core_timers")
     {
          AttachHooks({ModuleHook::OnThreadPoolsReady,
                       ModuleHook::OnEveryOneMinute,
                       ModuleHook::OnIdleTick});
     }

     bool Start(const ServerConfig &, std::string &) override
     {
          if (Instance)
          {
               const time_t NowTime = Instance->Time();

               if (LastSnapshot == 0)
               {
                    LastSnapshot = NowTime;
               }

               if (LastFlush == 0)
               {
                    LastFlush = NowTime;
               }
          }

          return true;
     }

     void Stop() override
     {

     }

     void OnThreadPoolsReady() override
     {
          if (Instance && Instance->Config && Instance->Config->GetNoForkMode())
          {
               ConsoleWriter::WriteStartup("threadpool ready!", true, false);
          }
     }

     void OnEveryOneMinute() override
     {
          return;
          
          if (Instance && Instance->Config && Instance->Logs && Instance->Config->GetNoForkMode())
          {
               const time_t NowTime = Instance->Time();
               const std::string FriendlyTime = FormatFriendlyTime(NowTime);

               Instance->Logs->Normal("core_timers", "running");
               ConsoleWriter::WriteStartup("One minute! (" + FriendlyTime + ")", true, false);
          }

          if (!Instance || !Instance->IPFilter)
          {
               return;
          }

          auto *Filter = Instance->IPFilter.get();

          if (!Filter->IsEnabled())
          {
               LastFlush = Instance->Time();
               return;
          }

          const time_t NowTime = Instance->Time();

          if (NowTime <= 0)
          {
               return;
          }

          if (LastFlush == 0)
          {
               LastFlush = NowTime;
               return;
          }

          if (NowTime - LastFlush >= DNS_CACHE_FLUSH_INTERVAL_SEC)
          {
               Filter->FlushDNSCache();
               LastFlush = NowTime;

               if (Instance->Logs)
               {
                    Instance->Logs->Debug("ip_allow", "DNS cache flush triggered by core_timers module.");
               }
          }
     }

     void OnIdleTick(time_t NowTime) override
     {
          if (NowTime <= 0)
          {
               return;
          }

          if (LastSnapshot == 0)
          {
               LastSnapshot = NowTime;
               return;
          }

          if (NowTime - LastSnapshot >= HEALTH_SNAPSHOT_INTERVAL_SEC)
          {
               EmitDaemonHealthSnapshot(NowTime);
               LastSnapshot = NowTime;
          }
     }
};

MODULE_LOAD(CoreTimersModule)
