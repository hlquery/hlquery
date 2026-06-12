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

#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <string>
#include <vector>

#include "cli/cliutils.h"
#include "app.h"
#include "runtime/clock.h"
#include "vendor/json/json.hpp"

enum class LoadedModuleFilter
{
     All,
     CoreOnly,
     OptionalOnly
};

std::string JoinStrings(const std::vector<std::string> &Values, const std::string &Separator)
{
     std::string joined_value;

     for (size_t i = 0; i < Values.size(); ++i)
     {
          if (i > 0)
          {
               joined_value += Separator;
          }

          joined_value += Values[i];
     }

     return joined_value;
}

/* Converts one JSON value into a compact display string. */

std::string JSONValueToString(const nlohmann::json &Value)
{
     if (Value.is_string())
     {
          return Value.get<std::string>();
     }

     if (Value.is_boolean())
     {
          return Value.get<bool>() ? "true" : "false";
     }

     if (Value.is_number_integer())
     {
          return std::to_string(Value.get<long long>());
     }

     if (Value.is_number_unsigned())
     {
          return std::to_string(Value.get<unsigned long long>());
     }

     if (Value.is_number_float())
     {
          return std::to_string(Value.get<double>());
     }

     if (Value.is_null())
     {
          return "null";
     }

     return Value.dump();
}

/* Prints one generic key/value view for an object payload. */

void PrintObjectAsTable(HLQueryCLI &CLI, const nlohmann::json &ObjectValue)
{
     std::vector<std::vector<std::string>> rows;

     for (auto it = ObjectValue.begin(); it != ObjectValue.end(); ++it)
     {
          if (it.value().is_array() || it.value().is_object())
          {
               continue;
          }

          rows.push_back({it.key(), JSONValueToString(it.value())});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({"Key", "Value"}, rows);
     }
}

/* Prints one command list in table form. */

void PrintCommandsTable(HLQueryCLI &CLI, const nlohmann::json &Commands)
{
     std::vector<std::vector<std::string>> rows;

     for (const auto &Command : Commands)
     {
          if (!Command.is_object())
          {
               continue;
          }

          const std::string route = Command.contains("route") ? JSONValueToString(Command["route"]) : "";
          const std::string syntax = Command.contains("syntax") ? JSONValueToString(Command["syntax"]) : "";
          const std::string summary = Command.contains("summary") ? JSONValueToString(Command["summary"]) : "";

          rows.push_back({route, syntax, summary});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({"Route", "Syntax", "Summary"}, rows);
     }
}

/* Builds one readable parameter signature for a command. */

std::string BuildParameterSignature(const nlohmann::json &Parameters)
{
     std::vector<std::string> segments;

     for (const auto &Parameter : Parameters)
     {
          if (!Parameter.is_object())
          {
               continue;
          }

          const std::string name = Parameter.contains("name") ? JSONValueToString(Parameter["name"]) : "param";
          const std::string type = Parameter.contains("type") ? JSONValueToString(Parameter["type"]) : "string";
          const bool required = Parameter.contains("required") && Parameter["required"].is_boolean() ? Parameter["required"].get<bool>() : false;

          segments.push_back((required ? "" : "[") + name + ":" + type + (required ? "" : "]"));
     }

     return JoinStrings(segments, " ");
}

/* Prints parameter details for each command. */

void PrintCommandDetails(HLQueryCLI &CLI, const nlohmann::json &Commands)
{
     for (const auto &Command : Commands)
     {
          if (!Command.is_object())
          {
               continue;
          }

          const std::string route = Command.contains("route") ? JSONValueToString(Command["route"]) : "";
          const std::string syntax = Command.contains("syntax") ? JSONValueToString(Command["syntax"]) : "";
          const std::string summary = Command.contains("summary") ? JSONValueToString(Command["summary"]) : "";

          std::cout << "\nRoute: " << route << "\n";

          if (!summary.empty())
          {
               std::cout << "Summary: " << summary << "\n";
          }

          if (!syntax.empty())
          {
               std::cout << "Syntax: " << syntax << "\n";
          }

          if (!Command.contains("parameters") || !Command["parameters"].is_array() || Command["parameters"].empty())
          {
               std::cout << "Parameters: none\n";
               continue;
          }

          const std::string signature = BuildParameterSignature(Command["parameters"]);

          if (!signature.empty())
          {
               std::cout << "Parameters: " << signature << "\n";
          }

          std::vector<std::vector<std::string>> rows;

          for (const auto &Parameter : Command["parameters"])
          {
               if (!Parameter.is_object())
               {
                    continue;
               }

               rows.push_back({
                    Parameter.contains("name") ? JSONValueToString(Parameter["name"]) : "",
                    Parameter.contains("type") ? JSONValueToString(Parameter["type"]) : "",
                    Parameter.contains("required") ? JSONValueToString(Parameter["required"]) : "false",
                    Parameter.contains("description") ? JSONValueToString(Parameter["description"]) : ""
               });
          }

          if (!rows.empty())
          {
               CLI.PrintTable({"Name", "Type", "Required", "Description"}, rows);
         }
    }
}

/* Formats module requirement metadata for display. */

std::string FormatModuleRequirements(const nlohmann::json &Module)
{
     std::vector<std::string> Requirements;

     if (Requirements.empty())
     {
          return "none";
     }

     return JoinStrings(Requirements, ", ");
}

/* Prints one array of scalar values in table form. */

void PrintScalarArrayTable(HLQueryCLI &CLI, const std::string &Header, const nlohmann::json &Values)
{
     std::vector<std::vector<std::string>> rows;

     for (const auto &Value : Values)
     {
          rows.push_back({JSONValueToString(Value)});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({Header}, rows);
     }
}

/* Prints one array of collection names in table form. */

void PrintCollectionsTable(HLQueryCLI &CLI, const nlohmann::json &Values)
{
     PrintScalarArrayTable(CLI, "Collection", Values);
}

LoadedModuleFilter ParseLoadedModuleFilter(const std::string &FilterText, bool &Valid)
{
     Valid = true;

     if (FilterText.empty())
     {
          return LoadedModuleFilter::All;
     }

     if (FilterText == "1")
     {
          return LoadedModuleFilter::CoreOnly;
     }

     if (FilterText == "0")
     {
          return LoadedModuleFilter::OptionalOnly;
     }

     Valid = false;
     return LoadedModuleFilter::All;
}

bool IsCoreLoadedModule(const std::string &ModuleName)
{
     return ModuleName.rfind("core_", 0) == 0;
}

void PrintLoadedModulesTable(HLQueryCLI &CLI, const std::vector<std::string> &ModuleNames)
{
     std::vector<std::vector<std::string>> rows;
     rows.reserve(ModuleNames.size());

     for (const auto &ModuleName : ModuleNames)
     {
          rows.push_back({ModuleName});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({"Module"}, rows);
          return;
     }

     std::cout << "No loaded modules." << std::endl;
}

void PrintDocumentsTable(HLQueryCLI &CLI, const nlohmann::json &Values)
{
     std::vector<std::vector<std::string>> rows;

     for (const auto &Value : Values)
     {
          if (!Value.is_object())
          {
               continue;
          }

          rows.push_back({
               Value.contains("collection") ? JSONValueToString(Value["collection"]) : "",
               Value.contains("id") ? JSONValueToString(Value["id"]) : "",
               Value.contains("title") ? JSONValueToString(Value["title"]) : "",
               Value.contains("content_preview") ? JSONValueToString(Value["content_preview"]) : ""});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({"Collection", "ID", "Title", "Preview"}, rows);
     }
}

void PrintHitsTable(HLQueryCLI &CLI, const nlohmann::json &Values)
{
     std::vector<std::vector<std::string>> rows;

     for (const auto &Value : Values)
     {
          if (!Value.is_object())
          {
               continue;
          }

          const nlohmann::json &document = Value.contains("document") && Value["document"].is_object() ? Value["document"] : Value;
          rows.push_back({
               Value.contains("collection") ? JSONValueToString(Value["collection"]) : "",
               document.contains("id") ? JSONValueToString(document["id"]) : "",
               document.contains("title") ? JSONValueToString(document["title"]) : "",
               Value.contains("score") ? JSONValueToString(Value["score"]) : (Value.contains("text_match") ? JSONValueToString(Value["text_match"]) : "")});
     }

     if (!rows.empty())
     {
          CLI.PrintTable({"Collection", "ID", "Title", "Score"}, rows);
     }
     else
     {
          std::cout << "No matching documents were returned. Try a broader query or adjust your filters." << std::endl;
     }
}

/* Prints one module JSON response in a CLI-friendly format. */

void PrintModuleJSON(HLQueryCLI &CLI, const nlohmann::json &Root)
{
     if (Root.is_object())
     {
          if (Root.contains("action") &&
              Root["action"].is_string() &&
              (Root["action"].get<std::string>() == "list_collections" ||
               Root["action"].get<std::string>() == "match_collections") &&
              Root.contains("collections") &&
              Root["collections"].is_array())
          {
               std::cout << Root.dump(2) << std::endl;
               return;
          }

          PrintObjectAsTable(CLI, Root);

          if (Root.contains("requirement_flags"))
          {
               std::cout << "Requirements: " << FormatModuleRequirements(Root) << "\n";
          }

          if (Root.contains("commands") && Root["commands"].is_array())
          {
               PrintCommandsTable(CLI, Root["commands"]);
               PrintCommandDetails(CLI, Root["commands"]);
               return;
          }

          if (Root.contains("words") && Root["words"].is_array())
          {
               PrintScalarArrayTable(CLI, "Word", Root["words"]);
               return;
          }

          if (Root.contains("follow_ups") && Root["follow_ups"].is_array())
          {
               PrintScalarArrayTable(CLI, "Follow Up", Root["follow_ups"]);
          }

          if (Root.contains("collections") && Root["collections"].is_array())
          {
               PrintCollectionsTable(CLI, Root["collections"]);
               /* fall through so hits can also be printed */
          }

          if (Root.contains("documents") && Root["documents"].is_array())
          {
               PrintDocumentsTable(CLI, Root["documents"]);
               return;
          }

          if (Root.contains("hits") && Root["hits"].is_array())
          {
               PrintHitsTable(CLI, Root["hits"]);
               return;
          }

          if (Root.contains("modules") && Root["modules"].is_array())
          {
               for (const auto &Module : Root["modules"])
               {
                    if (!Module.is_object())
                    {
                         continue;
                    }

                    const std::string name = Module.contains("name") ? JSONValueToString(Module["name"]) : "";
                    const std::string summary = Module.contains("summary") ? JSONValueToString(Module["summary"]) : "";
                    const std::string syntax = Module.contains("syntax") ? JSONValueToString(Module["syntax"]) : "";
                    std::cout << "\nModule: " << name << "\n";

                    if (!summary.empty())
                    {
                         std::cout << "Summary: " << summary << "\n";
                    }

                    if (!syntax.empty())
                    {
                         std::cout << "Syntax: " << syntax << "\n";
                    }

                    if (Module.contains("requirement_flags"))
                    {
                         std::cout << "Requirements: " << FormatModuleRequirements(Module) << "\n";
                    }

                    if (Module.contains("commands") && Module["commands"].is_array() && !Module["commands"].empty())
                    {
                         PrintCommandsTable(CLI, Module["commands"]);
                         PrintCommandDetails(CLI, Module["commands"]);
                    }
                    else
                    {
                         std::cout << "Commands: none\n";
                    }
               }

               return;
          }

          return;
     }

     if (Root.is_array())
     {
          PrintScalarArrayTable(CLI, "Value", Root);
          return;
     }

     std::cout << Root.dump(2) << std::endl;
}

/* Parses and prints one module response body. */

void PrintModuleResponse(HLQueryCLI &CLI, const std::string &ModuleName, const std::string &Body, bool json_output = false)
{
     if (json_output)
     {
          std::cout << Body << std::endl;
          return;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(Body);

          if (ModuleName == "ai_query")
          {
               std::cout << root.dump(2) << std::endl;
               return;
          }

          PrintModuleJSON(CLI, root);
     }
     catch (...)
     {
          std::cout << Body << std::endl;
     }
}

bool TryParseQueuedLlamaJobID(const std::string &Body, std::string &JobID)
{
     try
     {
          const nlohmann::json root = nlohmann::json::parse(Body);
          if (!root.is_object())
          {
               return false;
          }

          if (!root.contains("job_id") || !root["job_id"].is_string())
          {
               return false;
          }

          JobID = root["job_id"].get<std::string>();
          return !JobID.empty();
     }
     catch (...)
     {
          return false;
     }
}

bool TryPrintCompletedLlamaJob(HLQueryCLI &CLI, const std::string &Body, bool json_output)
{
     try
     {
          const nlohmann::json root = nlohmann::json::parse(Body);
          if (!root.is_object() || !root.contains("status") || !root["status"].is_string())
          {
               return false;
          }

          const std::string status = root["status"].get<std::string>();
          if (status != "completed" && status != "failed")
          {
               return false;
          }

          if (root.contains("result"))
          {
               if (json_output)
               {
                    std::cout << root["result"].dump() << std::endl;
               }
               else
               {
                    PrintModuleJSON(CLI, root["result"]);
               }

               return true;
          }

          if (json_output)
          {
               std::cout << Body << std::endl;
          }
          else
          {
               PrintModuleJSON(CLI, root);
          }

          return true;
     }
     catch (...)
     {
          return false;
     }
}
void HLQueryCLI::ListModules(const std::string &filter)
{
     bool valid_filter = false;
     const LoadedModuleFilter parsed_filter = ParseLoadedModuleFilter(filter, valid_filter);

     if (!valid_filter)
     {
          PrintError("Invalid modules filter", "Usage: modules [1|0]");
          return;
     }

     HTTPResponse response = MakeRequest("GET", "/health");

     if (CheckRequestFailed(response, false, "/health"))
     {
          return;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("loaded_modules") || !root["loaded_modules"].is_array())
          {
               PrintError("Server response did not include loaded_modules", "");
               return;
          }

          std::vector<std::string> module_names;

          for (const auto &ModuleValue : root["loaded_modules"])
          {
               if (!ModuleValue.is_string())
               {
                    continue;
               }

               const std::string module_name = ModuleValue.get<std::string>();
               const bool is_core = IsCoreLoadedModule(module_name);

               if (parsed_filter == LoadedModuleFilter::CoreOnly && !is_core)
               {
                    continue;
               }

               if (parsed_filter == LoadedModuleFilter::OptionalOnly && is_core)
               {
                    continue;
               }

               module_names.push_back(module_name);
          }

          PrintLoadedModulesTable(*this, module_names);
     }
     catch (...)
     {
          PrintError("Failed to parse /health response", "");
     }
}

void HLQueryCLI::ShowModuleSyntax(const std::string &module_name)
{
     HTTPResponse response = MakeRequest("GET", "/modules/" + hlquery_cli::UrlEncode(module_name) + "/syntax");

     if (CheckRequestFailed(response, false, "/modules/" + module_name + "/syntax"))
     {
          return;
     }

     PrintModuleResponse(*this, module_name, response.Body);
}

void HLQueryCLI::RunModuleCommand(const std::string &module_name, const std::string &route, const std::vector<std::string> &args)
{
     std::string effective_route = route;
     std::vector<std::string> effective_args = args;
     bool async_requested = false;
     if (!route.empty())
     {
          const std::string syntax_path = "/modules/" + hlquery_cli::UrlEncode(module_name) + "/syntax";
          HTTPResponse syntax_response = MakeRequest("GET", syntax_path);

          if (!CheckRequestFailed(syntax_response, false, syntax_path))
          {
               try
               {
                    const nlohmann::json syntax_json = nlohmann::json::parse(syntax_response.Body);
                    std::unordered_set<std::string> known_routes;

                    if (syntax_json.contains("commands") && syntax_json["commands"].is_array())
                    {
                         for (const auto &Command : syntax_json["commands"])
                         {
                              if (!Command.is_object() || !Command.contains("route") || !Command["route"].is_string())
                              {
                                   continue;
                              }

                              known_routes.insert(Command["route"].get<std::string>());
                         }
                    }

                    if (!known_routes.empty() && known_routes.find(route) == known_routes.end())
                    {
                         if (known_routes.find("ask") != known_routes.end())
                         {
                              effective_route = "ask";
                         }
                         else
                         {
                              effective_route.clear();
                         }
                         effective_args.clear();
                         effective_args.push_back(route);
                         effective_args.insert(effective_args.end(), args.begin(), args.end());
                    }
               }
               catch (...)
               {
               }
          }
     }

     nlohmann::json body_json = nlohmann::json::object();

     std::vector<std::string> positional_parameters;
     bool json_output = false;

     for (const auto &arg : effective_args)
     {
          if (arg == "--json" || arg == "--dump")
          {
               json_output = true;
               continue;
          }

          if (arg == "--async")
          {
               async_requested = true;
               continue;
          }

          if (arg.rfind("--", 0) == 0)
          {
               const size_t equals_pos = arg.find('=');

               if (equals_pos == std::string::npos)
               {
                    const std::string flag_name = arg.substr(2);
                    body_json[flag_name] = "true";
               }
               else
               {
                    const std::string flag_name = arg.substr(2, equals_pos - 2);
                    const std::string flag_value = arg.substr(equals_pos + 1);
                    body_json[flag_name] = flag_value;
               }
          }
          else
          {
               positional_parameters.push_back(arg);
          }
     }

     if (!positional_parameters.empty())
     {
          body_json["parameters"] = positional_parameters;
     }

     const std::string normalized_route = effective_route.empty() ? "" : "/" + hlquery_cli::UrlEncode(effective_route);

     const std::string path = "/modules/" + hlquery_cli::UrlEncode(module_name) + normalized_route;

     const int request_timeout_seconds =
          (module_name == "llama") ? std::max(60, DefaultTimeoutSeconds) : DefaultTimeoutSeconds;

     HTTPResponse response = MakeRequest("POST", path, body_json.dump(), request_timeout_seconds);

     if (CheckRequestFailed(response, false, path))
     {
          return;
     }

     if (module_name == "llama" && !async_requested && response.StatusCode == 202)
     {
          std::string job_id;
          if (TryParseQueuedLlamaJobID(response.Body, job_id))
          {
               const auto deadline = Now() + std::chrono::seconds(std::max(1, request_timeout_seconds));

               while (Now() < deadline)
               {
                    nlohmann::json poll_body = nlohmann::json::object();
                    poll_body["parameters"] = nlohmann::json::array({job_id});

                    HTTPResponse poll_response = MakeRequest("POST", "/modules/llama/job", poll_body.dump(), request_timeout_seconds);
                    if (CheckRequestFailed(poll_response, false, "/modules/llama/job"))
                    {
                         return;
                    }

                    if (TryPrintCompletedLlamaJob(*this, poll_response.Body, json_output))
                    {
                         return;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
               }
          }
     }

     PrintModuleResponse(*this, module_name, response.Body, json_output);
}

void HLQueryCLI::LoadModule(const std::string &module_name)
{
     const std::string path = "/modules/load/" + hlquery_cli::UrlEncode(module_name);
     HTTPResponse response = MakeRequest("POST", path, "{}");

     if (CheckRequestFailed(response, false, path))
     {
          return;
     }

     PrintModuleResponse(*this, "load", response.Body);
}

void HLQueryCLI::UnloadModule(const std::string &module_name)
{
     const std::string path = "/modules/unload/" + hlquery_cli::UrlEncode(module_name);
     HTTPResponse response = MakeRequest("POST", path, "{}");

     if (CheckRequestFailed(response, false, path))
     {
          return;
     }

     PrintModuleResponse(*this, "unload", response.Body);
}
