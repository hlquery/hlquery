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
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
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
#include "api/searchcache.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/hybrid_rank_fusion.h"
#include "search/document_collection_store.h"
#include "search/lexical_inverted_index.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides document API handlers for ingestion, retrieval, updates, and deletion. */

static nlohmann::json BuildDocumentJSON(const Document &Doc)
{
     nlohmann::json J;

     if (!Doc.ID.empty())
     {
          J["id"] = Doc.ID;
     }

     if (!Doc.Title.empty())
     {
          J["title"] = Doc.Title;
     }

     if (!Doc.Content.empty())
     {
          J["content"] = Doc.Content;
     }

     if (!Doc.Fields.empty())
     {
          for (const auto &Pair : Doc.Fields)
          {
               J[Pair.first] = Pair.second;
          }
     }

     if (Doc.Score != 0.0)
     {
          J["score"] = Doc.Score;
     }

     if (Doc.Timestamp != 0)
     {
          J["timestamp"] = Doc.Timestamp;
     }

     return J;
}

/* Implements the compare natural string helper. */

static int CompareNaturalString(const std::string &A, const std::string &B)
{
     size_t I = 0;
     size_t J = 0;

     while (I < A.length() && J < B.length())
     {
          if (std::isdigit(static_cast<unsigned char>(A[I])) && std::isdigit(static_cast<unsigned char>(B[J])))
          {
               size_t NumStartA = I;
               size_t NumStartB = J;

               while (I < A.length() && std::isdigit(static_cast<unsigned char>(A[I])))
               {
                    I++;
               }

               while (J < B.length() && std::isdigit(static_cast<unsigned char>(B[J])))
               {
                    J++;
               }

               const long long NumA = std::stoll(A.substr(NumStartA, I - NumStartA));
               const long long NumB = std::stoll(B.substr(NumStartB, J - NumStartB));

               if (NumA != NumB)
               {
                    return (NumA < NumB) ? -1 : 1;
               }
          }
          else
          {
               const char CharA = std::tolower(static_cast<unsigned char>(A[I]));
               const char CharB = std::tolower(static_cast<unsigned char>(B[J]));

               if (CharA != CharB)
               {
                    return (CharA < CharB) ? -1 : 1;
               }

               I++;
               J++;
          }
     }

     if (I < A.length())
     {
          return 1;
     }

     if (J < B.length())
     {
          return -1;
     }

     return 0;
}

/* Handles list documents requests. */

HttpResponse SearchAPI::HandleListDocuments(const HttpRequest &Request)
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

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleListDocuments: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     int OffsetVal = 0;
     int LimitVal = 100;
     bool IncludeCreatedAtVal = true;
     std::string SortByStr = "";

     auto OffsetIt = Request.QueryParams.find("offset");

     if (OffsetIt != Request.QueryParams.end())
     {
          try
          {
               OffsetVal = std::stoi(OffsetIt->second);

               if (OffsetVal < 0)
               {
                    OffsetVal = 0;
               }
          }
          catch (...)
          {
          }
     }

     auto LimitIt = Request.QueryParams.find("limit");

     if (LimitIt != Request.QueryParams.end())
     {
          try
          {
               LimitVal = std::stoi(LimitIt->second);

               if (LimitVal < 1)
               {
                    LimitVal = 1;
               }

               if (LimitVal > 1000)
               {
                    LimitVal = 1000;
               }
          }
          catch (...)
          {
          }
     }

     auto SortByIt = Request.QueryParams.find("sort_by");

     if (SortByIt != Request.QueryParams.end())
     {
          SortByStr = SortByIt->second;
     }
     else
     {
          std::vector<std::string> DefaultSortBy = ResolveDefaultCollectionSortBy(CollectionName);

          if (!DefaultSortBy.empty())
          {
               SortByStr = DefaultSortBy.front();
          }
     }

     auto IncludeDateIt = Request.QueryParams.find("include_created_at");

     if (IncludeDateIt != Request.QueryParams.end())
     {
          std::string Value = IncludeDateIt->second;
          std::transform(Value.begin(), Value.end(), Value.begin(), ::tolower);
          IncludeCreatedAtVal = (Value == "true" || Value == "1" || Value == "yes");
     }

     const bool RequiresGlobalSort = !SortByStr.empty();
     std::vector<Document> Documents;

     const auto AppendStorageDocuments = [&Documents](const std::vector<Document> &StorageDocs)
     {
          Documents.reserve(Documents.size() + StorageDocs.size());

          for (const auto &StorageDoc : StorageDocs)
          {
               Document DocObj;

               DocObj.ID = StorageDoc.ID;
               DocObj.Title = StorageDoc.Title;
               DocObj.Content = StorageDoc.Content;
               DocObj.Fields = StorageDoc.Fields;
               DocObj.Score = StorageDoc.Score;
               DocObj.Timestamp = StorageDoc.Timestamp;

               Documents.push_back(DocObj);
          }
     };

     try
     {
          Documents.clear();

          if (RequiresGlobalSort)
          {
               const int BatchLimit = 1000;
               int BatchOffset = 0;

               while (true)
               {
                    const auto StorageDocs = HybridStorageManagerInstance().ListDocuments(CollectionName, BatchLimit, BatchOffset);

                    if (StorageDocs.empty())
                    {
                         break;
                    }

                    AppendStorageDocuments(StorageDocs);

                    if (static_cast<int>(StorageDocs.size()) < BatchLimit)
                    {
                         break;
                    }

                    BatchOffset += static_cast<int>(StorageDocs.size());
               }
          }
          else
          {
               const auto StorageDocs = HybridStorageManagerInstance().ListDocuments(CollectionName, LimitVal, OffsetVal);
               AppendStorageDocuments(StorageDocs);
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception listing documents: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to list documents: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception listing documents.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while listing documents\"}";

          return Response;
     }

     int TotalVal = 0;

     try
     {
          TotalVal = static_cast<int>(HybridStorageManagerInstance().GetCollectionDocumentCount(CollectionName));

          if (TotalVal == 0 && !Documents.empty())
          {
               TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "HandleListDocuments: Metadata says 0 docs but found " + std::to_string(Documents.size()) + " documents - metadata is wrong, using estimate.");
               }
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception getting document count: " + std::string(E.what()) + " - using Documents.size() as fallback.");
          }

          TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception getting document count - using Documents.size() as fallback.");
          }

          TotalVal = static_cast<int>(Documents.size() + (RequiresGlobalSort ? 0 : OffsetVal));
     }

     if (Documents.empty() && OffsetVal >= TotalVal)
     {
          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

          Response.Body = "{\"documents\":[],\"total\":" + std::to_string(TotalVal) + "}";

          return Response;
     }

     if (!SortByStr.empty())
     {
          std::string FieldNameVal = SortByStr;
          bool DescendingVal = false;

          if (SortByStr.find(":desc") != std::string::npos)
          {
               FieldNameVal = SortByStr.substr(0, SortByStr.find(":desc"));
               DescendingVal = true;
          }
          else if (SortByStr.find(":asc") != std::string::npos)
          {
               FieldNameVal = SortByStr.substr(0, SortByStr.find(":asc"));
               DescendingVal = false;
          }

          std::sort(Documents.begin(), Documents.end(), [&FieldNameVal, DescendingVal](const Document &A, const Document &B)
                    {
                         std::string AValue;
                         std::string BValue;

                         if (FieldNameVal == "id")
                         {
                              AValue = A.ID;
                              BValue = B.ID;
                         }
                         else if (FieldNameVal == "title")
                         {
                              AValue = A.Title;
                              BValue = B.Title;
                         }
                         else if (FieldNameVal == "created_at" || FieldNameVal == "timestamp")
                         {
                              long long ATimestamp = A.Timestamp;
                              long long BTimestamp = B.Timestamp;

                              if (ATimestamp != BTimestamp)
                              {
                                   return DescendingVal ? ATimestamp > BTimestamp : ATimestamp < BTimestamp;
                              }

                              int IDCmp = CompareNaturalString(A.ID, B.ID);
                              if (IDCmp != 0)
                              {
                                   return IDCmp < 0;
                              }

                              return CompareNaturalString(A.Title, B.Title) < 0;
                         }
                         else
                         {
                              AValue = A.Fields.count(FieldNameVal) ? A.Fields.at(FieldNameVal) : "";
                              BValue = B.Fields.count(FieldNameVal) ? B.Fields.at(FieldNameVal) : "";
                         }

                         if (AValue != BValue)
                         {
                              auto ParseDateFunc = [](const std::string &DateStr, std::chrono::system_clock::time_point &TP) -> bool
                              {
                                   if (DateStr.empty())
                                   {
                                        return false;
                                   }

                                   struct tm TMStruct = {};
                                   std::istringstream SS(DateStr);

                                   SS >> std::get_time(&TMStruct, "%Y-%m-%dT%H:%M:%S");

                                   if (SS.fail())
                                   {
                                        SS.clear();
                                        SS.str(DateStr);
                                        SS >> std::get_time(&TMStruct, "%Y-%m-%d %H:%M:%S");

                                        if (SS.fail())
                                        {
                                             return false;
                                        }
                                   }

                                   TP = std::chrono::system_clock::from_time_t(std::mktime(&TMStruct));

                                   return true;
                              };

                              std::chrono::system_clock::time_point TP1;
                              std::chrono::system_clock::time_point TP2;

                              if (ParseDateFunc(AValue, TP1) && ParseDateFunc(BValue, TP2))
                              {
                                   if (TP1 < TP2)
                                   {
                                        return !DescendingVal;
                                   }

                                   if (TP1 > TP2)
                                   {
                                        return DescendingVal;
                                   }

                                   return false;
                              }

                              try
                              {
                                   double ANum = std::stod(AValue);
                                   double BNum = std::stod(BValue);

                                   if (ANum != BNum)
                                   {
                                        return DescendingVal ? ANum > BNum : ANum < BNum;
                                   }
                              }
                              catch (...)
                              {
                                   int Cmp = CompareNaturalString(AValue, BValue);
                                   if (Cmp != 0)
                                   {
                                        return DescendingVal ? (Cmp > 0) : (Cmp < 0);
                                   }
                              }
                         }

                         int IDCmp = CompareNaturalString(A.ID, B.ID);
                         if (IDCmp != 0)
                         {
                              return IDCmp < 0;
                         }

                         return CompareNaturalString(A.Title, B.Title) < 0;
                    });

          if (OffsetVal > 0 || static_cast<int>(Documents.size()) > LimitVal)
          {
               const size_t SliceStart = static_cast<size_t>(std::min(OffsetVal, static_cast<int>(Documents.size())));
               const size_t SliceEnd = std::min(Documents.size(), SliceStart + static_cast<size_t>(LimitVal));

               if (SliceStart >= Documents.size())
               {
                    Documents.clear();
               }
               else
               {
                    Documents = std::vector<Document>(Documents.begin() + static_cast<std::ptrdiff_t>(SliceStart),
                                                      Documents.begin() + static_cast<std::ptrdiff_t>(SliceEnd));
               }
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"documents\":[";

     for (size_t I = 0; I < Documents.size(); ++I)
     {
          if (I > 0)
          {
               Response.Body += ",";
          }

          Response.Body += "{";
          Response.Body += "\"id\":\"" + EscapeJSONString(Documents[I].ID) + "\"";

          if (!Documents[I].Title.empty())
          {
               Response.Body += ",\"title\":\"" + EscapeJSONString(Documents[I].Title) + "\"";
          }

          if (!Documents[I].Content.empty())
          {
               Response.Body += ",\"content\":\"" + EscapeJSONString(Documents[I].Content) + "\"";
          }

          Response.Body += ",\"score\":" + std::to_string(Documents[I].Score);
          Response.Body += ",\"timestamp\":" + std::to_string(Documents[I].Timestamp);

          if (IncludeCreatedAtVal && Documents[I].Timestamp > 0)
          {
               auto TimePointVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(Documents[I].Timestamp));
               time_t TimeTVal = std::chrono::system_clock::to_time_t(TimePointVal);
               auto MSSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(TimePointVal.time_since_epoch()).count();
               long long MSVal = MSSinceEpoch % 1000;

               struct tm TMBuf;
               struct tm *TM = gmtime_r(&TimeTVal, &TMBuf);

               if (TM)
               {
                    std::ostringstream OSS;

                    OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
                    OSS << '.' << std::setfill('0') << std::setw(3) << MSVal << 'Z';

                    Response.Body += ",\"created_at\":\"" + OSS.str() + "\"";
               }
               else
               {
                    Response.Body += ",\"created_at\":\"" + std::to_string(Documents[I].Timestamp) + "\"";
               }
          }

          for (const auto &Field : Documents[I].Fields)
          {
               if (Field.first == "_collection")
               {
                    continue;
               }

               Response.Body += ",\"" + EscapeJSONString(Field.first) + "\":";

               const std::string &FieldValueVal = Field.second;

               if (!FieldValueVal.empty())
               {
                    char *EndPtr = nullptr;

                    std::strtod(FieldValueVal.c_str(), &EndPtr);

                    bool IsNumberVal = false;

                    if (EndPtr != nullptr && EndPtr == FieldValueVal.c_str() + FieldValueVal.length())
                    {
                         if (!FieldValueVal.empty() && (std::isdigit(FieldValueVal[0]) || FieldValueVal[0] == '+' || FieldValueVal[0] == '-' || FieldValueVal[0] == '.'))
                         {
                              IsNumberVal = true;
                         }
                    }

                    if (IsNumberVal)
                    {
                         Response.Body += FieldValueVal;
                    }
                    else if (FieldValueVal == "true" || FieldValueVal == "false")
                    {
                         Response.Body += FieldValueVal;
                    }
                    else
                    {
                         Response.Body += "\"" + EscapeJSONString(FieldValueVal) + "\"";
                    }
               }
               else
               {
                    Response.Body += "\"\"";
               }
          }

          Response.Body += "}";
     }

     Response.Body += "],\"total\":" + std::to_string(TotalVal) + "}";

     return Response;
}

/* HandleAddDocument adds a single document to a collection. */

HttpResponse SearchAPI::HandleAddDocument(const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleAddDocument ENTRY - method=" + Request.Method + " path=" + Request.Path + ".");
     }

     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (Request.Body.size() > 20 * 1024 * 1024)
     {
          HttpResponse Response(Status::PAYLOAD_TOO_LARGE, StatusText(Status::PAYLOAD_TOO_LARGE), "application/json");

          Response.Body = "{\"error\":\"Document too large\",\"message\":\"Document exceeds maximum size of 20MB\",\"max_size\":\"20MB\"}";

          return Response;
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Collection name extracted: '" + CollectionName + "'.");
     }

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     bool CollectionExistsVal = false;

     try
     {
          CollectionExistsVal = HybridStorageManagerInstance().CollectionExists(CollectionName);
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception checking collection existence for '" + CollectionName + "': " + std::string(E.what()) + " - will attempt to auto-create via AddDocument.");
          }
     }

     if (!CollectionExistsVal && Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Collection '" + CollectionName + "' does not exist - AddDocument will auto-create it.");
     }

     Document DocumentObj;
     std::string ParseErrorStr;

     if (!ParseDocumentFromJSON(Request.Body, DocumentObj, &ParseErrorStr))
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          std::string ErrorDetails = ParseErrorStr.empty() ? "Failed to parse document JSON" : ParseErrorStr;

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(ErrorDetails) + "\",\"details\":\"" + EscapeJSONString(ErrorDetails) + "\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "JSON parse failed: " + ErrorDetails + " (body size: " + std::to_string(Request.Body.size()) + ").");
          }

          ConsoleWriter::WriteError("JSON parse failed: " + ErrorDetails);

          return Response;
     }

     bool HasMeaningfulContent = false;

     if (!DocumentObj.Title.empty() || !DocumentObj.Content.empty())
     {
          HasMeaningfulContent = true;
     }
     else if (!DocumentObj.Fields.empty())
     {
          for (const auto &[Key, Value] : DocumentObj.Fields)
          {
               if (Key != "invalid" && Key != "score" && Key != "timestamp" && !Value.empty())
               {
                    HasMeaningfulContent = true;

                    break;
               }
          }
     }

     if (!HasMeaningfulContent)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document\",\"message\":\"Document must contain at least one meaningful field (title, content, or valid custom field)\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Document validation failed: no meaningful fields.");
          }

          return Response;
     }

     if (DocumentObj.ID.empty())
     {
          std::string AutoID = "doc_" + std::to_string([&]() -> int64_t
                                                       {
                                                            if (auto *Inst = Instance; Inst)
                                                            {
                                                                 return Inst->NowMs();
                                                            }

                                                            return static_cast<int64_t>(NowMs());
                                                       }()) +
                               "_" + std::to_string(rand() % 1000000);

          DocumentObj.ID = AutoID;

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "Auto-generated document ID: " + AutoID + ".");
          }
     }

     if (DocumentObj.ID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID must be a non-empty string\"}";

          return Response;
     }

     if (DocumentObj.ID.size() > 256)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Document ID too long\",\"message\":\"Document ID must be between 1 and 256 characters\",\"max_length\":256}";

          return Response;
     }

     for (char C : DocumentObj.ID)
     {
          if (!std::isalnum(C) && C != '_' && C != '-' && C != '.')
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

               Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID can only contain alphanumeric characters, underscores, hyphens, and dots\"}";

               return Response;
          }
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentObj.ID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpRequest ProxyReq = Request;
               nlohmann::json DocJSON = BuildDocumentJSON(DocumentObj);
               ProxyReq.Body = DocJSON.dump();
               ProxyReq.Headers["Content-Type"] = "application/json";

               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(ProxyReq, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward document to target node." : ProxyError);
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "Calling HybridStorageManagerInstance().");
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "[DUPLICATE_CHECK_START] HandleAddDocument: Checking for duplicate - collection='" + CollectionName + "' doc_id='" + DocumentObj.ID + "'.");
     }

     try
     {
          auto &StorageManagerRef = HybridStorageManagerInstance();

          if (Instance && Instance->Database)
          {
               if (CollectionName.empty() || DocumentObj.ID.empty())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "[VALIDATION_FAIL] HandleAddDocument: Empty collection or doc_id - REJECTING.");
                    }

                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Collection name and document ID cannot be empty\"}";

                    return Response;
               }

               std::string DocKey = "doc:" + CollectionName + ":" + DocumentObj.ID;

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "[API_CHECK] Calling Database->Exists() for key: " + DocKey + ".");
               }

               bool DocumentExistsVal = Instance->Database->Exists(DocKey);

               if (DocumentExistsVal)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "[UPSERT] Document ID '" + DocumentObj.ID + "' exists in collection '" + CollectionName + "' - will be updated.");
                    }
               }
               else
               {
                    Instance->Logs->Normal("search_api", "[INSERT] Document ID '" + DocumentObj.ID + "' not found in collection '" + CollectionName + "' - will be inserted.");
               }
          }
          else
          {
               if (StorageManagerRef.CollectionExists(CollectionName))
               {
                    Document ExistingDoc = StorageManagerRef.GetDocument(CollectionName, DocumentObj.ID);

                    if (!ExistingDoc.ID.empty() && ExistingDoc.ID == DocumentObj.ID)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("search_api", "[UPSERT_FALLBACK] Document with ID '" + DocumentObj.ID + "' already exists in collection '" + CollectionName + "' - will be updated.");
                         }
                    }
               }
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception checking for existing document: " + std::string(E.what()) + " - proceeding to storage layer for upsert.");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception checking for existing document - proceeding to storage layer for upsert.");
          }
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreAddDocument, CollectionName, DocumentObj, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "add_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = false;

     try
     {
          auto &StorageManagerRef = HybridStorageManagerInstance();
          Document StorageDoc;

          StorageDoc.ID = DocumentObj.ID;
          StorageDoc.Title = DocumentObj.Title;
          StorageDoc.Content = DocumentObj.Content;
          StorageDoc.Fields = DocumentObj.Fields;
          StorageDoc.Score = DocumentObj.Score;

          if (DocumentObj.Timestamp == 0)
          {
               if (Instance)
               {
                    StorageDoc.Timestamp = Instance->NowMs();
               }
               else
               {
                    StorageDoc.Timestamp = NowMs();
               }
          }
          else
          {
               StorageDoc.Timestamp = DocumentObj.Timestamp;
          }

          SuccessVal = StorageManagerRef.AddDocument(CollectionName, StorageDoc);
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception adding document: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to add document: " + EscapeJSONString(E.what()) + "\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception adding document.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while adding document\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     if (!SuccessVal)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to add document to storage\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "add_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was written locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentObj.ID) + "\"}";
          return JournalResponse;
     }

     FOREACH_MOD(OnAddDocument, CollectionName, DocumentObj.ID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "add_document", &ReplicationError))
     {
          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          Response.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was written locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentObj.ID) + "\"}";
          return Response;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     HttpResponse Response(Status::CREATED, StatusText(Status::CREATED), "application/json");
     nlohmann::json ResultJSON;

     ResultJSON["message"] = "Document added successfully";
     ResultJSON["id"] = DocumentObj.ID;
     ResultJSON["code"] = Code::DOCUMENT_CREATED;
     ResultJSON["code_text"] = CodeText(Code::DOCUMENT_CREATED);

     Response.Body = ResultJSON.dump();

     return Response;
}

/* HandleUpdateDocument updates a document by ID (partial update). */

HttpResponse SearchAPI::HandleUpdateDocument(const HttpRequest &Request)
{
     if (Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID validation failed\"}";

          return Response;
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward update to target node." : ProxyError);
          }
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleDeleteDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     Document ExistingDoc;

     try
     {
          Document StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);

          ExistingDoc.ID = StorageDoc.ID;
          ExistingDoc.Title = StorageDoc.Title;
          ExistingDoc.Content = StorageDoc.Content;
          ExistingDoc.Fields = StorageDoc.Fields;
          ExistingDoc.Score = StorageDoc.Score;
          ExistingDoc.Timestamp = StorageDoc.Timestamp;
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception getting document for update: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to retrieve document: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception getting document for update.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while retrieving document\"}";

          return Response;
     }

     if (ExistingDoc.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     nlohmann::json UpdateJSON;

     try
     {
          UpdateJSON = nlohmann::json::parse(Request.Body);
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }

     Document DocumentObj = ExistingDoc;

     DocumentObj.ID = DocumentID;

     if (UpdateJSON.contains("title"))
     {
          if (UpdateJSON["title"].is_string())
          {
               DocumentObj.Title = UpdateJSON["title"].get<std::string>();
               std::string TitleError;

               if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid title value\",\"message\":\"" + EscapeJSONString(TitleError) + "\"}";

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Title validation failed: " + TitleError + ".");
                    }

                    return Response;
               }
          }
          else if (!UpdateJSON["title"].is_null())
          {
               DocumentObj.Title = UpdateJSON["title"].dump();
               std::string TitleError;

               if (!ValidateFieldValue(DocumentObj.Title, &TitleError, "title"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid title value\",\"message\":\"" + EscapeJSONString(TitleError) + "\"}";

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Title validation failed: " + TitleError + ".");
                    }

                    return Response;
               }
          }
          else
          {
               DocumentObj.Title = "";
          }
     }

     if (UpdateJSON.contains("content"))
     {
          if (UpdateJSON["content"].is_string())
          {
               DocumentObj.Content = UpdateJSON["content"].get<std::string>();
               std::string ContentError;

               if (!ValidateFieldValue(DocumentObj.Content, &ContentError, "content"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid content value\",\"message\":\"" + EscapeJSONString(ContentError) + "\"}";

                    return Response;
               }
          }
          else if (!UpdateJSON["content"].is_null())
          {
               DocumentObj.Content = UpdateJSON["content"].dump();
               std::string ContentError;

               if (!ValidateFieldValue(DocumentObj.Content, &ContentError, "content"))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    Response.Body = "{\"error\":\"Invalid content value\",\"message\":\"" + EscapeJSONString(ContentError) + "\"}";

                    return Response;
               }
          }
          else
          {
               DocumentObj.Content = "";
          }
     }

     for (const auto &[Key, Value] : UpdateJSON.items())
     {
          if (Key == "id" || Key == "title" || Key == "content")
          {
               continue;
          }

          if (Value.is_null())
          {
               DocumentObj.Fields.erase(Key);
          }
          else if (Value.is_string())
          {
               std::string Val = Value.get<std::string>();
               std::string FieldError;

               if (!ValidateFieldValue(Val, &FieldError, Key))
               {
                    HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

                    nlohmann::json ErrorJSON;
                    ErrorJSON["error"] = "Invalid field value";
                    ErrorJSON["field"] = Key;
                    ErrorJSON["message"] = FieldError;
                    Response.Body = ErrorJSON.dump();

                    return Response;
               }

               DocumentObj.Fields[Key] = Val;
          }
          else
          {
               DocumentObj.Fields[Key] = Value.dump();
          }
     }

     Document StorageDoc;

     StorageDoc.ID = DocumentObj.ID;
     StorageDoc.Title = DocumentObj.Title;
     StorageDoc.Content = DocumentObj.Content;
     StorageDoc.Fields = DocumentObj.Fields;
     StorageDoc.Score = DocumentObj.Score;
     StorageDoc.Timestamp = Instance ? Instance->NowMs() : static_cast<uint64_t>(time(nullptr) * 1000);

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateDocument, CollectionName, DocumentObj, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "update_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().AddDocument(CollectionName, StorageDoc);

     if (!SuccessVal)
     {
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to update document in storage\"}";

          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return Response;
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "update_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was updated locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Document updated successfully\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
     FOREACH_MOD(OnUpdateDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "update_document", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was updated locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleDeleteDocument deletes a single document by ID. */

HttpResponse SearchAPI::HandleDeleteDocument(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Distributed ingest unavailable.",
                                         ProxyError.empty() ? "Failed to forward delete to target node." : ProxyError);
          }
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleDeleteDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "delete_document", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().DeleteDocument(CollectionName, DocumentID);

     if (!SuccessVal)
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_document", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Document was deleted locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"deleted\":true,\"message\":\"Document deleted successfully\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
     FOREACH_MOD(OnDeleteDocument, CollectionName, DocumentID, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;

     if (!ReplicateWriteRequest(Request, "delete_document", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Document was deleted locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"id\":\"" + EscapeJSONString(DocumentID) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleDeleteDocumentsByFilter deletes multiple documents matching a filter. */

HttpResponse SearchAPI::HandleDeleteDocumentsByFilter(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteDocuments, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::unordered_map<std::string, std::string> SearchParams;

     if (!Request.QueryParams.empty())
     {
          SearchParams.insert(Request.QueryParams.begin(), Request.QueryParams.end());
     }

     if (!Request.Body.empty())
     {
          try
          {
               nlohmann::json RequestJSON = nlohmann::json::parse(Request.Body);

               if (RequestJSON.contains("query"))
               {
                    if (RequestJSON["query"].is_string())
                    {
                         SearchParams["q"] = RequestJSON["query"].get<std::string>();
                    }
                    else if (RequestJSON["query"].is_object())
                    {
                         if (RequestJSON["query"].contains("q") && RequestJSON["query"]["q"].is_string())
                         {
                              SearchParams["q"] = RequestJSON["query"]["q"].get<std::string>();
                         }

                         if (RequestJSON["query"].contains("filter_by") && RequestJSON["query"]["filter_by"].is_string())
                         {
                              SearchParams["filter_by"] = RequestJSON["query"]["filter_by"].get<std::string>();
                         }
                    }
               }
          }
          catch (const std::exception &)
          {
               return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          }
     }

     ComprehensiveSearchQuery SearchQueryObj = ParseComprehensiveSearchQuery(SearchParams);
     SearchQueryObj.PerPage = std::min(10000, std::max(1, SearchQueryObj.PerPage));

     ComprehensiveSearchResult SearchResultVal = PerformComprehensiveSearch(CollectionName, SearchQueryObj);

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!SearchResultVal.Hits.empty() &&
         !PrepareReplicationOutboxRecord(Request, "delete_documents", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     int DeletedVal = 0;
     int FailedVal = 0;

     for (const auto &HitObj : SearchResultVal.Hits)
     {
          if (!HitObj.Document.count("id"))
          {
               continue;
          }

          std::string DocID = HitObj.Document.at("id");

          if (HybridStorageManagerInstance().DeleteDocument(CollectionName, DocID))
          {
               DeletedVal++;
          }
          else
          {
               FailedVal++;
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json ResultJSON;

     ResultJSON["deleted"] = DeletedVal;
     ResultJSON["failed"] = FailedVal;
     ResultJSON["total"] = static_cast<int>(SearchResultVal.Hits.size());

     Response.Body = ResultJSON.dump();
     if (DeletedVal > 0)
     {
          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_documents", &ReplicationJournalError))
          {
               HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Delete-by-filter was applied locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\"}";
               return JournalResponse;
          }

          FOREACH_MOD(OnDeleteDocuments, CollectionName, static_cast<uint64_t>(DeletedVal), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          BumpCollectionMutationVersion(CollectionName);

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "delete_documents", &ReplicationError))
          {
               HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Delete-by-filter was applied locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\"}";
               return ReplicationResponse;
          }

          ClearReplicationOutboxRecord(ReplicationOutboxID);
     }
     else
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
     }

     return Response;
}

/* HandleGetDocument gets a single document by its ID. */

HttpResponse SearchAPI::HandleGetDocument(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid document ID\",\"message\":\"Document ID validation failed\"}";

          return Response;
     }

     auto RouteIt = Request.QueryParams.find("route");
     const bool HasRoute = (RouteIt != Request.QueryParams.end() && !RouteIt->second.empty());
     if (HasRoute)
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;
          if (!ResolveDistributedRoute(RouteIt->second, &TargetHost, &TargetPort, &IsLocal))
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Invalid distributed route target.",
                                         "Use route=local or route=<host[:port]> for a configured distributed node.");
          }

          if (!IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Routed document fetch unavailable.",
                                         ProxyError.empty() ? "Failed to forward document request to the routed node." : ProxyError);
          }
     }

     bool CollectionExistsLocally = HybridStorageManagerInstance().CollectionExists(CollectionName);
     Document DocObj;
     bool LocalDocLoaded = false;

     try
     {
          Document StorageDoc = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);

          if (!StorageDoc.ID.empty())
          {
               DocObj.ID = StorageDoc.ID;
               DocObj.Title = StorageDoc.Title;
               DocObj.Content = StorageDoc.Content;
               DocObj.Fields = StorageDoc.Fields;
               DocObj.Score = StorageDoc.Score;
               DocObj.Timestamp = StorageDoc.Timestamp;
               LocalDocLoaded = true;
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception in GetDocument: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to retrieve document: " + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception in GetDocument.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while retrieving document\"}";

          return Response;
     }

     if (!HasRoute && !LocalDocLoaded && ShouldAttemptDistributedIngest(Request))
     {
          std::string TargetHost;
          int TargetPort = 0;
          bool IsLocal = false;

          if (SelectDistributedNodeForKey(DocumentID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
          {
               HttpResponse ProxyResp;
               std::string ProxyError;
               if (ProxyDistributedRequest(Request, TargetHost, TargetPort, &ProxyResp, &ProxyError))
               {
                    return ProxyResp;
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "HandleGetDocument: distributed fetch failed for '" + DocumentID + "' on " + TargetHost + ":" + std::to_string(TargetPort) + ", falling back to local lookup. Error: " + (ProxyError.empty() ? std::string("unknown proxy error") : ProxyError) + ".");
               }
          }
     }

     if (!CollectionExistsLocally)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "HandleGetDocument: collection '" + CollectionName + "' not found by CollectionExists.");
          }

          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleGetDocument: Collection found, calling GetDocument('" + CollectionName + "', '" + DocumentID + "').");
     }

     if (DocObj.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     bool IncludeCreatedAtVal = true;
     auto IncludeDateIt = Request.QueryParams.find("include_created_at");

     if (IncludeDateIt != Request.QueryParams.end())
     {
          std::string Value = IncludeDateIt->second;

          std::transform(Value.begin(), Value.end(), Value.begin(), ::tolower);

          IncludeCreatedAtVal = (Value == "true" || Value == "1" || Value == "yes");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{";
     Response.Body += "\"id\":\"" + EscapeJSONString(DocObj.ID) + "\"";

     if (!DocObj.Title.empty())
     {
          Response.Body += ",\"title\":\"" + EscapeJSONString(DocObj.Title) + "\"";
     }

     if (!DocObj.Content.empty())
     {
          Response.Body += ",\"content\":\"" + EscapeJSONString(DocObj.Content) + "\"";
     }

     if (IncludeCreatedAtVal && DocObj.Timestamp > 0)
     {
          auto TimePointVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(DocObj.Timestamp));
          time_t TimeTVal = std::chrono::system_clock::to_time_t(TimePointVal);
          auto MSSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(TimePointVal.time_since_epoch()).count();
          long long MSVal = MSSinceEpoch % 1000;

          struct tm TMBuf;
          struct tm *TM = gmtime_r(&TimeTVal, &TMBuf);

          if (TM)
          {
               std::ostringstream OSS;

               OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
               OSS << '.' << std::setfill('0') << std::setw(3) << MSVal << 'Z';

               Response.Body += ",\"created_at\":\"" + OSS.str() + "\"";
          }
          else
          {
               Response.Body += ",\"created_at\":\"" + std::to_string(DocObj.Timestamp) + "\"";
          }
     }

     for (const auto &Field : DocObj.Fields)
     {
          Response.Body += ",\"" + EscapeJSONString(Field.first) + "\":";

          const std::string &FieldValueVal = Field.second;

          if (!FieldValueVal.empty())
          {
               char *EndPtr = nullptr;

               std::strtod(FieldValueVal.c_str(), &EndPtr);

               bool IsNumberVal = false;

               if (EndPtr != nullptr && EndPtr == FieldValueVal.c_str() + FieldValueVal.length())
               {
                    if (!FieldValueVal.empty() && (std::isdigit(FieldValueVal[0]) || FieldValueVal[0] == '+' || FieldValueVal[0] == '-' || FieldValueVal[0] == '.'))
                    {
                         IsNumberVal = true;
                    }
               }

               if (IsNumberVal)
               {
                    Response.Body += FieldValueVal;
               }
               else if (FieldValueVal == "true" || FieldValueVal == "false")
               {
                    Response.Body += FieldValueVal;
               }
               else
               {
                    Response.Body += "\"" + EscapeJSONString(FieldValueVal) + "\"";
               }
          }
          else
          {
               Response.Body += "\"\"";
          }
     }

     Response.Body += "}";

     return Response;
}

/* Handles get document context requests. */

HttpResponse SearchAPI::HandleGetDocumentContext(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string CollectionName = ExtractCollectionFromPath(Request.Path);
     const std::string DocumentID = ExtractDocumentIdFromPath(Request.Path);

     if (CollectionName.empty() || DocumentID.empty())
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::DOCUMENT_INVALID_ID,
                                    "Invalid document ID",
                                    "Document ID validation failed.");
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     Document DocObj;

     try
     {
          DocObj = HybridStorageManagerInstance().GetDocument(CollectionName, DocumentID);
     }
     catch (const std::exception &E)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Internal server error",
                                    "Failed to retrieve document context: " + std::string(E.what()) + ".");
     }
     catch (...)
     {
          return BuildErrorResponse(Status::INTERNAL_SERVER_ERROR,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Internal server error",
                                    "Unknown error occurred while retrieving document context.");
     }

     if (DocObj.ID.empty())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::DOCUMENT_NOT_FOUND, "Document not found", "The specified document does not exist in this collection.");
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["id"] = DocumentID;

     Root["pending"] = false;
     Root["count"] = 0;
     Root["suggestions"] = nlohmann::json::array();
     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     return Response;
}

/* Handles bulk import documents requests. */

HttpResponse SearchAPI::HandleBulkImportDocuments(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Request.Body.empty())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid request\",\"message\":\"Request body is empty\"}";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Request body is empty.");
          }

          return Response;
     }

     nlohmann::json Payload;

     try
     {
          Payload = nlohmann::json::parse(Request.Body);
     }
     catch (const std::exception &E)
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON\",\"message\":\"Failed to parse JSON: " + EscapeJSONString(E.what()) + "\",\"details\":\"" + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }

     if (!Payload.contains("documents") || !Payload["documents"].is_array())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid payload\",\"message\":\"Expected 'documents' array in request body\",\"details\":\"Expected 'documents' array\"}";

          return Response;
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreBulkImportDocuments, CollectionName, static_cast<uint64_t>(Payload["documents"].size()), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::vector<Document> Documents;
     std::vector<std::string> ParseErrors;

     Documents.reserve(Payload["documents"].size());

     for (const auto &DocJSON : Payload["documents"])
     {
          if (!DocJSON.is_object())
          {
               ParseErrors.push_back("Document entry is not a JSON object");

               continue;
          }

          Document DocumentObj;
          std::string ParseErrorStr;

          if (!ParseDocumentFromJSON(DocJSON, DocumentObj, &ParseErrorStr))
          {
               ParseErrors.push_back("Parse error: " + ParseErrorStr);

               continue;
          }

          Documents.push_back(DocumentObj);
     }

     std::unordered_set<std::string> SeenIDs;
     std::unordered_set<std::string> DuplicateIDs;
     size_t FailedCount = ParseErrors.size();
     std::vector<std::string> ErrorMessages = ParseErrors;
     std::vector<Document> UniqueDocuments;

     UniqueDocuments.reserve(Documents.size());

     for (const auto &DocObj : Documents)
     {
          if (DocObj.ID.empty())
          {
               continue;
          }

          if (SeenIDs.find(DocObj.ID) != SeenIDs.end())
          {
               DuplicateIDs.insert(DocObj.ID);
               FailedCount++;
               ErrorMessages.push_back("Duplicate document ID in batch: '" + DocObj.ID + "'");

               continue;
          }

          SeenIDs.insert(DocObj.ID);
          UniqueDocuments.push_back(DocObj);
     }

     size_t RemoteImportedCount = 0;

     if (ShouldAttemptDistributedIngest(Request))
     {
          std::vector<Document> LocalDocs;
          std::map<std::string, std::vector<Document>> RemoteDocs;

          for (const auto &DocObj : UniqueDocuments)
          {
               std::string TargetHost;
               int TargetPort = 0;
               bool IsLocal = false;

               if (SelectDistributedNodeForKey(DocObj.ID, &TargetHost, &TargetPort, &IsLocal) && !IsLocal)
               {
                    std::string Key = TargetHost + ":" + std::to_string(TargetPort);
                    RemoteDocs[Key].push_back(DocObj);
               }
               else
               {
                    LocalDocs.push_back(DocObj);
               }
          }

          for (const auto &NodePair : RemoteDocs)
          {
               const std::string &Key = NodePair.first;
               size_t ColonPos = Key.rfind(':');
               if (ColonPos == std::string::npos)
               {
                    FailedCount += NodePair.second.size();
                    continue;
               }

               std::string Host = Key.substr(0, ColonPos);
               int Port = std::stoi(Key.substr(ColonPos + 1));

               nlohmann::json PayloadJSON;
               PayloadJSON["documents"] = nlohmann::json::array();
               for (const auto &DocObj : NodePair.second)
               {
                    PayloadJSON["documents"].push_back(BuildDocumentJSON(DocObj));
               }

               HttpRequest ProxyReq = Request;
               ProxyReq.Body = PayloadJSON.dump();
               ProxyReq.Headers["Content-Type"] = "application/json";

               HttpResponse ProxyResp;
               std::string ProxyError;
               if (!ProxyDistributedRequest(ProxyReq, Host, Port, &ProxyResp, &ProxyError))
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back(ProxyError.empty() ? "Failed to forward bulk import to " + Key : ProxyError);
                    }
                    continue;
               }

               if (ProxyResp.StatusCode < 200 || ProxyResp.StatusCode >= 300)
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Remote bulk import failed on " + Key + " with status " + std::to_string(ProxyResp.StatusCode));
                    }
                    continue;
               }

               try
               {
                    nlohmann::json RespJSON = nlohmann::json::parse(ProxyResp.Body);
                    RemoteImportedCount += RespJSON.value("imported", 0);
                    FailedCount += RespJSON.value("failed", 0);
                    if (RespJSON.contains("errors") && RespJSON["errors"].is_array() && ErrorMessages.size() < 100)
                    {
                         for (const auto &ErrVal : RespJSON["errors"])
                         {
                              if (ErrVal.is_string())
                              {
                                   ErrorMessages.push_back(ErrVal.get<std::string>());
                              }
                         }
                    }
               }
               catch (...)
               {
                    FailedCount += NodePair.second.size();
                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Failed to parse remote bulk import response from " + Key);
                    }
               }
          }

          UniqueDocuments = std::move(LocalDocs);
     }

     size_t BatchChunkSize = 2000;
     size_t ImportedCount = RemoteImportedCount;
     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     const bool IsResyncImport = (Request.Headers.count("X-HLQ-Resync-Session") || Request.Headers.count("x-hlq-resync-session"));
     bool AssumeNewDocuments = false;

     auto AssumeNewIt = Request.QueryParams.find("assume_new");

     if (AssumeNewIt != Request.QueryParams.end())
     {
          std::string Value = AssumeNewIt->second;
          std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });
          AssumeNewDocuments = (Value == "1" || Value == "true" || Value == "yes" || Value == "on");
     }

     auto BatchSizeIt = Request.QueryParams.find("batch_size");

     if (BatchSizeIt != Request.QueryParams.end())
     {
          try
          {
               const size_t RequestedBatchSize = static_cast<size_t>(std::stoul(BatchSizeIt->second));

               if (RequestedBatchSize > 0)
               {
                    BatchChunkSize = std::min<size_t>(RequestedBatchSize, 10000);
               }
          }
          catch (...)
          {
          }
     }

     if (!UniqueDocuments.empty() &&
         !PrepareReplicationOutboxRecord(Request, "bulk_import", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     for (size_t ChunkStart = 0; ChunkStart < UniqueDocuments.size(); ChunkStart += BatchChunkSize)
     {
          size_t ChunkEnd = std::min(ChunkStart + BatchChunkSize, UniqueDocuments.size());
          std::vector<Document> Chunk(UniqueDocuments.begin() + ChunkStart, UniqueDocuments.begin() + ChunkEnd);

          try
          {
               bool BatchResultVal = false;

               try
               {
                    std::vector<Document> StorageDocs;
                    StorageDocs.reserve(Chunk.size());

                    for (const auto &DocObj : Chunk)
                    {
                         Document StorageDoc;

                         StorageDoc.ID = DocObj.ID;
                         StorageDoc.Title = DocObj.Title;
                         StorageDoc.Content = DocObj.Content;
                         StorageDoc.Fields = DocObj.Fields;
                         StorageDoc.Score = DocObj.Score;
                         StorageDoc.Timestamp = DocObj.Timestamp;

                         StorageDocs.push_back(StorageDoc);
                    }

                    size_t BatchInsertedCount = HybridStorageManagerInstance().AddDocumentsBatch(CollectionName, StorageDocs, AssumeNewDocuments);
                    BatchResultVal = (BatchInsertedCount > 0);

                    if (BatchResultVal)
                    {
                         ImportedCount += BatchInsertedCount;
                    }
               }
               catch (const std::exception &E)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Exception in bulk import batch (size=" + std::to_string(Chunk.size()) + ", collection='" + CollectionName + "'): " + std::string(E.what()) + ".");
                    }

                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Batch import failed: " + std::string(E.what()));
                    }

                    BatchResultVal = false;
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("search_api", "Unknown exception in bulk import batch (size=" + std::to_string(Chunk.size()) + ", collection='" + CollectionName + "').");
                    }

                    if (ErrorMessages.size() < 100)
                    {
                         ErrorMessages.push_back("Batch import failed: Unknown error occurred");
                    }

                    BatchResultVal = false;
               }

               if (!BatchResultVal)
               {
                    size_t IndividualSuccessCount = 0;

                    for (const auto &DocObj : Chunk)
                    {
                         if (Instance && Instance->Database)
                         {
                              std::string DocKey = "doc:" + CollectionName + ":" + DocObj.ID;
                              std::string ExistingData = Instance->Database->Get(DocKey);

                              if (!ExistingData.empty())
                              {
                                   if (Instance && Instance->Logs)
                                   {
                                        Instance->Logs->Normal("search_api", "Bulk import fallback: Skipping duplicate document ID '" + DocObj.ID + "' in collection '" + CollectionName + "'.");
                                   }

                                   continue;
                              }
                         }

                         try
                         {
                              Document StorageDoc;

                              StorageDoc.ID = DocObj.ID;
                              StorageDoc.Title = DocObj.Title;
                              StorageDoc.Content = DocObj.Content;
                              StorageDoc.Fields = DocObj.Fields;
                              StorageDoc.Score = DocObj.Score;
                              StorageDoc.Timestamp = DocObj.Timestamp;

                              if (HybridStorageManagerInstance().AddDocument(CollectionName, StorageDoc))
                              {
                                   IndividualSuccessCount++;
                              }
                              else
                              {
                                   FailedCount++;
                                   std::string DocIDStr = DocObj.ID.empty() ? "<unknown>" : DocObj.ID;

                                   if (ErrorMessages.size() < 100)
                                   {
                                        ErrorMessages.push_back("Failed to add document '" + DocIDStr + "'");
                                   }
                              }
                         }
                         catch (...)
                         {
                              FailedCount++;
                         }
                    }

                    ImportedCount += IndividualSuccessCount;
               }
          }
          catch (...)
          {
               FailedCount += Chunk.size();
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     nlohmann::json ResponseJSON;

     ResponseJSON["message"] = "Bulk import completed";
     ResponseJSON["imported"] = ImportedCount;
     ResponseJSON["failed"] = FailedCount;

     if (!ErrorMessages.empty())
     {
          ResponseJSON["errors"] = ErrorMessages;
     }

     Response.Body = ResponseJSON.dump();

     if (ImportedCount > 0)
     {
          if (IsResyncImport)
          {
               MaybeTriggerCrashInjection("replication_resync_import");
          }

          MaybeTriggerCrashInjection("replication_after_local_write");

          if (!ReplicationOutboxID.empty() &&
              !MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "bulk_import", &ReplicationJournalError))
          {
               HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Bulk import completed locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\"}";
               return JournalResponse;
          }

          FOREACH_MOD(OnBulkImportDocuments, CollectionName, static_cast<uint64_t>(ImportedCount), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
          BumpCollectionMutationVersion(CollectionName);

          std::string ReplicationError;
          if (!ReplicateWriteRequest(Request, "bulk_import", &ReplicationError))
          {
               HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
               ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Bulk import completed locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\"}";
               return ReplicationResponse;
          }
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleUpdateByQuery updates documents matching a query. */

HttpResponse SearchAPI::HandleUpdateByQuery(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateByQuery, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     /* This endpoint would normally take a query and an update document. */

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Update by query completed\",\"updated\":0}";

     FOREACH_MOD(OnUpdateByQuery, CollectionName, static_cast<uint64_t>(0), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}

/* HandleDeleteByQuery deletes documents matching a query. */

HttpResponse SearchAPI::HandleDeleteByQuery(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteByQuery, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     /* This endpoint would normally take a query and delete matching documents. */

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Delete by query completed\",\"deleted\":0}";

     FOREACH_MOD(OnDeleteByQuery, CollectionName, static_cast<uint64_t>(0), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return Response;
}
