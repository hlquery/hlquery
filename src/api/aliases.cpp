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
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pthread.h>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/common.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides alias API handlers for collection alias lookup and mutation. */

std::string ExtractAliasCollectionFromPath(const std::string &Path)
{
     std::regex AliasCollectionRegex(R"(/collections/([^/]+)/aliases)");
     std::smatch Match;

     if (std::regex_search(Path, Match, AliasCollectionRegex))
     {
          return Match[1].str();
     }

     return "";
}

/* Applies alias pre check processing. */

HttpResponse ApplyAliasPreCheck(const ModulePreCheckResult &PreCheck)
{
     if (PreCheck.Action == ModulePreCheckAction::Deny)
     {
          return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
     }

     return HttpResponse(Status::OK, StatusText(Status::OK), "application/json");
}

/* Checks whether alias pre check failure exists. */

bool HasAliasPreCheckFailure(const HttpResponse &Response)
{
     return Response.StatusCode != Status::OK;
}

/* HandleListAliases lists all collection aliases. */

/*
 * SearchAPI::HandleListAliases implementation.
 */

HttpResponse SearchAPI::HandleListAliases(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string FilterCollection;
     auto FilterIt = Request.QueryParams.find("collection");

     if (FilterIt != Request.QueryParams.end())
     {
          FilterCollection = FilterIt->second;
     }
     else
     {
          FilterCollection = ExtractAliasCollectionFromPath(Request.Path);
     }

     nlohmann::json AliasesArray = nlohmann::json::array();

     if (Instance && Instance->Database)
     {
          std::vector<std::string> AliasKeys = Instance->Database->Keys("alias:*");

          for (const auto &Key : AliasKeys)
          {
               std::string AliasJSON = Instance->Database->Get(Key);

               if (!AliasJSON.empty())
               {
                    try
                    {
                         nlohmann::json AliasData = nlohmann::json::parse(AliasJSON);

                         if (!FilterCollection.empty())
                         {
                              std::string AliasCollection;

                              if (AliasData.contains("collection_name") && AliasData["collection_name"].is_string())
                              {
                                   AliasCollection = AliasData["collection_name"].get<std::string>();
                              }
                              else if (AliasData.contains("collection") && AliasData["collection"].is_string())
                              {
                                   AliasCollection = AliasData["collection"].get<std::string>();
                              }

                              if (AliasCollection != FilterCollection)
                              {
                                   continue;
                              }
                         }

                         AliasesArray.push_back(AliasData);
                    }
                    catch (...)
                    {
                         /* Skip invalid JSON. */
                    }
               }
          }
     }

     nlohmann::json ResultJSON;

     ResultJSON["aliases"] = AliasesArray;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = ResultJSON.dump();

     return Response;
}

/* HandleCreateOrUpdateAlias creates or updates a collection alias. */

/*
 * SearchAPI::HandleCreateOrUpdateAlias implementation.
 */

HttpResponse SearchAPI::HandleCreateOrUpdateAlias(const HttpRequest &Request)
{
     if (Request.Method != "POST" && Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string AliasName = ExtractAliasNameFromPath(Request.Path);

     if (AliasName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          HttpResponse PreCheckResponse = ApplyAliasPreCheck(
               RUN_MODULE_PRECHECK(OnPreUpsertAlias, AliasName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty()));

          if (HasAliasPreCheckFailure(PreCheckResponse))
          {
               return PreCheckResponse;
          }
     }

     /* Parse alias data from request body. */

     try
     {
          nlohmann::json AliasData = nlohmann::json::parse(Request.Body);
          std::string CollectionName;

          if (AliasData.contains("collection_name") && AliasData["collection_name"].is_string())
          {
               CollectionName = AliasData["collection_name"].get<std::string>();
          }
          else if (AliasData.contains("collection") && AliasData["collection"].is_string())
          {
               CollectionName = AliasData["collection"].get<std::string>();
          }
          else
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          /* Validate collection exists. */

          std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

          if (It == CollectionsList.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }

          /* Preserve existing created_at on update. */

          std::string AliasKey = "alias:" + AliasName;
          std::string CreatedAt = GetCurrentTimestamp();

          if (Instance && Instance->Database)
          {
               std::string ExistingAliasJSON = Instance->Database->Get(AliasKey);

               if (!ExistingAliasJSON.empty())
               {
                    try
                    {
                         nlohmann::json ExistingAlias = nlohmann::json::parse(ExistingAliasJSON);

                         if (ExistingAlias.contains("created_at") && ExistingAlias["created_at"].is_string())
                         {
                              CreatedAt = ExistingAlias["created_at"].get<std::string>();
                         }
                    }
                    catch (...)
                    {
                         /* Ignore parse error, keep new created_at. */
                    }
               }
          }

          /* Build alias JSON. */

          nlohmann::json AliasJSON;

          AliasJSON["name"] = AliasName;
          AliasJSON["collection_name"] = CollectionName;
          AliasJSON["collection"] = CollectionName;
          AliasJSON["created_at"] = CreatedAt;
          AliasJSON["updated_at"] = GetCurrentTimestamp();

          /* Save to LSM storage. */

          if (!Instance || !Instance->Database || !Instance->Database->Set(AliasKey, AliasJSON.dump()))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Alias created/updated\",\"name\":\"" + EscapeJSONString(AliasName) + "\",\"collection_name\":\"" + EscapeJSONString(CollectionName) + "\"}";
          FOREACH_MOD(OnUpsertAlias, AliasName, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          return Response;
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
}

/* HandleGetAlias gets an alias. */

/*
 * SearchAPI::HandleGetAlias implementation.
 */

HttpResponse SearchAPI::HandleGetAlias(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string AliasName = ExtractAliasNameFromPath(Request.Path);

     if (AliasName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     std::string AliasKey = "alias:" + AliasName;
     std::string AliasJSON = HybridStorageManagerInstance().Get(AliasKey);

     if (AliasJSON.empty())
     {
          return HttpResponse(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = AliasJSON;

     return Response;
}

/* HandleDeleteAlias deletes an alias. */

/*
 * SearchAPI::HandleDeleteAlias implementation.
 */

HttpResponse SearchAPI::HandleDeleteAlias(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string AliasName = ExtractAliasNameFromPath(Request.Path);

     if (AliasName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          HttpResponse PreCheckResponse = ApplyAliasPreCheck(
               RUN_MODULE_PRECHECK(OnPreDeleteAlias, AliasName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty()));

          if (HasAliasPreCheckFailure(PreCheckResponse))
          {
               return PreCheckResponse;
          }
     }

     std::string AliasKey = "alias:" + AliasName;
     std::string AliasJSON = HybridStorageManagerInstance().Get(AliasKey);

     if (AliasJSON.empty())
     {
          return HttpResponse(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");
     }

     if (!Instance || !Instance->Database)
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }

     if (Instance->Database->Del(AliasKey) == 0)
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Alias deleted\",\"name\":\"" + EscapeJSONString(AliasName) + "\"}";
     FOREACH_MOD(OnDeleteAlias, AliasName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}
