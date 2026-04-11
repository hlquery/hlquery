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
#include <vector>

#include "core/modules.h"
#include "search/cstore.h"
#include "utils/jsonbuilder.h"

/* Runtime module that manages collection language metadata. */

class LangRuntimeModule final : public RuntimeModule
{
   private:

     /* Resolves the collection argument from positional or named parameters. */

     std::string GetCollectionParameter(const ModuleCommandRequest &Request) const
     {
          if (!Request.Parameters.empty())
          {
               return Request.Parameters[0];
          }

          const auto it = Request.NamedParameters.find("collection");

          if (it != Request.NamedParameters.end())
          {
               return it->second;
          }

          return "";
     }

     /* Resolves the language argument from positional or named parameters. */

     std::string GetLanguageParameter(const ModuleCommandRequest &Request) const
     {
          if (Request.Parameters.size() >= 2)
          {
               return Request.Parameters[1];
          }

          const auto it = Request.NamedParameters.find("lang");

          if (it != Request.NamedParameters.end())
          {
               return it->second;
          }

          return "";
     }

   public:

     /* Initialize the lang runtime module. */

     LangRuntimeModule()
         : RuntimeModule("lang", true)
     {
     }

     /* Start the module. */

     bool Start(const ServerConfig &, std::string &) override
     {
          return true;
     }

     /* Stop the module. */

     void Stop() override
     {
     }

     /* Describe the module CLI surface. */

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription description;

          description.Name = "lang";
          description.Summary = "Manages collection _lang metadata.";
          description.Syntax = "hlquery-cli module lang <status|get|set> <collection> [lang]";
          description.Examples.push_back("hlquery-cli module lang status");
          description.Examples.push_back("hlquery-cli module lang get books");
          description.Examples.push_back("hlquery-cli module lang set books en");

          return description;
     }

     /* Describe the supported module commands. */

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          std::vector<ModuleCommandSpec> commands;

          ModuleCommandParameterSpec collection_param;
          collection_param.Name = "collection";
          collection_param.Type = "string";
          collection_param.Description = "Collection name.";
          collection_param.Required = true;

          ModuleCommandParameterSpec lang_param;
          lang_param.Name = "lang";
          lang_param.Type = "string";
          lang_param.Description = "Language code to store under _lang.";
          lang_param.Required = true;

          ModuleCommandSpec status_command;

          status_command.Route = "status";
          status_command.Summary = "Shows lang module status.";
          status_command.Syntax = "module lang status";
          commands.push_back(status_command);

          ModuleCommandSpec get_command;

          get_command.Route = "get";
          get_command.Summary = "Gets _lang metadata for one collection.";
          get_command.Syntax = "module lang get <collection>";
          get_command.MinParameters = 1;
          get_command.MaxParameters = 1;
          get_command.Parameters.push_back(collection_param);
          commands.push_back(get_command);

          ModuleCommandSpec set_command;

          set_command.Route = "set";
          set_command.Summary = "Sets _lang metadata for one collection.";
          set_command.Syntax = "module lang set <collection> <lang>";
          set_command.MinParameters = 2;
          set_command.MaxParameters = 2;
          set_command.Parameters.push_back(collection_param);
          set_command.Parameters.push_back(lang_param);
          commands.push_back(set_command);

          return commands;
     }

     /* Execute one lang module command. */

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string route = Request.Route.empty() ? "status" : Request.Route;

          if (route == "status")
          {
               ModuleCommandResponse response;

               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "lang")
                    .Add("message", "Collection language metadata module is loaded.")
                    .ToString();

               return response;
          }

          if (route == "get")
          {
               const std::string collection = GetCollectionParameter(Request);

               if (collection.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Missing collection parameter.").ToString();

                    return response;
               }

               if (!HybridStorageManagerInstance().CollectionExists(collection))
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 404;
                    response.Body = JsonBuilder().Add("error", "Collection not found.").ToString();

                    return response;
               }

               CollectionConfig config;
               HybridStorageManagerInstance().GetCollectionConfig(collection, config);

               ModuleCommandResponse response;

               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "lang")
                    .Add("collection", collection)
                    .Add("lang", config.Metadata.count("_lang") ? config.Metadata["_lang"] : "")
                    .ToString();

               return response;
          }

          if (route == "set")
          {
               const std::string collection = GetCollectionParameter(Request);
               const std::string lang = GetLanguageParameter(Request);

               if (collection.empty() || lang.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Missing collection or lang parameter.").ToString();

                    return response;
               }

               if (!HybridStorageManagerInstance().CollectionExists(collection))
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 404;
                    response.Body = JsonBuilder().Add("error", "Collection not found.").ToString();

                    return response;
               }

               if (!HybridStorageManagerInstance().UpdateCollectionMetadata(collection, "_lang", lang))
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 500;
                    response.Body = JsonBuilder().Add("error", "Failed to update collection metadata.").ToString();

                    return response;
               }

               ModuleCommandResponse response;

               response.Success = true;
               response.Body = JsonBuilder()
                    .Add("module", "lang")
                    .Add("collection", collection)
                    .Add("lang", lang)
                    .Add("message", "Collection language updated.")
                    .ToString();

               return response;
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(LangRuntimeModule)
