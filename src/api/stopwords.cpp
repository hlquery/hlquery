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
#include "api/lexicalcache.h"
#include "api/searchcache.h"
#include "api/common.h"
#include "api/lexicalsort.h"
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

/* Provides stopword API handlers for per-collection text filtering. */

static const char *kGlobalStopwordsCollection = "__global__";

/* Implements the trim stopword value helper. */

static std::string TrimStopwordValue(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");
     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

/* Normalizes stopword value values. */

static std::string NormalizeStopwordValue(const std::string &Value)
{
     std::string Result = TrimStopwordValue(Value);
     std::transform(Result.begin(), Result.end(), Result.begin(),
                    [](unsigned char Ch)
                    {
                         return static_cast<char>(std::tolower(Ch));
                    });
     return Result;
}

/* Implements the stopword JSON value to text helper. */

static std::string StopwordJSONValueToText(const nlohmann::json &Value)
{
     if (Value.is_string())
     {
          return Value.get<std::string>();
     }

     if (Value.is_object())
     {
          if (Value.contains("word") && Value["word"].is_string())
          {
               return Value["word"].get<std::string>();
          }

          if (Value.contains("text") && Value["text"].is_string())
          {
               return Value["text"].get<std::string>();
          }

          if (Value.contains("value") && Value["value"].is_string())
          {
               return Value["value"].get<std::string>();
          }
     }

     return "";
}

/* Implements the stopwords equal helper. */

static bool StopwordsEqual(const std::string &Left, const std::string &Right)
{
     const std::string NormalizedLeft = NormalizeStopwordValue(Left);
     const std::string NormalizedRight = NormalizeStopwordValue(Right);
     return !NormalizedLeft.empty() && NormalizedLeft == NormalizedRight;
}

/* Checks whether global stopwords path applies. */

static bool IsGlobalStopwordsPath(const std::string &Path)
{
     return Path == "/stopwords/global" ||
            Path.find("/stopwords/global/") == 0 ||
            Path == "/stopword_sets/global" ||
            Path.find("/stopword_sets/global/") == 0;
}

/* Resolves stopword scope values. */

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

     LexicalSortOptions SortOptions;
     std::string SortError;
     if (!ResolveLexicalSortOptions(Request.QueryParams, "word", {"word"}, &SortOptions, &SortError))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid sort parameter.", SortError);
     }

     std::sort(Result["stopwords"].begin(), Result["stopwords"].end(), [&SortOptions](const nlohmann::json &Left, const nlohmann::json &Right)
               {
                    const std::string LeftText = StopwordJSONValueToText(Left);
                    const std::string RightText = StopwordJSONValueToText(Right);
                    return CompareLexicalSortValues(NormalizeStopwordValue(LeftText),
                                                    NormalizeStopwordValue(RightText),
                                                    LeftText + "\n" + Left.dump(),
                                                    RightText + "\n" + Right.dump(),
                                                    SortOptions.SortOrder);
               });
     Result["sort_by"] = SortOptions.SortBy;
     Result["sort_order"] = SortOptions.SortOrder;
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
          const std::string Source =
               StopwordData.contains("source") && StopwordData["source"].is_string()
                    ? TrimStopwordValue(StopwordData["source"].get<std::string>())
                    : "";
          std::vector<std::string> WordsToAdd;
          std::unordered_set<std::string> SeenWords;

          auto AppendWord = [&](const std::string &RawWord) -> bool
          {
               const std::string WordStr = TrimStopwordValue(RawWord);
               const std::string Normalized = NormalizeStopwordValue(WordStr);

               if (WordStr.empty() || Normalized.empty())
               {
                    return false;
               }

               if (SeenWords.insert(Normalized).second)
               {
                    WordsToAdd.push_back(WordStr);
               }

               return true;
          };

          const char *WordsArrayKey = nullptr;
          if (StopwordData.contains("words") && StopwordData["words"].is_array())
          {
               WordsArrayKey = "words";
          }
          else if (StopwordData.contains("stopwords") && StopwordData["stopwords"].is_array())
          {
               WordsArrayKey = "stopwords";
          }

          if (WordsArrayKey != nullptr)
          {
               for (const auto &WordVal : StopwordData[WordsArrayKey])
               {
                    if (!WordVal.is_string())
                    {
                         return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
                    }

                    if (!AppendWord(WordVal.get<std::string>()))
                    {
                         return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
                    }
               }
          }
          else if (StopwordData.contains("word") && StopwordData["word"].is_string())
          {
               if (!AppendWord(StopwordData["word"].get<std::string>()))
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

          int AddedCount = 0;

          for (const auto &WordStr : WordsToAdd)
          {
               bool Exists = false;

               for (const auto &SW : StopwordsArray)
               {
                    std::string ExistingWord = StopwordJSONValueToText(SW);

                    if (!ExistingWord.empty() && StopwordsEqual(WordStr, ExistingWord))
                    {
                         Exists = true;
                         break;
                    }
               }

               if (!Exists)
               {
                    nlohmann::json NewStopword;

                    NewStopword["word"] = WordStr;
                    NewStopword["created_at"] = GetCurrentTimestamp();
                    NewStopword["updated_at"] = GetCurrentTimestamp();
                    if (!Source.empty())
                    {
                         NewStopword["source"] = Source;
                    }

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

          std::string ReplicationOutboxID;
          std::string ReplicationJournalError;
          if (!PrepareReplicationOutboxRecord(Request, "create_stopword", &ReplicationOutboxID, &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal unavailable.",
                                         ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
          }

          if (!Instance || !Instance->Database || !Instance->Database->Set(StopwordsKey, UpdatedJSON))
          {
               ClearReplicationOutboxRecord(ReplicationOutboxID);
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "create_stopword", &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal incomplete.",
                                         ReplicationJournalError.empty() ? "Stopword was written locally but replication state was not committed durably." : ReplicationJournalError);
          }
          BumpCollectionMutationVersion(IsGlobalScope ? "*" : CollectionName);
          if (IsGlobalScope)
          {
               LexicalQueryCache::InvalidateAll();
               SearchResponseCache::InvalidateAll();
          }
          else
          {
               LexicalQueryCache::InvalidateCollection(CollectionName);
               SearchResponseCache::InvalidateCollection(CollectionName);
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
               FOREACH_MOD(OnCreateStopword, CollectionName, static_cast<uint64_t>(AddedCount), IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          }

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "create_stopword", &ReplicationError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication incomplete.",
                                         ReplicationError.empty() ? "Stopword was written locally but replica acknowledgement failed." : ReplicationError);
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
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

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteStopword, CollectionName, WordStr, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

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

          bool FoundVal = false;

          for (auto SWIt = StopwordsArray.begin(); SWIt != StopwordsArray.end(); ++SWIt)
          {
               std::string ExistingWord = StopwordJSONValueToText(*SWIt);

               if (!ExistingWord.empty() && StopwordsEqual(WordStr, ExistingWord))
               {
                    StopwordsArray.erase(SWIt);
                    FoundVal = true;
                    break;
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

          std::string ReplicationOutboxID;
          std::string ReplicationJournalError;
          if (!PrepareReplicationOutboxRecord(Request, "delete_stopword", &ReplicationOutboxID, &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal unavailable.",
                                         ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
          }

          if (!Instance || !Instance->Database || !Instance->Database->Set(StopwordsKey, UpdatedJSON))
          {
               ClearReplicationOutboxRecord(ReplicationOutboxID);
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_stopword", &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal incomplete.",
                                         ReplicationJournalError.empty() ? "Stopword was deleted locally but replication state was not committed durably." : ReplicationJournalError);
          }
          BumpCollectionMutationVersion(IsGlobalScope ? "*" : CollectionName);
          if (IsGlobalScope)
          {
               LexicalQueryCache::InvalidateAll();
               SearchResponseCache::InvalidateAll();
          }
          else
          {
               LexicalQueryCache::InvalidateCollection(CollectionName);
               SearchResponseCache::InvalidateCollection(CollectionName);
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Stopword deleted\",\"word\":\"" + EscapeJSONString(WordStr) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";
          FOREACH_MOD(OnDeleteStopword, CollectionName, WordStr, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "delete_stopword", &ReplicationError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication incomplete.",
                                         ReplicationError.empty() ? "Stopword was deleted locally but replica acknowledgement failed." : ReplicationError);
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }
     catch (const std::exception &)
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }
}

/* Handles list global stopwords requests. */

HttpResponse SearchAPI::HandleListGlobalStopwords(const HttpRequest &Request)
{
     return HandleListStopwords(Request);
}

/* Handles create global stopword requests. */

HttpResponse SearchAPI::HandleCreateGlobalStopword(const HttpRequest &Request)
{
     return HandleCreateStopword(Request);
}

/* Handles delete global stopword requests. */

HttpResponse SearchAPI::HandleDeleteGlobalStopword(const HttpRequest &Request)
{
     return HandleDeleteStopword(Request);
}
