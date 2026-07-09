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

#include <chrono>
#include <regex>
#include <stdexcept>

#include "api/apikeys.h"
#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "vendor/json/json.hpp"

/* Provides API key management endpoints for administrative clients. */

using json = nlohmann::json;

/* ExtractKeyIDFromPath extracts key ID from path like /keys/{id}. */

std::string SearchAPI::ExtractKeyIDFromPath(const std::string &Path)
{
     std::regex KeyRegex(R"(/keys/([^/]+))");
     std::smatch Match;

     if (std::regex_search(Path, Match, KeyRegex))
     {
          return Match[1].str();
     }

     return "";
}

/* Helper to convert APIKeyAction to string. */

static std::string ActionToString(APIKeyAction Action)
{
     switch (Action)
     {
          case APIKeyAction::SEARCH:
               return "search";
          case APIKeyAction::CREATE:
               return "create";
          case APIKeyAction::UPDATE:
               return "update";
          case APIKeyAction::DELETE:
               return "delete";
          case APIKeyAction::COLLECTIONS_LIST:
               return "collections_list";
          case APIKeyAction::COLLECTIONS_CREATE:
               return "collections_create";
          case APIKeyAction::COLLECTIONS_DELETE:
               return "collections_delete";
          case APIKeyAction::IMPORT:
               return "import";
          case APIKeyAction::ALL:
               return "*";
     }

     return "unknown";
}

/* Helper to convert string to APIKeyAction. */

static bool StringToAction(const std::string &S, APIKeyAction &OutAction)
{
     if (S == "search")
     {
          OutAction = APIKeyAction::SEARCH;
          return true;
     }

     if (S == "create")
     {
          OutAction = APIKeyAction::CREATE;
          return true;
     }

     if (S == "update")
     {
          OutAction = APIKeyAction::UPDATE;
          return true;
     }

     if (S == "delete")
     {
          OutAction = APIKeyAction::DELETE;
          return true;
     }

     if (S == "collections_list")
     {
          OutAction = APIKeyAction::COLLECTIONS_LIST;
          return true;
     }

     if (S == "collections_create")
     {
          OutAction = APIKeyAction::COLLECTIONS_CREATE;
          return true;
     }

     if (S == "collections_delete")
     {
          OutAction = APIKeyAction::COLLECTIONS_DELETE;
          return true;
     }

     if (S == "import")
     {
          OutAction = APIKeyAction::IMPORT;
          return true;
     }

     if (S == "*" || S == "all")
     {
          OutAction = APIKeyAction::ALL;
          return true;
     }

     return false;
}

static APIKeyAction ParseActionOrThrow(const std::string &S)
{
     APIKeyAction Action;

     if (!StringToAction(S, Action))
     {
          throw std::invalid_argument("Invalid API key action: " + S);
     }

     return Action;
}

static int ParseRateLimitOrThrow(const json &Body)
{
     int RateLimit = Body["rate_limit_per_minute"];

     if (RateLimit <= 0)
     {
          throw std::invalid_argument("rate_limit_per_minute must be greater than 0");
     }

     return RateLimit;
}

/* HandleListKeys lists all API keys. */

HttpResponse SearchAPI::HandleListKeys(const HttpRequest &Request)
{
     auto Keys = APIKeyManager::Instance().ListKeys();

     json Response;
     Response["keys"] = json::array();

     for (const auto &Key : Keys)
     {
          json KeyJSON;
          KeyJSON["id"] = Key.ID;
          KeyJSON["description"] = Key.Description;

          json Scopes = json::object();

          for (const auto &ScopePair : Key.Scopes)
          {
               json ScopeJSON;
               json Actions = json::array();

               for (auto Action : ScopePair.second.Actions)
               {
                    Actions.push_back(ActionToString(Action));
               }

               ScopeJSON["actions"] = Actions;
               ScopeJSON["embedded_filters"] = ScopePair.second.EmbeddedFilters;
               Scopes[ScopePair.first] = ScopeJSON;
          }

          KeyJSON["scopes"] = Scopes;
          KeyJSON["rate_limit_per_minute"] = Key.RateLimitPerMinute;
          KeyJSON["allow_hanalyzer"] = Key.AllowHanalyzer;
          KeyJSON["use_count"] = Key.UseCount;
          KeyJSON["last_used_at"] = std::chrono::system_clock::to_time_t(Key.LastUsedAt);
          KeyJSON["created_at"] = std::chrono::system_clock::to_time_t(Key.CreatedAt);

          Response["keys"].push_back(KeyJSON);
     }

     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = Response.dump();
     return Resp;
}

/* HandleCreateKey creates a new API key. */

HttpResponse SearchAPI::HandleCreateKey(const HttpRequest &Request)
{
     try
     {
          json Body = json::parse(Request.Body);

          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreCreateKey, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

               if (PreCheck.Action == ModulePreCheckAction::Deny)
               {
                    return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
               }
          }

          APIKey KeySpec;

          if (Body.contains("description"))
          {
               KeySpec.Description = Body["description"];
          }

          if (Body.contains("rate_limit_per_minute"))
          {
               KeySpec.RateLimitPerMinute = ParseRateLimitOrThrow(Body);
          }

          if (Body.contains("allow_hanalyzer"))
          {
               KeySpec.AllowHanalyzer = Body["allow_hanalyzer"];
          }

          if (Body.contains("scopes"))
          {
               for (auto &[ColName, ScopeJSON] : Body["scopes"].items())
               {
                    CollectionScope Scope;

                    if (ScopeJSON.contains("actions"))
                    {
                         for (const auto &ActionStr : ScopeJSON["actions"])
                         {
                              Scope.Actions.insert(ParseActionOrThrow(ActionStr.get<std::string>()));
                         }
                    }

                    if (ScopeJSON.contains("embedded_filters"))
                    {
                         Scope.EmbeddedFilters = ScopeJSON["embedded_filters"];
                    }

                    KeySpec.Scopes[ColName] = Scope;
               }
          }
          else if (Body.contains("collections"))
          {
               /* Backward compatibility: map collections/actions/filters to scopes. */

               std::vector<std::string> Cols = Body["collections"].get<std::vector<std::string>>();
               std::unordered_set<APIKeyAction> Actions;
               std::string Filters;

               if (Body.contains("embedded_filters"))
               {
                    Filters = Body["embedded_filters"];
               }

               if (Body.contains("actions"))
               {
                    for (const auto &ActionStr : Body["actions"])
                    {
                         Actions.insert(ParseActionOrThrow(ActionStr.get<std::string>()));
                    }
               }

               for (const auto &Col : Cols)
               {
                    KeySpec.Scopes[Col] = {Actions, Filters};
               }
          }

          std::string RawKey = APIKeyManager::Instance().CreateKey(KeySpec);

          if (RawKey.empty())
          {
               HttpResponse Resp(500, "Internal Server Error", "application/json");
               Resp.Body = GenerateErrorResponse("Failed to persist API key");
               return Resp;
          }

          json Response;
          Response["key"] = RawKey;
          Response["id"] = KeySpec.ID;

          std::string Dump = Response.dump();

          if (Instance && Instance->Logs)
          {
          }

          HttpResponse Resp(201, "Created", "application/json");
          Resp.Body = Dump;
          return Resp;
     }
     catch (const std::exception &E)
     {
          HttpResponse Resp(400, "Bad Request", "application/json");
          Resp.Body = GenerateErrorResponse(E.what());
          return Resp;
     }
}

/* HandleGetKey retrieves an API key by ID. */

HttpResponse SearchAPI::HandleGetKey(const HttpRequest &Request)
{
     std::string KeyID = ExtractKeyIDFromPath(Request.Path);

     APIKey Key;

     if (!APIKeyManager::Instance().GetKey(KeyID, &Key))
     {
          return HttpResponse(404, "Not Found", GenerateErrorResponse("Key not found"));
     }

     json KeyJSON;
     KeyJSON["id"] = Key.ID;
     KeyJSON["description"] = Key.Description;

     json Scopes = json::object();

     for (const auto &ScopePair : Key.Scopes)
     {
          json ScopeJSON;
          json Actions = json::array();

          for (auto Action : ScopePair.second.Actions)
          {
               Actions.push_back(ActionToString(Action));
          }

          ScopeJSON["actions"] = Actions;
          ScopeJSON["embedded_filters"] = ScopePair.second.EmbeddedFilters;
          Scopes[ScopePair.first] = ScopeJSON;
     }

     KeyJSON["scopes"] = Scopes;
     KeyJSON["rate_limit_per_minute"] = Key.RateLimitPerMinute;
     KeyJSON["allow_hanalyzer"] = Key.AllowHanalyzer;
     KeyJSON["use_count"] = Key.UseCount;
     KeyJSON["last_used_at"] = std::chrono::system_clock::to_time_t(Key.LastUsedAt);
     KeyJSON["created_at"] = std::chrono::system_clock::to_time_t(Key.CreatedAt);

     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = KeyJSON.dump();
     return Resp;
}

/* HandleDeleteKey deletes an API key. */

HttpResponse SearchAPI::HandleDeleteKey(const HttpRequest &Request)
{
     std::string KeyID = ExtractKeyIDFromPath(Request.Path);

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteKey, KeyID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     APIKey Existing;

     if (!APIKeyManager::Instance().GetKey(KeyID, &Existing))
     {
          HttpResponse Resp(404, "Not Found", "application/json");
          Resp.Body = GenerateErrorResponse("Key not found");
          return Resp;
     }

     if (APIKeyManager::Instance().DeleteKey(KeyID))
     {
          json Response;
          Response["id"] = KeyID;
          Response["deleted"] = true;

          HttpResponse Resp(200, "OK", "application/json");
          Resp.Body = Response.dump();
          return Resp;
     }

     HttpResponse Resp(500, "Internal Server Error", "application/json");
     Resp.Body = GenerateErrorResponse("Failed to delete key");
     return Resp;
}

/* HandleUpdateKey updates an API key. */

HttpResponse SearchAPI::HandleUpdateKey(const HttpRequest &Request)
{
     std::string KeyID = ExtractKeyIDFromPath(Request.Path);

     try
     {
          json Body = json::parse(Request.Body);

          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateKey, KeyID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

               if (PreCheck.Action == ModulePreCheckAction::Deny)
               {
                    return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
               }
          }

          APIKey Existing;

          if (!APIKeyManager::Instance().GetKey(KeyID, &Existing))
          {
               return HttpResponse(404, "Not Found", GenerateErrorResponse("Key not found"));
          }

          APIKey KeySpec = Existing;

          if (Body.contains("description"))
          {
               KeySpec.Description = Body["description"];
          }

          if (Body.contains("rate_limit_per_minute"))
          {
               KeySpec.RateLimitPerMinute = ParseRateLimitOrThrow(Body);
          }

          if (Body.contains("allow_hanalyzer"))
          {
               KeySpec.AllowHanalyzer = Body["allow_hanalyzer"];
          }

          if (Body.contains("scopes"))
          {
               KeySpec.Scopes.clear();

               for (auto &[ColName, ScopeJSON] : Body["scopes"].items())
               {
                    CollectionScope Scope;

                    if (ScopeJSON.contains("actions"))
                    {
                         for (const auto &ActionStr : ScopeJSON["actions"])
                         {
                              Scope.Actions.insert(ParseActionOrThrow(ActionStr.get<std::string>()));
                         }
                    }

                    if (ScopeJSON.contains("embedded_filters"))
                    {
                         Scope.EmbeddedFilters = ScopeJSON["embedded_filters"];
                    }

                    KeySpec.Scopes[ColName] = Scope;
               }
          }
          else if (Body.contains("collections") || Body.contains("actions") || Body.contains("embedded_filters") ||
                   Body.contains("add_collections") || Body.contains("remove_collections"))
          {
               std::unordered_set<APIKeyAction> Actions;
               std::string Filters;
               bool actions_provided = false;
               bool filters_provided = false;

               if (Body.contains("actions"))
               {
                    for (const auto &ActionStr : Body["actions"])
                    {
                         Actions.insert(ParseActionOrThrow(ActionStr.get<std::string>()));
                    }
                    actions_provided = true;
               }

               if (Body.contains("embedded_filters"))
               {
                    Filters = Body["embedded_filters"];
                    filters_provided = true;
               }

               if (Body.contains("collections"))
               {
                    KeySpec.Scopes.clear();
                    std::vector<std::string> Cols = Body["collections"].get<std::vector<std::string>>();

                    if (!actions_provided && !KeySpec.Scopes.empty())
                    {
                         Actions = KeySpec.Scopes.begin()->second.Actions;
                    }

                    if (!filters_provided && !KeySpec.Scopes.empty())
                    {
                         Filters = KeySpec.Scopes.begin()->second.EmbeddedFilters;
                    }

                    for (const auto &Col : Cols)
                    {
                         KeySpec.Scopes[Col] = {Actions, Filters};
                    }
               }

               if (Body.contains("add_collections"))
               {
                    std::vector<std::string> ToAdd = Body["add_collections"].get<std::vector<std::string>>();

                    if (!actions_provided && !KeySpec.Scopes.empty())
                    {
                         Actions = KeySpec.Scopes.begin()->second.Actions;
                    }

                    if (Actions.empty())
                    {
                         Actions = {APIKeyAction::SEARCH};
                    }

                    if (!filters_provided && !KeySpec.Scopes.empty())
                    {
                         Filters = KeySpec.Scopes.begin()->second.EmbeddedFilters;
                    }

                    for (const auto &Col : ToAdd)
                    {
                         KeySpec.Scopes[Col] = {Actions, Filters};
                    }
               }

               if (Body.contains("remove_collections"))
               {
                    std::vector<std::string> ToRemove = Body["remove_collections"].get<std::vector<std::string>>();
                    for (const auto &Col : ToRemove)
                    {
                         KeySpec.Scopes.erase(Col);
                    }
               }

               /* If actions or filters were provided but not collections, apply to all existing scopes. */

               if ((actions_provided || filters_provided) && !Body.contains("collections") && !Body.contains("add_collections"))
               {
                    for (auto &[Col, Scope] : KeySpec.Scopes)
                    {
                         if (actions_provided)
                         {
                              Scope.Actions = Actions;
                         }

                         if (filters_provided)
                         {
                              Scope.EmbeddedFilters = Filters;
                         }
                    }
               }
          }

          if (APIKeyManager::Instance().UpdateKey(KeyID, KeySpec))
          {
               json Response;
               Response["id"] = KeyID;
               Response["updated"] = true;

               HttpResponse Resp(200, "OK", "application/json");
               Resp.Body = Response.dump();
               return Resp;
          }

          HttpResponse Resp(500, "Internal Server Error", "application/json");
          Resp.Body = GenerateErrorResponse("Failed to update key");
          return Resp;
     }
     catch (const std::exception &E)
     {
          HttpResponse Resp(400, "Bad Request", "application/json");
          Resp.Body = GenerateErrorResponse(E.what());
          return Resp;
     }
}
