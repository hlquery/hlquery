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

#include <string>

#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "core/modules.h"
#include "utils/protocol.h"

namespace
{
/* Build a standard deny response for demo mode. */

ModulePreCheckResult MakeDeniedResult(const std::string &Operation, const std::string &Message)
{
     ModulePreCheckResult Result;

     Result.Action = ModulePreCheckAction::Deny;
     Result.HttpStatus = Status::FORBIDDEN;
     Result.ProtocolCode = Code::SYSTEM_MAINTENANCE;
     Result.Message = "Demo mode is enabled";
     Result.Details = Message + " " + Operation + " is disabled.";

     return Result;
}
}

/* Runtime module that enforces demo mode restrictions. */

class DemoRuntimeModule final : public AutoRuntimeModule<DemoRuntimeModule>
{
   private:

     bool BlockLinks = true;

     bool BlockAdmin = true;
     std::string Message = "Search and browsing are enabled. Write and admin actions are blocked in demo mode.";

     /* Deny a mutating operation when demo mode is active. */

     ModulePreCheckResult Block(const std::string &Operation) const
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("modules", "Demo module denied operation: " + Operation + ".");
          }

          return MakeDeniedResult(Operation, Message);
     }

     /* Record a successful operation observed by the demo module. */

     void Observe(const std::string &Operation) const
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("modules", "Demo module observed successful operation: " + Operation + ".");
          }
     }

   public:

     /* Initialize the demo runtime module. */

     DemoRuntimeModule()
          : AutoRuntimeModule("demo", false)
     {
     }

     /* Start the module and load demo settings. */

     bool Start(const ServerConfig &Config, std::string &) override
     {
          auto Tag = Config.GetConfigReader().GetTag("demo");

          if (Tag)
          {
               BlockLinks = Tag->GetBool("block_links", true);
               BlockAdmin = Tag->GetBool("block_admin", true);
               Message = Tag->GetString("message", Message);
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("modules", std::string("Demo module loaded - block_links=") + (BlockLinks ? "true" : "false") + ", block_admin=" + (BlockAdmin ? "true" : "false") + ".");
          }

          if (Instance && Instance->Modules)
          {
               Instance->Modules->SetDemoModeState(true, Message);
          }

          return true;
     }

     /* Stop the module. */

     void Stop() override
     {
          if (Instance && Instance->Modules)
          {
               Instance->Modules->SetDemoModeState(false, "");
          }
     }

     /* Report whether demo mode is enabled. */

     bool IsDemoModeEnabled() const override
     {
          return true;
     }

     /* Return the user facing demo mode message. */

     std::string GetDemoModeMessage() const override
     {
          return Message;
     }

     /* Block collection creation. */

     ModulePreCheckResult OnPreCreateCollection(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Collection creation");
     }

     /* Block collection updates. */

     ModulePreCheckResult OnPreUpdateCollection(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Collection update");
     }

     /* Block collection deletion. */

     ModulePreCheckResult OnPreDeleteCollection(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Collection deletion");
     }

     /* Block document creation. */

     ModulePreCheckResult OnPreAddDocument(const std::string &, const Document &, const std::string &, const std::string &, bool) override
     {
          return Block("Document creation");
     }

     /* Block document updates. */

     ModulePreCheckResult OnPreUpdateDocument(const std::string &, const Document &, const std::string &, const std::string &, bool) override
     {
          return Block("Document update");
     }

     /* Block bulk imports. */

     ModulePreCheckResult OnPreBulkImportDocuments(const std::string &, uint64_t, const std::string &, const std::string &, bool) override
     {
          return Block("Document import");
     }

     /* Block document deletion. */

     ModulePreCheckResult OnPreDeleteDocument(const std::string &, const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Document deletion");
     }

     /* Block bulk document deletion. */

     ModulePreCheckResult OnPreDeleteDocuments(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Bulk document deletion");
     }

     /* Block update by query operations. */

     ModulePreCheckResult OnPreUpdateByQuery(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Update by query");
     }

     /* Block delete by query operations. */

     ModulePreCheckResult OnPreDeleteByQuery(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Delete by query");
     }

     /* Block alias changes. */

     ModulePreCheckResult OnPreUpsertAlias(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Alias update");
     }

     /* Block alias deletion. */

     ModulePreCheckResult OnPreDeleteAlias(const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Alias deletion");
     }

     /* Block synonym changes. */

     ModulePreCheckResult OnPreUpsertSynonym(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool) override
     {
          return Block("Synonym update");
     }

     /* Block synonym deletion. */

     ModulePreCheckResult OnPreDeleteSynonym(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool) override
     {
          return Block("Synonym deletion");
     }

     /* Block stopword changes. */

     ModulePreCheckResult OnPreCreateStopword(const std::string &, const std::vector<std::string> &, bool, const std::string &, const std::string &, bool) override
     {
          return Block("Stopword update");
     }

     /* Block stopword deletion. */

     ModulePreCheckResult OnPreDeleteStopword(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool) override
     {
          return Block("Stopword deletion");
     }

     /* Block override changes. */

     ModulePreCheckResult OnPreUpsertOverride(const std::string &, const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Override update");
     }

     /* Block override deletion. */

     ModulePreCheckResult OnPreDeleteOverride(const std::string &, const std::string &, const std::string &, const std::string &, bool) override
     {
          return Block("Override deletion");
     }

     /* Block user creation when admin restrictions are enabled. */

     ModulePreCheckResult OnPreCreateUser(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("User creation") : ModulePreCheckResult();
     }

     /* Block user updates when admin restrictions are enabled. */

     ModulePreCheckResult OnPreUpdateUser(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("User update") : ModulePreCheckResult();
     }

     /* Block user deletion when admin restrictions are enabled. */

     ModulePreCheckResult OnPreDeleteUser(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("User deletion") : ModulePreCheckResult();
     }

     /* Block key creation when admin restrictions are enabled. */

     ModulePreCheckResult OnPreCreateKey(const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("API key creation") : ModulePreCheckResult();
     }

     /* Block key updates when admin restrictions are enabled. */

     ModulePreCheckResult OnPreUpdateKey(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("API key update") : ModulePreCheckResult();
     }

     /* Block key deletion when admin restrictions are enabled. */

     ModulePreCheckResult OnPreDeleteKey(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("API key deletion") : ModulePreCheckResult();
     }

     /* Block link connect when link restrictions are enabled. */

     ModulePreCheckResult OnPreLinksConnect(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockLinks ? Block("Link connect") : ModulePreCheckResult();
     }

     /* Block link disconnect when link restrictions are enabled. */

     ModulePreCheckResult OnPreLinksDisconnect(const std::string &, const std::string &, const std::string &, bool) override
     {
          return BlockLinks ? Block("Link disconnect") : ModulePreCheckResult();
     }

     /* Block flush when admin restrictions are enabled. */

     ModulePreCheckResult OnPreFlush(const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("Flush") : ModulePreCheckResult();
     }

     /* Block repair when admin restrictions are enabled. */

     ModulePreCheckResult OnPreRepair(const std::string &, const std::string &, bool) override
     {
          return BlockAdmin ? Block("Repair") : ModulePreCheckResult();
     }

     /* Observe update-by-query completion when demo mode allows it. */

     void OnUpdateByQuery(const std::string &Collection, uint64_t UpdatedCount, const std::string &, const std::string &, bool) override
     {
          Observe("Update by query on collection '" + Collection + "' updated " + std::to_string(UpdatedCount) + " documents");
     }

     /* Observe delete-by-query completion when demo mode allows it. */

     void OnDeleteByQuery(const std::string &Collection, uint64_t DeletedCount, const std::string &, const std::string &, bool) override
     {
          Observe("Delete by query on collection '" + Collection + "' deleted " + std::to_string(DeletedCount) + " documents");
     }

     /* Observe link connect completion when demo mode allows it. */

     void OnLinksConnect(const std::string &Endpoint, const std::string &, const std::string &, bool) override
     {
          Observe("Link connect to '" + Endpoint + "'");
     }

     /* Observe link disconnect completion when demo mode allows it. */

     void OnLinksDisconnect(const std::string &Endpoint, const std::string &, const std::string &, bool) override
     {
          Observe("Link disconnect from '" + Endpoint + "'");
     }

     /* Observe repair completion when demo mode allows it. */

     void OnRepair(const std::string &, const std::string &, bool) override
     {
          Observe("Repair");
     }
};

MODULE_LOAD(DemoRuntimeModule)
