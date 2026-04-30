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
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#include <vendor/json/json.hpp>

#include "core/typedefs.h"
#include "cli/cliutils.h"
#include "app.h"

namespace
{
std::string JoinDocumentStrings(const std::vector<std::string> &values, const std::string &separator)
{
     std::string joined;

     for (size_t index = 0; index < values.size(); ++index)
     {
          if (index > 0)
          {
               joined += separator;
          }

          joined += values[index];
     }

     return joined;
}

/* Parse a CLI value as JSON when it looks like a literal, otherwise keep it as a string. */

nlohmann::json ParseUpdateFieldValue(const std::string &field_value)
{
     const nlohmann::json parsed_value = nlohmann::json::parse(field_value, nullptr, false);

     if (!parsed_value.is_discarded())
     {
          return parsed_value;
     }

     return field_value;
}

bool LooksLikeAggregateSQL(const std::string &sql)
{
     std::string upper = sql;

     std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c)
                    { return static_cast<char>(std::toupper(c)); });

     return upper.find("SELECT AVG(") != std::string::npos ||
            upper.find("SELECT SUM(") != std::string::npos ||
            upper.find("SELECT MIN(") != std::string::npos ||
            upper.find("SELECT MAX(") != std::string::npos ||
            upper.find("SELECT COUNT(") != std::string::npos ||
            upper.find("SELECT STATS(") != std::string::npos ||
            upper.find(", AVG(") != std::string::npos ||
            upper.find(", SUM(") != std::string::npos ||
            upper.find(", MIN(") != std::string::npos ||
            upper.find(", MAX(") != std::string::npos ||
            upper.find(", COUNT(") != std::string::npos ||
            upper.find(", STATS(") != std::string::npos;
}

void PrintAggregationsTable(HLQueryCLI &cli, const nlohmann::json &aggregations)
{
     std::vector<std::vector<std::string>> rows;

     for (auto it = aggregations.begin(); it != aggregations.end(); ++it)
     {
          const std::string name = it.key();
          const nlohmann::json &aggregation = it.value();
          const std::string type = aggregation.value("type", "");

          if (aggregation.contains("metrics") && aggregation["metrics"].is_object())
          {
               for (auto metric_it = aggregation["metrics"].begin(); metric_it != aggregation["metrics"].end(); ++metric_it)
               {
                    rows.push_back({name, type, metric_it.key(), metric_it.value().dump()});
               }
          }
          else if (aggregation.contains("buckets") && aggregation["buckets"].is_array())
          {
               for (const auto &bucket : aggregation["buckets"])
               {
                    rows.push_back({name, type, bucket.value("key", ""), std::to_string(bucket.value("doc_count", 0))});
               }
          }
     }

     if (!rows.empty())
     {
          cli.PrintTable({"Aggregation", "Type", "Metric", "Value"}, rows);
     }
}

void PrintSQLRowsTable(HLQueryCLI &cli, const nlohmann::json &rows_json)
{
     if (!rows_json.is_array() || rows_json.empty() || !rows_json[0].is_object())
     {
          return;
     }

     std::vector<std::string> headers;
     for (auto it = rows_json[0].begin(); it != rows_json[0].end(); ++it)
     {
          headers.push_back(it.key());
     }

     std::vector<std::vector<std::string>> rows;
     rows.reserve(rows_json.size());

     for (const auto &row_json : rows_json)
     {
          if (!row_json.is_object())
          {
               continue;
          }

          std::vector<std::string> row;
          row.reserve(headers.size());

          for (const auto &header : headers)
          {
               if (!row_json.contains(header))
               {
                    row.push_back("");
                    continue;
               }

               const auto &value = row_json[header];
               if (value.is_string())
               {
                    row.push_back(value.get<std::string>());
               }
               else
               {
                    row.push_back(value.dump());
               }
          }

          rows.push_back(std::move(row));
     }

     if (!rows.empty())
     {
          cli.PrintTable(headers, rows);
     }
}

}

bool HLQueryCLI::IsDataTooMessyForTable(const nlohmann::json &doc)
{
     /* Track size metrics that help decide table output. */

     int field_count = 0;
     int total_width = 0;
     int long_text_fields = 0;
     int nested_objects = 0;
     int array_fields = 0;

     for (auto &[key, value] : doc.items())
     {
          field_count++;
          total_width += key.length();

          if (value.is_string())
          {
               std::string str_val = value.get<std::string>();

               total_width += str_val.length();

               if (str_val.length() > 200)
               {
                    long_text_fields++;
               }

               if (str_val.find('\n') != std::string::npos || str_val.find('\t') != std::string::npos || str_val.find('\r') != std::string::npos)
               {
                    long_text_fields++;
               }
          }
          else if (value.is_object())
          {
               nested_objects++;
               total_width += 50;
          }
          else if (value.is_array())
          {
               array_fields++;
               total_width += 50;
          }
          else if (value.is_number() || value.is_boolean())
          {
               std::string val_str = value.dump();

               total_width += val_str.length();
          }
          else
          {
               total_width += value.dump().length();
          }
     }

     int terminal_width = 120;

#ifdef TIOCGSIZE
     struct winsize w;

     /* Use the terminal width when available for sizing decisions. */

     if (ioctl(STDOUT_FILENO, TIOCGSIZE, &w) == 0)
     {
          terminal_width = w.ws_col;
     }
#endif

     bool too_many_fields = field_count > 15;
     bool too_wide = total_width > (terminal_width * 0.8);
     bool too_many_long_texts = long_text_fields > 3;
     bool has_nested = nested_objects > 0 || array_fields > 0;
     bool exceeds_terminal = total_width > terminal_width;

     return too_many_fields || too_wide || too_many_long_texts || has_nested || exceeds_terminal;
}

/* Lists documents in a collection. */

void HLQueryCLI::ListDocuments(const std::string &collection_name, int offset, int limit)
{
     /* Validate the collection name before hitting the server. */

     if (collection_name.empty() || collection_name.find_first_of(" \t\n\r") != std::string::npos)
     {
          PrintError("Invalid collection name", "Collection name cannot be empty or contain whitespace");

          return;
     }

     /* Stream documents in pages to avoid large responses. */

     const int PAGE_SIZE_VAL = 100;

     int current_offset = offset;
     int remaining = limit;

     bool has_more = true;

     size_t total_val = 0;

     std::vector<nlohmann::json> all_documents;

     while (has_more && remaining > 0)
     {
          int page_limit = std::min(PAGE_SIZE_VAL, remaining);

          std::string path = "/collections/" + collection_name + "/documents";

          path += "?offset=" + std::to_string(current_offset) + "&limit=" + std::to_string(page_limit);

          HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

          if (response.StatusCode == 404)
          {
               PrintError("Collection '" + collection_name + "' not found", "Check collection name spelling");

               return;
          }
          else if (response.StatusCode != 200)
          {
               CheckRequestFailed(response);

               return;
          }

          nlohmann::json root;

          try
          {
               root = nlohmann::json::parse(response.Body);
          }
          catch (const std::exception &e)
          {
               PrintError("Failed to parse JSON response", std::string(e.what()));

               return;
          }

          if (total_val == 0 && root.contains("total") && root["total"].is_number_integer())
          {
               total_val = root["total"].get<size_t>();
          }

          if (!root.contains("documents") || !root["documents"].is_array())
          {
               PrintError("Invalid documents data", "Response missing 'documents' array");

               return;
          }

          nlohmann::json documents = root["documents"];

          if (documents.size() == 0)
          {
               if (current_offset == offset)
               {
                    PrintInfo("No documents found in collection '" + collection_name + "'");
               }

               has_more = false;

               break;
          }

          for (const auto &doc : documents)
          {
               all_documents.push_back(doc);

               remaining--;

               if (remaining <= 0)
               {
                    has_more = false;
                    break;
               }
          }

          current_offset += documents.size();
          has_more = (documents.size() == static_cast<size_t>(page_limit));
     }

     if (total_val == 0)
     {
          total_val = all_documents.size();
     }

     std::cout << "Found " << all_documents.size() << " document(s)";

     if (offset > 0 || limit != 20)
     {
          std::cout << " (showing " << (offset + 1) << "-" << (offset + all_documents.size()) << " of " << total_val << ").";
     }
     else if (total_val > all_documents.size())
     {
          std::cout << " (showing first " << all_documents.size() << " of " << total_val << ").";
     }

     std::cout << " in collection '" << collection_name << "':\n\n";

     std::vector<std::string> headers = {"#", "Document ID", "Fields"};
     std::vector<std::vector<std::string>> rows;

     for (size_t i = 0; i < all_documents.size(); i++)
     {
          nlohmann::json doc = all_documents[i];

          int field_count = 0;

          for (auto &[key, value] : doc.items())
          {
               if (key == "id" || key == "score" || key == "timestamp" || key == "collection_id")
               {
                    continue;
               }

               std::string field_value;

               if (value.is_string())
               {
                    field_value = value.get<std::string>();
               }
               else if (value.is_number())
               {
                    field_value = std::to_string(value.get<double>());
               }
               else if (value.is_boolean())
               {
                    field_value = value.get<bool>() ? "true" : "false";
               }
               else
               {
                    field_value = value.dump();
               }

               if (field_value.empty() || field_value == "null" || field_value == "\"\"")
               {
                    continue;
               }

               field_count++;
          }

          rows.push_back({std::to_string(i + 1), doc["id"].get<std::string>(), std::to_string(field_count) + " fields"});
     }

     PrintTable(headers, rows);
}

/* Opens a specific document. */

void HLQueryCLI::OpenDocument(const std::string &collection_name, const std::string &document_id, const std::string &format, const std::string &route)
{
     std::string path = "/collections/" + collection_name + "/documents/" + document_id;
     if (!route.empty())
     {
          path += "?route=" + hlquery_cli::UrlEncode(route);
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (response.StatusCode == 404)
     {
          PrintError("Collection '" + collection_name + "' or document '" + document_id + "' not found");

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
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response");

          return;
     }

     std::string actual_format = format;

     if (actual_format == "json")
     {
          std::cout << doc.dump(2) << std::endl;
     }
     else if (actual_format == "text")
     {
          for (auto &[key, value] : doc.items())
          {
               std::cout << key << ": ";

               if (value.is_string())
               {
                    std::cout << value.get<std::string>();
               }
               else if (value.is_number_integer())
               {
                    std::cout << value.get<int64_t>();
               }
               else if (value.is_number_float())
               {
                    std::cout << value.get<double>();
               }
               else if (value.is_boolean())
               {
                    std::cout << (value.get<bool>() ? "true" : "false");
               }
               else
               {
                    std::cout << value.dump();
               }

               newline();
          }
     }
     else
     {
          std::vector<std::string> headers = {"Field", "Value"};
          std::vector<std::vector<std::string>> rows;

          for (auto &[key, value] : doc.items())
          {
               std::string value_str;

               if (value.is_string())
               {
                    std::string str_val = value.get<std::string>();

                    value_str = str_val;

                    size_t pos = 0;

                    while ((pos = value_str.find('\n', pos)) != std::string::npos)
                    {
                         value_str.replace(pos, 1, " ");
                         pos++;
                    }
               }
               else if (value.is_number_integer())
               {
                    value_str = std::to_string(value.get<int64_t>());
               }
               else if (value.is_number_float())
               {
                    value_str = std::to_string(value.get<double>());
               }
               else if (value.is_boolean())
               {
                    value_str = value.get<bool>() ? "true" : "false";
               }
               else
               {
                    value_str = value.dump();
               }

               rows.push_back({key, value_str});
          }

          if (!rows.empty())
          {
               PrintTable(headers, rows);
          }
     }
}

/* Selects a specific field from a document. */

void HLQueryCLI::SelectField(const std::string &field_names, const std::string &collection_name, const std::string &document_id)
{
     std::string path = "/collections/" + collection_name + "/documents/" + document_id;

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (response.StatusCode == 404)
     {
          PrintError("Collection '" + collection_name + "' or document '" + document_id + "' not found");

          return;
     }
     else if (response.StatusCode != 200)
     {
          CheckRequestFailed(response);

          return;
     }

     try
     {
          nlohmann::json doc = nlohmann::json::parse(response.Body);

          std::vector<std::string> fields;

          std::istringstream iss(field_names);

          std::string field;

          while (std::getline(iss, field, ','))
          {
               field.erase(0, field.find_first_not_of(" \t"));
               field.erase(field.find_last_not_of(" \t") + 1);

               if (!field.empty())
               {
                    fields.push_back(field);
               }
          }

          if (fields.empty())
          {
               PrintError("No valid field names provided");

               return;
          }

          for (const auto &field_name : fields)
          {
               if (!doc.contains(field_name))
               {
                    PrintError("Field '" + field_name + "' not found in document");

                    return;
               }
          }

          if (fields.size() == 1)
          {
               const std::string &field_name = fields[0];

               auto &value = doc[field_name];

               if (value.is_string())
               {
                    std::cout << value.get<std::string>() << std::endl;
               }
               else if (value.is_number_integer())
               {
                    std::cout << value.get<int64_t>() << std::endl;
               }
               else if (value.is_number_float())
               {
                    std::cout << value.get<double>() << std::endl;
               }
               else if (value.is_boolean())
               {
                    std::cout << (value.get<bool>() ? "true" : "false") << std::endl;
               }
               else
               {
                    std::cout << value.dump() << std::endl;
               }
          }
          else
          {
               std::vector<std::string> headers = {"Field", "Value"};
               std::vector<std::vector<std::string>> rows;

               for (const auto &field_name : fields)
               {
                    auto &value = doc[field_name];

                    std::string value_str;

                    if (value.is_string())
                    {
                         value_str = value.get<std::string>();
                    }
                    else if (value.is_number_integer())
                    {
                         value_str = std::to_string(value.get<int64_t>());
                    }
                    else if (value.is_number_float())
                    {
                         value_str = std::to_string(value.get<double>());
                    }
                    else if (value.is_boolean())
                    {
                         value_str = value.get<bool>() ? "true" : "false";
                    }
                    else
                    {
                         value_str = value.dump();
                    }

                    rows.push_back({field_name, value_str});
               }

               PrintTable(headers, rows);
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse JSON response");

          return;
     }
}

/* Updates a specific field in a document. */

void HLQueryCLI::UpdateDocumentField(const std::string &collection_name, const std::string &document_id, const std::string &field_name, const std::string &field_value)
{
     if (collection_name.empty() || document_id.empty() || field_name.empty())
     {
          PrintError("Invalid update arguments", "Collection name, document ID, and field name are required");
          return;
     }

     nlohmann::json body;
     body[field_name] = ParseUpdateFieldValue(field_value);

     const std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) +
                              "/documents/" + hlquery_cli::UrlEncode(document_id) +
                              "?distributed=off";

     HLQueryCLI::HTTPResponse response = MakeRequest("PUT", path, body.dump());

     if (response.StatusCode == 404)
     {
          PrintError("Collection '" + collection_name + "' or document '" + document_id + "' not found");
          return;
     }
     else if (response.StatusCode != 200)
     {
          CheckRequestFailed(response);
          return;
     }

     PrintSuccess("Updated field '" + field_name + "' in document '" + document_id + "'");
}

/* Searches for documents in a collection. */

void HLQueryCLI::PrintMaybeSuggestions(const nlohmann::json &root, const std::string &query, const std::string &collection_name)
{
     if (!root.contains("maybe") || !root["maybe"].is_object())
     {
          return;
     }

     const nlohmann::json &MaybeJSON = root["maybe"];

     const size_t Count = MaybeJSON.value("count", static_cast<size_t>(0));
     const int Threshold = MaybeJSON.value("threshold", 0);
     const int Limit = MaybeJSON.value("limit", 0);
     const std::string Message = MaybeJSON.value("message", "");

     std::vector<std::vector<std::string>> Rows;

     if (MaybeJSON.contains("suggestions") && MaybeJSON["suggestions"].is_array())
     {
          size_t Index = 1;

          for (const auto &Suggestion : MaybeJSON["suggestions"])
          {
               std::string Text;

               if (Suggestion.contains("text") && Suggestion["text"].is_string())
               {
                    Text = Suggestion["text"].get<std::string>();
               }
               else if (Suggestion.contains("name") && Suggestion["name"].is_string())
               {
                    Text = Suggestion["name"].get<std::string>();
               }

               std::string SourceCollection = Suggestion.value("collection", collection_name);
               std::string DocumentID = Suggestion.value("id", "");
               std::string Score = Suggestion.contains("score") ? Suggestion["score"].dump() : "";

               Rows.push_back({std::to_string(Index++), Text, SourceCollection, DocumentID, Score});
          }
     }

     std::cout << "\nMaybe suggestions for '" << query << "' in collection '" << collection_name << "'";

     if (Threshold > 0)
     {
          std::cout << " when results are below " << Threshold;
     }

     std::cout << ".\n";
     if (Count == 0)
     {
          std::cout << "Nothing found for '" << query << "' in collection '" << collection_name << "'";
          if (Threshold > 0)
          {
               std::cout << " under the maybe threshold";
          }
          std::cout << ".\n";
     }
     else
     {
          std::cout << "'" << query << "' matched " << Count << " suggestion(s)";
     }

     if (Count > 0 && Limit > 0)
     {
          std::cout << " (limit " << Limit << ")";
     }

     if (Count > 0)
     {
          std::cout << ".\n";
     }

     if (!Message.empty() && Message != "ok")
     {
          std::cout << "Message: " << Message << ".\n";
     }

     if (Rows.empty())
     {
          return;
     }

     std::cout << "\n";

     std::vector<std::string> Headers = {"#", "Suggestion", "Collection", "Document ID", "Score"};

     PrintTable(Headers, Rows);
}

void HLQueryCLI::SearchDocuments(const std::string &collection_name, const std::string &query, int limit, int offset, const std::string &sort, bool exact_match, bool highlight, const std::string &highlight_fields, const std::string &distributed, const std::string &route, int snippet_threshold, int maybe_min, int maybe_limit, bool json_output)
{
     (void)snippet_threshold;

     if (collection_name.empty() || collection_name.find_first_of(" \t\n\r") != std::string::npos)
     {
          PrintError("Invalid collection name", "Collection name cannot be empty or contain whitespace");

          return;
     }

     if (query.length() > 10000)
     {
          PrintError("Query too long", "Maximum query length is 10000 characters");

          return;
     }

     auto search_start = std::chrono::steady_clock::now();

     const int PAGE_SIZE_VAL = 100;

     int current_offset = offset;
     int remaining = limit;

     bool has_more = true;

     std::vector<std::vector<std::string>> all_rows;
     nlohmann::json aggregated_hits = nlohmann::json::array();
     nlohmann::json maybe_root;
     nlohmann::json first_root;
     bool has_maybe_payload = false;
     nlohmann::json last_root;
     size_t total_found = 0;
     bool has_found_value = false;
     while (has_more && remaining > 0)
     {
          int page_limit = std::min(PAGE_SIZE_VAL, remaining);

          std::string path = "/collections/" + collection_name + "/documents/search";

          std::string fields_to_search = "title,content";

          std::string query_string = "q=" + hlquery_cli::UrlEncode(query) + "&limit=" + std::to_string(page_limit) + "&offset=" + std::to_string(current_offset);

          query_string += "&query_by=" + hlquery_cli::UrlEncode(fields_to_search);

          if (!sort.empty())
          {
               query_string += "&sort_by=" + hlquery_cli::UrlEncode(sort);
          }

          if (exact_match)
          {
               query_string += "&prioritize_exact_match=true";
          }

          if (highlight)
          {
               query_string += "&highlight=true";

               if (!highlight_fields.empty())
               {
                    query_string += "&highlight_fields=" + hlquery_cli::UrlEncode(highlight_fields);
               }
          }

          if (!distributed.empty())
          {
               query_string += "&distributed=" + hlquery_cli::UrlEncode(distributed);
          }

          if (!route.empty())
          {
               query_string += "&route=" + hlquery_cli::UrlEncode(route);
          }

          if (maybe_min >= 0)
          {
               query_string += "&maybe_min=" + std::to_string(maybe_min);
          }

          if (maybe_limit > 0)
          {
               query_string += "&maybe_limit=" + std::to_string(maybe_limit);
          }

          HLQueryCLI::HTTPResponse response = MakeRequest("GET", path + "?" + query_string);

          if (response.StatusCode == 404)
          {
               try
               {
                    nlohmann::json error_json = nlohmann::json::parse(response.Body);

                    if (error_json.contains("error") && error_json["error"].get<std::string>() == "Collection not found")
                    {
                         PrintError("Collection '" + collection_name + "' not found", "Check collection name spelling or use 'list' to see available collections");

                         return;
                    }
               }
               catch (...)
               {
                    /* Ignore. */
               }
          }

          if (CheckRequestFailed(response))
          {
               return;
          }

          nlohmann::json root;

          try
          {
               root = nlohmann::json::parse(response.Body);
          }
          catch (const std::exception &e)
          {
               PrintError("Failed to parse search response");

               return;
          }

          if (first_root.is_null())
          {
               first_root = root;
          }

          if (!has_maybe_payload && root.contains("maybe") && root["maybe"].is_object())
          {
               maybe_root = root;
               has_maybe_payload = true;
          }

          last_root = root;

          if (root.contains("found") && root["found"].is_number_unsigned())
          {
               total_found = root["found"].get<size_t>();
               has_found_value = true;
          }

          if (!root.contains("hits") || !root["hits"].is_array())
          {
               PrintError("Invalid search response format", "Response missing 'hits' array");

               return;
          }

          nlohmann::json hits = root["hits"];

          if (json_output || RawMode)
          {
               for (const auto &hit : hits)
               {
                    aggregated_hits.push_back(hit);
               }
          }

          if (hits.size() == 0)
          {
               has_more = false;
               break;
          }

          for (size_t i = 0; i < hits.size(); i++)
          {
               nlohmann::json hit = hits[i];

               std::string doc_id = "";

               if (hit.contains("document") && hit["document"].contains("id"))
               {
                    doc_id = hit["document"]["id"].get<std::string>();
               }
               else if (hit.contains("id"))
               {
                    doc_id = hit["id"].get<std::string>();
               }

               std::string score_str = "";

               if (hit.contains("text_match"))
               {
                    score_str = std::to_string(hit["text_match"].get<double>());
               }
               else if (hit.contains("score"))
               {
                    score_str = std::to_string(hit["score"].get<double>());
               }

               std::string title = "";
               std::string content = "";
               std::string fields_preview = "";

               if (hit.contains("document"))
               {
                    nlohmann::json doc = hit["document"];

                    if (doc.contains("title"))
                    {
                         title = doc["title"].get<std::string>();
                    }
                    else if (doc.contains("field_0"))
                    {
                         title = doc["field_0"].get<std::string>();
                    }
                    else if (doc.contains("name"))
                    {
                         title = doc["name"].get<std::string>();
                    }

                    if (doc.contains("content"))
                    {
                         content = doc["content"].get<std::string>();
                    }
                    else if (doc.contains("field_1"))
                    {
                         content = doc["field_1"].get<std::string>();
                    }
                    else if (doc.contains("description"))
                    {
                         content = doc["description"].get<std::string>();
                    }
                    else if (doc.contains("field_2"))
                    {
                         content = doc["field_2"].get<std::string>();
                    }

                    int field_count = 0;

                    std::vector<std::string> field_names;

                    for (auto &[key, value] : doc.items())
                    {
                         if (key == "id" || key == "title" || key == "content" || key == "collection_id")
                         {
                              continue;
                         }

                         std::string field_value;

                         if (value.is_string())
                         {
                              field_value = value.get<std::string>();
                         }
                         else if (value.is_number_integer())
                         {
                              field_value = std::to_string(value.get<int64_t>());
                         }
                         else if (value.is_number_float())
                         {
                              field_value = std::to_string(value.get<double>());
                         }
                         else if (value.is_boolean())
                         {
                              field_value = value.get<bool>() ? "true" : "false";
                         }
                         else
                         {
                              field_value = value.dump();
                         }

                         if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                         {
                              field_count++;

                              if (field_names.size() < 3)
                              {
                                   field_names.push_back(key);
                              }
                         }
                    }

                    fields_preview = std::to_string(field_count) + " fields";

                    if (!field_names.empty())
                    {
                         fields_preview += " (" + field_names[0];

                         for (size_t j = 1; j < field_names.size(); j++)
                         {
                              fields_preview += ", " + field_names[j];
                         }

                         fields_preview += ")";
                    }
               }

               if (content.length() > 60)
               {
                    content = content.substr(0, 57) + "...";
               }

               if (title.length() > 30)
               {
                    title = title.substr(0, 27) + "...";
               }

               const std::string doc_ref = std::to_string(offset + static_cast<int>(all_rows.size()) + 1);

               all_rows.push_back({doc_ref, doc_id, score_str, title, content, fields_preview});

               remaining--;

               if (remaining <= 0)
               {
                    has_more = false;
                    break;
               }
          }

          current_offset += hits.size();
          has_more = (static_cast<int>(hits.size()) == page_limit && remaining > 0);
     }

     if (json_output || RawMode)
     {
          nlohmann::json output = first_root.is_object() ? first_root : (last_root.is_object() ? last_root : nlohmann::json::object());
          auto search_end = std::chrono::steady_clock::now();
          auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();

          output["hits"] = aggregated_hits;

          if (has_found_value)
          {
               output["found"] = total_found;
          }
          else
          {
               output["found"] = aggregated_hits.size();
          }

          if (has_maybe_payload && maybe_root.contains("maybe"))
          {
               output["maybe"] = maybe_root["maybe"];
          }

          output["cli"] = {
              {"collection", collection_name},
              {"requested_query", query},
              {"requested_limit", limit},
              {"requested_offset", offset},
              {"duration_ms", duration_ms}
          };

          std::cout << output.dump(2) << "\n";
          return;
     }

     if (!all_rows.empty())
     {
          std::vector<std::string> headers = {"Doc", "Document ID", "Score", "Title", "Content Preview", "Fields"};

          std::cout << "Search results for '" << query << "' in collection '" + collection_name + "':.\n";
          std::cout << "Found " << all_rows.size() << " document(s)";

          if (offset > 0 || limit != 10000)
          {
               std::cout << " (showing " << (offset + 1) << "-" << (offset + all_rows.size()) << ").";
          }

          std::cout << "\n\n";

          PrintTable(headers, all_rows);
     }
     else if (current_offset == offset)
     {
          PrintInfo("No documents found matching your search");
     }

     if (has_maybe_payload)
     {
          PrintMaybeSuggestions(maybe_root, query, collection_name);
     }

     auto search_end = std::chrono::steady_clock::now();
     auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
     std::cout << "Search completed in " << duration_ms << " ms.\n";
}

void HLQueryCLI::SearchSQL(const std::string &sql, const std::string &collection_name, bool json_output)
{
     if (sql.empty())
     {
          PrintError("SQL query cannot be empty");
          return;
     }

     if (!collection_name.empty() && collection_name.find_first_of(" \t\n\r") != std::string::npos)
     {
          PrintError("Invalid collection name", "Collection name cannot contain whitespace");
          return;
     }

     auto search_start = std::chrono::steady_clock::now();
     const std::string path = "/sql";
     const std::string query_string = "sql=" + hlquery_cli::UrlEncode(sql);
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path + "?" + query_string);

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SQL search response");
          return;
     }

     const bool has_hits = root.contains("hits") && root["hits"].is_array();
     const bool has_aggregations = root.contains("aggregations") && root["aggregations"].is_object() && !root["aggregations"].empty();
     const bool has_rows = root.contains("rows") && root["rows"].is_array();
     const bool has_write_result = root.contains("deleted") || root.contains("updated") || root.contains("message");

     if (!has_hits && !has_aggregations && !has_rows && !has_write_result)
     {
          PrintError("Invalid SQL search response format", "Response missing 'hits', 'rows', and 'aggregations'");
          return;
     }

     const nlohmann::json hits = has_hits ? root["hits"] : nlohmann::json::array();

     if (json_output || RawMode)
     {
          nlohmann::json output = root;
          auto search_end = std::chrono::steady_clock::now();
          auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();

          output["cli"] = {
              {"collection", collection_name},
              {"requested_sql", sql},
              {"duration_ms", duration_ms}
          };

          std::cout << output.dump(2) << "\n";
          return;
     }

     if (has_write_result)
     {
          if (root.contains("message") && root["message"].is_string())
          {
               std::cout << root["message"].get<std::string>() << "\n";
          }

          if (root.contains("deleted"))
          {
               std::cout << "Deleted: " << root["deleted"].dump();
               if (root.contains("failed"))
               {
                    std::cout << ", Failed: " << root["failed"].dump();
               }
               if (root.contains("total"))
               {
                    std::cout << ", Total matched: " << root["total"].dump();
               }
               std::cout << "\n";
          }

          if (root.contains("updated"))
          {
               std::cout << "Updated: " << root["updated"].dump() << "\n";
          }

          auto search_end = std::chrono::steady_clock::now();
          auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
          std::cout << "Search completed in " << duration_ms << " ms.\n";
          return;
     }

     const bool aggregate_sql = LooksLikeAggregateSQL(sql);

     if (has_rows)
     {
          std::cout << "\nSQL rows for `" << sql << "`";

          if (!collection_name.empty())
          {
               std::cout << " in collection '" << collection_name << "'";
          }

          std::cout << ":\n";
          PrintSQLRowsTable(*this, root["rows"]);
          std::cout << root["rows"].size() << " result" << (root["rows"].size() == 1 ? "" : "s") << " shown.\n";

          auto search_end = std::chrono::steady_clock::now();
          auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
          std::cout << "Search completed in " << duration_ms << " ms.\n";
          return;
     }

     if (has_aggregations)
     {
          std::cout << "\nSQL aggregations for `" << sql << "`";

          if (!collection_name.empty())
          {
               std::cout << " in collection '" << collection_name << "'";
          }

          std::cout << ":\n";
          PrintAggregationsTable(*this, root["aggregations"]);

          if (aggregate_sql)
          {
               auto search_end = std::chrono::steady_clock::now();
               auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
               std::cout << "Search completed in " << duration_ms << " ms.\n";
               return;
          }
     }

     std::vector<std::vector<std::string>> all_rows;
     for (const auto &hit : hits)
     {
          std::string doc_id;
          std::string score_str;
          std::string title;
          std::string content;
          std::string fields_preview;
          std::string hit_collection = collection_name;

          if (hit.contains("document") && hit["document"].is_object())
          {
               const nlohmann::json &doc = hit["document"];

               if (doc.contains("id") && doc["id"].is_string())
               {
                    doc_id = doc["id"].get<std::string>();
               }

               if (doc.contains("_collection") && doc["_collection"].is_string())
               {
                    hit_collection = doc["_collection"].get<std::string>();
               }

               if (doc.contains("title") && doc["title"].is_string())
               {
                    title = doc["title"].get<std::string>();
               }
               else if (doc.contains("field_0") && doc["field_0"].is_string())
               {
                    title = doc["field_0"].get<std::string>();
               }
               else if (doc.contains("name") && doc["name"].is_string())
               {
                    title = doc["name"].get<std::string>();
               }

               if (doc.contains("content") && doc["content"].is_string())
               {
                    content = doc["content"].get<std::string>();
               }
               else if (doc.contains("field_1") && doc["field_1"].is_string())
               {
                    content = doc["field_1"].get<std::string>();
               }
               else if (doc.contains("description") && doc["description"].is_string())
               {
                    content = doc["description"].get<std::string>();
               }
               else if (doc.contains("field_2") && doc["field_2"].is_string())
               {
                    content = doc["field_2"].get<std::string>();
               }

               int field_count = 0;
               std::vector<std::string> field_names;

               for (auto &[key, value] : doc.items())
               {
                    if (key == "id" || key == "title" || key == "content" || key == "collection_id" || key == "_collection")
                    {
                         continue;
                    }

                    const std::string field_value = value.is_string() ? value.get<std::string>() : value.dump();

                    if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                    {
                         field_count++;

                         if (field_names.size() < 3)
                         {
                              field_names.push_back(key);
                         }
                    }
               }

               fields_preview = std::to_string(field_count) + " fields";

               if (!field_names.empty())
               {
                    fields_preview += " (" + field_names[0];

                    for (size_t j = 1; j < field_names.size(); ++j)
                    {
                         fields_preview += ", " + field_names[j];
                    }

                    fields_preview += ")";
               }
          }

          if (hit.contains("text_match"))
          {
               score_str = std::to_string(hit["text_match"].get<double>());
          }
          else if (hit.contains("score"))
          {
               score_str = std::to_string(hit["score"].get<double>());
          }

          if (content.length() > 60)
          {
               content = content.substr(0, 57) + "...";
          }

          if (title.length() > 30)
          {
               title = title.substr(0, 27) + "...";
          }

          const std::string doc_ref = std::to_string(all_rows.size() + 1);

          all_rows.push_back({doc_ref, doc_id, score_str, title, content, fields_preview});
     }

     if (!all_rows.empty())
     {
          const std::vector<std::string> headers = {"Doc", "Document ID", "Score", "Title", "Content Preview", "Fields"};

          std::cout << "SQL results for `" << sql << "`";

          if (!collection_name.empty())
          {
               std::cout << " in collection '" << collection_name << "'";
          }

          std::cout << ":\n";
          PrintTable(headers, all_rows);
     }
     else
     {
          PrintInfo("No documents found for SQL query");
     }

     auto search_end = std::chrono::steady_clock::now();
     auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
     std::cout << "Search completed in " << duration_ms << " ms.\n";
}

void HLQueryCLI::MaybeSuggest(const std::string &query, const std::string &collection_name, int limit, int min_results, bool json_output)
{
     if (query.empty())
     {
          PrintError("Query cannot be empty", "Usage: maybe <query> <collection> [limit] [min_results]");
          return;
     }

     if (collection_name.empty() || collection_name.find_first_of(" \t\n\r") != std::string::npos)
     {
          PrintError("Invalid collection name", "Collection name cannot be empty or contain whitespace");
          return;
     }

     if (!CollectionExists(collection_name))
     {
          PrintError("Collection '" + collection_name + "' not found", "Check collection name spelling");
          return;
     }

     if (limit < 1)
     {
          limit = 1;
     }
     if (limit > 20)
     {
          limit = 20;
     }

     if (min_results < 1)
     {
          min_results = 1;
     }

     std::string path = "/collections/" + collection_name + "/documents/search";
     std::string query_string = "q=" + hlquery_cli::UrlEncode(query) + "&query_by=" + hlquery_cli::UrlEncode("title,content") + "&limit=" + std::to_string(std::max(1, std::min(min_results, 20))) + "&offset=0";
     query_string += "&maybe_min=" + std::to_string(min_results);
     query_string += "&maybe_limit=" + std::to_string(limit);

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path + "?" + query_string);

     if (CheckRequestFailed(response))
     {
          return;
     }

     if (json_output || RawMode)
     {
          try
          {
               std::cout << nlohmann::json::parse(response.Body).dump(2) << "\n";
          }
          catch (const std::exception &)
          {
               std::cout << response.Body << "\n";
          }
          return;
     }

     nlohmann::json root;
     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse maybe response");
          return;
     }

     std::string message = root.value("message", "");
     const size_t Found = root.value("found", static_cast<size_t>(0));

     if (Found >= static_cast<size_t>(min_results) && (!root.contains("maybe") || !root["maybe"].is_object()))
     {
          std::cout << "'" << query << "' matched " << Found << " document(s) in collection '" << collection_name << "'. ";
          std::cout << "Maybe suggestions were skipped because the result count is above the threshold of " << min_results << ".\n";
          if (!message.empty() && message != "ok")
          {
               std::cout << "Message: " << message << ".\n";
          }
          return;
     }

     PrintMaybeSuggestions(root, query, collection_name);
}

void HLQueryCLI::ShowDocumentContext(const std::string &collection_name, const std::string &document_id, bool json_output)
{
     if (collection_name.empty() || document_id.empty())
     {
          PrintError("Collection name and document ID are required", "Usage: sam <document-id>");
          return;
     }

     const std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) +
                              "/documents/" + hlquery_cli::UrlEncode(document_id) + "/context";

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response))
     {
          return;
     }

     if (json_output || RawMode)
     {
          try
          {
               std::cout << nlohmann::json::parse(response.Body).dump(2) << "\n";
          }
          catch (const std::exception &)
          {
               std::cout << response.Body << "\n";
          }
          return;
     }

     nlohmann::json root;
     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse document context response");
          return;
     }

     std::cout << "Context phrases for document '" << document_id << "' in collection '" << collection_name << "':.\n";

     if (root.value("pending", false))
     {
          std::cout << "Background context job is still pending.\n";
     }

     if (!root.contains("suggestions") || !root["suggestions"].is_array() || root["suggestions"].empty())
     {
          std::cout << "No contextual phrases available.\n";
          return;
     }

     std::vector<std::vector<std::string>> Rows;
     size_t Index = 1;

     for (const auto &Suggestion : root["suggestions"])
     {
          Rows.push_back({std::to_string(Index++),
                          Suggestion.value("text", ""),
                          Suggestion.value("kind", "")});
     }

     PrintTable({"#", "Phrase", "Kind"}, Rows);
}

void HLQueryCLI::RebuildSAMCollection(const std::string &collection_name, bool json_output)
{
     if (collection_name.empty())
     {
          PrintError("Collection name is required", "Usage: sam run <collection>");
          return;
     }

     const std::string path = "/sam/rebuild?collection=" + hlquery_cli::UrlEncode(collection_name);
     HLQueryCLI::HTTPResponse response = MakeRequest("POST", path, "", std::max(15, DefaultTimeoutSeconds));

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM rebuild response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     if (root.value("started", false))
     {
          std::cout << "SAM rebuild started in the background for collection '" << collection_name << "'.\n";
          return;
     }

     std::cout << "SAM rebuild is already running for collection '" << collection_name << "'.\n";
}

void HLQueryCLI::SearchSAM(const std::string &collection_name, const std::string &query, int limit, bool json_output)
{
     if (collection_name.empty() || query.empty())
     {
          PrintError("Collection and query are required", "Usage: sam search <collection> <query> [limit]");
          return;
     }

     if (limit <= 0)
     {
          limit = 20;
     }

     std::string path = "/sam/search?collection=" + hlquery_cli::UrlEncode(collection_name) +
                        "&q=" + hlquery_cli::UrlEncode(query) +
                        "&limit=" + std::to_string(limit);
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM search response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     const nlohmann::json &hits = root["hits"];

     std::cout << "SAM results for '" << query << "' in collection '" << collection_name << "':.\n";

     if (!hits.is_array() || hits.empty())
     {
          std::cout << "No SAM matches found.\n";
          return;
     }

     std::vector<std::vector<std::string>> rows;
     size_t index = 1;

     for (const auto &hit : hits)
     {
          std::ostringstream score_stream;
          score_stream << std::fixed << std::setprecision(2) << hit.value("score", 0.0);

          const std::string matched_path = hit.value("matched_path", "");
          const std::string term_origin = hit.value("term_origin", "");
          const int evidence_count = hit.value("evidence_count", 0);
          std::string provenance = matched_path.empty() ? hit.value("source", "") : matched_path;

          if (!term_origin.empty())
          {
               provenance += provenance.empty() ? term_origin : ":" + term_origin;
          }

          rows.push_back({
               std::to_string(index++),
               hit.value("id", ""),
               hit.value("title", ""),
               hit.value("term", ""),
               provenance,
               std::to_string(evidence_count),
               score_stream.str()
          });
     }

     PrintTable({"#", "ID", "Title", "Matched Term", "Provenance", "Ev", "Score"}, rows);

     std::cout << "Score breakdowns:\n";

     size_t detail_index = 1;
     for (const auto &hit : hits)
     {
          const nlohmann::json &breakdown = hit.contains("score_breakdown") ? hit["score_breakdown"] : nlohmann::json{};
          std::ostringstream detail;
          detail << std::fixed << std::setprecision(2)
                 << "#" << detail_index++
                 << " term=" << breakdown.value("term_score", 0.0)
                 << " source_doc=" << breakdown.value("source_doc_score", 0.0)
                 << " evidence=" << breakdown.value("evidence_bonus", 0.0)
                 << " doc_prior=" << breakdown.value("doc_prior", 0.0)
                 << " source_bonus=" << breakdown.value("source_doc_bonus", 0.0)
                 << " final=" << breakdown.value("final_score", hit.value("score", 0.0));

          std::cout << detail.str() << "\n";
     }
}

void HLQueryCLI::ShowSAMStatus(const std::string &collection_name, bool json_output)
{
     const std::string path = collection_name.empty()
          ? "/sam/status"
          : "/sam/status?collection=" + hlquery_cli::UrlEncode(collection_name);
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path, "", std::max(60, DefaultTimeoutSeconds));

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM status response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     if (collection_name.empty())
     {
          std::cout << "SAM background status:.\n";

          const nlohmann::json &collections = root["collections"];

          if (collections.is_array() && !collections.empty())
          {
               std::vector<std::vector<std::string>> rows;
               bool any_running = false;

               for (const auto &entry : collections)
               {
                    const bool running = entry.value("running", false);
                    any_running = any_running || running;

                    rows.push_back({
                         entry.value("collection", ""),
                         running ? "yes" : "no",
                         entry.value("completed", false) ? "yes" : "no",
                         std::to_string(entry.value("indexed", static_cast<size_t>(0))),
                         std::to_string(entry.value("failed", static_cast<size_t>(0))),
                         std::to_string(entry.value("pending", static_cast<size_t>(0))),
                         std::to_string(entry.value("total", static_cast<size_t>(0)))
                    });
               }

               PrintTable({"Collection", "Running", "Completed", "Indexed", "Failed", "Pending", "Total"}, rows);

               const nlohmann::json &running_collections = root["running_collections"];

               if (running_collections.is_array() && !running_collections.empty())
               {
                    std::vector<std::string> names;

                    for (const auto &entry : running_collections)
                    {
                         if (entry.is_string() && !entry.get<std::string>().empty())
                         {
                              names.push_back(entry.get<std::string>());
                         }
                    }

                    if (!names.empty())
                    {
                         std::ostringstream stream;

                         for (size_t index = 0; index < names.size(); ++index)
                         {
                              if (index > 0)
                              {
                                   stream << ", ";
                              }

                              stream << names[index];
                         }

                         std::cout << "Currently indexing: " << stream.str() << "\n";
                    }
               }
               else if (!any_running)
               {
                    std::cout << root.value("message", "No SAM collections are currently indexing.") << "\n";
               }
          }
          else
          {
               std::cout << root.value("message", "No SAM collections are currently indexing.") << "\n";
          }

          const nlohmann::json &active_searches = root["active_searches"];

          if (active_searches.is_array() && !active_searches.empty())
          {
               std::vector<std::vector<std::string>> rows;

               for (const auto &entry : active_searches)
               {
                    rows.push_back({
                         entry.value("collection", ""),
                         entry.value("query", ""),
                         std::to_string(entry.value("started_ms", static_cast<uint64_t>(0))),
                         entry.value("running", false) ? "yes" : "no"
                    });
               }

               std::cout << "Active SAM searches:\n";
               PrintTable({"Collection", "Query", "StartedMS", "Running"}, rows);
          }

          if (root.contains("latest_search") && root["latest_search"].is_object())
          {
               const nlohmann::json &latest = root["latest_search"];
               std::cout << "Latest SAM search: "
                         << latest.value("collection", "")
                         << " -> "
                         << latest.value("query", "")
                         << " (results="
                         << latest.value("result_count", static_cast<size_t>(0))
                         << ").\n";
          }

          return;
     }

     std::vector<std::vector<std::string>> rows;
     rows.push_back({"collection", root.value("collection", "")});
     rows.push_back({"known", root.value("known", false) ? "yes" : "no"});
     rows.push_back({"running", root.value("running", false) ? "yes" : "no"});
     rows.push_back({"completed", root.value("completed", false) ? "yes" : "no"});
     rows.push_back({"indexed", std::to_string(root.value("indexed", static_cast<size_t>(0)))});
     rows.push_back({"failed", std::to_string(root.value("failed", static_cast<size_t>(0)))});
     rows.push_back({"search_running", root.value("search_running", false) ? "yes" : "no"});
     rows.push_back({"active_search_count", std::to_string(root.value("active_search_count", static_cast<size_t>(0)))});

     const std::string error_value = root.value("error", "");

     if (!error_value.empty())
     {
          rows.push_back({"error", error_value});
     }

     rows.push_back({"message", root.value("message", "")});

     std::cout << "SAM status for collection '" << collection_name << "':.\n";
     PrintTable({"Field", "Value"}, rows);

     const nlohmann::json &active_searches = root["active_searches"];

     if (active_searches.is_array() && !active_searches.empty())
     {
          std::vector<std::vector<std::string>> search_rows;

          for (const auto &entry : active_searches)
          {
               search_rows.push_back({
                    entry.value("query", ""),
                    std::to_string(entry.value("started_ms", static_cast<uint64_t>(0))),
                    entry.value("running", false) ? "yes" : "no"
               });
          }

          std::cout << "Active SAM searches:\n";
          PrintTable({"Query", "StartedMS", "Running"}, search_rows);
     }

     if (root.contains("latest_search") && root["latest_search"].is_object())
     {
          const nlohmann::json &latest = root["latest_search"];
          std::cout << "Latest SAM search: "
                    << latest.value("query", "")
                    << " (results="
                    << latest.value("result_count", static_cast<size_t>(0))
                    << ").\n";
     }
}

void HLQueryCLI::ShowSAMHistory(const std::string &collection_name, int limit, bool json_output)
{
     if (limit <= 0)
     {
          limit = 100;
     }

     std::string path = "/sam/history?limit=" + std::to_string(limit);

     if (!collection_name.empty())
     {
          path += "&collection=" + hlquery_cli::UrlEncode(collection_name);
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path, "", std::max(60, DefaultTimeoutSeconds));

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM history response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     const nlohmann::json &history = root["history"];
     std::cout << "SAM recent search history";

     if (!collection_name.empty())
     {
          std::cout << " for collection '" << collection_name << "'";
     }

     std::cout << ":.\n";

     if (!history.is_array() || history.empty())
     {
          std::cout << "No SAM search history found.\n";
          return;
     }

     std::vector<std::vector<std::string>> rows;
     size_t index = 1;

     for (const auto &entry : history)
     {
          std::string best_match;
          std::string llm_intent = entry.value("resolved_interpretation", "");
          std::string conclusion = entry.value("resolved_conclusion", "");

          if (conclusion.empty())
          {
               conclusion = entry.value("conclusion", "");
          }

          if (entry.contains("best_match") && entry["best_match"].is_object())
          {
               const nlohmann::json &best = entry["best_match"];
               best_match = best.value("title", "");

               if (best_match.empty())
               {
                    best_match = best.value("id", "");
               }
          }

          if (best_match.empty() && entry.contains("suggestions") && entry["suggestions"].is_array())
          {
               for (const auto &suggestion : entry["suggestions"])
               {
                    if (suggestion.is_string() && !suggestion.get<std::string>().empty())
                    {
                         best_match = suggestion.get<std::string>();
                         break;
                    }
               }
          }

          rows.push_back({
               std::to_string(index++),
               entry.value("collection", ""),
               entry.value("query", ""),
               std::to_string(entry.value("uses", static_cast<uint64_t>(0))),
               llm_intent,
               best_match,
               conclusion
          });
     }

     PrintTable({"#", "Collection", "Query", "Uses", "LLM Intent", "Best Match", "Conclusion"}, rows);
}

void HLQueryCLI::ListSAMDocuments(const std::string &collection_name, int offset, int limit, bool json_output)
{
     if (collection_name.empty())
     {
          PrintError("Collection name is required", "Usage: sam ls [collection] [offset limit]");
          return;
     }

     if (offset < 0)
     {
          offset = 0;
     }

     if (limit <= 0)
     {
          limit = 20;
     }

     const std::string path = "/sam/documents?collection=" + hlquery_cli::UrlEncode(collection_name) +
                              "&offset=" + std::to_string(offset) +
                              "&limit=" + std::to_string(limit);
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM list response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     const nlohmann::json &documents = root["documents"];

     std::cout << "SAM indexed documents for collection '" << collection_name << "':.\n";

     if (root.value("rebuilding", false))
     {
          std::cout << "Background indexing is still running.\n";
     }

     if (!documents.is_array() || documents.empty())
     {
          std::cout << "No SAM documents found.\n";
          return;
     }

     std::vector<std::vector<std::string>> rows;
     size_t index = static_cast<size_t>(offset) + 1;

     for (const auto &entry : documents)
     {
          std::string terms_preview;

          if (entry.contains("terms") && entry["terms"].is_array())
          {
               size_t shown = 0;

               for (const auto &term : entry["terms"])
               {
                    if (!term.is_object())
                    {
                         continue;
                    }

                    if (!terms_preview.empty())
                    {
                         terms_preview += ", ";
                    }

                    const std::string text = term.value("text", "");
                    const double score = term.value("score", 0.0);

                    if (text.empty())
                    {
                         continue;
                    }

               std::ostringstream score_stream;
               score_stream << std::fixed << std::setprecision(2) << score;
               const std::string source = term.value("source", "");
               terms_preview += text + " (" + score_stream.str();
               if (!source.empty())
               {
                    terms_preview += ", " + source;
               }
               terms_preview += ")";
               shown++;

                    if (shown >= 2)
                    {
                         break;
                    }
               }
          }

          rows.push_back({
               std::to_string(index++),
               entry.value("id", ""),
               entry.value("title", ""),
               terms_preview
          });
     }

     PrintTable({"#", "ID", "Title", "Terms"}, rows);
}

void HLQueryCLI::OpenSAMDocument(const std::string &collection_name, const std::string &document_id, bool json_output)
{
     if (collection_name.empty() || document_id.empty())
     {
          PrintError("Collection name and document ID are required", "Usage: sam open <collection>/<document-id>");
          return;
     }

     const std::string path = "/sam/documents/" + hlquery_cli::UrlEncode(collection_name) +
                              "/" + hlquery_cli::UrlEncode(document_id);
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse SAM document response");
          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     std::vector<std::vector<std::string>> metadata_rows;
     metadata_rows.push_back({"collection", root.value("collection", "")});
     metadata_rows.push_back({"id", root.value("id", "")});
     metadata_rows.push_back({"title", root.value("title", "")});
     metadata_rows.push_back({"language", root.value("lang", "")});
     metadata_rows.push_back({"label", root.value("label", "")});
     metadata_rows.push_back({"format", root.value("format", "")});
     metadata_rows.push_back({"term_count", root.contains("terms") && root["terms"].is_array()
                                               ? std::to_string(root["terms"].size())
                                               : "0"});

     std::cout << "SAM entry for '" << collection_name << "/" << document_id << "':.\n";

     if (root.value("rebuilding", false))
     {
          std::cout << "Background indexing is still running for this collection.\n";
     }

     PrintTable({"Field", "Value"}, metadata_rows);

     if (root.contains("analysis") && root["analysis"].is_object())
     {
          const nlohmann::json &analysis = root["analysis"];
          std::vector<std::vector<std::string>> analysis_rows;

          if (!analysis.value("subject", "").empty())
          {
               analysis_rows.push_back({"subject", analysis.value("subject", "")});
          }

          if (!analysis.value("summary", "").empty())
          {
               analysis_rows.push_back({"about", analysis.value("summary", "")});
          }

          auto AppendAnalysisList = [&](const char *field_name, const char *label)
          {
               if (!analysis.contains(field_name) || !analysis[field_name].is_array() || analysis[field_name].empty())
               {
                    return;
               }

               std::vector<std::string> values;

               for (const auto &item : analysis[field_name])
               {
                    if (item.is_string() && !item.get<std::string>().empty())
                    {
                         values.push_back(item.get<std::string>());
                    }
               }

               if (!values.empty())
               {
                    analysis_rows.push_back({label, JoinDocumentStrings(values, ", ")});
               }
          };

          AppendAnalysisList("aliases", "aliases");
          AppendAnalysisList("descriptors", "descriptors");
          AppendAnalysisList("queries", "queries");

          if (!analysis_rows.empty())
          {
               std::cout << "Analysis:\n";
               PrintTable({"Field", "Value"}, analysis_rows);
          }
     }

     if (root.contains("terms") && root["terms"].is_array() && !root["terms"].empty())
     {
          std::cout << "Ranked terms:\n";
          std::vector<std::vector<std::string>> term_rows;
          size_t rank = 1;

          for (const auto &term : root["terms"])
          {
               if (!term.is_object())
               {
                    continue;
               }

               std::ostringstream score_stream;
               score_stream << std::fixed << std::setprecision(2) << term.value("score", 0.0);
               std::ostringstream signal_stream;
               signal_stream << std::fixed << std::setprecision(2) << term.value("signal", 0.0);

               term_rows.push_back({
                    std::to_string(rank++),
                    term.value("text", ""),
                    term.value("kind", ""),
                    term.value("source", ""),
                    score_stream.str(),
                    signal_stream.str()
               });
          }

          PrintTable({"#", "Term", "Kind", "Source", "Score", "Signal"}, term_rows);
     }

     if (root.contains("document") && root["document"].is_object() && !root["document"].empty())
     {
          std::cout << "Source document:\n";
          const nlohmann::json &doc = root["document"];
          std::vector<std::vector<std::string>> doc_rows;

          for (auto it = doc.begin(); it != doc.end(); ++it)
          {
               doc_rows.push_back({
                    it.key(),
                    it.value().is_string() ? it.value().get<std::string>() : it.value().dump()
               });
          }

          PrintTable({"Field", "Value"}, doc_rows);
     }
}

void HLQueryCLI::SearchAcrossCollections(const std::string &query, const std::vector<std::string> &collections, int limit, int offset, const std::string &sort, bool exact_match, bool highlight, const std::string &highlight_fields, const std::string &distributed, const std::string &route, bool distributed_collections, int maybe_min, int maybe_limit, bool json_output)
{
     if (query.empty())
     {
          PrintError("Query cannot be empty", "Provide a search string");

          return;
     }

     if (query.length() > 10000)
     {
          PrintError("Query too long", "Maximum query length is 10000 characters");

          return;
     }

     auto search_start = std::chrono::steady_clock::now();

     const int PAGE_SIZE_VAL = 100;

     int current_offset = offset;
     int remaining = limit;
     bool has_more = true;
     bool has_found_value = false;
     size_t total_found = 0;

     std::vector<std::vector<std::string>> all_rows;
     nlohmann::json aggregated_hits = nlohmann::json::array();
     nlohmann::json first_root;
     nlohmann::json last_root;

     std::string collections_desc = collections.empty() ? "all collections" : "collections '" + ([](const std::vector<std::string> &list)
                                                                                                 {
                                                                                                      std::ostringstream oss;
                                                                                                      for (size_t i = 0; i < list.size(); ++i)
                                                                                                      {
                                                                                                           if (i > 0)
                                                                                                           {
                                                                                                                oss << ",";
                                                                                                           }

                                                                                                           oss << list[i];
                                                                                                      }

                                                                                                      return oss.str();
                                                                                                 })(collections) +
                                                                                   "'";

     while (has_more && remaining > 0)
     {
          int page_limit = std::min(PAGE_SIZE_VAL, remaining);

          std::string path = "/search";

          std::string query_string = "q=" + hlquery_cli::UrlEncode(query) + "&limit=" + std::to_string(page_limit) + "&offset=" + std::to_string(current_offset);

          query_string += "&query_by=" + hlquery_cli::UrlEncode("title,content");

          if (!sort.empty())
          {
               query_string += "&sort_by=" + hlquery_cli::UrlEncode(sort);
          }

          if (exact_match)
          {
               query_string += "&prioritize_exact_match=true";
          }

          if (highlight)
          {
               query_string += "&highlight=true";

               if (!highlight_fields.empty())
               {
                    query_string += "&highlight_fields=" + hlquery_cli::UrlEncode(highlight_fields);
               }
          }

          if (!distributed.empty())
          {
               query_string += "&distributed=" + hlquery_cli::UrlEncode(distributed);
          }

          if (!route.empty())
          {
               query_string += "&route=" + hlquery_cli::UrlEncode(route);
          }

          if (distributed_collections)
          {
               query_string += "&distributed_collections=true";
          }

          if (!collections.empty())
          {
               std::ostringstream oss;

               for (size_t i = 0; i < collections.size(); ++i)
               {
                    if (i > 0)
                    {
                         oss << ",";
                    }

                    oss << collections[i];
               }

               query_string += "&collections=" + hlquery_cli::UrlEncode(oss.str());
          }

          if (maybe_min >= 0)
          {
               query_string += "&maybe_min=" + std::to_string(maybe_min);
          }

          if (maybe_limit > 0)
          {
               query_string += "&maybe_limit=" + std::to_string(maybe_limit);
          }

          HLQueryCLI::HTTPResponse response = MakeRequest("GET", path + "?" + query_string);

          if (CheckRequestFailed(response))
          {
               return;
          }

          nlohmann::json root;

          try
          {
               root = nlohmann::json::parse(response.Body);
          }
          catch (const std::exception &)
          {
               PrintError("Failed to parse search response");

               return;
          }

          if (!root.contains("hits") || !root["hits"].is_array())
          {
               PrintError("Invalid search response format", "Response missing 'hits' array");

               return;
          }

          if (first_root.is_null())
          {
               first_root = root;
          }

          if (root.contains("found") && root["found"].is_number_unsigned())
          {
               total_found = root["found"].get<size_t>();
               has_found_value = true;
          }

          last_root = root;

          nlohmann::json hits = root["hits"];

          if (json_output || RawMode)
          {
               for (const auto &hit : hits)
               {
                    aggregated_hits.push_back(hit);
               }
          }

          if (hits.empty())
          {
               has_more = false;
               break;
          }

          for (size_t i = 0; i < hits.size(); i++)
          {
               nlohmann::json hit = hits[i];

               std::string doc_id = "";

               if (hit.contains("document") && hit["document"].contains("id"))
               {
                    doc_id = hit["document"]["id"].get<std::string>();
               }
               else if (hit.contains("id"))
               {
                    doc_id = hit["id"].get<std::string>();
               }

               std::string score_str = "";

               if (hit.contains("text_match"))
               {
                    score_str = std::to_string(hit["text_match"].get<double>());
               }
               else if (hit.contains("score"))
               {
                    score_str = std::to_string(hit["score"].get<double>());
               }

               std::string title = "";
               std::string content = "";
               std::string fields_preview = "";
               std::string collection_name = "-";

               if (hit.contains("document"))
               {
                    nlohmann::json doc = hit["document"];

                    if (doc.contains("_collection"))
                    {
                         collection_name = doc["_collection"].get<std::string>();
                    }
                    else if (doc.contains("collection"))
                    {
                         collection_name = doc["collection"].get<std::string>();
                    }
                    else if (doc.contains("collection_id"))
                    {
                         collection_name = doc["collection_id"].get<std::string>();
                    }

                    if (doc.contains("title"))
                    {
                         title = doc["title"].get<std::string>();
                    }
                    else if (doc.contains("field_0"))
                    {
                         title = doc["field_0"].get<std::string>();
                    }
                    else if (doc.contains("name"))
                    {
                         title = doc["name"].get<std::string>();
                    }

                    if (doc.contains("content"))
                    {
                         content = doc["content"].get<std::string>();
                    }
                    else if (doc.contains("field_1"))
                    {
                         content = doc["field_1"].get<std::string>();
                    }
                    else if (doc.contains("description"))
                    {
                         content = doc["description"].get<std::string>();
                    }
                    else if (doc.contains("field_2"))
                    {
                         content = doc["field_2"].get<std::string>();
                    }

                    int field_count = 0;
                    std::vector<std::string> field_names;

                    for (auto &[key, value] : doc.items())
                    {
                         if (key == "id" || key == "title" || key == "content" || key == "collection_id" || key == "_collection" || key == "collection")
                         {
                              continue;
                         }

                         std::string field_value;

                         if (value.is_string())
                         {
                              field_value = value.get<std::string>();
                         }
                         else if (value.is_number_integer())
                         {
                              field_value = std::to_string(value.get<int64_t>());
                         }
                         else if (value.is_number_float())
                         {
                              field_value = std::to_string(value.get<double>());
                         }
                         else if (value.is_boolean())
                         {
                              field_value = value.get<bool>() ? "true" : "false";
                         }
                         else
                         {
                              field_value = value.dump();
                         }

                         if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                         {
                              field_count++;

                              if (field_names.size() < 3)
                              {
                                   field_names.push_back(key);
                              }
                         }
                    }

                    fields_preview = std::to_string(field_count) + " fields";

                    if (!field_names.empty())
                    {
                         fields_preview += " (" + field_names[0];

                         for (size_t j = 1; j < field_names.size(); j++)
                         {
                              fields_preview += ", " + field_names[j];
                         }

                         fields_preview += ")";
                    }
               }

               if (content.length() > 60)
               {
                    content = content.substr(0, 57) + "...";
               }

               if (title.length() > 30)
               {
                    title = title.substr(0, 27) + "...";
               }

               all_rows.push_back({std::to_string(all_rows.size() + 1), collection_name, doc_id, score_str, title, content, fields_preview});

               remaining--;

               if (remaining <= 0)
               {
                    has_more = false;
                    break;
               }
          }

          current_offset += static_cast<int>(hits.size());
          has_more = (static_cast<int>(hits.size()) == page_limit && remaining > 0);
     }

     if (json_output || RawMode)
     {
          nlohmann::json output = first_root.is_object() ? first_root : (last_root.is_object() ? last_root : nlohmann::json::object());
          auto search_end = std::chrono::steady_clock::now();
          auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();

          output["hits"] = aggregated_hits;

          if (has_found_value)
          {
               output["found"] = total_found;
          }
          else
          {
               output["found"] = aggregated_hits.size();
          }

          output["cli"] = {
              {"requested_query", query},
              {"requested_collections", collections},
              {"requested_limit", limit},
              {"requested_offset", offset},
              {"duration_ms", duration_ms}
          };

          std::cout << output.dump(2) << "\n";
          return;
     }

     if (!all_rows.empty())
     {
          std::vector<std::string> headers = {"#", "Collection", "Document ID", "Score", "Title", "Content Preview", "Fields"};

          std::cout << "Search results for '" << query << "' across " << collections_desc << ":\n";

          if (has_found_value)
          {
               std::cout << "Found " << total_found << " document(s)";
          }
          else
          {
               std::cout << "Found " << all_rows.size() << " document(s)";
          }

          if (offset > 0 || limit != 10000)
          {
               std::cout << " (showing " << (offset + 1) << "-" << (offset + static_cast<int>(all_rows.size())) << ").";
          }

          std::cout << "\n\n";

          PrintTable(headers, all_rows);
     }
     else
     {
          PrintInfo("No documents found matching your search across " + collections_desc);
     }

     auto search_end = std::chrono::steady_clock::now();
     auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();
     std::cout << "Search completed in " << duration_ms << " ms.\n";
}

/* Performs a vector search. */

void HLQueryCLI::VectorSearch(const std::string &collection_name, const std::string &vector_str, const std::string &field_name, int limit, bool json_output)
{
     std::string path = "/collections/" + collection_name + "/search";

     nlohmann::json search_request;

     search_request["vector_query"] = vector_str;
     search_request["limit"] = limit;
     search_request["remote_embedding_embedding_field"] = field_name;

     std::string body = search_request.dump();

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", path, body);

     if (CheckRequestFailed(response))
     {
          return;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          PrintError("Failed to parse vector search response");

          return;
     }

     if (!root.contains("hits") || !root["hits"].is_array())
     {
          PrintError("Invalid vector search response format");

          return;
     }

     if (json_output || RawMode)
     {
          std::cout << root.dump(2) << "\n";
          return;
     }

     nlohmann::json hits = root["hits"];

     size_t total_val = (root.contains("found") && root["found"].is_number_unsigned())
                            ? root["found"].get<size_t>()
                            : hits.size();

     std::cout << "Vector search results in collection '" << collection_name << "':.\n";
     std::cout << "Found " << hits.size() << " document(s) (showing " << hits.size() << " of " << total_val << ").\n\n";

     if (hits.size() == 0)
     {
          PrintInfo("No documents found with similar vectors");

          return;
     }

     std::vector<std::string> headers = {"#", "Document ID", "Similarity", "Title", "Content Preview", "Fields"};
     std::vector<std::vector<std::string>> rows;

     for (size_t i = 0; i < hits.size(); i++)
     {
          nlohmann::json hit = hits[i];

          std::string doc_id;

          if (hit.contains("document") && hit["document"].contains("id"))
          {
               doc_id = hit["document"]["id"].get<std::string>();
          }
          else if (hit.contains("id"))
          {
               doc_id = hit["id"].get<std::string>();
          }

          std::string similarity_str = "N/A";

          if (hit.contains("vector_score"))
          {
               std::ostringstream oss;

               oss << std::fixed << std::setprecision(4) << hit["vector_score"].get<double>();

               similarity_str = oss.str();
          }

          std::string title = "";
          std::string content = "";
          std::string fields_preview = "";

          if (hit.contains("document"))
          {
               nlohmann::json doc = hit["document"];

               if (doc.contains("title"))
               {
                    title = doc["title"].get<std::string>();
               }
               else if (doc.contains("field_0"))
               {
                    title = doc["field_0"].get<std::string>();
               }
               else if (doc.contains("name"))
               {
                    title = doc["name"].get<std::string>();
               }

               if (doc.contains("content"))
               {
                    content = doc["content"].get<std::string>();
               }
               else if (doc.contains("field_1"))
               {
                    content = doc["field_1"].get<std::string>();
               }
               else if (doc.contains("description"))
               {
                    content = doc["description"].get<std::string>();
               }
               else if (doc.contains("field_2"))
               {
                    content = doc["field_2"].get<std::string>();
               }

               int field_count = 0;

               std::vector<std::string> field_names;

               for (auto &[key, value] : doc.items())
               {
                    if (key == "id" || key == "title" || key == "content" || key == "collection_id")
                    {
                         continue;
                    }

                    std::string field_value;

                    if (value.is_string())
                    {
                         field_value = value.get<std::string>();
                    }
                    else if (value.is_number_integer())
                    {
                         field_value = std::to_string(value.get<int64_t>());
                    }
                    else if (value.is_number_float())
                    {
                         field_value = std::to_string(value.get<double>());
                    }
                    else if (value.is_boolean())
                    {
                         field_value = value.get<bool>() ? "true" : "false";
                    }
                    else
                    {
                         field_value = value.dump();
                    }

                    if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                    {
                         field_count++;

                         if (field_names.size() < 3)
                         {
                              field_names.push_back(key);
                         }
                    }
               }

               fields_preview = std::to_string(field_count) + " fields";

               if (!field_names.empty())
               {
                    fields_preview += " (" + field_names[0];

                    for (size_t j = 1; j < field_names.size(); j++)
                    {
                         fields_preview += ", " + field_names[j];
                    }

                    fields_preview += ")";
               }
          }

          if (content.length() > 50)
          {
               content = content.substr(0, 47) + "...";
          }

          if (title.length() > 30)
          {
               title = title.substr(0, 27) + "...";
          }

          rows.push_back({std::to_string(i + 1), doc_id, similarity_str, title, content, fields_preview});
     }

     PrintTable(headers, rows);
}

/* Gets available fields in a collection. */

std::string HLQueryCLI::GetAvailableFields(const std::string &collection_name)
{
     std::string path = "/collections/" + collection_name + "/documents?limit=1";

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return "";
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array() || root["documents"].empty())
          {
               return "";
          }

          nlohmann::json doc = root["documents"][0];

          std::vector<std::string> fields;

          if (doc.contains("fields") && doc["fields"].is_object())
          {
               for (const auto &[key, value] : doc["fields"].items())
               {
                    std::string field_value;

                    if (value.is_string())
                    {
                         field_value = value.get<std::string>();
                    }
                    else if (value.is_number())
                    {
                         field_value = std::to_string(value.get<double>());
                    }
                    else if (value.is_boolean())
                    {
                         field_value = value.get<bool>() ? "true" : "false";
                    }
                    else
                    {
                         field_value = value.dump();
                    }

                    if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                    {
                         fields.push_back(key);
                    }
               }
          }
          else
          {
               for (const auto &[key, value] : doc.items())
               {
                    if (key == "id" || key == "score" || key == "timestamp" || key == "created_at" || key == "collection_id")
                    {
                         continue;
                    }

                    std::string field_value;

                    if (value.is_string())
                    {
                         field_value = value.get<std::string>();
                    }
                    else if (value.is_number())
                    {
                         field_value = std::to_string(value.get<double>());
                    }
                    else if (value.is_boolean())
                    {
                         field_value = value.get<bool>() ? "true" : "false";
                    }
                    else
                    {
                         field_value = value.dump();
                    }

                    if (!field_value.empty() && field_value != "null" && field_value != "\"\"")
                    {
                         fields.push_back(key);
                    }
               }
          }

          std::string result;

          for (size_t i = 0; i < fields.size(); i++)
          {
               if (i > 0)
               {
                    result += ",";
               }

               result += fields[i];
          }

          return result;
     }
     catch (const std::exception &e)
     {
          return "";
     }
}

/* Deletes a specific document. */

void HLQueryCLI::DeleteDocument(const std::string &collection_name, const std::string &id)
{
     if (collection_name.empty() || id.empty())
     {
          PrintError("Invalid arguments", "Collection name and document ID are required");

          return;
     }

     HLQueryCLI::HTTPResponse response = MakeRequest("DELETE", "/collections/" + collection_name + "/documents/" + id);

     if (response.StatusCode == 200)
     {
          PrintSuccess("Document '" + id + "' deleted from collection '" + collection_name + "'");
     }
     else if (response.StatusCode == 404)
     {
          PrintError("Document '" + id + "' not found in collection '" + collection_name + "'");
     }
     else
     {
          CheckRequestFailed(response);
     }
}

/* Deletes documents by filter. */

void HLQueryCLI::DeleteDocumentsByFilter(const std::string &collection_name, const std::string &filter)
{
     if (collection_name.empty() || filter.empty())
     {
          PrintError("Invalid arguments", "Collection name and filter are required");

          return;
     }

     nlohmann::json body;

     body["query"] = filter;

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", "/collections/" + collection_name + "/documents/_delete_by_query", body.dump());

     if (response.StatusCode == 200)
     {
          try
          {
               nlohmann::json res = nlohmann::json::parse(response.Body);

               int deleted = res.value("deleted", 0);

               PrintSuccess("Deleted " + std::to_string(deleted) + " document(s) matching filter from collection '" + collection_name + "'");
          }
          catch (...)
          {
               PrintSuccess("Documents matching filter deleted from collection '" + collection_name + "'");
          }
     }
     else
     {
          CheckRequestFailed(response);
     }
}

/* Adds a document to a collection. */

void HLQueryCLI::AddDocument(const std::string &collection_name, const std::string &id, const std::string &title, const std::string &content, const std::map<std::string, std::string> &fields)
{
     nlohmann::json doc;

     doc["id"] = id;
     doc["title"] = title;
     doc["content"] = content;

     if (!fields.empty())
     {
          nlohmann::json fields_obj;

          for (const auto &field : fields)
          {
               fields_obj[field.first] = field.second;
          }

          doc["fields"] = fields_obj;
     }

     std::string json_str = doc.dump();

     HLQueryCLI::HTTPResponse response = MakeRequest("POST", "/collections/" + collection_name + "/documents", json_str);

     if (response.StatusCode == 201)
     {
          PrintSuccess("Document '" + id + "' added to collection '" + collection_name + "'");
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
