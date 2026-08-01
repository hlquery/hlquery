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
#include <exception>
#include <map>
#include <string>
#include <vector>

#include "api/userauth.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "search/rocksdb_storage_engine.h"
#include "vendor/json/json.hpp"

/* Builds the fixed storage prefix used by one module. */

static std::string BuildModuleStoragePrefix(const std::string &ModuleName)
{
     return "module_data:" + ModuleName + ":";
}

/* Removes the module prefix from one stored key. */

static std::string StripModuleStoragePrefix(const std::string &FullKey, const std::string &Prefix)
{
     if (FullKey.rfind(Prefix, 0) != 0)
     {
          return FullKey;
     }

     return FullKey.substr(Prefix.size());
}

static std::string ExtractAuthTokenFromHeaders(const std::map<std::string, std::string> &Headers)
{
     auto AuthIt = Headers.find("Authorization");

     if (AuthIt == Headers.end())
     {
          AuthIt = Headers.find("authorization");
     }

     std::string Token;

     if (AuthIt != Headers.end())
     {
          Token = AuthIt->second;
     }
     else
     {
          auto APIKeyIt = Headers.find("X-API-Key");

          if (APIKeyIt == Headers.end())
          {
               APIKeyIt = Headers.find("x-api-key");
          }

          if (APIKeyIt != Headers.end())
          {
               Token = APIKeyIt->second;
          }
     }

     if (Token.rfind("Bearer ", 0) == 0)
     {
          Token = Token.substr(7);
     }

     return Token;
}

/* Extracts command parameters from a JSON request body. */

static void ParseJSONBodyParameters(const std::string &Body, ModuleCommandRequest &Request)
{
     if (Body.empty())
     {
          return;
     }

     try
     {
          const nlohmann::json body_json = nlohmann::json::parse(Body);

          if (body_json.is_object())
          {
               for (auto it = body_json.begin(); it != body_json.end(); ++it)
               {
                    if (it.key() == "parameters" && it.value().is_array())
                    {
                         for (const auto &Entry : it.value())
                         {
                              if (Entry.is_string())
                              {
                                   Request.Parameters.push_back(Entry.get<std::string>());
                              }
                              else
                              {
                                   Request.Parameters.push_back(Entry.dump());
                              }
                         }

                         Request.PositionalParameters = Request.Parameters;
                         continue;
                    }

                    if (it.value().is_string())
                    {
                         Request.NamedParameters[it.key()] = it.value().get<std::string>();
                    }
                    else
                    {
                         Request.NamedParameters[it.key()] = it.value().dump();
                    }
               }
          }
          else if (body_json.is_array())
          {
               for (const auto &Entry : body_json)
               {
                    if (Entry.is_string())
                    {
                         Request.Parameters.push_back(Entry.get<std::string>());
                    }
                    else
                    {
                         Request.Parameters.push_back(Entry.dump());
                    }
               }

               Request.PositionalParameters = Request.Parameters;
          }
     }
     catch (const std::exception &Error)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("modules", "Failed to parse module command JSON body: " + std::string(Error.what()) + ".");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("modules", "Failed to parse module command JSON body with unknown exception.");
          }
     }
}

/* Default module destructor. */

RuntimeModule::~RuntimeModule() = default;

void CompositeRuntimeModule::AddComponent(RuntimeModule &Component)
{
     if (&Component == this)
     {
          return;
     }

     if (std::find(Components.begin(), Components.end(), &Component) == Components.end())
     {
          Components.push_back(&Component);
     }
}

std::vector<RuntimeModule *> CompositeRuntimeModule::GetHookTargets(ModuleHook Hook)
{
     std::vector<RuntimeModule *> Targets = RuntimeModule::GetHookTargets(Hook);

     for (RuntimeModule *Component : Components)
     {
          if (!Component)
          {
               continue;
          }

          std::vector<RuntimeModule *> ComponentTargets = Component->GetHookTargets(Hook);
          Targets.insert(Targets.end(), ComponentTargets.begin(), ComponentTargets.end());
     }

     return Targets;
}

bool CompositeRuntimeModule::Start(const ServerConfig &Config, std::string &ErrorMessage)
{
     std::vector<RuntimeModule *> StartedComponents;
     StartedComponents.reserve(Components.size());

     for (RuntimeModule *Component : Components)
     {
          if (!Component)
          {
               continue;
          }

          std::string ComponentError;
          bool Started = false;

          try
          {
               Started = Component->Start(Config, ComponentError);
          }
          catch (const std::exception &Error)
          {
               ComponentError = Error.what();
               Started = false;
          }
          catch (...)
          {
               ComponentError = "unknown exception";
               Started = false;
          }

          if (Started)
          {
               StartedComponents.push_back(Component);
               continue;
          }

          for (auto It = StartedComponents.rbegin(); It != StartedComponents.rend(); ++It)
          {
               try
               {
                    (*It)->Stop();
               }
               catch (...)
               {
               }
          }

          ErrorMessage = "Component '" + Component->GetName() + "' failed to start";

          if (!ComponentError.empty())
          {
               ErrorMessage += ": " + ComponentError;
          }

          ErrorMessage += ".";
          return false;
     }

     return true;
}

void CompositeRuntimeModule::Stop()
{
     for (auto It = Components.rbegin(); It != Components.rend(); ++It)
     {
          if (!*It)
          {
               continue;
          }

          try
          {
               (*It)->Stop();
          }
          catch (...)
          {
          }
     }
}

void CompositeRuntimeModule::OnUnloadModule()
{
     for (auto It = Components.rbegin(); It != Components.rend(); ++It)
     {
          if (!*It)
          {
               continue;
          }

          try
          {
               (*It)->OnUnloadModule();
          }
          catch (...)
          {
          }
     }
}

/* Returns the storage prefix reserved for this module. */

std::string RuntimeModule::GetStoragePrefix() const
{
     return BuildModuleStoragePrefix(GetName());
}

/* Builds one fully-qualified storage key for this module. */

std::string RuntimeModule::MakeStorageKey(const std::string &Key) const
{
     return GetStoragePrefix() + Key;
}

/* Stores one module-scoped value in the shared database. */

bool RuntimeModule::SetStorageValue(const std::string &Key, const std::string &Value) const
{
     if (!Instance || !Instance->Database || Key.empty())
     {
          return false;
     }

     return Instance->Database->Set(MakeStorageKey(Key), Value);
}

/* Retrieves one module-scoped value from the shared database. */

std::string RuntimeModule::GetStorageValue(const std::string &Key) const
{
     if (!Instance || !Instance->Database || Key.empty())
     {
          return "";
     }

     return Instance->Database->Get(MakeStorageKey(Key));
}

/* Deletes one module-scoped value from the shared database. */

bool RuntimeModule::DeleteStorageValue(const std::string &Key) const
{
     if (!Instance || !Instance->Database || Key.empty())
     {
          return false;
     }

     return Instance->Database->Del(MakeStorageKey(Key)) > 0;
}

/* Lists module-scoped keys relative to the module namespace. */

std::vector<std::string> RuntimeModule::ListStorageKeys(const std::string &Pattern) const
{
     std::vector<std::string> relative_keys;

     if (!Instance || !Instance->Database)
     {
          return relative_keys;
     }

     const std::string prefix = GetStoragePrefix();
     const std::string pattern = prefix + (Pattern.empty() ? "*" : Pattern);
     const std::vector<std::string> full_keys = Instance->Database->Keys(pattern);
     relative_keys.reserve(full_keys.size());

     for (const auto &FullKey : full_keys)
     {
          relative_keys.push_back(StripModuleStoragePrefix(FullKey, prefix));
     }

     return relative_keys;
}

/* Deletes every stored key owned by this module. */

size_t RuntimeModule::ClearStorage() const
{
     if (!Instance || !Instance->Database)
     {
          return 0;
     }

     const std::string prefix = GetStoragePrefix();
     const std::vector<std::string> full_keys = Instance->Database->Keys(prefix + "*");
     size_t deleted_count = 0;

     for (const auto &FullKey : full_keys)
     {
          if (Instance->Database->Del(FullKey) > 0)
          {
               ++deleted_count;
          }
     }

     return deleted_count;
}

/* Converts one HTTP request into a shared module command request. */

HttpResponse RuntimeModule::HandleAPIRequest(const HttpRequest &Request, const std::string &SubPath) const
{
     ModuleCommandRequest command_request;

     command_request.Transport = "http";
     command_request.Route = SubPath.empty() ? "status" : SubPath;
     command_request.Body = Request.Body;
     command_request.NamedParameters = Request.QueryParams;
     command_request.Authenticated = Request.Authenticated;
     command_request.APIKeyID = Request.APIKeyID;
     command_request.IsAPIKey = !Request.APIKeyID.empty();
     command_request.RemoteAddress = Request.RemoteAddress;
     command_request.AuthToken = ExtractAuthTokenFromHeaders(Request.Headers);
     command_request.IsCancelled = Request.IsCancelled;

     if (Instance && Instance->Users)
     {
          if (!Instance->Users->IsAuthEnabled())
          {
               command_request.IsAdmin = true;
          }
          else if (command_request.Authenticated && !command_request.IsAPIKey && !command_request.AuthToken.empty())
          {
               const AuthResult Auth = Instance->Users->AuthenticateToken(command_request.AuthToken);

               if (Auth.Valid)
               {
                    command_request.RequesterUser = Auth.UserObj.Name;
                    command_request.IsAdmin = Auth.UserObj.IsAdmin();
               }
          }
     }

     ParseJSONBodyParameters(Request.Body, command_request);
     ModuleCommandResponse command_response = const_cast<RuntimeModule *>(this)->HandleCommand(command_request);
     HttpResponse response(command_response.StatusCode,
                           (command_response.StatusCode >= 200 && command_response.StatusCode < 300) ? "OK" : "Error",
                           command_response.ContentType.empty() ? "application/json" : command_response.ContentType);

     response.Body = command_response.Body;
     return response;
}
