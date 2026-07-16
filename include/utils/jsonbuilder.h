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

#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "api/httpserver.h"
#include "core/config.h"
#include "core/httpcodes.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

/* JSON Builder - Fluent API for building JSON objects and arrays. */

class CoreExport JsonBuilder
{
   public:
     JsonBuilder() : IsObjectVal(true), FirstItemVal(true)
     {
          JSONStream << "{";
     }

     explicit JsonBuilder(bool IsObject) : IsObjectVal(IsObject), FirstItemVal(true)
     {
          if (IsObjectVal)
          {
               JSONStream << "{";
          }
          else
          {
               JSONStream << "[";
          }
     }

     JsonBuilder &Add(const std::string &Key, const std::string &Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":\"" << EscapeJSONString(Value) << "\"";
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, const char *Value)
     {
          return Add(Key, std::string(Value));
     }

     JsonBuilder &Add(const std::string &Key, int Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << Value;
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, long long Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << Value;
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, unsigned long long Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << Value;
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, double Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << std::fixed << std::setprecision(6) << Value;
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, bool Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << (Value ? "true" : "false");
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, const nlohmann::json &Value)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << Value.dump();
          return *this;
     }

     JsonBuilder &Add(const std::string &Key, const JsonBuilder &Nested)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << Nested.ToString();
          return *this;
     }

     JsonBuilder &AddItem(const std::string &Value)
     {
          if (!IsObjectVal)
          {
               AddComma();
               JSONStream << "\"" << EscapeJSONString(Value) << "\"";
          }

          return *this;
     }

     JsonBuilder &AddItem(int Value)
     {
          if (!IsObjectVal)
          {
               AddComma();
               JSONStream << Value;
          }

          return *this;
     }

     JsonBuilder &AddItem(const JsonBuilder &Nested)
     {
          if (!IsObjectVal)
          {
               AddComma();
               JSONStream << Nested.ToString();
          }

          return *this;
     }

     JsonBuilder &AddItem(const nlohmann::json &Value)
     {
          if (!IsObjectVal)
          {
               AddComma();
               JSONStream << Value.dump();
          }

          return *this;
     }

     JsonBuilder &AddRaw(const std::string &Key, const std::string &RawJSON)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":" << RawJSON;
          return *this;
     }

     JsonBuilder Object(const std::string &Key)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":";
          return JsonBuilder(true);
     }

     JsonBuilder Array(const std::string &Key)
     {
          AddComma();
          JSONStream << "\"" << EscapeJSONString(Key) << "\":[";
          return JsonBuilder(false);
     }

     std::string ToString() const
     {
          if (IsObjectVal)
          {
               JSONStream << "}";
          }
          else
          {
               JSONStream << "]";
          }

          return JSONStream.str();
     }

     nlohmann::json ToJson()
     {
          return nlohmann::json::parse(ToString());
     }

   private:
     void AddComma()
     {
          if (!FirstItemVal)
          {
               JSONStream << ",";
          }

          FirstItemVal = false;
     }

     static std::string EscapeJSONString(const std::string &StrVal)
     {
          std::string Result;
          Result.reserve(StrVal.size() + 10);

          for (char C : StrVal)
          {
               switch (C)
               {
                    case '"':
                         Result += "\\\"";
                         break;
                    case '\\':
                         Result += "\\\\";
                         break;
                    case '\b':
                         Result += "\\b";
                         break;
                    case '\f':
                         Result += "\\f";
                         break;
                    case '\n':
                         Result += "\\n";
                         break;
                    case '\r':
                         Result += "\\r";
                         break;
                    case '\t':
                         Result += "\\t";
                         break;
                    default:
                         if (static_cast<unsigned char>(C) < 0x20)
                         {
                              std::ostringstream OSS;
                              OSS << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(C));
                              Result += OSS.str();
                         }
                         else
                         {
                              Result += C;
                         }

                         break;
               }
          }

          return Result;
     }

     mutable std::ostringstream JSONStream;
     bool IsObjectVal;
     bool FirstItemVal;
};

/* HTTP Response Builder - Fluent API for building HTTP responses with JSON bodies. */

class HttpResponseBuilder
{
   public:
     HttpResponseBuilder(int StatusCodeVal = 200, const std::string &StatusTextParam = "OK")
         : StatusCode(StatusCodeVal), StatusTextVal(StatusTextParam)
     {
          JSONBuilderPtr = std::make_unique<JsonBuilder>();
     }

     HttpResponseBuilder &Status(int CodeVal, const std::string &TextVal = "")
     {
          StatusCode = CodeVal;

          if (!TextVal.empty())
          {
               StatusTextVal = TextVal;
          }

          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, const std::string &Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, const char *Value)
     {
          JSONBuilderPtr->Add(Key, std::string(Value));
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, int Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, long long Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, unsigned long long Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, double Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, bool Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     HttpResponseBuilder &Add(const std::string &Key, const nlohmann::json &Value)
     {
          JSONBuilderPtr->Add(Key, Value);
          return *this;
     }

     JsonBuilder Object(const std::string &Key)
     {
          return JSONBuilderPtr->Object(Key);
     }

     JsonBuilder Array(const std::string &Key)
     {
          return JSONBuilderPtr->Array(Key);
     }

     HttpResponseBuilder &Header(const std::string &Key, const std::string &Value)
     {
          Headers[Key] = Value;
          return *this;
     }

     HttpResponseBuilder &ProtocolCode(int CodeVal)
     {
          JSONBuilderPtr->Add("code", CodeVal);
          JSONBuilderPtr->Add("code_text", CodeText(CodeVal));
          return *this;
     }

     static HttpResponseBuilder Error(int HttpCode, const std::string &ErrorMsg, const std::string &Message = "", int ProtocolCodeVal = 0)
     {
          HttpResponseBuilder Builder(HttpCode, StatusText(HttpCode));
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", ErrorMsg);

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          if (ProtocolCodeVal > 0)
          {
               Builder.ProtocolCode(ProtocolCodeVal);
          }

          return Builder;
     }

     static HttpResponseBuilder ErrorWithCode(int HttpCode, int ProtocolCodeVal, const std::string &ErrorMsg, const std::string &Message = "")
     {
          return Error(HttpCode, ErrorMsg, Message, ProtocolCodeVal);
     }

     static HttpResponseBuilder NotFound(const std::string &Path = "", const std::string &Method = "")
     {
          HttpResponseBuilder Builder(404, "Not Found");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Route not found");

          if (!Path.empty())
          {
               Builder.Add("path", Path);
          }

          if (!Method.empty())
          {
               Builder.Add("method", Method);
          }

          return Builder;
     }

     static HttpResponseBuilder BadRequest(const std::string &Message)
     {
          HttpResponseBuilder Builder(400, "Bad Request");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Bad Request");

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          return Builder;
     }

     static HttpResponseBuilder Unauthorized(const std::string &Message = "")
     {
          HttpResponseBuilder Builder(401, "Unauthorized");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Authentication required");

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          Builder.Header("WWW-Authenticate", "Bearer");
          return Builder;
     }

     static HttpResponseBuilder Forbidden(const std::string &Message = "")
     {
          HttpResponseBuilder Builder(403, "Forbidden");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Forbidden");

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          return Builder;
     }

     static HttpResponseBuilder InternalError(const std::string &Message = "")
     {
          HttpResponseBuilder Builder(500, "Internal Server Error");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Internal server error");

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          return Builder;
     }

     static HttpResponseBuilder ServiceUnavailable(const std::string &Message = "")
     {
          HttpResponseBuilder Builder(503, "Service Unavailable");
          Builder.Header("Content-Type", "application/json");
          Builder.Add("error", "Service Unavailable");

          if (!Message.empty())
          {
               Builder.Add("message", Message);
          }

          return Builder;
     }

     HttpResponse Build()
     {
          HttpResponse Response(StatusCode, StatusTextVal, "application/json");
          Response.Body = JSONBuilderPtr->ToString();
          Response.Headers.insert(Headers.begin(), Headers.end());
          return Response;
     }

     HttpResponse BuildWithBody(const std::string &BodyVal, const std::string &ContentType = "text/plain")
     {
          HttpResponse Response(StatusCode, StatusTextVal, ContentType);
          Response.Body = BodyVal;
          Response.Headers.insert(Headers.begin(), Headers.end());
          return Response;
     }

   private:
     int StatusCode;
     std::string StatusTextVal;
     std::unique_ptr<JsonBuilder> JSONBuilderPtr;
     std::map<std::string, std::string> Headers;
};
