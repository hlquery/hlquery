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

#include <sstream>
#include <string>
#include <vector>

#include "api/httpserver.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "search/cstore.h"
#include "utils/consolewriter.h"
#include "utils/jsonbuilder.h"
#include "vendor/json/json.hpp"

namespace
{
std::string DescribeActor(const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
{
     std::ostringstream out;
     out << "ip=" << (RequesterIP.empty() ? "-" : RequesterIP)
         << ", user=" << (RequesterUser.empty() ? "-" : RequesterUser)
         << ", authenticated=" << (Authenticated ? "true" : "false");
     return out.str();
}

std::string DescribeGlobalScope(bool GlobalScope)
{
     return GlobalScope ? "global" : "collection";
}

std::string JoinWords(const std::vector<std::string> &Words)
{
     std::ostringstream out;

     for (size_t i = 0; i < Words.size(); ++i)
     {
          if (i != 0)
          {
               out << ",";
          }
          out << Words[i];
     }

     return out.str();
}

void Trace(const std::string &Message)
{
     ConsoleWriter::WriteInfo("[m_debug] " + Message);
}
}

/* Runtime module used to print extensive debug information for module callbacks. */

class DebugRuntimeModule final : public RuntimeModule
{
   private:

     /* Build the shared command summary payload. */

     nlohmann::json BuildCommandsJSON() const
     {
          nlohmann::json CommandsJSON = nlohmann::json::array();

          for (const auto &Command : GetCommandSpecs())
          {
               nlohmann::json CommandJSON;

               CommandJSON["route"] = Command.Route;
               CommandJSON["summary"] = Command.Summary;
               CommandJSON["syntax"] = Command.Syntax;
               CommandJSON["min_parameters"] = Command.MinParameters;
               CommandJSON["max_parameters"] = Command.MaxParameters;

               CommandsJSON.push_back(CommandJSON);
          }

          return CommandsJSON;
     }

     /* Build the module status response payload. */

     std::string BuildStatusJSON() const
     {
          JsonBuilder ResponseJSON;

          ResponseJSON.Add("module", "debug");
          ResponseJSON.Add("enabled_api_route", true);
          ResponseJSON.Add("loaded", true);
          ResponseJSON.Add("message", "Debug module is loaded.");
          ResponseJSON.Add("commands", BuildCommandsJSON());

          if (Instance && HybridStorageManagerInstance().IsInitialized())
          {
               const std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();

               ResponseJSON.Add("collections_total", static_cast<unsigned long long>(Collections.size()));
          }

          return ResponseJSON.ToString();
     }

     ModulePreCheckResult AllowAndTrace(const std::string &Message)
     {
          Trace(Message);
          return ModulePreCheckResult();
     }

   public:

     DebugRuntimeModule()
         : RuntimeModule("debug", true)
     {
          AttachHooks({ModuleHook::OnThreadPoolsReady,
                       ModuleHook::OnEveryOneMinute,
                       ModuleHook::OnNewTimer,
                       ModuleHook::OnRequestAnalytics,
                       ModuleHook::OnAuthenticatedRequest,
                       ModuleHook::OnSearchCollection,
                       ModuleHook::OnSearchDocument,
                       ModuleHook::ComputeSearchWeightMultiplier,
                       ModuleHook::OnPreCreateCollection,
                       ModuleHook::OnPreUpdateCollection,
                       ModuleHook::OnPreDeleteCollection,
                       ModuleHook::OnPreAddDocument,
                       ModuleHook::OnPreUpdateDocument,
                       ModuleHook::OnPreBulkImportDocuments,
                       ModuleHook::OnPreDeleteDocument,
                       ModuleHook::OnPreDeleteDocuments,
                       ModuleHook::OnPreUpdateByQuery,
                       ModuleHook::OnPreDeleteByQuery,
                       ModuleHook::OnPreUpsertAlias,
                       ModuleHook::OnPreDeleteAlias,
                       ModuleHook::OnPreUpsertSynonym,
                       ModuleHook::OnPreDeleteSynonym,
                       ModuleHook::OnPreCreateStopword,
                       ModuleHook::OnPreDeleteStopword,
                       ModuleHook::OnPreUpsertOverride,
                       ModuleHook::OnPreDeleteOverride,
                       ModuleHook::OnPreCreateUser,
                       ModuleHook::OnPreUpdateUser,
                       ModuleHook::OnPreDeleteUser,
                       ModuleHook::OnPreCreateKey,
                       ModuleHook::OnPreUpdateKey,
                       ModuleHook::OnPreDeleteKey,
                       ModuleHook::OnPreLinksConnect,
                       ModuleHook::OnPreLinksDisconnect,
                       ModuleHook::OnPreFlush,
                       ModuleHook::OnPreRepair,
                       ModuleHook::OnCreateCollection,
                       ModuleHook::OnUpdateCollection,
                       ModuleHook::OnDeleteCollection,
                       ModuleHook::OnAddDocument,
                       ModuleHook::OnUpdateDocument,
                       ModuleHook::OnDeleteDocument,
                       ModuleHook::OnDeleteDocuments,
                       ModuleHook::OnBulkImportDocuments,
                       ModuleHook::OnUpdateByQuery,
                       ModuleHook::OnDeleteByQuery,
                       ModuleHook::OnUpsertSynonym,
                       ModuleHook::OnDeleteSynonym,
                       ModuleHook::OnCreateStopword,
                       ModuleHook::OnDeleteStopword,
                       ModuleHook::OnUpsertOverride,
                       ModuleHook::OnDeleteOverride,
                       ModuleHook::OnUpsertAlias,
                       ModuleHook::OnDeleteAlias,
                       ModuleHook::OnFlush,
                       ModuleHook::OnLinksConnect,
                       ModuleHook::OnLinksDisconnect,
                       ModuleHook::OnRepair,
                       ModuleHook::OnAnalyticsClick});
     }

     bool Start(const ServerConfig &, std::string &) override
     {
          Trace("module started");
          return true;
     }

     void Stop() override
     {
          Trace("module stopped");
     }

     void OnThreadPoolsReady() override
     {
          Trace("thread pools ready");
     }

     void OnEveryOneMinute() override
     {
//          Trace("every one minute tick");
     }

     void OnNewTimer(uint64_t DelayMS, bool Repeating, size_t TotalTimers) override
     {
          Trace("new timer: delay_ms=" + std::to_string(DelayMS) +
                ", repeating=" + (Repeating ? "true" : "false") +
                ", total_timers=" + std::to_string(TotalTimers));
     }

     void OnRequestAnalytics(const HttpRequest &Request, const HttpResponse &Response, RouteAction ActionVal) override
     {
          Trace("request analytics: action=" + std::string(RouteActionName(ActionVal)) +
                ", method=" + Request.Method +
                ", path=" + Request.Path +
                ", status=" + std::to_string(Response.StatusCode));
     }

     void OnAuthenticatedRequest(const HttpRequest &Request, RouteAction ActionVal) override
     {
          Trace("authenticated request: action=" + std::string(RouteActionName(ActionVal)) +
                ", method=" + Request.Method +
                ", path=" + Request.Path +
                ", api_key=" + (Request.APIKeyID.empty() ? "-" : Request.APIKeyID));
     }

     void OnSearchCollection(const SearchEvent &Event) override
     {
          Trace("search collection: collection=" + Event.Collection +
                ", query=" + Event.Query +
                ", analytics_tag=" + (Event.AnalyticsTag.empty() ? "-" : Event.AnalyticsTag) +
                ", found=" + std::to_string(Event.Found) +
                ", returned=" + std::to_string(Event.Returned) +
                ", search_ms=" + std::to_string(Event.SearchTimeMS) +
                ", distributed=" + (Event.Distributed ? "true" : "false") +
                ", " + DescribeActor(Event.RequesterIP, Event.RequesterUser, Event.Authenticated));
     }

     void OnSearchDocument(const SearchEvent &Event) override
     {
          Trace("search document: collection=" + Event.Collection +
                ", query=" + Event.Query +
                ", analytics_tag=" + (Event.AnalyticsTag.empty() ? "-" : Event.AnalyticsTag) +
                ", found=" + std::to_string(Event.Found) +
                ", returned=" + std::to_string(Event.Returned) +
                ", search_ms=" + std::to_string(Event.SearchTimeMS) +
                ", distributed=" + (Event.Distributed ? "true" : "false") +
                ", " + DescribeActor(Event.RequesterIP, Event.RequesterUser, Event.Authenticated));
     }

     float ComputeSearchWeightMultiplier(const std::string &Collection,
                                         const std::string &Query,
                                         const std::string &RankingMode,
                                         const SearchHit &,
                                         float BaseScore) override
     {
          Trace("search weight requested: collection=" + Collection +
                ", query=" + Query +
                ", ranking_mode=" + RankingMode +
                ", base_score=" + std::to_string(BaseScore) + ".");

          return 1.0f;
     }

     ModulePreCheckResult OnPreCreateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre create collection: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpdateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre update collection: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete collection: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreAddDocument(const std::string &Collection, const Document &DocumentObj, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre add document: collection=" + Collection + ", document_id=" + DocumentObj.ID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpdateDocument(const std::string &Collection, const Document &DocumentObj, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre update document: collection=" + Collection + ", document_id=" + DocumentObj.ID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreBulkImportDocuments(const std::string &Collection, uint64_t DocumentCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre bulk import documents: collection=" + Collection + ", count=" + std::to_string(DocumentCount) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete document: collection=" + Collection + ", document_id=" + DocumentID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteDocuments(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete documents: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpdateByQuery(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre update by query: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteByQuery(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete by query: collection=" + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpsertAlias(const std::string &AliasName, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre upsert alias: alias=" + AliasName + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteAlias(const std::string &AliasName, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete alias: alias=" + AliasName + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpsertSynonym(const std::string &Collection, const std::string &SynonymID, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre upsert synonym: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", synonym_id=" + SynonymID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteSynonym(const std::string &Collection, const std::string &SynonymID, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete synonym: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", synonym_id=" + SynonymID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreCreateStopword(const std::string &Collection, const std::vector<std::string> &Words, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre create stopword: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", count=" + std::to_string(Words.size()) + ", words=" + JoinWords(Words) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteStopword(const std::string &Collection, const std::string &Word, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete stopword: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", word=" + Word + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpsertOverride(const std::string &Collection, const std::string &OverrideID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre upsert override: collection=" + Collection + ", override_id=" + OverrideID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteOverride(const std::string &Collection, const std::string &OverrideID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete override: collection=" + Collection + ", override_id=" + OverrideID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreCreateUser(const std::string &Name, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre create user: name=" + Name + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpdateUser(const std::string &Name, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre update user: name=" + Name + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteUser(const std::string &Name, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete user: name=" + Name + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreCreateKey(const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre create key: " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreUpdateKey(const std::string &KeyID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre update key: key_id=" + KeyID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreDeleteKey(const std::string &KeyID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre delete key: key_id=" + KeyID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreLinksConnect(const std::string &Endpoint, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre links connect: endpoint=" + Endpoint + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreLinksDisconnect(const std::string &Endpoint, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre links disconnect: endpoint=" + Endpoint + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreFlush(const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre flush: " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModulePreCheckResult OnPreRepair(const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          return AllowAndTrace("pre repair: " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnCreateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("collection created: " + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpdateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("collection updated: " + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("collection deleted: " + Collection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnAddDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("document added: collection=" + Collection + ", document_id=" + DocumentID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpdateDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("document updated: collection=" + Collection + ", document_id=" + DocumentID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("document deleted: collection=" + Collection + ", document_id=" + DocumentID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteDocuments(const std::string &Collection, uint64_t Count, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("documents deleted: collection=" + Collection + ", count=" + std::to_string(Count) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnBulkImportDocuments(const std::string &Collection, uint64_t ImportedCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("documents bulk imported: collection=" + Collection + ", count=" + std::to_string(ImportedCount) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpdateByQuery(const std::string &Collection, uint64_t UpdatedCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("update by query completed: collection=" + Collection + ", updated=" + std::to_string(UpdatedCount) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteByQuery(const std::string &Collection, uint64_t DeletedCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("delete by query completed: collection=" + Collection + ", deleted=" + std::to_string(DeletedCount) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpsertSynonym(const std::string &Collection, const std::string &SynonymID, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("synonym upserted: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", synonym_id=" + SynonymID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteSynonym(const std::string &Collection, const std::string &SynonymID, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("synonym deleted: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", synonym_id=" + SynonymID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnCreateStopword(const std::string &Collection, uint64_t Count, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("stopword created: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", count=" + std::to_string(Count) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteStopword(const std::string &Collection, const std::string &Word, bool GlobalScope, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("stopword deleted: scope=" + DescribeGlobalScope(GlobalScope) + ", collection=" + Collection + ", word=" + Word + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpsertOverride(const std::string &Collection, const std::string &OverrideID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("override upserted: collection=" + Collection + ", override_id=" + OverrideID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteOverride(const std::string &Collection, const std::string &OverrideID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("override deleted: collection=" + Collection + ", override_id=" + OverrideID + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnUpsertAlias(const std::string &AliasName, const std::string &TargetCollection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("alias upserted: alias=" + AliasName + ", target_collection=" + TargetCollection + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnDeleteAlias(const std::string &AliasName, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("alias deleted: alias=" + AliasName + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnFlush(uint64_t CollectionsDeleted, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("flush completed: collections_deleted=" + std::to_string(CollectionsDeleted) + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnLinksConnect(const std::string &Endpoint, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("links connect completed: endpoint=" + Endpoint + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnLinksDisconnect(const std::string &Endpoint, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("links disconnect completed: endpoint=" + Endpoint + ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnRepair(const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated) override
     {
          Trace("repair completed: " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     void OnAnalyticsClick(const std::string &Collection,
                           const std::string &Query,
                           const std::string &DocumentID,
                           signed int Rank,
                           const std::string &RequesterIP,
                           const std::string &RequesterUser,
                           bool Authenticated) override
     {
          Trace("analytics click: collection=" + Collection +
                ", query=" + Query +
                ", document_id=" + DocumentID +
                ", rank=" + std::to_string(Rank) +
                ", " + DescribeActor(RequesterIP, RequesterUser, Authenticated));
     }

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription Description;

          Description.Name = "debug";
          Description.Summary = "Prints extensive runtime callback and request diagnostics for development debugging.";
          Description.Syntax = "hlquery-cli module debug <status|help|hello>";
          Description.MinParameters = 0;
          Description.MaxParameters = 0;
          Description.Examples.push_back("hlquery-cli module debug status");
          Description.Examples.push_back("hlquery-cli module debug help");
          Description.Examples.push_back("hlquery-cli module debug hello");

          return Description;
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          std::vector<ModuleCommandSpec> Commands;

          ModuleCommandSpec StatusCommand;
          StatusCommand.Route = "status";
          StatusCommand.Summary = "Returns the current debug-module status payload.";
          StatusCommand.Syntax = "module debug status";
          Commands.push_back(StatusCommand);

          ModuleCommandSpec HelpCommand;
          HelpCommand.Route = "help";
          HelpCommand.Summary = "Lists supported debug-module commands.";
          HelpCommand.Syntax = "module debug help";
          Commands.push_back(HelpCommand);

          ModuleCommandSpec HelloCommand;
          HelloCommand.Route = "hello";
          HelloCommand.Summary = "Returns a hello-world response from the debug module.";
          HelloCommand.Syntax = "module debug hello";
          Commands.push_back(HelloCommand);

          return Commands;
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string Route = Request.Route.empty() ? "status" : Request.Route;

          if (Route == "status")
          {
               Trace("module command executed: route=status.");

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.Body = BuildStatusJSON();
               return Response;
          }

          if (Route == "help")
          {
               Trace("module command executed: route=help.");

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.Body = JsonBuilder()
                    .Add("module", "debug")
                    .Add("message", "Available debug module commands.")
                    .Add("commands", BuildCommandsJSON())
                    .ToString();
               return Response;
          }

          if (Route == "hello")
          {
               Trace("module command executed: route=hello.");

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.Body = JsonBuilder()
                    .Add("module", "debug")
                    .Add("message", "Hello world from the debug module.")
                    .ToString();
               return Response;
          }

          return RuntimeModule::HandleCommand(Request);
     }

     HttpResponse HandleAPIRequest(const HttpRequest &Request, const std::string &SubPath) const override
     {
          Trace("module api request: method=" + Request.Method + ", path=" + Request.Path + ", sub_path=" + (SubPath.empty() ? "status" : SubPath) + ".");

          return RuntimeModule::HandleAPIRequest(Request, SubPath);
     }
};

MODULE_LOAD(DebugRuntimeModule)
