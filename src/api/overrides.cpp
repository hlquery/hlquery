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
#include "search/hybrid_rank_fusion.h"
#include "search/document_collection_store.h"
#include "search/lexical_inverted_index.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides override API handlers for search result adjustments. */

/* HandleListOverrides lists all query overrides for a collection. */

/*
 * SearchAPI::HandleListOverrides implementation.
 */

HttpResponse SearchAPI::HandleListOverrides(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     /* For now, return empty overrides - actual override logic would go here. */

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"overrides\":[],\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

     return Response;
}

/* HandleCreateOrUpdateOverride creates or updates a query override. */

/*
 * SearchAPI::HandleCreateOrUpdateOverride implementation.
 */

HttpResponse SearchAPI::HandleCreateOrUpdateOverride(const HttpRequest &Request)
{
     if (Request.Method != "POST" && Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string OverrideID = ExtractOverrideIdFromPath(Request.Path);

     if (CollectionName.empty() || OverrideID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     /* Parse override data from request body. */

     try
     {
          nlohmann::json OverrideData = nlohmann::json::parse(Request.Body);

          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpsertOverride, CollectionName, OverrideID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

               if (PreCheck.Action == ModulePreCheckAction::Deny)
               {
                    return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
               }
          }

          /* Validate override data - must have at least rule, includes, excludes, filter_by, or sort_by. */

          bool HasValidField =
               OverrideData.contains("rule") || OverrideData.contains("includes") || OverrideData.contains("excludes") || OverrideData.contains("filter_by") || OverrideData.contains("sort_by");

          if (!HasValidField)
          {
               HttpResponse ErrorResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

               ErrorResponse.Body = "{\"error\":\"Invalid override\",\"message\":\"Override must contain at least one of: rule, includes, excludes, filter_by, sort_by\"}";

               return ErrorResponse;
          }

          /* Build override JSON. */

          nlohmann::json OverrideJSON;

          OverrideJSON["id"] = OverrideID;
          OverrideJSON["collection"] = CollectionName;
          OverrideJSON["created_at"] = GetCurrentTimestamp();
          OverrideJSON["updated_at"] = GetCurrentTimestamp();

          if (OverrideData.contains("rule"))
          {
               OverrideJSON["rule"] = OverrideData["rule"];
          }
          else
          {
               OverrideJSON["rule"] = nlohmann::json::object();
          }

          if (OverrideData.contains("includes"))
          {
               OverrideJSON["includes"] = OverrideData["includes"];
          }
          else
          {
               OverrideJSON["includes"] = nlohmann::json::array();
          }

          if (OverrideData.contains("excludes"))
          {
               OverrideJSON["excludes"] = OverrideData["excludes"];
          }
          else
          {
               OverrideJSON["excludes"] = nlohmann::json::array();
          }

          if (OverrideData.contains("filter_by"))
          {
               OverrideJSON["filter_by"] = OverrideData["filter_by"];
          }

          if (OverrideData.contains("sort_by"))
          {
               OverrideJSON["sort_by"] = OverrideData["sort_by"];
          }

          /* Save to LSM storage. */

          std::string OverrideKey = "override:" + CollectionName + ":" + OverrideID;

          /* HybridStorageManager doesn't have Put - use Database::Set instead. */

          if (!Instance->Database || !Instance->Database->Set(OverrideKey, OverrideJSON.dump()))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Override created/updated\",\"id\":\"" + EscapeJSONString(OverrideID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
          FOREACH_MOD(OnUpsertOverride, CollectionName, OverrideID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          return Response;
     }
     catch (const std::exception &)
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }
}

/* HandleGetOverride gets an override. */

/*
 * SearchAPI::HandleGetOverride implementation.
 */

HttpResponse SearchAPI::HandleGetOverride(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string OverrideID = ExtractOverrideIdFromPath(Request.Path);

     if (CollectionName.empty() || OverrideID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     /* Get override from LSM storage. */

     std::string OverrideKey = "override:" + CollectionName + ":" + OverrideID;
     std::string OverrideJSON = HybridStorageManagerInstance().Get(OverrideKey);

     if (OverrideJSON.empty())
     {
          return HttpResponse(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = OverrideJSON;

     return Response;
}

/* HandleDeleteOverride deletes an override. */

/*
 * SearchAPI::HandleDeleteOverride implementation.
 */

HttpResponse SearchAPI::HandleDeleteOverride(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string OverrideID = ExtractOverrideIdFromPath(Request.Path);

     if (CollectionName.empty() || OverrideID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     /* Delete override from LSM storage. */

     std::string OverrideKey = "override:" + CollectionName + ":" + OverrideID;
     std::string OverrideJSON = HybridStorageManagerInstance().Get(OverrideKey);

     if (OverrideJSON.empty())
     {
          return HttpResponse(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteOverride, CollectionName, OverrideID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     /* Delete from LSM using HybridStorageManager for consistency. */

     if (!HybridStorageManagerInstance().Delete(OverrideKey))
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Override deleted\",\"id\":\"" + EscapeJSONString(OverrideID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
     FOREACH_MOD(OnDeleteOverride, CollectionName, OverrideID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}
