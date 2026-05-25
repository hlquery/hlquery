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

#pragma once

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
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "api/searchapi.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/rfusion.h"
#include "search/cstore.h"
#include "search/storageengine.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/protocol.h"
#include "utils/wildcard.h"
#include "vendor/json/json.hpp"

/* BuildErrorResponse builds error responses with protocol codes. */

inline HttpResponse BuildErrorResponse(int HttpStatus, int ProtocolCode, const std::string &Error, const std::string &Message = "")
{
     (void)Error;

     HttpResponse Response(HttpStatus, StatusText(HttpStatus), "application/json");

     nlohmann::json ErrorJSON;

     /* Use code_text as the error field to avoid duplication - code_text is the canonical error name. */

     ErrorJSON["error"] = CodeText(ProtocolCode);

     if (!Message.empty())
     {
          ErrorJSON["message"] = Message;
     }

     ErrorJSON["code"] = ProtocolCode;

     ErrorJSON["code_text"] = CodeText(ProtocolCode);

     Response.Body = ErrorJSON.dump();

     return Response;
}

struct NodeEndpointParseOptions
{
     int DefaultPort = 9200;
     bool AllowEmptyPort = true;
};

inline std::string TrimNodeEndpointValue(const std::string &Value)
{
     size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

inline bool ParseSharedNodeEndpoint(const std::string &Raw,
                                    std::string &HostOut,
                                    int &PortOut,
                                    std::string *SchemeOut = nullptr,
                                    const NodeEndpointParseOptions &Options = NodeEndpointParseOptions())
{
     std::string Node = TrimNodeEndpointValue(Raw);
     if (Node.empty())

     {
          return false;
     }

     std::string Scheme;

     if (Node.rfind("http://", 0) == 0)
     {
          Scheme = "http";
          Node = Node.substr(7);
     }
     else if (Node.rfind("https://", 0) == 0)
     {
          Scheme = "https";
          Node = Node.substr(8);
     }

     size_t SlashPos = Node.find('/');

     if (SlashPos != std::string::npos)
     {
          Node = Node.substr(0, SlashPos);
     }

     std::string Host = Node;
     int Port = Options.DefaultPort;
     size_t ColonPos = Node.rfind(':');

     if (!Node.empty() && Node.front() == '[')
     {
          size_t BracketPos = Node.find(']');
          if (BracketPos == std::string::npos)
          {
               return false;
          }

          Host = Node.substr(1, BracketPos - 1);
          std::string Rest = TrimNodeEndpointValue(Node.substr(BracketPos + 1));

          if (!Rest.empty())
          {
               if (Rest.front() != ':')
               {
                    return false;
               }

               std::string PortStr = Rest.substr(1);
               if (PortStr.empty())
               {
                    if (!Options.AllowEmptyPort)
                    {
                         return false;
                    }
               }
               else
               {
                    int ParsedPort = 0;
                    auto [Ptr, EC] = std::from_chars(PortStr.data(), PortStr.data() + PortStr.size(), ParsedPort);
                    if (EC != std::errc() || Ptr != PortStr.data() + PortStr.size())
                    {
                         return false;
                    }
                    Port = ParsedPort;
               }
          }
     }
     else if (ColonPos != std::string::npos)
     {
          if (Node.find(':') != ColonPos)
          {
               Host = Node;
          }
          else
          {
               Host = Node.substr(0, ColonPos);
               std::string PortStr = Node.substr(ColonPos + 1);
               if (PortStr.empty())
               {
                    if (!Options.AllowEmptyPort)
                    {
                         return false;
                    }
               }
               else
               {
                    int ParsedPort = 0;
                    auto [Ptr, EC] = std::from_chars(PortStr.data(), PortStr.data() + PortStr.size(), ParsedPort);
                    if (EC != std::errc() || Ptr != PortStr.data() + PortStr.size())
                    {
                         return false;
                    }
                    Port = ParsedPort;
               }
          }
     }

     Host = TrimNodeEndpointValue(Host);

     if (Host.empty() || Port <= 0 || Port > 65535)
     {
          return false;
     }

     HostOut = Host;
     PortOut = Port;

     if (SchemeOut)
     {
          *SchemeOut = Scheme;
     }

     return true;
}
