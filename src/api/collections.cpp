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
#include <array>
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
#include <netdb.h>
#include <pthread.h>
#include <regex>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
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
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* Provides collection API handlers for creation, updates, inspection, and deletion. */

struct CollectionMaybeSettings
{
     bool Enabled = false;
     int MinResults = -1;
     int Limit = 5;
};

static int ParseCollectionMaybeInt(const std::string &value, int fallback)
{
     try
     {
          int parsed = std::stoi(value);
          if (parsed < 0)
          {
               return fallback;
          }
          return parsed;
     }
     catch (...)
     {
          return fallback;
     }
}

static std::string ParseCollectionMaybeToken(const std::string &value)
{
     std::string out;
     out.reserve(value.size());

     for (unsigned char c : value)
     {
          if (!std::isspace(c))
          {
               out.push_back(static_cast<char>(std::tolower(c)));
          }
     }

     return out;
}

static bool IsCollectionMaybeTruthyToken(const std::string &token)
{
     return token.empty() || token == "1" || token == "true" || token == "yes" || token == "on";
}

static bool IsCollectionMaybeFalsyToken(const std::string &token)
{
     return token == "0" || token == "false" || token == "no" || token == "off";
}

static bool IsCollectionTruthyParam(const std::map<std::string, std::string> &params, const std::string &name)
{
     auto it = params.find(name);

     if (it == params.end())
     {
          return false;
     }

     return IsCollectionMaybeTruthyToken(ParseCollectionMaybeToken(it->second));
}

static CollectionMaybeSettings ParseCollectionMaybeSettings(const std::map<std::string, std::string> &params)
{
     CollectionMaybeSettings out;
     bool explicit_disable = false;
     constexpr int default_min_results = 5;
     constexpr int default_limit = 5;

     auto itMaybe = params.find("maybe");

     if (itMaybe != params.end())
     {
          std::string raw = itMaybe->second;
          std::string maybe_token = ParseCollectionMaybeToken(raw);

          if (IsCollectionMaybeFalsyToken(maybe_token))
          {
               explicit_disable = true;
          }
          else if (IsCollectionMaybeTruthyToken(maybe_token))
          {
               out.Enabled = true;
               out.MinResults = default_min_results;
               out.Limit = default_limit;
          }

          std::string normalized;
          normalized.reserve(raw.size());

          for (unsigned char c : raw)
          {
               if (c == ':' || c == ';' || c == '|')
               {
                    normalized.push_back(',');
               }
               else if (std::isspace(c))
               {
                    normalized.push_back(',');
               }
               else
               {
                    normalized.push_back(static_cast<char>(c));
               }
          }

          std::stringstream ss(normalized);
          std::string part;
          std::vector<std::string> parts;

          while (std::getline(ss, part, ','))
          {
               if (!part.empty())
               {
                    parts.push_back(part);
               }
          }

          if (!parts.empty() && !explicit_disable && !IsCollectionMaybeTruthyToken(maybe_token))
          {
               out.Enabled = true;
               out.MinResults = ParseCollectionMaybeInt(parts[0], default_min_results);
               if (parts.size() >= 2)
               {
                    out.Limit = ParseCollectionMaybeInt(parts[1], default_limit);
               }
          }
     }

     auto itMin = params.find("maybe_min");

     if (itMin != params.end() && !explicit_disable)
     {
          out.Enabled = true;
          out.MinResults = ParseCollectionMaybeInt(itMin->second, out.MinResults < 0 ? default_min_results : out.MinResults);
     }

     auto itLimit = params.find("maybe_limit");

     if (itLimit != params.end() && !explicit_disable)
     {
          out.Enabled = true;
          out.Limit = ParseCollectionMaybeInt(itLimit->second, out.Limit);
     }

     if (explicit_disable)
     {
          out.Enabled = false;
          out.MinResults = -1;
          out.Limit = default_limit;
          return out;
     }

     if (!out.Enabled)
     {
          return out;
     }

     if (out.MinResults < 0)
     {
          out.MinResults = default_min_results;
     }
     if (out.Limit < 1)
     {
          out.Limit = default_limit;
     }
     if (out.Limit > 20)
     {
          out.Limit = 20;
     }

     return out;
}

static std::string DistToLowerCopy(const std::string &Value)
{
     std::string Out = Value;
     std::transform(Out.begin(), Out.end(), Out.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Out;
}

static std::string DistTrimCopy(const std::string &Value)
{
     size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static std::string DistGetHeaderValueInsensitive(const std::map<std::string, std::string> &Headers, const std::string &Name)
{
     auto It = Headers.find(Name);

     if (It != Headers.end())
     {
          return It->second;
     }

     std::string LowerName = DistToLowerCopy(Name);
     for (const auto &Pair : Headers)
     {
          if (DistToLowerCopy(Pair.first) == LowerName)
          {
               return Pair.second;
          }
     }

     return "";
}

static void DistBuildPeerAuthHeaders(const std::string &Endpoint,
                                     const HttpRequest &Request,
                                     bool UseSecondaryPeerToken,
                                     std::string &ApiKeyOut,
                                     std::string &AuthorizationOut)
{
     ApiKeyOut = DistGetHeaderValueInsensitive(Request.Headers, "X-API-Key");
     AuthorizationOut = DistGetHeaderValueInsensitive(Request.Headers, "Authorization");

     if (!Instance || !Instance->Config || Endpoint.empty())
     {
          return;
     }

     std::string PrimaryToken;
     std::string SecondaryToken;
     if (!Instance->Config->GetClusterPeerTokens(Endpoint, &PrimaryToken, &SecondaryToken))
     {
          return;
     }

     if (PrimaryToken.empty())
     {
          return;
     }

     const std::string &SelectedToken =
          (UseSecondaryPeerToken && !SecondaryToken.empty()) ? SecondaryToken : PrimaryToken;

     ApiKeyOut = SelectedToken;
     AuthorizationOut = "Bearer " + SelectedToken;
}

static bool DistHasSecondaryClusterPeerToken(const std::string &Endpoint)
{
     if (!Instance || !Instance->Config || Endpoint.empty())
     {
          return false;
     }

     std::string PrimaryToken;
     std::string SecondaryToken;
     if (!Instance->Config->GetClusterPeerTokens(Endpoint, &PrimaryToken, &SecondaryToken))
     {
          return false;
     }

     return !SecondaryToken.empty();
}

static bool DistCollectionNameMatchesQuery(const std::string &Name,
                                           const std::string &SearchVal,
                                           const std::string &PatternVal,
                                           bool UseWildcard)
{
     if (UseWildcard)
     {
          return Wildcard::Match(Name, PatternVal);
     }

     if (SearchVal.empty())
     {
          return true;
     }

     return DistToLowerCopy(Name) == DistToLowerCopy(SearchVal);
}

static bool DistCollectionQueryUsesWildcard(const std::string &SearchVal, const std::string &PatternVal)
{
     if (!PatternVal.empty())
     {
          return true;
     }

     return SearchVal.find('*') != std::string::npos || SearchVal.find('?') != std::string::npos;
}

static bool DistParseNodeEndpoint(const std::string &Raw, std::string &HostOut, int &PortOut)
{
     std::string Scheme;
     if (!ParseSharedNodeEndpoint(Raw, HostOut, PortOut, &Scheme))
     {
          return false;
     }

     if (!Scheme.empty() && Scheme != "http")
     {
          return false;
     }

     return true;
}

static bool DistIsLocalHostName(const std::string &Host)
{
     std::string Lower = DistToLowerCopy(Host);
     return (Lower == "localhost" || Lower == "127.0.0.1" || Lower == "::1" || Lower == "0.0.0.0");
}

struct DistNodeEndpoint
{
     std::string Host;
     int Port = 0;
     bool IsLocal = false;
     std::string Raw;
};

static bool DistIsReplicationReplicaEndpoint(const std::string &Endpoint)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     const auto SlaveNodes = Instance->Config->GetSlaveNodes();
     return std::find(SlaveNodes.begin(), SlaveNodes.end(), Endpoint) != SlaveNodes.end();
}

static bool DistBuildClusterEndpoints(std::vector<DistNodeEndpoint> &OutNodes)
{
     if (!Instance || !Instance->Config)
     {
          return false;
     }

     const auto &Nodes = Instance->Config->GetClusterNodes();
     if (Nodes.empty())
     {
          return false;
     }

     std::vector<int> LocalPorts;
     for (const auto &Bind : Instance->Config->GetBindConfigs())
     {
          LocalPorts.push_back(Bind.port);
     }

     for (const auto &Raw : Nodes)
     {
          std::string Host;
          int Port = 0;
          if (!DistParseNodeEndpoint(Raw, Host, Port))
          {
               continue;
          }

          bool Local = false;
          for (int BindPort : LocalPorts)
          {
               if (Port == BindPort)
               {
                    if (DistIsLocalHostName(Host))
                    {
                         Local = true;
                    }
                    else
                    {
                         std::string BindAddr = Instance->Config->GetBindAddress();
                         if (!BindAddr.empty() && BindAddr != "0.0.0.0" && DistToLowerCopy(BindAddr) == DistToLowerCopy(Host))
                         {
                              Local = true;
                         }
                    }
               }
          }

          DistNodeEndpoint Endpoint;
          Endpoint.Host = Host;
          Endpoint.Port = Port;
          Endpoint.IsLocal = Local;
          Endpoint.Raw = Raw;
          OutNodes.push_back(Endpoint);
     }

     OutNodes.erase(std::remove_if(OutNodes.begin(), OutNodes.end(),
                                   [](const DistNodeEndpoint &Node)
                                   {
                                        return !Node.IsLocal && DistIsReplicationReplicaEndpoint(Node.Raw);
                                   }),
                    OutNodes.end());

     return !OutNodes.empty();
}

static bool DistSendHttpRequest(const std::string &Endpoint,
                                const std::string &Host,
                                int Port,
                                const HttpRequest &Request,
                                bool UseSecondaryPeerToken,
                                int TimeoutMS,
                                int *OutStatus,
                                std::string *OutBody,
                                std::string *OutError)
{
     if (OutStatus)
     {
          *OutStatus = 0;
     }
     if (OutBody)
     {
          OutBody->clear();
     }

     addrinfo Hints{};
     Hints.ai_family = AF_UNSPEC;
     Hints.ai_socktype = SOCK_STREAM;
     Hints.ai_protocol = IPPROTO_TCP;

     addrinfo *Res = nullptr;
     std::string PortStr = std::to_string(Port);
     if (getaddrinfo(Host.c_str(), PortStr.c_str(), &Hints, &Res) != 0 || !Res)
     {
          if (OutError)
          {
               *OutError = "Failed to resolve host: " + Host;
          }
          return false;
     }

     int Sock = -1;
     for (addrinfo *P = Res; P != nullptr; P = P->ai_next)
     {
          Sock = socket(P->ai_family, P->ai_socktype, P->ai_protocol);
          if (Sock < 0)
          {
               continue;
          }

          struct timeval TV;
          TV.tv_sec = TimeoutMS / 1000;
          TV.tv_usec = (TimeoutMS % 1000) * 1000;
          setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, &TV, sizeof(TV));
          setsockopt(Sock, SOL_SOCKET, SO_SNDTIMEO, &TV, sizeof(TV));

          if (connect(Sock, P->ai_addr, P->ai_addrlen) == 0)
          {
               break;
          }

          close(Sock);
          Sock = -1;
     }

     freeaddrinfo(Res);

     if (Sock < 0)
     {
          if (OutError)
          {
               *OutError = "Failed to connect to " + Host + ":" + PortStr;
          }
          return false;
     }

     auto UrlEncode = [](const std::string &Value) -> std::string
     {
          std::ostringstream Encoded;
          Encoded.fill('0');
          Encoded << std::uppercase << std::hex;
          for (unsigned char C : Value)
          {
               const bool Unreserved =
                    (C >= 'A' && C <= 'Z') ||
                    (C >= 'a' && C <= 'z') ||
                    (C >= '0' && C <= '9') ||
                    C == '-' || C == '_' || C == '.' || C == '~';
               if (Unreserved)
               {
                    Encoded << static_cast<char>(C);
               }
               else
               {
                    Encoded << '%' << std::setw(2) << static_cast<int>(C);
               }
          }
          return Encoded.str();
     };

     std::string Path = Request.Path.empty() ? "/" : Request.Path;
     if (!Request.QueryParams.empty())
     {
          std::string QueryString;
          bool First = true;
          for (const auto &Pair : Request.QueryParams)
          {
               if (!First)
               {
                    QueryString += "&";
               }
               First = false;
               QueryString += UrlEncode(Pair.first);
               QueryString += "=";
               QueryString += UrlEncode(Pair.second);
          }
          if (!QueryString.empty())
          {
               Path += "?";
               Path += QueryString;
          }
     }

     std::ostringstream Req;
     Req << Request.Method << " " << Path << " HTTP/1.1\r\n";
     Req << "Host: " << Host << ":" << Port << "\r\n";
     Req << "Connection: close\r\n";
     Req << "Accept: application/json\r\n";
     Req << "X-HLQ-Distributed-Hop: 1\r\n";

     std::string ApiKey;
     std::string Auth;
     DistBuildPeerAuthHeaders(Endpoint, Request, UseSecondaryPeerToken, ApiKey, Auth);
     if (!ApiKey.empty())
     {
          Req << "X-API-Key: " << ApiKey << "\r\n";
     }
     if (!Auth.empty())
     {
          Req << "Authorization: " << Auth << "\r\n";
     }

     auto ContentType = DistGetHeaderValueInsensitive(Request.Headers, "Content-Type");
     if (!Request.Body.empty())
     {
          if (ContentType.empty())
          {
               ContentType = "application/json";
          }
          Req << "Content-Type: " << ContentType << "\r\n";
          Req << "Content-Length: " << Request.Body.size() << "\r\n";
     }
     else
     {
          Req << "Content-Length: 0\r\n";
     }

     Req << "\r\n";
     if (!Request.Body.empty())
     {
          Req << Request.Body;
     }

     std::string ReqStr = Req.str();
     ssize_t Sent = 0;
     while (Sent < static_cast<ssize_t>(ReqStr.size()))
     {
          ssize_t WriteCount = send(Sock, ReqStr.data() + Sent, ReqStr.size() - static_cast<size_t>(Sent), 0);
          if (WriteCount <= 0)
          {
               if (OutError)
               {
                    *OutError = "Failed to send request to " + Host + ":" + PortStr;
               }
               close(Sock);
               return false;
          }
          Sent += WriteCount;
     }

     std::string Response;
     char Buffer[4096];
     while (true)
     {
          ssize_t ReadCount = recv(Sock, Buffer, sizeof(Buffer), 0);
          if (ReadCount <= 0)
          {
               break;
          }
          Response.append(Buffer, static_cast<size_t>(ReadCount));
     }

     close(Sock);

     size_t HeaderEnd = Response.find("\r\n\r\n");
     if (HeaderEnd == std::string::npos)
     {
          if (OutError)
          {
               *OutError = "Invalid HTTP response from " + Host + ":" + PortStr;
          }
          return false;
     }

     std::string HeaderStr = Response.substr(0, HeaderEnd);
     std::string BodyStr = Response.substr(HeaderEnd + 4);

     std::istringstream HeaderStream(HeaderStr);
     std::string StatusLine;
     std::getline(HeaderStream, StatusLine);
     int StatusCode = 0;

     if (!StatusLine.empty())
     {
          std::istringstream StatusSS(StatusLine);
          std::string HttpVersion;
          StatusSS >> HttpVersion >> StatusCode;
     }

     if (OutStatus)
     {
          *OutStatus = StatusCode;
     }

     if (OutBody)
     {
          *OutBody = BodyStr;
     }

     return true;
}

static bool DistSendPeerCollectionRequest(const std::string &Endpoint,
                                          const std::string &Host,
                                          int Port,
                                          const HttpRequest &Request,
                                          int TimeoutMS,
                                          int *OutStatus,
                                          std::string *OutBody,
                                          std::string *OutError)
{
     if (!DistSendHttpRequest(Endpoint, Host, Port, Request, false, TimeoutMS, OutStatus, OutBody, OutError))
     {
          return false;
     }

     if ((*OutStatus == 401 || *OutStatus == 403) && DistHasSecondaryClusterPeerToken(Endpoint))
     {
          if (DistSendHttpRequest(Endpoint, Host, Port, Request, true, TimeoutMS, OutStatus, OutBody, OutError) &&
              *OutStatus >= 200 && *OutStatus < 300)
          {
               return true;
          }
     }

     if (*OutStatus == 403 && OutBody)
     {
          std::string LowerBody = *OutBody;
          std::transform(LowerBody.begin(), LowerBody.end(), LowerBody.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (LowerBody.find("authentication is disabled") != std::string::npos ||
              LowerBody.find("tokens are not accepted") != std::string::npos)
          {
               HttpRequest NoAuthRequest = Request;
               NoAuthRequest.Headers.erase("Authorization");
               NoAuthRequest.Headers.erase("authorization");
               NoAuthRequest.Headers.erase("X-API-Key");
               NoAuthRequest.Headers.erase("x-api-key");
               return DistSendHttpRequest("", Host, Port, NoAuthRequest, false, TimeoutMS, OutStatus, OutBody, OutError);
          }
     }

     return true;
}

struct DistCollectionEntry
{
     std::vector<std::string> Nodes;
     std::map<std::string, long long> NodeDocumentCounts;
     long long NumDocuments = 0;
     std::string CreatedAt;
     bool HasCounts = false;
};

static void DistAddCollectionsFromJSON(const nlohmann::json &ResponseJSON,
                                       const std::string &NodeLabel,
                                       std::map<std::string, DistCollectionEntry> &OutMap)
{
     if (!ResponseJSON.contains("collections") || !ResponseJSON["collections"].is_array())
     {
          return;
     }

     for (const auto &Entry : ResponseJSON["collections"])
     {
          std::string Name;
          long long NumDocuments = 0;
          std::string CreatedAt;
          bool HasCounts = false;

          if (Entry.is_string())
          {
               Name = Entry.get<std::string>();
          }
          else if (Entry.is_object() && Entry.contains("name") && Entry["name"].is_string())
          {
               Name = Entry["name"].get<std::string>();
               if (Entry.contains("num_documents"))
               {
                    try
                    {
                         NumDocuments = Entry["num_documents"].get<long long>();
                         HasCounts = true;
                    }
                    catch (...)
                    {
                    }
               }
               if (Entry.contains("created_at"))
               {
                    try
                    {
                         if (Entry["created_at"].is_string())
                         {
                              CreatedAt = Entry["created_at"].get<std::string>();
                         }
                         else
                         {
                              CreatedAt = Entry["created_at"].dump();
                         }
                    }
                    catch (...)
                    {
                    }
               }
          }

          if (Name.empty())
          {
               continue;
          }

          auto &Record = OutMap[Name];
          if (std::find(Record.Nodes.begin(), Record.Nodes.end(), NodeLabel) == Record.Nodes.end())
          {
               Record.Nodes.push_back(NodeLabel);
          }

          if (HasCounts)
          {
               Record.NodeDocumentCounts[NodeLabel] = NumDocuments;
               if (!Record.HasCounts || NumDocuments > Record.NumDocuments)
               {
                    Record.NumDocuments = NumDocuments;
               }
               Record.HasCounts = true;
          }

          if (!CreatedAt.empty())
          {
               if (Record.CreatedAt.empty())
               {
                    Record.CreatedAt = CreatedAt;
               }
               else if (CreatedAt < Record.CreatedAt)
               {
                    Record.CreatedAt = CreatedAt;
               }
          }
     }
}

static std::string DistGetLocalCollectionCreatedAt(const std::string &CollectionName)
{
     std::string ColDir = std::string(HLQUERY_DATA_DIR) + "/collections/" + CollectionName;
     std::string MarkerFile = ColDir + "/.collection";

     if (!std::filesystem::exists(MarkerFile))
     {
          return "";
     }

     std::ifstream Marker(MarkerFile);
     if (!Marker.is_open())
     {
          return "";
     }

     std::string Line;
     std::getline(Marker, Line);

     if (!std::getline(Marker, Line) || Line.find("created_at:") != 0)
     {
          return "";
     }

     try
     {
          long long TimestampSeconds = std::stoll(Line.substr(11));
          time_t TimeVal = static_cast<time_t>(TimestampSeconds);

          struct tm TMBuf;
          struct tm *TM = gmtime_r(&TimeVal, &TMBuf);
          if (!TM)
          {
               return "";
          }

          std::ostringstream OSS;
          OSS << std::put_time(TM, "%Y-%m-%dT%H:%M:%S");
          OSS << ".000Z";
          return OSS.str();
     }
     catch (...)
     {
          return "";
     }
}

static void DistAddLocalCollections(const std::string &NodeLabel,
                                    std::map<std::string, DistCollectionEntry> &OutMap,
                                    nlohmann::json &Errors)
{
     std::vector<std::string> LocalCollections;
     try
     {
          LocalCollections = HybridStorageManagerInstance().ListCollections();
     }
     catch (const std::exception &E)
     {
          nlohmann::json Err;
          Err["endpoint"] = NodeLabel;
          Err["error"] = std::string("Local collections error: ") + E.what();
          Errors.push_back(Err);
          return;
     }

     for (const auto &Name : LocalCollections)
     {
          auto &Record = OutMap[Name];
          if (std::find(Record.Nodes.begin(), Record.Nodes.end(), NodeLabel) == Record.Nodes.end())
          {
               Record.Nodes.push_back(NodeLabel);
          }
          Record.NumDocuments = HybridStorageManagerInstance().GetCollectionDocumentCount(Name);
          Record.NodeDocumentCounts[NodeLabel] = Record.NumDocuments;
          Record.CreatedAt = DistGetLocalCollectionCreatedAt(Name);
          Record.HasCounts = true;
     }
}
/* HandleCreateCollection creates a new collection with schema. */

HttpResponse SearchAPI::HandleCreateCollection(const HttpRequest &Request)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleCreateCollection called with method: " + Request.Method + ".");
          Instance->Logs->Debug("search_api", "Request body: " + Request.Body + ".");
     }

     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     /* Check if request body is empty. */

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

     CollectionConfig ConfigVal;

     if (!ParseCollectionConfigFromJSON(Request.Body, ConfigVal))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "ParseCollectionConfigFromJSON failed.");
          }

          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Invalid JSON or missing required fields\",\"message\":\"Failed to parse collection configuration. Required: 'name' and either 'fields' array or 'searchable_fields' array\"}";

          return Response;
     }

     /* Validate collection schema. */

     std::string ValidationError;

     if (!ValidateCollectionSchema(ConfigVal, ValidationError))
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "Schema validation failed: " + ValidationError + ".");
          }

          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");

          Response.Body = "{\"error\":\"Schema validation failed\",\"message\":\"" + EscapeJSONString(ValidationError) + "\"}";

          return Response;
     }

     /* Check if collection already exists BEFORE calling CreateCollection. */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "[COLLECTION_CHECK_START] HandleCreateCollection: Checking if collection '" + ConfigVal.Name + "' already exists.");
     }

     bool CollectionExistsVal = false;

     try
     {
          CollectionExistsVal = HybridStorageManagerInstance().CollectionExists(ConfigVal.Name);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "[COLLECTION_CHECK_RESULT] CollectionExists() returned: " + std::string(CollectionExistsVal ? "EXISTS" : "NOT_EXISTS") + " for collection: " + ConfigVal.Name + ".");
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "[COLLECTION_CHECK_ERROR] Exception checking collection existence: " + std::string(E.what()) + " - will check again in CreateCollection.");
          }
     }

     if (CollectionExistsVal)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "[COLLECTION_DUPLICATE_REJECTED_API] Collection '" + ConfigVal.Name + "' already exists - REJECTING at API layer.");
          }

          HttpResponse Conflict(Status::CONFLICT, StatusText(Status::CONFLICT), "application/json");

          Conflict.Body = "{\"error\":\"Collection already exists\",\"message\":\"A collection with name '" + EscapeJSONString(ConfigVal.Name) + "' already exists\",\"name\":\"" + EscapeJSONString(ConfigVal.Name) + "\"}";

          return Conflict;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "[COLLECTION_CREATE_CALL] About to call CreateCollection for: " + ConfigVal.Name + ".");
     }

     /* Convert CollectionConfig to CollectionConfig. */

     CollectionConfig StorageConfig;

     StorageConfig.Name = ConfigVal.Name;
     StorageConfig.Fields = ConfigVal.Fields;
     StorageConfig.Metadata = ConfigVal.Metadata;

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreCreateCollection, ConfigVal.Name, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "create_collection", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().CreateCollection(ConfigVal.Name, StorageConfig);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "[COLLECTION_CREATE_RESULT] CreateCollection returned: " + std::string(SuccessVal ? "SUCCESS" : "FAILURE") + " for collection: " + ConfigVal.Name + ".");
     }

     if (!SuccessVal)
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
          const bool ExistsAfterFailure = HybridStorageManagerInstance().CollectionExists(ConfigVal.Name);
          if (ExistsAfterFailure)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "[COLLECTION_DUPLICATE_REJECTED_STORAGE] CreateCollection returned false and collection exists: '" + ConfigVal.Name + "'.");
               }

               HttpResponse Conflict(Status::CONFLICT, StatusText(Status::CONFLICT), "application/json");
               Conflict.Body = "{\"error\":\"Collection already exists\",\"message\":\"A collection with name '" + EscapeJSONString(ConfigVal.Name) + "' already exists\",\"name\":\"" + EscapeJSONString(ConfigVal.Name) + "\"}";
               return Conflict;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("search_api", "[COLLECTION_CREATE_STORAGE_FAILURE] CreateCollection returned false and collection does not exist: '" + ConfigVal.Name + "'.");
          }

          HttpResponse Failure(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          Failure.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to persist collection metadata\"}";
          return Failure;
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "create_collection", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Collection was created locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"name\":\"" + EscapeJSONString(ConfigVal.Name) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::CREATED, StatusText(Status::CREATED), "application/json");

     Response.Body = "{\"message\":\"Collection created successfully\",\"name\":\"" + EscapeJSONString(ConfigVal.Name) + "\"}";
     BumpCollectionMutationVersion(ConfigVal.Name);
     FOREACH_MOD(OnCreateCollection, ConfigVal.Name, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "create_collection", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Collection was created locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"name\":\"" + EscapeJSONString(ConfigVal.Name) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "Returning 201 response for: " + ConfigVal.Name + ".");
     }

     return Response;
}

/* HandleDeleteCollection deletes a collection. */

HttpResponse SearchAPI::HandleDeleteCollection(const HttpRequest &Request)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleDeleteCollection called for path: " + Request.Path + ".");
     }

     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleDeleteCollection: Deleting collection '" + CollectionName + "'.");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteCollection, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;

     if (!PrepareReplicationOutboxRecord(Request, "delete_collection", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     bool SuccessVal = HybridStorageManagerInstance().DeleteCollection(CollectionName);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleDeleteCollection: DeleteCollection returned " + std::string(SuccessVal ? "true" : "false") + ".");
     }

     if (!SuccessVal)
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "delete_collection", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Collection was deleted locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"name\":\"" + EscapeJSONString(CollectionName) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Collection deleted successfully\"}";
     FOREACH_MOD(OnDeleteCollection, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "delete_collection", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Collection was deleted locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"name\":\"" + EscapeJSONString(CollectionName) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleFlush flushes all data from database. */

HttpResponse SearchAPI::HandleFlush(const HttpRequest &Request)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "HandleFlush called.");
     }

     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreFlush, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleFlush: Flushing all data from database - removing all collections, documents, indexes, and caches.");
     }

     /* Get collection count before flush for response. */

     auto CollectionsList = HybridStorageManagerInstance().ListCollections();
     size_t CollectionsCount = CollectionsList.size();
     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "flush", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     /* Perform complete flush - removes all data, indexes, caches, and starts from scratch. */
     bool SuccessVal = HybridStorageManagerInstance().FlushAll();

     if (!SuccessVal)
     {
          ClearReplicationOutboxRecord(ReplicationOutboxID);
          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Flush operation failed\",\"message\":\"An error occurred while flushing the database\"}";

          return Response;
     }
     if (Request.Headers.count("X-HLQ-Resync-Session") || Request.Headers.count("x-hlq-resync-session"))
     {
          MaybeTriggerCrashInjection("replication_resync_flush");
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "flush", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Flush completed locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\"}";
          return JournalResponse;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("search_api", "HandleFlush: Flush completed successfully - deleted " + std::to_string(CollectionsCount) + " collections, cleared all indexes, caches, and data. System is now empty.");
     }

     nlohmann::json ResponseJSON;

     ResponseJSON["message"] = "All data flushed successfully - system reset to empty state";
     ResponseJSON["collections_deleted"] = CollectionsCount;
     ResponseJSON["indexes_cleared"] = true;
     ResponseJSON["caches_cleared"] = true;
     ResponseJSON["mmap_indexes_removed"] = true;     ResponseJSON["success"] = true;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Headers["X-HLQ-Flush-Synced"] = "1";
     Response.Body = ResponseJSON.dump();
     FOREACH_MOD(OnFlush, static_cast<uint64_t>(CollectionsCount), Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     ResetCollectionMutationVersions();
     BumpCollectionMutationVersion("*");

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "flush", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Flush completed locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* CompareNaturalString performs natural string comparison for alphanumeric sorting. */

static int CompareNaturalString(const std::string &A, const std::string &B)
{
     size_t I = 0;
     size_t J = 0;

     while (I < A.length() && J < B.length())
     {
          /* Check if both characters are digits. */

          if (std::isdigit(static_cast<unsigned char>(A[I])) && std::isdigit(static_cast<unsigned char>(B[J])))
          {
               /* Extract numeric values. */

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

               std::string NumStrA = A.substr(NumStartA, I - NumStartA);
               std::string NumStrB = B.substr(NumStartB, J - NumStartB);

               /* Compare as numbers. */

               long long NumA = std::stoll(NumStrA);
               long long NumB = std::stoll(NumStrB);

               if (NumA != NumB)
               {
                    return (NumA < NumB) ? -1 : 1;
               }
          }
          else
          {
               /* Compare as characters (case-insensitive). */

               char CharA = std::tolower(static_cast<unsigned char>(A[I]));
               char CharB = std::tolower(static_cast<unsigned char>(B[J]));

               if (CharA != CharB)
               {
                    return (CharA < CharB) ? -1 : 1;
               }

               I++;
               J++;
          }
     }

     /* One string is a prefix of the other. */

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

/* HandleListCollections lists all collections with optional pagination and sorting. */

HttpResponse SearchAPI::HandleListCollections(const HttpRequest &Request)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("search_api", "[HandleListCollections] ENTRY - method=" + Request.Method + ".");
     }

     if (Request.Method != "GET")
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] Method not allowed: " + Request.Method + ".");
          }

          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     HttpResponse CachedResponse;
     if (SearchResponseCache::Get("collections", Request, "*", CachedResponse))
     {
          return CachedResponse;
     }

     long long OffsetVal = 0;
     long long LimitVal = -1;
     std::string PatternVal;
     std::string SearchVal;
     std::string SortByVal = "name:asc";
     std::string SortOrderVal;
     CollectionMaybeSettings MaybeCfg = ParseCollectionMaybeSettings(Request.QueryParams);
     const bool NamesOnly = IsCollectionTruthyParam(Request.QueryParams, "names_only") ||
                            IsCollectionTruthyParam(Request.QueryParams, "name_only");

     auto OffsetIt = Request.QueryParams.find("offset");

     if (OffsetIt != Request.QueryParams.end())
     {
          try
          {
               OffsetVal = std::stoll(OffsetIt->second);
               if (OffsetVal < 0)
               {
                    OffsetVal = 0;
               }
          }
          catch (...)
          {
               OffsetVal = 0;
          }
     }

     auto LimitIt = Request.QueryParams.find("limit");

     if (LimitIt != Request.QueryParams.end())
     {
          try
          {
               LimitVal = std::stoll(LimitIt->second);
               if (LimitVal < 1)
               {
                    LimitVal = -1;
               }
          }
          catch (...)
          {
               LimitVal = -1;
          }
     }

     auto SearchIt = Request.QueryParams.find("search");
     if (SearchIt != Request.QueryParams.end())
     {
          SearchVal = DistTrimCopy(SearchIt->second);
     }

     auto PatternIt = Request.QueryParams.find("pattern");
     if (PatternIt != Request.QueryParams.end())
     {
          PatternVal = DistTrimCopy(PatternIt->second);
     }

     auto SortOrderIt = Request.QueryParams.find("sort_order");
     if (SortOrderIt != Request.QueryParams.end())
     {
          SortOrderVal = DistTrimCopy(SortOrderIt->second);
     }

     auto SortIt = Request.QueryParams.find("sort_by");
     if (SortIt != Request.QueryParams.end())
     {
          SortByVal = DistTrimCopy(SortIt->second);
          if (SortByVal.empty())
          {
               SortByVal = "name:asc";
          }
     }

     std::vector<std::string> AllCollections;

     try
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] Calling ListCollections().");
          }

          AllCollections = HybridStorageManagerInstance().ListCollections();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] ListCollections() returned " + std::to_string(AllCollections.size()) + " collections.");
          }

          /* Debug: log first few collection names. */

          if (AllCollections.size() > 0 && Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               std::string FirstFew = "";

               for (size_t I = 0; I < std::min(AllCollections.size(), size_t(5)); I++)
               {
                    if (I > 0)
                    {
                         FirstFew += ", ";
                    }

                    FirstFew += "'" + AllCollections[I] + "'";
               }

               Instance->Logs->Debug("search_api", "[HandleListCollections] First few collections: " + FirstFew + ".");
          }
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] CRASH: Exception in ListCollections: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to list collections: " + EscapeJSONString(E.what()) + "\"}";

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] Returning error response after exception.");
          }

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] CRASH: Unknown exception in ListCollections.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred while listing collections\"}";

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("search_api", "[HandleListCollections] Returning error response after unknown exception.");
          }

          return Response;
     }

     bool UseWildcard = false;
     if (!PatternVal.empty())
     {
          UseWildcard = true;
     }
     else if (!SearchVal.empty() && (SearchVal.find('*') != std::string::npos || SearchVal.find('?') != std::string::npos))
     {
          PatternVal = SearchVal;
          UseWildcard = true;
     }

     if (UseWildcard && Instance && Instance->Config && !Instance->Config->GetIndexingEnableCollectionWildcards())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          Response.Body = "{\"error\":\"Collection wildcard search disabled\",\"message\":\"Wildcard collection-name matching is disabled by configuration (indexing.enable_collection_wildcards=false)\"}";
          return Response;
     }

     std::vector<std::string> FilteredCollections;
     FilteredCollections.reserve(AllCollections.size());

     for (const auto &Name : AllCollections)
     {
          if (!DistCollectionNameMatchesQuery(Name, SearchVal, PatternVal, UseWildcard))
          {
               continue;
          }

          FilteredCollections.push_back(Name);
     }

     std::string SortFieldName = SortByVal;
     bool Descending = false;
     size_t ColonPos = SortByVal.find(':');

     if (ColonPos != std::string::npos)
     {
          SortFieldName = SortByVal.substr(0, ColonPos);
          std::string Order = SortByVal.substr(ColonPos + 1);
          Order = DistToLowerCopy(DistTrimCopy(Order));
          if (Order == "desc")
          {
               Descending = true;
          }
     }
     else if (!SortOrderVal.empty())
     {
          std::string Order = DistToLowerCopy(DistTrimCopy(SortOrderVal));
          if (Order == "desc")
          {
               Descending = true;
          }
     }

     SortFieldName = DistToLowerCopy(DistTrimCopy(SortFieldName));
     if (SortFieldName.empty())
     {
          SortFieldName = "name";
     }

     bool SortNeedsMetadata = (SortFieldName == "num_documents" || SortFieldName == "documents" || SortFieldName == "doc_count" ||
                               SortFieldName == "created_at" || SortFieldName == "created");

     struct CollectionEntry
     {
          std::string Name;
          long long NumDocuments = 0;
          std::string CreatedAt;
     };

     std::vector<CollectionEntry> CollectionsList;
     if (SortNeedsMetadata)
     {
          CollectionsList.reserve(FilteredCollections.size());
          for (const auto &Name : FilteredCollections)
          {
               CollectionEntry Entry;
               Entry.Name = Name;
               Entry.NumDocuments = HybridStorageManagerInstance().GetCollectionDocumentCount(Name);
               Entry.CreatedAt = GetCollectionCreatedAt(Name);
               CollectionsList.push_back(std::move(Entry));
          }

          std::sort(CollectionsList.begin(), CollectionsList.end(), [&](const CollectionEntry &A, const CollectionEntry &B)
                    {
                         if (SortFieldName == "num_documents" || SortFieldName == "documents" || SortFieldName == "doc_count")
                         {
                              if (A.NumDocuments == B.NumDocuments)
                              {
                                   return CompareNaturalString(A.Name, B.Name) < 0;
                              }
                              return Descending ? (A.NumDocuments > B.NumDocuments) : (A.NumDocuments < B.NumDocuments);
                         }

                         if (A.CreatedAt == B.CreatedAt)
                         {
                              return CompareNaturalString(A.Name, B.Name) < 0;
                         }
                         return Descending ? (A.CreatedAt > B.CreatedAt) : (A.CreatedAt < B.CreatedAt);
                    });
     }
     else
     {
          std::sort(FilteredCollections.begin(), FilteredCollections.end(), [&](const std::string &A, const std::string &B)
                    {
                         int Cmp = CompareNaturalString(A, B);
                         return Descending ? (Cmp > 0) : (Cmp < 0);
                    });
     }

     size_t TotalCount = SortNeedsMetadata ? CollectionsList.size() : FilteredCollections.size();
     size_t Start = static_cast<size_t>(std::min<long long>(OffsetVal, static_cast<long long>(TotalCount)));
     size_t End = TotalCount;
     if (LimitVal > 0)
     {
          End = std::min(TotalCount, Start + static_cast<size_t>(LimitVal));
     }

     nlohmann::json ResponseJSON;
     ResponseJSON["collections"] = nlohmann::json::array();

     if (NamesOnly && !SortNeedsMetadata)
     {
          for (size_t I = Start; I < End; ++I)
          {
               nlohmann::json Entry;
               Entry["name"] = FilteredCollections[I];
               ResponseJSON["collections"].push_back(Entry);
          }
     }
     else if (SortNeedsMetadata)
     {
          for (size_t I = Start; I < End; ++I)
          {
               CollectionConfig ConfigVal;
               nlohmann::json Entry;
               HybridStorageManagerInstance().GetCollectionConfig(CollectionsList[I].Name, ConfigVal);
               Entry["name"] = CollectionsList[I].Name;
               Entry["num_documents"] = CollectionsList[I].NumDocuments;
               Entry["created_at"] = CollectionsList[I].CreatedAt;
               Entry["metadata"] = ConfigVal.Metadata;
               ResponseJSON["collections"].push_back(Entry);
          }
     }
     else
     {
          for (size_t I = Start; I < End; ++I)
          {
               const std::string &Name = FilteredCollections[I];
               CollectionConfig ConfigVal;
               nlohmann::json Entry;
               HybridStorageManagerInstance().GetCollectionConfig(Name, ConfigVal);
               Entry["name"] = Name;
               Entry["num_documents"] = HybridStorageManagerInstance().GetCollectionDocumentCount(Name);
               Entry["created_at"] = GetCollectionCreatedAt(Name);
               Entry["metadata"] = ConfigVal.Metadata;
               ResponseJSON["collections"].push_back(Entry);
          }
     }

     ResponseJSON["total"] = AllCollections.size();
     ResponseJSON["found"] = TotalCount;
     ResponseJSON["offset"] = OffsetVal;
     if (LimitVal > 0)
     {
          ResponseJSON["limit"] = LimitVal;
     }

     if (MaybeCfg.Enabled && static_cast<int>(TotalCount) < MaybeCfg.MinResults && (!SearchVal.empty() || !PatternVal.empty()))
     {
          std::vector<std::pair<int, std::string>> Ranked;
          std::unordered_set<std::string> Existing(FilteredCollections.begin(), FilteredCollections.end());
          std::string SearchLowerMaybe = DistToLowerCopy(SearchVal);

          for (const auto &Name : AllCollections)
          {
               if (Existing.find(Name) != Existing.end())
               {
                    continue;
               }

               std::string NameLower = DistToLowerCopy(Name);
               int Score = 0;

               if (!SearchLowerMaybe.empty())
               {
                    size_t Pos = NameLower.find(SearchLowerMaybe);
                    if (Pos != std::string::npos)
                    {
                         Score += 100 - static_cast<int>(std::min<size_t>(Pos, 80));
                    }

                    size_t PrefixLen = 0;
         
                    while (PrefixLen < NameLower.size() && PrefixLen < SearchLowerMaybe.size() && NameLower[PrefixLen] == SearchLowerMaybe[PrefixLen])
                    {
                         PrefixLen++;
                    }
         
                    Score += static_cast<int>(PrefixLen) * 4;
               }
               else if (!PatternVal.empty())
               {
                    if (Wildcard::Match(Name, PatternVal))
                    {
                         Score += 80;
                    }
               }

               if (Score <= 0)
               {
                    Score = 1;
               }

               Ranked.push_back({Score, Name});
          }

          std::sort(Ranked.begin(), Ranked.end(), [](const auto &A, const auto &B)
                    {
                         if (A.first != B.first)
                         {
                              return A.first > B.first;
                         }
                         return CompareNaturalString(A.second, B.second) < 0;
                    });

          nlohmann::json MaybeJSON;
          MaybeJSON["threshold"] = MaybeCfg.MinResults;
          MaybeJSON["limit"] = MaybeCfg.Limit;
          MaybeJSON["suggestions"] = nlohmann::json::array();

          int Added = 0;
          for (const auto &Pair : Ranked)
          {
               if (Added >= MaybeCfg.Limit)
               {
                    break;
               }
               nlohmann::json Row;
               Row["name"] = Pair.second;
               Row["score"] = Pair.first;
               MaybeJSON["suggestions"].push_back(Row);
               Added++;
          }

          MaybeJSON["count"] = Added;
          if (Added == 0)
          {
               MaybeJSON["message"] = "nothing man";
          }
          else
          {
               MaybeJSON["message"] = "ok";
          }
          ResponseJSON["maybe"] = MaybeJSON;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ResponseJSON.dump();
     SearchResponseCache::Put("collections", Request, "*", Response);
     return Response;
}

/* HandleListCollectionsDistributed lists collections across configured nodes. */

HttpResponse SearchAPI::HandleListCollectionsDistributed(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     long long OffsetVal = 0;
     long long LimitVal = -1;
     std::string PatternVal;
     std::string SearchVal;
     std::string SortByVal = "name:asc";
     std::string SortOrderVal;

     auto OffsetIt = Request.QueryParams.find("offset");
     if (OffsetIt != Request.QueryParams.end())
     {
          try
          {
               OffsetVal = std::stoll(OffsetIt->second);
               if (OffsetVal < 0)
               {
                    OffsetVal = 0;
               }
          }
          catch (...)
          {
               OffsetVal = 0;
          }
     }

     auto LimitIt = Request.QueryParams.find("limit");
     if (LimitIt != Request.QueryParams.end())
     {
          try
          {
               LimitVal = std::stoll(LimitIt->second);
               if (LimitVal < 1)
               {
                    LimitVal = -1;
               }
          }
          catch (...)
          {
               LimitVal = -1;
          }
     }

     auto SearchIt = Request.QueryParams.find("search");
     if (SearchIt != Request.QueryParams.end())
     {
          SearchVal = DistTrimCopy(SearchIt->second);
     }

     auto PatternIt = Request.QueryParams.find("pattern");
     if (PatternIt != Request.QueryParams.end())
     {
          PatternVal = DistTrimCopy(PatternIt->second);
     }

     auto SortOrderIt = Request.QueryParams.find("sort_order");
     if (SortOrderIt != Request.QueryParams.end())
     {
          SortOrderVal = DistTrimCopy(SortOrderIt->second);
     }

     auto SortIt = Request.QueryParams.find("sort_by");
     if (SortIt != Request.QueryParams.end())
     {
          SortByVal = DistTrimCopy(SortIt->second);
          if (SortByVal.empty())
          {
               SortByVal = "name:asc";
          }
     }

     std::vector<DistNodeEndpoint> Nodes;
     DistBuildClusterEndpoints(Nodes);

     std::map<std::string, DistCollectionEntry> CollectionsMap;
     nlohmann::json Errors = nlohmann::json::array();

     auto RouteIt = Request.QueryParams.find("route");
     const bool HasRoute = (RouteIt != Request.QueryParams.end() && !DistTrimCopy(RouteIt->second).empty());
     bool RouteIsLocal = false;
     std::string RoutedHost;
     int RoutedPort = 0;
     if (HasRoute)
     {
          if (!ResolveDistributedRoute(RouteIt->second, &RoutedHost, &RoutedPort, &RouteIsLocal))
          {
               HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
               Response.Body = "{\"error\":\"Invalid distributed route target\",\"message\":\"Use route=local or route=<host[:port]> for a configured distributed node.\"}";
               return Response;
          }

          if (!RouteIsLocal)
          {
               Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
                                          [&](const DistNodeEndpoint &Node)
                                          {
                                               return Node.IsLocal || DistToLowerCopy(Node.Host) != DistToLowerCopy(RoutedHost) || Node.Port != RoutedPort;
                                          }),
                           Nodes.end());
          }
          else
          {
               Nodes.clear();
          }
     }

     if (!HasRoute || RouteIsLocal)
     {
          DistAddLocalCollections("local", CollectionsMap, Errors);
     }

     int TimeoutMS = 2000;
     if (Instance && Instance->Config)
     {
          TimeoutMS = Instance->Config->GetDistributedSearchTimeoutMS();
     }

     for (const auto &Node : Nodes)
     {
          if (Node.IsLocal)
          {
               continue;
          }

          HttpRequest RemoteRequest = Request;
          RemoteRequest.Method = "GET";
          RemoteRequest.Path = "/collections";
          RemoteRequest.QueryParams.clear();
          RemoteRequest.Body.clear();

          int StatusCode = 0;
          std::string Body;
          std::string Error;
          if (!DistSendPeerCollectionRequest(Node.Raw, Node.Host, Node.Port, RemoteRequest, TimeoutMS, &StatusCode, &Body, &Error))
          {
               nlohmann::json Err;
               Err["endpoint"] = Node.Raw;
               Err["error"] = Error.empty() ? "Request failed" : Error;
               Errors.push_back(Err);
               continue;
          }

          if (StatusCode < 200 || StatusCode >= 300)
          {
               nlohmann::json Err;
               Err["endpoint"] = Node.Raw;
               Err["error"] = "Remote node returned status " + std::to_string(StatusCode);
               Errors.push_back(Err);
               continue;
          }

          nlohmann::json ResponseJSON;
          try
          {
               ResponseJSON = nlohmann::json::parse(Body);
          }
          catch (const nlohmann::json::exception &E)
          {
               nlohmann::json Err;
               Err["endpoint"] = Node.Raw;
               Err["error"] = "Failed to parse remote response JSON";
               Err["message"] = E.what();
               Errors.push_back(Err);
               continue;
          }

          DistAddCollectionsFromJSON(ResponseJSON, Node.Raw, CollectionsMap);
     }

     std::string SortFieldName = SortByVal;
     bool Descending = false;
     size_t ColonPos = SortByVal.find(':');
     if (ColonPos != std::string::npos)
     {
          SortFieldName = SortByVal.substr(0, ColonPos);
          std::string Order = SortByVal.substr(ColonPos + 1);
          Order = DistToLowerCopy(DistTrimCopy(Order));
         
          if (Order == "desc")
          {
               Descending = true;
          }
     }
     else if (!SortOrderVal.empty())
     {
          std::string Order = DistToLowerCopy(DistTrimCopy(SortOrderVal));
          if (Order == "desc")
          {
               Descending = true;
          }
     }

     SortFieldName = DistToLowerCopy(DistTrimCopy(SortFieldName));

     if (SortFieldName.empty())
     {
          SortFieldName = "name";
     }

     bool UseWildcard = false;

     if (!PatternVal.empty())
     {
          UseWildcard = true;
     }
     else if (!SearchVal.empty() && (SearchVal.find('*') != std::string::npos || SearchVal.find('?') != std::string::npos))
     {
          PatternVal = SearchVal;
          UseWildcard = true;
     }

     if (DistCollectionQueryUsesWildcard(SearchVal, PatternVal) &&
         Instance && Instance->Config && !Instance->Config->GetIndexingEnableCollectionWildcards())
     {
          HttpResponse Response(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
          Response.Body = "{\"error\":\"Collection wildcard search disabled\",\"message\":\"Wildcard collection-name matching is disabled by configuration (indexing.enable_collection_wildcards=false)\"}";
          return Response;
     }

     std::vector<std::pair<std::string, DistCollectionEntry>> CollectionsList;
     CollectionsList.reserve(CollectionsMap.size());

     for (const auto &Pair : CollectionsMap)
     {
          if (!DistCollectionNameMatchesQuery(Pair.first, SearchVal, PatternVal, UseWildcard))
          {
               continue;
          }

          CollectionsList.push_back(Pair);
     }

     std::sort(CollectionsList.begin(), CollectionsList.end(), [&](const auto &A, const auto &B)
               {
                    if (SortFieldName == "num_documents" || SortFieldName == "documents" || SortFieldName == "doc_count")
                    {
                         if (A.second.NumDocuments == B.second.NumDocuments)
                         {
                              return CompareNaturalString(A.first, B.first) < 0;
                         }
                         return Descending ? (A.second.NumDocuments > B.second.NumDocuments) : (A.second.NumDocuments < B.second.NumDocuments);
                    }

                    if (SortFieldName == "created_at" || SortFieldName == "created")
                    {
                         if (A.second.CreatedAt == B.second.CreatedAt)
                         {
                              return CompareNaturalString(A.first, B.first) < 0;
                         }
                         return Descending ? (A.second.CreatedAt > B.second.CreatedAt) : (A.second.CreatedAt < B.second.CreatedAt);
                    }

                    int Cmp = CompareNaturalString(A.first, B.first);
                    if (Descending)
                    {
                         return Cmp > 0;
                    }
                    return Cmp < 0;
               });

     size_t TotalCount = CollectionsList.size();
     size_t Start = static_cast<size_t>(std::min<long long>(OffsetVal, static_cast<long long>(TotalCount)));
     size_t End = TotalCount;
     if (LimitVal > 0)
     {
          End = std::min(TotalCount, Start + static_cast<size_t>(LimitVal));
     }

     nlohmann::json ResponseJSON;
     ResponseJSON["status"] = "ok";
     ResponseJSON["nodes"] = nlohmann::json::array();
     for (const auto &Node : Nodes)
     {
          ResponseJSON["nodes"].push_back(Node.Raw);
     }
     ResponseJSON["node_count"] = Nodes.size();

     nlohmann::json CollectionsArray = nlohmann::json::array();
     for (size_t I = Start; I < End; ++I)
     {
          nlohmann::json Entry;
          Entry["name"] = CollectionsList[I].first;
          Entry["nodes"] = CollectionsList[I].second.Nodes;
          Entry["num_documents"] = CollectionsList[I].second.NumDocuments;
          Entry["created_at"] = CollectionsList[I].second.CreatedAt;
          Entry["per_node"] = nlohmann::json::array();
          for (const auto &NodeCount : CollectionsList[I].second.NodeDocumentCounts)
          {
               nlohmann::json NodeEntry;
               NodeEntry["node"] = NodeCount.first;
               NodeEntry["num_documents"] = NodeCount.second;
               Entry["per_node"].push_back(NodeEntry);
          }
          CollectionsArray.push_back(Entry);
     }

     ResponseJSON["collections"] = CollectionsArray;
     ResponseJSON["total"] = CollectionsMap.size();
     ResponseJSON["found"] = TotalCount;
     ResponseJSON["offset"] = OffsetVal;
     if (LimitVal > 0)
     {
          ResponseJSON["limit"] = LimitVal;
     }
     ResponseJSON["errors"] = Errors;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ResponseJSON.dump();
     return Response;
}

/* HandleGetCollection returns information about a specific collection. */

HttpResponse SearchAPI::HandleGetCollection(const HttpRequest &Request)
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

     HttpResponse CachedResponse;
     if (SearchResponseCache::Get("collections", Request, CollectionName, CachedResponse))
     {
          return CachedResponse;
     }

     try
     {
          bool CollectionExistsVal = false;

          try
          {
               CollectionExistsVal = HybridStorageManagerInstance().CollectionExists(CollectionName);
          }
          catch (const std::exception &E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "Exception checking collection existence: " + std::string(E.what()) + ".");
               }

               HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

               Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Failed to check collection existence\"}";

               return Response;
          }

          if (!CollectionExistsVal)
          {
               return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
          }

          size_t NumDocumentsVal = 0;

          try
          {
               NumDocumentsVal = HybridStorageManagerInstance().GetCollectionDocumentCount(CollectionName);
          }
          catch (const std::exception &E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "Exception getting document count: " + std::string(E.what()) + " - using 0.");
               }

               NumDocumentsVal = 0;
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "Unknown exception getting document count - using 0.");
               }

               NumDocumentsVal = 0;
          }

          size_t SizeBytesVal = 0;

          try
          {
               SizeBytesVal = HybridStorageManagerInstance().GetCollectionSize(CollectionName);
          }
          catch (const std::exception &E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "Exception getting collection size: " + std::string(E.what()) + " - using 0.");
               }

               SizeBytesVal = 0;
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("search_api", "Unknown exception getting collection size - using 0.");
               }

               SizeBytesVal = 0;
          }

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
          CollectionConfig ConfigVal;

          HybridStorageManagerInstance().GetCollectionConfig(CollectionName, ConfigVal);

          nlohmann::json ResponseJSON;
          ResponseJSON["name"] = CollectionName;
          ResponseJSON["num_documents"] = NumDocumentsVal;
          ResponseJSON["size_bytes"] = SizeBytesVal;
          ResponseJSON["created_at"] = GetCollectionCreatedAt(CollectionName);
          ResponseJSON["fields"] = ConfigVal.Fields;
          ResponseJSON["metadata"] = ConfigVal.Metadata;

          nlohmann::json SearchableFields = nlohmann::json::array();

          for (const auto &FieldPair : ConfigVal.Fields)
          {
               SearchableFields.push_back(FieldPair.first);
          }

          ResponseJSON["searchable_fields"] = SearchableFields;
          ResponseJSON["filterable_fields"] = nlohmann::json::array();
          ResponseJSON["sortable_fields"] = nlohmann::json::array();
          Response.Body = ResponseJSON.dump();

          SearchResponseCache::Put("collections", Request, CollectionName, Response);
          return Response;
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Exception in HandleGetCollection: " + std::string(E.what()) + ".");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"" + EscapeJSONString(E.what()) + "\"}";

          return Response;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("search_api", "Unknown exception in HandleGetCollection.");
          }

          HttpResponse Response(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");

          Response.Body = "{\"error\":\"Internal server error\",\"message\":\"Unknown error occurred\"}";

          return Response;
     }
}

HttpResponse SearchAPI::HandleGetCollectionLanguage(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string CollectionName = ExtractCollectionFromPath(Request.Path);

     if (CollectionName.empty())
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     HttpResponse CachedResponse;
     if (SearchResponseCache::Get("collections", Request, CollectionName, CachedResponse))
     {
          return CachedResponse;
     }

     if (!HybridStorageManagerInstance().CollectionExists(CollectionName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     auto ReadDocLanguage = [](const Document& Doc) -> std::string
     {
          static const std::array<const char*, 4> LanguageFields = {"lang", "language", "_lang", "locale"};

          for (const char* Field : LanguageFields)
          {
               auto It = Doc.Fields.find(Field);
               if (It == Doc.Fields.end())
               {
                    continue;
               }

               std::string Value = DistTrimCopy(It->second);
               if (Value.empty() || Value == "und")
               {
                    continue;
               }

               return Value;
          }

          return "";
     };

     const size_t SampleLimit = 200;
     const std::vector<Document> Docs = HybridStorageManagerInstance().ListDocuments(CollectionName, static_cast<int>(SampleLimit), 0);

     std::string Language;
     std::string FirstLang;
     bool SawAny = false;

     for (const auto& Doc : Docs)
     {
          std::string DocLang = ReadDocLanguage(Doc);
          if (DocLang.empty())
          {
               continue;
          }

          if (!SawAny)
          {
               FirstLang = DocLang;
               SawAny = true;
               continue;
          }

          if (!FirstLang.empty() && DocLang != FirstLang)
          {
               Language = "multi";
               break;
          }
     }

     if (Language.empty() && SawAny)
     {
          Language = FirstLang;
     }

     if (Language.empty())
     {
          CollectionConfig ConfigVal;
          HybridStorageManagerInstance().GetCollectionConfig(CollectionName, ConfigVal);
          auto It = ConfigVal.Metadata.find("_lang");
          if (It != ConfigVal.Metadata.end())
          {
               Language = DistTrimCopy(It->second);
          }
     }

     if (Language.empty())
     {
          Language = "und";
     }

     nlohmann::json Root;
     Root["ok"] = true;
     Root["collection"] = CollectionName;
     Root["lang"] = Language;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = Root.dump();
     SearchResponseCache::Put("collections", Request, CollectionName, Response);
     return Response;
}

/* HandleUpdateCollection updates an existing collection. */

HttpResponse SearchAPI::HandleUpdateCollection(const HttpRequest &Request)
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

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     std::string BodyWithName = Request.Body;

     if (!BodyWithName.empty() && BodyWithName[0] == '{')
     {
          try
          {
               nlohmann::json BodyJSON = nlohmann::json::parse(BodyWithName);

               if (!BodyJSON.contains("name") || BodyJSON["name"].is_null() || BodyJSON["name"].empty())
               {
                    BodyJSON["name"] = CollectionName;
                    BodyWithName = BodyJSON.dump();
               }
          }
          catch (const std::exception &)
          {
          }
     }

     CollectionConfig ConfigVal;

     if (!ParseCollectionConfigFromJSON(BodyWithName, ConfigVal))
     {
          return HttpResponse(Status::BAD_REQUEST, StatusText(Status::BAD_REQUEST), "application/json");
     }

     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateCollection, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string ReplicationOutboxID;
     std::string ReplicationJournalError;
     if (!PrepareReplicationOutboxRecord(Request, "update_collection", &ReplicationOutboxID, &ReplicationJournalError))
     {
          return BuildErrorResponse(Status::SERVICE_UNAVAILABLE,
                                    Code::SEARCH_INVALID_PARAMETER,
                                    "Replication journal unavailable.",
                                    ReplicationJournalError.empty() ? "Failed to persist replication intent before local write." : ReplicationJournalError);
     }

     for (const auto &metadata_pair : ConfigVal.Metadata)
     {
          if (!HybridStorageManagerInstance().UpdateCollectionMetadata(CollectionName, metadata_pair.first, metadata_pair.second))
          {
               ClearReplicationOutboxRecord(ReplicationOutboxID);
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }
     }

     MaybeTriggerCrashInjection("replication_after_local_write");

     if (!MarkReplicationOutboxCommitted(ReplicationOutboxID, Request, "update_collection", &ReplicationJournalError))
     {
          HttpResponse JournalResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          JournalResponse.Body = "{\"error\":\"Replication journal incomplete\",\"message\":\"Collection metadata was updated locally but replication state was not committed durably\",\"details\":\"" + EscapeJSONString(ReplicationJournalError) + "\",\"name\":\"" + EscapeJSONString(CollectionName) + "\"}";
          return JournalResponse;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"message\":\"Collection updated successfully\",\"name\":\"" + EscapeJSONString(CollectionName) + "\"}";
     FOREACH_MOD(OnUpdateCollection, CollectionName, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     BumpCollectionMutationVersion(CollectionName);

     std::string ReplicationError;
     if (!ReplicateWriteRequest(Request, "update_collection", &ReplicationError))
     {
          HttpResponse ReplicationResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ReplicationResponse.Body = "{\"error\":\"Replication incomplete\",\"message\":\"Collection metadata was updated locally but replica acknowledgement failed\",\"details\":\"" + EscapeJSONString(ReplicationError) + "\",\"name\":\"" + EscapeJSONString(CollectionName) + "\"}";
          return ReplicationResponse;
     }

     ClearReplicationOutboxRecord(ReplicationOutboxID);

     return Response;
}

/* HandleFacetCounts handles facet counts for a collection. */

HttpResponse SearchAPI::HandleFacetCounts(const HttpRequest &Request)
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

     std::vector<std::string> CollectionsList = HybridStorageManagerInstance().ListCollections();
     auto It = std::find(CollectionsList.begin(), CollectionsList.end(), CollectionName);

     if (It == CollectionsList.end())
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"facet_counts\":{},\"collection\":\"" + EscapeJSONString(CollectionName) + "\"}";

     return Response;
}
