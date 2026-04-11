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

#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

namespace
{
std::string NormalizeModulePath(const std::string &Path)
{
     std::string normalized_path = Path;

     const size_t query_pos = normalized_path.find('?');

     if (query_pos != std::string::npos)
     {
          normalized_path = normalized_path.substr(0, query_pos);
     }

     if (normalized_path.size() > 1 && normalized_path.back() == '/')
     {
          normalized_path.pop_back();
     }

     return normalized_path;
}

std::vector<std::string> SplitPathSegments(const std::string &Path)
{
     std::vector<std::string> segments;

     size_t start = 0;

     while (start < Path.size())
     {
          size_t slash_pos = Path.find('/', start);

          if (slash_pos == std::string::npos)
          {
               slash_pos = Path.size();
          }

          if (slash_pos > start)
          {
               segments.push_back(Path.substr(start, slash_pos - start));
          }

          start = slash_pos + 1;
     }

     return segments;
}

std::string ExtractModuleNameFromRequest(const HttpRequest &Request)
{
     const std::vector<std::string> segments = SplitPathSegments(NormalizeModulePath(Request.Path));

     if (segments.size() < 2 || segments[0] != "modules")
     {
          return "";
     }

     return segments[1];
}

std::string ExtractModuleSubPathFromRequest(const HttpRequest &Request)
{
     const std::vector<std::string> segments = SplitPathSegments(NormalizeModulePath(Request.Path));

     if (segments.size() <= 2)
     {
          return "";
     }

     std::string sub_path;

     for (size_t i = 2; i < segments.size(); ++i)
     {
          if (!sub_path.empty())
          {
               sub_path += "/";
          }

          sub_path += segments[i];
     }

     return sub_path;
}

nlohmann::json BuildModuleDescriptionJSON(const ModuleAPIDescription &Description)
{
     nlohmann::json module_json;

     module_json["name"] = Description.Name;
     module_json["summary"] = Description.Summary;
     module_json["syntax"] = Description.Syntax;
     module_json["min_parameters"] = Description.MinParameters;
     module_json["max_parameters"] = Description.MaxParameters;
     module_json["parameters"] = nlohmann::json::array();
     module_json["examples"] = nlohmann::json::array();

     for (const auto &Parameter : Description.Parameters)
     {
          nlohmann::json parameter_json;
          parameter_json["name"] = Parameter.Name;
          parameter_json["type"] = Parameter.Type;
          parameter_json["description"] = Parameter.Description;
          parameter_json["required"] = Parameter.Required;
          module_json["parameters"].push_back(parameter_json);
     }

     for (const auto &Example : Description.Examples)
     {
          module_json["examples"].push_back(Example);
     }

     module_json["requirement_flags"] = Description.RequirementFlags;

     return module_json;
}

nlohmann::json BuildModuleCommandJSON(const ModuleCommandSpec &Command)
{
     nlohmann::json command_json;
     command_json["route"] = Command.Route;
     command_json["summary"] = Command.Summary;
     command_json["syntax"] = Command.Syntax;
     command_json["min_parameters"] = Command.MinParameters;
     command_json["max_parameters"] = Command.MaxParameters;
     command_json["parameters"] = nlohmann::json::array();
     command_json["examples"] = nlohmann::json::array();

     for (const auto &Parameter : Command.Parameters)
     {
          nlohmann::json parameter_json;
          parameter_json["name"] = Parameter.Name;
          parameter_json["type"] = Parameter.Type;
          parameter_json["description"] = Parameter.Description;
          parameter_json["required"] = Parameter.Required;
          command_json["parameters"].push_back(parameter_json);
     }

     for (const auto &Example : Command.Examples)
     {
          command_json["examples"].push_back(Example);
     }

     return command_json;
}

nlohmann::json BuildModuleWithCommandsJSON(const ModuleAPIDescription &Description, const std::vector<ModuleCommandSpec> &Commands)
{
     nlohmann::json module_json = BuildModuleDescriptionJSON(Description);
     module_json["commands"] = nlohmann::json::array();

     for (const auto &Command : Commands)
     {
          module_json["commands"].push_back(BuildModuleCommandJSON(Command));
     }

     return module_json;
}
}
HttpResponse SearchAPI::HandleListModules(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json response_json;
     response_json["modules"] = nlohmann::json::array();

     if (!Instance || !Instance->Modules)
     {
          response_json["count"] = 0;
          response_json["modules"] = nlohmann::json::array();
     }
     else
     {
          const std::vector<ModuleAPIDescription> descriptions = Instance->Modules->GetModuleAPIDescriptions();

          for (const auto &Description : descriptions)
          {
               std::vector<ModuleCommandSpec> commands;
               Instance->Modules->GetModuleCommandSpecs(Description.Name, &commands);
               response_json["modules"].push_back(BuildModuleWithCommandsJSON(Description, commands));
          }

          response_json["count"] = descriptions.size();
     }

     HttpResponse response(Status::OK, StatusText(Status::OK), "application/json");
     response.Body = response_json.dump();

     return response;
}

HttpResponse SearchAPI::HandleModuleSyntax(const HttpRequest &Request)
{
     const std::string module_name = ExtractModuleNameFromRequest(Request);

     if (module_name.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid module path.", "Module name is required.");
     }

     if (!Instance || !Instance->Modules)
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE, MODULE_UNAVAILABLE, "Module manager unavailable.", "Runtime modules are not available.");
     }

     ModuleAPIDescription description;
     std::vector<ModuleCommandSpec> commands;

     if (!Instance->Modules->GetModuleAPIDescription(module_name, &description))
     {
          return BuildErrorResponse(Status::NOT_FOUND, MODULE_NOT_FOUND, "Module not found.", "The specified module is not loaded.");
     }

     Instance->Modules->GetModuleCommandSpecs(module_name, &commands);

     nlohmann::json response_json = BuildModuleWithCommandsJSON(description, commands);

     HttpResponse response(Status::OK, StatusText(Status::OK), "application/json");
     response.Body = response_json.dump();

     return response;
}

HttpResponse SearchAPI::HandleModuleAPI(const HttpRequest &Request)
{
     const std::string module_name = ExtractModuleNameFromRequest(Request);

     if (module_name.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid module path.", "Module name is required.");
     }

     if (!Instance || !Instance->Modules)
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE, MODULE_UNAVAILABLE, "Module manager unavailable.", "Runtime modules are not available.");
     }

     const std::string sub_path = ExtractModuleSubPathFromRequest(Request);

     if (sub_path == "syntax")
     {
          return HandleModuleSyntax(Request);
     }

     HttpResponse response = Instance->Modules->HandleModuleAPIRequest(module_name, Request, sub_path);

     if (response.StatusCode == 404 && response.Body.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, MODULE_ROUTE_NOT_FOUND, "Route not found.", "The module did not recognize the requested route.");
     }

     return response;
}
