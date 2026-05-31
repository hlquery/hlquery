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

#include <iostream>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

#include "cli/cliutils.h"
#include "app.h"

static bool IsGlobalScopeName(const std::string &name)
{
     return name == "global" || name == "__global__";
}
void HLQueryCLI::ListSynonymsCounts()
{
     /* Fetch synonym counts across all collections. */

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", "/synonyms");

     if (CheckRequestFailed(response, false, "/synonyms"))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("synonyms") || !root["synonyms"].is_array())
          {
               PrintError("Invalid synonyms data");

               return;
          }

          nlohmann::json synonyms = root["synonyms"];

          if (synonyms.empty())
          {
               PrintInfo("No synonyms found in any collection.");

               return;
          }

          std::vector<std::string> headers = {"#", "Collection Name", "Synonym Groups"};
          std::vector<std::vector<std::string>> rows;

          int count = 1;

          /* Build a row for each collection summary. */

          for (const auto &item : synonyms)
          {
               std::string col_name = item.contains("collection") ? item["collection"].get<std::string>() : "unknown";
               int syn_count = item.contains("count") ? item["count"].get<int>() : 0;

               rows.push_back({std::to_string(count++), col_name, std::to_string(syn_count)});
          }

          std::cout << "Synonym counts per collection.\n";

          PrintTable(headers, rows);
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response: " + std::string(e.what()));
     }
}

/* Lists synonyms for a collection. */

void HLQueryCLI::ListSynonyms(const std::string &collection_name)
{
     /* Request synonym entries for a single collection. */

     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/synonyms/global" : "/collections/" + collection_name + "/synonyms";

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response, false, path))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("synonyms") || !root["synonyms"].is_array())
          {
               PrintError("Invalid synonyms data for " + std::string(global_scope ? "global scope" : "collection '" + collection_name + "'") + ".");

               return;
          }

          nlohmann::json synonyms = root["synonyms"];

          if (synonyms.empty())
          {
               PrintInfo(std::string("No synonyms found for ") + (global_scope ? "global scope." : "collection '" + collection_name + "'."));

               return;
          }

          std::vector<std::string> headers = {"#", "ID", "Root Term", "Synonyms"};
          std::vector<std::vector<std::string>> rows;

          int count = 1;

          for (const auto &item : synonyms)
          {
               std::string id = item.contains("id") ? item["id"].get<std::string>() : "N/A";
               std::string root_term = item.contains("root") ? item["root"].get<std::string>() : "N/A";

               std::string syn_list = "";

               if (item.contains("synonyms") && item["synonyms"].is_array())
               {
                    for (size_t i = 0; i < item["synonyms"].size(); i++)
                    {
                         if (i > 0)
                         {
                              syn_list += ", ";
                         }

                         syn_list += item["synonyms"][i].get<std::string>();
                    }
               }

               rows.push_back({std::to_string(count++), id, root_term, syn_list});
          }

          std::cout << (global_scope ? "Global synonyms:.\n" : "Synonyms for collection '" + collection_name + "':.\n");

          PrintTable(headers, rows);
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response: " + std::string(e.what()));
     }
}

/* Adds a synonym group. */

void HLQueryCLI::AddSynonym(const std::string &collection_name, const std::string &synonym_id, const std::string &root_term, const std::vector<std::string> &synonyms)
{
     nlohmann::json body;

     body["root"] = root_term;
     body["synonyms"] = synonyms;

     std::string json_str = body.dump();

     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/synonyms/global/" + synonym_id : "/collections/" + collection_name + "/synonyms/" + synonym_id;

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", path, json_str);

     if (response.StatusCode == 200 || response.StatusCode == 201)
     {
          PrintSuccess(global_scope ? "Global synonym group '" + synonym_id + "' added." : "Synonym group '" + synonym_id + "' added to collection '" + collection_name + "'.");
     }
     else
     {
          CheckRequestFailed(response, false, path);
     }
}

/* Deletes a synonym group. */

void HLQueryCLI::DeleteSynonym(const std::string &collection_name, const std::string &synonym_id)
{
     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/synonyms/global/" + synonym_id : "/collections/" + collection_name + "/synonyms/" + synonym_id;

     HLQueryCLI::HTTPResponse response = MakeRequest("DELETE", path);

     if (response.StatusCode == 200)
     {
          PrintSuccess(global_scope ? "Global synonym group '" + synonym_id + "' deleted." : "Synonym group '" + synonym_id + "' deleted from collection '" + collection_name + "'.");
     }
     else if (response.StatusCode == 404)
     {
          PrintError(global_scope ? "Global synonym group '" + synonym_id + "' not found." : "Synonym group '" + synonym_id + "' not found in collection '" + collection_name + "'.");
     }
     else
     {
          CheckRequestFailed(response, false, path);
     }
}
