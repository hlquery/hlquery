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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "api/common.h"
#include "api/lexicalsort.h"
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

static const char *kGlobalSynonymsCollection = "__global__";
static constexpr double kSAMAutoExplicitAliasConfidence = 0.95;

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

static std::string TrimSynonymTerm(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static bool IsSAMAutoAliasField(const std::string &Field)
{
     static const std::unordered_set<std::string> AliasFields = {
          "alias", "aliases", "slug", "handle", "username", "short_name", "nickname"
     };

     return AliasFields.count(NormalizeSynonymTerm(TrimSynonymTerm(Field))) > 0;
}

static bool IsSAMAutoSynonymTerm(const std::string &Value)
{
     const std::string Term = TrimSynonymTerm(Value);

     if (Term.size() < 2 || Term.size() > 96)
     {
          return false;
     }

     return std::any_of(Term.begin(), Term.end(),
                        [](unsigned char Ch)
                        {
                             return std::isalnum(Ch) != 0;
                        });
}

static void AppendSAMAutoAliasValues(const std::string &RawValue,
                                     std::vector<std::string> &Values)
{
     try
     {
          const nlohmann::json Parsed = nlohmann::json::parse(RawValue);

          if (Parsed.is_array())
          {
               for (const auto &Entry : Parsed)
               {
                    if (Entry.is_string() && IsSAMAutoSynonymTerm(Entry.get<std::string>()))
                    {
                         Values.push_back(TrimSynonymTerm(Entry.get<std::string>()));
                    }
               }

               return;
          }
     }
     catch (const std::exception &)
     {
     }

     std::string Current;
     for (char Ch : RawValue)
     {
          if (Ch == ',' || Ch == ';' || Ch == '|' || Ch == '\n')
          {
               if (IsSAMAutoSynonymTerm(Current))
               {
                    Values.push_back(TrimSynonymTerm(Current));
               }

               Current.clear();
               continue;
          }

          Current.push_back(Ch);
     }

     if (IsSAMAutoSynonymTerm(Current))
     {
          Values.push_back(TrimSynonymTerm(Current));
     }
}

static std::string BuildSAMAutoSynonymID(const std::string &Root)
{
     uint64_t Hash = 1469598103934665603ULL;

     for (unsigned char Ch : NormalizeSynonymTerm(Root))
     {
          Hash ^= static_cast<uint64_t>(Ch);
          Hash *= 1099511628211ULL;
     }

     std::ostringstream Stream;
     Stream << "sam-auto-" << std::hex << Hash;
     return Stream.str();
}

static std::vector<std::string> TokenizeSAMAutoStopwordText(const std::string &Text)
{
     std::vector<std::string> Tokens;
     std::string Current;

     auto Flush = [&]()
     {
          if (Current.size() >= 2 && Current.size() <= 32)
          {
               Tokens.push_back(Current);
          }
          Current.clear();
     };

     for (unsigned char Ch : Text)
     {
          if (std::isalpha(Ch))
          {
               Current.push_back(static_cast<char>(std::tolower(Ch)));
          }
          else
          {
               Flush();
          }
     }

     Flush();
     return Tokens;
}

static std::string GetSynonymSortValue(const nlohmann::json &Synonym, const std::string &SortBy)
{
     if (!Synonym.is_object() || !Synonym.contains(SortBy) || !Synonym[SortBy].is_string())
     {
          return "";
     }

     return NormalizeSynonymTerm(TrimSynonymTerm(Synonym[SortBy].get<std::string>()));
}

static std::string GetSynonymSortTieBreaker(const nlohmann::json &Synonym)
{
     if (!Synonym.is_object())
     {
          return Synonym.dump();
     }

     return GetSynonymSortValue(Synonym, "id") + "\n" +
            GetSynonymSortValue(Synonym, "root") + "\n" +
            Synonym.dump();
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

     LexicalSortOptions SortOptions;
     std::string SortError;
     if (!ResolveLexicalSortOptions(Request.QueryParams, "root", {"root", "id"}, &SortOptions, &SortError))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::SEARCH_INVALID_PARAMETER, "Invalid sort parameter.", SortError);
     }

     std::sort(SynonymsArray.begin(), SynonymsArray.end(), [&SortOptions](const nlohmann::json &Left, const nlohmann::json &Right)
     {
          return CompareLexicalSortValues(GetSynonymSortValue(Left, SortOptions.SortBy),
                                          GetSynonymSortValue(Right, SortOptions.SortBy),
                                          GetSynonymSortTieBreaker(Left),
                                          GetSynonymSortTieBreaker(Right),
                                          SortOptions.SortOrder);
     });
     Result["sort_by"] = SortOptions.SortBy;
     Result["sort_order"] = SortOptions.SortOrder;
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

          const std::string RootTerm = TrimSynonymTerm(SynonymData["root"].get<std::string>());
          const std::string NormalizedRoot = NormalizeSynonymTerm(RootTerm);

          if (RootTerm.empty() || NormalizedRoot.empty())
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               Response.Body = "{\"error\":\"Invalid synonym group\",\"message\":\"Root term is required.\"}";
               return Response;
          }

          std::unordered_set<std::string> SeenSynonyms;
          nlohmann::json SanitizedSynonyms = nlohmann::json::array();

          for (const auto &Syn : SynonymData["synonyms"])
          {
               if (!Syn.is_string())
               {
                    return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               }

               const std::string SynonymTerm = TrimSynonymTerm(Syn.get<std::string>());
               const std::string NormalizedSynonym = NormalizeSynonymTerm(SynonymTerm);

               if (SynonymTerm.empty() || NormalizedSynonym.empty())
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
                    Response.Body = "{\"error\":\"Invalid synonym group\",\"message\":\"Synonyms must be non-empty strings.\"}";
                    return Response;
               }

               if (!NormalizedRoot.empty() && NormalizedSynonym == NormalizedRoot)
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
                    Response.Body = "{\"error\":\"Invalid synonym group\",\"message\":\"Root term cannot also appear in synonyms.\"}";
                    return Response;
               }

               if (SeenSynonyms.insert(NormalizedSynonym).second)
               {
                    SanitizedSynonyms.push_back(SynonymTerm);
               }
          }

          if (SanitizedSynonyms.empty())
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               Response.Body = "{\"error\":\"Invalid synonym group\",\"message\":\"At least one synonym is required.\"}";
               return Response;
          }

          SynonymData["root"] = RootTerm;
          SynonymData["synonyms"] = SanitizedSynonyms;
          const std::string Source =
               SynonymData.contains("source") && SynonymData["source"].is_string()
                    ? TrimSynonymTerm(SynonymData["source"].get<std::string>())
                    : "";

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
                    if (!Source.empty())
                    {
                         Syn["source"] = Source;
                    }
                    else
                    {
                         Syn.erase("source");
                    }
                    if (SynonymData.contains("confidence") && SynonymData["confidence"].is_number())
                    {
                         Syn["confidence"] = SynonymData["confidence"];
                    }
                    else
                    {
                         Syn.erase("confidence");
                    }

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
               if (!Source.empty())
               {
                    NewSynonym["source"] = Source;
               }
               if (SynonymData.contains("confidence") && SynonymData["confidence"].is_number())
               {
                    NewSynonym["confidence"] = SynonymData["confidence"];
               }

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

          std::string ReplicationOutboxID;
          std::string ReplicationJournalError;
          if (!PrepareReplicationOutboxRecord(Request, "upsert_synonym", &ReplicationOutboxID, &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal unavailable.",
                                         ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
          }

          if (!Instance || !Instance->Database || !Instance->Database->Set(SynonymsKey, UpdatedJSON))
          {
               ClearReplicationOutboxRecord(ReplicationOutboxID);
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "upsert_synonym", &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal incomplete.",
                                         ReplicationJournalError.empty() ? "Synonym was written locally but replication state was not committed durably." : ReplicationJournalError);
          }

          SyncSAMLexicalChange(CollectionName, IsGlobalScope);
          BumpCollectionMutationVersion(IsGlobalScope ? "*" : CollectionName);

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Synonym created/updated\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\",\"scope\":\"" + (IsGlobalScope ? "global" : "collection") + "\"}";
          FOREACH_MOD(OnUpsertSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "upsert_synonym", &ReplicationError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication incomplete.",
                                         ReplicationError.empty() ? "Synonym was written locally but replica acknowledgement failed." : ReplicationError);
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
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

          std::string ReplicationOutboxID;
          std::string ReplicationJournalError;
          if (!PrepareReplicationOutboxRecord(Request, "delete_synonym", &ReplicationOutboxID, &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal unavailable.",
                                         ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
          }

          if (!Instance->Database->Set(SynonymsKey, UpdatedJSON))
          {
               ClearReplicationOutboxRecord(ReplicationOutboxID);
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_synonym", &ReplicationJournalError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication journal incomplete.",
                                         ReplicationJournalError.empty() ? "Synonym was deleted locally but replication state was not committed durably." : ReplicationJournalError);
          }

          SyncSAMLexicalChange(CollectionName, IsGlobalScope);
          BumpCollectionMutationVersion(IsGlobalScope ? "*" : CollectionName);

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"message\":\"Synonym deleted\",\"id\":\"" + EscapeJSONString(SynonymID) + "\",\"collection\":\"" + EscapeJSONString(CollectionName) + "\",\"scope\":\"" + (IsGlobalScope ? "global" : "collection") + "\"}";
          FOREACH_MOD(OnDeleteSynonym, CollectionName, SynonymID, IsGlobalScope, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "delete_synonym", &ReplicationError))
          {
               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Replication incomplete.",
                                         ReplicationError.empty() ? "Synonym was deleted locally but replica acknowledgement failed." : ReplicationError);
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
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

bool SearchAPI::ImproveSAMLexicalResources(const std::string &Collection,
                                           size_t *SynonymUpdates,
                                           size_t *StopwordUpdates,
                                           std::string *ErrorMessage)
{
     if (SynonymUpdates)
     {
          *SynonymUpdates = 0;
     }

     if (StopwordUpdates)
     {
          *StopwordUpdates = 0;
     }

     if (Collection.empty() || !Instance || !Instance->Config || !Instance->Config->GetSamAutoLexicalEnabled())
     {
          return true;
     }

     if (Instance->Config->GetReplicaModeEnabled() &&
         !Instance->Config->GetReplicaAllowWrites())
     {
          return true;
     }

     const int MaxDocuments = Instance->Config->GetSamAutoLexicalMaxDocuments();
     const std::vector<Document> Documents =
          HybridStorageManagerInstance().ListDocuments(Collection, MaxDocuments, 0);

     if (Documents.empty())
     {
          return true;
     }

     if (Instance->Config->GetSamAutoSynonymsEnabled() &&
         Instance->Config->GetSamAutoSynonymMinConfidence() <= kSAMAutoExplicitAliasConfidence)
     {
          nlohmann::json ExistingSynonyms = nlohmann::json::array();
          const std::string ExistingJSON =
               HybridStorageManagerInstance().Get("synonyms:" + Collection);

          if (!ExistingJSON.empty())
          {
               try
               {
                    const nlohmann::json Parsed = nlohmann::json::parse(ExistingJSON);
                    ExistingSynonyms = Parsed.is_array()
                         ? Parsed
                         : Parsed.value("synonyms", nlohmann::json::array());
               }
               catch (const std::exception &)
               {
               }
          }

          std::unordered_set<std::string> ManualRoots;
          std::unordered_map<std::string, nlohmann::json> ExistingAutoGroups;
          size_t AutoGroupCount = 0;

          for (const auto &Group : ExistingSynonyms)
          {
               if (!Group.is_object())
               {
                    continue;
               }

               const std::string ID = Group.value("id", "");
               const std::string Root = NormalizeSynonymTerm(Group.value("root", ""));

               if (Group.value("source", "") == "sam_auto")
               {
                    ++AutoGroupCount;
                    ExistingAutoGroups[ID] = Group;
               }
               else if (!Root.empty())
               {
                    ManualRoots.insert(Root);
               }
          }

          const size_t MaxUpdates =
               static_cast<size_t>(Instance->Config->GetSamAutoSynonymMaxGroupsPerPass());
          const size_t MaxGroups =
               static_cast<size_t>(Instance->Config->GetSamAutoSynonymMaxGroups());
          const size_t MaxTerms =
               static_cast<size_t>(Instance->Config->GetSamAutoSynonymMaxTermsPerGroup());
          size_t Updates = 0;

          for (const auto &Doc : Documents)
          {
               if (Updates >= MaxUpdates)
               {
                    break;
               }

               const std::string Root = TrimSynonymTerm(Doc.Title);
               const std::string NormalizedRoot = NormalizeSynonymTerm(Root);
               if (!IsSAMAutoSynonymTerm(Root) || ManualRoots.count(NormalizedRoot) > 0)
               {
                    continue;
               }

               std::vector<std::string> Aliases;
               for (const auto &Field : Doc.Fields)
               {
                    if (IsSAMAutoAliasField(Field.first))
                    {
                         AppendSAMAutoAliasValues(Field.second, Aliases);
                    }
               }

               std::sort(Aliases.begin(), Aliases.end(),
                         [](const std::string &A, const std::string &B)
                         {
                              return NormalizeSynonymTerm(A) < NormalizeSynonymTerm(B);
                         });
               Aliases.erase(std::unique(Aliases.begin(), Aliases.end(),
                                         [](const std::string &A, const std::string &B)
                                         {
                                              return NormalizeSynonymTerm(A) == NormalizeSynonymTerm(B);
                                         }),
                             Aliases.end());
               Aliases.erase(std::remove_if(Aliases.begin(), Aliases.end(),
                                            [&](const std::string &Alias)
                                            {
                                                 return NormalizeSynonymTerm(Alias) == NormalizedRoot;
                                            }),
                             Aliases.end());

               if (Aliases.size() > MaxTerms)
               {
                    Aliases.resize(MaxTerms);
               }
               if (Aliases.empty())
               {
                    continue;
               }

               const std::string ID = BuildSAMAutoSynonymID(Root);
               const auto ExistingIt = ExistingAutoGroups.find(ID);
               if (ExistingIt == ExistingAutoGroups.end() && AutoGroupCount >= MaxGroups)
               {
                    continue;
               }
               if (ExistingIt != ExistingAutoGroups.end() &&
                   ExistingIt->second.value("root", "") == Root &&
                   ExistingIt->second.value("synonyms", nlohmann::json::array()) == Aliases)
               {
                    continue;
               }

               HttpRequest Request;
               Request.Method = "POST";
               Request.Path = "/collections/" + Collection + "/synonyms/" + ID;
               Request.RemoteAddress = "sam:auto_lexical";
               Request.Body = nlohmann::json({
                    {"root", Root},
                    {"synonyms", Aliases},
                    {"source", "sam_auto"},
                    {"confidence", kSAMAutoExplicitAliasConfidence}
               }).dump();

               const HttpResponse Response = HandleCreateOrUpdateSynonym(Request);

               if (Response.StatusCode < 200 || Response.StatusCode >= 300)
               {
                    if (ErrorMessage)
                    {
                         *ErrorMessage = "Failed to persist SAM auto synonym group '" + ID + "'.";
                    }

                    return false;
               }

               if (ExistingIt == ExistingAutoGroups.end())
               {
                    ++AutoGroupCount;
               }
               ++Updates;
          }

          if (SynonymUpdates)
          {
               *SynonymUpdates = Updates;
          }
     }

     if (Instance->Config->GetSamAutoStopwordsEnabled() &&
         Documents.size() >= static_cast<size_t>(Instance->Config->GetSamAutoStopwordMinDocuments()))
     {
          std::unordered_map<std::string, size_t> DocumentFrequencies;
          for (const auto &Doc : Documents)
          {
               std::unordered_set<std::string> Seen;
               std::string Text = Doc.Title + " " + Doc.Content;
               for (const auto &Field : Doc.Fields)
               {
                    Text += " " + Field.second;
               }
               for (const auto &Token : TokenizeSAMAutoStopwordText(Text))
               {
                    Seen.insert(Token);
               }
               for (const auto &Token : Seen)
               {
                    ++DocumentFrequencies[Token];
               }
          }

          std::unordered_set<std::string> ExistingWords;
          size_t ExistingAutoWords = 0;
          const std::string ExistingJSON =
               HybridStorageManagerInstance().Get("stopwords:" + Collection);
          if (!ExistingJSON.empty())
          {
               try
               {
                    const nlohmann::json Parsed = nlohmann::json::parse(ExistingJSON);
                    const nlohmann::json Words = Parsed.is_array()
                         ? Parsed
                         : Parsed.value("stopwords", nlohmann::json::array());
                    for (const auto &Word : Words)
                    {
                         const std::string Text = Word.is_string()
                              ? Word.get<std::string>()
                              : Word.value("word", "");
                         if (!Text.empty())
                         {
                              ExistingWords.insert(NormalizeSynonymTerm(Text));
                         }
                         if (Word.is_object() && Word.value("source", "") == "sam_auto")
                         {
                              ++ExistingAutoWords;
                         }
                    }
               }
               catch (const std::exception &)
               {
               }
          }

          std::vector<std::pair<std::string, size_t>> Candidates;
          const double MinimumRatio = Instance->Config->GetSamAutoStopwordMinDocumentRatio();
          for (const auto &Entry : DocumentFrequencies)
          {
               if (ExistingWords.count(Entry.first) == 0 &&
                   static_cast<double>(Entry.second) / static_cast<double>(Documents.size()) >= MinimumRatio)
               {
                    Candidates.push_back(Entry);
               }
          }
          std::sort(Candidates.begin(), Candidates.end(),
                    [](const auto &A, const auto &B)
                    {
                         return A.second == B.second ? A.first < B.first : A.second > B.second;
                    });

          const size_t RemainingCapacity =
               ExistingAutoWords >= static_cast<size_t>(Instance->Config->GetSamAutoStopwordMaxWords())
                    ? 0
                    : static_cast<size_t>(Instance->Config->GetSamAutoStopwordMaxWords()) - ExistingAutoWords;
          const size_t MaxUpdates = std::min(
               RemainingCapacity,
               static_cast<size_t>(Instance->Config->GetSamAutoStopwordMaxWordsPerPass()));
          nlohmann::json Words = nlohmann::json::array();

          for (size_t Index = 0; Index < Candidates.size() && Index < MaxUpdates; ++Index)
          {
               Words.push_back(Candidates[Index].first);
          }

          if (!Words.empty())
          {
               HttpRequest Request;
               Request.Method = "POST";
               Request.Path = "/collections/" + Collection + "/stopwords";
               Request.RemoteAddress = "sam:auto_lexical";
               Request.Body = nlohmann::json({
                    {"words", Words},
                    {"source", "sam_auto"}
               }).dump();

               const HttpResponse Response = HandleCreateStopword(Request);
               if (Response.StatusCode < 200 || Response.StatusCode >= 300)
               {
                    if (ErrorMessage)
                    {
                         *ErrorMessage = "Failed to persist SAM auto stopwords.";
                    }
                    return false;
               }

               if (StopwordUpdates)
               {
                    *StopwordUpdates = Words.size();
               }
          }
     }

     return true;
}
