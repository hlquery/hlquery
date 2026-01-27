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
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "hlquery_cli.h"

/* Main entry point for the hlquery CLI. */

int main(int argc, char* argv[]) 
{
     try 
     {
          /* Default connection values for local development. */

          std::string base_url = "http://localhost:9200";
          std::string host_val = "localhost";

          int port_val = 9200;

          /* CLI settings with safe defaults. */

          bool raw_mode_val = false;

          std::string auth_token_val = "";

          std::vector<std::string> args_vec;

          /* Pull base URL from environment when available. */

          const char* env_base_url = std::getenv("HLQ_BASE_URL");

          if (env_base_url && *env_base_url) 
          {
               base_url = env_base_url;

               size_t start_pos = base_url.find("://");

               if (start_pos != std::string::npos) 
               {
                    start_pos += 3;

                    size_t colon_pos = base_url.find(":", start_pos);
                    size_t slash_pos = base_url.find("/", start_pos);

                    if (colon_pos != std::string::npos && colon_pos < (slash_pos != std::string::npos ? slash_pos : base_url.length())) 
                    {
                         host_val = base_url.substr(start_pos, colon_pos - start_pos);

                         try 
                         {
                              port_val = std::stoi(base_url.substr(colon_pos + 1, (slash_pos != std::string::npos ? slash_pos : base_url.length()) - colon_pos - 1));
                         } 
                         catch (...) 
                         {
                              /* Ignore. */
                         }
                    } 
                    else if (slash_pos != std::string::npos) 
                    {
                         host_val = base_url.substr(start_pos, slash_pos - start_pos);
                    } 
                    else 
                    {
                         host_val = base_url.substr(start_pos);
                    }
               }
          }
    
          /* Load the auth token from the environment if provided. */

          const char* env_auth = std::getenv("HLQ_AUTH_TOKEN");

          if (env_auth && *env_auth) 
          {
               auth_token_val = env_auth;
          }
    
          int timeout_seconds_val = 5;
          (void)timeout_seconds_val;
          (void)host_val;
          (void)port_val;
          (void)base_url;
          (void)auth_token_val;
          (void)raw_mode_val;

          /* Parse CLI arguments and collect positional values. */

          for (int i = 1; i < argc; i++) 
          {
               std::string arg = argv[i];

               if ((arg == "-url" || arg == "--url") && i + 1 < argc) 
               {
                    base_url = argv[++i];

                    size_t start_pos = base_url.find("://");

                    if (start_pos != std::string::npos) 
                    {
                         start_pos += 3;

                         size_t colon_pos = base_url.find(":", start_pos);
                         size_t slash_pos = base_url.find("/", start_pos);

                         if (colon_pos != std::string::npos && colon_pos < (slash_pos != std::string::npos ? slash_pos : base_url.length())) 
                         {
                              host_val = base_url.substr(start_pos, colon_pos - start_pos);

                              try 
                              {
                                   port_val = std::stoi(base_url.substr(colon_pos + 1, (slash_pos != std::string::npos ? slash_pos : base_url.length()) - colon_pos - 1));
                         } 
                              catch (...) 
                              {
                                   /* Ignore. */
                              }
                         } 
                         else if (slash_pos != std::string::npos) 
                         {
                              host_val = base_url.substr(start_pos, slash_pos - start_pos);
                         } 
                         else 
                         {
                              host_val = base_url.substr(start_pos);
                         }
                    }
               } 
               else if (arg == "--host" && i + 1 < argc) 
               {
                    host_val = argv[++i];
                    base_url = "http://" + host_val + ":" + std::to_string(port_val);
               } 
               else if (arg == "--port" && i + 1 < argc) 
               {
                    try 
                    {
                         port_val = std::stoi(argv[++i]);
                         base_url = "http://" + host_val + ":" + std::to_string(port_val);
                    } 
                    catch (...) 
                    {
                         std::cerr << "Error: Invalid port value: " << argv[i] << ".\n" << std::endl;

                         return 1;
                    }
               } 
               else if ((arg == "-auth" || arg == "--auth") && i + 1 < argc) 
               {
                    auth_token_val = argv[++i];  
               } 
               else if (arg == "--raw") 
               {
                    raw_mode_val = true;
               } 
               else if (arg == "--timeout" && i + 1 < argc) 
               {
                    try 
                    {
                         timeout_seconds_val = std::stoi(argv[++i]);
                    } 
                    catch (...) 
                    {
                         std::cerr << "Error: Invalid timeout value: " << argv[i] << ".\n" << std::endl;

                         return 1;
                    }
               } 
               else if (arg == "-h" || arg == "--help") 
               {
                    HLQueryCLI cli_help;

                    cli_help.ShowHelp();

                    return 0;
               } 
               else if (arg == "-e" || arg == "--examples") 
               {
                    HLQueryCLI cli_examples;

                    cli_examples.ShowExamples();

                    return 0;
               } 
               else if (arg == "-r" || arg == "--routes") 
               {
                    HLQueryCLI cli_routes;

                    cli_routes.ShowRoutes();

                    return 0;
               } 
               else 
               {
                    args_vec.push_back(arg);
               }
          }
    
          /* Show a short quick-start when no arguments are provided. */

          if (args_vec.empty()) 
          {
               std::cout << "hlquery-cli v1.0.0 - Command Line Interface for hlquery search engine.\n";
               std::cout << "\n\n";
               std::cout << "Quick Start:.\n";
               std::cout << "  xx cols                    # List collections\n";
               std::cout << "  xx info <collection>       # Show collection details\n";
               std::cout << "  xx docs <collection>       # List documents\n";
               std::cout << "  xx search <col> <query>    # Search documents\n";
               std::cout << "  xx status                  # Check server status\n";
               std::cout << "  xx adv                     # Show advanced server information\n";
               std::cout << "  xx uptime [-d]             # Show server uptime\n";
               std::cout << "  xx tx [kb|mb|gb]           # Show total bytes transferred\n";
               std::cout << "  xx conn                   # Show active connections\n";
               std::cout << "  xx lsm                    # Show deep LSM statistics\n";
               std::cout << "  xx doctotal                # Show total documents and collections\n";
               std::cout << "  xx flush                   # Flush all data (destructive)\n";
               std::cout << "  xx syn list                # Show synonym counts per collection\n";
               std::cout << "  xx stop list               # Show stopword counts per collection\n\n";
               std::cout << "For detailed help and examples, run: xx help.\n\n";

               return 0;
          }
    
          /* Build the CLI instance with the resolved connection settings. */

          HLQueryCLI cli_instance(base_url, raw_mode_val, auth_token_val);

          /* The first positional argument is always the command. */

          std::string command_str = args_vec[0];
    
          /* Dispatch CLI commands to their handlers. */

          if (command_str == "cols") 
          {
               int offset_val = 0;
               int limit_val = 10000; 

               bool json_output_flag = false;
            
               int positional_arg_count_val = 0;
            
               for (size_t i = 1; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "--json") 
                    {
                         json_output_flag = true;
                    } 
                    else if (args_vec[i] == "--offset" && i + 1 < args_vec.size()) 
                    {
                         try 
                         {
                              offset_val = std::stoi(args_vec[++i]);
                         } 
                         catch (...) 
                         {
                              std::cout << "Error: Invalid offset: " << args_vec[i] << "." << std::endl;

                              return 1;
                         }
                    } 
                    else if (args_vec[i] == "--limit" && i + 1 < args_vec.size()) 
                    {
                         try 
                         {
                              limit_val = std::stoi(args_vec[++i]);
                         } 
                         catch (...) 
                         {
                              std::cout << "Error: Invalid limit: " << args_vec[i] << "." << std::endl;

                              return 1;
                         }
                    } 
                    else 
                    {
                         try 
                         {
                              if (positional_arg_count_val == 0) 
                              {
                                   offset_val = std::stoi(args_vec[i]);
                                   positional_arg_count_val++;
                              } 
                              else if (positional_arg_count_val == 1) 
                              {
                                   limit_val = std::stoi(args_vec[i]);
                                   positional_arg_count_val++;
                              }
                         } 
                         catch (...) 
                         {
                              /* Ignore. */
                         }
                    }
               }
            
               cli_instance.ListCollections(offset_val, limit_val, json_output_flag);
          } 
          else if (command_str == "info" && args_vec.size() >= 2) 
          {
               cli_instance.ShowCollectionInfo(args_vec[1]);
          } 
          else if (command_str == "docs" && args_vec.size() >= 2) 
          {
               int offset_val = 0;
               int limit_val = 10000;
            
               if (args_vec.size() == 3) 
               {
                    try 
                    {
                         limit_val = std::stoi(args_vec[2]);
                    } 
                    catch (...) 
                    {
                         std::cout << "Error: Invalid limit: " << args_vec[2] << ".\n" << std::endl;

                         return 1;
                    }
               } 
               else if (args_vec.size() >= 4) 
               {
                    try 
                    {
                         offset_val = std::stoi(args_vec[2]);
                         limit_val = std::stoi(args_vec[3]);
                    } 
                    catch (...) 
                    {
                         std::cout << "Error: Invalid offset or limit.\n" << std::endl;

                         return 1;
                    }
               }
            
               cli_instance.ListDocuments(args_vec[1], offset_val, limit_val);
          } 
          else if (command_str == "status" || command_str == "stats") 
          {
               cli_instance.ShowStatus();
          } 
          else if (command_str == "adv" || command_str == "advanced") 
          {
               cli_instance.ShowAdvanced();
          } 
          else if (command_str == "ping") 
          {
               cli_instance.ShowPing();
          } 
          else if (command_str == "uptime") 
          {
               bool days_format_flag = false;

               for (size_t i = 1; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "-d") 
                    {
                         days_format_flag = true;
                         break;
                    }
               }

               cli_instance.ShowUptime(days_format_flag);
          } 
          else if (command_str == "dbsize") 
          {
               std::string unit_str = "mb";

               if (args_vec.size() >= 2) 
               {
                    unit_str = args_vec[1];

                    std::transform(unit_str.begin(), unit_str.end(), unit_str.begin(), ::tolower);
               }

               cli_instance.ShowDatabaseSize(unit_str);
          } 
          else if (command_str == "tx") 
          {
               std::string unit_str = "kb";

               if (args_vec.size() >= 2) 
               {
                    unit_str = args_vec[1];

                    std::transform(unit_str.begin(), unit_str.end(), unit_str.begin(), ::tolower);
               }

               cli_instance.ShowTransferStats(unit_str);
          } 
          else if (command_str == "conn" || command_str == "connections") 
          {
               cli_instance.ShowConnections();
          } 
          else if (command_str == "lsm") 
          {
               cli_instance.ShowLSM();
          } 
          else if (command_str == "doctotal") 
          {
               int offset_val = 0;
               int limit_val = 0;

               for (size_t i = 1; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "--offset" && i + 1 < args_vec.size()) 
                    {
                         try 
                         {
                              offset_val = std::stoi(args_vec[++i]);
                         } 
                         catch (...) 
                         {
                              std::cerr << "Error: Invalid offset: " << args_vec[i] << "." << std::endl;

                              return 1;
                         }
                    } 
                    else if (args_vec[i] == "--limit" && i + 1 < args_vec.size()) 
                    {
                         try 
                         {
                              limit_val = std::stoi(args_vec[++i]);
                         } 
                         catch (...) 
                         {
                              std::cerr << "Error: Invalid limit: " << args_vec[i] << "." << std::endl;

                              return 1;
                         }
                    }
               }

               cli_instance.ShowDocTotal(offset_val, limit_val);
          } 
          else if (command_str == "coltotal") 
          {
               cli_instance.ShowDocTotal();  
          } 
          else if (command_str == "flush") 
          {
               bool force_flag = false;

               for (size_t i = 1; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "--force" || args_vec[i] == "-f") 
                    {
                         force_flag = true;
                         break;
                    }
               }

               cli_instance.FlushAll(!force_flag);
          }
          else if (command_str == "rebuild-counters" || command_str == "repair") 
          {
               std::string collection_name_str = "";

               bool rebuild_index_flag = false;

               if (args_vec.size() >= 2) 
               {
                    for (size_t i = 1; i < args_vec.size(); i++) 
                    {
                         if (args_vec[i] == "--index" || args_vec[i] == "--rebuild-index") 
                         {
                              rebuild_index_flag = true;
                         } 
                         else 
                         {
                              collection_name_str = args_vec[i];
                         }
                    }
               }

               cli_instance.RebuildCounters(collection_name_str, rebuild_index_flag);
          } 
          else if (command_str == "wal-stats") 
          {
               std::string collection_name_str = "";

               if (args_vec.size() >= 2) 
               {
                    collection_name_str = args_vec[1];
               }

               cli_instance.ShowWALStats(collection_name_str);
          } 
          else if (command_str == "create") 
          {
               if (args_vec.size() < 3) 
               {
                    std::cerr << "Error: 'create' command requires a collection name and at least one searchable field." << std::endl;
                    std::cerr << "Usage: xx create <collection> <field1> [field2 ...]." << std::endl;

                    return 1;
               }

               std::vector<std::string> searchable_fields_vec(args_vec.begin() + 2, args_vec.end());

               cli_instance.CreateCollection(args_vec[1], searchable_fields_vec);
          } 
          else if (command_str == "delete") 
          {
               if (args_vec.size() < 2) 
               {
                    std::cerr << "Error: 'delete' command requires a collection name." << std::endl;
                    std::cerr << "Usage: xx delete <collection> [document_id] [--filter=<query>]." << std::endl;

                    return 1;
               }
            
               std::string collection_str = args_vec[1];
            
               if (args_vec.size() == 2) 
               {
                    cli_instance.DeleteCollection(collection_str);
               } 
               else 
               {
                    std::string document_id_str = "";
                    std::string filter_str = "";
                
                    for (size_t i = 2; i < args_vec.size(); i++) 
                    {
                         if (args_vec[i].substr(0, 9) == "--filter=") 
                         {
                              filter_str = args_vec[i].substr(9);
                         } 
                         else if (args_vec[i][0] != '-') 
                         {
                              document_id_str = args_vec[i];
                         }
                    }
                
                    if (!filter_str.empty()) 
                    {
                         cli_instance.DeleteDocumentsByFilter(collection_str, filter_str);
                    } 
                    else if (!document_id_str.empty()) 
                    {
                         cli_instance.DeleteDocument(collection_str, document_id_str);
                    }
               }
          }
          else if (command_str == "add") 
          {
               if (args_vec.size() < 5) 
               {
                    std::cerr << "Error: 'add' command requires collection, id, title, and content." << std::endl;
                    std::cerr << "Usage: xx add <col> <id> <title> <content> [field=value...]." << std::endl;

                    return 1;
               }

               std::string col_str = args_vec[1];
               std::string id_str = args_vec[2];
               std::string title_str = args_vec[3];
               std::string content_str = args_vec[4];

               std::map<std::string, std::string> extra_fields_map;

               for (size_t i = 5; i < args_vec.size(); i++) 
               {
                    size_t eq_pos = args_vec[i].find('=');

                    if (eq_pos != std::string::npos) 
                    {
                         extra_fields_map[args_vec[i].substr(0, eq_pos)] = args_vec[i].substr(eq_pos + 1);
                    }
               }

               cli_instance.AddDocument(col_str, id_str, title_str, content_str, extra_fields_map);
          } 
          else if (command_str == "search") 
          {
               if (args_vec.size() < 3) 
               {
                    std::cerr << "Error: 'search' command requires collection and query." << std::endl;
                    std::cerr << "Usage: xx search <col> <query> [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2]." << std::endl;

                    return 1;
               }

               std::string collection_str = args_vec[1];
               std::string query_str = args_vec[2];

               int limit_val = 10000;
               int offset_val = 0;

               std::string sort_str = "";

               bool exact_flag = false;
               bool highlight_flag = false;

               std::string hfields_str = "";
            
               for (size_t i = 3; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "--exact" || args_vec[i] == "-e") 
                    {
                         exact_flag = true;
                    } 
                    else if (args_vec[i] == "--highlight" || args_vec[i] == "-h") 
                    {
                         highlight_flag = true;
                    } 
                    else if (args_vec[i].substr(0, 9) == "--fields=") 
                    {
                         hfields_str = args_vec[i].substr(9);
                    } 
                    else 
                    {
                         try 
                         {
                              if (i == 3) 
                              {
                                   limit_val = std::stoi(args_vec[i]);
                              } 
                              else if (i == 4) 
                              {
                                   offset_val = std::stoi(args_vec[i]);
                              } 
                              else if (i == 5) 
                              {
                                   sort_str = args_vec[i];
                              }
                         } 
                         catch (...) 
                         {
                              /* Ignore. */
                         }
                    }
               }

               cli_instance.SearchDocuments(collection_str, query_str, limit_val, offset_val, sort_str, exact_flag, highlight_flag, hfields_str);
          } 
          else if (command_str == "vector-search") 
          {
               if (args_vec.size() < 3) 
               {
                    std::cerr << "Error: 'vector-search' command requires collection and vector." << std::endl;
                    std::cerr << "Usage: xx vector-search <col> <vector> [field] [limit]." << std::endl;

                    return 1;
               }

               std::string col_str = args_vec[1];
               std::string vector_str = args_vec[2];
               std::string field_str = (args_vec.size() >= 4) ? args_vec[3] : "embedding";

               int limit_val = (args_vec.size() >= 5) ? std::stoi(args_vec[4]) : 10;

               cli_instance.VectorSearch(col_str, vector_str, field_str, limit_val);
          } 
          else if (command_str == "open" && args_vec.size() >= 2) 
          {
               std::string format_val = "table";
               std::string col_doc_str = args_vec[1];
            
               for (size_t i = 2; i < args_vec.size(); i++) 
               {
                    if (args_vec[i] == "--json") 
                    {
                         format_val = "json";
                    } 
                    else if (args_vec[i] == "--text") 
                    {
                         format_val = "text";
                    } 
                    else if (args_vec[i] == "--table") 
                    {
                         format_val = "table";
                    }
               }
            
               size_t slash_pos = col_doc_str.find('/');

               if (slash_pos == std::string::npos) 
               {
                    std::cerr << "Error: Invalid format. Use: open <collection>/<document_id> [--table|--json|--text]" << std::endl;
                    return 1;
               }
            
               std::string collection_str = col_doc_str.substr(0, slash_pos);
               std::string document_id_str = col_doc_str.substr(slash_pos + 1);
            
               cli_instance.OpenDocument(collection_str, document_id_str, format_val);
          } 
          else if (command_str == "select" && args_vec.size() >= 3) 
          {
               std::string field_name_str = args_vec[1];
               std::string col_doc_str = args_vec[2];
            
               size_t slash_pos = col_doc_str.find('/');

               if (slash_pos == std::string::npos) 
               {
                    std::cerr << "Error: Invalid format. Use: select <field> <collection>/<document_id>" << std::endl;
                    return 1;
               }
            
               std::string collection_str = col_doc_str.substr(0, slash_pos);
               std::string document_id_str = col_doc_str.substr(slash_pos + 1);
            
               cli_instance.SelectField(field_name_str, collection_str, document_id_str);
          } 
          else if (command_str == "health") 
          {
               HLQueryCLI::HTTPResponse health_resp = cli_instance.MakeRequest("GET", "/health");
               HLQueryCLI::HTTPResponse stats_resp = cli_instance.MakeRequest("GET", "/stats");
            
               bool healthy = (health_resp.StatusCode == 200);
               bool stats_ok = (stats_resp.StatusCode == 200);
            
               if (healthy && stats_ok) 
               {
                    std::cout << "✓ Server is healthy" << std::endl;

                    if (health_resp.StatusCode == 200) 
                    {
                         try 
                         {
                              nlohmann::json health_json = nlohmann::json::parse(health_resp.Body);

                              if (health_json.contains("status")) 
                              {
                                   std::cout << "  Status: " << health_json["status"].get<std::string>() << "\n";
                              }
                         } 
                         catch (...) 
                         {
                              /* Ignore. */
                         }
                    }

                    return 0;
               } 
               else 
               {
                    std::cerr << "✗ Server health check failed" << std::endl;

                    if (!healthy) 
                    {
                         std::cerr << "  /health returned: " << health_resp.StatusCode << "\n";
                    }

                    if (!stats_ok) 
                    {
                         std::cerr << "  /stats returned: " << stats_resp.StatusCode << "\n";
                    }

                    return 2;  
               }
          } 
          else if (command_str == "verify") 
          {
               std::cout << "Running consistency verification suite...\n\n";
            
               std::cout << "1. Checking doctotal consistency...\n";
               cli_instance.ShowDocTotal();
            
               std::cout << "\n2. Checking collections consistency...\n";
               cli_instance.ListCollections(0, 10000, false);
            
               std::cout << "\n3. Sampling documents...\n";

               HLQueryCLI::HTTPResponse cols_resp = cli_instance.MakeRequest("GET", "/collections");

               if (cols_resp.StatusCode == 200) 
               {
                    try 
                    {
                         nlohmann::json cols_json = nlohmann::json::parse(cols_resp.Body);

                         if (cols_json.contains("collections") && cols_json["collections"].is_array()) 
                         {
                              int sampled = 0;

                              for (const auto& col : cols_json["collections"]) 
                              {
                                   std::string col_name = col["name"].get<std::string>();

                                   HLQueryCLI::HTTPResponse docs_resp = cli_instance.MakeRequest("GET", "/collections/" + col_name + "/documents?limit=1");

                                   if (docs_resp.StatusCode == 200) 
                                   {
                                        sampled++;
                                   }
                              }

                              std::cout << "  Sampled " << sampled << " collection(s)\n";
                         }
                    } 
                    catch (...) 
                    {
                         std::cerr << "  Error sampling documents\n";
                    }
               }
            
               if (cli_instance.GetExitCode() > 0) 
               {
                    return cli_instance.GetExitCode();
               }

               std::cout << "\n✓ All consistency checks passed\n";
          } 
          else if (command_str == "debug" && args_vec.size() >= 2 && args_vec[1] == "benchmark-state") 
          {
               std::cout << "Inspecting benchmark collections...\n\n";

               HLQueryCLI::HTTPResponse cols_resp = cli_instance.MakeRequest("GET", "/collections");

               if (cols_resp.StatusCode == 200) 
               {
                    try 
                    {
                         nlohmann::json cols_json = nlohmann::json::parse(cols_resp.Body);

                         if (cols_json.contains("collections") && cols_json["collections"].is_array()) 
                         {
                              int bench_count = 0;
                              int inconsistent = 0;

                              for (const auto& col : cols_json["collections"]) 
                              {
                                   std::string col_name = col["name"].get<std::string>();

                                   if (col_name.find("bench_collection_") == 0) 
                                   {
                                        bench_count++;

                                        int list_docs = col["num_documents"].get<int>();
                                
                                        HLQueryCLI::HTTPResponse col_info = cli_instance.MakeRequest("GET", "/collections/" + col_name);

                                        if (col_info.StatusCode == 200) 
                                        {
                                             try 
                                             {
                                                  nlohmann::json col_json = nlohmann::json::parse(col_info.Body);

                                                  int info_docs = col_json.contains("num_documents") ? col_json["num_documents"].get<int>() : 0;

                                                  if (list_docs != info_docs) 
                                                  {
                                                       inconsistent++;
                                                       std::cout << "  ✗ " << col_name << ": list=" << list_docs 
                                                                 << " info=" << info_docs << " (diff: " << (info_docs - list_docs) << ")\n";
                                                  }
                                             } 
                                             catch (...) 
                                             {
                                                  inconsistent++;
                                                  std::cout << "  ✗ " << col_name << ": failed to parse info\n";
                                             }
                                        }
                                   }
                              }

                              std::cout << "\nFound " << bench_count << " benchmark collection(s)\n";
                              std::cout << "Inconsistent counters: " << inconsistent << "\n";

                              if (inconsistent > 0) 
                              {
                                   return 3;  
                              }
                         }
                    } 
                    catch (...) 
                    {
                         std::cerr << "Error parsing collections\n";
                         return 2;
                    }
               }
          } 
          else if (command_str == "syn" || command_str == "syns" || command_str == "addsyn") 
          {
               if (command_str == "addsyn")
               {
                    if (args_vec.size() < 5)
                    {
                         std::cerr << "Error: 'addsyn' requires collection, id, root, and synonyms." << std::endl;
                         return 1;
                    }
                    std::string col_str = args_vec[1];
                    std::string id_str = args_vec[2];
                    std::string root_str = args_vec[3];
                    std::vector<std::string> syns_vec;
                    std::istringstream ss(args_vec[4]);
                    std::string s_str;
                    while (std::getline(ss, s_str, ',')) 
                    {
                         syns_vec.push_back(s_str);
                    }
                    cli_instance.AddSynonym(col_str, id_str, root_str, syns_vec);
               }
               else if (args_vec.size() >= 2 && args_vec[1] == "list") 
               {
                    std::string col_str = (args_vec.size() >= 3) ? args_vec[2] : "";

                    if (col_str.empty()) 
                    {
                         cli_instance.ListSynonymsCounts();
                    } 
                    else 
                    {
                         cli_instance.ListSynonyms(col_str);
                    }
               } 
               else if (args_vec.size() >= 5 && args_vec[1] == "add") 
               {
                    std::string col_str = args_vec[2];
                    std::string id_str = args_vec[3];
                    std::string root_str = args_vec[4];

                    std::vector<std::string> syns_vec;

                    std::istringstream ss(args_vec[5]);

                    std::string s_str;

                    while (std::getline(ss, s_str, ',')) 
                    {
                         syns_vec.push_back(s_str);
                    }

                    cli_instance.AddSynonym(col_str, id_str, root_str, syns_vec);
               } 
               else if (args_vec.size() >= 4 && (args_vec[1] == "del" || args_vec[1] == "delete")) 
               {
                    cli_instance.DeleteSynonym(args_vec[2], args_vec[3]);
               }
          } 
          else if (command_str == "stop" || command_str == "stopwords" || command_str == "addstop") 
          {
               if (command_str == "addstop")
               {
                    if (args_vec.size() < 3)
                    {
                         std::cerr << "Error: 'addstop' requires collection and word." << std::endl;
                         return 1;
                    }
                    cli_instance.AddStopword(args_vec[1], args_vec[2]);
               }
               else if (args_vec.size() >= 2 && args_vec[1] == "list") 
               {
                    std::string col_str = (args_vec.size() >= 3) ? args_vec[2] : "";

                    if (col_str.empty()) 
                    {
                         cli_instance.ListStopwordsCounts();
                    } 
                    else 
                    {
                         cli_instance.ListStopwords(col_str);
                    }
               } 
               else if (args_vec.size() >= 4 && args_vec[1] == "add") 
               {
               cli_instance.AddStopword(args_vec[2], args_vec[3]);
               } 
               else if (args_vec.size() >= 4 && (args_vec[1] == "del" || args_vec[1] == "delete")) 
               {
                    cli_instance.DeleteStopword(args_vec[2], args_vec[3]);
               }
          }
          else if (command_str == "keys")
          {
               if (args_vec.size() >= 2 && args_vec[1] == "list")
               {
                    cli_instance.ListKeys();
               }
               else if (args_vec.size() >= 2 && args_vec[1] == "create")
               {
                    std::string desc = "";
                    std::vector<std::string> cols;
                    std::vector<std::string> actions;
                    std::string embedded_filters = "";
                    bool allow_hanalyzer = false;

                    for (size_t i = 2; i < args_vec.size(); i++)
                    {
                         if (args_vec[i].substr(0, 7) == "--desc=") desc = args_vec[i].substr(7);
                         else if (args_vec[i].substr(0, 7) == "--cols=")
                         {
                              std::string c_str = args_vec[i].substr(7);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) cols.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 10) == "--actions=")
                         {
                              std::string a_str = args_vec[i].substr(10);
                              std::istringstream ss(a_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) actions.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 19) == "--embedded-filters=")
                         {
                              embedded_filters = args_vec[i].substr(19);
                         }
                         else if (args_vec[i] == "--hanalyzer")
                         {
                              allow_hanalyzer = true;
                         }
                    }

                    if (actions.empty()) actions = {"search"};

                    cli_instance.CreateKey(desc, cols, actions, 0, embedded_filters, allow_hanalyzer);
               }
               else if (args_vec.size() >= 3 && (args_vec[1] == "del" || args_vec[1] == "delete"))
               {
                    cli_instance.DeleteKey(args_vec[2]);
               }
               else if (args_vec.size() >= 3 && args_vec[1] == "update")
               {
                    std::string key_id = args_vec[2];
                    std::string desc = "";
                    std::vector<std::string> cols;
                    std::vector<std::string> actions;
                    std::vector<std::string> add_cols;
                    std::vector<std::string> remove_cols;
                    std::string embedded_filters = "";
                    bool allow_hanalyzer = false;

                    for (size_t i = 3; i < args_vec.size(); i++)
                    {
                         if (args_vec[i].substr(0, 7) == "--desc=") desc = args_vec[i].substr(7);
                         else if (args_vec[i].substr(0, 7) == "--cols=")
                         {
                              std::string c_str = args_vec[i].substr(7);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) cols.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 11) == "--add-cols=")
                         {
                              std::string c_str = args_vec[i].substr(11);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) add_cols.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 14) == "--remove-cols=")
                         {
                              std::string c_str = args_vec[i].substr(14);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) remove_cols.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 10) == "--actions=")
                         {
                              std::string a_str = args_vec[i].substr(10);
                              std::istringstream ss(a_str);
                              std::string s;
                              while (std::getline(ss, s, ',')) actions.push_back(s);
                         }
                         else if (args_vec[i].substr(0, 19) == "--embedded-filters=")
                         {
                              embedded_filters = args_vec[i].substr(19);
                         }
                         else if (args_vec[i] == "--hanalyzer")
                         {
                              allow_hanalyzer = true;
                         }
                    }

                    cli_instance.UpdateKey(key_id, desc, cols, actions, embedded_filters, allow_hanalyzer, add_cols, remove_cols);
               }
               else
               {
                    std::cout << "Usage: xx keys list\n";
                    std::cout << "       xx keys create [--desc=...] [--cols=c1,c2] [--actions=a1,a2] [--hanalyzer]\n";
                    std::cout << "       xx keys delete <id>\n";
                    std::cout << "       xx keys update <id> [--desc=...] [--cols=c1,c2] [--actions=a1,a2] [--hanalyzer] [--add-cols=c1,c2] [--remove-cols=c1,c2]\n";
               }
          }
          else if (command_str == "help")
          {
               cli_instance.ShowHelp();
          } 
          else 
          {
               std::cerr << "Unknown command: " << command_str << "." << std::endl;
               std::cerr << "Run 'xx help' for usage info." << std::endl;

               return 1;
          }

          return cli_instance.GetExitCode();
     } 
     catch (const std::exception& e) 
     {
          std::cerr << "Error: " << e.what() << "." << std::endl;

          return 1;
     }
}
