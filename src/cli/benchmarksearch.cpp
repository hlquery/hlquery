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
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "utils/tools.h"

/* External declarations. */

extern std::string g_collection_prefix;

/* Prints search results. */

void PrintSearchResult(int Search_num, const std::string &query_desc, const HTTPResponse &response, const std::string &collection)
{
     std::cout << "\n[" << Search_num << "] " << query_desc << ".\n";
     std::cout << "Collection: " << collection << ".\n";

     if (response.StatusCode != 200)
     {
          std::cout << "Status: " << response.StatusCode << " (Issue).\n";

          if (!response.Body.empty())
          {
               std::cout << "Response: " << response.Body.substr(0, 200) << ".\n";
          }

          return;
     }

     try
     {
          nlohmann::json result = nlohmann::json::parse(response.Body);

          int found = result.contains("found") ? result["found"].get<int>() : 0;

          std::cout << "Found: " << found << " document(s)\n";

          if (result.contains("hits") && result["hits"].is_array())
          {
               auto hits = result["hits"];

               int num_to_show = std::min(3, static_cast<int>(hits.size()));

               for (int i = 0; i < num_to_show; i++)
               {
                    auto hit = hits[i];

                    std::cout << "  Result " << (i + 1) << ":\n";

                    if (hit.contains("document"))
                    {
                         auto doc = hit["document"];

                         if (doc.contains("id"))
                         {
                              std::cout << "    ID: " << doc["id"].get<std::string>() << ".\n";
                         }

                         if (doc.contains("title"))
                         {
                              std::cout << "    Title: " << doc["title"].get<std::string>() << ".\n";
                         }

                         if (doc.contains("content"))
                         {
                              std::string content = doc["content"].get<std::string>();

                              if (content.length() > 80)
                              {
                                   content = content.substr(0, 77) + "...";
                              }

                              std::cout << "    Content: " << content << ".\n";
                         }
                    }
                    else if (hit.contains("id"))
                    {
                         std::cout << "    ID: " << hit["id"].get<std::string>() << ".\n";
                    }

                    if (hit.contains("score"))
                    {
                         std::cout << "    Score: " << hit["score"].get<double>() << ".\n";
                    }
               }

               if (hits.size() > static_cast<size_t>(num_to_show))
               {
                    std::cout << "  ... and " << (hits.size() - num_to_show) << " more result(s).\n";
               }
          }
     }
     catch (...)
     {
          std::cout << "Status: 200 (Parse issue).\n";
     }
}

/* Runs search benchmarks. */

void RunSearches(const std::string &base_url, const std::string &auth_token)
{
     BenchmarkClient client(base_url, auth_token);

     std::vector<std::string> all_collections = client.ListCollections();

     std::vector<std::string> bench_collections;

     for (const auto &col : all_collections)
     {
          if (col.find(g_collection_prefix) == 0 || col.find("random_") == 0 || col == "unorganized")
          {
               bench_collections.push_back(col);
          }
     }

     if (bench_collections.empty())
     {
          std::cout << "No benchmark collections found. Please run benchmark first to create collections and insert data.\n";
          return;
     }

     std::cout << "HLQuery Search Benchmark.\n";
     std::cout << "\n";
     std::cout << "Found " << bench_collections.size() << " benchmark collection(s).\n";
     std::cout << "Running comprehensive searches of all types...\n\n";

     std::uniform_int_distribution<size_t> col_dist(0, bench_collections.size() - 1);
     std::uniform_int_distribution<> num_dist(0, 99);

     int search_count_val = 0;

     auto start_time = std::chrono::high_resolution_clock::now();

     std::vector<std::string> base_queries =
          {
               "Document", "Collection", "content", "Lorem", "ipsum", "dolor",
               "consectetur", "adipiscing", "elit", "inserted", "thread",
               "music", "science", "band", "cake", "discovery"};

     for (int i = 0; i < 15; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Basic Search: '" + query + "'", response, collection);
     }

     std::vector<std::string> wildcard_prefix =
          {
               "*Document", "*Collection", "*content", "*Lorem", "*ipsum",
               "*dolor", "*consectetur", "*adipiscing", "*elit", "*thread"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_prefix[i % wildcard_prefix.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Wildcard prefix: '" + query + "'", response, collection);
     }

     std::vector<std::string> wildcard_suffix =
          {
               "Document*", "Collection*", "content*", "Lorem*", "ipsum*",
               "dolor*", "consectetur*", "adipiscing*", "elit*", "thread*"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_suffix[i % wildcard_suffix.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Wildcard suffix: '" + query + "'", response, collection);
     }

     std::vector<std::string> wildcard_middle =
          {
               "Doc*ment", "Col*ction", "con*ent", "Lo*em", "ip*um",
               "do*or", "cons*etur", "adip*ing", "el*t", "thr*ad"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_middle[i % wildcard_middle.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Wildcard middle: '" + query + "'", response, collection);
     }

     std::vector<std::string> quoted_phrases =
          {
               "\"Document in\"", "\"in Collection\"", "\"the content\"", "\"Lorem ipsum\"",
               "\"ipsum dolor\"", "\"dolor sit\"", "\"consectetur adipiscing\"",
               "\"adipiscing elit\"", "\"inserted by\"", "\"by thread\""};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = quoted_phrases[i % quoted_phrases.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Quoted phrase: '" + query + "'", response, collection);
     }

     std::vector<std::string> or_queries =
          {
               "Document OR Collection", "Lorem OR ipsum", "content OR thread",
               "dolor OR consectetur", "adipiscing OR elit", "Document OR ipsum",
               "Collection OR content", "Lorem OR dolor", "thread OR inserted",
               "ipsum OR adipiscing"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = or_queries[i % or_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Boolean OR: '" + query + "'", response, collection);
     }

     std::vector<std::string> and_queries =
          {
               "Document AND Collection", "Lorem AND ipsum", "content AND thread",
               "dolor AND consectetur", "adipiscing AND elit", "Document AND content",
               "Collection AND thread", "Lorem AND dolor", "thread AND inserted",
               "ipsum AND adipiscing"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = and_queries[i % and_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Boolean AND: '" + query + "'", response, collection);
     }

     std::vector<std::string> plus_queries =
          {
               "Document +Collection", "Lorem +ipsum", "content +thread",
               "dolor +consectetur", "adipiscing +elit", "Document +content",
               "Collection +thread", "Lorem +dolor", "thread +inserted",
               "ipsum +adipiscing"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = plus_queries[i % plus_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Plus operator: '" + query + "'", response, collection);
     }

     std::vector<std::string> minus_queries =
          {
               "Document -ipsum", "Collection -dolor", "content -Lorem",
               "Lorem -adipiscing", "ipsum -elit", "Document -thread",
               "Collection -content", "Lorem -ipsum", "thread -Document",
               "content -Collection"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = minus_queries[i % minus_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Minus operator: '" + query + "'", response, collection);
     }

     std::vector<std::string> complex_queries =
          {
               "Document* AND content", "Lorem OR ipsum*", "\"Document in\" AND Collection",
               "Document +content -ipsum", "Collection* OR \"the content\"",
               "Document AND (content OR thread)", "Lorem* +ipsum -dolor",
               "\"in Collection\" OR Document*", "content* AND \"inserted by\"",
               "Document OR Collection* AND content"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = complex_queries[i % complex_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Complex query: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          std::map<std::string, std::string> params;

          params["limit"] = std::to_string((i % 5 + 1) * 2);

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Search with limit=" + params["limit"], response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = "Document";

          std::map<std::string, std::string> params;

          params["limit"] = "3";
          params["offset"] = std::to_string(i * 2);

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Search with offset=" + params["offset"], response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 5;

          if (i % 2 == 0)
          {
               Search_params["sort_by"] = "title:asc";
          }

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "POST Search: '" + query + "'" + (i % 2 == 0 ? " (sorted)" : ""), response, collection);
     }

     std::vector<std::string> post_wildcards =
          {
               "Document*", "*Collection", "Doc*ment", "Col*ction", "con*ent",
               "Lo*em", "ip*um", "do*or", "cons*etur", "thr*ad"};

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = post_wildcards[i % post_wildcards.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 5;

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "POST wildcard: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = quoted_phrases[i % quoted_phrases.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 5;

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "POST quoted: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = "Document";

          nlohmann::json Search_params;

          Search_params["q"] = query;

          if (i % 2 == 0)
          {
               Search_params["query_by"] = "title";
          }
          else
          {
               Search_params["query_by"] = "content";
          }

          Search_params["limit"] = 5;

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Search query_by=" + Search_params["query_by"].get<std::string>(), response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          nlohmann::json multi_search_json;

          multi_search_json["Searches"] = nlohmann::json::array();

          for (int j = 0; j < 2; j++)
          {
               std::string collection = bench_collections[col_dist(Tools::GetRNG())];

               std::string query = base_queries[(i + j) % base_queries.size()];

               nlohmann::json search_item;

               search_item["collection"] = collection;
               search_item["q"] = query;
               search_item["query_by"] = "title,content";
               search_item["per_page"] = 3;

               multi_search_json["Searches"].push_back(search_item);
          }

          auto response = client.MultiSearch(multi_search_json);

          std::cout << "\n[" << ++search_count_val << "] Multi-Search (2 queries).\n";

          if (response.StatusCode == 200)
          {
               try
               {
                    nlohmann::json result = nlohmann::json::parse(response.Body);

                    if (result.contains("results") && result["results"].is_array())
                    {
                         std::cout << "Results: " << result["results"].size() << " Search result(s).\n";

                         for (size_t j = 0; j < result["results"].size() && j < 2; j++)
                         {
                              auto res = result["results"][j];

                              int found = res.contains("found") ? res["found"].get<int>() : 0;

                              std::cout << "  Search " << (j + 1) << ": Found " << found << " document(s).\n";
                         }
                    }
               }
               catch (...)
               {
                    std::cout << "Status: 200 (Parse issue).\n";
               }
          }
          else
          {
               std::cout << "Status: " << response.StatusCode << ".\n";
          }
     }

     for (int i = 0; i < 10; i++)
     {
          nlohmann::json multi_search_json;

          multi_search_json["Searches"] = nlohmann::json::array();

          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          nlohmann::json search1;

          search1["collection"] = collection;
          search1["q"] = wildcard_suffix[i % wildcard_suffix.size()];
          search1["query_by"] = "title,content";
          search1["per_page"] = 3;

          multi_search_json["Searches"].push_back(search1);

          nlohmann::json search2;

          search2["collection"] = collection;
          search2["q"] = quoted_phrases[i % quoted_phrases.size()];
          search2["query_by"] = "title,content";
          search2["per_page"] = 3;

          multi_search_json["Searches"].push_back(search2);

          auto response = client.MultiSearch(multi_search_json);

          std::cout << "\n[" << ++search_count_val << "] Multi-Search (wildcard + phrase).\n";

          if (response.StatusCode == 200)
          {
               try
               {
                    nlohmann::json result = nlohmann::json::parse(response.Body);

                    if (result.contains("results") && result["results"].is_array())
                    {
                         std::cout << "Results: " << result["results"].size() << " Search result(s).\n";
                    }
               }
               catch (...)
               {
                    std::cout << "Status: 200 (Parse issue).\n";
               }
          }
          else
          {
               std::cout << "Status: " << response.StatusCode << ".\n";
          }
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Search with highlighting: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_suffix[i % wildcard_suffix.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + wildcard: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = quoted_phrases[i % quoted_phrases.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + phrase: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = or_queries[i % or_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + OR: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = and_queries[i % and_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + AND: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_prefix[i % wildcard_prefix.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + prefix wildcard: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = wildcard_middle[i % wildcard_middle.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + middle wildcard: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = complex_queries[i % complex_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + complex: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting (title only): '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting (content only): '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = plus_queries[i % plus_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + plus: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          std::string query = base_queries[i % base_queries.size()];

          nlohmann::json Search_params;

          Search_params["q"] = query;
          Search_params["query_by"] = "title,content";
          Search_params["limit"] = 3;
          Search_params["highlight"] = true;
          Search_params["highlight_fields"] = "title,content";

          if (i % 2 == 0)
          {
               Search_params["sort_by"] = "title:asc";
          }
          else
          {
               Search_params["sort_by"] = "_text_match:desc";
          }

          auto response = client.SearchPost(collection, Search_params);

          PrintSearchResult(++search_count_val, "Highlighting + sorted: '" + query + "'", response, collection);
     }

     for (int i = 0; i < 10; i++)
     {
          std::string collection = bench_collections[col_dist(Tools::GetRNG())];

          int doc_num = num_dist(Tools::GetRNG());

          std::string query = "Document " + std::to_string(doc_num);

          std::map<std::string, std::string> params;

          params["limit"] = "5";

          auto response = client.Search(collection, query, params);

          PrintSearchResult(++search_count_val, "Random Search: '" + query + "'", response, collection);
     }

     auto end_time = std::chrono::high_resolution_clock::now();

     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

     std::cout << "\n"
               << std::string(60, '=') << "\n";
     std::cout << "Search Benchmark Complete!.\n";
     std::cout << "\n";
     std::cout << "Total searches: " << search_count_val << ".\n";
     std::cout << "Total time: " << duration.count() << " ms.\n";

     if (search_count_val > 0 && duration.count() > 0)
     {
          std::cout << "Average time per search: " << (duration.count() / static_cast<double>(search_count_val)) << " ms.\n";
          std::cout << "Searches per second: " << (search_count_val * 1000.0 / duration.count()) << ".\n";
     }
}
