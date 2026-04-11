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
#include "core/threadlimit.h"
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

namespace
{
static const char *kGlobalSynonymsCollection = "__global__";

static bool IsGlobalSynonymsPath(const std::string &Path)
{
     return Path == "/synonyms/global" || Path.find("/synonyms/global/") == 0;
}

static std::string ExtractGlobalSynonymId(const std::string &Path)
{
     std::regex GlobalSynonymRegex(R"(^/synonyms/global/([^/?]+))");
     std::smatch Match;

     if (std::regex_search(Path, Match, GlobalSynonymRegex))
     {
          return Match[1].str();
     }

     return "";
}

static bool ResolveSynonymScope(const std::string &Path,
                                const std::string &ExtractedCollection,
                                std::string *OutCollection,
                                bool *OutIsGlobal)
{
     if (OutCollection == nullptr || OutIsGlobal == nullptr)
     {
          return false;
     }

     if (!ExtractedCollection.empty())
     {
          *OutCollection = ExtractedCollection;
          *OutIsGlobal = false;
          return true;
     }

     if (IsGlobalSynonymsPath(Path))
     {
          *OutCollection = kGlobalSynonymsCollection;
          *OutIsGlobal = true;
          return true;
     }

     *OutCollection = "";
     *OutIsGlobal = false;
     return false;
}

static HttpResponse ApplySynonymPreCheck(const ModulePreCheckResult &PreCheck)
{
     if (PreCheck.Action == ModulePreCheckAction::Deny)
     {
          return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
     }

     return HttpResponse(0, "", "");
}

static bool HasModulePreCheckFailure(const HttpResponse &Response)
{
     return Response.StatusCode != 0;
}

static std::string NormalizeSynonymTerm(const std::string &Value)
{
     std::string Result;
     Result.reserve(Value.size());

     for (unsigned char ch : Value)
     {
          Result.push_back(static_cast<char>(std::tolower(ch)));
     }

     return Result;
}
}
/* List all synonyms for a collection. */

/*
 * SearchAPI::HandleListSynonyms implementation.
 */

HttpResponse SearchAPI::HandleListSynonyms(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     if (!ResolveSynonymScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(Collections.begin(), Collections.end(), CollectionName);
          if (It == Collections.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Get synonyms from LSM storage. */

     std::string SynonymsKey = "synonyms:" + CollectionName;

     std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     nlohmann::json Result;

     Result["collection"] = CollectionName;
     Result["scope"] = IsGlobalScope ? "global" : "collection";
     Result["count"] = 0;

     nlohmann::json SynonymsArray = nlohmann::json::array();

     if (!SynonymsJSON.empty())
     {
          try
          {
               nlohmann::json RootObj = nlohmann::json::parse(SynonymsJSON);

               nlohmann::json *SynonymsPtr = nullptr;
               nlohmann::json SynonymsArrayTemp;

               if (RootObj.is_object() && RootObj.contains("synonyms"))
               {
                    SynonymsPtr = &RootObj["synonyms"];
               }
               else if (RootObj.is_array())
               {
                    SynonymsArrayTemp = RootObj;
                    SynonymsPtr = &SynonymsArrayTemp;
               }

               if (SynonymsPtr && SynonymsPtr->is_array())
               {
                    SynonymsArray = *SynonymsPtr;
                    Result["count"] = SynonymsArray.size();
               }
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("search_api", "HandleListSynonyms: Failed to parse synonyms JSON: " + std::string(e.what()) + ".");
               }
          }
     }

     Result["synonyms"] = SynonymsArray;

     Response.Body = Result.dump(2);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleListSynonyms: Returning " + std::to_string(Result["count"].get<int>()) + " synonym groups for collection '" + CollectionName + "'.");
     }

     return Response;
}

/* List synonyms for all collections. */

/*
 * SearchAPI::HandleListAllSynonyms implementation.
 */

HttpResponse SearchAPI::HandleListAllSynonyms(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     nlohmann::json CollectionsJSON = nlohmann::json::array();

     auto Collections = HybridStorageManagerInstance().ListCollections();

     for (const auto &CollectionName : Collections)
     {
          std::string SynonymsKey = "synonyms:" + CollectionName;

          std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

          nlohmann::json Entry;

          Entry["collection"] = CollectionName;
          Entry["synonyms"] = nlohmann::json::array();

          if (!SynonymsJSON.empty())
          {
               try
               {
                    nlohmann::json Parsed = nlohmann::json::parse(SynonymsJSON);

                    if (Parsed.is_object() && Parsed.contains("synonyms") && Parsed["synonyms"].is_array())
                    {
                         Entry["synonyms"] = Parsed["synonyms"];
                    }
                    else if (Parsed.is_array())
                    {
                         Entry["synonyms"] = Parsed;
                    }
               }
               catch (const std::exception &)
               {
               }
          }

          CollectionsJSON.push_back(Entry);
     }

     {
          std::string SynonymsKey = std::string("synonyms:") + kGlobalSynonymsCollection;
          std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

          nlohmann::json Entry;
          Entry["collection"] = kGlobalSynonymsCollection;
          Entry["scope"] = "global";
          Entry["synonyms"] = nlohmann::json::array();

          if (!SynonymsJSON.empty())
          {
               try
               {
                    nlohmann::json Parsed = nlohmann::json::parse(SynonymsJSON);
                    if (Parsed.is_object() && Parsed.contains("synonyms") && Parsed["synonyms"].is_array())
                    {
                         Entry["synonyms"] = Parsed["synonyms"];
                    }
                    else if (Parsed.is_array())
                    {
                         Entry["synonyms"] = Parsed;
                    }
               }
               catch (const std::exception &)
               {
               }
          }

          CollectionsJSON.push_back(Entry);
     }

     nlohmann::json Result;

     Result["collections"] = CollectionsJSON;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = Result.dump();

     return Response;
}

/* Create or update a synonym group. */

/*
 * SearchAPI::HandleCreateOrUpdateSynonym implementation.
 */

HttpResponse SearchAPI::HandleCreateOrUpdateSynonym(const HttpRequest &Request)
{
     if (Request.Method != "POST" && Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     if (!ResolveSynonymScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     std::string SynonymID = ExtractSynonymIdFromPath(Request.Path);
     if (SynonymID.empty() && IsGlobalScope)
     {
          SynonymID = ExtractGlobalSynonymId(Request.Path);
     }

     if (CollectionName.empty() || SynonymID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(Collections.begin(), Collections.end(), CollectionName);
          if (It == Collections.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Parse synonym data from request body. */

     try
     {
          nlohmann::json SynonymData = nlohmann::json::parse(Request.Body);

          if (!SynonymData.contains("root") || !SynonymData.contains("synonyms"))
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          if (!SynonymData["root"].is_string())
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          if (!SynonymData["synonyms"].is_array())
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          const std::string RootTerm = SynonymData["root"].get<std::string>();
          const std::string NormalizedRoot = NormalizeSynonymTerm(RootTerm);
          std::unordered_set<std::string> SeenSynonyms;

          for (const auto &Syn : SynonymData["synonyms"])
          {
               if (!Syn.is_string())
               {
                    return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               }

               const std::string SynonymTerm = Syn.get<std::string>();
               const std::string NormalizedSynonym = NormalizeSynonymTerm(SynonymTerm);

               if (!NormalizedRoot.empty() && NormalizedSynonym == NormalizedRoot)
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
                    Response.Body = "{\"error\":\"Invalid synonym group\",\"message\":\"Root term cannot also appear in synonyms.\"}";
                    return Response;
               }

               SeenSynonyms.insert(NormalizedSynonym);
          }

          /* Get existing synonyms for this collection. */

          std::string SynonymsKey = "synonyms:" + CollectionName;

          std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

          nlohmann::json RootObj;

          if (!SynonymsJSON.empty())
          {
               try
               {
                    nlohmann::json Parsed = nlohmann::json::parse(SynonymsJSON);

                    if (Parsed.is_array())
                    {
                         RootObj["synonyms"] = Parsed;
                    }
                    else if (Parsed.is_object())
                    {
                         RootObj = Parsed;
                    }
                    else
                    {
                         RootObj["synonyms"] = nlohmann::json::array();
                    }
               }
               catch (const std::exception &)
               {
                    RootObj["synonyms"] = nlohmann::json::array();
               }
          }
          else
          {
               RootObj["synonyms"] = nlohmann::json::array();
          }

          if (!RootObj.contains("synonyms"))
          {
               RootObj["synonyms"] = nlohmann::json::array();
          }

          nlohmann::json &SynonymsArray = RootObj["synonyms"];

          bool FoundVal = false;

          for (auto &Syn : SynonymsArray)
          {
               if (Syn.contains("id") && Syn["id"] == SynonymID)
               {
                    Syn["root"] = SynonymData["root"];
                    Syn["synonyms"] = SynonymData["synonyms"];
                    Syn["updated_at"] = GetCurrentTimestamp();

                    FoundVal = true;
                    break;
               }
          }

          if (!FoundVal)
          {
               nlohmann::json NewSynonym;

               NewSynonym["id"] = SynonymID;
               NewSynonym["root"] = SynonymData["root"];
               NewSynonym["synonyms"] = SynonymData["synonyms"];
               NewSynonym["created_at"] = GetCurrentTimestamp();
               NewSynonym["updated_at"] = GetCurrentTimestamp();

               SynonymsArray.push_back(NewSynonym);
          }

          if (Instance && Instance->Modules)
          {
               HttpResponse PreCheckResponse = ApplySynonymPreCheck(
                    RUN_MODULE_PRECHECK(OnPreUpsertSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty()));

               if (HasModulePreCheckFailure(PreCheckResponse))
               {
                    return PreCheckResponse;
               }
          }

          std::string UpdatedJSON = RootObj.dump();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "Saving synonym to LSM - key: " + SynonymsKey + ", data: " + UpdatedJSON.substr(0, 200) + ".");
          }

          if (!Instance || !Instance->Database || !Instance->Database->Set(SynonymsKey, UpdatedJSON))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Synonym created/updated\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\",\"scope\":\"" + (IsGlobalScope ? "global" : "collection") + "\"}";
          NOTIFY_MODULES(OnUpsertSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          return Response;
     }
     catch (const nlohmann::json::parse_error &e)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(e.what()) + "\"}";

          return Response;
     }
     catch (const nlohmann::json::type_error &e)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid data type\",\"message\":\"" + EscapeJSONString(e.what()) + "\"}";

          return Response;
     }
     catch (const std::exception &e)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"" + EscapeJSONString(e.what()) + "\"}";

          return Response;
     }
}

/* Get a single synonym group. */

/*
 * SearchAPI::HandleGetSynonym implementation.
 */

HttpResponse SearchAPI::HandleGetSynonym(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;

     if (!ResolveSynonymScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     std::string SynonymID = ExtractSynonymIdFromPath(Request.Path);

     if (SynonymID.empty() && IsGlobalScope)
     {
          SynonymID = ExtractGlobalSynonymId(Request.Path);
     }

     if (CollectionName.empty() || SynonymID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(Collections.begin(), Collections.end(), CollectionName);

          if (It == Collections.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Get synonyms from LSM storage. */

     std::string SynonymsKey = "synonyms:" + CollectionName;

     std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

     if (SynonymsJSON.empty())
     {
          HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

          Response.Body = "{\"error\":\"Synonym not found\",\"message\":\"The specified synonym does not exist in this collection\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" +
                          EscapeJSONString(CollectionName) + "\"}";

          return Response;
     }

     try
     {
          if (Instance && Instance->Modules)
          {
               HttpResponse PreCheckResponse = ApplySynonymPreCheck(
                    RUN_MODULE_PRECHECK(OnPreDeleteSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty()));

               if (HasModulePreCheckFailure(PreCheckResponse))
               {
                    return PreCheckResponse;
               }
          }

          nlohmann::json RootObj = nlohmann::json::parse(SynonymsJSON);

          nlohmann::json *SynonymsPtr = nullptr;
          nlohmann::json SynonymsArrayVal;

          if (RootObj.is_object() && RootObj.contains("synonyms"))
          {
               SynonymsPtr = &RootObj["synonyms"];
          }
          else if (RootObj.is_array())
          {
               SynonymsArrayVal = RootObj;
               SynonymsPtr = &SynonymsArrayVal;
          }
          else
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          for (const auto &Syn : *SynonymsPtr)
          {
               if (Syn.contains("id") && Syn["id"] == SynonymID)
               {
                    HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

                    Response.Body = Syn.dump();

                    return Response;
               }
          }

          HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

          Response.Body = "{\"error\":\"Synonym not found\",\"message\":\"The specified synonym does not exist in this collection\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" +
                          EscapeJSONString(CollectionName) + "\"}";

          return Response;
     }
     catch (const nlohmann::json::parse_error &e)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse synonyms JSON: " + EscapeJSONString(e.what()) + "\"}";

          return Response;
     }
     catch (const std::exception &e)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"" + EscapeJSONString(e.what()) + "\"}";

          return Response;
     }
}

/* Delete a synonym group. */

/*
 * SearchAPI::HandleDeleteSynonym implementation.
 */

HttpResponse SearchAPI::HandleDeleteSynonym(const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleDeleteSynonym CALLED - method: " + Request.Method + ", path: " + Request.Path + ".");
     }

     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     ResolveSynonymScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope);
     std::string SynonymID = ExtractSynonymIdFromPath(Request.Path);
     if (SynonymID.empty() && IsGlobalScope)
     {
          SynonymID = ExtractGlobalSynonymId(Request.Path);
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Extracted - collection: '" + CollectionName + "', synonym_id: '" + SynonymID + "'.");
     }

     if (CollectionName.empty() || SynonymID.empty())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "DELETE synonym FAILED - empty collection or id.");
          }

          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> Collections = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(Collections.begin(), Collections.end(), CollectionName);
          if (It == Collections.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Get synonyms from LSM storage. */

     std::string SynonymsKey = "synonyms:" + CollectionName;

     std::string SynonymsJSON = HybridStorageManagerInstance().Get(SynonymsKey);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Retrieved - key: " + SynonymsKey + ", empty: " + (SynonymsJSON.empty() ? "YES" : "NO") + ", size: " + std::to_string(SynonymsJSON.size()) + ".");
     }

     if (SynonymsJSON.empty())
     {
          HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

          Response.Body = "{\"error\":\"Synonym not found\",\"message\":\"The specified synonym does not exist in this collection\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" +
                          EscapeJSONString(CollectionName) + "\"}";

          return Response;
     }

     try
     {
          nlohmann::json RootObj = nlohmann::json::parse(SynonymsJSON);

          if (!RootObj.contains("synonyms") || !RootObj["synonyms"].is_array())
          {
               HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

               Response.Body = "{\"error\":\"Invalid data format\",\"message\":\"Invalid synonyms data format\"}";

               return Response;
          }

          nlohmann::json &SynonymsArray = RootObj["synonyms"];

          bool FoundVal = false;
          std::string RootTerm;

          for (auto SynIt = SynonymsArray.begin(); SynIt != SynonymsArray.end(); ++SynIt)
          {
               bool Matches = false;

               if (SynIt->contains("id") && (*SynIt)["id"] == SynonymID)
               {
                    Matches = true;
               }
               else if (SynIt->contains("root") && (*SynIt)["root"] == SynonymID)
               {
                    Matches = true;
               }

               if (Matches)
               {
                    if (SynIt->contains("root"))
                    {
                         RootTerm = (*SynIt)["root"].get<std::string>();
                    }
                    else if (SynIt->contains("id"))
                    {
                         RootTerm = (*SynIt)["id"].get<std::string>();
                    }

                    SynonymsArray.erase(SynIt);

                    FoundVal = true;
                    break;
               }
          }

          if (!FoundVal)
          {
               HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

               Response.Body = "{\"error\":\"Synonym not found\",\"message\":\"The specified synonym does not exist in this collection\",\"id\":\"" + EscapeJSONString(SynonymID) +
                               "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

               return Response;
          }

          std::string UpdatedJSON = RootObj.dump();

          if (!Instance || !Instance->Database)
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          if (!Instance->Database->Set(SynonymsKey, UpdatedJSON))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Synonym deleted\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\",\"scope\":\"" + (IsGlobalScope ? "global" : "collection") + "\"}";
          NOTIFY_MODULES(OnDeleteSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          return Response;
     }
     catch (const std::exception &)
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }
}

HttpResponse SearchAPI::HandleListGlobalSynonyms(const HttpRequest &Request)
{
     return HandleListSynonyms(Request);
}

HttpResponse SearchAPI::HandleCreateOrUpdateGlobalSynonym(const HttpRequest &Request)
{
     return HandleCreateOrUpdateSynonym(Request);
}

HttpResponse SearchAPI::HandleGetGlobalSynonym(const HttpRequest &Request)
{
     return HandleGetSynonym(Request);
}

HttpResponse SearchAPI::HandleDeleteGlobalSynonym(const HttpRequest &Request)
{
     return HandleDeleteSynonym(Request);
}
