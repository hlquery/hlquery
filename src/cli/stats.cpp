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
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vendor/json/json.hpp>

#include "cli/cliutils.h"
#include "app.h"
#include "runtime/clock.h"
#include "utils/consolewriter.h"

namespace
{
std::string Pluralize(long long value, const char *singular, const char *plural)
{
     return std::to_string(value) + " " + (value == 1 ? singular : plural);
}

std::string FormatUptimeSeconds(long long uptime_seconds, bool detailed_format)
{
     long long uptime = std::max(0LL, uptime_seconds);
     long long days = uptime / 86400;
     long long hours = (uptime % 86400) / 3600;
     long long minutes = (uptime % 3600) / 60;
     long long seconds = uptime % 60;

     if (detailed_format)
     {
          std::ostringstream detail;
          detail << "Server up for " << days << "d " << hours << "h "
                 << minutes << "m " << seconds << "s";
          return detail.str();
     }

     std::ostringstream summary;
     summary << "Server up for " << Pluralize(days, "day", "days")
             << ", " << hours << "h " << minutes << "m " << seconds << "s";
     return summary.str();
}
}
/* Shows server status. */

void HLQueryCLI::ShowStatus()
{
     /* Build a table of key status values. */

     std::vector<std::vector<std::string>> rows;
     std::unordered_set<std::string> added_metrics;

     auto add_row = [&rows, &added_metrics](const std::string &label, const std::string &value)
     {
          if (added_metrics.insert(label).second)
          {
               rows.push_back({label, value});
          }
     };

     bool has_error = false;

     /* Fetch lightweight health details first. */

     HLQueryCLI::HTTPResponse health_response = MakeRequest("GET", "/health");

     if (health_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json health_root = nlohmann::json::parse(health_response.Body);

               add_row("Health", health_root["status"].get<std::string>());
               if (health_root.contains("socket_engine") && health_root["socket_engine"].is_string())
               {
                    add_row("Engine", health_root["socket_engine"].get<std::string>());
               }
               else
               {
                    add_row("Engine", health_root["engine"].get<std::string>());
               }

               add_row("Version", health_root["version"].get<std::string>());

               if (health_root.contains("auth_required"))
               {
                    add_row("Auth required", health_root["auth_required"].get<bool>() ? "yes" : "no");
               }
          }
          catch (const std::exception &e)
          {
               has_error = true;
          }
     }
     else
     {
          has_error = true;
     }

     auto stats_start = Now();
     HLQueryCLI::HTTPResponse stats_response = MakeRequest("GET", "/stats");
     auto stats_end = Now();
     auto ping_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stats_end - stats_start).count();

     /* Pull the rest of the server statistics. */

     if (stats_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json stats_root = nlohmann::json::parse(stats_response.Body);

               if (stats_root.contains("server") && stats_root["server"].contains("uptime_seconds"))
               {
                    add_row("Uptime", std::to_string(stats_root["server"]["uptime_seconds"].get<int>()) + " s");
               }
               else if (stats_root.contains("uptime_seconds"))
               {
                    add_row("Uptime", std::to_string(stats_root["uptime_seconds"].get<int>()) + " s");
               }

               if (stats_root.contains("collections") && stats_root["collections"].contains("total"))
               {
                    add_row("Collections", std::to_string(stats_root["collections"]["total"].get<size_t>()));
               }
               else if (stats_root.contains("collections_total"))
               {
                    add_row("Collections", std::to_string(stats_root["collections_total"].get<size_t>()));
               }

               if (stats_root.contains("server") && stats_root["server"].contains("memory_usage_bytes"))
               {
                    uint64_t memory_bytes = stats_root["server"]["memory_usage_bytes"].get<uint64_t>();

                    double memory_kb = static_cast<double>(memory_bytes) / 1024.0;

                    std::ostringstream mem_stream;

                    mem_stream << std::fixed << std::setprecision(2) << memory_kb << " KB";

                    add_row("Memory", mem_stream.str());
               }

               if (stats_root.contains("server") && stats_root["server"].contains("cpu_usage_percent"))
               {
                    std::ostringstream cpu_stream;

                    cpu_stream << std::fixed << std::setprecision(1) << stats_root["server"]["cpu_usage_percent"].get<double>() << "%";

                    add_row("CPU", cpu_stream.str());
               }

               if (stats_root.contains("cache"))
               {
                    auto cache_root = stats_root["cache"];

                    if (cache_root.contains("hits"))
                    {
                         add_row("Cache hits", std::to_string(cache_root["hits"].get<uint64_t>()));
                    }

                    if (cache_root.contains("hit_rate"))
                    {
                         std::ostringstream hit_rate_stream;

                         hit_rate_stream << std::fixed << std::setprecision(1) << cache_root["hit_rate"].get<double>() << "%";

                         add_row("Cache hit rate", hit_rate_stream.str());
                    }
               }

               if (stats_root.contains("indexing") && stats_root["indexing"].is_object())
               {
                    auto indexing_root = stats_root["indexing"];
                    bool in_progress = indexing_root.value("in_progress", false);
                    bool needs_loading = indexing_root.value("needs_loading", false);
                    double percent = indexing_root.value("percent_complete", 0.0);

                    std::ostringstream indexing_stream;
                    indexing_stream << std::fixed << std::setprecision(1) << percent << "%";

                    if (in_progress)
                    {
                         indexing_stream << " (in progress)";
                    }
                    else if (needs_loading)
                    {
                         indexing_stream << " (not loaded)";
                    }
                    else
                    {
                         indexing_stream << " (complete)";
                    }

                    add_row("Indexing", indexing_stream.str());

                    if (indexing_root.contains("documents_indexed") && indexing_root.contains("documents_total"))
                    {
                         size_t docs_indexed = indexing_root["documents_indexed"].get<size_t>();
                         size_t docs_total = indexing_root["documents_total"].get<size_t>();
                         std::ostringstream docs_stream;
                         docs_stream << docs_indexed << " / " << docs_total << " docs";
                         add_row("Documents indexed", docs_stream.str());
                    }

                    if (indexing_root.contains("collections_pending"))
                    {
                         size_t pending = indexing_root["collections_pending"].get<size_t>();
                         if (pending > 0)
                         {
                              add_row("Collections pending", std::to_string(pending));
                         }
                    }

                    if (indexing_root.contains("collections_indexing") && indexing_root["collections_indexing"].is_array())
                    {
                         auto arr = indexing_root["collections_indexing"];

                         if (!arr.empty())
                         {
                              std::ostringstream list_stream;
                              size_t names_to_show = std::min<size_t>(arr.size(), 3);
                              list_stream << arr[0].get<std::string>();

                              for (size_t idx = 1; idx < names_to_show; ++idx)
                              {
                                   list_stream << ", " << arr[idx].get<std::string>();
                              }

                              if (arr.size() > names_to_show)
                              {
                                   list_stream << " (and " << (arr.size() - names_to_show) << " more)";
                              }

                              add_row("Indexing collections", list_stream.str());
                         }
                    }

                    if (indexing_root.contains("collections_unloaded") && indexing_root["collections_unloaded"].is_array())
                    {
                         auto arr = indexing_root["collections_unloaded"];

                         if (!arr.empty())
                         {
                              std::ostringstream list_stream;
                              size_t names_to_show = std::min<size_t>(arr.size(), 3);
                              list_stream << arr[0].get<std::string>();

                              for (size_t idx = 1; idx < names_to_show; ++idx)
                              {
                                   list_stream << ", " << arr[idx].get<std::string>();
                              }

                              if (arr.size() > names_to_show)
                              {
                                   list_stream << " (and " << (arr.size() - names_to_show) << " more)";
                              }

                              add_row("Unloaded collections", list_stream.str());
                         }
                    }

                    if (indexing_root.contains("collections_partial") && indexing_root["collections_partial"].is_array())
                    {
                         auto arr = indexing_root["collections_partial"];

                         if (!arr.empty())
                         {
                              std::ostringstream list_stream;
                              size_t names_to_show = std::min<size_t>(arr.size(), 3);
                              list_stream << arr[0].get<std::string>();

                              for (size_t idx = 1; idx < names_to_show; ++idx)
                              {
                                   list_stream << ", " << arr[idx].get<std::string>();
                              }

                              if (arr.size() > names_to_show)
                              {
                                   list_stream << " (and " << (arr.size() - names_to_show) << " more)";
                              }

                              add_row("Partial collections", list_stream.str());
                         }
                    }
               }

               if (stats_root.contains("auth_required"))
               {
                    add_row("Auth required", stats_root["auth_required"].get<bool>() ? "yes" : "no");
               }
               else if (stats_root.contains("auth_enabled"))
               {
                    add_row("Auth required", stats_root["auth_enabled"].get<bool>() ? "yes" : "no");
               }
          }
          catch (const std::exception &e)
          {
               has_error = true;
          }
     }
     else if (stats_response.StatusCode == 401 || stats_response.StatusCode == 403)
     {
          add_row("Auth required", "yes");
     }
     else
     {
          has_error = true;
     }

     if (stats_response.StatusCode > 0)
     {
          add_row("Ping", std::to_string(ping_ms) + " ms");
     }

     if (has_error && rows.empty())
     {
          PrintError("Request failed");

          return;
     }

     if (!rows.empty())
     {
          PrintTable({"Metric", "Value"}, rows);
     }
}

/* Shows advanced server information. */

void HLQueryCLI::ShowAdvanced()
{
     std::cout << "Advanced Server Information:.\n\n";

     std::vector<std::string> headers = {"#", "Property", "Value"};
     std::vector<std::vector<std::string>> all_rows;

     int count_val = 1;

     auto add_row = [&](const std::string &p, const std::string &v)
     {
          all_rows.push_back({std::to_string(count_val++), p, v});
     };

     HLQueryCLI::HTTPResponse health_response = MakeRequest("GET", "/health");

     if (health_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json health_json = nlohmann::json::parse(health_response.Body);

               add_row("Health", (health_json.contains("status") ? health_json["status"].get<std::string>() : "unknown"));
               if (health_json.contains("socket_engine") && health_json["socket_engine"].is_string())
               {
                    add_row("Engine", health_json["socket_engine"].get<std::string>());
               }
               else
               {
                    add_row("Engine", (health_json.contains("engine") ? health_json["engine"].get<std::string>() : "unknown"));
               }
               add_row("Version", (health_json.contains("version") ? health_json["version"].get<std::string>() : "unknown"));
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     HLQueryCLI::HTTPResponse stats_response = MakeRequest("GET", "/stats");

     if (stats_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json stats_json = nlohmann::json::parse(stats_response.Body);

               if (stats_json.contains("uptime_seconds"))
               {
                    int uptime = std::max(0, stats_json["uptime_seconds"].get<int>());
                    int days = uptime / 86400;
                    int hours = (uptime % 86400) / 3600;
                    int minutes = (uptime % 3600) / 60;
                    int seconds = uptime % 60;

                    add_row("Uptime", std::to_string(days) + "d " + std::to_string(hours) + "h " +
                                           std::to_string(minutes) + "m " + std::to_string(seconds) + "s");
               }
               else if (stats_json.contains("server") && stats_json["server"].contains("uptime_seconds"))
               {
                    int uptime = std::max(0, stats_json["server"]["uptime_seconds"].get<int>());
                    int days = uptime / 86400;
                    int hours = (uptime % 86400) / 3600;
                    int minutes = (uptime % 3600) / 60;
                    int seconds = uptime % 60;

                    add_row("Uptime", std::to_string(days) + "d " + std::to_string(hours) + "h " +
                                           std::to_string(minutes) + "m " + std::to_string(seconds) + "s");
               }

               if (stats_json.contains("collections_total"))
               {
                    add_row("Total Collections", std::to_string(stats_json["collections_total"].get<size_t>()));
               }
               else if (stats_json.contains("collections") && stats_json["collections"].contains("total"))
               {
                    add_row("Total Collections", std::to_string(stats_json["collections"]["total"].get<size_t>()));
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     HLQueryCLI::HTTPResponse metrics_response = MakeRequest("GET", "/metrics");

     if (metrics_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json metrics_json = nlohmann::json::parse(metrics_response.Body);

               if (metrics_json.contains("startup"))
               {
                    auto startup = metrics_json["startup"];

                    if (startup.contains("collections_loaded"))
                    {
                         add_row("Collections Loaded", (startup["collections_loaded"].get<int>() > 0 ? "Yes" : "No"));
                    }

                    if (startup.contains("lazy_loading_fallback"))
                    {
                         add_row("Lazy Loading Fallback", (startup["lazy_loading_fallback"].get<bool>() ? "Yes" : "No"));
                    }
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     HLQueryCLI::HTTPResponse storage_response = MakeRequest("GET", "/admin/storage_status");

     if (storage_response.StatusCode == 200)
     {
          try
          {
               nlohmann::json storage_json = nlohmann::json::parse(storage_response.Body);

               if (storage_json.contains("total_collections"))
               {
                    add_row("Storage Collections", std::to_string(storage_json["total_collections"].get<uint64_t>()));
               }

               if (storage_json.contains("total_documents"))
               {
                    add_row("Storage Documents", std::to_string(storage_json["total_documents"].get<uint64_t>()));
               }

               if (storage_json.contains("sstable_count"))
               {
                    add_row("SSTable Count", std::to_string(storage_json["sstable_count"].get<int>()));
               }

               if (storage_json.contains("sstable_size_bytes"))
               {
                    double sstable_gb = static_cast<double>(storage_json["sstable_size_bytes"].get<uint64_t>()) / (1024.0 * 1024.0 * 1024.0);

                    std::stringstream ss;

                    ss << std::fixed << std::setprecision(2) << sstable_gb << " GB";

                    add_row("SSTable Size", ss.str());
               }

               if (storage_json.contains("wal_size_bytes"))
               {
                    double wal_mb = static_cast<double>(storage_json["wal_size_bytes"].get<uint64_t>()) / (1024.0 * 1024.0);

                    std::stringstream ss;

                    ss << std::fixed << std::setprecision(2) << wal_mb << " MB";

                    add_row("WAL Size", ss.str());
               }

               if (storage_json.contains("checkpoint_exists"))
               {
                    add_row("Checkpoint", (storage_json["checkpoint_exists"].get<bool>() ? "Yes" : "No"));
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     PrintTable(headers, all_rows);

     std::cout << "\n";
}

/* Shows server uptime. */

void HLQueryCLI::ShowUptime(bool detailed_format)
{
     HLQueryCLI::HTTPResponse stats_response = MakeRequest("GET", "/stats");

     if (CheckRequestFailed(stats_response))
     {
          return;
     }

     try
     {
          nlohmann::json stats_root = nlohmann::json::parse(stats_response.Body);
          long long uptime_seconds = -1;

          if (stats_root.contains("uptime_seconds") && stats_root["uptime_seconds"].is_number_integer())
          {
               uptime_seconds = stats_root["uptime_seconds"].get<long long>();
          }
          else if (stats_root.contains("server") && stats_root["server"].contains("uptime_seconds") &&
                   stats_root["server"]["uptime_seconds"].is_number_integer())
          {
               uptime_seconds = stats_root["server"]["uptime_seconds"].get<long long>();
          }

          if (uptime_seconds >= 0)
          {
               std::cout << FormatUptimeSeconds(uptime_seconds, detailed_format) << "\n";
          }
          else
          {
               PrintError("Missing uptime information in server response");
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse server stats");
     }
}

/* Shows total document and collection counts and checks consistency. */

void HLQueryCLI::ShowDocTotal(int offset, int limit)
{
     size_t doctotal_doctotal = 0;
     size_t coltotal_doctotal = 0;
     size_t doctotal_stats = 0;
     size_t coltotal_stats = 0;
     size_t coltotal_collections = 0;

     bool has_mismatch = false;

     HLQueryCLI::HTTPResponse doctotal_resp = MakeRequest("GET", "/doctotal", "", DefaultTimeoutSeconds);

     if (CheckRequestFailed(doctotal_resp, false, "/doctotal"))
     {
          return;
     }

     try
     {
          nlohmann::json doctotal_json = nlohmann::json::parse(doctotal_resp.Body);

          if (doctotal_json.contains("doctotal") && doctotal_json["doctotal"].is_number())
          {
               doctotal_doctotal = doctotal_json["doctotal"].get<size_t>();
          }

          if (doctotal_json.contains("coltotal") && doctotal_json["coltotal"].is_number())
          {
               coltotal_doctotal = doctotal_json["coltotal"].get<size_t>();
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse /doctotal response: " + std::string(e.what()));

          SetExitCode(2);

          return;
     }

     HLQueryCLI::HTTPResponse stats_resp = MakeRequest("GET", "/stats", "", DefaultTimeoutSeconds);

     if (stats_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json stats_json = nlohmann::json::parse(stats_resp.Body);

               if (stats_json.contains("collections") && stats_json["collections"].is_object())
               {
                    auto &cols = stats_json["collections"];

                    if (cols.contains("total") && cols["total"].is_number())
                    {
                         coltotal_stats = cols["total"].get<size_t>();
                    }

                    if (cols.contains("documents_total") && cols["documents_total"].is_number())
                    {
                         doctotal_stats = cols["documents_total"].get<size_t>();
                    }
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     HLQueryCLI::HTTPResponse collections_resp = MakeRequest("GET", "/collections", "", DefaultTimeoutSeconds);

     if (collections_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json collections_json = nlohmann::json::parse(collections_resp.Body);

               if (collections_json.contains("collections") && collections_json["collections"].is_array())
               {
                    coltotal_collections = collections_json["collections"].size();
               }
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     if (doctotal_stats > 0 && doctotal_doctotal != doctotal_stats)
     {
          ConsoleWriter::WriteError("ERROR: Document count mismatch!", false);
          ConsoleWriter::WriteError("  /doctotal reports: " + std::to_string(doctotal_doctotal) + " documents.", true);
          ConsoleWriter::WriteError("  /stats reports: " + std::to_string(doctotal_stats) + " documents.", true);

          has_mismatch = true;
     }

     if (coltotal_stats > 0 && coltotal_doctotal != coltotal_stats)
     {
          ConsoleWriter::WriteError("ERROR: Collection count mismatch!", false);
          ConsoleWriter::WriteError("  /doctotal reports: " + std::to_string(coltotal_doctotal) + " collections.", true);
          ConsoleWriter::WriteError("  /stats reports: " + std::to_string(coltotal_stats) + " collections.", true);

          has_mismatch = true;
     }

     if (coltotal_collections > 0 && coltotal_doctotal != coltotal_collections)
     {
          ConsoleWriter::WriteError("ERROR: Collection count mismatch!", false);
          ConsoleWriter::WriteError("  /doctotal reports: " + std::to_string(coltotal_doctotal) + " collections.", true);
          ConsoleWriter::WriteError("  /collections reports: " + std::to_string(coltotal_collections) + " collections.", true);

          has_mismatch = true;
     }

     std::cout << doctotal_doctotal << " doctotal and " << coltotal_doctotal << " coltotal.\n";

     if (doctotal_stats > 0 || coltotal_stats > 0 || coltotal_collections > 0)
     {
          std::cout << "\nCross-check results:.\n";

          if (doctotal_stats > 0)
          {
               std::cout << "  /stats: " << doctotal_stats << " docs, " << coltotal_stats << " cols.\n";
          }

          if (coltotal_collections > 0)
          {
               std::cout << "  /collections: " << coltotal_collections << " cols.\n";
          }
     }

     if (has_mismatch)
     {
          ConsoleWriter::WriteError("FATAL: Data inconsistency detected! Counts do not match across endpoints.", true);
          ConsoleWriter::WriteError("  This indicates a serious data integrity issue.", true);
          ConsoleWriter::WriteError("  Run '" + ProgramName + " rebuild-counters' to fix counters, or investigate data loss.", true);

          SetExitCode(3);
     }
}

/* Shows WAL statistics. */

void HLQueryCLI::ShowWALStats(const std::string &collection_name)
{
     HLQueryCLI::HTTPResponse startup_resp = MakeRequest("GET", "/startup", "", DefaultTimeoutSeconds);

     if (startup_resp.StatusCode != 200)
     {
          PrintError("Failed to get WAL statistics from server");

          return;
     }

     try
     {
          nlohmann::json startup_json = nlohmann::json::parse(startup_resp.Body);

          if (!startup_json.contains("wal_replay"))
          {
               PrintInfo("WAL replay statistics not available");

               return;
          }

          nlohmann::json wal_replay = startup_json["wal_replay"];

          std::unordered_map<std::string, nlohmann::json> per_collection;

          if (wal_replay.contains("per_collection_entries") && wal_replay["per_collection_entries"].is_object())
          {
               for (auto &[col, count] : wal_replay["per_collection_entries"].items())
               {
                    if (!collection_name.empty() && col != collection_name)
                    {
                         continue;
                    }

                    per_collection[col] = nlohmann::json::object();
                    per_collection[col]["entries"] = count.get<uint64_t>();
               }
          }

          if (wal_replay.contains("per_collection_puts") && wal_replay["per_collection_puts"].is_object())
          {
               for (auto &[col, count] : wal_replay["per_collection_puts"].items())
               {
                    if (!collection_name.empty() && col != collection_name)
                    {
                         continue;
                    }

                    if (per_collection.find(col) == per_collection.end())
                    {
                         per_collection[col] = nlohmann::json::object();
                    }

                    per_collection[col]["puts"] = count.get<uint64_t>();
               }
          }

          if (wal_replay.contains("per_collection_deletes") && wal_replay["per_collection_deletes"].is_object())
          {
               for (auto &[col, count] : wal_replay["per_collection_deletes"].items())
               {
                    if (!collection_name.empty() && col != collection_name)
                    {
                         continue;
                    }

                    if (per_collection.find(col) == per_collection.end())
                    {
                         per_collection[col] = nlohmann::json::object();
                    }

                    per_collection[col]["deletes"] = count.get<uint64_t>();
               }
          }

          std::unordered_map<std::string, uint64_t> last_lsn_map;

          if (startup_json.contains("wal_index") && startup_json["wal_index"].is_object())
          {
               try
               {
                    for (auto &[col, entry] : startup_json["wal_index"].items())
                    {
                         if (!collection_name.empty() && col != collection_name)
                         {
                              continue;
                         }

                         if (entry.is_object() && entry.contains("last_applied_lsn"))
                         {
                              last_lsn_map[col] = entry["last_applied_lsn"].get<uint64_t>();
                         }
                    }
               }
               catch (...)
               {
                    /* Ignore. */
               }
          }

          std::vector<std::vector<std::string>> rows;

          for (const auto &[col, stats] : per_collection)
          {
               uint64_t entries = stats.contains("entries") ? stats["entries"].get<uint64_t>() : 0;
               uint64_t puts = stats.contains("puts") ? stats["puts"].get<uint64_t>() : 0;
               uint64_t deletes = stats.contains("deletes") ? stats["deletes"].get<uint64_t>() : 0;
               uint64_t last_lsn = last_lsn_map.find(col) != last_lsn_map.end() ? last_lsn_map[col] : 0;

               uint64_t estimated_bytes = entries * 100;

               rows.push_back({col,
                               std::to_string(last_lsn),
                               std::to_string(entries),
                               std::to_string(puts),
                               std::to_string(deletes),
                               FormatBytes(estimated_bytes)});
          }

          if (!rows.empty())
          {
               std::vector<std::string> headers = {"Collection", "Last LSN", "Entries", "PUTs", "DELETEs", "Est. Bytes"};

               std::cout << "\nPer-Collection WAL Statistics:.\n\n";

               PrintTable(headers, rows);

               uint64_t total_entries = wal_replay.contains("total_entries_replayed") ? wal_replay["total_entries_replayed"].get<uint64_t>() : 0;
               uint64_t total_skipped = wal_replay.contains("total_entries_skipped") ? wal_replay["total_entries_skipped"].get<uint64_t>() : 0;
               uint64_t max_lsn = wal_replay.contains("max_replayed_lsn") ? wal_replay["max_replayed_lsn"].get<uint64_t>() : 0;

               std::cout << "\nSummary:.\n";
               std::cout << "  Total entries replayed: " << total_entries << ".\n";
               std::cout << "  Total entries skipped: " << total_skipped << ".\n";
               std::cout << "  Max replayed LSN: " << max_lsn << ".\n";
               std::cout << "  Collections with WAL entries: " << rows.size() << ".\n";
          }
          else
          {
               PrintInfo("No WAL statistics available for " + (collection_name.empty() ? "any collection" : "collection '" + collection_name + "'"));
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse WAL statistics: " + std::string(e.what()));
     }
}

/* Shows database size. */

void HLQueryCLI::ShowDatabaseSize(const std::string &unit)
{
     HLQueryCLI::HTTPResponse stats_response = MakeRequest("GET", "/stats");

     if (CheckRequestFailed(stats_response))
     {
          return;
     }

     try
     {
          nlohmann::json stats_root = nlohmann::json::parse(stats_response.Body);

          auto formatBytes = [&unit](long long bytes) -> std::string
          {
               double value = 0.0;

               std::string unit_str = "";

               if (unit == "kb")
               {
                    value = bytes / 1024.0;
                    unit_str = "KB";
               }
               else if (unit == "gb")
               {
                    value = bytes / (1024.0 * 1024.0 * 1024.0);
                    unit_str = "GB";
               }
               else
               {
                    value = bytes / (1024.0 * 1024.0);
                    unit_str = "MB";
               }

               std::ostringstream stream;

               if (value < 0.01 && value > 0)
               {
                    stream << std::fixed << std::setprecision(4) << value << " " << unit_str;
               }
               else if (value < 1.0)
               {
                    stream << std::fixed << std::setprecision(2) << value << " " << unit_str;
               }
               else
               {
                    stream << std::fixed << std::setprecision(2) << value << " " << unit_str;
               }

               return stream.str();
          };

          long long total_size_bytes = 0;
          long long documents_size_bytes = 0;
          long long sstable_size_bytes = 0;
          long long memtable_size_bytes = 0;
          long long rocksdb_size_bytes = 0;
          long long legacy_size_bytes = 0;

          int sstable_count = 0;

          long long io_bytes_processed = 0;

          if (stats_root.contains("lsm"))
          {
               auto &lsm = stats_root["lsm"];

               if (lsm.contains("total_size") && lsm["total_size"].is_number())
               {
                    total_size_bytes = lsm["total_size"].get<long long>();
               }

               if (lsm.contains("documents_size") && lsm["documents_size"].is_number())
               {
                    documents_size_bytes = lsm["documents_size"].get<long long>();
               }

               if (lsm.contains("bytes_written") && lsm["bytes_written"].is_number())
               {
                    sstable_size_bytes = lsm["bytes_written"].get<long long>();
               }

               if (lsm.contains("memtable_size") && lsm["memtable_size"].is_number())
               {
                    memtable_size_bytes = lsm["memtable_size"].get<long long>();
               }

               if (lsm.contains("sstable_count") && lsm["sstable_count"].is_number())
               {
                    sstable_count = lsm["sstable_count"].get<int>();
               }

               if (lsm.contains("rocksdb_size") && lsm["rocksdb_size"].is_number())
               {
                    rocksdb_size_bytes = lsm["rocksdb_size"].get<long long>();
               }

               if (lsm.contains("legacy_size") && lsm["legacy_size"].is_number())
               {
                    legacy_size_bytes = lsm["legacy_size"].get<long long>();
               }
          }

          if (stats_root.contains("io"))
          {
               auto &io = stats_root["io"];

               if (io.contains("total_bytes_processed") && io["total_bytes_processed"].is_number())
               {
                    io_bytes_processed = io["total_bytes_processed"].get<long long>();
               }
          }

          std::vector<std::vector<std::string>> rows;

          auto add_row = [&rows](const std::string &label, const std::string &value)
          {
               rows.push_back({label, value});
          };

          if (total_size_bytes > 0)
          {
               add_row("Total Database Size", formatBytes(total_size_bytes));
          }
          else
          {
               long long calculated_total = documents_size_bytes + sstable_size_bytes + memtable_size_bytes + rocksdb_size_bytes + legacy_size_bytes;

               if (calculated_total > 0)
               {
                    add_row("Total Database Size", formatBytes(calculated_total));
               }
          }

          if (documents_size_bytes > 0)
          {
               add_row("Documents (on disk)", formatBytes(documents_size_bytes));
          }

          if (rocksdb_size_bytes > 0)
          {
               add_row("RocksDB (total usage)", formatBytes(rocksdb_size_bytes));
          }

          if (sstable_size_bytes > 0)
          {
               std::ostringstream sst_stream;

               sst_stream << formatBytes(sstable_size_bytes) << " (" << sstable_count << " SSTable files)";

               add_row("Indexes (SSTables)", sst_stream.str());
          }

          if (legacy_size_bytes > 0)
          {
               add_row("Legacy Storage", formatBytes(legacy_size_bytes));
          }

          if (memtable_size_bytes > 0)
          {
               add_row("Memory (MemTable)", formatBytes(memtable_size_bytes));
          }

          if (io_bytes_processed > 0)
          {
               add_row("Total I/O Processed", formatBytes(io_bytes_processed));
          }

          if (!rows.empty())
          {
               PrintTable({"Metric", "Value"}, rows);
          }
          else
          {
               PrintInfo("No database size information available.");
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse database stats: " + std::string(e.what()));
     }
}

/* Shows transfer statistics. */

void HLQueryCLI::ShowTransferStats(const std::string &unit)
{
     HLQueryCLI::HTTPResponse stats_response = MakeRequest("GET", "/stats");

     if (CheckRequestFailed(stats_response))
     {
          return;
     }

     try
     {
          nlohmann::json stats_root = nlohmann::json::parse(stats_response.Body);

          uint64_t total_bytes_processed = 0;

          if (stats_root.contains("io"))
          {
               if (stats_root["io"].contains("total_bytes_processed"))
               {
                    total_bytes_processed = std::stoull(stats_root["io"]["total_bytes_processed"].dump());
               }
          }

          double size_value = 0.0;

          std::string unit_str = "";

          int precision = 2;

          if (unit == "gb")
          {
               size_value = static_cast<double>(total_bytes_processed) / (1024.0 * 1024.0 * 1024.0);
               unit_str = "GB";

               if (size_value < 0.01 && size_value > 0.0)
               {
                    precision = 6;
               }
               else if (size_value < 1.0)
               {
                    precision = 4;
               }
               else
               {
                    precision = 2;
               }
          }
          else if (unit == "mb")
          {
               size_value = static_cast<double>(total_bytes_processed) / (1024.0 * 1024.0);
               unit_str = "MB";
          }
          else
          {
               size_value = static_cast<double>(total_bytes_processed) / 1024.0;
               unit_str = "KB";
          }

          std::ostringstream size_stream;

          size_stream << std::fixed << std::setprecision(precision) << size_value << " " << unit_str;

          std::cout << "Total bytes transferred: " << size_stream.str() << ".\n";
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse transfer stats");
     }
}

/* Shows active connections. */

void HLQueryCLI::ShowConnections()
{
     HLQueryCLI::HTTPResponse conn_response = MakeRequest("GET", "/connections");

     if (CheckRequestFailed(conn_response))
     {
          return;
     }

     try
     {
          nlohmann::json conn_root = nlohmann::json::parse(conn_response.Body);

          if (conn_root.contains("active_connections"))
          {
               uint64_t active_connections = std::stoull(conn_root["active_connections"].dump());

               std::cout << "ACTIVE Connections: " << active_connections << ".\n";
          }
          else
          {
               PrintError("Missing active_connections in response");
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse connections response");
     }
}

/* Shows LSM information. */

void HLQueryCLI::ShowLSM()
{
     HLQueryCLI::HTTPResponse lsm_response = MakeRequest("GET", "/rocksdb");

     if (CheckRequestFailed(lsm_response))
     {
          return;
     }

     try
     {
          nlohmann::json lsm_root = nlohmann::json::parse(lsm_response.Body);

          std::vector<std::vector<std::string>> rows;

          auto add_row = [&rows](const std::string &label, const std::string &value)
          {
               rows.push_back({label, value});
          };

          if (lsm_root.contains("memory"))
          {
               auto mem = lsm_root["memory"];

               if (mem.contains("memtable_size_kb"))
               {
                    double mem_kb = mem["memtable_size_kb"].get<double>();

                    std::ostringstream mem_stream;

                    mem_stream << std::fixed << std::setprecision(2) << mem_kb << " KB";

                    add_row("MemTable Size", mem_stream.str());
               }
          }

          if (lsm_root.contains("disk"))
          {
               auto disk = lsm_root["disk"];

               if (disk.contains("sstable_count"))
               {
                    add_row("SSTable Files", std::to_string(disk["sstable_count"].get<int>()));
               }

               if (disk.contains("sstable_size_mb"))
               {
                    double disk_mb = disk["sstable_size_mb"].get<double>();

                    std::ostringstream disk_stream;

                    disk_stream << std::fixed << std::setprecision(2) << disk_mb << " MB";

                    add_row("SSTable Size", disk_stream.str());
               }
          }

          if (lsm_root.contains("documents"))
          {
               auto docs = lsm_root["documents"];

               if (docs.contains("total_documents"))
               {
                    add_row("Total Documents", std::to_string(docs["total_documents"].get<int>()));
               }

               if (docs.contains("total_collections"))
               {
                    add_row("Total Collections", std::to_string(docs["total_collections"].get<int>()));
               }

               if (docs.contains("docs_in_memory"))
               {
                    add_row("Docs in Memory", std::to_string(docs["docs_in_memory"].get<int>()));
               }

               if (docs.contains("docs_on_disk"))
               {
                    add_row("Docs on Disk", std::to_string(docs["docs_on_disk"].get<int>()));
               }
          }

          if (lsm_root.contains("performance"))
          {
               auto perf = lsm_root["performance"];

               if (perf.contains("index_size_mb"))
               {
                    double index_mb = perf["index_size_mb"].get<double>();

                    std::ostringstream index_stream;

                    index_stream << std::fixed << std::setprecision(2) << index_mb << " MB";

                    add_row("Index Size", index_stream.str());
               }
          }

          if (!rows.empty())
          {
               PrintTable({"Metric", "Value"}, rows);
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse LSM response: " + std::string(e.what()));
     }
}

/* Shows ping information. */

void HLQueryCLI::ShowPing()
{
     auto start_time_val = Now();

     HLQueryCLI::HTTPResponse response = MakeRequest("GET", "/ping");

     auto end_time_val = Now();

     auto duration_val = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - start_time_val);

     int64_t ping_time_ms = duration_val.count();

     if (response.StatusCode == 200)
     {
          if (ping_time_ms >= 1000)
          {
               double ping_time_s = ping_time_ms / 1000.0;

               std::cout << std::fixed << std::setprecision(2) << ping_time_s << " s" << std::endl;
          }
          else
          {
               std::cout << ping_time_ms << " ms" << std::endl;
          }
     }
     else
     {
          if (ping_time_ms >= 1000)
          {
               double ping_time_s = ping_time_ms / 1000.0;

               std::cout << std::fixed << std::setprecision(2) << ping_time_s << " s (error: HTTP " << response.StatusCode << ")" << std::endl;
          }
          else
          {
               std::cout << ping_time_ms << " ms (error: HTTP " << response.StatusCode << ")" << std::endl;
          }

          CheckRequestFailed(response);
     }
}

/* Shows configured distributed links and optional ping summary. */

void HLQueryCLI::ShowLinks(bool ping_all)
{
     const std::string endpoint = ping_all ? "/links/ping" : "/links";
     if (ping_all)
     {
          std::cout << "Pinging configured links...\n";
     }
     HLQueryCLI::HTTPResponse response = MakeRequest("GET", endpoint);

     if (CheckRequestFailed(response, false, endpoint))
     {
          return;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          nlohmann::json distributed = root;
          if (root.contains("distributed_search") && root["distributed_search"].is_object())
          {
               distributed = root["distributed_search"];
          }

          if (!distributed.is_object())
          {
               PrintInfo("Distributed link metadata is not available on this server.");
               return;
          }
          bool enabled = distributed.value("enabled", false);
          std::string mode = distributed.value("mode", "unknown");

          std::cout << "Distributed search: " << (enabled ? "enabled" : "disabled")
                    << " (mode: " << mode << ")\n";

          nlohmann::json nodes = nlohmann::json::array();
          if (distributed.contains("nodes") && distributed["nodes"].is_array())
          {
               nodes = distributed["nodes"];
          }

          nlohmann::json slaves = nlohmann::json::array();
          if (distributed.contains("slaves") && distributed["slaves"].is_array())
          {
               slaves = distributed["slaves"];
          }

          if (nodes.empty() && slaves.empty())
          {
               PrintInfo("No links configured.");
               return;
          }

          std::vector<std::vector<std::string>> rows;
          size_t reachable_count = 0;
          size_t total_count = 0;

          auto append_rows = [&](const nlohmann::json &entries, const std::string &purpose)
          {
               for (const auto &node : entries)
               {
                    total_count++;

                    std::string node_endpoint = node.value("endpoint", "unknown");
                    std::string host = node.value("host", "");
                    int port = node.value("port", 0);
                    bool reachable = node.value("reachable", false);
                    int status_code = node.value("status_code", 0);
                    std::string error = node.value("error", "");
                    double latency_ms = node.value("latency_ms", 0.0);

                    if (reachable)
                    {
                         reachable_count++;
                    }

                    std::ostringstream target_stream;
                    if (!host.empty() && port > 0)
                    {
                         target_stream << host << ":" << port;
                    }
                    else
                    {
                         target_stream << "-";
                    }

                    std::ostringstream latency_stream;
                    if (latency_ms > 0.0)
                    {
                         latency_stream << std::fixed << std::setprecision(1) << latency_ms << " ms";
                    }
                    else
                    {
                         latency_stream << "-";
                    }

                    std::ostringstream status_stream;
                    if (status_code > 0)
                    {
                         status_stream << status_code;
                    }
                    else
                    {
                         status_stream << "-";
                    }

                    rows.push_back({purpose,
                                    node_endpoint,
                                    target_stream.str(),
                                    reachable ? "yes" : "no",
                                    latency_stream.str(),
                                    status_stream.str(),
                                    error.empty() ? "-" : error});
               }
          };

          append_rows(nodes, "distributed_search");
          append_rows(slaves, "slave");

          PrintTable({"Purpose", "Endpoint", "Target", "Reachable", "Latency", "HTTP", "Error"}, rows);

          std::cout << "\nReachable links: " << reachable_count << "/" << total_count << "\n";

          if (ping_all && reachable_count < total_count)
          {
               SetExitCode(2);
          }
     }
     catch (const std::exception &e)
     {
          PrintError("Failed to parse /status response: " + std::string(e.what()));
     }
}
