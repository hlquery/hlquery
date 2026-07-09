/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#include <cctype>
#include <regex>

#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "vendor/json/json.hpp"

/* Provides preset API handlers for reusable search configuration. */

namespace
{
     constexpr const char *kPresetsKey = "presets";

     std::string ExtractPresetName(const std::string &Path)
     {
          std::regex PresetRegex(R"(^/presets/([^/?]+)$)");
          std::smatch Match;

          if (std::regex_search(Path, Match, PresetRegex))
          {
               return Match[1].str();
          }

          return "";
     }

     bool IsValidPresetName(const std::string &Name)
     {
          if (Name.empty() || Name.size() > 128 || Name.find("..") != std::string::npos)
          {
               return false;
          }

          for (char Ch : Name)
          {
               if (!std::isalnum(static_cast<unsigned char>(Ch)) && Ch != '_' && Ch != '-' && Ch != '.')
               {
                    return false;
               }
          }

          return true;
     }

     nlohmann::json LoadPresets()
     {
          const std::string Raw = HybridStorageManagerInstance().Get(kPresetsKey);

          if (Raw.empty())
          {
               return nlohmann::json::object();
          }

          try
          {
               nlohmann::json Parsed = nlohmann::json::parse(Raw);
               return Parsed.is_object() ? Parsed : nlohmann::json::object();
          }
          catch (const std::exception &)
          {
               return nlohmann::json::object();
          }
     }

     bool SavePresets(const nlohmann::json &Presets)
     {
          return Instance && Instance->Database && Instance->Database->Set(kPresetsKey, Presets.dump());
     }
}

HttpResponse SearchAPI::HandleListPresets(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     nlohmann::json ResponseJSON;
     ResponseJSON["presets"] = LoadPresets();

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ResponseJSON.dump();
     return Response;
}

HttpResponse SearchAPI::HandleCreateOrUpdatePreset(const HttpRequest &Request)
{
     if (Request.Method != "POST" && Request.Method != "PUT")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string PresetName = ExtractPresetName(Request.Path);

     if (!IsValidPresetName(PresetName))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid preset name.", "Preset names may contain letters, numbers, underscore, dash, and dot.");
     }

     try
     {
          nlohmann::json Preset = nlohmann::json::parse(Request.Body);

          if (!Preset.is_object())
          {
               return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid preset.", "Preset body must be a JSON object.");
          }

          nlohmann::json Presets = LoadPresets();
          Presets[PresetName] = Preset;

          if (!SavePresets(Presets))
          {
               return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
          }

          nlohmann::json ResponseJSON;
          ResponseJSON["name"] = PresetName;
          ResponseJSON["preset"] = Preset;

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
          Response.Body = ResponseJSON.dump();
          return Response;
     }
     catch (const std::exception &E)
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid JSON.", E.what());
     }
}

HttpResponse SearchAPI::HandleGetPreset(const HttpRequest &Request)
{
     if (Request.Method != "GET")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string PresetName = ExtractPresetName(Request.Path);

     if (!IsValidPresetName(PresetName))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid preset name.", "Preset names may contain letters, numbers, underscore, dash, and dot.");
     }

     nlohmann::json Presets = LoadPresets();

     if (!Presets.contains(PresetName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::SEARCH_INVALID_PARAMETER, "Preset not found.", "The requested preset does not exist.");
     }

     nlohmann::json ResponseJSON;
     ResponseJSON["name"] = PresetName;
     ResponseJSON["preset"] = Presets[PresetName];

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ResponseJSON.dump();
     return Response;
}

HttpResponse SearchAPI::HandleDeletePreset(const HttpRequest &Request)
{
     if (Request.Method != "DELETE")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     const std::string PresetName = ExtractPresetName(Request.Path);

     if (!IsValidPresetName(PresetName))
     {
          return BuildErrorResponse(Status::BAD_REQUEST, Code::VALIDATION_INVALID_JSON, "Invalid preset name.", "Preset names may contain letters, numbers, underscore, dash, and dot.");
     }

     nlohmann::json Presets = LoadPresets();

     if (!Presets.contains(PresetName))
     {
          return BuildErrorResponse(Status::NOT_FOUND, Code::SEARCH_INVALID_PARAMETER, "Preset not found.", "The requested preset does not exist.");
     }

     Presets.erase(PresetName);

     if (!SavePresets(Presets))
     {
          return HttpResponse(Status::INTERNAL_SERVER_ERROR, StatusText(Status::INTERNAL_SERVER_ERROR), "application/json");
     }

     nlohmann::json ResponseJSON;
     ResponseJSON["name"] = PresetName;
     ResponseJSON["deleted"] = true;

     HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
     Response.Body = ResponseJSON.dump();
     return Response;
}
