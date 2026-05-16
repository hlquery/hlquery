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
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"

/* Defines and implements global variables for benchmarking. */

std::vector<std::string> FAKE_STOPWORDS = {"for", "a", "an"};

std::atomic<bool> g_benchmark_should_stop{false};

std::atomic<bool> g_flood_should_stop{false};

std::string g_collection_prefix = "bench_collection_";

bool verbose_mode = false;

std::atomic<int> spinner_index{0};

std::atomic<int> last_printed_percent{-1};

std::mutex progress_bar_mutex;

std::mutex console_mutex;

std::ofstream *log_file_stream = nullptr;

std::mutex log_mutex;

std::atomic<int> collections_created{0};

std::atomic<int> documents_inserted{0};

std::atomic<int> additional_documents_inserted{0};

std::atomic<int> collections_skipped{0};

std::atomic<int> documents_skipped{0};

std::atomic<int> additional_documents_skipped{0};

AdvancedMetrics advanced_metrics;

std::mutex advanced_metrics_mutex;

/* Helper functions for reporting. */

void LogError(const std::string &message)
{
     std::lock_guard<std::mutex> lock(log_mutex);

     std::cerr << message << std::flush;

     if (log_file_stream && log_file_stream->is_open())
     {
          *log_file_stream << message;
          log_file_stream->flush();
     }
}

void LogOutput(const std::string &message)
{
     std::lock_guard<std::mutex> lock(log_mutex);

     std::cout << message << std::flush;

     if (log_file_stream && log_file_stream->is_open())
     {
          *log_file_stream << message;
          log_file_stream->flush();
     }
}

void ResetProgressBar()
{
     last_printed_percent.store(-1);
}

void PrintStats()
{
     if (!verbose_mode)
     {
          return;
     }

     std::lock_guard<std::mutex> lock(console_mutex);

     std::cout << "Collections: " << collections_created.load() << " created, " << collections_skipped.load() << " skipped | " << "Documents: " << documents_inserted.load() << " inserted, " << documents_skipped.load() << " skipped.\n"
               << std::flush;
}

void PrintProgressBar(int current, int total, const std::string &label, int /* bar_width*/)
{
     if (verbose_mode)
     {
          return;
     }

     int percent = total > 0 ? static_cast<int>(static_cast<double>(current) / static_cast<double>(total) * 100.0) : 0;

     int last_percent = last_printed_percent.load();

     if (percent / 5 == last_percent / 5 && current < total)
     {
          return;
     }

     if (!progress_bar_mutex.try_lock())
     {
          return;
     }

     int current_percent = total > 0 ? static_cast<int>(static_cast<double>(current) / static_cast<double>(total) * 100.0) : 0;

     int current_bucket = current_percent / 5;

     int last_bucket = last_printed_percent.load() / 5;

     if (current_bucket == last_bucket && current < total)
     {
          progress_bar_mutex.unlock();

          return;
     }

     char buffer[256];

     int len;

     if (current >= total || current_percent >= 100)
     {
          len = snprintf(buffer, sizeof(buffer), "\r\033[2K%s DONE %d%% (%d/%d)", label.c_str(), current_percent, current, total);
     }
     else
     {
          int idx = spinner_index.fetch_add(1);

          static const char spinner_chars[] = {'|', '/', '-', '\\'};
          char spinner = spinner_chars[idx % 4];

          len = snprintf(buffer, sizeof(buffer), "\r\033[2K%s %c %d%% (%d/%d)", label.c_str(), spinner, current_percent, current, total);
     }

     if (len > 0 && len < static_cast<int>(sizeof(buffer)))
     {
          ssize_t written = write(STDOUT_FILENO, buffer, len);

          (void)written;
     }

     last_printed_percent.store(current_percent);

     progress_bar_mutex.unlock();
}

void PrintSpinner(const std::string &label, int attempt, int total_attempts, bool done)
{
     if (verbose_mode)
     {
          return;
     }

     if (!progress_bar_mutex.try_lock())
     {
          return;
     }

     char buffer[256];
     int len = 0;

     if (done)
     {
          len = snprintf(buffer, sizeof(buffer), "\r\033[2K%s DONE (%d/%d)", label.c_str(), attempt, total_attempts);
     }
     else
     {
          int idx = spinner_index.fetch_add(1);
          static const char spinner_chars[] = {'|', '/', '-', '\\'};
          char spinner = spinner_chars[idx % 4];
          len = snprintf(buffer, sizeof(buffer), "\r\033[2K%s %c (%d/%d)", label.c_str(), spinner, attempt, total_attempts);
     }

     if (len > 0 && len < static_cast<int>(sizeof(buffer)))
     {
          ssize_t written = write(STDOUT_FILENO, buffer, len);
          (void)written;
     }

     progress_bar_mutex.unlock();
}

void GetFinalCounts(BenchmarkClient &client, AdvancedMetrics &metrics, bool verbose)
{
     if (verbose)
     {
          std::cout << "\nFetching final counts from server...\n";
     }

     HTTPResponse update_resp = client.UpdateCounters(g_collection_prefix);

     if (update_resp.StatusCode != 200 && verbose)
     {
          std::cerr << "  Warning: update-counters returned status " << update_resp.StatusCode << ".\n";
     }

     HTTPResponse doctotal_resp = client.GetDocTotal(g_collection_prefix);

     if (doctotal_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json result = nlohmann::json::parse(doctotal_resp.Body);

               if (result.contains("doctotal"))
               {
                    metrics.FinalDocumentsCount = result["doctotal"].get<int>();
               }

               if (result.contains("coltotal"))
               {
                    metrics.FinalCollectionsCount = result["coltotal"].get<int>();
               }
          }
          catch (...)
          {
               if (verbose)
               {
                    std::cerr << "  Warning: Could not parse doctotal response.\n";
               }
          }
     }

     std::vector<std::string> collections = client.ListCollections();

     metrics.FinalCollectionsCount = collections.size();
     metrics.FinalCollectionNames = collections;

     int total_docs_val = 0;

     for (const auto &col_name : collections)
     {
          HTTPResponse col_resp = client.GetCollection(col_name);

          if (col_resp.StatusCode == 200)
          {
               try
               {
                    nlohmann::json col_json = nlohmann::json::parse(col_resp.Body);

                    int doc_count = 0;

                    if (col_json.contains("num_documents"))
                    {
                         doc_count = col_json["num_documents"].get<int>();
                    }
                    else if (col_json.contains("document_count"))
                    {
                         doc_count = col_json["document_count"].get<int>();
                    }

                    metrics.FinalPerCollectionCounts[col_name] = doc_count;

                    total_docs_val += doc_count;
               }
               catch (...)
               {
                    /* Ignore. */
               }
          }
     }

     if (metrics.FinalDocumentsCount == 0 && total_docs_val > 0)
     {
          metrics.FinalDocumentsCount = total_docs_val;
     }

     if (verbose)
     {
          std::cout << "  Final collections: " << metrics.FinalCollectionsCount << ".\n";
          std::cout << "  Final documents: " << metrics.FinalDocumentsCount << ".\n";
     }
}

std::vector<int64_t> CalculatePercentiles(const std::vector<int64_t> &timings)
{
     if (timings.empty())
     {
          return {};
     }

     std::vector<int64_t> sorted = timings;

     std::sort(sorted.begin(), sorted.end());

     std::vector<int64_t> percentiles;

     if (sorted.size() > 0)
     {
          size_t p50_idx = sorted.size() * 0.5;
          size_t p90_idx = sorted.size() * 0.9;
          size_t p99_idx = sorted.size() * 0.99;

          if (p99_idx >= sorted.size())
          {
               p99_idx = sorted.size() - 1;
          }

          percentiles.push_back(sorted[p50_idx]);
          percentiles.push_back(sorted[p90_idx]);
          percentiles.push_back(sorted[p99_idx]);
     }

     return percentiles;
}

void WriteAdvancedJSON(const std::string &filename, const AdvancedMetrics &metrics)
{
     nlohmann::json output;

     output["config"] =
          {
               {"url", metrics.ConfigURL},
               {"collections", metrics.ConfigCollections},
               {"documents", metrics.ConfigDocuments},
               {"threads", metrics.ConfigThreads},
               {"batch_size", metrics.ConfigBatchSize}};

     int64_t phase1_dur = metrics.Phase1DurationMS > 0 ? metrics.Phase1DurationMS : (metrics.Phase1EndMS - metrics.Phase1StartMS);
     int64_t phase2_dur = metrics.Phase2DurationMS > 0 ? metrics.Phase2DurationMS : (metrics.Phase2EndMS - metrics.Phase2StartMS);
     int64_t total_dur = metrics.TotalEndMS > 0 ? metrics.TotalEndMS : (phase1_dur + phase2_dur);

     output["timing"] =
          {
               {"phase1_start_ms", metrics.Phase1StartMS},
               {"phase1_end_ms", metrics.Phase1EndMS},
               {"phase1_duration_ms", phase1_dur},
               {"phase2_start_ms", metrics.Phase2StartMS},
               {"phase2_end_ms", metrics.Phase2EndMS},
               {"phase2_duration_ms", phase2_dur},
               {"ingest_start_ms", metrics.IngestStartMS},
               {"ingest_end_ms", metrics.IngestEndMS},
               {"ingest_duration_ms", metrics.IngestDurationMS},
               {"commit_start_ms", metrics.CommitStartMS},
               {"commit_end_ms", metrics.CommitEndMS},
               {"commit_duration_ms", metrics.CommitDurationMS},
               {"total_duration_ms", total_dur}};

     output["results"] =
          {
               {"phase1", {{"collections_created", metrics.Phase1CollectionsCreated}, {"collections_skipped", metrics.Phase1CollectionsSkipped}, {"throughput_collections_per_sec", metrics.Phase1ThroughputCollectionsPerSec}}},
               {"phase2", {{"documents_inserted", metrics.Phase2DocumentsInserted}, {"documents_skipped", metrics.Phase2DocumentsSkipped}, {"throughput_docs_per_sec", metrics.Phase2ThroughputDocsPerSec}}},
               {"total", {{"throughput_docs_per_sec", metrics.TotalThroughputDocsPerSec}}}};

     output["metrics"] =
          {
               {"collection_creation_timings_ms", metrics.CollectionTimings},
               {"batch_insertion_timings_ms", metrics.BatchTimings},
               {"batch_sizes", metrics.BatchSizes},
               {"batch_collections", metrics.BatchCollections}};

     output["durability"] =
          {
               {"config_path", metrics.DurabilityConfigPath},
               {"wal_sync_mode", metrics.WalSyncMode},
               {"wal_bytes_per_sync", metrics.WalBytesPerSync},
               {"manual_wal_flush", metrics.ManualWalFlush},
               {"commit_status_code", metrics.CommitStatusCode}};

     if (!metrics.CollectionTimings.empty())
     {
          double sum = 0;

          for (auto t : metrics.CollectionTimings)
          {
               sum += t;
          }

          double mean = sum / metrics.CollectionTimings.size();

          std::vector<int64_t> sorted_collections = metrics.CollectionTimings;

          std::sort(sorted_collections.begin(), sorted_collections.end());

          double median = sorted_collections[sorted_collections.size() / 2];
          double min = sorted_collections[0];
          double max = sorted_collections[sorted_collections.size() - 1];

          output["statistics"]["collection_creation"] =
               {
                    {"mean_ms", mean},
                    {"median_ms", median},
                    {"min_ms", min},
                    {"max_ms", max},
                    {"count", metrics.CollectionTimings.size()}};
     }

     if (!metrics.BatchTimings.empty())
     {
          double sum = 0;

          for (auto t : metrics.BatchTimings)
          {
               sum += t;
          }

          double mean = sum / metrics.BatchTimings.size();

          std::vector<int64_t> sorted = metrics.BatchTimings;

          std::sort(sorted.begin(), sorted.end());

          double median = sorted[sorted.size() / 2];
          double min = sorted[0];
          double max = sorted[sorted.size() - 1];

          std::vector<double> batch_throughputs;

          for (size_t i = 0; i < metrics.BatchTimings.size(); i++)
          {
               if (metrics.BatchTimings[i] > 0)
               {
                    double throughput = (metrics.BatchSizes[i] * 1000.0) / metrics.BatchTimings[i];

                    batch_throughputs.push_back(throughput);
               }
          }

          output["statistics"]["batch_insertion"] =
               {
                    {"mean_ms", mean},
                    {"median_ms", median},
                    {"min_ms", min},
                    {"max_ms", max},
                    {"count", metrics.BatchTimings.size()}};

          if (!batch_throughputs.empty())
          {
               double sum_tp = 0;

               for (auto tp : batch_throughputs)
               {
                    sum_tp += tp;
               }

               double mean_tp = sum_tp / batch_throughputs.size();

               std::sort(batch_throughputs.begin(), batch_throughputs.end());

               double median_tp = batch_throughputs[batch_throughputs.size() / 2];

               output["statistics"]["batch_throughput"] =
                    {
                         {"mean_docs_per_sec", mean_tp},
                         {"median_docs_per_sec", median_tp},
                         {"min_docs_per_sec", batch_throughputs[0]},
                         {"max_docs_per_sec", batch_throughputs[batch_throughputs.size() - 1]}};
          }
     }

     if (!metrics.DetailedOperations.empty())
     {
          output["detailed_operations"] = nlohmann::json::array();

          for (const auto &op_metric : metrics.DetailedOperations)
          {
               nlohmann::json op_json;

               op_json["operation_type"] = op_metric.OperationType;
               op_json["operation_subtype"] = op_metric.OperationSubtype;
               op_json["duration_ms"] = op_metric.DurationMS;
               op_json["success"] = op_metric.Success;
               op_json["result_count"] = op_metric.ResultCount;
               op_json["collection_name"] = op_metric.CollectionName;
               op_json["metadata"] = op_metric.Metadata;

               output["detailed_operations"].push_back(op_json);
          }
     }

     output["operation_timings"] =
          {
               {"Search_timings_ms", metrics.SearchTimings},
               {"multi_Search_timings_ms", metrics.MultiSearchTimings},
               {"document_get_timings_ms", metrics.DocumentGetTimings},
               {"document_update_timings_ms", metrics.DocumentUpdateTimings},
               {"document_delete_timings_ms", metrics.DocumentDeleteTimings},
               {"collection_list_timings_ms", metrics.CollectionListTimings},
               {"collection_get_timings_ms", metrics.CollectionGetTimings},
               {"collection_update_timings_ms", metrics.CollectionUpdateTimings},
               {"collection_delete_timings_ms", metrics.CollectionDeleteTimings},
               {"synonym_operation_timings_ms", metrics.SynonymOperationTimings},
               {"stopword_operation_timings_ms", metrics.StopwordOperationTimings},
               {"override_operation_timings_ms", metrics.OverrideOperationTimings}};

     if (!metrics.DetailedOperations.empty())
     {
          std::map<std::string, int> op_type_counts;
          std::map<std::string, int64_t> op_type_total_time;
          std::map<std::string, int> op_type_success_count;

          for (const auto &op_metric : metrics.DetailedOperations)
          {
               std::string op_type = op_metric.OperationType;

               op_type_counts[op_type]++;
               op_type_total_time[op_type] += op_metric.DurationMS;

               if (op_metric.Success)
               {
                    op_type_success_count[op_type]++;
               }
          }

          output["operation_statistics"] = nlohmann::json::object();

          for (const auto &pair : op_type_counts)
          {
               std::string op_type = pair.first;

               int count = pair.second;

               int64_t total_time = op_type_total_time[op_type];
               int success_count = op_type_success_count[op_type];

               double avg_time = count > 0 ? (total_time / static_cast<double>(count)) : 0.0;
               double success_rate = count > 0 ? (success_count / static_cast<double>(count) * 100.0) : 0.0;

               output["operation_statistics"][op_type] =
                    {
                         {"count", count},
                         {"total_time_ms", total_time},
                         {"average_time_ms", avg_time},
                         {"success_count", success_count},
                         {"success_rate_percent", success_rate}};
          }
     }

     if (!metrics.QueryTypeTimings.empty())
     {
          output["query_type_performance"] = nlohmann::json::object();

          for (const auto &pair : metrics.QueryTypeTimings)
          {
               output["query_type_performance"][pair.first] = pair.second;
          }
     }

     if (!metrics.FilterTypeTimings.empty())
     {
          output["filter_type_performance"] = nlohmann::json::object();

          for (const auto &pair : metrics.FilterTypeTimings)
          {
               output["filter_type_performance"][pair.first] = pair.second;
          }
     }

     if (!metrics.SortTypeTimings.empty())
     {
          output["sort_type_performance"] = nlohmann::json::object();

          for (const auto &pair : metrics.SortTypeTimings)
          {
               output["sort_type_performance"][pair.first] = pair.second;
          }
     }

     if (!metrics.DocumentSizeTimings.empty())
     {
          output["document_size_impact"] = nlohmann::json::object();

          for (const auto &pair : metrics.DocumentSizeTimings)
          {
               output["document_size_impact"][std::to_string(pair.first)] = pair.second;
          }
     }

     if (!metrics.BatchSizeTimings.empty())
     {
          output["batch_size_impact"] = nlohmann::json::object();

          for (const auto &pair : metrics.BatchSizeTimings)
          {
               output["batch_size_impact"][std::to_string(pair.first)] = pair.second;
          }
     }

     if (!metrics.ThreadCountThroughput.empty())
     {
          output["thread_count_throughput"] = nlohmann::json::object();

          for (const auto &pair : metrics.ThreadCountThroughput)
          {
               output["thread_count_throughput"][std::to_string(pair.first)] = pair.second;
          }
     }

     output["final_statistics"] =
          {
               {"total_operations", metrics.FinalTotalOperations},
               {"successful_operations", metrics.FinalSuccessfulOperations},
               {"success_rate_percent", metrics.FinalSuccessRate},
               {"average_operation_time_ms", metrics.FinalAvgOperationTime},
               {"total_operation_time_ms", metrics.FinalTotalOperations * metrics.FinalAvgOperationTime}};

     if (!metrics.RunID.empty())
     {
          output["run_id"] = metrics.RunID;
     }

     if (!metrics.RunSeed.empty())
     {
          output["run_seed"] = metrics.RunSeed;
     }

     output["final_counts"] =
          {
               {"collections", metrics.FinalCollectionsCount},
               {"documents", metrics.FinalDocumentsCount},
               {"per_collection", metrics.FinalPerCollectionCounts},
               {"collection_names", metrics.FinalCollectionNames}};

     if (!metrics.LatencyPercentiles.empty())
     {
          output["latency_percentiles"] = metrics.LatencyPercentiles;
     }

     if (!metrics.BatchTimings.empty())
     {
          std::vector<int64_t> sorted = metrics.BatchTimings;

          std::sort(sorted.begin(), sorted.end());

          if (sorted.size() > 0)
          {
               size_t p50_idx = sorted.size() * 0.5;
               size_t p90_idx = sorted.size() * 0.9;
               size_t p99_idx = sorted.size() * 0.99;

               if (p99_idx >= sorted.size())
               {
                    p99_idx = sorted.size() - 1;
               }

               output["latency_stats"] =
                    {
                         {"p50_ms", sorted[p50_idx]},
                         {"p90_ms", sorted[p90_idx]},
                         {"p99_ms", sorted[p99_idx]},
                         {"min_ms", sorted[0]},
                         {"max_ms", sorted[sorted.size() - 1]}};
          }
     }

     std::ofstream file(filename);

     if (file.is_open())
     {
          file << output.dump(2);

          file.close();

          std::cout << "\nAdvanced metrics written to: " << filename << ".\n";
     }
     else
     {
          std::cerr << "Note: Could not write to " << filename << ".\n";
     }
}

void CheckConsistency(BenchmarkClient &client, bool verbose)
{
     if (verbose)
     {
          std::cout << "\nChecking consistency across endpoints...\n";
     }

     std::map<std::string, int> endpoint_collections;
     std::map<std::string, int> endpoint_documents;

     HTTPResponse status_resp = client.MakeRequest("GET", "/status");

     if (status_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json status_json = nlohmann::json::parse(status_resp.Body);

               if (status_json.contains("stats") && status_json["stats"].contains("collections"))
               {
                    auto &cols = status_json["stats"]["collections"];

                    if (cols.contains("total"))
                    {
                         endpoint_collections["/status"] = cols["total"].get<int>();
                    }

                    if (cols.contains("documents_total"))
                    {
                         endpoint_documents["/status"] = cols["documents_total"].get<int>();
                    }
               }
          }
          catch (...)
          {
               if (verbose)
               {
                    std::cerr << "  Warning: Could not parse /status.\n";
               }
          }
     }

     HTTPResponse stats_resp = client.GetStats();

     if (stats_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json stats_json = nlohmann::json::parse(stats_resp.Body);

               if (stats_json.contains("collections"))
               {
                    auto &cols = stats_json["collections"];

                    if (cols.contains("total"))
                    {
                         endpoint_collections["/stats"] = cols["total"].get<int>();
                    }

                    if (cols.contains("documents_total"))
                    {
                         endpoint_documents["/stats"] = cols["documents_total"].get<int>();
                    }
               }
          }
          catch (...)
          {
               if (verbose)
               {
                    std::cerr << "  Warning: Could not parse /stats.\n";
               }
          }
     }

     HTTPResponse doctotal_resp = client.GetDocTotal(g_collection_prefix);

     if (doctotal_resp.StatusCode == 200)
     {
          try
          {
               nlohmann::json doctotal_json = nlohmann::json::parse(doctotal_resp.Body);

               if (doctotal_json.contains("coltotal"))
               {
                    endpoint_collections["/doctotal"] = doctotal_json["coltotal"].get<int>();
               }

               if (doctotal_json.contains("doctotal"))
               {
                    endpoint_documents["/doctotal"] = doctotal_json["doctotal"].get<int>();
               }
          }
          catch (...)
          {
               if (verbose)
               {
                    std::cerr << "  Warning: Could not parse /doctotal.\n";
               }
          }
     }

     std::cout << "\nConsistency Summary:.\n";
     std::cout << "  Collections:.\n";

     bool collections_consistent = true;

     int first_col_count = -1;

     for (const auto &pair : endpoint_collections)
     {
          std::cout << "    " << pair.first << ": " << pair.second << ".\n";

          if (first_col_count == -1)
          {
               first_col_count = pair.second;
          }
          else if (pair.second != first_col_count)
          {
               collections_consistent = false;
          }
     }

     std::cout << "  Documents:.\n";

     bool documents_consistent = true;

     int first_doc_count = -1;

     for (const auto &pair : endpoint_documents)
     {
          std::cout << "    " << pair.first << ": " << pair.second << ".\n";

          if (first_doc_count == -1)
          {
               first_doc_count = pair.second;
          }
          else if (pair.second != first_doc_count)
          {
               documents_consistent = false;
          }
     }

     if (collections_consistent && documents_consistent)
     {
          std::cout << "\n✓ All endpoints are consistent.\n";
     }
     else
     {
          std::cout << "\n✗ Inconsistencies detected!.\n";

          if (!collections_consistent)
          {
               std::cout << "  Collections counts differ across endpoints.\n";
          }

          if (!documents_consistent)
          {
               std::cout << "  Document counts differ across endpoints.\n";
          }
     }
}

void CleanupBenchmarkCollections(BenchmarkClient &client, bool verbose)
{
     if (verbose)
     {
          std::cout << "\nCleaning up benchmark collections...\n";
     }

     std::vector<std::string> collections = client.ListCollections();

     int deleted_count = 0;

     for (const auto &col_name : collections)
     {
          if (col_name.find(g_collection_prefix) == 0 || col_name.find("random_") == 0)
          {
               if (client.DeleteCollection(col_name))
               {
                    deleted_count++;

                    if (verbose)
                    {
                         std::cout << "  Deleted: " << col_name << ".\n";
                    }
               }
          }
     }

     if (verbose)
     {
          std::cout << "  Total deleted: " << deleted_count << " collections.\n";
     }
     else
     {
          std::cout << "Deleted " << deleted_count << " benchmark collections.\n";
     }
}
