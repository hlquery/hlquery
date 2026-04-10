/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
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

namespace
{
bool IsGlobalScopeName(const std::string &name)
{
     return name == "global" || name == "__global__";
}
}
void HLQueryCLI::ListStopwordsCounts()
{
     /* Fetch stopword counts across all collections. */

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", "/stopwords");

     if (CheckRequestFailed(response, false, "/stopwords"))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("stopwords") || !root["stopwords"].is_array())
          {
               PrintError("Invalid stopwords data");

               return;
          }

          nlohmann::json stopwords = root["stopwords"];

          if (stopwords.empty())
          {
               PrintInfo("No stopwords found in any collection.");

               return;
          }

          std::vector<std::string> headers = {"#", "Collection Name", "Stopwords Count"};
          std::vector<std::vector<std::string>> rows;

          int count = 1;

          /* Build a row for each collection summary. */

          for (const auto &item : stopwords)
          {
               std::string col_name = item.contains("collection") ? item["collection"].get<std::string>() : "unknown";
               int sw_count = item.contains("count") ? item["count"].get<int>() : 0;

               rows.push_back({std::to_string(count++), col_name, std::to_string(sw_count)});
          }

          std::cout << "Stopword counts per collection.\n";

          PrintTable(headers, rows);
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response: " + std::string(e.what()));
     }
}

/* Lists stopwords for a collection. */

void HLQueryCLI::ListStopwords(const std::string &collection_name)
{
     /* Request stopword entries for a single collection. */

     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/stopwords/global" : "/collections/" + collection_name + "/stopwords";

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response, false, path))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("stopwords") || !root["stopwords"].is_array())
          {
               PrintError("Invalid stopwords data for " + std::string(global_scope ? "global scope" : "collection '" + collection_name + "'") + ".");

               return;
          }

          nlohmann::json stopwords = root["stopwords"];

          if (stopwords.empty())
          {
               PrintInfo(std::string("No stopwords found for ") + (global_scope ? "global scope." : "collection '" + collection_name + "'."));

               return;
          }

          std::vector<std::string> headers = {"#", "Stopword"};
          std::vector<std::vector<std::string>> rows;

          int count = 1;

          for (const auto &item : stopwords)
          {
               std::string word = item.is_string() ? item.get<std::string>() : (item.contains("word") ? item["word"].get<std::string>() : "N/A");

               rows.push_back({std::to_string(count++), word});
          }

          std::cout << (global_scope ? "Global stopwords:.\n" : "Stopwords for collection '" + collection_name + "':.\n");

          PrintTable(headers, rows);
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response: " + std::string(e.what()));
     }
}

/* Adds a stopword. */

void HLQueryCLI::AddStopword(const std::string &collection_name, const std::string &word)
{
     nlohmann::json body;

     body["word"] = word;

     std::string json_str = body.dump();

     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/stopwords/global" : "/collections/" + collection_name + "/stopwords";

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", path, json_str);

     if (response.StatusCode == 200 || response.StatusCode == 201)
     {
          PrintSuccess(global_scope ? "Global stopword '" + word + "' added." : "Stopword '" + word + "' added to collection '" + collection_name + "'.");
     }
     else
     {
          CheckRequestFailed(response, false, path);
     }
}

/* Deletes a stopword. */

void HLQueryCLI::DeleteStopword(const std::string &collection_name, const std::string &word)
{
     bool global_scope = IsGlobalScopeName(collection_name);
     std::string path = global_scope ? "/stopwords/global/" + word : "/collections/" + collection_name + "/stopwords/" + word;

     HLQueryCLI::HTTPResponse response = MakeRequest("DELETE", path);

     if (response.StatusCode == 200)
     {
          PrintSuccess(global_scope ? "Global stopword '" + word + "' deleted." : "Stopword '" + word + "' deleted from collection '" + collection_name + "'.");
     }
     else if (response.StatusCode == 404)
     {
          PrintError(global_scope ? "Global stopword '" + word + "' not found." : "Stopword '" + word + "' not found in collection '" + collection_name + "'.");
     }
     else
     {
          CheckRequestFailed(response, false, path);
     }
}
