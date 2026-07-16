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

/*
 * File-backed data loader for the benchmark's --fake mode.
 * One JSON file represents one collection; files beginning with '_' contain
 * global benchmark resources.
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "benchmarkfixtures.h"

extern std::atomic<bool> g_benchmark_should_stop;

namespace
{
std::string ExecutablePath;

std::string ReplaceAll(std::string value, const std::string &needle, const std::string &replacement)
{
     size_t offset = 0;
     while ((offset = value.find(needle, offset)) != std::string::npos)
     {
          value.replace(offset, needle.size(), replacement);
          offset += replacement.size();
     }
     return value;
}

std::string Expand(std::string value, const std::string &name, const std::string &tag, size_t index)
{
     value = ReplaceAll(std::move(value), "{collection}", name);
     value = ReplaceAll(std::move(value), "{tag}", tag);
     value = ReplaceAll(std::move(value), "{index}", std::to_string(index + 1U));
     return value;
}

std::filesystem::path FixtureDirectory()
{
     if (const char *configured = std::getenv("HLQUERY_BENCHMARK_DIR"))
     {
          if (*configured != '\0')
          {
               return configured;
          }
     }

     const std::filesystem::path local = std::filesystem::path("run") / "benchmark";
     if (std::filesystem::is_directory(local))
     {
          return local;
     }

     if (!ExecutablePath.empty())
     {
          std::error_code error;
          std::filesystem::path executable = std::filesystem::absolute(ExecutablePath, error);
          if (!error)
          {
               const std::filesystem::path sibling = executable.parent_path().parent_path() / "benchmark";
               if (std::filesystem::is_directory(sibling))
               {
                    return sibling;
               }
          }
     }

     return local;
}

bool LoadJSON(const std::filesystem::path &path, nlohmann::json &value)
{
     std::ifstream input(path);
     if (!input)
     {
          std::cerr << "✗ Cannot open benchmark fixture '" << path.string() << "'.\n";
          return false;
     }

     try
     {
          input >> value;
     }
     catch (const std::exception &error)
     {
          std::cerr << "✗ Invalid benchmark fixture '" << path.string() << "': " << error.what() << ".\n";
          return false;
     }
     return true;
}

nlohmann::json CommonFields()
{
     return nlohmann::json::array({
          {{"name", "title"}, {"type", "string"}},
          {{"name", "content"}, {"type", "string"}},
          {{"name", "description"}, {"type", "string"}},
          {{"name", "labels"}, {"type", "string"}}
     });
}

bool ApplyGlobalFixture(BenchmarkClient &client, const nlohmann::json &fixture)
{
     size_t index = 0;
     for (const auto &entry : fixture.value("synonyms", nlohmann::json::array()))
     {
          if (!entry.is_object() || !entry.contains("root") || !entry.contains("synonyms"))
          {
               throw std::runtime_error("global synonym entries require root and synonyms");
          }
          const std::string id = entry.value("id", "benchmark_global_syn_" + std::to_string(++index));
          if (!client.AddGlobalSynonym(id, entry.at("root").get<std::string>(), entry.at("synonyms").get<std::vector<std::string>>()))
          {
               return false;
          }
     }

     for (const auto &word : fixture.value("stopwords", nlohmann::json::array()))
     {
          if (!client.AddGlobalStopword(word.get<std::string>()))
          {
               return false;
          }
     }
     return true;
}

bool LoadCollectionFixture(BenchmarkClient &client, const std::filesystem::path &path,
                           const nlohmann::json &fixture, bool verbose)
{
     if (!fixture.is_object() || !fixture.contains("collection"))
     {
          throw std::runtime_error("collection fixture requires a string 'collection'");
     }

     const std::string name = fixture.at("collection").get<std::string>();
     if (name.empty())
     {
          throw std::runtime_error("collection name cannot be empty");
     }

     nlohmann::json fields = CommonFields();
     for (const auto &field : fixture.value("fields", nlohmann::json::array()))
     {
          fields.push_back(field);
     }

     const std::string sorting = fixture.value("default_sorting_field", "");
     const nlohmann::json metadata = fixture.value("metadata", nlohmann::json::object());
     if (!client.CreateCollectionWithSchemaLocal(name, fields, sorting, metadata))
     {
          std::cerr << "✗ Failed to create collection from '" << path.string() << "'.\n";
          return false;
     }

     const std::vector<std::string> tags = fixture.value("tags", std::vector<std::string>{"sample"});
     if (tags.empty())
     {
          throw std::runtime_error("tags cannot be empty");
     }

     nlohmann::json documents = fixture.value("documents", nlohmann::json::array());
     const size_t count = fixture.value("count", documents.size());
     if (documents.empty() && count == 0)
     {
          throw std::runtime_error("fixture requires documents or a positive count");
     }

     const nlohmann::json defaults = fixture.value("document_defaults", nlohmann::json::object());
     const nlohmann::json sequence_fields = fixture.value("sequence_fields", nlohmann::json::object());
     const std::string title_template = fixture.value("title_template", "{collection} benchmark document {index}: {tag}");
     const std::string content_template = fixture.value("content_template", "Document {index} in Collection {collection}. The content covers {tag}. Lorem ipsum dolor sit amet consectetur adipiscing elit inserted by benchmark thread.");

     size_t inserted = 0;
     for (size_t i = 0; i < count; ++i)
     {
          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          const std::string &tag = tags[i % tags.size()];
          nlohmann::json document = documents.empty() ? nlohmann::json::object() : documents[i % documents.size()];
          if (!document.is_object())
          {
               throw std::runtime_error("documents must contain JSON objects");
          }

          for (auto it = defaults.begin(); it != defaults.end(); ++it)
          {
               if (!document.contains(it.key()))
               {
                    document[it.key()] = it.value();
               }
          }
          for (auto it = document.begin(); it != document.end(); ++it)
          {
               if (it.value().is_string())
               {
                    it.value() = Expand(it.value().get<std::string>(), name, tag, i);
               }
          }
          for (auto it = sequence_fields.begin(); it != sequence_fields.end(); ++it)
          {
               const double start = it.value().value("start", 1.0);
               const double step = it.value().value("step", 1.0);
               const double value = start + step * static_cast<double>(i);
               document[it.key()] = it.value().value("integer", false) ? nlohmann::json(static_cast<int64_t>(value)) : nlohmann::json(value);
          }

          const std::string base_id = document.value("id", name + "_" + std::to_string(i + 1U));
          document["id"] = (i < documents.size() || documents.empty()) ? base_id : base_id + "_" + std::to_string(i + 1U);
          document["document_id"] = document["id"];
          if (!document.contains("title")) document["title"] = Expand(title_template, name, tag, i);
          if (!document.contains("content")) document["content"] = Expand(content_template, name, tag, i);
          if (!document.contains("description")) document["description"] = "File-backed benchmark fixture for " + name;
          if (!document.contains("labels")) document["labels"] = nlohmann::json::array({name, tag}).dump();
          else if (document["labels"].is_array()) document["labels"] = document["labels"].dump();

          if (client.UpsertDocumentWithFieldsLocal(name, document))
          {
               ++inserted;
          }
     }

     size_t synonym_index = 0;
     for (const auto &entry : fixture.value("synonyms", nlohmann::json::array()))
     {
          const std::string id = entry.value("id", name + "_syn_" + std::to_string(++synonym_index));
          if (!client.AddSynonym(name, id, entry.at("root").get<std::string>(), entry.at("synonyms").get<std::vector<std::string>>()))
          {
               return false;
          }
     }
     for (const auto &word : fixture.value("stopwords", nlohmann::json::array()))
     {
          if (!client.AddStopword(name, word.get<std::string>()))
          {
               return false;
          }
     }
     for (const auto &alias : fixture.value("aliases", nlohmann::json::array()))
     {
          if (!client.CreateAlias(alias.get<std::string>(), name))
          {
               return false;
          }
     }

     std::cout << "✓ Inserted " << inserted << " fake documents into '" << name << "' from " << path.filename().string() << ".\n";
     if (verbose)
     {
          std::cout << "  ↳ Loaded schema and lexical resources from " << path.string() << ".\n";
     }
     return inserted == count;
}
} // namespace

void SetBenchmarkFixtureExecutable(const std::string &path)
{
     ExecutablePath = path;
}

BenchmarkFixtureLoadResult LoadBenchmarkFixtures(const std::string &base_url,
                                                  const std::string &auth_token,
                                                  bool reuse_collections,
                                                  bool verbose)
{
     const std::filesystem::path directory = FixtureDirectory();
     if (!std::filesystem::is_directory(directory))
     {
          return BenchmarkFixtureLoadResult::NotFound;
     }

     std::vector<std::filesystem::path> files;
     for (const auto &entry : std::filesystem::directory_iterator(directory))
     {
          if (entry.is_regular_file() && entry.path().extension() == ".json")
          {
               files.push_back(entry.path());
          }
     }
     std::sort(files.begin(), files.end());
     if (files.empty())
     {
          return BenchmarkFixtureLoadResult::NotFound;
     }

     BenchmarkClient client(base_url, auth_token, reuse_collections);
     const std::string connection_error = client.TestConnection();
     if (!connection_error.empty())
     {
          std::cerr << "✗ Cannot connect to server for benchmark fixtures: " << connection_error << ".\n";
          return BenchmarkFixtureLoadResult::Failed;
     }

     try
     {
          for (const auto &path : files)
          {
               nlohmann::json fixture;
               if (!LoadJSON(path, fixture))
               {
                    return BenchmarkFixtureLoadResult::Failed;
               }
               const bool global = path.filename().string().front() == '_';
               if (!(global ? ApplyGlobalFixture(client, fixture) : LoadCollectionFixture(client, path, fixture, verbose)))
               {
                    return BenchmarkFixtureLoadResult::Failed;
               }
          }
     }
     catch (const std::exception &error)
     {
          std::cerr << "✗ Invalid benchmark fixture in '" << directory.string() << "': " << error.what() << ".\n";
          return BenchmarkFixtureLoadResult::Failed;
     }

     return BenchmarkFixtureLoadResult::Loaded;
}
