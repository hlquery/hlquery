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
#include <filesystem>
#include <regex>
#include <sstream>

#include "api/searchapi.h"
#include "api/common.h"
#include "api/userauth.h"
#include "common/cryptoutils.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "vendor/json/json.hpp"

/* Provides user management API handlers for administrative clients. */

using json = nlohmann::json;

static HttpResponse MakeJSONResponse(int Code, const std::string &Status, const std::string &Body)
{
     HttpResponse Resp(Code, Status, "application/json");
     Resp.Body = Body;
     return Resp;
}

static std::string GenerateUserToken()
{
     std::vector<uint8_t> RandomBytesVal = RandomBytes(32);
     return Hex(RandomBytesVal);
}

static std::string MaskToken(const std::string &Token)
{
     if (Token.empty())
     {
          return "";
     }

     if (Token.size() <= 4)
     {
          return std::string(Token.size(), '*');
     }

     return std::string(Token.size() - 4, '*') + Token.substr(Token.size() - 4);
}

static std::vector<std::string> FlagsToStrings(const std::set<UserFlag> &Flags)
{
     std::vector<std::string> Result;

     if (Flags.find(UserFlag::ADMIN) != Flags.end())
     {
          Result.push_back("admin");
     }

     if (Flags.find(UserFlag::USER) != Flags.end())
     {
          Result.push_back("user");
     }

     return Result;
}

static std::set<UserFlag> ParseFlags(const json &FlagsValue, std::string &ErrorMsg)
{
     std::set<UserFlag> Flags;

     auto AddFlag = [&](const std::string &Val)
     {
          std::string Lower = Val;
          std::transform(Lower.begin(), Lower.end(), Lower.begin(), ::tolower);

          if (Lower == "admin")
          {
               Flags.insert(UserFlag::ADMIN);
               Flags.insert(UserFlag::USER);
               return;
          }

          if (Lower == "user")
          {
               Flags.insert(UserFlag::USER);
               return;
          }

          ErrorMsg = "Invalid flag: " + Val;
     };

     if (FlagsValue.is_string())
     {
          std::string FlagsStr = FlagsValue.get<std::string>();
          std::stringstream SS(FlagsStr);
          std::string Item;

          while (std::getline(SS, Item, ','))
          {
               if (Item.empty())
               {
                    continue;
               }
           
               AddFlag(Item);
           
               if (!ErrorMsg.empty())
               {
                    return {};
               }
          }
     }
     else if (FlagsValue.is_array())
     {
          for (const auto &FlagVal : FlagsValue)
          {
               if (!FlagVal.is_string())
               {
                    ErrorMsg = "Flags must be strings";
                    return {};
               }
           
               AddFlag(FlagVal.get<std::string>());
           
               if (!ErrorMsg.empty())
               {
                    return {};
               }
          }
     }
     else if (!FlagsValue.is_null())
     {
          ErrorMsg = "Flags must be a string or array";
          return {};
     }

     if (Flags.empty())
     {
          Flags.insert(UserFlag::USER);
     }

     return Flags;
}

static json UserToJSON(const User &UserObj, bool IncludeToken)
{
     json Result;
     Result["name"] = UserObj.Name;
     Result["description"] = UserObj.Description;
     Result["flags"] = FlagsToStrings(UserObj.Flags);
     Result["token_present"] = !UserObj.Token.empty();
     Result["token_masked"] = MaskToken(UserObj.Token);

     if (IncludeToken)
     {
          Result["token"] = UserObj.Token;
     }

     return Result;
}

static std::string ExtractUserNameFromPath(const std::string &Path)
{
     std::regex UserRegex(R"(/users/([^/]+))");
     std::smatch Match;

     if (std::regex_search(Path, Match, UserRegex))
     {
          return Match[1].str();
     }

     return "";
}

HttpResponse SearchAPI::HandleListUsers(const HttpRequest &Request)
{
     (void)Request;

     json Response;
     Response["auth_enabled"] = (Instance && Instance->Users) ? Instance->Users->IsAuthEnabled() : false;
     Response["users"] = json::array();

     if (Instance && Instance->Users)
     {
          auto Users = Instance->Users->GetAllUsers();

          for (const auto &UserObj : Users)
          {
               Response["users"].push_back(UserToJSON(UserObj, false));
          }

          Response["count"] = static_cast<int>(Users.size());
     }
     else
     {
          Response["count"] = 0;
     }

     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = Response.dump();
     return Resp;
}

HttpResponse SearchAPI::HandleCreateUser(const HttpRequest &Request)
{
     if (!Instance || !Instance->Users)
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"User manager unavailable\"}");
     }

     json Body;

     try
     {
          Body = json::parse(Request.Body.empty() ? "{}" : Request.Body);
     }
     catch (const json::exception &)
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Invalid JSON\"}");
     }

     if (!Body.contains("name") || !Body["name"].is_string())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Missing or invalid name\"}");
     }

     std::string Name = Body["name"].get<std::string>();
     if (Name.empty())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Name cannot be empty\"}");
     }

     if (Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreCreateUser, Name, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     if (Instance->Users->GetUser(Name).has_value())
     {
          return MakeJSONResponse(409, "Conflict", "{\"error\":\"User already exists\"}");
     }

     std::string Token = "";
     if (Body.contains("token") && Body["token"].is_string())
     {
          Token = Body["token"].get<std::string>();
     }
     if (Token.empty())
     {
          Token = GenerateUserToken();
     }

     if (Instance->Users->IsValidToken(Token))
     {
          return MakeJSONResponse(409, "Conflict", "{\"error\":\"Token already in use\"}");
     }

     std::string ErrorMsg;
     std::set<UserFlag> Flags;

     if (Body.contains("flags"))
     {
          Flags = ParseFlags(Body["flags"], ErrorMsg);
     }
     else
     {
          Flags.insert(UserFlag::USER);
     }

     if (!ErrorMsg.empty())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"" + ErrorMsg + "\"}");
     }

     std::string Description = "";

     if (Body.contains("description") && Body["description"].is_string())
     {
          Description = Body["description"].get<std::string>();
     }

     User NewUser(Name, Token, Flags, Description);
     Instance->Users->AddUser(NewUser);

     std::string UsersDat = std::string(HLQUERY_ADMIN_DIR) + "/users.dat";
     std::filesystem::create_directories(HLQUERY_ADMIN_DIR);

     if (!Instance->Users->SaveUsersToEncryptedFile(UsersDat))
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"Failed to persist users\"}");
     }

     json Response = UserToJSON(NewUser, true);
     HttpResponse Resp(201, "Created", "application/json");
     Resp.Body = Response.dump();
     return Resp;
}

HttpResponse SearchAPI::HandleGetUser(const HttpRequest &Request)
{
     if (!Instance || !Instance->Users)
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"User manager unavailable\"}");
     }

     std::string Name = ExtractUserNameFromPath(Request.Path);
     if (Name.empty())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Missing user name\"}");
     }

     auto UserOpt = Instance->Users->GetUser(Name);
     if (!UserOpt.has_value())
     {
          return MakeJSONResponse(404, "Not Found", "{\"error\":\"User not found\"}");
     }

     json Response = UserToJSON(UserOpt.value(), false);
     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = Response.dump();
     return Resp;
}

HttpResponse SearchAPI::HandleDeleteUser(const HttpRequest &Request)
{
     if (!Instance || !Instance->Users)
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"User manager unavailable\"}");
     }

     std::string Name = ExtractUserNameFromPath(Request.Path);
     if (Name.empty())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Missing user name\"}");
     }

     if (Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreDeleteUser, Name, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     if (!Instance->Users->RemoveUser(Name))
     {
          return MakeJSONResponse(404, "Not Found", "{\"error\":\"User not found\"}");
     }

     std::string UsersDat = std::string(HLQUERY_ADMIN_DIR) + "/users.dat";
     std::filesystem::create_directories(HLQUERY_ADMIN_DIR);

     if (!Instance->Users->SaveUsersToEncryptedFile(UsersDat))
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"Failed to persist users\"}");
     }

     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = "{\"deleted\":true}";
     return Resp;
}

HttpResponse SearchAPI::HandleUpdateUser(const HttpRequest &Request)
{
     if (!Instance || !Instance->Users)
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"User manager unavailable\"}");
     }

     std::string Name = ExtractUserNameFromPath(Request.Path);
     if (Name.empty())
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Missing user name\"}");
     }

     if (Instance->Modules)
     {
          ModulePreCheckResult PreCheck = RUN_MODULE_PRECHECK(OnPreUpdateUser, Name, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          if (PreCheck.Action == ModulePreCheckAction::Deny)
          {
               return BuildErrorResponse(PreCheck.HttpStatus, PreCheck.ProtocolCode, PreCheck.Message, PreCheck.Details);
          }
     }

     auto UserOpt = Instance->Users->GetUser(Name);
     if (!UserOpt.has_value())
     {
          return MakeJSONResponse(404, "Not Found", "{\"error\":\"User not found\"}");
     }

     json Body;
     try
     {
          Body = json::parse(Request.Body.empty() ? "{}" : Request.Body);
     }
     catch (const json::exception &)
     {
          return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Invalid JSON\"}");
     }

     if (Body.contains("name") && Body["name"].is_string())
     {
          if (Body["name"].get<std::string>() != Name)
          {
               return MakeJSONResponse(400, "Bad Request", "{\"error\":\"Name cannot be changed\"}");
          }
     }

     User Updated = UserOpt.value();

     if (Body.contains("description") && Body["description"].is_string())
     {
          Updated.Description = Body["description"].get<std::string>();
     }

     std::string ErrorMsg;

     if (Body.contains("flags"))
     {
          auto Flags = ParseFlags(Body["flags"], ErrorMsg);
          if (!ErrorMsg.empty())
          {
               return MakeJSONResponse(400, "Bad Request", "{\"error\":\"" + ErrorMsg + "\"}");
          }

          Updated.Flags = Flags;
     }

     bool IncludeToken = false;

     if (Body.contains("token") && Body["token"].is_string())
     {
          std::string Token = Body["token"].get<std::string>();

          if (!Token.empty() && Token != Updated.Token)
          {
               if (Instance->Users->IsValidToken(Token))
               {
                    return MakeJSONResponse(409, "Conflict", "{\"error\":\"Token already in use\"}");
               }
               
               Updated.Token = Token;
               IncludeToken = true;
          }
     }

     Instance->Users->UpdateUser(Updated);

     std::string UsersDat = std::string(HLQUERY_ADMIN_DIR) + "/users.dat";
     std::filesystem::create_directories(HLQUERY_ADMIN_DIR);

     if (!Instance->Users->SaveUsersToEncryptedFile(UsersDat))
     {
          return MakeJSONResponse(500, "Internal Server Error", "{\"error\":\"Failed to persist users\"}");
     }

     json Response = UserToJSON(Updated, IncludeToken);
     HttpResponse Resp(200, "OK", "application/json");
     Resp.Body = Response.dump();
     return Resp;
}
