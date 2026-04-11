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

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/configreader.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "modules/extra/m_analytics/analyticsmanager.h"
#include "utils/jsonbuilder.h"

/* Runtime module that forwards events into the analytics manager. */

class AnalyticsRuntimeModule final : public RuntimeModule
{
   private:

     /* Owns the analytics manager while the module is active. */

     std::unique_ptr<AnalyticsManager> manager;

     /* Starts the analytics manager if it is enabled. */

     void EnsureStarted()
     {
          if (manager && manager->IsEnabled())
          {
               manager->Start();
          }
     }

   public:

     /* Initialize the analytics runtime module. */

     AnalyticsRuntimeModule() : RuntimeModule("analytics", true)
     {
          AttachHooks({ModuleHook::OnRequestAnalytics,
                       ModuleHook::OnSearchCollection,
                       ModuleHook::OnSearchDocument,
                       ModuleHook::OnCreateCollection,
                       ModuleHook::OnUpdateCollection,
                       ModuleHook::OnDeleteCollection,
                       ModuleHook::OnAddDocument,
                       ModuleHook::OnUpdateDocument,
                       ModuleHook::OnDeleteDocument,
                       ModuleHook::OnDeleteDocuments,
                       ModuleHook::OnBulkImportDocuments,
                       ModuleHook::OnUpsertSynonym,
                       ModuleHook::OnDeleteSynonym,
                       ModuleHook::OnCreateStopword,
                       ModuleHook::OnDeleteStopword,
                       ModuleHook::OnUpsertOverride,
                       ModuleHook::OnDeleteOverride,
                       ModuleHook::OnUpsertAlias,
                       ModuleHook::OnDeleteAlias,
                       ModuleHook::OnFlush,
                       ModuleHook::OnAnalyticsClick,
                       ModuleHook::OnThreadPoolsReady});
     }

     /* Start the module and build the analytics manager from config. */

     bool Start(const ServerConfig &, std::string &ErrorMessage) override
     {
          if (!Instance || !Instance->Config)
          {
               ErrorMessage = "Analytics module requires a live hlquery instance and configuration.";
               return false;
          }

          auto analytics_tag = Instance->Config->GetConfigReader().GetTag("analytics");

          if (!analytics_tag)
          {
               return true;
          }

          const std::string endpoint = analytics_tag->GetString("endpoint");

          if (endpoint.empty())
          {
               return true;
          }

#if !defined(HLQUERY_HAS_OPENSSL)
          if (endpoint.rfind("https://", 0) == 0)
          {
               ErrorMessage = "Analytics endpoint '" + endpoint + "' requires SSL support, but this build does not include OpenSSL. Use http:// or rebuild with SSL enabled.";
               return false;
          }
#endif

          int flush_interval_seconds = 300;

          try
          {
               const int requested_flush_interval = analytics_tag->GetInt("flush_interval", flush_interval_seconds);

               if (requested_flush_interval < 10)
               {
                    throw std::runtime_error("Invalid analytics.flush_interval: minimum allowed value is 10 seconds.");
               }

               flush_interval_seconds = std::clamp(requested_flush_interval, 10, 86400);
          }
          catch (const std::exception &Error)
          {
               ErrorMessage = Error.what();
               return false;
          }

          const int connect_timeout_ms = std::clamp(analytics_tag->GetInt("connect_timeout_ms", 5000), 250, 60000);
          const std::string api_token = analytics_tag->GetString("api_token");
          const bool track_reads = analytics_tag->GetBool("track_reads", true);
          const bool track_writes = analytics_tag->GetBool("track_writes", true);

          /* Analytics always uses the global hlquery logger owned by Instance. */
          manager = std::make_unique<AnalyticsManager>(Instance->Config->GetServerName(),
                                                       Instance->Config->GetServerId(),
                                                       Instance->Config->GetDatabaseEngine(),
                                                       endpoint,
                                                       api_token,
                                                       flush_interval_seconds,
                                                       connect_timeout_ms,
                                                       track_reads,
                                                       track_writes);

          if (!manager)
          {
               ErrorMessage = "Failed to allocate analytics manager.";
               return false;
          }

          return true;
     }

     /* Stop the module and flush pending analytics. */

     void Stop() override
     {
          if (manager)
          {
               manager->FlushNow();
               manager.reset();
          }
     }

     /* Record generic request analytics for one completed route. */

     void OnRequestAnalytics(const HttpRequest &Request, const HttpResponse &Response, RouteAction ActionVal) override
     {
          EnsureStarted();

          if (manager && manager->IsEnabled())
          {
               manager->RecordRequest(Request, Response, ActionVal);
          }
     }

     /* Record one collection search event. */

     void OnSearchCollection(const SearchEvent &Event) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordSearchEvent("CollectionSearch", Event.Collection, Event.SearchTimeMS, Event.Found, Event.Returned, Event.RequesterIP, Event.RequesterUser, Event.Authenticated);
          }
     }

     /* Record one document search event. */

     void OnSearchDocument(const SearchEvent &Event) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordSearchEvent("DocumentSearch", Event.Collection, Event.SearchTimeMS, Event.Found, Event.Returned, Event.RequesterIP, Event.RequesterUser, Event.Authenticated);
          }
     }

     /* Record one collection-creation event. */

     void OnCreateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("CreateCollection", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one collection-update event. */

     void OnUpdateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("UpdateCollection", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one collection-deletion event. */

     void OnDeleteCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("DeleteCollection", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one document-creation event. */

     void OnAddDocument(const std::string &Collection, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordDocumentEvent("AddDocument", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one document-update event. */

     void OnUpdateDocument(const std::string &Collection, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordDocumentEvent("UpdateDocument", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one document-deletion event. */

     void OnDeleteDocument(const std::string &Collection, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordDocumentEvent("DeleteDocument", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one bulk document-deletion event. */

     void OnDeleteDocuments(const std::string &Collection, uint64_t Count, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCountedEvent("DeleteDocuments", Collection, Count, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one bulk import event. */

     void OnBulkImportDocuments(const std::string &Collection, uint64_t ImportedCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCountedEvent("BulkImportDocuments", Collection, ImportedCount, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one synonym upsert event. */

     void OnUpsertSynonym(const std::string &Collection,
                          const std::string &,
                          bool GlobalScope,
                          const std::string &RequesterIP,
                          const std::string &RequesterUser,
                          bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent(GlobalScope ? "UpsertGlobalSynonym" : "UpsertSynonym", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one synonym deletion event. */

     void OnDeleteSynonym(const std::string &Collection,
                          const std::string &,
                          bool GlobalScope,
                          const std::string &RequesterIP,
                          const std::string &RequesterUser,
                          bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent(GlobalScope ? "DeleteGlobalSynonym" : "DeleteSynonym", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one stopword creation event. */

     void OnCreateStopword(const std::string &Collection,
                           uint64_t Count,
                           bool GlobalScope,
                           const std::string &RequesterIP,
                           const std::string &RequesterUser,
                           bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCountedEvent(GlobalScope ? "CreateGlobalStopword" : "CreateStopword", Collection, Count, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one stopword deletion event. */

     void OnDeleteStopword(const std::string &Collection,
                           const std::string &,
                           bool GlobalScope,
                           const std::string &RequesterIP,
                           const std::string &RequesterUser,
                           bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent(GlobalScope ? "DeleteGlobalStopword" : "DeleteStopword", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one override upsert event. */

     void OnUpsertOverride(const std::string &Collection, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("UpsertOverride", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one override deletion event. */

     void OnDeleteOverride(const std::string &Collection, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("DeleteOverride", Collection, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one alias upsert event. */

     void OnUpsertAlias(const std::string &AliasName, const std::string &, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("UpsertAlias", AliasName, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one alias deletion event. */

     void OnDeleteAlias(const std::string &AliasName, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCollectionEvent("DeleteAlias", AliasName, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one flush event. */

     void OnFlush(uint64_t CollectionsDeleted, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordCountedEvent("Flush", "*", std::max<uint64_t>(CollectionsDeleted, 1), RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Record one analytics click event. */

     void OnAnalyticsClick(const std::string &Collection,
                           const std::string &,
                           const std::string &,
                           int Rank,
                           const std::string &RequesterIP,
                           const std::string &RequesterUser,
                           bool Authenticated) override
     {
          EnsureStarted();

          if (manager)
          {
               manager->RecordClickEvent(Collection, Rank, RequesterIP, RequesterUser, Authenticated);
          }
     }

     /* Ensure analytics starts once thread pools are available. */

     void OnThreadPoolsReady() override
     {
          EnsureStarted();
     }

     /* Describe the analytics module CLI surface. */

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription description;
          description.Name = "analytics";
          description.Summary = "Exposes runtime analytics module status and flush control.";
          description.Syntax = "GET /modules/analytics | POST /modules/analytics/flush";
          description.MinParameters = 0;
          description.MaxParameters = 0;
          description.Examples.push_back("curl http://localhost:9200/modules/analytics");
          description.Examples.push_back("curl -X POST http://localhost:9200/modules/analytics/flush");

          return description;
     }

     /* Describe the analytics module commands. */

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          std::vector<ModuleCommandSpec> commands;

          ModuleCommandSpec status_command;
          status_command.Route = "status";
          status_command.Summary = "Shows analytics module state.";
          status_command.Syntax = "module analytics status";
          commands.push_back(status_command);

          ModuleCommandSpec flush_command;
          flush_command.Route = "flush";
          flush_command.Summary = "Flushes analytics immediately.";
          flush_command.Syntax = "module analytics flush";
          commands.push_back(flush_command);

          return commands;
     }

     /* Execute one analytics module command. */

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string route = Request.Route.empty() ? "status" : Request.Route;

          if (route == "status")
          {
               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "analytics")
                    .Add("loaded", static_cast<bool>(manager))
                    .Add("enabled", manager && manager->IsEnabled())
                    .Add("message", manager ? "Analytics module is loaded." : "Analytics module is not configured.")
                    .ToString();
               return response;
          }

          if (route == "flush")
          {
               if (!manager)
               {
                    ModuleCommandResponse response;
                    response.Success = false;
                    response.StatusCode = 409;
                    response.Body = JsonBuilder().Add("error", "Analytics manager is not available.").ToString();
                    return response;
               }

               if (!manager->RequestFlushAsync())
               {
                    ModuleCommandResponse response;
                    response.Success = false;
                    response.StatusCode = 503;
                    response.Body = JsonBuilder().Add("error", "Analytics flush could not be scheduled.").ToString();
                    return response;
               }

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "analytics")
                    .Add("message", "Analytics flush scheduled.")
                    .ToString();
               return response;
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(AnalyticsRuntimeModule)
