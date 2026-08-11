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

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <vendor/json/json.hpp>
#include <sha2/sha2.h>

#include "benchmarkclient.h"
#include "runtime/clock.h"
#include "utils/tools.h"

/* Task-specific functions. */

std::string GenerateContentWithSynonymsAndStopwords(int doc_id, int thread_id, int col_idx, std::mt19937 &gen);

void PrintProgressBar(int current, int total, const std::string &label, int bar_width = 50);

static int GetBenchmarkDocsForCollection(int collection_index, int docs_per_collection, int remaining_docs)
{
     return docs_per_collection + ((collection_index < remaining_docs) ? 1 : 0);
}

static std::string MakeBenchmarkDocumentID(int collection_index, int document_index, const std::string &run_id, bool reuse_collections)
{
     if (!reuse_collections && !run_id.empty())
     {
          const std::string run_suffix = run_id.size() > 2 ? run_id.substr(2) : run_id;
          return std::to_string(collection_index) + std::to_string(document_index) + run_suffix;
     }

     return std::to_string(collection_index) + "_" + std::to_string(document_index);
}

std::string BenchmarkSHA256(const std::string &value)
{
     unsigned char digest[SHA256_DIGEST_SIZE];
     sha256(reinterpret_cast<const unsigned char *>(value.data()),
            static_cast<unsigned int>(value.size()), digest);
     std::ostringstream output;
     output << std::hex << std::setfill('0');
     for (unsigned char byte : digest)
     {
          output << std::setw(2) << static_cast<unsigned int>(byte);
     }
     return output.str();
}

VerifiedBenchmarkDocument BuildVerifiedBenchmarkDocument(int collection_index, int document_index,
                                                          const std::string &run_id, const std::string &seed,
                                                          bool reuse_collections)
{
     VerifiedBenchmarkDocument document;
     document.ID = MakeBenchmarkDocumentID(collection_index, document_index, run_id, reuse_collections);
     document.Title = "Document " + std::to_string(document_index) + " in Collection " + std::to_string(collection_index);
     document.Content = "Deterministic HLQuery benchmark document for run " + run_id +
                        ", seed " + seed + ", collection " + std::to_string(collection_index) +
                        ", ordinal " + std::to_string(document_index) + ".";
     document.RunID = run_id;
     document.Seed = seed;
     document.Ordinal = document_index;
     document.Collection = collection_index;

     nlohmann::ordered_json canonical = {
          {"benchmark_seed", document.Seed},
          {"collection_number", document.Collection},
          {"content", document.Content},
          {"document_id", document.ID},
          {"ordinal", document.Ordinal},
          {"run_id", document.RunID},
          {"title", document.Title}
     };
     document.Payload = canonical.dump();
     document.PayloadHash = BenchmarkSHA256(document.Payload);
     return document;
}

static int NormalizeInsertedCount(int inserted, size_t batch_size)
{
     if (inserted < 0)
     {
          return 0;
     }

     if (static_cast<size_t>(inserted) > batch_size)
     {
          return static_cast<int>(batch_size);
     }

     return inserted;
}

static void RecordInsertedDocumentBytes(int64_t batch_bytes, int inserted, size_t batch_size)
{
     if (inserted <= 0 || batch_size == 0)
     {
          return;
     }

     const int64_t inserted_bytes = (batch_bytes * inserted) / static_cast<int64_t>(batch_size);
     benchmark_document_bytes.fetch_add(inserted_bytes);
}

/* Deletes collections in a thread. */

void DeleteCollectionsThread(const std::string &base_url, const std::string &auth_token, int start_idx, int end_idx, int total_collections, const std::set<std::string> &existing_collections)
{
     BenchmarkClient client(base_url, auth_token);

     for (int i = start_idx; i < end_idx; i++)
     {
          std::string collection_name = MakeBenchmarkCollectionName(i);

          if (existing_collections.find(collection_name) != existing_collections.end())
          {
               try
               {
                    bool deleted = client.DeleteCollection(collection_name);

                    if (!deleted)
                    {
                         /* Ignore deletion failure. */
                    }
               }
               catch (...)
               {
                    /* Ignore exception. */
               }
          }
     }
}

/* Creates collections in a thread. */

void CreateCollectionsThread(const std::string &base_url, const std::string &auth_token, int start_idx, int end_idx, bool collect_metrics, int total_collections, bool reuse_collections)
{
     BenchmarkClient client(base_url, auth_token, reuse_collections);

     client.Reset();

     for (int i = start_idx; i < end_idx; i++)
     {
          auto start = Now();

          std::string collection_name = MakeBenchmarkCollectionName(i);

          const int COLLECTION_TIMEOUT_MS = 10000;

          bool success = false;

          try
          {
               client.ResetConnection();
               success = client.CreateCollection(collection_name, COLLECTION_TIMEOUT_MS);
          }
          catch (...)
          {
               success = false;
          }

          auto end = Now();

          if (success)
          {
               collections_created.fetch_add(1);
          }
          else
          {
               collections_skipped.fetch_add(1);

               if (verbose_mode)
               {
                    std::lock_guard<std::mutex> lock(console_mutex);

                    std::cerr << "  Failed to create collection " << i << ".\n";
               }
          }

          client.ResetConnection();

          if (collect_metrics)
          {
               auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

               std::lock_guard<std::mutex> lock(advanced_metrics_mutex);

               advanced_metrics.CollectionTimings.push_back(duration.count());
          }

          int total_created = collections_created.load();
          int total_skipped_collections = collections_skipped.load();
          int total_inserted_docs = documents_inserted.load();
          int total_skipped_docs = documents_skipped.load();

          if (verbose_mode)
          {
               std::lock_guard<std::mutex> lock(console_mutex);

               std::cout << "  Created collection " << i << " (" << total_created << "/" << total_collections << ").\n";
               std::cout << "Collections: " << total_created << " created, " << total_skipped_collections << " skipped | " << "Documents: " << total_inserted_docs << " inserted, " << total_skipped_docs << " skipped.\n";
          }

          PrintProgressBar(total_created, total_collections, "Creating collections");
     }
}

/* Inserts additional documents in a thread. */

void InsertAdditionalDocumentsThread(const std::string &base_url, const std::string &auth_token, int num_collections, int start_doc_idx, int additional_docs, int thread_id, int thread_count, int batch_size, bool collect_metrics, int total_documents, const std::string &run_id, const std::string &seed, bool reuse_collections)
{
     BenchmarkClient client(base_url, auth_token, reuse_collections);

     client.Reset();

     const int safe_thread_count = std::max(1, thread_count);
     int task_index = 0;

     for (int batch_round = 0;; ++batch_round)
     {
          if (g_benchmark_should_stop.load())
          {
               return;
          }

          bool has_more_batches = false;

          for (int col_idx = 0; col_idx < num_collections; ++col_idx)
          {
               const int batch_start = batch_round * batch_size;
               if (batch_start >= additional_docs)
               {
                    continue;
               }

               has_more_batches = true;
               const int current_task = task_index++;
               if ((current_task % safe_thread_count) != thread_id)
               {
                    continue;
               }

               if (g_benchmark_should_stop.load())
               {
                    return;
               }

               std::string collection_name = MakeBenchmarkCollectionName(col_idx);
               int batch_end = std::min(batch_start + batch_size, additional_docs);
               std::vector<VerifiedBenchmarkDocument> batch;
               batch.reserve(static_cast<size_t>(batch_end - batch_start));

               for (int batch_idx = batch_start; batch_idx < batch_end; batch_idx++)
               {
                    int doc_idx = start_doc_idx + batch_idx;

                    batch.push_back(BuildVerifiedBenchmarkDocument(col_idx, doc_idx, run_id, seed, reuse_collections));
               }

               if (g_benchmark_should_stop.load())
               {
                    return;
               }

               auto batch_start_time = Now();
               int64_t batch_document_bytes = 0;
               for (const auto &doc : batch) batch_document_bytes += static_cast<int64_t>(doc.Payload.size() + doc.PayloadHash.size());

               int inserted = 0;

               int batch_num = (batch_start / batch_size) + 1;

               try
               {
                    inserted = client.InsertVerifiedDocumentsBulk(collection_name, batch);
               }
               catch (...)
               {
                    inserted = 0;
                    client.ResetConnection();
               }

               auto batch_end_time = Now();
               inserted = NormalizeInsertedCount(inserted, batch.size());

               documents_inserted.fetch_add(inserted);
               documents_skipped.fetch_add(static_cast<int>(batch.size()) - inserted);
               additional_documents_inserted.fetch_add(inserted);
               additional_documents_skipped.fetch_add(static_cast<int>(batch.size()) - inserted);
               RecordInsertedDocumentBytes(batch_document_bytes, inserted, batch.size());

               if (collect_metrics)
               {
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end_time - batch_start_time);
                    const int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end_time - batch_start_time).count();
                    const int64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

                    std::lock_guard<std::mutex> lock(advanced_metrics_mutex);

                    advanced_metrics.BatchTimings.push_back(duration.count());
                    advanced_metrics.BatchSizes.push_back(batch.size());
                    advanced_metrics.BatchCollections.push_back(collection_name);
                    advanced_metrics.LatencySamples.push_back({timestamp_ns, thread_id, collection_name, batch_num,
                         static_cast<int>(batch.size()), batch_document_bytes, latency_ns,
                         inserted == static_cast<int>(batch.size()) ? 200 : 0, -1,
                         inserted == static_cast<int>(batch.size()) ? "" : "partial_or_failed_batch"});
               }

               int total_inserted = additional_documents_inserted.load();

               if (total_documents > 0)
               {
                    PrintProgressBar(total_inserted, total_documents, "Inserting additional documents");
               }

               if (verbose_mode && (total_inserted % 100 == 0 || inserted == 0))
               {
                    std::lock_guard<std::mutex> lock(console_mutex);

                    std::cout << "  Collection " << col_idx << " additional batch " << batch_num << ": " << inserted << "/" << batch.size() << " inserted (approx additional total: " << total_inserted << ").\n";
               }
          }

          if (!has_more_batches)
          {
               break;
          }
     }

     client.ResetConnection();
}

/* Inserts documents in a thread. */

void InsertDocumentsThread(const std::string &base_url, const std::string &auth_token, int num_collections, int docs_per_collection, int remaining_docs, int thread_id, int thread_count, int batch_size, bool collect_metrics, int total_documents, const std::string &run_id, const std::string &seed, bool reuse_collections)
{
     BenchmarkClient client(base_url, auth_token, reuse_collections);

     client.Reset();

     const int safe_thread_count = std::max(1, thread_count);
     int task_index = 0;

     for (int batch_round = 0;; ++batch_round)
     {
          if (g_benchmark_should_stop.load())
          {
               return;
          }

          bool has_more_batches = false;

          for (int col_idx = 0; col_idx < num_collections; ++col_idx)
          {
               const int docs_in_collection = GetBenchmarkDocsForCollection(col_idx, docs_per_collection, remaining_docs);
               const int batch_start = batch_round * batch_size;
               if (batch_start >= docs_in_collection)
               {
                    continue;
               }

               has_more_batches = true;
               const int current_task = task_index++;
               if ((current_task % safe_thread_count) != thread_id)
               {
                    continue;
               }

               if (g_benchmark_should_stop.load())
               {
                    return;
               }

               std::string collection_name = MakeBenchmarkCollectionName(col_idx);
               int batch_end = std::min(batch_start + batch_size, docs_in_collection);
               std::vector<VerifiedBenchmarkDocument> batch;
               batch.reserve(static_cast<size_t>(batch_end - batch_start));

               for (int doc_idx = batch_start; doc_idx < batch_end; doc_idx++)
               {
                    batch.push_back(BuildVerifiedBenchmarkDocument(col_idx, doc_idx, run_id, seed, reuse_collections));
               }

               if (g_benchmark_should_stop.load())
               {
                    return;
               }

               auto batch_start_time = Now();
               int64_t batch_document_bytes = 0;
               for (const auto &doc : batch) batch_document_bytes += static_cast<int64_t>(doc.Payload.size() + doc.PayloadHash.size());

               int inserted = 0;

               int batch_num = (batch_start / batch_size) + 1;

               try
               {
                    inserted = client.InsertVerifiedDocumentsBulk(collection_name, batch);
               }
               catch (...)
               {
                    inserted = 0;
                    client.ResetConnection();
               }

               auto batch_end_time = Now();
               inserted = NormalizeInsertedCount(inserted, batch.size());

               documents_inserted.fetch_add(inserted);
               documents_skipped.fetch_add(static_cast<int>(batch.size()) - inserted);
               RecordInsertedDocumentBytes(batch_document_bytes, inserted, batch.size());

               if (collect_metrics)
               {
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end_time - batch_start_time);
                    const int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end_time - batch_start_time).count();
                    const int64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

                    std::lock_guard<std::mutex> lock(advanced_metrics_mutex);

                    advanced_metrics.BatchTimings.push_back(duration.count());
                    advanced_metrics.BatchSizes.push_back(batch.size());
                    advanced_metrics.BatchCollections.push_back(collection_name);
                    advanced_metrics.LatencySamples.push_back({timestamp_ns, thread_id, collection_name, batch_num,
                         static_cast<int>(batch.size()), batch_document_bytes, latency_ns,
                         inserted == static_cast<int>(batch.size()) ? 200 : 0, -1,
                         inserted == static_cast<int>(batch.size()) ? "" : "partial_or_failed_batch"});
               }

               int total_inserted = documents_inserted.load();
               int total_collections = collections_created.load();
               int total_skipped_collections = collections_skipped.load();
               int total_skipped_documents = documents_skipped.load();

               if (total_documents > 0)
               {
                    PrintProgressBar(total_inserted, total_documents, "Inserting documents");
               }

               if (verbose_mode && (total_inserted % 100 == 0 || inserted == 0))
               {
                    std::lock_guard<std::mutex> lock(console_mutex);

                    std::cout << "  Collection " << col_idx << " batch " << batch_num << ": " << inserted << "/" << batch.size() << " inserted (approx total: " << total_inserted << ").\n";
                    std::cout << "Collections: " << total_collections << " created, " << total_skipped_collections << " skipped | " << "Documents: " << total_inserted << " inserted, " << total_skipped_documents << " skipped.\n";
               }
          }

          if (!has_more_batches)
          {
               break;
          }
     }

     client.ResetConnection();
}
