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

#include "api/httpserver.h"
#include "api/searchapi.h"
#include "api/common.h"
#include "api/userauth.h"
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

struct LinkEndpointInfo
{
     std::string RawEndpoint;
     std::string NormalizedEndpoint;
     std::string Host;
     int Port = 0;
     bool IsValid = false;
     bool IsLocal = false;
     bool Reachable = false;
     int StatusCode = 0;
     double LatencyMS = 0.0;
     std::string Error;
};

static bool HealthIsLoopbackHost(const std::string &Host)
{
     return Host == "127.0.0.1" || Host == "localhost" || Host == "::1";
}

static std::string HealthTrimWhitespace(const std::string &Value)
{
     size_t Start = 0;

     while (Start < Value.size() && std::isspace(static_cast<unsigned char>(Value[Start])))
     {
          ++Start;
     }

     size_t End = Value.size();

     while (End > Start && std::isspace(static_cast<unsigned char>(Value[End - 1])))
     {
          --End;
     }

     return Value.substr(Start, End - Start);
}

static bool HealthParseBoolParam(const std::map<std::string, std::string> &Params, const std::string &Key, bool DefaultValue = false)
{
     const auto it = Params.find(Key);

     if (it == Params.end())
     {
          return DefaultValue;
     }

     std::string normalized = HealthTrimWhitespace(it->second);
     std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch)
                    {
                         return static_cast<char>(std::tolower(ch));
                    });

     if (normalized.empty())
     {
          return DefaultValue;
     }

     return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

static bool HealthBindMatchesHost(const std::string &BindAddress, const std::string &Host)
{
     if (BindAddress.empty())
     {
          return false;
     }

     if (BindAddress == "0.0.0.0" || BindAddress == "::")
     {
          return true;
     }

     if (BindAddress == Host)
     {
          return true;
     }

     if ((BindAddress == "127.0.0.1" || BindAddress == "localhost") &&
         (Host == "127.0.0.1" || Host == "localhost"))
     {
          return true;
     }

     return false;
}

static bool HealthIsLocalHttpBind(const std::string &Host, int Port)
{
     if (Port <= 0 || !Instance || !Instance->Config)
     {
          return false;
     }

     const bool IsLoopback = HealthIsLoopbackHost(Host);
     const bool IsWildcard = (Host == "0.0.0.0" || Host == "::");

     const auto &BindConfigs = Instance->Config->GetBindConfigs();
     for (const auto &Bind : BindConfigs)
     {
          if (Bind.port != Port)
          {
               continue;
          }

          if (Bind.type != "http")
          {
               continue;
          }

          if (IsWildcard)
          {
               if (Bind.address == "0.0.0.0" || Bind.address == "::")
               {
                    return true;
               }

               continue;
          }

          if (Bind.address == Host)
          {
               return true;
          }

          if (IsLoopback && HealthBindMatchesHost(Bind.address, Host))
          {
               return true;
          }
     }

     return false;
}

static bool HealthParseNodeEndpoint(const std::string &Raw, std::string &HostOut, int &PortOut)
{
     NodeEndpointParseOptions Options;
     Options.DefaultPort = 9200;
     Options.AllowEmptyPort = false;
     return ParseSharedNodeEndpoint(Raw, HostOut, PortOut, nullptr, Options);
}

static LinkEndpointInfo HealthBuildEndpointInfo(const std::string &RawEndpoint)
{
     LinkEndpointInfo Info;
     Info.RawEndpoint = RawEndpoint;
     Info.NormalizedEndpoint = HealthTrimWhitespace(RawEndpoint);

     if (!HealthParseNodeEndpoint(Info.NormalizedEndpoint, Info.Host, Info.Port))
     {
          Info.Error = "Invalid endpoint format";
          return Info;
     }

     Info.IsValid = true;
     Info.NormalizedEndpoint = Info.Host + ":" + std::to_string(Info.Port);
     Info.IsLocal = HealthIsLocalHttpBind(Info.Host, Info.Port);
     return Info;
}

static bool HealthSendPingRequest(const std::string &Host,
                                  int Port,
                                  int TimeoutMS,
                                  int *OutStatusCode,
                                  double *OutLatencyMS,
                                  std::string *OutError);

static std::vector<std::string> HealthGetLocalLoadedModules();

static bool HealthValidateRemoteModules(const std::string &ResponseBody, std::string *OutError);

static void HealthProbeEndpoint(LinkEndpointInfo &Info, bool PingNode)
{
     if (!Info.IsValid || !PingNode)
     {
          return;
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("links", "Ping check for link " + Info.Host + ":" + std::to_string(Info.Port) + ".");
     }

     if (Info.IsLocal)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("links", "Ping shortcut for local bind " + Info.Host + ":" + std::to_string(Info.Port) + " (marked reachable without outbound probe).");
          }

          Info.Reachable = true;
          Info.StatusCode = 200;
          Info.LatencyMS = 0.0;
          return;
     }

     const int TimeoutMS = (Instance && Instance->Config) ? Instance->Config->GetDistributedSearchTimeoutMS() : 0;
     bool Ok = HealthSendPingRequest(Info.Host, Info.Port, TimeoutMS, &Info.StatusCode, &Info.LatencyMS, &Info.Error);
     Info.Reachable = Ok && Info.StatusCode >= 200 && Info.StatusCode < 300;
}

static nlohmann::json HealthEndpointInfoToJSON(const LinkEndpointInfo &Info)
{
     nlohmann::json EndpointJSON;
     EndpointJSON["endpoint"] = Info.RawEndpoint;
     EndpointJSON["normalized_endpoint"] = Info.NormalizedEndpoint;
     EndpointJSON["reachable"] = Info.Reachable;

     if (!Info.IsValid)
     {
          EndpointJSON["error"] = Info.Error.empty() ? "Invalid endpoint format" : Info.Error;
          return EndpointJSON;
     }

     EndpointJSON["host"] = Info.Host;
     EndpointJSON["port"] = Info.Port;
     EndpointJSON["is_local"] = Info.IsLocal;
     EndpointJSON["status_code"] = Info.StatusCode;
     EndpointJSON["latency_ms"] = Info.LatencyMS;

     if (!Info.Error.empty())
     {
          EndpointJSON["error"] = Info.Error;
     }

     return EndpointJSON;
}

static nlohmann::json BuildLinksNodesJSON(const std::vector<std::string> &Nodes, bool PingNodes)
{
     nlohmann::json NodesArray = nlohmann::json::array();

     if (!Instance || !Instance->Config)
     {
          return NodesArray;
     }

     for (const auto &RawNode : Nodes)
     {
          LinkEndpointInfo Info = HealthBuildEndpointInfo(RawNode);
          HealthProbeEndpoint(Info, PingNodes);
          NodesArray.push_back(HealthEndpointInfoToJSON(Info));
     }

     return NodesArray;
}

static nlohmann::json BuildSocketIOStatsJSON()
{
     SocketEngine::IOStats IOStatsVal = SocketEngine::GetIOStats();

     nlohmann::json IOJSON;
     IOJSON["total_bytes_processed"] = IOStatsVal.TotalBytesProcessed;
     IOJSON["active_connections"] = IOStatsVal.ActiveConnections;

     return IOJSON;
}

static uint64_t HealthReadCPUUsagePercent()
{
     std::ifstream StatFileStream("/proc/stat");
     std::string LineContent;

     if (!std::getline(StatFileStream, LineContent))
     {
          return 0;
     }

     std::istringstream Iss(LineContent);
     std::string CPULabel;
     uint64_t UserVal = 0;
     uint64_t NiceVal = 0;
     uint64_t SystemVal = 0;
     uint64_t IdleVal = 0;
     uint64_t IOWaitVal = 0;
     uint64_t IRQVal = 0;
     uint64_t SoftIRQVal = 0;
     uint64_t StealVal = 0;

     Iss >> CPULabel >> UserVal >> NiceVal >> SystemVal >> IdleVal >> IOWaitVal >> IRQVal >> SoftIRQVal >> StealVal;

     if (CPULabel != "cpu")
     {
          return 0;
     }

     const uint64_t TotalIdleTime = IdleVal + IOWaitVal;
     const uint64_t TotalNonIdleTime = UserVal + NiceVal + SystemVal + IRQVal + SoftIRQVal + StealVal;
     const uint64_t TotalTime = TotalIdleTime + TotalNonIdleTime;

     static uint64_t PrevTotalTime = 0;
     static uint64_t PrevIdleTime = 0;
     static uint64_t LastCPUPercent = 0;

     if (PrevTotalTime > 0 && TotalTime > PrevTotalTime)
     {
          const uint64_t TotalDelta = TotalTime - PrevTotalTime;
          const uint64_t IdleDelta = TotalIdleTime - PrevIdleTime;

          if (TotalDelta > 0)
          {
               LastCPUPercent = 100 - (IdleDelta * 100 / TotalDelta);
          }
     }

     PrevTotalTime = TotalTime;
     PrevIdleTime = TotalIdleTime;

     return LastCPUPercent;
}

static uint64_t HealthReadMemoryUsageBytes()
{
     std::ifstream MemInfoFileStream("/proc/meminfo");
     std::string LineContent;
     uint64_t TotalMemoryValue = 0;
     uint64_t AvailableMemoryValue = 0;

     while (std::getline(MemInfoFileStream, LineContent))
     {
          if (LineContent.find("MemTotal:") == 0)
          {
               std::istringstream Iss(LineContent);
               std::string LabelStr;
               std::string UnitStr;
               uint64_t Value = 0;
               Iss >> LabelStr >> Value >> UnitStr;
               TotalMemoryValue = Value * 1024;
          }
          else if (LineContent.find("MemAvailable:") == 0)
          {
               std::istringstream Iss(LineContent);
               std::string LabelStr;
               std::string UnitStr;
               uint64_t Value = 0;
               Iss >> LabelStr >> Value >> UnitStr;
               AvailableMemoryValue = Value * 1024;
          }
     }

     if (TotalMemoryValue == 0 || AvailableMemoryValue > TotalMemoryValue)
     {
          return 0;
     }

     return TotalMemoryValue - AvailableMemoryValue;
}

static bool HealthNormalizeEndpointValue(std::string &Endpoint, std::string &OutError)
{
     Endpoint = HealthTrimWhitespace(Endpoint);

     if (Endpoint.empty())
     {
          OutError = "Endpoint is empty";
          return false;
     }

     LinkEndpointInfo Info = HealthBuildEndpointInfo(Endpoint);
     if (!Info.IsValid)
     {
          OutError = Info.Error.empty() ? "Invalid endpoint format" : Info.Error;
          return false;
     }

     Endpoint = Info.NormalizedEndpoint;
     return true;
}

static HttpResponse BuildJSONResponse(Status StatusVal, const nlohmann::json &Body)
{
     HttpResponse Response(StatusVal, StatusText(StatusVal), "application/json");
     Response.Body = Body.dump();
     return Response;
}

static HttpResponse BuildLinksErrorResponse(Status StatusVal,
                                            const std::string &Error,
                                            const std::string &Message = std::string())
{
     nlohmann::json Body;
     Body["error"] = Error;

     if (!Message.empty())
     {
          Body["message"] = Message;
     }

     return BuildJSONResponse(StatusVal, Body);
}

static nlohmann::json BuildLinksResponseBody(bool PingNodes)
{
     nlohmann::json LinksJSON;
     LinksJSON["status"] = "ok";
     LinksJSON["timestamp"] = Instance ? Instance->NowMs() : static_cast<long long>(time(nullptr) * 1000);

     if (Instance && Instance->Config)
     {
          LinksJSON["enabled"] = Instance->Config->GetDistributedSearchEnabled();
          LinksJSON["mode"] = Instance->Config->GetDistributedSearchMode();
          LinksJSON["timeout_ms"] = Instance->Config->GetDistributedSearchTimeoutMS();
          LinksJSON["replication_enabled"] = Instance->Config->GetReplicationEnabled();
          LinksJSON["replication_mode"] = Instance->Config->GetReplicationMode();
          LinksJSON["replication_timeout_ms"] = Instance->Config->GetReplicationTimeoutMS();

          auto NodesArray = BuildLinksNodesJSON(Instance->Config->GetClusterNodes(), PingNodes);
          LinksJSON["nodes"] = NodesArray;
          LinksJSON["node_count"] = NodesArray.size();

          auto SlaveArray = BuildLinksNodesJSON(Instance->Config->GetSlaveNodes(), PingNodes);
          LinksJSON["slaves"] = SlaveArray;
          LinksJSON["slave_count"] = SlaveArray.size();
     }

     return LinksJSON;
}

static ModuleManager *GetModuleManager()
{
     return Instance ? Instance->Modules.get() : nullptr;
}

static bool ExtractLinkEndpoint(const HttpRequest &Request, std::string &OutEndpoint, std::string &OutError)
{
     auto It = Request.QueryParams.find("endpoint");
     if (It != Request.QueryParams.end() && !It->second.empty())
     {
          OutEndpoint = It->second;
          return HealthNormalizeEndpointValue(OutEndpoint, OutError);
     }

     It = Request.QueryParams.find("node");
     if (It != Request.QueryParams.end() && !It->second.empty())
     {
          OutEndpoint = It->second;
          return HealthNormalizeEndpointValue(OutEndpoint, OutError);
     }

     if (Request.Body.empty())
     {
          OutError = "Request body is empty";
          return false;
     }

     try
     {
          auto BodyJSON = nlohmann::json::parse(Request.Body);

          if (BodyJSON.is_string())
          {
               OutEndpoint = BodyJSON.get<std::string>();
               return HealthNormalizeEndpointValue(OutEndpoint, OutError);
          }

          if (!BodyJSON.is_object())
          {
               OutError = "Invalid JSON body";
               return false;
          }

          if (BodyJSON.contains("endpoint") && BodyJSON["endpoint"].is_string())
          {
               OutEndpoint = BodyJSON["endpoint"].get<std::string>();
               return HealthNormalizeEndpointValue(OutEndpoint, OutError);
          }

          if (BodyJSON.contains("node") && BodyJSON["node"].is_string())
          {
               OutEndpoint = BodyJSON["node"].get<std::string>();
               return HealthNormalizeEndpointValue(OutEndpoint, OutError);
          }

          if (BodyJSON.contains("host") && BodyJSON["host"].is_string())
          {
               std::string Host = BodyJSON["host"].get<std::string>();
               int Port = 9200;
               if (BodyJSON.contains("port"))
               {
                    try
                    {
                         Port = BodyJSON["port"].get<int>();
                    }
                    catch (...)
                    {
                         OutError = "Invalid port";
                         return false;
                    }
               }
               if (Host.empty() || Port <= 0)
               {
                    OutError = "Invalid host or port";
                    return false;
               }
               OutEndpoint = Host + ":" + std::to_string(Port);
               return HealthNormalizeEndpointValue(OutEndpoint, OutError);
          }

          OutError = "Missing endpoint";
          return false;
     }
     catch (const std::exception &E)
     {
          OutError = std::string("Invalid JSON: ") + E.what();
          return false;
     }
}

static bool HealthSendPingRequest(const std::string &Host,
                                  int Port,
                                  int TimeoutMS,
                                  int *OutStatusCode,
                                  double *OutLatencyMS,
                                  std::string *OutError)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("links", "Ping sent to " + Host + ":" + std::to_string(Port) + " (/health).");
     }

     if (OutStatusCode)
     {
          *OutStatusCode = 0;
     }
     if (OutLatencyMS)
     {
          *OutLatencyMS = 0.0;
     }

     addrinfo Hints{};
     Hints.ai_family = AF_UNSPEC;
     Hints.ai_socktype = SOCK_STREAM;
     Hints.ai_protocol = IPPROTO_TCP;

     addrinfo *Res = nullptr;
     const std::string PortStr = std::to_string(Port);
     if (getaddrinfo(Host.c_str(), PortStr.c_str(), &Hints, &Res) != 0 || !Res)
     {
          if (OutError)
          {
               *OutError = "Failed to resolve host";
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
               *OutError = "Failed to connect";
          }
          return false;
     }

     auto Start = Now();
     std::ostringstream Req;
     Req << "GET /health HTTP/1.1\r\n";
     Req << "Host: " << Host << ":" << Port << "\r\n";
     Req << "Connection: close\r\n";
     Req << "Accept: application/json\r\n";
     Req << "X-HLQ-Link-Ping: 1\r\n";
     Req << "\r\n";

     const std::string ReqStr = Req.str();
     ssize_t Sent = 0;
     while (Sent < static_cast<ssize_t>(ReqStr.size()))
     {
          ssize_t WriteCount = send(Sock, ReqStr.data() + Sent, ReqStr.size() - static_cast<size_t>(Sent), 0);
          if (WriteCount <= 0)
          {
               close(Sock);
               if (OutError)
               {
                    *OutError = "Failed to write request";
               }
               return false;
          }
          Sent += WriteCount;
     }

     std::string Response;
     char Buffer[2048];
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
     auto End = Now();
     if (OutLatencyMS)
     {
          *OutLatencyMS = std::chrono::duration<double, std::milli>(End - Start).count();
     }

     size_t LineEnd = Response.find("\r\n");
     if (LineEnd == std::string::npos)
     {
          if (OutError)
          {
               *OutError = "Invalid HTTP response";
          }
          return false;
     }

     std::string StatusLine = Response.substr(0, LineEnd);
     std::istringstream StatusSS(StatusLine);
     std::string HttpVersion;
     int StatusCode = 0;
     StatusSS >> HttpVersion >> StatusCode;
     if (StatusCode <= 0)
     {
          if (OutError)
          {
               *OutError = "Missing status code";
          }
          return false;
     }

     if (OutStatusCode)
     {
          *OutStatusCode = StatusCode;
     }

     if (StatusCode >= 200 && StatusCode < 300)
     {
          size_t HeaderEnd = Response.find("\r\n\r\n");
          if (HeaderEnd == std::string::npos)
          {
               if (OutError)
               {
                    *OutError = "Invalid HTTP response body";
               }
               return false;
          }

          const std::string ResponseBody = Response.substr(HeaderEnd + 4);

          if (!HealthValidateRemoteModules(ResponseBody, OutError))
          {
               return false;
          }
     }

     if (Instance && Instance->Logs)
     {
          std::ostringstream LatencyStream;
          LatencyStream << std::fixed << std::setprecision(1) << (OutLatencyMS ? *OutLatencyMS : 0.0);
          Instance->Logs->Normal("links", "Ping response from " + Host + ":" + std::to_string(Port) + " status=" + std::to_string(StatusCode) + " latency_ms=" + LatencyStream.str() + ".");
     }
     return true;
}

static std::vector<std::string> HealthGetLocalLoadedModules()
{
     if (!Instance || !Instance->Modules)
     {
          return {};
     }

     std::vector<std::string> Modules = Instance->Modules->GetLoadedModuleNames();
     std::sort(Modules.begin(), Modules.end());
     Modules.erase(std::unique(Modules.begin(), Modules.end()), Modules.end());
     return Modules;
}

static bool HealthValidateRemoteModules(const std::string &ResponseBody, std::string *OutError)
{
     const std::vector<std::string> LocalModules = HealthGetLocalLoadedModules();

     try
     {
          nlohmann::json Root = nlohmann::json::parse(ResponseBody);

          if (!Root.contains("loaded_modules") || !Root["loaded_modules"].is_array())
          {
               if (OutError)
               {
                    *OutError = "Remote /health response is missing loaded_modules";
               }
               return false;
          }

          std::vector<std::string> RemoteModules;
          for (const auto &ModuleName : Root["loaded_modules"])
          {
               if (ModuleName.is_string())
               {
                    RemoteModules.push_back(ModuleName.get<std::string>());
               }
          }

          std::sort(RemoteModules.begin(), RemoteModules.end());
          RemoteModules.erase(std::unique(RemoteModules.begin(), RemoteModules.end()), RemoteModules.end());

          if (RemoteModules == LocalModules)
          {
               return true;
          }

          std::vector<std::string> MissingOnRemote;
          std::vector<std::string> ExtraOnRemote;

          for (const auto &ModuleName : LocalModules)
          {
               if (!std::binary_search(RemoteModules.begin(), RemoteModules.end(), ModuleName))
               {
                    MissingOnRemote.push_back(ModuleName);
               }
          }

          for (const auto &ModuleName : RemoteModules)
          {
               if (!std::binary_search(LocalModules.begin(), LocalModules.end(), ModuleName))
               {
                    ExtraOnRemote.push_back(ModuleName);
               }
          }

          if (OutError)
          {
               std::ostringstream ErrorStream;
               ErrorStream << "Module mismatch";

               if (!MissingOnRemote.empty())
               {
                    ErrorStream << " missing_remote=[";
                    for (size_t I = 0; I < MissingOnRemote.size(); ++I)
                    {
                         if (I)
                         {
                              ErrorStream << ",";
                         }
                         ErrorStream << MissingOnRemote[I];
                    }
                    ErrorStream << "]";
               }

               if (!ExtraOnRemote.empty())
               {
                    ErrorStream << " extra_remote=[";
                    for (size_t I = 0; I < ExtraOnRemote.size(); ++I)
                    {
                         if (I)
                         {
                              ErrorStream << ",";
                         }
                         ErrorStream << ExtraOnRemote[I];
                    }
                    ErrorStream << "]";
               }

               *OutError = ErrorStream.str();
          }

          return false;
     }
     catch (const std::exception &)
     {
          if (OutError)
          {
               *OutError = "Failed to parse remote /health response";
          }
          return false;
     }
}
/* HandlePing responds to ping request. */

HttpResponse SearchAPI::HandlePing(const HttpRequest &Request)
{
     (void)Request;

     long long MSSinceEpoch = Instance ? Instance->NowMs() : time(nullptr) * 1000;

     nlohmann::json PingJSON;
     PingJSON["status"] = "ok";
     PingJSON["timestamp"] = MSSinceEpoch;

     bool AuthEnabled = false;
     bool DemoMode = false;
     std::string DemoMessage;
     if (Instance && Instance->Users)
     {
          AuthEnabled = Instance->Users->IsAuthEnabled();
     }
     ModuleManager *Modules = GetModuleManager();

     if (Modules)
     {
          DemoMode = Modules->IsDemoModeEnabled();
          DemoMessage = Modules->GetDemoModeMessage();
     }

     PingJSON["auth_enabled"] = AuthEnabled;
     PingJSON["auth_required"] = AuthEnabled;
     PingJSON["demo_mode"] = DemoMode;
     PingJSON["readonly_mode"] = DemoMode;
     if (!DemoMessage.empty())
     {
          PingJSON["demo_message"] = DemoMessage;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = PingJSON.dump();

     return Response;
}

/* HandleHealth returns system health information. */

HttpResponse SearchAPI::HandleHealth(const HttpRequest &Request)
{
     auto HeaderEqualsTruthy = [&](const std::string &Name) -> bool
     {
          for (const auto &Pair : Request.Headers)
          {
               std::string Key = Pair.first;
               std::string Value = Pair.second;
               std::transform(Key.begin(), Key.end(), Key.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               std::transform(Value.begin(), Value.end(), Value.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (Key == Name && (Value == "1" || Value == "true" || Value == "yes" || Value == "on"))
               {
                    return true;
               }
          }
          return false;
     };

     if (HeaderEqualsTruthy("x-hlq-link-ping") && Instance && Instance->Logs)
     {
          const std::string Remote = Request.RemoteAddress.empty() ? std::string("unknown") : Request.RemoteAddress;
          Instance->Logs->Normal("links", "Ping received from " + Remote + ":" + std::to_string(Request.RemotePort) + " on /health.");
     }

     nlohmann::json HealthJSON;

     bool HealthDegraded = false;
     std::string HealthReason;
     bool AuthEnabled = false;
     bool DemoMode = false;
     std::string DemoMessage;

     ModuleManager *Modules = GetModuleManager();

     if (Instance)
     {
          HealthDegraded = Instance->StatsVal.IsHealthDegraded();
          if (HealthDegraded)
          {
               HealthReason = Instance->StatsVal.GetHealthDegradedReason();
          }
          if (Instance->Users)
          {
               AuthEnabled = Instance->Users->IsAuthEnabled();
          }
     }

     if (Modules)
     {
          DemoMode = Modules->IsDemoModeEnabled();
          DemoMessage = Modules->GetDemoModeMessage();
     }

     HealthJSON["status"] = HealthDegraded ? "degraded" : "ok";
     HealthJSON["engine"] = SOCKETENGINE_NAME;
     HealthJSON["socket_engine"] = SOCKETENGINE_NAME;
     HealthJSON["server"] = "hlquery";
     HealthJSON["version"] = "1.0";
     HealthJSON["health_degraded"] = HealthDegraded;
     HealthJSON["auth_enabled"] = AuthEnabled;
     HealthJSON["auth_required"] = AuthEnabled;
     HealthJSON["demo_mode"] = DemoMode;
     HealthJSON["readonly_mode"] = DemoMode;
     if (!DemoMessage.empty())
     {
          HealthJSON["demo_message"] = DemoMessage;
     }
     HealthJSON["loaded_modules"] = nlohmann::json::array();
     if (Modules)
     {
          HealthJSON["loaded_modules"] = Modules->GetLoadedModuleNames();
     }
     if (!HealthReason.empty())
     {
          HealthJSON["health_reason"] = HealthReason;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = HealthJSON.dump();

     return Response;
}

HttpResponse SearchAPI::HandleReady(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json ReadyJSON;
     ReadyJSON["server"] = "hlquery";
     ReadyJSON["ready"] = true;
     ReadyJSON["status"] = "ready";

     bool IsReady = IsInitialized();
     bool IsLoading = false;
     bool SyncInProgress = false;
     if (Instance)
     {
          SyncInProgress = Instance->IsSyncInProgress();

          for (auto *ServerVal : Instance->HTTPServers)
          {
               if (ServerVal && ServerVal->IsLoading())
               {
                    IsLoading = true;
                    break;
               }
          }
     }

     if (!IsReady || IsLoading || SyncInProgress)
     {
          ReadyJSON["ready"] = false;
          ReadyJSON["status"] = "not_ready";
          ReadyJSON["initialized"] = IsReady;
          ReadyJSON["loading"] = IsLoading;
          ReadyJSON["sync_in_progress"] = SyncInProgress;
          ReadyJSON["listeners_configured"] = Instance ? Instance->GetConfiguredListenerCount() : 0;
          ReadyJSON["listeners_started"] = Instance ? Instance->GetStartedListenerCount() : 0;
          ReadyJSON["listeners_skipped"] = Instance ? Instance->GetSkippedListenerCount() : 0;
          ReadyJSON["listener_last_error"] = Instance ? Instance->GetLastListenerError() : std::string();

          HttpResponse Response(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          Response.Body = ReadyJSON.dump();
          return Response;
     }

     ReadyJSON["initialized"] = true;
     ReadyJSON["loading"] = false;
     ReadyJSON["sync_in_progress"] = false;
     ReadyJSON["listeners_configured"] = Instance ? Instance->GetConfiguredListenerCount() : 0;
     ReadyJSON["listeners_started"] = Instance ? Instance->GetStartedListenerCount() : 0;
     ReadyJSON["listeners_skipped"] = Instance ? Instance->GetSkippedListenerCount() : 0;
     ReadyJSON["listener_last_error"] = Instance ? Instance->GetLastListenerError() : std::string();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ReadyJSON.dump();
     return Response;
}

/* HandleMetrics returns system metrics. */

HttpResponse SearchAPI::HandleMetrics(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json MetricsJSON;

     MetricsJSON["status"] = "ok";

     if (Instance)
     {
          auto State = Instance->StatsVal.GetStartupState();

          MetricsJSON["startup"] = nlohmann::json::object();
          MetricsJSON["startup"]["collections_loaded"] = State.CollectionsLoadedCount;
          MetricsJSON["startup"]["lazy_loading_fallback"] = State.LazyLoadingFallback;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = MetricsJSON.dump();

     return Response;
}

/* HandleEtc returns protocol codes. */

HttpResponse SearchAPI::HandleEtc(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json ProtocolCodes;

     ProtocolCodes["protocol_name"] = "hlquery";
     ProtocolCodes["version"] = 1;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = ProtocolCodes.dump(2);

     return Response;
}

/* HandleStats returns system statistics. */

HttpResponse SearchAPI::HandleStats(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json StatsJSON;

     if (Instance)
     {
          long long MSSinceEpoch = Instance->NowMs();
          time_t UptimeVal = time(nullptr) - Instance->StatsVal.GetStartupTime();
          bool AuthEnabled = false;
          bool DemoMode = false;
          std::string DemoMessage;

          if (Instance->Users)
          {
               AuthEnabled = Instance->Users->IsAuthEnabled();
          }

          if (Instance->Modules)
          {
               DemoMode = Instance->Modules->IsDemoModeEnabled();
               DemoMessage = Instance->Modules->GetDemoModeMessage();
          }

          auto &storage = HybridStorageManager::GetInstance();
          std::vector<std::string> collections = storage.ListCollections();

          StatsJSON["timestamp_ms"] = MSSinceEpoch;
          StatsJSON["uptime_seconds"] = UptimeVal;
          StatsJSON["collections_total"] = collections.size();
          StatsJSON["auth_enabled"] = AuthEnabled;
          StatsJSON["auth_required"] = AuthEnabled;
          StatsJSON["demo_mode"] = DemoMode;
          StatsJSON["readonly_mode"] = DemoMode;
          StatsJSON["io"] = BuildSocketIOStatsJSON();
          if (!DemoMessage.empty())
          {
               StatsJSON["demo_message"] = DemoMessage;
          }

          if (Instance->Config)
          {
               nlohmann::json ServerJSON;
               ServerJSON["name"] = Instance->Config->GetServerName();
               ServerJSON["id"] = Instance->Config->GetServerId();
               ServerJSON["uptime_seconds"] = UptimeVal;
               ServerJSON["cpu_usage_percent"] = HealthReadCPUUsagePercent();
               ServerJSON["memory_usage_bytes"] = HealthReadMemoryUsageBytes();
               StatsJSON["server"] = ServerJSON;
          }

          if (Instance->Database)
          {
               auto db_stats = Instance->Database->GetRocksDBStats();

               nlohmann::json LSMJSON;

               if (db_stats.total_db_size > 0)
               {
                    LSMJSON["total_size"] = db_stats.total_db_size;
                    LSMJSON["rocksdb_size"] = db_stats.total_db_size;
               }

               if (db_stats.memtable_size > 0)
               {
                    LSMJSON["memtable_size"] = db_stats.memtable_size;
               }

               if (db_stats.num_sst_files > 0)
               {
                    LSMJSON["sstable_count"] = db_stats.num_sst_files;
               }

               if (!LSMJSON.empty())
               {
                    StatsJSON["lsm"] = LSMJSON;
               }
          }

          if (Instance->SearchIndex)
          {
               size_t total_documents = 0;
               size_t indexed_documents = 0;
               size_t pending_collections = 0;
               size_t active_collections = 0;
               size_t unloaded_collections = 0;
               size_t partial_collections = 0;
               std::vector<std::string> active_list;
               std::vector<std::string> unloaded_list;
               std::vector<std::string> partial_list;

               for (const auto &name : collections)
               {
                    size_t metadata_count = storage.GetCollectionDocumentCount(name);
                    size_t indexed_count = Instance->SearchIndex->GetDocumentCount(name);

                    size_t collection_total = metadata_count;

                    if (collection_total == 0 && indexed_count > 0)
                    {
                         collection_total = indexed_count;
                    }

                    total_documents += collection_total;
                    indexed_documents += std::min(collection_total, indexed_count);

                    bool currently_indexing = storage.IsCollectionIndexing(name);
                    bool not_loaded = (collection_total > 0 && indexed_count == 0);
                    bool partially_loaded = (collection_total > 0 && indexed_count > 0 && indexed_count < collection_total);

                    if (currently_indexing)
                    {
                         active_collections++;
                         if (active_list.size() < 12)
                         {
                              active_list.push_back(name);
                         }
                    }
                    else if (partially_loaded)
                    {
                         partial_collections++;
                         if (partial_list.size() < 12)
                         {
                              partial_list.push_back(name);
                         }
                    }
                    else if (not_loaded)
                    {
                         unloaded_collections++;
                         if (unloaded_list.size() < 12)
                         {
                              unloaded_list.push_back(name);
                         }
                    }
               }

               pending_collections = active_collections + unloaded_collections + partial_collections;

               double percent_complete = 100.0;

               if (total_documents > 0)
               {
                    percent_complete = (static_cast<double>(indexed_documents) / static_cast<double>(total_documents)) * 100.0;
                    if (percent_complete < 0.0)
                    {
                         percent_complete = 0.0;
                    }
                    else if (percent_complete > 100.0)
                    {
                         percent_complete = 100.0;
                    }
               }

               nlohmann::json indexing_json;
               indexing_json["in_progress"] = active_collections > 0;
               indexing_json["needs_loading"] = pending_collections > 0;
               indexing_json["percent_complete"] = percent_complete;
               indexing_json["documents_total"] = total_documents;
               indexing_json["documents_indexed"] = indexed_documents;
               indexing_json["collections_indexing"] = active_list;
               indexing_json["collections_partial"] = partial_list;
               indexing_json["collections_pending"] = pending_collections;
               indexing_json["collections_indexing_count"] = active_collections;
               indexing_json["collections_partial_count"] = partial_collections;
               indexing_json["collections_indexed"] = collections.size() > pending_collections ? (collections.size() - pending_collections) : 0;

               StatsJSON["indexing"] = indexing_json;
          }

          StatsJSON["storage"]["post_delete_cleanup_failures_total"] = storage.GetPostDeleteCleanupFailures();
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = StatsJSON.dump();

     return Response;
}

/* HandleMetricsHistory returns historical metrics. */

HttpResponse SearchAPI::HandleMetricsHistory(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json ResultJSON;

     if (Instance)
     {
          decltype(Instance->Metrics->CPUMetrics.GetPoints()) CPUPoints;

          if (Instance && Instance->Metrics)
          {
               CPUPoints = Instance->Metrics->CPUMetrics.GetPoints();
          }
          nlohmann::json CPUData = nlohmann::json::array();

          for (const auto &Point : CPUPoints)
          {
               CPUData.push_back({{"timestamp", std::chrono::system_clock::to_time_t(Point.Timestamp)},
                                  {"value", Point.Value}});
          }

          ResultJSON["cpu"] = CPUData;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = ResultJSON.dump();

     return Response;
}

/* HandleCache returns cache statistics. */

HttpResponse SearchAPI::HandleCache(const HttpRequest &Request)
{
     (void)Request;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"status\":\"ok\"}";

     return Response;
}

/* HandleConnections returns active connections. */

HttpResponse SearchAPI::HandleConnections(const HttpRequest &Request)
{
     (void)Request;

     auto IOStats = SocketEngine::GetIOStats();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"active_connections\":" + std::to_string(IOStats.ActiveConnections) + "}";

     return Response;
}

/* HandleRocksDB returns RocksDB statistics. */

HttpResponse SearchAPI::HandleRocksDB(const HttpRequest &Request)
{
     (void)Request;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"status\":\"ok\"}";

     return Response;
}

/* HandleStatus returns overall system status. */

HttpResponse SearchAPI::HandleStatus(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json StatusJSON;

     StatusJSON["status"] = "ok";
     StatusJSON["timestamp"] = GetCurrentTimestamp();

     try
     {
          HttpResponse StatsResponse = HandleStats(Request);
          StatusJSON["stats"] = nlohmann::json::parse(StatsResponse.Body);
     }
     catch (...)
     {
          nlohmann::json FallbackStatsJSON;
          FallbackStatsJSON["io"] = BuildSocketIOStatsJSON();
          StatusJSON["stats"] = FallbackStatsJSON;
     }

     if (Instance && Instance->Config)
     {
          nlohmann::json DistributedJSON;
          DistributedJSON["enabled"] = Instance->Config->GetDistributedSearchEnabled();
          DistributedJSON["mode"] = Instance->Config->GetDistributedSearchMode();
          DistributedJSON["timeout_ms"] = Instance->Config->GetDistributedSearchTimeoutMS();

          nlohmann::json NodesArray = BuildLinksNodesJSON(Instance->Config->GetClusterNodes(), true);

          DistributedJSON["nodes"] = NodesArray;
          DistributedJSON["node_count"] = NodesArray.size();

          nlohmann::json ReplicationJSON;
          nlohmann::json SlaveArray = BuildLinksNodesJSON(Instance->Config->GetSlaveNodes(), true);
          ReplicationJSON["slaves"] = SlaveArray;
          ReplicationJSON["slave_count"] = SlaveArray.size();
          ReplicationJSON["enabled"] = Instance->Config->GetReplicationEnabled();
          ReplicationJSON["mode"] = Instance->Config->GetReplicationMode();
          ReplicationJSON["timeout_ms"] = Instance->Config->GetReplicationTimeoutMS();
          ReplicationStatusSnapshot ReplicationStatus = GetReplicationStatusSnapshot();
          ReplicationJSON["requests_attempted"] = ReplicationStatus.RequestsAttempted;
          ReplicationJSON["requests_succeeded"] = ReplicationStatus.RequestsSucceeded;
          ReplicationJSON["requests_failed"] = ReplicationStatus.RequestsFailed;
          ReplicationJSON["replica_acks"] = ReplicationStatus.ReplicaAcks;
          if (!ReplicationStatus.LastError.empty())
          {
               ReplicationJSON["last_error"] = ReplicationStatus.LastError;
               ReplicationJSON["last_error_ts"] = ReplicationStatus.LastErrorTimestampMS;
          }
          StatusJSON["replication"] = ReplicationJSON;
          StatusJSON["distributed_search"] = DistributedJSON;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = StatusJSON.dump();

     return Response;
}

HttpResponse SearchAPI::HandleSearchConfig(const HttpRequest &Request)
{
     (void)Request;

     if (!Instance || !Instance->Config)
     {
          nlohmann::json ErrorJSON;
          ErrorJSON["error"] = "server_not_ready";
          ErrorJSON["message"] = "Search configuration is not available yet.";

          HttpResponse ErrorResponse(Status::SERVICE_UNAVAILABLE, StatusText(Status::SERVICE_UNAVAILABLE), "application/json");
          ErrorResponse.Body = ErrorJSON.dump();
          return ErrorResponse;
     }

     ServerConfig *Config = Instance->Config.get();

     nlohmann::json ConfigJSON;
     ConfigJSON["algorithm"] = Config->GetSearchAlgorithm();
     ConfigJSON["default_ranking"] = Config->GetDefaultRanking();
     ConfigJSON["k1"] = Config->GetRankingK1();
     ConfigJSON["b"] = Config->GetRankingB();
     ConfigJSON["delta"] = Config->GetRankingDelta();
     ConfigJSON["max_query_length"] = Config->GetQuerySettingsMaxQueryLength();
     ConfigJSON["max_query_terms"] = Config->GetQuerySettingsMaxQueryTerms();
     ConfigJSON["min_query_length"] = Config->GetQuerySettingsMinQueryLength();
     ConfigJSON["enable_stemming"] = Config->GetQuerySettingsEnableStemming();
     ConfigJSON["enable_synonyms"] = Config->GetQuerySettingsEnableSynonyms();
     ConfigJSON["enable_fuzzy"] = Config->GetQuerySettingsEnableFuzzy();
     ConfigJSON["fuzzy_max_distance"] = Config->GetQuerySettingsFuzzyMaxDistance();
     ConfigJSON["require_exact_identifier_tokens"] = Config->GetQuerySettingsRequireExactIdentifierTokens();
     ConfigJSON["default_limit"] = Config->GetLimitsDefaultLimit();
     ConfigJSON["max_limit"] = Config->GetLimitsMaxLimit();
     ConfigJSON["min_limit"] = Config->GetLimitsMinLimit();
     ConfigJSON["max_offset"] = Config->GetLimitsMaxOffset();
     ConfigJSON["idf_cache"] = Config->GetPerformanceIdfCache();
     ConfigJSON["doc_length_cache"] = Config->GetPerformanceDocLengthCache();
     ConfigJSON["max_cache_size_mb"] = Config->GetPerformanceMaxCacheSizeMb();
     ConfigJSON["cache_ttl_seconds"] = Config->GetPerformanceCacheTtlSeconds();
     ConfigJSON["query_timeout_ms"] = Config->GetTimeoutsQueryTimeoutMs();
     ConfigJSON["indexing_timeout_ms"] = Config->GetTimeoutsIndexingTimeoutMs();
     ConfigJSON["max_candidates"] = Config->GetTimeoutsMaxCandidates();
     ConfigJSON["min_candidates"] = Config->GetTimeoutsMinCandidates();
     ConfigJSON["min_score_threshold"] = Config->GetScoringMinScoreThreshold();
     ConfigJSON["normalize_scores"] = Config->GetScoringNormalizeScores();
     ConfigJSON["score_precision"] = Config->GetScoringScorePrecision();
     ConfigJSON["enable_score_explanation"] = Config->GetScoringEnableScoreExplanation();
     ConfigJSON["enable_wildcards"] = Config->GetIndexingEnableWildcards();
     ConfigJSON["enable_prefix_matching"] = Config->GetIndexingEnablePrefixMatching();
     ConfigJSON["store_positions"] = Config->GetIndexingStorePositions();
     ConfigJSON["store_offsets"] = Config->GetIndexingStoreOffsets();
     ConfigJSON["track_total_hits"] = Config->GetSearchOptionsTrackTotalHits();
     ConfigJSON["track_scores"] = Config->GetSearchOptionsTrackScores();
     ConfigJSON["request_cache"] = Config->GetSearchOptionsRequestCache();
     ConfigJSON["allow_partial_search_results"] = Config->GetSearchOptionsAllowPartialSearchResults();
     ConfigJSON["log_queries"] = Config->GetLoggingLogQueries();
     ConfigJSON["log_slow_queries"] = Config->GetLoggingLogSlowQueries();
     ConfigJSON["slow_query_threshold_ms"] = Config->GetLoggingSlowQueryThresholdMs();
     ConfigJSON["log_level"] = Config->GetLoggingLogLevel();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ConfigJSON.dump();
     return Response;
}

/* HandleLinksList returns configured cluster links. */

HttpResponse SearchAPI::HandleLinksList(const HttpRequest &Request)
{
     (void)Request;

     const bool ping_nodes = HealthParseBoolParam(Request.QueryParams, "ping", false);
     return BuildJSONResponse(Status::OK, BuildLinksResponseBody(ping_nodes));
}

/* HandleLinksPing pings configured cluster links. */

HttpResponse SearchAPI::HandleLinksPing(const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("links", "Received /links/ping request.");
     }

     return BuildJSONResponse(Status::OK, BuildLinksResponseBody(true));
}

/* HandleLinksConnect adds a cluster link at runtime. */

HttpResponse SearchAPI::HandleLinksConnect(const HttpRequest &Request)
{
     std::string Endpoint;
     std::string Error;
     if (!ExtractLinkEndpoint(Request, Endpoint, Error))
     {
          return BuildLinksErrorResponse(Status::BAD_REQUEST, "Invalid request", Error);
     }

     if (!Instance || !Instance->Config)
     {
          return BuildLinksErrorResponse(Status::INTERNAL_SERVER_ERROR, "Server not ready");
     }

     if (Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreLinksConnect, Endpoint, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     LinkEndpointInfo Info = HealthBuildEndpointInfo(Endpoint);
     if (!Info.IsValid)
     {
          return BuildLinksErrorResponse(Status::BAD_REQUEST, "Invalid request", Info.Error.empty() ? "Invalid endpoint format" : Info.Error);
     }

     if (!Info.IsLocal)
     {
          const int TimeoutMS = Instance->Config ? Instance->Config->GetDistributedSearchTimeoutMS() : 0;
          if (Instance->Logs)
          {
               Instance->Logs->Normal("links", "Preflight check for link " + Info.Host + ":" + std::to_string(Info.Port) + ".");
          }

          const bool ProbeOK = HealthSendPingRequest(Info.Host, Info.Port, TimeoutMS, &Info.StatusCode, &Info.LatencyMS, &Info.Error);
          if (!ProbeOK || Info.StatusCode < 200 || Info.StatusCode >= 300)
          {
               std::ostringstream Message;
               Message << "Preflight failed for " << Info.Host << ":" << Info.Port;
               if (Info.StatusCode > 0)
               {
                    Message << " status=" << Info.StatusCode;
               }
               if (!Info.Error.empty())
               {
                    Message << " error=" << Info.Error;
               }
               return BuildLinksErrorResponse(Status::BAD_REQUEST, "Failed to add link", Message.str());
          }
     }

     std::string AddError;
     if (!Instance->Config->AddClusterNode(Endpoint, &AddError))
     {
          return BuildLinksErrorResponse(Status::BAD_REQUEST, "Failed to add link", AddError);
     }

     FOREACH_MOD(OnLinksConnect, Endpoint, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return HandleLinksList(Request);
}

/* HandleLinksDisconnect removes a cluster link at runtime. */

HttpResponse SearchAPI::HandleLinksDisconnect(const HttpRequest &Request)
{
     std::string Endpoint;
     std::string Error;
     if (!ExtractLinkEndpoint(Request, Endpoint, Error))
     {
          return BuildLinksErrorResponse(Status::BAD_REQUEST, "Invalid request", Error);
     }

     if (!Instance || !Instance->Config)
     {
          return BuildLinksErrorResponse(Status::INTERNAL_SERVER_ERROR, "Server not ready");
     }

     if (Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreLinksDisconnect, Endpoint, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string RemoveError;
     if (!Instance->Config->RemoveClusterNode(Endpoint, &RemoveError))
     {
          return BuildLinksErrorResponse(Status::BAD_REQUEST, "Failed to remove link", RemoveError);
     }

     FOREACH_MOD(OnLinksDisconnect, Endpoint, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

     return HandleLinksList(Request);
}

/* HandleStartup returns startup information. */

HttpResponse SearchAPI::HandleStartup(const HttpRequest &Request)
{
     (void)Request;

     nlohmann::json StartupJSON;
     StartupJSON["loaded_modules"] = nlohmann::json::array();

     if (Instance)
     {
          auto State = Instance->StatsVal.GetStartupState();

          StartupJSON["collections_loaded"] = State.CollectionsLoadedCount;
          StartupJSON["sync_complete"] = State.SyncComplete;
          StartupJSON["listeners_configured"] = Instance->GetConfiguredListenerCount();
          StartupJSON["listeners_started"] = Instance->GetStartedListenerCount();
          StartupJSON["listeners_skipped"] = Instance->GetSkippedListenerCount();
          StartupJSON["listener_last_error"] = Instance->GetLastListenerError();

          if (Instance->Modules)
          {
               StartupJSON["loaded_modules"] = Instance->Modules->GetLoadedModuleNames();
          }
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = StartupJSON.dump();

     return Response;
}

/* HandleIntegrity performs data integrity check. */

HttpResponse SearchAPI::HandleIntegrity(const HttpRequest &Request)
{
     std::string collection_name;
     const auto collection_it = Request.QueryParams.find("collection");

     if (collection_it != Request.QueryParams.end())
     {
          collection_name = HealthTrimWhitespace(collection_it->second);
     }

     if (!collection_name.empty() && !HybridStorageManager::GetInstance().CollectionExists(collection_name))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     const IntegrityReport report = HybridStorageManager::GetInstance().CheckIntegrity(collection_name);

     nlohmann::json response_json;
     response_json["status"] = (report.CounterMismatches == 0 && report.IndexMismatches == 0) ? "ok" : "degraded";
     response_json["success"] = report.Success;
     response_json["collections_scanned"] = report.CollectionsScanned;
     response_json["counter_mismatches"] = report.CounterMismatches;
     response_json["index_mismatches"] = report.IndexMismatches;

     nlohmann::json collections_json = nlohmann::json::array();

     for (const auto &status : report.Collections)
     {
          nlohmann::json entry;
          entry["collection"] = status.Collection;
          entry["exists"] = status.CollectionExists;
          entry["metadata_count"] = status.MetadataCount;
          entry["actual_count"] = status.ActualCount;
          entry["metadata_match"] = status.MetadataMatch;
          entry["index_present"] = status.IndexPresent;
          entry["index_verified"] = status.IndexVerified;
          entry["indexed_count"] = status.IndexedCount;
          entry["index_match"] = status.IndexMatch;

          if (!status.Error.empty())
          {
               entry["note"] = status.Error;
          }

          collections_json.push_back(std::move(entry));
     }

     response_json["collections"] = std::move(collections_json);

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = response_json.dump();
     return Response;
}

/* HandleDocTotal returns total document count. */

HttpResponse SearchAPI::HandleDocTotal(const HttpRequest &Request)
{
     std::string prefix_filter;
     const auto prefix_it = Request.QueryParams.find("prefix");

     if (prefix_it != Request.QueryParams.end())
     {
          prefix_filter = HealthTrimWhitespace(prefix_it->second);
     }

     size_t TotalDocs = 0;
     auto CollectionsList = HybridStorageManager::GetInstance().ListCollections();
     size_t IncludedCollections = 0;

     for (const auto &Name : CollectionsList)
     {
          if (!prefix_filter.empty() && Name.rfind(prefix_filter, 0) != 0)
          {
               continue;
          }

          TotalDocs += HybridStorageManager::GetInstance().GetCollectionDocumentCount(Name);
          IncludedCollections++;
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     nlohmann::json DocTotalJSON;
     DocTotalJSON["doctotal"] = TotalDocs;
     DocTotalJSON["coltotal"] = prefix_filter.empty() ? CollectionsList.size() : IncludedCollections;
     if (!prefix_filter.empty())
     {
          DocTotalJSON["prefix"] = prefix_filter;
     }

     Response.Body = DocTotalJSON.dump();

     return Response;
}

/* HandleUpdateCounters triggers document counter update. */

HttpResponse SearchAPI::HandleUpdateCounters(const HttpRequest &Request)
{
     std::string prefix_filter;
     const auto prefix_it = Request.QueryParams.find("prefix");

     if (prefix_it != Request.QueryParams.end())
     {
          prefix_filter = HealthTrimWhitespace(prefix_it->second);
     }

     if (!prefix_filter.empty())
     {
          HybridStorageManager::GetInstance().UpdateCollectionCountersPrefix(prefix_filter, true);
     }
     else
     {
          HybridStorageManager::GetInstance().UpdateCollectionCounters(true);
     }

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     if (!prefix_filter.empty())
     {
          Response.Body = "{\"status\":\"ok\",\"prefix\":\"" + prefix_filter + "\"}";
     }
     else
     {
          Response.Body = "{\"status\":\"ok\"}";
     }

     return Response;
}

/* HandleDebugCounters returns debug counter information. */

HttpResponse SearchAPI::HandleDebugCounters(const HttpRequest &Request)
{
     (void)Request;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"status\":\"ok\"}";

     return Response;
}

/* HandleRepair triggers system repair. */

HttpResponse SearchAPI::HandleRepair(const HttpRequest &Request)
{
     if (Instance && Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreRepair, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     std::string collection_name;
     const auto collection_it = Request.QueryParams.find("collection");

     if (collection_it != Request.QueryParams.end())
     {
          collection_name = HealthTrimWhitespace(collection_it->second);
     }

     if (!collection_name.empty() && !HybridStorageManager::GetInstance().CollectionExists(collection_name))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::COLLECTION_NOT_FOUND, "Collection not found", "The specified collection does not exist.");
     }

     const bool rebuild_index = HealthParseBoolParam(Request.QueryParams, "rebuild_index", false) ||
                                HealthParseBoolParam(Request.QueryParams, "index", false);

     const IntegrityReport report = HybridStorageManager::GetInstance().RepairIntegrity(collection_name, rebuild_index);

     nlohmann::json response_json;
     response_json["status"] = report.Success ? "ok" : "partial";
     response_json["success"] = report.Success;
     response_json["rebuild_index"] = report.RebuildIndex;
     response_json["collections_scanned"] = report.CollectionsScanned;
     response_json["collections_repaired"] = report.CollectionsRepaired;
     response_json["counter_mismatches_before_repair"] = report.CounterMismatches;
     response_json["index_mismatches_before_repair"] = report.IndexMismatches;

     nlohmann::json collections_json = nlohmann::json::array();

     for (const auto &status : report.Collections)
     {
          nlohmann::json entry;
          entry["collection"] = status.Collection;
          entry["exists"] = status.CollectionExists;
          entry["metadata_count"] = status.MetadataCount;
          entry["actual_count"] = status.ActualCount;
          entry["metadata_match"] = status.MetadataMatch;
          entry["index_present"] = status.IndexPresent;
          entry["index_verified"] = status.IndexVerified;
          entry["indexed_count"] = status.IndexedCount;
          entry["index_match"] = status.IndexMatch;
          entry["index_rebuilt"] = status.IndexRebuilt;
          entry["reindexed_documents"] = status.ReindexedDocuments;

          if (!status.Error.empty())
          {
               entry["error"] = status.Error;
          }

          collections_json.push_back(std::move(entry));
     }

     response_json["collections"] = std::move(collections_json);

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = response_json.dump();

     if (report.Success)
     {
          FOREACH_MOD(OnRepair, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());
     }

     return Response;
}

/* HandleSelfCheck performs a self-check. */

HttpResponse SearchAPI::HandleSelfCheck(const HttpRequest &Request)
{
     (void)Request;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");

     Response.Body = "{\"status\":\"ok\",\"initialized\":" + std::string(IsInitialized() ? "true" : "false") + "}";

     return Response;
}
