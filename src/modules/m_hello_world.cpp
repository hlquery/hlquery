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
#include "utils/jsonbuilder.h"

/* Minimal runtime module that exposes one hello-world command. */

class HelloWorldModule final : public AutoCompositeRuntimeModule<HelloWorldModule>
{
   public:
     HelloWorldModule()
         : AutoCompositeRuntimeModule("hello_world", true)
     {
     }

     bool Start(const ServerConfig &, std::string &) override
     {
          return true;
     }

     void Stop() override
     {
     }

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription Description;

          Description.Name = "hello_world";
          Description.Summary = "Returns a hello-world response from a runtime module.";
          Description.Syntax = "hlquery-cli module hello_world hello";
          Description.Examples.push_back("hlquery-cli module hello_world hello");

          return Description;
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          ModuleCommandSpec Hello;

          Hello.Route = "hello";
          Hello.Summary = "Returns the module greeting.";
          Hello.Syntax = "module hello_world hello";
          Hello.Examples.push_back("hlquery-cli module hello_world hello");

          return {Hello};
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string Route = Request.Route.empty() ? "hello" : Request.Route;

          if (Route == "hello")
          {
               ModuleCommandResponse Response;

               Response.Success = true;
               Response.Message = "Hello world from hlquery.";
               Response.Body = JsonBuilder()
                                    .Add("module", "hello_world")
                                    .Add("message", Response.Message)
                                    .ToString();
               return Response;
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(HelloWorldModule)
