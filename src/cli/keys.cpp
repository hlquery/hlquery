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

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

#include "app.h"

using json = nlohmann::json;

/* Lists API keys. */

void HLQueryCLI::ListKeys()
{
     /* Request the current key list from the server. */

     HTTPResponse resp = MakeRequest("GET", "/keys");

     if (CheckRequestFailed(resp))
     {
          return;
     }

     try
     {
          json data = json::parse(resp.Body);

          if (!data.contains("keys") || !data["keys"].is_array())
          {
               std::cout << "No API keys found.\n";

               return;
          }

          std::vector<std::string> headers = {"ID", "Description", "Scopes (Collection -> Actions)", "Hanalyzer", "Uses"};
          std::vector<std::vector<std::string>> rows;

          /* Build table rows for each key entry. */

          for (const auto &key : data["keys"])
          {
               std::string scope_summary = "";

               if (key.contains("scopes") && key["scopes"].is_object())
               {
                    for (auto &[col, scope] : key["scopes"].items())
                    {
                         if (!scope_summary.empty())
                         {
                              scope_summary += "\n";
                         }

                         scope_summary += col + ": ";

                         std::string acts = "";

                         for (const auto &a : scope["actions"])
                         {
                              if (!acts.empty())
                              {
                                   acts += ", ";
                              }

                              acts += a.get<std::string>();
                         }

                         scope_summary += "[" + acts + "]";

                         if (!scope["embedded_filters"].get<std::string>().empty())
                         {
                              scope_summary += " (Filter: " + scope["embedded_filters"].get<std::string>() + ")";
                         }
                    }
               }

               rows.push_back({key["id"].get<std::string>(),
                               key["description"].get<std::string>(),
                               scope_summary,
                               key.value("allow_hanalyzer", false) ? "Yes" : "No",
                               std::to_string(key["use_count"].get<int>())});
          }

          PrintTable(headers, rows);
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse response: ", e.what());
     }
}

/* Creates a new API key. */

void HLQueryCLI::CreateKey(const std::string &description, const std::vector<std::string> &collections, const std::vector<std::string> &actions, int expires_at, const std::string &embedded_filters, bool allow_hanalyzer)
{
     /* Compose the create-key payload. */

     json body;
     body["description"] = description;
     body["collections"] = collections;
     body["actions"] = actions;
     body["allow_hanalyzer"] = allow_hanalyzer;

     if (expires_at > 0)
     {
          body["expires_at"] = expires_at;
     }

     if (!embedded_filters.empty())
     {
          body["embedded_filters"] = embedded_filters;
     }

     HTTPResponse resp = MakeRequest("POST", "/keys", body.dump());

     if (CheckRequestFailed(resp))
     {
          return;
     }

     try
     {
          json data = json::parse(resp.Body);

          PrintSuccess("API key created successfully.");
          std::cout << "ID:  " << data["id"].get<std::string>() << "\n";
          std::cout << "Key: " << data["key"].get<std::string>() << "\n";
          std::cout << "Hanalyzer: " << (allow_hanalyzer ? "Enabled" : "Disabled") << "\n";
          std::cout << "\nIMPORTANT: Save this key, it will not be shown again!\n";
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse response: ", e.what());
     }
}

/* Deletes an API key by id. */

void HLQueryCLI::DeleteKey(const std::string &key_id)
{
     if (!ConfirmDestructiveAction("delete API key", key_id))
     {
          return;
     }

     HTTPResponse resp = MakeRequest("DELETE", "/keys/" + key_id);

     if (CheckRequestFailed(resp))
     {
          return;
     }

     PrintSuccess("API key deleted successfully.");
}

/* Updates an existing API key. */

void HLQueryCLI::UpdateKey(const std::string &key_id, const std::string &description, const std::vector<std::string> &collections, const std::vector<std::string> &actions, const std::string &embedded_filters, bool allow_hanalyzer, const std::vector<std::string> &add_collections, const std::vector<std::string> &remove_collections)
{
     /* Compose the update payload with the provided fields. */

     json body;

     if (!description.empty())
     {
          body["description"] = description;
     }

     if (!collections.empty())
     {
          body["collections"] = collections;
     }

     if (!actions.empty())
     {
          body["actions"] = actions;
     }

     if (!embedded_filters.empty())
     {
          body["embedded_filters"] = embedded_filters;
     }

     if (!add_collections.empty())
     {
          body["add_collections"] = add_collections;
     }

     if (!remove_collections.empty())
     {
          body["remove_collections"] = remove_collections;
     }

     body["allow_hanalyzer"] = allow_hanalyzer;

     HTTPResponse resp = MakeRequest("PUT", "/keys/" + key_id, body.dump());

     if (CheckRequestFailed(resp))
     {
          return;
     }

     PrintSuccess("API key updated successfully.");
}
