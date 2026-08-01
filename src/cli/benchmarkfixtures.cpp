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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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
               const std::filesystem::path prefix = executable.parent_path().parent_path();
               const std::filesystem::path source_sibling = prefix / "benchmark";
               if (std::filesystem::is_directory(source_sibling))
               {
                    return source_sibling;
               }

               const std::filesystem::path installed_sibling = prefix / "share" / "hlquery" / "benchmark";
               if (std::filesystem::is_directory(installed_sibling))
               {
                    return installed_sibling;
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
          {{"name", "labels"}, {"type", "string"}},
          {{"name", "is_synthetic"}, {"type", "bool"}},
          {{"name", "data_notice"}, {"type", "string"}},
          {{"name", "embedding"}, {"type", "float[]"}},
          {{"name", "location"}, {"type", "geo_point"}},
          {{"name", "location_name"}, {"type", "string"}}
     });
}

uint64_t StableFixtureHash(const std::string &value)
{
     uint64_t hash = 1469598103934665603ULL;
     for (unsigned char character : value)
     {
          hash ^= character;
          hash *= 1099511628211ULL;
     }
     return hash;
}

nlohmann::json BuildFixtureEmbedding(const std::string &collection,
                                     const std::string &tag,
                                     size_t index)
{
     const uint64_t seed = StableFixtureHash(collection + ":" + tag + ":" + std::to_string(index));
     nlohmann::json embedding = nlohmann::json::array();

     for (int dimension = 0; dimension < 4; ++dimension)
     {
          const uint64_t mixed = seed ^
                                 (static_cast<uint64_t>(dimension + 1) * 0x9e3779b97f4a7c15ULL) ^
                                 (static_cast<uint64_t>(index + 1U) * 101ULL);
          embedding.push_back(static_cast<double>(mixed % 2001U) / 1000.0 - 1.0);
     }

     return embedding;
}

std::pair<double, double> FixtureCollectionCenter(const std::string &collection)
{
     static const std::unordered_map<std::string, std::pair<double, double>> centers = {
          {"anomalies", {39.7392, -104.9903}},
          {"art", {40.7614, -73.9776}},
          {"books", {42.3601, -71.0589}},
          {"ecommerce", {47.6062, -122.3321}},
          {"fashion", {48.8566, 2.3522}},
          {"finance", {40.7069, -74.0113}},
          {"food", {40.7306, -73.9352}},
          {"history", {38.8895, -77.0353}},
          {"math", {42.3736, -71.1097}},
          {"movies", {34.0522, -118.2437}},
          {"music", {36.1627, -86.7816}},
          {"people", {41.8781, -87.6298}},
          {"saas", {37.7749, -122.4194}},
          {"science", {37.7749, -122.4194}},
          {"sports", {39.9526, -75.1652}},
          {"stocks", {40.7069, -74.0113}},
          {"technology", {37.3861, -122.0839}},
          {"travel", {47.6062, -122.3321}},
          {"universities", {39.8283, -98.5795}}};

     const auto found = centers.find(collection);
     return found == centers.end() ? std::make_pair(40.7128, -74.0060) : found->second;
}

nlohmann::json BuildFixtureLocation(const std::string &collection, size_t index)
{
     const auto center = FixtureCollectionCenter(collection);
     const int row = static_cast<int>(index % 5U) - 2;
     const int column = static_cast<int>((index / 5U) % 5U) - 2;

     return nlohmann::json::array({
          center.first + static_cast<double>(row) * 0.018,
          center.second + static_cast<double>(column) * 0.024
     });
}

std::string FixtureLocationName(const std::string &collection)
{
     static const std::unordered_map<std::string, std::string> names = {
          {"anomalies", "Synthetic Denver operations region"},
          {"art", "Synthetic New York arts district"},
          {"books", "Synthetic Boston reading district"},
          {"ecommerce", "Synthetic Seattle retail district"},
          {"fashion", "Synthetic Paris design district"},
          {"finance", "Synthetic New York finance district"},
          {"food", "Synthetic New York restaurant district"},
          {"history", "Synthetic Washington history district"},
          {"math", "Synthetic Cambridge learning district"},
          {"movies", "Synthetic Los Angeles studio district"},
          {"music", "Synthetic Nashville music district"},
          {"people", "Synthetic United States profile location"},
          {"saas", "Synthetic San Francisco software district"},
          {"science", "Synthetic San Francisco research district"},
          {"sports", "Synthetic Philadelphia sports district"},
          {"stocks", "Synthetic market-data region"},
          {"technology", "Synthetic Mountain View technology district"},
          {"travel", "Synthetic Seattle travel hub"},
          {"universities", "Synthetic United States campus location"}};

     const auto found = names.find(collection);
     return found == names.end() ? "Synthetic benchmark location" : found->second;
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
     const std::string title_template = fixture.value("title_template", "{collection} demo {index}: {tag}");
     const std::string content_template = fixture.value("content_template", "Synthetic {collection} demo record {index} covering {tag} with concrete searchable context.");

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
          if (!document.contains("description")) document["description"] = "Synthetic " + name + " sample about " + tag + " for search demonstrations.";
          if (!document.contains("labels")) document["labels"] = nlohmann::json::array({name, tag, "demo", "synthetic"}).dump();
          else if (document["labels"].is_array()) document["labels"] = document["labels"].dump();
          if (!document.contains("is_synthetic")) document["is_synthetic"] = true;
          if (!document.contains("data_notice")) document["data_notice"] = "Public HLQuery demonstration data. University names and campus locations are real catalog references; generated descriptions, people, organizations, artworks, rankings, incidents, and market instruments are synthetic.";
          if (!document.contains("embedding")) document["embedding"] = BuildFixtureEmbedding(name, tag, i);
          /* Do not fabricate exact campus coordinates for real university names. */
          if (!document.contains("location") && name != "universities") document["location"] = BuildFixtureLocation(name, i);
          if (!document.contains("location_name")) document["location_name"] = FixtureLocationName(name);

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

     std::cout << "✓ Inserted " << inserted << " synthetic demo documents into '" << name << "' from " << path.filename().string() << ".\n";
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
