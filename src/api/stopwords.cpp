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
static const char *kGlobalStopwordsCollection = "__global__";

static bool IsGlobalStopwordsPath(const std::string &Path)
{
     return Path == "/stopwords/global" || Path.find("/stopwords/global/") == 0;
}

static bool ResolveStopwordScope(const std::string &Path,
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

     if (IsGlobalStopwordsPath(Path))
     {
          *OutCollection = kGlobalStopwordsCollection;
          *OutIsGlobal = true;
          return true;
     }

     *OutCollection = "";
     *OutIsGlobal = false;
     return false;
}
}
/* HandleListAllStopwords lists stopwords for all collections. */

/*
 * SearchAPI::HandleListAllStopwords implementation.
 */

HttpResponse SearchAPI::HandleListAllStopwords(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     nlohmann::json CollectionsJSON = nlohmann::json::array();
     auto CollectionsList = HybridStorageManagerInstance().ListCollections();

     for (const auto &CollectionName : CollectionsList)
     {
          std::string StopwordsKey = "stopwords:" + CollectionName;
          std::string StopwordsJSON = HybridStorageManagerInstance().Get(StopwordsKey);

          nlohmann::json Entry;

          Entry["collection"] = CollectionName;
          Entry["stopwords"] = nlohmann::json::array();

          if (!StopwordsJSON.empty())
          {
               try
               {
                    nlohmann::json Parsed = nlohmann::json::parse(StopwordsJSON);

                    if (Parsed.is_object() && Parsed.contains("stopwords") && Parsed["stopwords"].is_array())
                    {
                         Entry["stopwords"] = Parsed["stopwords"];
                    }
                    else if (Parsed.is_array())
                    {
                         Entry["stopwords"] = Parsed;
                    }
               }
               catch (const std::exception &)
               {
               }
          }

          CollectionsJSON.push_back(Entry);
     }
     std::string GlobalKey = std::string("stopwords:") + kGlobalStopwordsCollection;
     std::string GlobalJSON = HybridStorageManagerInstance().Get(GlobalKey);

     nlohmann::json GlobalEntry;
     GlobalEntry["collection"] = kGlobalStopwordsCollection;
     GlobalEntry["stopwords"] = nlohmann::json::array();

     if (!GlobalJSON.empty())
     {
          try
          {
               nlohmann::json Parsed = nlohmann::json::parse(GlobalJSON);
               if (Parsed.is_object() && Parsed.contains("stopwords") && Parsed["stopwords"].is_array())
               {
                    GlobalEntry["stopwords"] = Parsed["stopwords"];
               }
               else if (Parsed.is_array())
               {
                    GlobalEntry["stopwords"] = Parsed;
               }
          }
          catch (const std::exception &)
          {
          }
     }

     CollectionsJSON.push_back(GlobalEntry);

     nlohmann::json Result;

     Result["collections"] = CollectionsJSON;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = Result.dump();

     return Response;
}

/* HandleListStopwords lists stopwords for a specific collection. */

/*
 * SearchAPI::HandleListStopwords implementation.
 */

HttpResponse SearchAPI::HandleListStopwords(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     if (!ResolveStopwordScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

          if (It == CollectionsList.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Get stopwords from LSM storage. */

     std::string StopwordsKey = "stopwords:" + CollectionName;
     std::string StopwordsJSON = HybridStorageManagerInstance().Get(StopwordsKey);

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json Result;

     Result["collection"] = CollectionName;
     Result["stopwords"] = nlohmann::json::array();

     if (!StopwordsJSON.empty())
     {
          try
          {
               nlohmann::json Parsed = nlohmann::json::parse(StopwordsJSON);

               if (Parsed.is_object())
               {
                    if (Parsed.contains("stopwords") && Parsed["stopwords"].is_array())
                    {
                         Result["stopwords"] = Parsed["stopwords"];
                    }
               }
               else if (Parsed.is_array())
               {
                    Result["stopwords"] = Parsed;
               }
          }
          catch (const std::exception &)
          {
          }
     }

     Response.Body = Result.dump();

     return Response;
}

/* HandleCreateStopword adds stopword(s) to a collection. */

/*
 * SearchAPI::HandleCreateStopword implementation.
 */

HttpResponse SearchAPI::HandleCreateStopword(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     if (!ResolveStopwordScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     /* Check if collection exists. */

     if (!IsGlobalScope)
     {
          std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

          if (It == CollectionsList.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     /* Parse stopword from request body. */

     try
     {
          nlohmann::json StopwordData = nlohmann::json::parse(Request.Body);
          std::vector<std::string> WordsToAdd;

          if (StopwordData.contains("words") && StopwordData["words"].is_array())
          {
               for (const auto &WordVal : StopwordData["words"])
               {
                    if (WordVal.is_string())
                    {
                         std::string WordStr = WordVal.get<std::string>();

                         if (!WordStr.empty())
                         {
                              WordsToAdd.push_back(WordStr);
                         }
                    }
               }
          }
          else if (StopwordData.contains("word") && StopwordData["word"].is_string())
          {
               std::string WordStr = StopwordData["word"].get<std::string>();

               if (!WordStr.empty())
               {
                    WordsToAdd.push_back(WordStr);
               }
               else
               {
                    return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               }
          }
          else
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          if (WordsToAdd.empty())
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }

          /* Get existing stopwords for this collection. */

          std::string StopwordsKey = "stopwords:" + CollectionName;
          std::string StopwordsJSON = HybridStorageManagerInstance().Get(StopwordsKey);

          nlohmann::json RootObj;

          if (!StopwordsJSON.empty())
          {
               try
               {
                    nlohmann::json Parsed = nlohmann::json::parse(StopwordsJSON);

                    if (Parsed.is_object())
                    {
                         RootObj = Parsed;
                    }
                    else if (Parsed.is_array())
                    {
                         RootObj["stopwords"] = Parsed;
                    }
                    else
                    {
                         RootObj["stopwords"] = nlohmann::json::array();
                    }
               }
               catch (const std::exception &)
               {
                    RootObj["stopwords"] = nlohmann::json::array();
               }
          }
          else
          {
               RootObj["stopwords"] = nlohmann::json::array();
          }

          if (!RootObj.contains("stopwords") || !RootObj["stopwords"].is_array())
          {
               RootObj["stopwords"] = nlohmann::json::array();
          }

          RootObj["collection"] = CollectionName;

          auto &StopwordsArray = RootObj["stopwords"];

          auto CaseInsensitiveEqual = [](const std::string &A, const std::string &B)
          {
               if (A.length() != B.length())
               {
                    return false;
               }

               for (size_t I = 0; I < A.length(); ++I)
               {
                    if (std::tolower(static_cast<unsigned char>(A[I])) != std::tolower(static_cast<unsigned char>(B[I])))
                    {
                         return false;
                    }
               }

               return true;
          };

          int AddedCount = 0;

          for (const auto &WordStr : WordsToAdd)
          {
               bool Exists = false;

               for (const auto &SW : StopwordsArray)
               {
                    if (SW.contains("word"))
                    {
                         std::string ExistingWord = SW["word"].get<std::string>();

                         if (CaseInsensitiveEqual(WordStr, ExistingWord))
                         {
                              Exists = true;
                              break;
                         }
                    }
               }

               if (!Exists)
               {
                    nlohmann::json NewStopword;

                    NewStopword["word"] = WordStr;
                    NewStopword["created_at"] = GetCurrentTimestamp();
                    NewStopword["updated_at"] = GetCurrentTimestamp();

                    StopwordsArray.push_back(NewStopword);
                    AddedCount++;
               }
          }

          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreCreateStopword, CollectionName, WordsToAdd, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

               if (PreCheck.Action == ModulePreCheckAction::Deny)
               {
                    return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
               }
          }

          std::string UpdatedJSON = RootObj.dump();

          if (!Instance || !Instance->Database || !Instance->Database->Set(StopwordsKey, UpdatedJSON))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          if (WordsToAdd.size() == 1)
          {
               Response.Body = "{\"message\":\"Stopword added\",\"word\":\"" + EscapeJSONString(WordsToAdd[0]) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
          }
          else
          {
               Response.Body = "{\"message\":\"Stopwords added\",\"count\":" + std::to_string(AddedCount) + ",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
          }

          if (AddedCount > 0)
          {
               NOTIFY_MODULES(OnCreateStopword, CollectionName, static_cast<uint64_t>(AddedCount), IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          }

          return Response;
     }
     catch (const std::exception &)
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }
}

/* HandleDeleteStopword deletes a stopword from a collection. */

/*
 * SearchAPI::HandleDeleteStopword implementation.
 */

HttpResponse SearchAPI::HandleDeleteStopword(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName;
     bool IsGlobalScope = false;
     if (!ResolveStopwordScope(Request.Path, ExtractCollectionFromPath(Request.Path), &CollectionName, &IsGlobalScope))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }
     std::string WordStr = ExtractStopwordFromPath(Request.Path);

     if (CollectionName.empty() || WordStr.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     std::string DecodedWord;

     for (size_t I = 0; I < WordStr.size(); ++I)
     {
          if (WordStr[I] == '%' && I + 2 < WordStr.size())
          {
               int HexVal;
               std::istringstream HexStream(WordStr.substr(I + 1, 2));

               if (HexStream >> std::hex >> HexVal && HexVal > 0 && HexVal < 256)
               {
                    DecodedWord += static_cast<char>(HexVal);
                    I += 2;
               }
               else
               {
                    DecodedWord += WordStr[I];
               }
          }
          else if (WordStr[I] == '+')
          {
               DecodedWord += ' ';
          }
          else
          {
               DecodedWord += WordStr[I];
          }
     }

     WordStr = DecodedWord;

     if (!IsGlobalScope)
     {
          std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
          auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

          if (It == CollectionsList.end())
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }
     }

     std::string StopwordsKey = "stopwords:" + CollectionName;
     std::string StopwordsJSON = HybridStorageManagerInstance().Get(StopwordsKey);

     if (StopwordsJSON.empty())
     {
          HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

          Response.Body = "{\"error\":\"Stopword not found\",\"message\":\"The specified stopword does not exist in this collection\",\"word\":\"" + EscapeJSONString(WordStr) +
                          "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

          return Response;
     }

     try
     {
          if (Instance && Instance->Modules)
          {
               ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteStopword, CollectionName, WordStr, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

               if (PreCheck.Action == ModulePreCheckAction::Deny)
               {
                    return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
               }
          }

          nlohmann::json RootObj = nlohmann::json::parse(StopwordsJSON);

          if (RootObj.is_array())
          {
               nlohmann::json LegacyWrapper;

               LegacyWrapper["stopwords"] = RootObj;
               RootObj = LegacyWrapper;
          }

          if (!RootObj.contains("stopwords") || !RootObj["stopwords"].is_array())
          {
               HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

               Response.Body = "{\"error\":\"Stopword not found\",\"message\":\"The specified stopword does not exist in this collection\",\"word\":\"" + EscapeJSONString(WordStr) +
                               "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

               return Response;
          }

          auto &StopwordsArray = RootObj["stopwords"];

          auto CaseInsensitiveEqual = [](const std::string &A, const std::string &B)
          {
               if (A.length() != B.length())
               {
                    return false;
               }

               for (size_t I = 0; I < A.length(); ++I)
               {
                    if (std::tolower(static_cast<unsigned char>(A[I])) != std::tolower(static_cast<unsigned char>(B[I])))
                    {
                         return false;
                    }
               }

               return true;
          };

          bool FoundVal = false;

          for (auto SWIt = StopwordsArray.begin(); SWIt != StopwordsArray.end(); ++SWIt)
          {
               if (SWIt->contains("word"))
               {
                    std::string ExistingWord = (*SWIt)["word"].get<std::string>();

                    if (CaseInsensitiveEqual(WordStr, ExistingWord))
                    {
                         StopwordsArray.erase(SWIt);
                         FoundVal = true;
                         break;
                    }
               }
          }

          if (!FoundVal)
          {
               HttpResponse Response(Status::NOT_FOUND, StatusText(Status::NOT_FOUND), "application/json");

               Response.Body = "{\"error\":\"Stopword not found\",\"message\":\"The specified stopword does not exist in this collection\",\"word\":\"" + EscapeJSONString(WordStr) +
                               "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

               return Response;
          }

          std::string UpdatedJSON = RootObj.dump();

          if (!Instance || !Instance->Database || !Instance->Database->Set(StopwordsKey, UpdatedJSON))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Stopword deleted\",\"word\":\"" + EscapeJSONString(WordStr) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
          NOTIFY_MODULES(OnDeleteStopword, CollectionName, WordStr, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          return Response;
     }
     catch (const std::exception &)
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }
}

HttpResponse SearchAPI::HandleListGlobalStopwords(const HttpRequest &Request)
{
     return HandleListStopwords(Request);
}

HttpResponse SearchAPI::HandleCreateGlobalStopword(const HttpRequest &Request)
{
     return HandleCreateStopword(Request);
}

HttpResponse SearchAPI::HandleDeleteGlobalStopword(const HttpRequest &Request)
{
     return HandleDeleteStopword(Request);
}
