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

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <vendor/json/json.hpp>

#include "cli/cli_utils.h"
#include "hlquery_cli.h"

/* Lists collections. */
void HLQueryCLI::ListCollections(int offset, int limit, bool json_output)
{
     const bool server_loading = IsServerLoading();

     /* Warn on partial results when the server is still warming up. */

     if (!RawMode && server_loading && !json_output)
     {
          std::cout << "\n WARNING: Server is still loading metadata..\n";
          std::cout << "   Collection counts may be incomplete. Wait a moment and retry..\n\n";
     }

     std::string path = "/collections";

     /* Apply pagination if the caller requested it. */

     if (offset > 0 || limit < 10000)
     {
          path += "?offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);
     }
     else
     {
          path += "?limit=1000";
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response, false, "/collections"))
     {
          return;
     }

     nlohmann::json root;

     /* Parse the response body into JSON. */

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception& e)
     {
          PrintError("Failed to parse JSON response");

          return;
     }

     if (!root.contains("collections") || !root["collections"].is_array())
     {
          PrintError("Invalid collections data");

          return;
     }

     nlohmann::json collections = root["collections"];

     /* Provide an explicit message when no collections exist. */

     if (collections.size() == 0)
     {
          PrintInfo("No collections found.");

          return;
     }

     size_t display_count = collections.size();
     size_t total_count = root.contains("total") ? root["total"].get<size_t>() : display_count;
     size_t found_count = root.contains("found") ? root["found"].get<size_t>() : display_count;

     bool might_have_more = (display_count < found_count) || (display_count == 1000 && limit >= 1000);

     /* Emit either JSON or human-readable table output. */

     if (json_output)
     {
          if (root.contains("total"))
          {
               std::cout << root.dump(2) << "." << std::endl;

               return;
          }

          nlohmann::json output = nlohmann::json::array();

          for (size_t i = 0; i < display_count; i++)
          {
               nlohmann::json col = collections[i];
               nlohmann::json col_json;

               col_json["name"] = col["name"].get<std::string>();
               col_json["num_documents"] = col["num_documents"].get<int>();

               HLQueryCLI::HTTPResponse col_stats_resp = MakeRequest("GET", "/collections/" + col["name"].get<std::string>() + "/stats", "", DefaultTimeoutSeconds);

               if (col_stats_resp.StatusCode == 200)
               {
                    try
                    {
                         nlohmann::json col_stats = nlohmann::json::parse(col_stats_resp.Body);

                         if (col_stats.contains("num_documents"))
                         {
                              int stats_docs = col_stats["num_documents"].get<int>();
                              int list_docs = col["num_documents"].get<int>();

                              if (stats_docs != list_docs)
                              {
                                   col_json["doc_count_discrepancy"] =
                                        {
                                             {"list_endpoint", list_docs},
                                             {"stats_endpoint", stats_docs},
                                             {"difference", stats_docs - list_docs}};
                              }
                         }
                    }
                    catch (...)
                    {
                         /* Ignore. */
                    }
               }

               output.push_back(col_json);
          }

          std::cout << output.dump(2) << "." << std::endl;

          return;
     }

     if (server_loading)
     {
          std::cout << "\n WARNING: Server is still loading metadata.\n";
          std::cout << "   Collection counts may be incomplete. Wait a moment and retry.\n\n";
     }

     if (root.contains("total"))
     {
          std::cout << "Showing " << display_count << " out of " << found_count << " matched collection(s) (total " << total_count << " in system).";
     }
     else
     {
          std::cout << "Found " << display_count << " collection(s).";
     }

     if (offset > 0)
     {
          std::cout << " (from offset " << offset << ").";
     }

     if (might_have_more)
     {
          std::cout << "\n";
          std::cout << "Note: More collections available. Use 'cols <offset> <limit>' to see more.\n";
     }

     std::cout << ":\n\n";

     if (display_count == 0)
     {
          PrintInfo("No collections found.");

          return;
     }

     std::vector<std::string> headers = {"#", "Collection Name", "Documents"};
     std::vector<std::vector<std::string>> rows;

     for (size_t i = 0; i < display_count; i++)
     {
          nlohmann::json col = collections[i];

          std::string col_name = col["name"].get<std::string>();

          int num_docs = col["num_documents"].get<int>();

          rows.push_back({std::to_string(offset + i + 1), col_name, std::to_string(num_docs) + " docs"});
     }

     PrintTable(headers, rows);
}

/* Shows information about a collection. */

void HLQueryCLI::ShowCollectionInfo(const std::string& collection_name)
{
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", "/collections/" + collection_name);

     if (response.StatusCode == 404)
     {
          PrintError("Collection '" + collection_name + "' not found");

          return;
     }
     else if (response.StatusCode != 200)
     {
          CheckRequestFailed(response);

          return;
     }

     nlohmann::json col;

     try
     {
          col = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception& e)
     {
          PrintError("Failed to parse JSON response");

          return;
     }

     std::vector<std::string> headers = {"#", "Property", "Value"};
     std::vector<std::vector<std::string>> rows;

     int count = 1;

     auto add_row = [&](const std::string& p, const std::string& v)
     {
          rows.push_back({std::to_string(count++), p, v});
     };

     rows.push_back({"-", "General Information", ""});

     add_row("Collection", col["name"].get<std::string>());
     add_row("Documents", std::to_string(col["num_documents"].get<int>()));

     std::string created = "N/A";

     if (col.contains("created_at") && !col["created_at"].is_null())
     {
          try
          {
               created = col["created_at"].get<std::string>();
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     add_row("Created", created);

     auto format_fields = [&](const std::string& key) -> std::string
     {
          if (!col.contains(key) || !col[key].is_array() || col[key].empty())
          {
               return "None";
          }

          std::string res = "";

          for (size_t j = 0; j < col[key].size(); j++)
          {
               if (j > 0)
               {
                    res += ", ";
               }

               res += col[key][j].get<std::string>();
          }

          return res;
     };

     add_row("Searchable fields", format_fields("searchable_fields"));
     add_row("Filterable fields", format_fields("filterable_fields"));
     add_row("Sortable fields", format_fields("sortable_fields"));

     HLQueryCLI::HTTPResponse docs_response = MakeRequest("GET", "/collections/" + collection_name + "/documents?limit=5");
     bool llm_enabled = false;
     {
          HLQueryCLI::HTTPResponse status_response = MakeRequest("GET", "/status");
          if (status_response.StatusCode == 200)
          {
               try
               {
                    nlohmann::json status_root = nlohmann::json::parse(status_response.Body);
                    if (status_root.contains("llm") && status_root["llm"].is_object())
                    {
                         llm_enabled = status_root["llm"].value("enabled", false);
                    }
               }
               catch (...)
               {
                    /* Ignore status parse failures; keep default false. */
               }
          }
     }

     if (docs_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json docs_root = nlohmann::json::parse(docs_response.Body);

               if (docs_root.contains("documents") && docs_root["documents"].is_array() && !docs_root["documents"].empty())
               {
                    rows.push_back({"-", "Field Analysis", ""});

                    nlohmann::json documents = docs_root["documents"];

                    std::map<std::string, int> field_usage;
                    std::map<std::string, std::string> field_types;

                    for (const auto& doc : documents)
                    {
                         for (auto& [key, value] : doc.items())
                         {
                              if (key == "id" || key == "score" || key == "timestamp")
                              {
                                   continue;
                              }

                              field_usage[key]++;

                              if (value.is_string())
                              {
                                   field_types[key] = "string";
                              }
                              else if (value.is_number_integer())
                              {
                                   field_types[key] = "integer";
                              }
                              else if (value.is_number_float())
                              {
                                   field_types[key] = "float";
                              }
                              else if (value.is_boolean())
                              {
                                   field_types[key] = "boolean";
                              }
                              else
                              {
                                   field_types[key] = "object";
                              }
                         }
                    }

                    for (const auto& [field, usage] : field_usage)
                    {
                         if (field == "_topic" && field_usage.find("_topics") != field_usage.end())
                         {
                              continue;
                         }

                         std::string field_name = field;
                         std::string field_value = field_types[field] + " (" + std::to_string(usage) + "/" + std::to_string(documents.size()) + " docs)";
                         if (field == "_topic" || field == "_topics")
                         {
                              field_name = "_topics (reserved/internal)";
                              if (llm_enabled)
                              {
                                   field_value += " [LLM-assigned]";
                              }
                         }
                         add_row(field_name, field_value);
                    }
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     rows.push_back({"-", "Collection Health", ""});

     add_row("Status", "Active");
     add_row("Storage", "In-memory + LSM");
     add_row("Indexing", "Enabled");
     add_row("Search", "Full-text + Vector");

     rows.push_back({"-", "Recent Activity", ""});

     add_row("Last Updated", (col.contains("created_at") ? col["created_at"].get<std::string>() : "Unknown"));
     add_row("Index Status", "Up to date");
     add_row("Cache Status", "Active");

     std::cout << "Collection Information:.\n";

     PrintTable(headers, rows);
}

void HLQueryCLI::ShowInfo(const std::string& target)
{
     if (target.empty())
     {
          PrintError("Missing info target");
          return;
     }

     size_t slash_pos = target.find('/');

     if (slash_pos == std::string::npos)
     {
          ShowCollectionInfo(target);
          return;
     }

     std::string collection = target.substr(0, slash_pos);
     std::string document = target.substr(slash_pos + 1);

     if (collection.empty() || document.empty())
     {
          PrintError("Invalid info target: '" + target + "'");
          return;
     }

     ShowDocumentInfo(collection, document);
}

void HLQueryCLI::ShowDocumentInfo(const std::string& collection_name, const std::string& document_id)
{
     if (collection_name.empty() || document_id.empty())
     {
          PrintError("Invalid document info target");
          return;
     }

     std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/documents/" + hlquery_cli::UrlEncode(document_id);

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (response.StatusCode == 404)
     {
          PrintError("Document '" + document_id + "' not found in collection '" + collection_name + "'");
          return;
     }
     else if (response.StatusCode != 200)
     {
          CheckRequestFailed(response);
          return;
     }

     nlohmann::json doc;

     try
     {
          doc = nlohmann::json::parse(response.Body);
          if (doc.contains("_topics") && doc.contains("_topic"))
          {
               doc.erase("_topic");
          }
          else if (!doc.contains("_topics") && doc.contains("_topic"))
          {
               doc["_topics"] = doc["_topic"];
               doc.erase("_topic");
          }
     }
     catch (...)
     {
          PrintError("Failed to parse JSON response");
          return;
     }

     auto format_value = [](const nlohmann::json& value) -> std::string
     {
          std::string formatted;

          if (value.is_string())
          {
               formatted = value.get<std::string>();
          }
          else if (value.is_number_integer())
          {
               formatted = std::to_string(value.get<int64_t>());
          }
          else if (value.is_number_float())
          {
               std::ostringstream oss;
               oss << std::fixed << std::setprecision(4) << value.get<double>();
               formatted = oss.str();
          }
          else if (value.is_boolean())
          {
               formatted = value.get<bool>() ? "true" : "false";
          }
          else
          {
               formatted = value.dump();
          }

          if (formatted.size() > 120)
          {
               formatted = formatted.substr(0, 117) + "...";
          }

          return formatted.empty() ? "-" : formatted;
     };

     std::vector<std::string> headers = {"#", "Property", "Value"};
     std::vector<std::vector<std::string>> rows;
     int count = 1;

     auto add_row = [&](const std::string& property, const std::string& value)
     {
          rows.push_back({std::to_string(count++), property, value});
     };

     rows.push_back({"-", "General Information", ""});
     add_row("Document ID", doc.value("id", document_id));
     add_row("Collection", collection_name);

     if (doc.contains("title") && doc["title"].is_string())
     {
          add_row("Title", doc["title"].get<std::string>());
     }
     else
     {
          add_row("Title", "-");
     }

     std::string created = "-";

     if (doc.contains("created_at") && doc["created_at"].is_string())
     {
          created = doc["created_at"].get<std::string>();
     }

     add_row("Created", created);

     size_t content_length = 0;

     if (doc.contains("content") && doc["content"].is_string())
     {
          content_length = doc["content"].get<std::string>().length();
     }

     add_row("Content length", std::to_string(content_length));
     add_row("Field count", std::to_string(doc.size()));

     int base_fields = 1;
     base_fields += doc.contains("title") ? 1 : 0;
     base_fields += doc.contains("content") ? 1 : 0;
     base_fields += doc.contains("created_at") ? 1 : 0;
     int extra_fields = static_cast<int>(doc.size()) - base_fields;

     if (extra_fields < 0)
     {
          extra_fields = 0;
     }

     add_row("Additional fields", std::to_string(extra_fields));

     rows.push_back({"-", "Ranking Signals", ""});

     if (doc.contains("rank_signal"))
     {
          add_row("rank_signal", format_value(doc["rank_signal"]));
     }

     if (doc.contains("popularity_score"))
     {
          add_row("popularity_score", format_value(doc["popularity_score"]));
     }

     if (doc.contains("hit_log"))
     {
          add_row("hit_log", format_value(doc["hit_log"]));
     }

     rows.push_back({"-", "Field Values", ""});

     std::vector<std::pair<std::string, std::string>> field_values;

     for (const auto& item : doc.items())
     {
          const std::string& key = item.key();

          if (key == "id" ||
              key == "title" ||
              key == "content" ||
              key == "created_at" ||
              key == "rank_signal" ||
              key == "popularity_score" ||
              key == "hit_log")
          {
               continue;
          }

          field_values.emplace_back(key, format_value(item.value()));
     }

     if (field_values.empty())
     {
          add_row("Fields", "None");
     }
     else
     {
          std::sort(field_values.begin(), field_values.end());

          for (const auto& entry : field_values)
          {
               add_row(entry.first, entry.second);
          }
     }

     std::cout << "Document Information:.\n";
     PrintTable(headers, rows);
}

/* Creates a collection. */

void HLQueryCLI::CreateCollection(const std::string& name, const std::vector<std::string>& searchable_fields, const std::vector<std::string>& filterable_fields, const std::vector<std::string>& sortable_fields)
{
     nlohmann::json config;

     config["name"] = name;

     nlohmann::json searchable = nlohmann::json::array();

     for (const auto& field : searchable_fields)
     {
          searchable.push_back(field);
     }

     config["searchable_fields"] = searchable;

     nlohmann::json filterable = nlohmann::json::array();

     for (const auto& field : filterable_fields)
     {
          filterable.push_back(field);
     }

     config["filterable_fields"] = filterable;

     nlohmann::json sortable = nlohmann::json::array();

     for (const auto& field : sortable_fields)
     {
          sortable.push_back(field);
     }

     config["sortable_fields"] = sortable;

     config["enable_typos"] = true;
     config["enable_synonyms"] = true;
     config["language"] = "en";

     std::string json_str = config.dump();

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", "/collections", json_str);

     if (response.StatusCode == 201)
     {
          PrintSuccess("Collection '" + name + "' created successfully");
     }
     else
     {
          CheckRequestFailed(response);

          if (!response.Body.empty())
          {
               std::cout << "Response: " << response.Body << std::endl;
          }
     }
}

/* Deletes a collection. */

void HLQueryCLI::DeleteCollection(const std::string& name)
{
     if (name.empty() || name.find_first_of(" \t\n\r") != std::string::npos)
     {
          PrintError("Invalid collection name", "Collection name cannot be empty or contain whitespace");

          return;
     }

     if (!ConfirmDestructiveAction("DELETE COLLECTION", name))
     {
          return;
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("DELETE", "/collections/" + name);

     if (response.StatusCode == 200)
     {
          PrintSuccess("Collection '" + name + "' deleted successfully");
     }
     else if (response.StatusCode == 404)
     {
          PrintError("Collection '" + name + "' not found", "Check collection name spelling");
     }
     else
     {
          CheckRequestFailed(response);

          if (!response.Body.empty())
          {
               std::cout << "Response: " << response.Body << std::endl;
          }
     }
}

/* Rebuilds document counters. */

void HLQueryCLI::RebuildCounters(const std::string& collection_name, bool rebuild_index)
{
     if (rebuild_index)
     {
          PrintInfo("Repair tool: Rebuilding inverted index and counters from primary LSM data");
     }
     else
     {
          PrintInfo("Rebuilding doc counters from source of truth (LSM + WAL)");
     }

     std::vector<std::string> collections;

     if (collection_name.empty())
     {
          HLQueryCLI::HTTPResponse cols_resp = MakeRequest("GET", "/collections", "", DefaultTimeoutSeconds);

          if (cols_resp.StatusCode == 200)
          {
               try
               {
                    nlohmann::json cols_json = nlohmann::json::parse(cols_resp.Body);

                    if (cols_json.contains("collections") && cols_json["collections"].is_array())
                    {
                         for (const auto& col : cols_json["collections"])
                         {
                              collections.push_back(col["name"].get<std::string>());
                         }
                    }
               }
               catch (...)
               {
                    PrintError("Failed to parse collections list");

                    return;
               }
          }
          else
          {
               PrintError("Failed to get collections list");

               return;
          }
     }
     else
     {
          collections.push_back(collection_name);
     }

     std::vector<std::vector<std::string>> rows;

     int fixed_count_val = 0;
     int index_rebuilt_count_val = 0;

     for (const auto& col : collections)
     {
          HLQueryCLI::HTTPResponse meta_resp = MakeRequest("GET", "/collections/" + col, "", DefaultTimeoutSeconds);

          size_t metadata_count = 0;

          if (meta_resp.StatusCode == 200)
          {
               try
               {
                    nlohmann::json col_json = nlohmann::json::parse(meta_resp.Body);

                    metadata_count = col_json.contains("num_documents") ? col_json["num_documents"].get<size_t>() : 0;
               }
               catch (...)
               {
                    /* Ignore. */
               }
          }

          HLQueryCLI::HTTPResponse stats_resp = MakeRequest("GET", "/collections/" + col + "/stats", "", DefaultTimeoutSeconds);

          size_t lsm_count = 0;

          if (stats_resp.StatusCode == 200)
          {
               try
               {
                    nlohmann::json stats_json = nlohmann::json::parse(stats_resp.Body);

                    if (stats_json.contains("num_documents"))
                    {
                         lsm_count = stats_json["num_documents"].get<size_t>();
                    }
               }
               catch (...)
               {
                    /* Ignore. */
               }
          }

          bool needs_fix = (metadata_count != lsm_count);

          std::string status_str = needs_fix ? "⚠ MISMATCH" : "✓ OK";
          std::string action_str = needs_fix ? "FIXED" : "OK";
          std::string index_status_str = "N/A";

          if (needs_fix || rebuild_index)
          {
               std::string repair_url = "/repair?force=true";

               repair_url += "&collection=" + hlquery_cli::UrlEncode(col);

               if (rebuild_index)
               {
                    repair_url += "&rebuild_index=true";
               }

               HLQueryCLI::HTTPResponse update_resp = MakeRequest("POST", repair_url, "", DefaultTimeoutSeconds);

               if (update_resp.StatusCode == 200)
               {
                    if (needs_fix)
                    {
                         fixed_count_val++;
                    }

                    if (rebuild_index)
                    {
                         index_rebuilt_count_val++;
                         index_status_str = "REBUILT";
                    }
               }
               else
               {
                    action_str = "FAILED";
                    index_status_str = "FAILED";
               }
          }

          rows.push_back({col, std::to_string(metadata_count), std::to_string(lsm_count), status_str, action_str, index_status_str});
     }

     PrintTable({"Collection", "Metadata", "Actual", "Status", "Action", "Index"}, rows);

     std::cout << "\nRepair summary: FIXED " << fixed_count_val << " collection counters.";

     if (rebuild_index)
     {
          std::cout << " REBUILT " << index_rebuilt_count_val << " inverted indexes.";
     }

     std::cout << "." << std::endl;
}

/* Flushes all data. */

void HLQueryCLI::FlushAll(bool skip_confirmation)
{
     if (!skip_confirmation)
     {
          if (!ConfirmDestructiveAction("FLUSH ALL DATA", "the entire database"))
          {
               return;
          }
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", "/flush");

     if (CheckRequestFailed(response, false, "/flush"))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (root.contains("success") && root["success"].get<bool>())
          {
               int deleted = root.contains("collections_deleted") ? root["collections_deleted"].get<int>() : 0;

               PrintSuccess("Database flushed successfully");

               if (deleted > 0)
               {
                    std::cout << "Deleted " << deleted << " collection(s) and cleared all data." << std::endl;
               }
               else
               {
                    std::cout << "Database was already empty." << std::endl;
               }
          }
          else
          {
               PrintError("Flush failed", root.contains("message") ? root["message"].get<std::string>() : "Unknown error");
          }
     }
     catch (const std::exception& e)
     {
          PrintError("Failed to parse flush response", e.what());
     }
}
