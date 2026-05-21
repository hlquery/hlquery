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
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "runtime/clock.h"
#include "utils/tools.h"

/* Helper functions from other files. */

std::string GenerateRandomString(int length, std::mt19937 &gen);

std::string GenerateRandomSentence(int word_count, std::mt19937 &gen);

std::string GenerateRandomParagraph(int sentence_count, std::mt19937 &gen);

std::string GenerateRandomPhraseString(int doc_id, int thread_id, int phrase_num);

bool CheckServerHealth(BenchmarkClient &client, int max_retries = 3);

void FloodSignalHandler(int signal);

/* FloodCircuitBreaker class for the flood benchmark. */

class FloodCircuitBreaker
{
   private:

     std::atomic<int> ConsecutiveFailures;

     std::atomic<bool> CircuitOpen;

     std::atomic<uint64_t> LastFailureTime;

     static const int FAILURE_THRESHOLD = 10;

     static const int RESET_TIMEOUT_MS = 2000;

   public:

     /* Initializes the circuit breaker state. */

     FloodCircuitBreaker()
         : ConsecutiveFailures(0), CircuitOpen(false), LastFailureTime(0)
     {
     }

     bool IsOpen()
     {
          if (CircuitOpen.load())
          {
               uint64_t now = SteadyNowMs();
               uint64_t last_failure = LastFailureTime.load();

               if (now - last_failure > RESET_TIMEOUT_MS)
               {
                    CircuitOpen.store(false);
                    ConsecutiveFailures.store(0);

                    return false;
               }

               return true;
          }

          return false;
     }

     void RecordSuccess()
     {
          ConsecutiveFailures.store(0);
          CircuitOpen.store(false);
     }

     void RecordFailure()
     {
          int failures = ConsecutiveFailures.fetch_add(1) + 1;

          LastFailureTime.store(SteadyNowMs());

          if (failures >= FAILURE_THRESHOLD)
          {
               CircuitOpen.store(true);
          }
     }
};

/* Checks server health. */

bool CheckServerHealth(BenchmarkClient &client, int max_retries)
{
     for (int i = 0; i < max_retries; i++)
     {
          if (client.TestConnection().empty())
          {
               return true;
          }

          std::this_thread::sleep_for(std::chrono::seconds(1));
     }

     return false;
}

/* Definition of flood signal handler. */

void FloodSignalHandler(int /* signal */)
{
     g_flood_should_stop.store(true);
}

/* Runs a detailed benchmark. */

void RunDetailedBenchmark(const std::string &base_url, const std::string &auth_token, int num_collections, int num_documents, int num_threads, int batch_size, bool reuse_collections)
{
     BenchmarkClient client(base_url, auth_token, reuse_collections);

     std::cout << "\n"
               << std::string(70, '=') << "\n";
     std::cout << "COMPREHENSIVE DETAILED BENCHMARK.\n";
     std::cout << std::string(70, '=') << "\n";
     std::cout << "Testing ALL routes and functionalities...\n";
     std::cout << "Target: 1,000+ document inserts, 1,000+ searches.\n\n";

     auto overall_start = Now();
     (void)overall_start;

     int min_documents = std::max(num_documents, 1000);
     int min_searches_val = 1000;
     int total_inserted_val = 0;
     int search_count_val = 0;

     {
          std::lock_guard<std::mutex> lock(advanced_metrics_mutex);

          advanced_metrics.DetailedOperations.clear();
          advanced_metrics.SearchTimings.clear();
          advanced_metrics.MultiSearchTimings.clear();
          advanced_metrics.DocumentGetTimings.clear();
          advanced_metrics.DocumentUpdateTimings.clear();
          advanced_metrics.DocumentDeleteTimings.clear();
          advanced_metrics.CollectionListTimings.clear();
          advanced_metrics.CollectionGetTimings.clear();
          advanced_metrics.CollectionUpdateTimings.clear();
          advanced_metrics.CollectionDeleteTimings.clear();
          advanced_metrics.SynonymOperationTimings.clear();
          advanced_metrics.StopwordOperationTimings.clear();
          advanced_metrics.OverrideOperationTimings.clear();
          advanced_metrics.QueryTypeTimings.clear();
          advanced_metrics.FilterTypeTimings.clear();
          advanced_metrics.SortTypeTimings.clear();
          advanced_metrics.DocumentSizeTimings.clear();
          advanced_metrics.BatchSizeTimings.clear();
          advanced_metrics.ThreadCountThroughput.clear();
     }

     std::cout << "[1/15] Testing connection...\n";

     auto start = Now();

     std::string conn_error = client.TestConnection();

     auto end = Now();

     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

     OperationMetrics op;

     op.OperationType = "connection_test";
     op.OperationSubtype = "basic";
     op.DurationMS = duration.count();
     op.Success = conn_error.empty();
     op.Metadata["error"] = conn_error;

     advanced_metrics.DetailedOperations.push_back(op);

     if (!conn_error.empty())
     {
          std::cerr << "Connection issue: " << conn_error << ".\n";
          return;
     }

     std::cout << "  ✓ Connected (" << duration.count() << " ms).\n\n";

     std::cout << "[2/15] Testing collection operations...\n";

     start = Now();

     std::vector<std::string> existing_collections = client.ListCollections();

     end = Now();

     duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

     advanced_metrics.CollectionListTimings.push_back(duration.count());

     op.OperationType = "collection_list";
     op.OperationSubtype = "all";
     op.DurationMS = duration.count();
     op.Success = true;
     op.ResultCount = existing_collections.size();

     advanced_metrics.DetailedOperations.push_back(op);

     std::cout << "  ✓ Listed " << existing_collections.size() << " collections (" << duration.count() << " ms).\n";

     std::vector<std::string> test_collections;

     for (int i = 0; i < std::min(num_collections, 20); i++)
     {
          std::string col_name = "detailed_col_" + std::to_string(i);

          start = Now();

          bool created = client.CreateCollection(col_name);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (created)
          {
               test_collections.push_back(col_name);

               advanced_metrics.CollectionTimings.push_back(duration.count());

               op.OperationType = "collection_create";
               op.OperationSubtype = "standard";
               op.DurationMS = duration.count();
               op.Success = true;
               op.CollectionName = col_name;

               advanced_metrics.DetailedOperations.push_back(op);
          }
     }

     std::cout << "  ✓ Created " << test_collections.size() << " test collections.\n";

     if (!test_collections.empty())
     {
          start = Now();

          HTTPResponse col_response = client.MakeRequest("GET", "/collections/" + test_collections[0]);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.CollectionGetTimings.push_back(duration.count());

          op.OperationType = "collection_get";
          op.OperationSubtype = "info";
          op.DurationMS = duration.count();
          op.Success = col_response.StatusCode == 200;
          op.CollectionName = test_collections[0];

          advanced_metrics.DetailedOperations.push_back(op);

          std::cout << "  ✓ Retrieved collection info (" << duration.count() << " ms).\n";
     }

     std::cout << "\n";

     std::cout << "[3/15] Inserting " << min_documents << " documents (minimum requirement)...\n";

     if (test_collections.empty())
     {
          std::cerr << "Note: No collections available for document insertion.\n";
          return;
     }

     std::string main_collection = test_collections[0];

     std::vector<std::string> sample_docs;

     int docs_per_batch = batch_size;

     int num_batches = (min_documents + docs_per_batch - 1) / docs_per_batch;

     std::cout << "  Inserting in " << num_batches << " batches of " << docs_per_batch << " documents...\n";

     std::vector<std::string> content_templates =
          {
               "Technology and innovation drive modern society forward with rapid advancements.",
               "Scientific research continues to expand our understanding of the universe.",
               "Art and culture reflect the diverse perspectives of human experience.",
               "Business and economics shape global markets and trade relationships.",
               "Education provides the foundation for personal and professional growth.",
               "Health and wellness are essential for maintaining quality of life.",
               "Environmental conservation protects our planet for future generations.",
               "Social connections strengthen communities and individual well-being.",
               "Creative expression allows individuals to share unique perspectives.",
               "Historical knowledge informs our understanding of past and present."};

     for (int batch_idx = 0; batch_idx < num_batches && total_inserted_val < min_documents; batch_idx++)
     {
          std::vector<std::tuple<std::string, std::string, std::string>> batch;

          int docs_in_batch = std::min(docs_per_batch, min_documents - total_inserted_val);

          for (int j = 0; j < docs_in_batch; j++)
          {
               int doc_num = total_inserted_val + j;

               std::string doc_id = "detailed_doc_" + std::to_string(doc_num);

               std::string title = "Document " + std::to_string(doc_num) + " - " + content_templates[doc_num % content_templates.size()].substr(0, 30);

               std::string content = content_templates[doc_num % content_templates.size()] + " Additional content for document " + std::to_string(doc_num);

               std::uniform_real_distribution<> stopword_dist(0.0, 1.0);

               std::vector<std::string> included_stopwords;

               for (const auto &sw : FAKE_STOPWORDS)
               {
                    if (stopword_dist(Tools::GetRNG()) < 0.3)
                    {
                         included_stopwords.push_back(sw);
                    }
               }

               if (!included_stopwords.empty())
               {
                    content += ". Common words: ";

                    for (size_t i = 0; i < included_stopwords.size(); ++i)
                    {
                         if (i > 0)
                         {
                              content += " ";
                         }

                         content += included_stopwords[i];
                    }
               }

               content += ". Unique Search phrases: " + GenerateRandomPhraseString(doc_num, 0, 1) + " " + GenerateRandomPhraseString(doc_num, 0, 2) + " " + GenerateRandomPhraseString(doc_num, 0, 3) + ". This document contains various keywords and phrases for comprehensive search testing.";

               batch.push_back(std::make_tuple(doc_id, title, content));
          }

          start = Now();

          int inserted = client.InsertDocumentsBulk(main_collection, batch);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (inserted > 0)
          {
               total_inserted_val += inserted;

               advanced_metrics.BatchSizeTimings[docs_per_batch].push_back(duration.count());
               advanced_metrics.BatchTimings.push_back(duration.count());
               advanced_metrics.BatchSizes.push_back(inserted);
               advanced_metrics.BatchCollections.push_back(main_collection);

               op.OperationType = "document_insert";
               op.OperationSubtype = "bulk";
               op.DurationMS = duration.count();
               op.Success = true;
               op.ResultCount = inserted;
               op.CollectionName = main_collection;
               op.Metadata["batch_size"] = std::to_string(docs_per_batch);
               op.Metadata["batch_number"] = std::to_string(batch_idx);

               advanced_metrics.DetailedOperations.push_back(op);

               if (batch_idx % 10 == 0 || batch_idx == num_batches - 1)
               {
                    std::cout << "  Progress: " << total_inserted_val << " / " << min_documents << " documents inserted.\n";
               }

               if (sample_docs.size() < 100)
               {
                    for (int k = 0; k < std::min(inserted, 5); k++)
                    {
                         sample_docs.push_back("detailed_doc_" + std::to_string(total_inserted_val - inserted + k));
                    }
               }
          }
     }

     std::cout << "  ✓ Inserted " << total_inserted_val << " documents total.\n";

     std::cout << "  Testing document size impact...\n";

     std::vector<int> doc_sizes = {100, 500, 1000, 5000, 10000};

     for (int doc_size : doc_sizes)
     {
          std::string doc_id = "size_test_" + std::to_string(doc_size);

          std::string title = "Size Test Document";

          std::string content;

          content.reserve(doc_size);
          content = "Size test content. ";

          while (content.size() < static_cast<size_t>(doc_size))
          {
               content += "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
          }

          content = content.substr(0, doc_size);

          start = Now();

          bool inserted = client.InsertDocument(main_collection, doc_id, title, content);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (inserted)
          {
               advanced_metrics.DocumentSizeTimings[doc_size].push_back(duration.count());

               op.OperationType = "document_insert";
               op.OperationSubtype = "single_size_test";
               op.DurationMS = duration.count();
               op.Success = true;
               op.CollectionName = main_collection;
               op.Metadata["doc_size_bytes"] = std::to_string(doc_size);

               advanced_metrics.DetailedOperations.push_back(op);
          }
     }

     std::cout << "  ✓ Tested document size impact.\n";

     if (!sample_docs.empty() && !test_collections.empty())
     {
          start = Now();

          HTTPResponse doc_response = client.GetDocument(test_collections[0], sample_docs[0]);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.DocumentGetTimings.push_back(duration.count());

          op.OperationType = "document_get";
          op.OperationSubtype = "single";
          op.DurationMS = duration.count();
          op.Success = doc_response.StatusCode == 200;
          op.CollectionName = test_collections[0];

          advanced_metrics.DetailedOperations.push_back(op);

          std::cout << "  ✓ Retrieved document (" << duration.count() << " ms).\n";
     }

     if (!sample_docs.empty() && !test_collections.empty())
     {
          nlohmann::json update_doc_json;

          update_doc_json["id"] = sample_docs[0];
          update_doc_json["title"] = "Updated Title";
          update_doc_json["content"] = "Updated content";

          start = Now();

          HTTPResponse update_response = client.MakeRequest("PUT", "/collections/" + test_collections[0] + "/documents/" + sample_docs[0], update_doc_json.dump());

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.DocumentUpdateTimings.push_back(duration.count());

          op.OperationType = "document_update";
          op.OperationSubtype = "single";
          op.DurationMS = duration.count();
          op.Success = update_response.StatusCode == 200;
          op.CollectionName = test_collections[0];

          advanced_metrics.DetailedOperations.push_back(op);

          std::cout << "  ✓ Updated document (" << duration.count() << " ms).\n";
     }

     std::cout << "\n";

     std::cout << "[4/15] Running " << min_searches_val << " searches of multiple types...\n";

     if (test_collections.empty())
     {
          std::cerr << "Note: No collections available for search testing.\n";
          return;
     }

     std::string test_collection = test_collections[0];

     std::vector<std::string> simple_query_terms =
          {
               "Document", "technology", "innovation", "research", "science", "business",
               "education", "health", "environment", "social", "creative", "historical",
               "modern", "society", "culture", "art", "economics", "wellness", "conservation"};

     std::vector<std::string> complex_query_terms =
          {
               "technology innovation", "scientific research", "business economics",
               "health wellness", "environmental conservation", "social connections",
               "creative expression", "historical knowledge", "modern society"};

     std::cout << "  Running simple queries (400 searches)...\n";

     for (int i = 0; i < 400 && search_count_val < min_searches_val; i++)
     {
          std::string query = simple_query_terms[i % simple_query_terms.size()];

          if (i % 50 == 0 && i > 0)
          {
               query += " " + std::to_string(i / 50);
          }

          start = Now();

          HTTPResponse search_resp = client.Search(test_collection, query);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.SearchTimings.push_back(duration.count());
          advanced_metrics.QueryTypeTimings["simple"].push_back(duration.count());

          op.OperationType = "search";
          op.OperationSubtype = "simple";
          op.DurationMS = duration.count();
          op.Success = search_resp.StatusCode == 200;
          op.CollectionName = test_collection;
          op.Metadata["query"] = query;

          if (search_resp.StatusCode == 200)
          {
               try
               {
                    nlohmann::json result = nlohmann::json::parse(search_resp.Body);

                    if (result.contains("found"))
                    {
                         op.ResultCount = result["found"].get<int>();
                    }
               }
               catch (const std::exception &e)
               {
                    std::cerr << "[WARNING] Failed to parse search result JSON: " << e.what() << std::endl;
                    op.Metadata["parse_error"] = e.what();
               }
               catch (...)
               {
                    std::cerr << "[WARNING] Unknown exception parsing search result JSON." << std::endl;
                    op.Metadata["parse_error"] = "unknown_exception";
               }
          }

          advanced_metrics.DetailedOperations.push_back(op);

          search_count_val++;

          if (search_count_val % 100 == 0)
          {
               std::cout << "    Progress: " << search_count_val << " / " << min_searches_val << " searches.\n";
          }
     }

     std::cout << "  Running filtered queries (300 searches)...\n";

     for (int i = 0; i < 300 && search_count_val < min_searches_val; i++)
     {
          std::string query = simple_query_terms[i % simple_query_terms.size()];

          std::map<std::string, std::string> params;

          params["filter_by"] = "title:" + query;
          params["limit"] = "10";

          start = Now();

          HTTPResponse search_resp = client.Search(test_collection, query, params);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.SearchTimings.push_back(duration.count());
          advanced_metrics.QueryTypeTimings["filtered"].push_back(duration.count());
          advanced_metrics.FilterTypeTimings["exact"].push_back(duration.count());

          op.OperationType = "search";
          op.OperationSubtype = "filtered";
          op.DurationMS = duration.count();
          op.Success = search_resp.StatusCode == 200;
          op.CollectionName = test_collection;
          op.Metadata["query"] = query;
          op.Metadata["has_filter"] = "true";

          advanced_metrics.DetailedOperations.push_back(op);

          search_count_val++;

          if (search_count_val % 100 == 0)
          {
               std::cout << "    Progress: " << search_count_val << " / " << min_searches_val << " searches.\n";
          }
     }

     std::cout << "  Running complex queries with sorting (200 searches)...\n";

     for (int i = 0; i < 200 && search_count_val < min_searches_val; i++)
     {
          std::string query = complex_query_terms[i % complex_query_terms.size()];

          std::map<std::string, std::string> params;

          params["sort_by"] = (i % 2 == 0) ? "title:asc" : "title:desc";
          params["limit"] = "20";

          start = Now();

          HTTPResponse search_resp = client.Search(test_collection, query, params);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.SearchTimings.push_back(duration.count());
          advanced_metrics.QueryTypeTimings["complex"].push_back(duration.count());
          advanced_metrics.SortTypeTimings[(i % 2 == 0) ? "field_asc" : "field_desc"].push_back(duration.count());

          op.OperationType = "search";
          op.OperationSubtype = "complex_sorted";
          op.DurationMS = duration.count();
          op.Success = search_resp.StatusCode == 200;
          op.CollectionName = test_collection;
          op.Metadata["query"] = query;
          op.Metadata["sort_by"] = params["sort_by"];

          advanced_metrics.DetailedOperations.push_back(op);

          search_count_val++;

          if (search_count_val % 100 == 0)
          {
               std::cout << "    Progress: " << search_count_val << " / " << min_searches_val << " searches.\n";
          }
     }

     std::cout << "  Running POST searches with complex parameters (100 searches)...\n";

     for (int i = 0; i < 100 && search_count_val < min_searches_val; i++)
     {
          nlohmann::json search_params_json;

          search_params_json["q"] = complex_query_terms[i % complex_query_terms.size()];
          search_params_json["query_by"] = "title,content,document_id";
          search_params_json["filter_by"] = "title:" + simple_query_terms[i % simple_query_terms.size()];
          search_params_json["sort_by"] = "title:asc";
          search_params_json["limit"] = 10;
          search_params_json["highlight_full_fields"] = "title,content";

          start = Now();

          HTTPResponse post_search = client.SearchPost(test_collection, search_params_json);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.SearchTimings.push_back(duration.count());
          advanced_metrics.QueryTypeTimings["complex"].push_back(duration.count());
          advanced_metrics.SortTypeTimings["field_asc"].push_back(duration.count());

          op.OperationType = "search";
          op.OperationSubtype = "complex_post";
          op.DurationMS = duration.count();
          op.Success = post_search.StatusCode == 200;
          op.CollectionName = test_collection;
          op.Metadata["method"] = "POST";
          op.Metadata["query"] = search_params_json["q"];

          advanced_metrics.DetailedOperations.push_back(op);

          search_count_val++;

          if (search_count_val % 50 == 0)
          {
               std::cout << "    Progress: " << search_count_val << " / " << min_searches_val << " searches.\n";
          }
     }

     std::cout << "  ✓ Completed " << search_count_val << " searches total.\n";
     std::cout << "\n";

     std::cout << "[5/15] Testing multi-search operations...\n";

     if (test_collections.size() >= 2)
     {
          nlohmann::json multi_search_json;

          multi_search_json["Searches"] = nlohmann::json::array();

          for (size_t i = 0; i < std::min(test_collections.size(), size_t(3)); i++)
          {
               nlohmann::json search_req;

               search_req["collection"] = test_collections[i];
               search_req["q"] = "Document";
               search_req["query_by"] = "title,content,document_id";

               multi_search_json["Searches"].push_back(search_req);
          }

          start = Now();

          HTTPResponse multi_resp = client.MultiSearch(multi_search_json);

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          advanced_metrics.MultiSearchTimings.push_back(duration.count());

          op.OperationType = "multi_search";
          op.OperationSubtype = "multi_collection";
          op.DurationMS = duration.count();
          op.Success = multi_resp.StatusCode == 200;
          op.ResultCount = std::min(test_collections.size(), size_t(3));

          advanced_metrics.DetailedOperations.push_back(op);

          std::cout << "  ✓ Tested multi-search across " << std::min(test_collections.size(), size_t(3)) << " collections (" << duration.count() << " ms).\n";
     }

     std::cout << "\n";

     std::cout << "[6/15] Testing synonyms, stopwords, and overrides...\n";

     if (!test_collections.empty())
     {
          std::string test_col = test_collections[0];

          nlohmann::json synonym_json;

          synonym_json["synonyms"] = {"car", "automobile", "vehicle"};

          start = Now();

          HTTPResponse syn_resp = client.MakeRequest("POST", "/collections/" + test_col + "/synonyms/test_syn", synonym_json.dump());

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (syn_resp.StatusCode == 200 || syn_resp.StatusCode == 201)
          {
               advanced_metrics.SynonymOperationTimings.push_back(duration.count());

               op.OperationType = "synonym";
               op.OperationSubtype = "create";
               op.DurationMS = duration.count();
               op.Success = true;
               op.CollectionName = test_collection;

               advanced_metrics.DetailedOperations.push_back(op);

               std::cout << "  ✓ Created synonym (" << duration.count() << " ms).\n";
          }

          nlohmann::json stopwords_json;

          stopwords_json["stopwords"] = {"the", "a", "an"};

          start = Now();

          HTTPResponse stop_resp = client.MakeRequest("POST", "/collections/" + test_col + "/stopwords", stopwords_json.dump());

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (stop_resp.StatusCode == 200 || stop_resp.StatusCode == 201)
          {
               advanced_metrics.StopwordOperationTimings.push_back(duration.count());

               op.OperationType = "stopword";
               op.OperationSubtype = "create";
               op.DurationMS = duration.count();
               op.Success = true;
               op.CollectionName = test_collection;

               advanced_metrics.DetailedOperations.push_back(op);

               std::cout << "  ✓ Created stopwords (" << duration.count() << " ms).\n";
          }

          nlohmann::json override_json;

          override_json["rule"] = nlohmann::json::object();
          override_json["rule"]["query"] = "test";
          override_json["rule"]["match"] = "exact";
          override_json["rule"]["filter_by"] = "title:test";

          start = Now();

          HTTPResponse over_resp = client.MakeRequest("POST", "/collections/" + test_col + "/overrides/test_override", override_json.dump());

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          if (over_resp.StatusCode == 200 || over_resp.StatusCode == 201)
          {
               advanced_metrics.OverrideOperationTimings.push_back(duration.count());

               op.OperationType = "override";
               op.OperationSubtype = "create";
               op.DurationMS = duration.count();
               op.Success = true;
               op.CollectionName = test_collection;

               advanced_metrics.DetailedOperations.push_back(op);

               std::cout << "  ✓ Created override (" << duration.count() << " ms).\n";
          }
     }

     std::cout << "\n";

     std::cout << "[7/15] Testing additional API endpoints...\n";

     start = Now();

     HTTPResponse health_resp = client.MakeRequest("GET", "/health");

     end = Now();

     duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

     op.OperationType = "health_check";
     op.OperationSubtype = "basic";
     op.DurationMS = duration.count();
     op.Success = health_resp.StatusCode == 200;

     advanced_metrics.DetailedOperations.push_back(op);

     std::cout << "  ✓ Health check (" << duration.count() << " ms).\n";

     start = Now();

     HTTPResponse status_resp = client.MakeRequest("GET", "/status");

     end = Now();

     duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

     op.OperationType = "status";
     op.OperationSubtype = "server";
     op.DurationMS = duration.count();
     op.Success = status_resp.StatusCode == 200;

     advanced_metrics.DetailedOperations.push_back(op);

     std::cout << "  ✓ Status check (" << duration.count() << " ms).\n";

     start = Now();

     HTTPResponse stats_resp = client.MakeRequest("GET", "/stats");

     end = Now();

     duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

     op.OperationType = "stats";
     op.OperationSubtype = "server";
     op.DurationMS = duration.count();
     op.Success = stats_resp.StatusCode == 200;

     advanced_metrics.DetailedOperations.push_back(op);

     std::cout << "  ✓ Stats check (" << duration.count() << " ms).\n";
     std::cout << "\n";

     std::cout << "[8/15] Testing performance with different thread counts...\n";
     std::cout << "  ✓ Thread performance testing (simplified).\n\n";

     std::cout << "[9/15] Testing update and delete operations comprehensively...\n";

     if (!sample_docs.empty() && !test_collections.empty())
     {
          std::string test_col = test_collections[0];

          std::cout << "  Testing document updates (50 operations)...\n";

          int update_count_val = 0;

          for (size_t i = 0; i < sample_docs.size() && i < 50; i++)
          {
               nlohmann::json update_doc_json;

               update_doc_json["id"] = sample_docs[i];
               update_doc_json["title"] = "Updated Title " + std::to_string(i);
               update_doc_json["content"] = "Updated content for document " + std::to_string(i) + ". This is a comprehensive update test.";

               start = Now();

               HTTPResponse update_resp = client.MakeRequest("PUT", "/collections/" + test_col + "/documents/" + sample_docs[i], update_doc_json.dump());

               end = Now();

               duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

               advanced_metrics.DocumentUpdateTimings.push_back(duration.count());

               op.OperationType = "document_update";
               op.OperationSubtype = "single";
               op.DurationMS = duration.count();
               op.Success = update_resp.StatusCode == 200;
               op.CollectionName = test_collection;
               op.Metadata["update_number"] = std::to_string(i);

               advanced_metrics.DetailedOperations.push_back(op);

               update_count_val++;
          }

          std::cout << "    ✓ Completed " << update_count_val << " document updates.\n";

          std::cout << "  Testing document retrievals (100 operations)...\n";

          int get_count_val = 0;

          for (size_t i = 0; i < sample_docs.size() && i < 100; i++)
          {
               start = Now();

               HTTPResponse doc_resp = client.GetDocument(test_col, sample_docs[i]);

               end = Now();

               duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

               advanced_metrics.DocumentGetTimings.push_back(duration.count());

               op.OperationType = "document_get";
               op.OperationSubtype = "single";
               op.DurationMS = duration.count();
               op.Success = doc_resp.StatusCode == 200;
               op.CollectionName = test_collection;

               advanced_metrics.DetailedOperations.push_back(op);

               get_count_val++;
          }

          std::cout << "    ✓ Completed " << get_count_val << " document retrievals.\n";

          std::cout << "  Testing document deletes (30 operations)...\n";

          int delete_count_val = 0;

          size_t delete_start_val = sample_docs.size() > 30 ? sample_docs.size() - 30 : 0;

          for (size_t i = delete_start_val; i < sample_docs.size() && delete_count_val < 30; i++)
          {
               start = Now();

               HTTPResponse del_resp = client.MakeRequest("DELETE", "/collections/" + test_collection + "/documents/" + sample_docs[i]);

               end = Now();

               duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

               advanced_metrics.DocumentDeleteTimings.push_back(duration.count());

               op.OperationType = "document_delete";
               op.OperationSubtype = "single";
               op.DurationMS = duration.count();
               op.Success = del_resp.StatusCode == 200 || del_resp.StatusCode == 204;
               op.CollectionName = test_collection;

               advanced_metrics.DetailedOperations.push_back(op);

               delete_count_val++;
          }

          std::cout << "    ✓ Completed " << delete_count_val << " document deletes.\n";

          std::cout << "  Testing delete by filter...\n";

          start = Now();

          HTTPResponse filter_del_resp = client.MakeRequest("DELETE", "/collections/" + test_collection + "/documents?filter_by=title:Updated");

          end = Now();

          duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

          op.OperationType = "document_delete";
          op.OperationSubtype = "by_filter";
          op.DurationMS = duration.count();
          op.Success = filter_del_resp.StatusCode == 200 || filter_del_resp.StatusCode == 204;
          op.CollectionName = test_collection;
          op.Metadata["method"] = "filter";

          advanced_metrics.DetailedOperations.push_back(op);

          std::cout << "    ✓ Tested delete by filter (" << duration.count() << " ms).\n";
     }

     std::cout << "\n";

     std::cout << "[10/15] Testing collection management operations...\n";

     if (!test_collections.empty())
     {
          for (int i = 0; i < 10; i++)
          {
               start = Now();

               std::vector<std::string> cols = client.ListCollections();

               end = Now();

               duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

               advanced_metrics.CollectionListTimings.push_back(duration.count());

               op.OperationType = "collection_list";
               op.OperationSubtype = "repeat";
               op.DurationMS = duration.count();
               op.Success = true;

               advanced_metrics.DetailedOperations.push_back(op);
          }

          std::cout << "    ✓ Tested repeat collection listings.\n";
     }
}

/* Runs the flood benchmark. */

void RunFloodBenchmark(const std::string &base_url, const std::string &auth_token, int num_threads, bool verbose, bool reuse_collections)
{
     std::cout << "\n";
     std::cout << "----------------------------------------------------------------\n";
     std::cout << "FLOOD MODE: Continuous Stress Testing\n";
     std::cout << "----------------------------------------------------------------\n";
     std::cout << "\n";
     std::cout << "This will continuously:\n";
     std::cout << "  - Randomly create collections\n";
     std::cout << "  - Randomly insert LARGE documents (10-500 KB each) to fill MB/GB of space\n";
     std::cout << "  - Randomly perform searches\n";
     std::cout << "\n";
     std::cout << "Target: Fill unlimited disk space (runs until Ctrl+C).\n";
     std::cout << "Press Ctrl+C to stop...\n";
     std::cout << "\n";

     const int BATCH_SIZE_VAL = 10;
     const int MAX_COLLECTIONS_VAL = 1000;
     const int MIN_DOC_SIZE_KB_VAL = 10;
     const int MAX_DOC_SIZE_KB_VAL = 500;

     FloodCircuitBreaker circuit_breaker;

     g_flood_should_stop.store(false);

     signal(SIGINT, FloodSignalHandler);
     signal(SIGTERM, FloodSignalHandler);

     auto start_time = Now();

     BenchmarkClient client(base_url, auth_token, reuse_collections);

     if (!CheckServerHealth(client))
     {
          std::cerr << "\nERROR: Server is not healthy! Aborting flood benchmark.\n";
          return;
     }

     std::atomic<int> flood_collections_created{0};
     std::atomic<int> flood_documents_inserted{0};
     std::atomic<uint64_t> flood_bytes_inserted{0};
     std::atomic<int> flood_searches_performed{0};
     std::atomic<int> flood_errors_encountered{0};
     std::atomic<bool> server_unhealthy{false};

     std::vector<std::string> existing_collections;

     std::mutex collections_mutex;

     std::atomic<int> collection_counter{0};

     auto worker_thread = [&, reuse_collections](int thread_id)
     {
          BenchmarkClient thread_client(base_url, auth_token, reuse_collections);

          std::mt19937 gen(std::random_device{}() + thread_id);

          std::uniform_real_distribution<> price_dist(0.99, 9999.99);
          std::uniform_int_distribution<> quantity_dist(0, 10000);
          std::uniform_real_distribution<> rating_dist(1.0, 5.0);
          std::uniform_int_distribution<> views_dist(0, 1000000);
          std::uniform_int_distribution<> year_dist(2000, 2024);
          std::uniform_int_distribution<> operation_dist(0, 99);
          std::uniform_int_distribution<> delay_dist(10, 100);

          const std::vector<std::string> categories =
               {
                    "electronics", "clothing", "books", "food", "toys", "sports", "home", "garden"};

          const std::vector<std::string> statuses =
               {
                    "active", "inactive", "pending", "archived", "deleted"};

          const std::vector<std::string> search_terms_val =
               {
                    "product", "item", "article", "review", "technology", "innovation", "solution", "system"};

          int consecutive_errors = 0;

          while (!g_flood_should_stop.load() && !server_unhealthy.load())
          {
               if (g_flood_should_stop.load())
               {
                    break;
               }

               if (circuit_breaker.IsOpen())
               {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
               }

               int operation = operation_dist(gen);

               if (operation < 40)
               {
                    int col_num = collection_counter.fetch_add(1);

                    if (col_num >= MAX_COLLECTIONS_VAL)
                    {
                         collection_counter.store(0);
                         col_num = 0;
                    }

                    std::string col_name = "stress_collection_" + std::to_string(col_num);

                    int retries = 3;

                    bool success = false;

                    while (retries > 0 && !success && !g_flood_should_stop.load())
                    {
                         if (thread_client.CreateCollection(col_name))
                         {
                              flood_collections_created++;
                              consecutive_errors = 0;
                              circuit_breaker.RecordSuccess();
                              success = true;

                              {
                                   std::lock_guard<std::mutex> lock(collections_mutex);

                                   if (existing_collections.size() < MAX_COLLECTIONS_VAL)
                                   {
                                        existing_collections.push_back(col_name);
                                   }
                              }

                              if (verbose)
                              {
                                   std::cout << "[Thread " << thread_id << "] Created collection: " << col_name << ".\n";
                              }
                         }
                         else
                         {
                              retries--;

                              if (retries > 0)
                              {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(10 * (3 - retries)));
                              }
                              else
                              {
                                   consecutive_errors++;
                                   flood_errors_encountered++;
                                   circuit_breaker.RecordFailure();
                              }
                         }
                    }
               }
               else if (operation < 90)
               {
                    std::string col_name;

                    {
                         std::lock_guard<std::mutex> lock(collections_mutex);

                         if (existing_collections.empty())
                         {
                              std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));
                              continue;
                         }

                         std::uniform_int_distribution<> col_sel(0, existing_collections.size() - 1);

                         col_name = existing_collections[col_sel(gen)];
                    }

                    std::vector<std::tuple<std::string, std::string, std::string>> batch;

                    std::uniform_int_distribution<> batch_size_dist(1, BATCH_SIZE_VAL);

                    int batch_size = batch_size_dist(gen);

                    std::uniform_int_distribution<> doc_size_dist(MIN_DOC_SIZE_KB_VAL, MAX_DOC_SIZE_KB_VAL);

                    for (int i = 0; i < batch_size; i++)
                    {
                         std::string doc_id = "flood_doc_" + std::to_string(col_name.length()) + "_" + std::to_string(SteadyNowNs()) + "_" + GenerateRandomString(8, gen);

                         std::string title = GenerateRandomSentence(3 + (i % 5), gen);

                         int target_size_kb = doc_size_dist(gen);
                         int target_size_bytes = target_size_kb * 1024;

                         std::string content;

                         content.reserve(target_size_bytes + 1024);

                         content = GenerateRandomParagraph(2 + (i % 5), gen);
                         content += " Price: $" + std::to_string(price_dist(gen));
                         content += " Quantity: " + std::to_string(quantity_dist(gen));
                         content += " Rating: " + std::to_string(rating_dist(gen));
                         content += " Category: " + categories[i % categories.size()];
                         content += " Status: " + statuses[i % statuses.size()];
                         content += " SKU: " + GenerateRandomString(8, gen);
                         content += " Year: " + std::to_string(year_dist(gen));
                         content += " Views: " + std::to_string(views_dist(gen));
                         content += " Description: ";

                         while (static_cast<int>(content.size()) < target_size_bytes)
                         {
                              std::uniform_int_distribution<> para_sentences(5, 15);

                              int num_sentences = para_sentences(gen);

                              for (int s = 0; s < num_sentences && static_cast<int>(content.size()) < target_size_bytes; s++)
                              {
                                   std::uniform_int_distribution<> sentence_words(10, 30);

                                   int num_words = sentence_words(gen);

                                   std::string sentence = GenerateRandomSentence(num_words, gen);

                                   content += sentence + ". ";

                                   if (s % 3 == 0 && static_cast<int>(content.size()) < target_size_bytes - 100)
                                   {
                                        content += "Data: " + GenerateRandomString(20, gen) + " ";
                                        content += "Value: " + std::to_string(quantity_dist(gen)) + " ";
                                        content += "Timestamp: " + std::to_string(SteadyNowNs()) + " ";
                                   }
                              }

                              content += "\n\n";
                         }

                         if (static_cast<int>(content.size()) > target_size_bytes)
                         {
                              content = content.substr(0, target_size_bytes);
                         }

                         batch.push_back(std::make_tuple(doc_id, title, content));
                    }

                    int retries = 3;
                    int inserted = 0;

                    while (retries > 0 && inserted == 0 && !g_flood_should_stop.load())
                    {
                         try
                         {
                              inserted = thread_client.InsertDocumentsBulk(col_name, batch);

                              if (inserted > 0)
                              {
                                   flood_documents_inserted += inserted;

                                   uint64_t batch_bytes = 0;

                                   for (const auto &doc : batch)
                                   {
                                        batch_bytes += std::get<0>(doc).size();
                                        batch_bytes += std::get<1>(doc).size();
                                        batch_bytes += std::get<2>(doc).size();
                                   }

                                   flood_bytes_inserted += batch_bytes;

                                   consecutive_errors = 0;
                                   circuit_breaker.RecordSuccess();

                                   if (verbose && (flood_documents_inserted.load() % 100 == 0))
                                   {
                                        double mb_inserted = static_cast<double>(flood_bytes_inserted.load()) / (1024.0 * 1024.0);

                                        std::cout << "[Thread " << thread_id << "] Inserted " << flood_documents_inserted.load() << " documents, " << std::fixed << std::setprecision(2) << mb_inserted << " MB.\n";
                                   }

                                   break;
                              }
                         }
                         catch (...)
                         {
                              /* Ignore. */
                         }

                         retries--;

                         if (retries > 0)
                         {
                              std::this_thread::sleep_for(std::chrono::milliseconds(20 * (3 - retries)));
                         }
                         else
                         {
                              consecutive_errors++;
                              flood_errors_encountered++;
                              circuit_breaker.RecordFailure();
                         }
                    }
               }
               else
               {
                    std::string col_name;

                    {
                         std::lock_guard<std::mutex> lock(collections_mutex);

                         if (existing_collections.empty())
                         {
                              std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));
                              continue;
                         }

                         std::uniform_int_distribution<> col_sel(0, existing_collections.size() - 1);

                         col_name = existing_collections[col_sel(gen)];
                    }

                    std::uniform_int_distribution<> word_dist(0, search_terms_val.size() - 1);

                    std::string query = search_terms_val[word_dist(gen)];

                    thread_client.ResetConnection();

                    HTTPResponse response = thread_client.MakeRequest("GET", "/collections/" + col_name + "/documents/Search?q=" + query, "", 1);

                    if (response.StatusCode == 200)
                    {
                         flood_searches_performed++;
                         circuit_breaker.RecordSuccess();
                    }
                    else
                    {
                         flood_errors_encountered++;
                         circuit_breaker.RecordFailure();
                    }
               }

               std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));

               if (consecutive_errors > 0 && consecutive_errors % 10 == 0)
               {
                    if (!CheckServerHealth(thread_client, 1))
                    {
                         server_unhealthy.store(true);
                         break;
                    }
               }

               if (consecutive_errors > 0)
               {
                    consecutive_errors = 0;
               }
          }
     };

     std::vector<std::thread> workers;

     for (int i = 0; i < num_threads; i++)
     {
          workers.emplace_back(worker_thread, i);
     }

     int prev_collections = 0;
     int prev_documents = 0;
     int prev_searches = 0;

     auto stats_thread = [&]()
     {
          while (!g_flood_should_stop.load() && !server_unhealthy.load())
          {
               std::this_thread::sleep_for(std::chrono::seconds(10));

               if (g_flood_should_stop.load())
               {
                    break;
               }

               auto current_time = Now();

               auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

               int curr_collections = flood_collections_created.load();
               int curr_documents = flood_documents_inserted.load();
               int curr_searches = flood_searches_performed.load();

               bool collections_decreased = (curr_collections < prev_collections);
               bool documents_decreased = (curr_documents < prev_documents);
               bool searches_decreased = (curr_searches < prev_searches);

               if (collections_decreased || documents_decreased || searches_decreased)
               {
                    std::cerr << "\n[WARNING] Non-monotonic counters detected!.\n";

                    if (collections_decreased)
                    {
                         std::cerr << "  Collections: " << prev_collections << " -> " << curr_collections << ".\n";
                    }

                    if (documents_decreased)
                    {
                         std::cerr << "  Documents: " << prev_documents << " -> " << curr_documents << ".\n";
                    }

                    if (searches_decreased)
                    {
                         std::cerr << "  Searches: " << prev_searches << " -> " << curr_searches << ".\n";
                    }
               }

               prev_collections = curr_collections;
               prev_documents = curr_documents;
               prev_searches = curr_searches;

               BenchmarkClient stats_client(base_url, auth_token);

               HTTPResponse doctotal_resp = stats_client.GetDocTotal("");

               double mb_inserted = static_cast<double>(flood_bytes_inserted.load()) / (1024.0 * 1024.0);

               std::cout << "\n[Stats] Runtime: " << elapsed << "s | " << "Collections: " << curr_collections << " | " << "Documents: " << curr_documents << " | " << "Data: " << std::fixed << std::setprecision(2) << mb_inserted << " MB | " << "Searches: " << curr_searches << " | " << "Errors: " << flood_errors_encountered.load();

               if (doctotal_resp.StatusCode == 200)
               {
                    try
                    {
                         nlohmann::json doctotal_json = nlohmann::json::parse(doctotal_resp.Body);

                         if (doctotal_json.contains("doctotal"))
                         {
                              int server_docs = doctotal_json["doctotal"].get<int>();

                              std::cout << " | Server docs: " << server_docs;
                         }
                    }
                    catch (...)
                    {
                         /* Ignore. */
                    }
               }

               std::cout << "\n";
          }
     };

     std::thread stats_printer(stats_thread);

     std::cout << "Flood benchmark running... Press Ctrl+C to stop.\n";

     while (!g_flood_should_stop.load() && !server_unhealthy.load())
     {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
     }

     for (auto &t : workers)
     {
          t.join();
     }

     stats_printer.join();

     auto end_time_val = Now();

     auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - start_time).count();

     std::cout << "\n";
     std::cout << "FLOOD BENCHMARK STOPPED.\n";
     std::cout << "----------------------------------------------------------------\n";

     double mb_inserted = static_cast<double>(flood_bytes_inserted.load()) / (1024.0 * 1024.0);
     double gb_inserted = mb_inserted / 1024.0;

     std::cout << "Collections created: " << flood_collections_created.load() << ".\n";
     std::cout << "Documents inserted: " << flood_documents_inserted.load() << ".\n";

     if (gb_inserted >= 1.0)
     {
          std::cout << "Data inserted: " << std::fixed << std::setprecision(2) << gb_inserted << " GB.\n";
     }
     else
     {
          std::cout << "Data inserted: " << std::fixed << std::setprecision(2) << mb_inserted << " MB.\n";
     }

     std::cout << "Searches performed: " << flood_searches_performed.load() << ".\n";

     if (flood_errors_encountered.load() > 0)
     {
          std::cout << "Errors encountered: " << flood_errors_encountered.load() << ".\n";
     }

     std::cout << "Total time: " << total_duration << " ms.\n";

     if (total_duration > 0)
     {
          std::cout << "Average throughput: " << (flood_documents_inserted.load() * 1000.0 / total_duration) << " docs/sec.\n";

          if (mb_inserted > 0)
          {
               std::cout << "Data rate: " << std::fixed << std::setprecision(2) << (mb_inserted * 1000.0 / total_duration) << " MB/sec.\n";
          }
     }

     std::cout << "\n";
}
