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

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "runtime/clock.h"

/* Signal and stat helpers. */

void BenchmarkSignalHandler(int signal);

void ResetGlobalStats();

void ResetProgressBar();
void PrintSpinner(const std::string &label, int attempt, int total_attempts, bool done);

/* Main mode functions. */

void RunSearches(const std::string &base_url, const std::string &auth_token);

void RunDetailedBenchmark(const std::string &base_url, const std::string &auth_token, int num_collections, int num_documents, int num_threads, int batch_size, bool reuse_collections);

void RunFloodBenchmark(const std::string &base_url, const std::string &auth_token, int num_threads, bool verbose, bool reuse_collections);

void DumpAllCollections(const std::string &base_url, const std::string &auth_token);

bool CreateFakeCollections(const std::string &base_url, const std::string &auth_token, bool reuse_collections, bool verbose);

void CreateCollectionsThread(const std::string &base_url, const std::string &auth_token, int start_idx, int end_idx, bool collect_metrics, int total_collections, bool reuse_collections);

void InsertDocumentsThread(const std::string &base_url, const std::string &auth_token, int num_collections, int docs_per_collection, int remaining_docs, int thread_id, int thread_count, int batch_size, bool collect_metrics, int total_documents, const std::string &run_id, bool reuse_collections);

void InsertAdditionalDocumentsThread(const std::string &base_url, const std::string &auth_token, int num_collections, int start_doc_idx, int additional_docs, int thread_id, int thread_count, int batch_size, bool collect_metrics, int total_documents, const std::string &run_id, bool reuse_collections);

void GetFinalCounts(BenchmarkClient &client, AdvancedMetrics &metrics, bool verbose, int num_collections = -1);

void CheckConsistency(BenchmarkClient &client, bool verbose, int num_collections = -1);

std::vector<int64_t> CalculatePercentiles(const std::vector<int64_t> &timings);

void WriteAdvancedJSON(const std::string &filename, const AdvancedMetrics &metrics);

void CleanupBenchmarkCollections(BenchmarkClient &client, bool verbose);

void LogOutput(const std::string &message);

struct DurabilityConfig
{
     std::string WalSyncMode = "unknown";
     std::string WalBytesPerSync = "unknown";
     std::string ManualWalFlush = "unknown";
};

std::string TrimWhitespace(const std::string &input)
{
     size_t start = input.find_first_not_of(" \t\r\n");
     if (start == std::string::npos)
     {
          return "";
     }

     size_t end = input.find_last_not_of(" \t\r\n");

     return input.substr(start, end - start + 1);
}

static std::string PadBenchmarkLabel(const std::string &Label, size_t Width = 23)
{
     if (Label.size() >= Width)
     {
          return Label;
     }

     return Label + std::string(Width - Label.size(), ' ');
}

static void PrintBenchmarkTitle(const std::string &Title)
{
     std::cout << Title << "\n";
     std::cout << std::string(Title.size(), '-') << "\n";
}

static void PrintBenchmarkSection(const std::string &Title)
{
     std::cout << "\n" << Title << "\n";
}

static void PrintBenchmarkValue(const std::string &Label, const std::string &Value)
{
     std::cout << "  · " << PadBenchmarkLabel(Label) << Value << "\n";
}

static void PrintBenchmarkStatus(const std::string &Label, const std::string &Value)
{
     LogOutput("  · " + PadBenchmarkLabel(Label) + Value + "\n");
}

static std::string FormatBenchmarkRate(double Value)
{
     std::ostringstream Stream;

     Stream << std::fixed << std::setprecision(1) << Value;

     return Stream.str();
}

struct FakeCollectionSpec
{
     std::string Name;
     std::vector<std::string> Tags;
};

struct RealDocSeed
{
     std::string Title;
     std::string Content;
};

struct UniversityBenchmarkSeed
{
     const char *Name;
     const char *State;
     const char *City;
     const char *Type;
};

struct PersonBenchmarkSeed
{
     std::string FirstName;
     std::string MiddleName;
     std::string LastName;
     std::string Biography;
};

struct AnomalyBenchmarkSeed
{
     std::string Id;
     std::string Title;
     std::string Content;
     std::string Category;
     std::string Summary;
     std::string Source;
     std::string Service;
     std::string Region;
     std::string ExpectedPattern;
     std::string ObservedSignal;
     std::string Severity;
     std::string ExpectedLabel;
};

struct FakeSynonymSeed
{
     std::string Root;
     std::vector<std::string> Synonyms;
};

static void AddFakeBenchmarkSearchFields(nlohmann::json &fields)
{
     fields.push_back({{"name", "title"}, {"type", "string"}});
     fields.push_back({{"name", "content"}, {"type", "string"}});
     fields.push_back({{"name", "description"}, {"type", "string"}});
     fields.push_back({{"name", "labels"}, {"type", "string"}});
     fields.push_back({{"name", "embedding"}, {"type", "float[]"}});
     fields.push_back({{"name", "location"}, {"type", "geo_point"}});
     fields.push_back({{"name", "location_name"}, {"type", "string"}});
}

static uint64_t StableBenchmarkHash(const std::string &value)
{
     uint64_t hash = 1469598103934665603ULL;

     for (unsigned char ch : value)
     {
          hash ^= ch;
          hash *= 1099511628211ULL;
     }

     return hash;
}

static nlohmann::json BuildFakeBenchmarkEmbedding(const std::string &collection,
                                                  const std::string &tag,
                                                  size_t index)
{
     const uint64_t seed = StableBenchmarkHash(collection + ":" + tag + ":" + std::to_string(index));
     nlohmann::json embedding = nlohmann::json::array();

     for (int dim = 0; dim < 4; ++dim)
     {
          const uint64_t shifted = seed ^ (static_cast<uint64_t>(dim + 1) * 0x9e3779b97f4a7c15ULL) ^ (static_cast<uint64_t>(collection.size() + tag.size() + index) * 101ULL);
          const double value = static_cast<double>(shifted % 2001U) / 1000.0 - 1.0;
          embedding.push_back(value);
     }

     return embedding;
}

static std::pair<double, double> FakeBenchmarkCollectionCenter(const std::string &collection)
{
     static const std::unordered_map<std::string, std::pair<double, double>> centers = {
          {"art", {40.7614, -73.9776}},
          {"books", {42.3601, -71.0589}},
          {"food", {40.7306, -73.9352}},
          {"history", {38.8895, -77.0353}},
          {"math", {42.3736, -71.1097}},
          {"movies", {34.0522, -118.2437}},
          {"music", {36.1627, -86.7816}},
          {"people", {41.8781, -87.6298}},
          {"science", {37.7749, -122.4194}},
          {"sports", {39.9526, -75.1652}},
          {"stocks", {40.7069, -74.0113}},
          {"technology", {37.3861, -122.0839}},
          {"travel", {47.6062, -122.3321}},
          {"universities", {39.8283, -98.5795}}};

     auto it = centers.find(collection);
     if (it != centers.end())
     {
          return it->second;
     }

     return {40.7128, -74.0060};
}

static nlohmann::json BuildFakeBenchmarkLocation(const std::string &collection, size_t index)
{
     const auto center = FakeBenchmarkCollectionCenter(collection);
     const int row = static_cast<int>(index % 5U) - 2;
     const int col = static_cast<int>((index / 5U) % 5U) - 2;
     const double lat = center.first + static_cast<double>(row) * 0.018;
     const double lon = center.second + static_cast<double>(col) * 0.024;

     nlohmann::json location = nlohmann::json::array();
     location.push_back(lat);
     location.push_back(lon);
     return location;
}

static std::string BuildFakeBenchmarkLocationName(const std::string &collection)
{
     static const std::unordered_map<std::string, std::string> names = {
          {"art", "New York gallery district"},
          {"books", "Boston reading district"},
          {"food", "New York restaurant district"},
          {"history", "Washington monument district"},
          {"math", "Cambridge academic district"},
          {"movies", "Los Angeles studio district"},
          {"music", "Nashville music district"},
          {"people", "Chicago community district"},
          {"science", "San Francisco research district"},
          {"sports", "Philadelphia stadium district"},
          {"stocks", "New York financial district"},
          {"technology", "Mountain View technology district"},
          {"travel", "Seattle travel hub"},
          {"universities", "United States campus reference"}};

     auto it = names.find(collection);
     return it == names.end() ? "Benchmark geo district" : it->second;
}

static std::string Capitalize(const std::string &input)
{
     if (input.empty())
     {
          return input;
     }

     std::string result = input;
     result[0] = static_cast<char>(std::toupper(result[0]));
     return result;
}

static std::string RemoveCommas(const std::string &input)
{
     if (input.find(',') == std::string::npos)
     {
          return input;
     }

     std::string result = input;
     for (char &ch : result)
     {
          if (ch == ',')
          {
               ch = ' ';
          }
     }

     return result;
}

static std::string HumanizeIdentifier(const std::string &input)
{
     std::string result = input;

     std::replace(result.begin(), result.end(), '_', ' ');

     return result;
}

static void AddUniqueText(std::vector<std::string> &values, std::unordered_set<std::string> &seen, const std::string &value)
{
     const std::string trimmed = TrimWhitespace(value);
     if (trimmed.empty() || seen.find(trimmed) != seen.end())
     {
          return;
     }

     seen.insert(trimmed);
     values.push_back(trimmed);
}

static std::string JoinTextValues(const std::vector<std::string> &values, const std::string &separator)
{
     std::string joined;
     for (const std::string &value : values)
     {
          if (!joined.empty())
          {
               joined += separator;
          }
          joined += value;
     }

     return joined;
}

static const std::vector<UniversityBenchmarkSeed> &GetUniversityBenchmarkSeeds()
{
     static const std::vector<UniversityBenchmarkSeed> seeds = {
          {"Harvard University", "Massachusetts", "Cambridge", "private_research"},
          {"Stanford University", "California", "Stanford", "private_research"},
          {"Massachusetts Institute of Technology", "Massachusetts", "Cambridge", "private_research"},
          {"University of California Berkeley", "California", "Berkeley", "public_research"},
          {"University of Washington Seattle", "Washington", "Seattle", "public_research"},
          {"University of Michigan Ann Arbor", "Michigan", "Ann Arbor", "public_research"},
          {"Cornell University", "New York", "Ithaca", "private_research"},
          {"Columbia University", "New York", "New York", "private_research"},
          {"University of Pennsylvania", "Pennsylvania", "Philadelphia", "private_research"},
          {"Yale University", "Connecticut", "New Haven", "private_research"},
          {"Princeton University", "New Jersey", "Princeton", "private_research"},
          {"University of California Los Angeles", "California", "Los Angeles", "public_research"},
          {"University of Chicago", "Illinois", "Chicago", "private_research"},
          {"Johns Hopkins University", "Maryland", "Baltimore", "private_research"},
          {"University of California San Diego", "California", "La Jolla", "public_research"},
          {"University of Wisconsin Madison", "Wisconsin", "Madison", "public_research"},
          {"Duke University", "North Carolina", "Durham", "private_research"},
          {"Northwestern University", "Illinois", "Evanston", "private_research"},
          {"University of Illinois Urbana Champaign", "Illinois", "Urbana Champaign", "public_research"},
          {"New York University", "New York", "New York", "private_research"},
          {"University of Texas at Austin", "Texas", "Austin", "public_research"},
          {"University of North Carolina Chapel Hill", "North Carolina", "Chapel Hill", "public_research"},
          {"Pennsylvania State University", "Pennsylvania", "University Park", "public_research"},
          {"University of Minnesota Twin Cities", "Minnesota", "Minneapolis", "public_research"},
          {"University of Florida", "Florida", "Gainesville", "public_research"},
          {"University of Southern California", "California", "Los Angeles", "private_research"},
          {"Carnegie Mellon University", "Pennsylvania", "Pittsburgh", "private_research"},
          {"Georgia Institute of Technology", "Georgia", "Atlanta", "public_research"},
          {"Ohio State University", "Ohio", "Columbus", "public_research"},
          {"Purdue University", "Indiana", "West Lafayette", "public_research"},
          {"University of Maryland College Park", "Maryland", "College Park", "public_research"},
          {"University of California Davis", "California", "Davis", "public_research"},
          {"University of California Irvine", "California", "Irvine", "public_research"},
          {"University of California Santa Barbara", "California", "Santa Barbara", "public_research"},
          {"University of Colorado Boulder", "Colorado", "Boulder", "public_research"},
          {"University of Virginia", "Virginia", "Charlottesville", "public_research"},
          {"Vanderbilt University", "Tennessee", "Nashville", "private_research"},
          {"Rice University", "Texas", "Houston", "private_research"},
          {"Washington University in St Louis", "Missouri", "St Louis", "private_research"},
          {"Emory University", "Georgia", "Atlanta", "private_research"},
          {"University of Arizona", "Arizona", "Tucson", "public_research"},
          {"Arizona State University", "Arizona", "Tempe", "public_research"},
          {"Michigan State University", "Michigan", "East Lansing", "public_research"},
          {"Rutgers University New Brunswick", "New Jersey", "New Brunswick", "public_research"},
          {"Texas A and M University", "Texas", "College Station", "public_research"},
          {"Indiana University Bloomington", "Indiana", "Bloomington", "public_research"},
          {"University of Pittsburgh", "Pennsylvania", "Pittsburgh", "public_research"},
          {"Boston University", "Massachusetts", "Boston", "private_research"},
          {"Brown University", "Rhode Island", "Providence", "private_research"},
          {"Dartmouth College", "New Hampshire", "Hanover", "private_research"},
          {"University of Utah", "Utah", "Salt Lake City", "public_research"},
          {"University of Iowa", "Iowa", "Iowa City", "public_research"},
          {"Iowa State University", "Iowa", "Ames", "public_research"},
          {"University of Oregon", "Oregon", "Eugene", "public_research"},
          {"Oregon State University", "Oregon", "Corvallis", "public_research"},
          {"University of California Santa Cruz", "California", "Santa Cruz", "public_research"},
          {"University of California Riverside", "California", "Riverside", "public_research"},
          {"University of California San Francisco", "California", "San Francisco", "public_health_sciences"},
          {"University of Massachusetts Amherst", "Massachusetts", "Amherst", "public_research"},
          {"University of Connecticut", "Connecticut", "Storrs", "public_research"},
          {"University of Delaware", "Delaware", "Newark", "public_research"},
          {"University of Georgia", "Georgia", "Athens", "public_research"},
          {"University of Kansas", "Kansas", "Lawrence", "public_research"},
          {"University of Kentucky", "Kentucky", "Lexington", "public_research"},
          {"University of Missouri", "Missouri", "Columbia", "public_research"},
          {"University of Nebraska Lincoln", "Nebraska", "Lincoln", "public_research"},
          {"University of New Mexico", "New Mexico", "Albuquerque", "public_research"},
          {"University of Oklahoma", "Oklahoma", "Norman", "public_research"},
          {"University of South Carolina", "South Carolina", "Columbia", "public_research"},
          {"University of Tennessee Knoxville", "Tennessee", "Knoxville", "public_research"},
          {"University of Vermont", "Vermont", "Burlington", "public_research"},
          {"Virginia Tech", "Virginia", "Blacksburg", "public_research"},
          {"North Carolina State University", "North Carolina", "Raleigh", "public_research"},
          {"Florida State University", "Florida", "Tallahassee", "public_research"},
          {"University of Miami", "Florida", "Coral Gables", "private_research"},
          {"Georgetown University", "District of Columbia", "Washington", "private_research"},
          {"George Washington University", "District of Columbia", "Washington", "private_research"},
          {"Tufts University", "Massachusetts", "Medford", "private_research"},
          {"Northeastern University", "Massachusetts", "Boston", "private_research"},
          {"Syracuse University", "New York", "Syracuse", "private_research"},
          {"Rensselaer Polytechnic Institute", "New York", "Troy", "private_research"},
          {"University at Buffalo", "New York", "Buffalo", "public_research"},
          {"Stony Brook University", "New York", "Stony Brook", "public_research"},
          {"Binghamton University", "New York", "Binghamton", "public_research"},
          {"Temple University", "Pennsylvania", "Philadelphia", "public_research"},
          {"Drexel University", "Pennsylvania", "Philadelphia", "private_research"},
          {"Case Western Reserve University", "Ohio", "Cleveland", "private_research"},
          {"University of Cincinnati", "Ohio", "Cincinnati", "public_research"},
          {"University of Houston", "Texas", "Houston", "public_research"},
          {"Baylor University", "Texas", "Waco", "private_research"},
          {"Southern Methodist University", "Texas", "Dallas", "private_research"},
          {"Tulane University", "Louisiana", "New Orleans", "private_research"},
          {"Louisiana State University", "Louisiana", "Baton Rouge", "public_research"},
          {"University of Alabama", "Alabama", "Tuscaloosa", "public_research"},
          {"Auburn University", "Alabama", "Auburn", "public_research"},
          {"Clemson University", "South Carolina", "Clemson", "public_research"},
          {"Colorado State University", "Colorado", "Fort Collins", "public_research"},
          {"Washington State University", "Washington", "Palouse", "public_research"},
          {"University of Nevada Reno", "Nevada", "Reno", "public_research"},
          {"Brigham Young University", "Utah", "Provo", "private_research"}};

     return seeds;
}

static std::vector<std::string> BuildUniversityLocationAliases(const UniversityBenchmarkSeed &seed)
{
     std::vector<std::string> aliases;
     std::unordered_set<std::string> seen;
     const std::string name = seed.Name;
     const std::string state = seed.State;
     const std::string city = seed.City;

     AddUniqueText(aliases, seen, city);
     AddUniqueText(aliases, seen, state);
     AddUniqueText(aliases, seen, city + " campus");
     AddUniqueText(aliases, seen, state + " university");

     if (state == "Massachusetts")
     {
          AddUniqueText(aliases, seen, "New England");
          AddUniqueText(aliases, seen, "Massachusetts college");
     }

     if (city == "Cambridge")
     {
          AddUniqueText(aliases, seen, "Boston");
          AddUniqueText(aliases, seen, "Greater Boston");
          AddUniqueText(aliases, seen, "Boston area");
          AddUniqueText(aliases, seen, "Massachusetts Bay");
     }

     if (city == "Boston" || city == "Medford")
     {
          AddUniqueText(aliases, seen, "Cambridge");
          AddUniqueText(aliases, seen, "Greater Boston");
          AddUniqueText(aliases, seen, "Boston area");
     }

     if (name.find("Harvard") != std::string::npos)
     {
          AddUniqueText(aliases, seen, "Harvard Square");
          AddUniqueText(aliases, seen, "Cambridge Massachusetts");
     }

     if (name.find("Massachusetts Institute of Technology") != std::string::npos)
     {
          AddUniqueText(aliases, seen, "MIT");
          AddUniqueText(aliases, seen, "Kendall Square");
          AddUniqueText(aliases, seen, "Cambridge Massachusetts");
     }

     if (city == "Stanford" || city == "Berkeley" || city == "San Francisco")
     {
          AddUniqueText(aliases, seen, "Bay Area");
          AddUniqueText(aliases, seen, "San Francisco Bay Area");
     }

     if (city == "Stanford")
     {
          AddUniqueText(aliases, seen, "Silicon Valley");
          AddUniqueText(aliases, seen, "Palo Alto");
     }

     if (city == "Berkeley")
     {
          AddUniqueText(aliases, seen, "East Bay");
          AddUniqueText(aliases, seen, "Oakland");
     }

     if (city == "La Jolla")
     {
          AddUniqueText(aliases, seen, "San Diego");
          AddUniqueText(aliases, seen, "San Diego area");
     }

     if (city == "New York")
     {
          AddUniqueText(aliases, seen, "NYC");
          AddUniqueText(aliases, seen, "Manhattan");
     }

     if (city == "Washington")
     {
          AddUniqueText(aliases, seen, "Washington DC");
          AddUniqueText(aliases, seen, "DC");
     }

     if (city == "Minneapolis")
     {
          AddUniqueText(aliases, seen, "Twin Cities");
          AddUniqueText(aliases, seen, "St Paul");
     }

     if (city == "University Park")
     {
          AddUniqueText(aliases, seen, "State College");
          AddUniqueText(aliases, seen, "central Pennsylvania");
     }

     if (city == "Urbana Champaign")
     {
          AddUniqueText(aliases, seen, "Champaign Urbana");
          AddUniqueText(aliases, seen, "Urbana");
          AddUniqueText(aliases, seen, "Champaign");
     }

     if (city == "Amherst")
     {
          AddUniqueText(aliases, seen, "Pioneer Valley");
          AddUniqueText(aliases, seen, "western Massachusetts");
     }

     return aliases;
}

static std::string BuildUniversityBenchmarkContent(const UniversityBenchmarkSeed &seed, size_t index)
{
     const std::vector<std::string> programs = {
          "engineering", "computer science", "medicine", "business", "public policy",
          "life sciences", "data science", "law", "education", "environmental research"};
     const std::vector<std::string> campus_terms = {
          "campus research labs", "student admissions", "faculty programs", "alumni networks",
          "graduate degrees", "public service", "technology transfer", "library systems"};

     const std::string &program_a = programs[index % programs.size()];
     const std::string &program_b = programs[(index + 3U) % programs.size()];
     const std::string &term_a = campus_terms[index % campus_terms.size()];
     const std::string &term_b = campus_terms[(index + 5U) % campus_terms.size()];

     return std::string(seed.Name) + " is a " + HumanizeIdentifier(seed.Type) + " institution in " + seed.City + ", " + seed.State +
            ". The profile highlights " + program_a + ", " +
            program_b + ", " + term_a + ", " + term_b +
            ", student body context, research visibility, and the surrounding campus community.";
}

static PersonBenchmarkSeed BuildPersonBenchmarkSeed(size_t index)
{
     static const std::vector<std::string> first_names = {
          "Adrian", "Bianca", "Caleb", "Diana", "Elias", "Farah", "Gabriel", "Helena", "Isaac", "Julia"};
     static const std::vector<std::string> middle_names = {
          "Alexis", "Brooke", "Cameron", "Drew", "Emery", "Francis", "Gray", "Harper", "Indigo", "Jordan"};
     static const std::vector<std::string> last_names = {
          "Anderson", "Bennett", "Castillo", "Donovan", "Ellis", "Foster", "Garcia", "Hughes", "Ibrahim", "Jensen"};
     static const std::vector<std::string> occupations = {
          "community librarian", "software engineer", "urban planner", "science teacher", "museum curator",
          "small business owner", "public health analyst", "civil engineer", "documentary editor", "food writer"};
     static const std::vector<std::string> locations = {
          "Portland", "Austin", "Chicago", "Atlanta", "Seattle",
          "Denver", "Boston", "Phoenix", "Minneapolis", "San Diego"};
     static const std::vector<std::string> interests = {
          "local history and neighborhood archives", "accessible technology and mentoring",
          "public transit and walkable streets", "hands-on science education and astronomy",
          "independent art spaces and oral histories", "regional markets and practical entrepreneurship",
          "community wellness and data literacy", "sustainable buildings and resilient infrastructure",
          "visual storytelling and public media", "seasonal cooking and family recipes"};

     const std::string &first_name = first_names[index % first_names.size()];
     const std::string &middle_name = middle_names[(index / first_names.size()) % middle_names.size()];
     const std::string &last_name = last_names[(index * 3U + index / first_names.size()) % last_names.size()];
     const std::string &occupation = occupations[(index * 7U) % occupations.size()];
     const std::string &location = locations[(index * 3U + 2U) % locations.size()];
     const std::string &interest = interests[(index * 9U + 1U) % interests.size()];
     const std::string full_name = first_name + " " + middle_name + " " + last_name;

     return {
          first_name,
          middle_name,
          last_name,
          full_name + " is a fictional " + occupation + " based in " + location +
               ". This synthetic benchmark profile describes work involving " + interest +
               ". The biography is intentionally fictional and exists only for search testing."};
}

static std::string Slugify(const std::string &input)
{
     std::string slug;
     slug.reserve(input.size());

     bool last_was_dash = false;
     for (unsigned char ch : input)
     {
          if (std::isalnum(ch))
          {
               slug.push_back(static_cast<char>(std::tolower(ch)));
               last_was_dash = false;
          }
          else if (!last_was_dash && !slug.empty())
          {
               slug.push_back('-');
               last_was_dash = true;
          }
     }

     while (!slug.empty() && slug.back() == '-')
     {
          slug.pop_back();
     }

     return slug;
}

static std::string MakeMeaningfulDocId(const std::string &collection,
                                       const std::string &title,
                                       const std::string &content,
                                       int index,
                                       std::unordered_set<std::string> &used_ids)
{
     std::string base = Slugify(title);
     if (base.empty())
     {
          base = Slugify(content);
     }
     if (base.empty())
     {
          base = "item";
     }

     if (base.size() > 42)
     {
          base = base.substr(0, 42);
          while (!base.empty() && base.back() == '-')
          {
               base.pop_back();
          }
     }

     std::string candidate = collection + "_" + base;
     if (used_ids.find(candidate) == used_ids.end())
     {
          used_ids.insert(candidate);
          return candidate;
     }

     candidate = collection + "_" + base + "-" + std::to_string(index + 1);
     if (used_ids.find(candidate) == used_ids.end())
     {
          used_ids.insert(candidate);
          return candidate;
     }

     int suffix = 2;
     while (true)
     {
          std::string next = collection + "_" + base + "-" + std::to_string(index + 1) + "-" + std::to_string(suffix++);
          if (used_ids.find(next) == used_ids.end())
          {
               used_ids.insert(next);
               return next;
          }
     }
}

static std::vector<nlohmann::json> BuildAnomalyBenchmarkDocuments()
{
     const std::vector<std::string> regions = {"us-east", "us-west", "eu-central", "sa-south"};
     const std::vector<std::string> services = {"payments", "auth", "search", "billing", "ingest"};
     std::vector<nlohmann::json> docs;
     docs.reserve(100);

     for (size_t i = 1; i <= 88; ++i)
     {
          const std::string &service = services[(i - 1U) % services.size()];
          const std::string &region = regions[(i - 1U) % regions.size()];
          const int latency = 110 + static_cast<int>((i * 7U) % 45U);
          const double error_rate = 0.2 + static_cast<double>((i * 3U) % 9U) / 10.0;
          const int requests = 930 + static_cast<int>((i * 17U) % 160U);
          const std::string padded = std::string(i < 10 ? "00" : (i < 100 ? "0" : "")) + std::to_string(i);

          std::ostringstream content;
          content << service << " in " << region << " handled " << requests
                  << " requests with p95 latency " << latency << " ms, error rate "
                  << std::fixed << std::setprecision(1) << error_rate
                  << " percent, stable retry volume, and normal customer behavior.";

          std::ostringstream observed;
          observed << "latency=" << latency << "ms error_rate=" << std::fixed << std::setprecision(1)
                   << error_rate << "% requests=" << requests;

          nlohmann::json doc;
          doc["id"] = "anom_normal_" + padded;
          doc["document_id"] = doc["id"];
          doc["title"] = "Routine " + service + " telemetry window " + padded;
          doc["content"] = content.str();
          doc["description"] = "Baseline operational record with expected values";
          doc["labels"] = "[\"normal\",\"telemetry\",\"baseline\"]";
          doc["embedding"] = BuildFakeBenchmarkEmbedding("anomalies", service, i);
          doc["location"] = BuildFakeBenchmarkLocation("technology", i);
          doc["location_name"] = "Synthetic anomaly benchmark operations";
          doc["category"] = "telemetry";
          doc["summary"] = "Baseline operational record with expected values";
          doc["source"] = "internal_observability";
          doc["service"] = service;
          doc["region"] = region;
          doc["expected_pattern"] = "p95 latency 100-170 ms, error rate below 1.5 percent, requests near daily baseline";
          doc["observed_signal"] = observed.str();
          doc["severity"] = "normal";
          doc["expected_label"] = "normal";
          doc["timestamp"] = static_cast<int64_t>(1777495000000LL + static_cast<int64_t>(i * 60000U));
          docs.push_back(std::move(doc));
     }

     const std::vector<AnomalyBenchmarkSeed> outliers = {
          {"anom_outlier_001", "Payments success spike with revenue drop", "Payments reported 99.9 percent authorization success while captured revenue dropped 74 percent in the same window. The metrics conflict and suggest silent settlement failure.", "business_metric", "Success rate and revenue moved in opposite directions", "internal_finance", "payments", "us-east", "authorization success and captured revenue usually move together", "auth_success=99.9% revenue_delta=-74%", "critical", "anomaly"},
          {"anom_outlier_002", "Auth login volume at impossible hour", "Auth saw 48200 successful logins from dormant accounts between 03:00 and 03:05 local time, but normal traffic for that segment is under 140 logins per five minutes.", "security", "Dormant accounts logged in far above baseline", "internal_security", "auth", "eu-central", "dormant account traffic under 140 logins per five minutes", "48200 logins in five minutes", "critical", "anomaly"},
          {"anom_outlier_003", "Search latency normal but timeout complaints surge", "Search telemetry showed p95 latency of 132 ms, but support tickets mention timeouts, blank pages, and stalled results across mobile clients after a CDN rule change.", "user_experience", "Server latency looks healthy while users report failures", "support_and_observability", "search", "us-west", "low p95 latency should align with low timeout complaints", "p95=132ms complaints=surging mobile timeout reports", "high", "anomaly"},
          {"anom_outlier_004", "Billing refunds exceed completed purchases", "Billing completed 912 purchases but generated 1844 refunds in the same six-hour window. Refund count should not exceed completed purchases without a backlog event.", "business_metric", "Refund count is greater than purchase count", "internal_finance", "billing", "sa-south", "refunds remain below same-window purchases unless backlog replay is declared", "purchases=912 refunds=1844 backlog_event=false", "high", "anomaly"},
          {"anom_outlier_005", "Ingest queue drained while disk usage climbed", "Ingest reports the queue drained to zero, yet disk usage climbed from 61 percent to 96 percent and no compaction job was running.", "infrastructure", "Queue state and disk pressure conflict", "internal_observability", "ingest", "us-east", "empty ingest queue should reduce temporary disk pressure", "queue=0 disk=96% compaction=none", "high", "anomaly"},
          {"anom_outlier_006", "Brave result conflicts with local incident status", "Local status says the public API is fully operational, but a simulated Brave web result reports a same-hour outage notice and customer reports from multiple regions.", "external_signal", "External web evidence contradicts local status", "brave_crawl_simulated", "public_api", "global", "external reports should agree with local incident status", "local_status=operational external_reports=outage", "medium", "anomaly"},
          {"anom_outlier_007", "Release claims rollback while version advanced", "The release note says version 4.8.2 was rolled back, but production headers report 4.8.3 on 87 percent of requests five minutes later.", "deployment", "Rollback claim does not match observed version headers", "release_monitor", "gateway", "us-west", "rollback should reduce or remove newer version traffic", "rollback_claim=4.8.2 production_version=4.8.3 traffic_share=87%", "medium", "anomaly"},
          {"anom_outlier_008", "Inventory sold negative units", "The catalog shows minus 37 units sold for a product that also reports 22 fulfilled shipments. Sales cannot be negative when shipments are positive.", "data_quality", "Negative sales with positive shipments", "warehouse_sync", "catalog", "eu-central", "sold units are zero or positive and align with fulfilled shipments", "sold_units=-37 fulfilled_shipments=22", "high", "anomaly"},
          {"anom_outlier_009", "Cache hit rate perfect during origin outage", "Cache metrics show 100 percent hit rate while origin errors are 52 percent and cache misses are still being counted. These values cannot all be true.", "telemetry", "Cache and origin metrics are mutually inconsistent", "internal_observability", "cdn", "global", "perfect cache hit rate should not coexist with counted misses and origin error traffic", "cache_hit=100% origin_errors=52% misses=counted", "high", "anomaly"},
          {"anom_outlier_010", "Fraud model confidence inverted", "The fraud model approved every transaction with risk score above 0.98 and rejected low-risk purchases below 0.05 after a feature flag change.", "ml_monitoring", "Decision direction appears inverted", "model_monitor", "risk", "us-east", "high risk scores should face stricter review than low risk scores", "approved_high_risk=true rejected_low_risk=true", "critical", "anomaly"},
          {"anom_outlier_011", "Temperature sensor reports below physical site minimum", "A datacenter rack sensor reported -54 C while adjacent sensors stayed between 21 C and 24 C and no cooling alert fired.", "iot", "Single sensor value is physically implausible", "facility_telemetry", "datacenter", "sa-south", "rack temperature should remain near adjacent sensors unless a cooling incident is active", "rack_temp=-54C adjacent=21-24C cooling_alert=false", "medium", "anomaly"},
          {"anom_outlier_012", "API token used before it was created", "Audit logs show token T-481 used at 10:14:03, but the key creation event is recorded at 10:18:44 with the same issuer.", "security", "Usage timestamp predates creation timestamp", "audit_log", "identity", "global", "token usage must occur after token creation", "used_at=10:14:03 created_at=10:18:44", "critical", "anomaly"},
     };

     for (size_t i = 0; i < outliers.size(); ++i)
     {
          const auto &seed = outliers[i];
          nlohmann::json doc;
          doc["id"] = seed.Id;
          doc["document_id"] = seed.Id;
          doc["title"] = seed.Title;
          doc["content"] = seed.Content;
          doc["description"] = seed.Summary;
          doc["labels"] = "[\"anomaly\",\"outlier\",\"" + seed.Category + "\"]";
          doc["embedding"] = BuildFakeBenchmarkEmbedding("anomalies", seed.Service, 88U + i);
          doc["location"] = BuildFakeBenchmarkLocation("technology", 88U + i);
          doc["location_name"] = "Synthetic anomaly benchmark operations";
          doc["category"] = seed.Category;
          doc["summary"] = seed.Summary;
          doc["source"] = seed.Source;
          doc["service"] = seed.Service;
          doc["region"] = seed.Region;
          doc["expected_pattern"] = seed.ExpectedPattern;
          doc["observed_signal"] = seed.ObservedSignal;
          doc["severity"] = seed.Severity;
          doc["expected_label"] = seed.ExpectedLabel;
          doc["timestamp"] = static_cast<int64_t>(1777501000000LL + static_cast<int64_t>((i + 1U) * 60000U));
          docs.push_back(std::move(doc));
     }

     return docs;
}

static std::string BuildBenchmarkDescription(const std::string &collection,
                                             const std::string &tag,
                                             const std::string &content)
{
     std::string trimmed = TrimWhitespace(content);

     if (!trimmed.empty())
     {
          size_t sentence_end = trimmed.find('.');
          std::string summary = (sentence_end != std::string::npos) ? trimmed.substr(0, sentence_end + 1) : trimmed;

          summary = TrimWhitespace(summary);
          if (!summary.empty())
          {
               if (summary.size() > 180)
               {
                    summary = TrimWhitespace(summary.substr(0, 177));
                    while (!summary.empty() && std::isalnum(static_cast<unsigned char>(summary.back())))
                    {
                         summary.pop_back();
                    }
                    summary = TrimWhitespace(summary);
                    if (summary.empty())
                    {
                         summary = TrimWhitespace(trimmed.substr(0, 177));
                    }
                    summary += "...";
               }

               return summary;
          }
     }

     if (collection == "technology")
     {
          return "A working technology brief about " + tag + " with concrete architecture, delivery, and reliability detail.";
     }
     else if (collection == "travel")
     {
          return "A travel-oriented note about " + tag + " with planning tradeoffs, local context, and realistic pacing.";
     }
     else if (collection == "books")
     {
          return "A reader-facing overview of " + tag + " focused on structure, taste, and recommendation patterns.";
     }

     return "A realistic " + collection + " note about " + tag + " with concrete examples and less benchmark boilerplate.";
}

static std::vector<std::string> ExtractBenchmarkKeywords(const std::string &text)
{
     static const std::unordered_set<std::string> stopwords = {
          "a",          "an",        "and",       "are",       "as",         "at",        "artist",     "artists",
          "book",       "books",     "by",        "for",       "from",       "in",        "is",         "its",
          "like",       "modern",    "note",      "of",        "on",         "profile",   "spotlight",  "such",
          "that",       "the",       "their",     "through",   "to",         "topic",     "with",       "work",
          "works",      "album",     "albums",    "science",   "travel",     "music",     "movie",      "movies",
          "film",       "films",     "history",   "technology","food",       "sports",    "art",        "books"};

     std::vector<std::string> keywords;
     std::string current;

     auto flush_current = [&]()
     {
          if (current.empty())
          {
               return;
          }

          while (!current.empty() && current.front() == '-')
          {
               current.erase(current.begin());
          }

          while (!current.empty() && current.back() == '-')
          {
               current.pop_back();
          }

          if (current.size() >= 3 && stopwords.find(current) == stopwords.end())
          {
               keywords.push_back(current);
          }

          current.clear();
     };

     for (unsigned char ch : text)
     {
          if (std::isalnum(ch))
          {
               current.push_back(static_cast<char>(std::tolower(ch)));
          }
          else if ((ch == '-' || ch == '\'') && !current.empty())
          {
               current.push_back(ch == '\'' ? '-' : static_cast<char>(ch));
          }
          else
          {
               flush_current();
          }
     }

     flush_current();
     return keywords;
}

static std::vector<std::string> BuildBenchmarkLabels(const std::string &collection,
                                                     const std::string &tag,
                                                     const std::string &title,
                                                     const std::string &content)
{
     std::vector<std::string> labels;
     std::unordered_set<std::string> seen;

     auto add_label = [&](const std::string &label)
     {
          if (label.empty() || seen.find(label) != seen.end())
          {
               return;
          }

          seen.insert(label);
          labels.push_back(label);
     };

     add_label(collection);

     std::vector<std::string> title_keywords = ExtractBenchmarkKeywords(title);
     std::vector<std::string> content_keywords = ExtractBenchmarkKeywords(content);

     if (title.find("Kendrick Lamar") != std::string::npos)
     {
          add_label("kendrick-lamar");
     }
     if (title.find("The Beatles") != std::string::npos)
     {
          add_label("the-beatles");
     }

     for (const auto &keyword : title_keywords)
     {
          add_label(keyword);
          if (labels.size() >= 4)
          {
               break;
          }
     }

     for (const auto &keyword : content_keywords)
     {
          add_label(keyword);
          if (labels.size() >= 6)
          {
               break;
          }
     }

     if (labels.size() < 2)
     {
          add_label(tag);
     }

     return labels;
}

static const std::vector<FakeSynonymSeed> kFakeBenchmarkSynonymSeeds = {
     {"car", {"automobile", "vehicle", "auto", "motorcar"}},
     {"phone", {"mobile", "cellphone", "smartphone", "device"}},
     {"computer", {"pc", "laptop", "desktop", "machine"}},
     {"house", {"home", "residence", "dwelling", "abode"}},
     {"dog", {"puppy", "canine", "pet", "hound"}},
     {"cat", {"kitten", "feline", "pet", "kitty"}},
     {"book", {"novel", "tome", "volume", "publication"}},
     {"food", {"meal", "cuisine", "dish", "fare"}},
     {"water", {"liquid", "h2o", "aqua", "fluid"}},
     {"tree", {"plant", "sapling", "wood", "forest"}},
     {"music", {"song", "melody", "tune", "track"}},
     {"science", {"physics", "biology", "chemistry", "experiment"}},
     {"cake", {"pastry", "dessert", "sweet", "bakery"}}};

static const std::vector<FakeSynonymSeed> kFakeGlobalSynonymSeeds = {
     {"global", {"sitewide", "federated", "shared"}},
     {"demo", {"sample", "fixture", "synthetic"}},
     {"popular", {"trending", "featured", "recommended"}},
     {"guide", {"tutorial", "walkthrough", "reference"}},
     {"profile", {"record", "entry", "document"}},
};

static const std::vector<std::string> kFakeGlobalStopwords = {
     "benchmark",
     "collection",
     "document",
     "content",
     "inserted",
};

static const std::unordered_map<std::string, std::vector<FakeSynonymSeed>> kFakeCollectionSynonymProfiles = {
     {"art",
      {
           {"painting", {"canvas", "portrait", "mural", "study"}},
           {"gallery", {"exhibit", "show", "installation", "display"}},
           {"sculpture", {"carving", "figure", "form", "piece"}},
      }},
     {"books",
      {
           {"book", {"novel", "volume", "paperback", "title"}},
           {"author", {"writer", "novelist", "essayist", "editor"}},
           {"story", {"narrative", "plot", "chapter", "series"}},
      }},
     {"food",
      {
           {"food", {"dish", "meal", "plate", "course"}},
           {"recipe", {"prep", "method", "cook", "kitchen"}},
           {"dessert", {"pastry", "sweet", "bakery", "treat"}},
      }},
     {"history",
      {
           {"history", {"archive", "record", "chronicle", "timeline"}},
           {"empire", {"dynasty", "kingdom", "state", "realm"}},
           {"war", {"campaign", "battle", "conflict", "front"}},
      }},
     {"math",
      {
           {"algebra", {"equation", "variable", "expression", "identity"}},
           {"geometry", {"angle", "shape", "theorem", "proof"}},
           {"calculus", {"derivative", "integral", "limit", "function"}},
      }},
     {"movies",
      {
           {"movie", {"film", "feature", "picture", "release"}},
           {"director", {"filmmaker", "producer", "editor", "screenwriter"}},
           {"scene", {"sequence", "shot", "frame", "cut"}},
      }},
     {"people",
      {
           {"person", {"individual", "profile", "contact", "name"}},
           {"biography", {"bio", "profile", "background", "summary"}},
           {"occupation", {"profession", "career", "role", "work"}},
      }},
     {"music",
      {
           {"music", {"song", "melody", "track", "tune"}},
           {"artist", {"performer", "vocalist", "band", "act"}},
           {"album", {"record", "release", "playlist", "catalog"}},
      }},
     {"science",
      {
           {"science", {"research", "study", "analysis", "experiment"}},
           {"physics", {"matter", "energy", "quantum", "force"}},
           {"biology", {"genetics", "cells", "species", "organism"}},
      }},
     {"sports",
      {
           {"sports", {"athletics", "competition", "fixture", "game"}},
           {"team", {"squad", "lineup", "club", "roster"}},
           {"match", {"contest", "playoff", "tournament", "final"}},
      }},
     {"stocks",
      {
           {"ticker", {"symbol", "cashtag", "equity", "fund"}},
           {"stock", {"share", "equity", "exchange", "price"}},
           {"portfolio", {"allocation", "holding", "basket", "exposure"}},
      }},
     {"technology",
      {
           {"software", {"platform", "service", "application", "stack"}},
           {"ai", {"automation", "model", "inference", "assistant"}},
           {"security", {"auth", "hardening", "access", "network"}},
      }},
     {"travel",
      {
           {"travel", {"journey", "trip", "route", "tour"}},
           {"itinerary", {"schedule", "plan", "stop", "leg"}},
           {"destination", {"city", "stay", "excursion", "guide"}},
      }},
     {"universities",
      {
           {"university", {"campus", "college", "institution", "school"}},
           {"research", {"faculty", "program", "department", "lab"}},
           {"student", {"admission", "alumni", "degree", "cohort"}},
           {"city", {"location", "metro", "region", "area"}},
           {"cambridge", {"boston", "greater boston", "massachusetts", "kendall square"}},
      }},
};

static const std::unordered_map<std::string, std::vector<std::string>> kFakeCollectionStopwordProfiles = {
     {"art", {"art", "gallery", "studio", "canvas"}},
     {"books", {"book", "author", "reader", "chapter"}},
     {"food", {"food", "dish", "kitchen", "flavor"}},
     {"history", {"history", "era", "archive", "empire"}},
     {"math", {"math", "proof", "equation", "theorem"}},
     {"stocks", {"stock", "session", "portfolio", "watchlist"}},
     {"movies", {"film", "scene", "director", "cinema"}},
     {"people", {"person", "profile", "biography", "name"}},
     {"music", {"music", "song", "album", "artist"}},
     {"science", {"study", "lab", "data", "theory"}},
     {"sports", {"team", "match", "season", "league"}},
     {"technology", {"tech", "software", "system", "platform"}},
     {"travel", {"travel", "trip", "route", "guide"}},
     {"universities", {"campus", "student", "faculty", "research"}},
};

static const std::vector<FakeSynonymSeed> &GetFakeCollectionSynonyms(const std::string &collection_name)
{
     const auto it = kFakeCollectionSynonymProfiles.find(collection_name);
     if (it != kFakeCollectionSynonymProfiles.end())
     {
          return it->second;
     }

     return kFakeBenchmarkSynonymSeeds;
}

static std::vector<std::string> GetFakeCollectionStopwords(const std::string &collection_name)
{
     std::vector<std::string> result = {"the", "and"};

     const auto it = kFakeCollectionStopwordProfiles.find(collection_name);
     if (it != kFakeCollectionStopwordProfiles.end())
     {
          result.insert(result.end(), it->second.begin(), it->second.end());
     }

     return result;
}

static std::string BuildCollectionSynonymDocHint(const std::string &collection_name, int index)
{
     const std::vector<FakeSynonymSeed> &synonyms = GetFakeCollectionSynonyms(collection_name);
     if (synonyms.empty())
     {
          return "";
     }

     const FakeSynonymSeed &seed = synonyms[static_cast<size_t>(index) % synonyms.size()];
     std::string hint = " Related terms: " + seed.Root;

     const size_t synonym_limit = std::min<size_t>(3, seed.Synonyms.size());
     for (size_t i = 0; i < synonym_limit; ++i)
     {
          hint += ", " + seed.Synonyms[i];
     }

     hint += ".";
     return hint;
}

bool CreateFakeCollections(const std::string &base_url, const std::string &auth_token, bool reuse_collections, bool verbose)
{
     static const std::unordered_map<std::string, std::vector<RealDocSeed>> RealSeeds = {
          {"books",
           {
                {"Book Spotlight: The Great Gatsby",
                 "F. Scott Fitzgerald's 1925 novel follows Jay Gatsby and the American Dream in Jazz Age New York, with strong themes of class, illusion, and loss."},
                {"Book Spotlight: To Kill a Mockingbird",
                 "Harper Lee's 1960 novel explores justice and racism in the U.S. South through Scout Finch's perspective, centered on the trial of Tom Robinson."},
                {"Book Spotlight: 1984",
                 "George Orwell's dystopian classic presents surveillance, propaganda, and authoritarian control through Winston Smith's life in Oceania."},
                {"Book Spotlight: Pride and Prejudice",
                 "Jane Austen's novel examines marriage, social class, and personal growth through the evolving relationship between Elizabeth Bennet and Mr. Darcy."},
                {"Book Spotlight: Moby-Dick",
                 "Herman Melville's maritime epic combines adventure and philosophy as Captain Ahab obsessively hunts the white whale."},
                {"Book Spotlight: One Hundred Years of Solitude",
                 "Gabriel Garcia Marquez's landmark of magical realism traces the Buendia family across generations in the town of Macondo."},
                {"Book Spotlight: The Lord of the Rings",
                 "J.R.R. Tolkien's fantasy trilogy chronicles the quest to destroy the One Ring and explores friendship, sacrifice, and power."},
                {"Book Spotlight: Beloved",
                 "Toni Morrison's novel addresses slavery, memory, and trauma through Sethe's life after escaping bondage."},
                {"Book Spotlight: The Brothers Karamazov",
                 "Fyodor Dostoevsky's novel explores morality, faith, and free will through conflicts among three brothers and their father."},
                {"Book Spotlight: The Catcher in the Rye",
                 "J.D. Salinger's novel follows Holden Caulfield's disillusionment, voice, and search for authenticity in postwar America."},
           }},
          {"music",
           {
                {"Artist Profile: The Beatles",
                 "The Beatles shaped modern pop and rock through influential songwriting, studio experimentation, and landmark albums such as Revolver and Abbey Road."},
                {"Artist Profile: Queen",
                 "Queen blended rock, opera, and theatrical performance, with Freddie Mercury's vocals and songs like Bohemian Rhapsody defining their legacy."},
                {"Artist Profile: Michael Jackson",
                 "Michael Jackson, widely known as the King of Pop, advanced global pop performance, production, and music video storytelling, especially across albums like Thriller and Bad."},
                {"Artist Profile: Miles Davis",
                 "Miles Davis drove multiple jazz eras, from cool jazz to modal and fusion, with key works including Kind of Blue and Bitches Brew."},
                {"Artist Profile: Bob Dylan",
                 "Bob Dylan transformed lyric-driven songwriting in folk and rock, with lasting influence from albums like Highway 61 Revisited."},
                {"Artist Profile: Nirvana",
                 "Nirvana brought grunge into mainstream rock in the early 1990s, led by Kurt Cobain and the album Nevermind."},
                {"Artist Profile: Madonna",
                 "Madonna, widely known as the Queen of Pop, combined pop reinvention, dance production, and visual identity across decades, shaping modern mainstream music culture."},
                {"Artist Profile: Radiohead",
                 "Radiohead moved from alternative rock into experimental electronic textures, especially through OK Computer and Kid A."},
                {"Artist Profile: Beyonce",
                 "Beyonce, often called Queen Bey, blends R&B, pop, and visual storytelling, with major cultural impact through performance and concept albums."},
                {"Artist Profile: Kendrick Lamar",
                 "Kendrick Lamar is recognized for narrative lyricism, social commentary, and modern hip-hop production on albums like To Pimp a Butterfly."},
           }},
          {"science",
           {
                {"Science Topic: Relativity",
                 "Einstein's special and general relativity explain spacetime, gravity, and high-speed motion, replacing Newtonian limits in extreme conditions."},
                {"Science Topic: Quantum Mechanics",
                 "Quantum theory describes matter and energy at atomic scales, including wave-particle duality, uncertainty, and probabilistic states."},
                {"Science Topic: DNA and Genetics",
                 "DNA stores hereditary information, while genes and mutation drive inheritance, variation, and many modern biotechnology applications."},
                {"Science Topic: Plate Tectonics",
                 "Plate tectonics explains earthquakes, volcanism, and mountain building through movement of Earth's lithospheric plates."},
                {"Science Topic: Evolution by Natural Selection",
                 "Darwinian evolution describes how populations change over time through selection pressure, adaptation, and common ancestry."},
                {"Science Topic: The Periodic Table",
                 "The periodic table organizes chemical elements by atomic number and recurring properties, guiding predictions in chemistry."},
                {"Science Topic: CRISPR Gene Editing",
                 "CRISPR-Cas systems enable targeted DNA editing and are widely studied for medicine, agriculture, and functional genomics."},
                {"Science Topic: Climate Science",
                 "Climate research measures long-term atmospheric and ocean changes, greenhouse forcing, and regional impacts on ecosystems and society."},
                {"Science Topic: Black Holes",
                 "Black holes are regions of intense gravity predicted by relativity, observed indirectly through radiation, lensing, and mergers."},
                {"Science Topic: Vaccines and Immunology",
                 "Vaccines train immune memory against pathogens, reducing severe disease and enabling broad public health prevention."},
           }},
          {"universities",
           {
                {"University of California Berkeley",
                 "California flagship public research university in Berkeley known for engineering, computer science, economics, public policy, and a startup-heavy Bay Area environment."},
                {"University of Michigan Ann Arbor",
                 "Michigan public university recognized for engineering, medicine, business, and large-scale research activity with strong alumni reach across industry and public leadership."},
                {"The Ohio State University",
                 "Ohio public research university in Columbus with major programs in medicine, business, engineering, agriculture, and a broad campus footprint tied to statewide impact."},
                {"University of Texas at Austin",
                 "Texas flagship public university with strength in engineering, business, computer science, energy research, and policy programs connected to a fast-growing state economy."},
                {"University of Washington Seattle",
                 "Washington public research university noted for medicine, computer science, public health, and close ties to regional technology and life-science employers."},
                {"University of Florida",
                 "Florida public flagship in Gainesville with nationally visible programs in engineering, business, agriculture, health, and a large in-state student base."},
                {"University of Illinois Urbana Champaign",
                 "Illinois public research university with standout engineering, computing, physics, and data-intensive research culture tied to both academia and industry."},
                {"Georgia Institute of Technology",
                 "Georgia technology-focused public university in Atlanta known for engineering, computing, design, analytics, and cooperative ties with startups and major employers."},
                {"Pennsylvania State University",
                 "Pennsylvania public university system led by the University Park campus, with strengths in engineering, business, agriculture, materials, and statewide extension work."},
                {"University of Massachusetts Amherst",
                 "Massachusetts public flagship with strong computer science, public health, engineering, sustainability, and social-science research in the New England region."},
           }},
          {"stocks",
           {
                {"S&P 500",
                 "Synthetic demo market note for $SPY (S&P 500): desk notes describe a steady index-tracking session where breadth, sector rotation, and risk appetite matter more than any single headline."},
                {"Nasdaq-100",
                 "Synthetic demo market note for $QQQ (Nasdaq-100): the watchlist focuses on growth-stock momentum, software demand, chip supply, and whether buyers keep supporting large-cap technology themes."},
                {"Dow Jones Industrial Average",
                 "Synthetic demo market note for $DIA (Dow Jones Industrial Average): market observers frame the session around blue-chip durability, dividend sensitivity, and defensive demand after a mixed macro morning."},
                {"Russell 2000",
                 "Synthetic demo market note for $IWM (Russell 2000): the small-cap basket draws attention from traders watching credit conditions, local-bank tone, and domestic cyclical participation."},
                {"20+ Year U.S. Treasury Bonds",
                 "Synthetic demo rates note for $TLT (20+ Year U.S. Treasury Bonds): bond-market commentary highlights duration exposure, rate expectations, and portfolio hedging as yields move through a narrow range."},
                {"Gold",
                 "Synthetic demo metals note for $GLD (Gold): the metals note tracks inflation hedging, real-yield pressure, and cautious allocation choices during a quieter risk session."},
                {"Crude Oil",
                 "Synthetic demo energy note for $USO (Crude Oil): the energy desk follows crude-linked sentiment, supply commentary, refinery margins, and the way commodity moves affect inflation narratives."},
                {"Apple",
                 "Synthetic demo company note for $AAPL (Apple): studies device demand, services mix, margin discipline, and index weight without making a live investment call."},
                {"Microsoft",
                 "Synthetic demo company note for $MSFT (Microsoft): centers on cloud backlog, enterprise software renewals, AI infrastructure cost, and platform spending discipline."},
                {"NVIDIA",
                 "Synthetic demo company note for $NVDA (NVIDIA): follows accelerator demand, data-center orders, supply-chain capacity, and growth-stock positioning."},
           }},
     };

     std::vector<FakeCollectionSpec> specs = {
          {"music", {"album", "artist", "live", "studio", "playlist", "symphony", "jazz", "rock", "pop", "indie"}},
          {"movies", {"film", "cinema", "director", "cast", "thriller", "drama", "comedy", "action", "classic", "sequel"}},
          {"people", {"person", "profile", "biography", "name", "occupation", "career", "contact", "fictional", "community", "background"}},
          {"art", {"painting", "sculpture", "gallery", "modern", "abstract", "portrait", "canvas", "exhibit", "mural", "installation"}},
          {"books", {"novel", "fiction", "nonfiction", "author", "series", "paperback", "hardcover", "fantasy", "mystery", "classic"}},
          {"travel", {"destination", "itinerary", "guide", "adventure", "beach", "mountain", "city", "budget", "luxury", "culture"}},
          {"food", {"recipe", "cuisine", "flavor", "restaurant", "dessert", "spice", "vegan", "grill", "street", "seasonal"}},
          {"sports", {"team", "match", "league", "championship", "player", "coach", "tournament", "season", "stadium", "score"}},
          {"science", {"research", "experiment", "physics", "biology", "chemistry", "astronomy", "lab", "discovery", "theory", "data"}},
          {"history", {"era", "archive", "ancient", "modern", "war", "empire", "documentary", "timeline", "heritage", "biography"}},
          {"technology", {"software", "hardware", "ai", "network", "security", "startup", "gadget", "cloud", "robotics", "mobile"}},
          {"math", {"algebra", "geometry", "calculus", "probability", "prime", "matrix", "vector", "theorem", "equation", "integral"}},
          {"stocks", {"spy", "qqq", "dia", "iwm", "tlt", "gld", "uso", "aapl", "msft", "nvda"}},
          {"universities", {"california", "michigan", "ohio", "texas", "washington", "florida", "illinois", "georgia", "pennsylvania", "massachusetts"}},
          {"anomalies", {"telemetry", "security", "business", "external", "brave", "rollback", "fraud", "audit", "outlier", "baseline"}}};

     BenchmarkClient client(base_url, auth_token, reuse_collections);
     std::vector<std::string> inserted_fake_collections;

     std::string conn_error = client.TestConnection();
     if (!conn_error.empty())
     {
          std::cerr << "✗ Cannot connect to server for fake collections: " << conn_error << ".\n";
          return false;
     }

     for (const auto &spec : specs)
     {
          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          const std::string collection_name = spec.Name;

          if (verbose)
          {
               LogOutput("Creating fake collection '" + collection_name + "'...\n");
          }

          bool collection_created = false;
          if (spec.Name == "food")
          {
               nlohmann::json food_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(food_fields);
               food_fields.push_back({{"name", "ingredients"}, {"type", "string"}});
               food_fields.push_back({{"name", "cuisine"}, {"type", "string"}});
               food_fields.push_back({{"name", "dish"}, {"type", "string"}});
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, food_fields, "");
          }
          else if (spec.Name == "universities")
          {
               nlohmann::json university_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(university_fields);
               university_fields.push_back({{"name", "state"}, {"type", "string"}});
               university_fields.push_back({{"name", "city"}, {"type", "string"}});
               university_fields.push_back({{"name", "city_aliases"}, {"type", "string"}});
               university_fields.push_back({{"name", "location_labels"}, {"type", "string"}});
               university_fields.push_back({{"name", "search_aliases"}, {"type", "string"}});
               university_fields.push_back({{"name", "institution_type"}, {"type", "string"}});
               university_fields.push_back({{"name", "rank"}, {"type", "int32"}});
               university_fields.push_back({{"name", "rank_signal"}, {"type", "float"}});
               university_fields.push_back({{"name", "rank_source"}, {"type", "string"}});
               university_fields.push_back({{"name", "rank_scope"}, {"type", "string"}});
               university_fields.push_back({{"name", "rank_edition"}, {"type", "string"}});
               university_fields.push_back({{"name", "webometrics_country_rank"}, {"type", "int32"}});
               university_fields.push_back({{"name", "webometrics_world_rank"}, {"type", "int32"}});
               university_fields.push_back({{"name", "webometrics_impact_rank"}, {"type", "int32"}});
               university_fields.push_back({{"name", "webometrics_openness_rank"}, {"type", "int32"}});
               university_fields.push_back({{"name", "webometrics_excellence_rank"}, {"type", "int32"}});

               nlohmann::json university_metadata = {
                    {"_default_sorting_field", "webometrics_world_rank"},
                    {"_default_sorting_order", "asc"},
                    {"_rank_field", "webometrics_world_rank"},
                    {"_rank_order", "asc"},
                    {"_rank_source", "webometrics_world_rank"},
                    {"_rank_scope", "benchmark"},
                    {"_rank_edition", "January 2026"},
                    {"_rank_algorithm", "linear_algebra"},
                    {"_rank_alpha", "0.85"},
                    {"_rank_beta", "4.0"},
                    {"_rank_weight", "0.75"},
                    {"_rank_tiebreak", "rank_signal_desc_webometrics_world_rank_asc"},
                    {"_rank_methodology", "deterministic_benchmark_webometrics_world_rank_1_to_100"}};

               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, university_fields, "webometrics_world_rank", university_metadata);
          }
          else if (spec.Name == "people")
          {
               nlohmann::json people_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(people_fields);
               people_fields.push_back({{"name", "first_name"}, {"type", "string"}});
               people_fields.push_back({{"name", "middle_name"}, {"type", "string"}});
               people_fields.push_back({{"name", "last_name"}, {"type", "string"}});
               people_fields.push_back({{"name", "full_name"}, {"type", "string"}});
               people_fields.push_back({{"name", "biography"}, {"type", "string"}});
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, people_fields, "");
          }
          else if (spec.Name == "math")
          {
               nlohmann::json math_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(math_fields);
               math_fields.push_back({{"name", "topic"}, {"type", "string"}});
               math_fields.push_back({{"name", "value"}, {"type", "float"}});
               math_fields.push_back({{"name", "value_b"}, {"type", "float"}});
               math_fields.push_back({{"name", "value_c"}, {"type", "float"}});
               math_fields.push_back({{"name", "equation_index"}, {"type", "int32"}});
               math_fields.push_back({{"name", "prime_candidate"}, {"type", "int32"}});
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, math_fields, "value");
          }
          else if (spec.Name == "stocks")
          {
               nlohmann::json stock_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(stock_fields);
               stock_fields.push_back({{"name", "ticker"}, {"type", "string"}});
               stock_fields.push_back({{"name", "cashtag"}, {"type", "string"}});
               stock_fields.push_back({{"name", "asset_class"}, {"type", "string"}});
               stock_fields.push_back({{"name", "watchlist"}, {"type", "string"}});
               stock_fields.push_back({{"name", "source"}, {"type", "string"}});
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, stock_fields, "");
          }
          else if (spec.Name == "anomalies")
          {
               nlohmann::json anomaly_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(anomaly_fields);
               anomaly_fields.push_back({{"name", "category"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "summary"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "source"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "service"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "region"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "expected_pattern"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "observed_signal"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "severity"}, {"type", "string"}});
               anomaly_fields.push_back({{"name", "expected_label"}, {"type", "string"}});
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, anomaly_fields, "");
          }
          else
          {
               nlohmann::json fake_fields = nlohmann::json::array();
               AddFakeBenchmarkSearchFields(fake_fields);
               collection_created = client.CreateCollectionWithSchemaLocal(collection_name, fake_fields, "");
          }

          if (!collection_created)
          {
               std::cerr << "✗ Failed to create fake collection '" << collection_name << "'.\n";
               continue;
          }

          if (verbose)
          {
               LogOutput("  ↳ Collection '" + collection_name + "' is ready; importing fake documents...\n");
          }

          const size_t docs_to_create = spec.Name == "universities" ? GetUniversityBenchmarkSeeds().size() : ((spec.Name == "people" || spec.Name == "anomalies") ? 100U : 10U);
          std::vector<std::tuple<std::string, std::string, std::string>> docs;
          docs.reserve(docs_to_create);
          std::vector<nlohmann::json> enriched_docs;
          enriched_docs.reserve(docs_to_create);
          std::unordered_set<std::string> used_ids;

          auto BuildRealisticTitle = [&](const std::string &collection, const std::string &tag, int index) -> std::string
          {
               const auto Pick = [&](const std::vector<std::string> &values, size_t offset = 0U) -> const std::string &
               {
                    return values[(static_cast<size_t>(index) + offset) % values.size()];
               };

               const std::vector<std::string> generic_patterns = {
                    Capitalize(tag) + " in " + Capitalize(collection),
                    "Inside " + Capitalize(tag) + " for " + Capitalize(collection),
                    Capitalize(collection) + ": " + Capitalize(tag) + " Playbook",
                    "Why " + tag + " matters in " + collection,
                    Capitalize(tag) + " patterns and practice",
                    "Working notes on " + tag,
                    Capitalize(collection) + " systems around " + tag,
                    "Understanding " + tag + " in context"};

               if (collection == "technology")
               {
                    const std::vector<std::string> patterns = {
                         Capitalize(tag) + " systems in production",
                         "Building with " + tag + " under real constraints",
                         "Inside " + tag + " platforms",
                         Capitalize(tag) + " operations and architecture",
                         "Shipping products with " + tag,
                         Capitalize(tag) + " reliability patterns",
                         "Practical " + tag + " engineering",
                         "How teams use " + tag};
                    return Pick(patterns);
               }

               if (collection == "travel")
               {
                    const std::vector<std::string> patterns = {
                         Capitalize(tag) + " routes worth planning",
                         "A traveler guide to " + tag,
                         Capitalize(tag) + " days on the road",
                         "How to plan around " + tag,
                         Capitalize(tag) + " stays and local rhythm",
                         "Moving through " + tag + " with less friction",
                         Capitalize(tag) + " decisions that change the trip",
                         "Seeing " + tag + " beyond the checklist"};
                    return Pick(patterns);
               }

               if (collection == "books")
               {
                    const std::vector<std::string> patterns = {
                         Capitalize(tag) + " books that hold up",
                         "Reading through " + tag,
                         Capitalize(tag) + " stories and structure",
                         "What readers notice in " + tag,
                         Capitalize(tag) + " shelves and recommendations",
                         "The pull of " + tag + " in books",
                         Capitalize(tag) + " titles worth revisiting",
                         "How " + tag + " shapes the reading experience"};
                    return Pick(patterns);
               }

               return Pick(generic_patterns);
          };

          auto BuildRealisticContent = [&](const std::string &collection, const std::string &tag, int index) -> std::string
          {
               const auto Pick = [&](const std::vector<std::string> &values, size_t offset = 0U) -> const std::string &
               {
                    return values[(static_cast<size_t>(index) + offset) % values.size()];
               };

               const std::vector<std::string> generic_intros = {
                    Capitalize(collection) + " coverage centered on " + tag + ", using concrete examples instead of filler text.",
                    "A practical " + collection + " brief on " + tag + " that highlights decisions, tradeoffs, and field context.",
                    "This " + collection + " entry examines " + tag + " through real scenarios, working notes, and observed patterns.",
                    "An in-depth " + collection + " write-up about " + tag + " with examples drawn from day-to-day use and review."};

               if (collection == "art")
               {
                    static const std::vector<std::string> intros = {
                         "A studio journal on " + tag + " that follows how an artist moves from sketches to a finished piece.",
                         "This art note studies " + tag + " inside workshops, galleries, and exhibition prep.",
                         "An art feature focused on " + tag + " with examples from curators, painters, and installation teams.",
                         "A practical review of " + tag + " in art, from first concept boards to final hanging decisions."};
                    static const std::vector<std::string> details = {
                         "It covers composition balance, surface preparation, framing choices, and how viewers read the work across a room.",
                         "The piece follows palette experiments, material handling, and the curation choices that shape a gallery wall.",
                         "It tracks critique sessions, lighting adjustments, and the difference between studio intent and exhibition presentation.",
                         "Notes include restoration concerns, display logistics, and why " + tag + " changes meaning in public installations."};
                    static const std::vector<std::string> closers = {
                         "Examples reference museum labels, collector feedback, and seasonal show planning.",
                         "The closing section compares abstract studies, figurative drafts, and contemporary exhibit pacing.",
                         "Additional commentary looks at catalog writing, audience flow, and conservation tradeoffs.",
                         "It finishes with observations on commission work, transport risks, and curatorial sequencing."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "books")
               {
                    static const std::vector<std::string> intros = {
                         "This books entry looks at " + tag + " through reader expectations, shelf positioning, and review culture.",
                         "A reading note on " + tag + " that compares how editors, critics, and book clubs describe the same work.",
                         "This literary brief centers on " + tag + " with attention to pacing, structure, and market fit.",
                         "A books-focused analysis of " + tag + " built around annotations, chapter rhythm, and reader payoff."};
                    static const std::vector<std::string> details = {
                         "It compares author voice, scene construction, and the small structural decisions that keep chapters moving.",
                         "The write-up discusses character arcs, point of view, and how covers and blurbs frame expectations before page one.",
                         "It reviews plot turns, prose density, and the gap between literary praise and general reader enthusiasm.",
                         "Notes include backlist performance, translation interest, and why " + tag + " attracts different recommendation patterns."};
                    static const std::vector<std::string> closers = {
                         "Examples mention debut releases, classroom adoption, and prize-season visibility.",
                         "The final section compares paperback discovery, subscription picks, and critic roundups.",
                         "It closes with observations on adaptation potential, reread value, and niche audience loyalty.",
                         "Additional notes cover indie bookstore placement, library holds, and discussion-group appeal."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "food")
               {
                    static const std::vector<std::string> intros = {
                         "A kitchen brief on " + tag + " that follows prep, timing, and plating decisions.",
                         "This food note focuses on " + tag + " from the perspective of cooks, diners, and menu writers.",
                         "A service-and-recipe review of " + tag + " using restaurant and home-cooking examples.",
                         "This food document looks at " + tag + " through ingredient handling, texture goals, and table response."};
                    static const std::vector<std::string> details = {
                         "It covers seasoning strategy, heat control, and the pairings that make the dish feel complete rather than busy.",
                         "The piece compares pantry substitutions, station workflow, and why the same plate lands differently across cuisines.",
                         "It tracks prep shortcuts, sauce consistency, and the service details that change perceived quality.",
                         "Notes include portion balance, menu placement, and how " + tag + " shifts between weekday comfort and special-occasion cooking."};
                    static const std::vector<std::string> closers = {
                         "Examples mention tasting menus, street-food versions, and nutrition-conscious rewrites.",
                         "It finishes with observations on seasonal produce, beverage pairings, and guest feedback.",
                         "Additional commentary covers family-style service, takeout durability, and price sensitivity.",
                         "The closing section compares restaurant polish, home practicality, and regional variation."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "history")
               {
                    static const std::vector<std::string> intros = {
                         "A historical note on " + tag + " that moves between archival evidence and later interpretation.",
                         "This history brief examines " + tag + " through timelines, letters, and contested public memory.",
                         "An overview of " + tag + " in history, built from primary sources and modern reassessment.",
                         "This history entry focuses on " + tag + " with attention to institutions, turning points, and source reliability."};
                    static const std::vector<std::string> details = {
                         "It compares official records, private accounts, and the way historians handle missing or biased evidence.",
                         "The article tracks political context, regional impact, and how a single event can mean different things across generations.",
                         "It discusses archives, classroom narratives, and the tension between national myth and documented fact.",
                         "Notes cover monuments, public debates, and why " + tag + " is often revisited during anniversaries and reforms."};
                    static const std::vector<std::string> closers = {
                         "Examples span ancient cases, industrial transitions, and modern cultural preservation efforts.",
                         "The closing section highlights social movements, local memory projects, and museum framing.",
                         "Additional notes compare textbook summaries, specialist research, and oral-history preservation.",
                         "It finishes with observations on heritage policy, translation gaps, and historical revision."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "movies")
               {
                    static const std::vector<std::string> intros = {
                         "A film note on " + tag + " that connects script intent, on-screen execution, and audience reaction.",
                         "This movie brief focuses on " + tag + " through editing rhythm, scene payoff, and performance choices.",
                         "An industry-facing review of " + tag + " covering both craft decisions and release strategy.",
                         "This cinema entry looks at " + tag + " from the angle of directors, critics, and opening-week viewers."};
                    static const std::vector<std::string> details = {
                         "It discusses blocking, cinematography, and the moments where screenplay structure either supports or undercuts emotion.",
                         "The write-up compares casting chemistry, trailer expectations, and how tone shifts affect reception.",
                         "It tracks franchise continuity, standout sequences, and the tradeoff between spectacle and character focus.",
                         "Notes include release windows, streaming spillover, and why " + tag + " often drives post-release debate."};
                    static const std::vector<std::string> closers = {
                         "Examples mention festival buzz, box office staying power, and critic-audience splits.",
                         "It closes with observations on sequel setup, soundtrack support, and memorable visual motifs.",
                         "Additional commentary covers awards visibility, rewatch value, and long-tail fandom.",
                         "The final section compares opening-night hype, word of mouth, and catalog longevity."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "music")
               {
                    static const std::vector<std::string> intros = {
                         "A music note on " + tag + " that moves between recording choices and live response.",
                         "This music brief studies " + tag + " through arrangement, performance feel, and release context.",
                         "An audio-focused review of " + tag + " built around hooks, dynamics, and audience replay behavior.",
                         "This music entry centers on " + tag + " with examples from studio sessions, tours, and playlist circulation."};
                    static const std::vector<std::string> details = {
                         "It examines groove, harmony, and how production polish changes the emotional weight of the same melody.",
                         "The write-up compares vocal texture, sequencing, and the difference between headphone detail and venue impact.",
                         "It tracks collaboration choices, crossover appeal, and why some arrangements reward repeated listening.",
                         "Notes include mixing decisions, crowd response, and how " + tag + " shapes identity across albums and singles."};
                    static const std::vector<std::string> closers = {
                         "Examples reference live sets, deluxe editions, and streaming-era discovery patterns.",
                         "The closing section compares radio friendliness, deep-cut loyalty, and remix potential.",
                         "Additional notes cover touring stamina, fan communities, and catalog cohesion.",
                         "It finishes with observations on playlist placement, session musicians, and genre blending."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "science")
               {
                    static const std::vector<std::string> intros = {
                         "A science brief on " + tag + " that emphasizes method, evidence quality, and unresolved questions.",
                         "This science entry looks at " + tag + " through experiment design, interpretation, and follow-up work.",
                         "An applied science note on " + tag + " with examples from lab practice, fieldwork, and replication studies.",
                         "This research summary focuses on " + tag + " and the assumptions behind the reported results."};
                    static const std::vector<std::string> details = {
                         "It discusses controls, measurement error, and how conclusions change when datasets are expanded or reanalyzed.",
                         "The piece compares lab methods, peer review feedback, and the gap between promising findings and established consensus.",
                         "It tracks instrument limits, reproducibility concerns, and where interpretation outruns the available evidence.",
                         "Notes include preprints, cross-discipline relevance, and why " + tag + " remains active in current research discussions."};
                    static const std::vector<std::string> closers = {
                         "Examples mention statistical power, protocol revisions, and practical downstream applications.",
                         "The final section compares exploratory work, validated results, and public communication risks.",
                         "Additional commentary covers datasets, negative findings, and funding-driven priorities.",
                         "It closes with observations on replication, peer critique, and next-step experiments."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "sports")
               {
                    static const std::vector<std::string> intros = {
                         "A sports note on " + tag + " that follows preparation, execution, and post-match review.",
                         "This sports brief studies " + tag + " through tactics boards, player form, and schedule pressure.",
                         "An analysis of " + tag + " in sports, focused on coaching choices and game-state decision making.",
                         "This sports entry looks at " + tag + " with examples from training sessions, competition footage, and league context."};
                    static const std::vector<std::string> details = {
                         "It breaks down shape, tempo control, and the personnel choices that change a match before the scoreboard does.",
                         "The article compares scouting notes, recovery windows, and why momentum often masks structural weaknesses.",
                         "It tracks substitutions, practice loads, and the moments where disciplined execution beats raw talent.",
                         "Notes include standings pressure, injury management, and how " + tag + " becomes more visible in tight fixtures."};
                    static const std::vector<std::string> closers = {
                         "Examples mention playoff pacing, rivalry games, and tournament adaptation.",
                         "The final section compares fan expectations, analyst ratings, and coach postgame framing.",
                         "Additional commentary covers travel fatigue, bench depth, and late-season adjustments.",
                         "It finishes with observations on youth development, veteran leadership, and competitive margins."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "technology")
               {
                    static const std::vector<std::string> intros = {
                         "A technology note on " + tag + " that connects architecture decisions to operational outcomes.",
                         "This technology brief focuses on " + tag + " using examples from shipping teams and production incidents.",
                         "An engineering-facing review of " + tag + " centered on maintainability, scale, and developer workflow.",
                         "This technology entry examines " + tag + " through system design, rollout strategy, and reliability concerns."};
                    static const std::vector<std::string> details = {
                         "It covers API boundaries, deployment habits, and the subtle tradeoffs between speed of delivery and long-term clarity.",
                         "The write-up discusses observability, auth design, and why tooling choices shape developer behavior as much as runtime performance.",
                         "It tracks migration risk, cloud cost pressure, and the places where automation helps or quietly adds fragility.",
                         "Notes include incident response, schema evolution, and how " + tag + " affects both product velocity and system safety."};
                    static const std::vector<std::string> closers = {
                         "Examples mention AI-assisted coding, release gating, and cross-team ownership.",
                         "The final section compares startup pragmatism, enterprise constraints, and platform maturity.",
                         "Additional commentary covers reliability budgets, local tooling, and rollout sequencing.",
                         "It closes with observations on security hardening, developer ergonomics, and support load."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }
               if (collection == "travel")
               {
                    static const std::vector<std::string> intros = {
                         "A travel note on " + tag + " that follows planning, arrival, and day-by-day pacing.",
                         "This travel brief studies " + tag + " through route decisions, local habits, and budget tradeoffs.",
                         "A destination-focused review of " + tag + " with examples from recent itineraries and traveler reports.",
                         "This travel entry looks at " + tag + " from the perspective of transport, timing, and on-the-ground experience."};
                    static const std::vector<std::string> details = {
                         "It covers transit choices, neighborhood fit, and how a realistic schedule changes the quality of the trip.",
                         "The piece compares hotel convenience, seasonal crowd levels, and the small logistics that save hours later.",
                         "It tracks food stops, walking routes, and the difference between checklist travel and a pace that leaves room for discovery.",
                         "Notes include local etiquette, packing strategy, and why " + tag + " can feel completely different across seasons."};
                    static const std::vector<std::string> closers = {
                         "Examples mention rail connections, weather windows, and budget-conscious alternatives.",
                         "The closing section compares weekend plans, longer stays, and remote-work viability.",
                         "Additional commentary covers family travel, solo safety, and reservation timing.",
                         "It finishes with observations on cultural events, scenic detours, and fatigue management."};
                    return Pick(intros) + " " + Pick(details, 1U) + " " + Pick(closers, 2U);
               }

               return Pick(generic_intros) + " Document index: " + std::to_string(index + 1) + ".";
          };

          if (spec.Name == "anomalies")
          {
               enriched_docs = BuildAnomalyBenchmarkDocuments();
               for (const auto &doc : enriched_docs)
               {
                    docs.emplace_back(doc.value<std::string>("id", ""),
                                      doc.value<std::string>("title", ""),
                                      doc.value<std::string>("content", ""));
               }
          }
          else
          {
               for (size_t i = 0; i < docs_to_create; i++)
               {
                    const std::string &tag = spec.Tags[i % spec.Tags.size()];
                    std::string title;
                    std::string content;
                    auto SeedIt = RealSeeds.find(spec.Name);
                    if (spec.Name == "universities" && i < GetUniversityBenchmarkSeeds().size())
                    {
                         const UniversityBenchmarkSeed &university_seed = GetUniversityBenchmarkSeeds()[i];
                         title = university_seed.Name;
                         content = BuildUniversityBenchmarkContent(university_seed, i);
                    }
                    else if (spec.Name == "people")
                    {
                         PersonBenchmarkSeed person = BuildPersonBenchmarkSeed(i);
                         title = person.FirstName + " " + person.MiddleName + " " + person.LastName;
                         content = person.Biography;
                    }
                    else if (SeedIt != RealSeeds.end() && i < SeedIt->second.size())
                    {
                         title = SeedIt->second[i].Title;
                         content = SeedIt->second[i].Content;
                    }
                    else
                    {
                         title = BuildRealisticTitle(spec.Name, tag, static_cast<int>(i));
                         content = BuildRealisticContent(spec.Name, tag, static_cast<int>(i));
                    }

                    if (spec.Name != "universities")
                    {
                         content += BuildCollectionSynonymDocHint(spec.Name, static_cast<int>(i));
                    }

                    std::string doc_id = MakeMeaningfulDocId(collection_name, title, content, static_cast<int>(i), used_ids);
                    std::string safe_title = RemoveCommas(title);
                    std::string safe_content = RemoveCommas(content);
                    std::string description = BuildBenchmarkDescription(spec.Name, tag, content);
                    std::string safe_description = RemoveCommas(description);
                    nlohmann::json label_list = nlohmann::json::array();
                    for (const auto &label : BuildBenchmarkLabels(spec.Name, tag, title, content))
                    {
                         label_list.push_back(label);
                    }

                    docs.emplace_back(doc_id, safe_title, safe_content);

                    nlohmann::json enriched_doc;
                    enriched_doc["id"] = doc_id;
                    enriched_doc["document_id"] = doc_id;
                    enriched_doc["title"] = safe_title;
                    enriched_doc["content"] = safe_content;
                    enriched_doc["description"] = safe_description;
                    enriched_doc["labels"] = label_list.dump();
                    enriched_doc["embedding"] = BuildFakeBenchmarkEmbedding(spec.Name, tag, i);
                    enriched_doc["location"] = BuildFakeBenchmarkLocation(spec.Name, i);
                    enriched_doc["location_name"] = BuildFakeBenchmarkLocationName(spec.Name);
                    enriched_docs.push_back(std::move(enriched_doc));
               }
          }

          size_t inserted = 0;
          for (const auto &doc : enriched_docs)
          {
               if (g_benchmark_should_stop.load())
               {
                    return false;
               }

               if (client.UpsertDocumentWithFieldsLocal(collection_name, doc))
               {
                    inserted++;
               }
          }

          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          size_t enriched_updated = inserted;

          if (spec.Name == "food")
          {
               static const std::vector<std::vector<std::string>> ingredient_profiles = {
                    {"extra virgin olive oil", "sea salt", "garlic", "fresh basil", "grated parmesan"},
                    {"soy sauce", "sesame oil", "ginger", "scallions", "rice vinegar"},
                    {"lime juice", "cilantro", "chili flakes", "red onion", "cumin"},
                    {"butter", "heavy cream", "black pepper", "thyme", "shallots"},
                    {"tomato paste", "smoked paprika", "oregano", "coriander", "bay leaf"},
                    {"coconut milk", "turmeric", "curry powder", "garam masala", "fresh ginger"}};

               size_t food_updated = 0;
               for (size_t i = 0; i < docs.size(); ++i)
               {
                    const auto &doc_tuple = docs[i];
                    const auto &profile = ingredient_profiles[i % ingredient_profiles.size()];

                    std::string ingredients;
                    for (size_t j = 0; j < profile.size(); ++j)
                    {
                         if (j > 0)
                         {
                              ingredients += " | ";
                         }
                         ingredients += profile[j];
                    }

                    nlohmann::json food_doc;
                    food_doc = enriched_docs[i];
                    food_doc["id"] = std::get<0>(doc_tuple);
                    food_doc["title"] = std::get<1>(doc_tuple);
                    food_doc["content"] = std::get<2>(doc_tuple) + " Ingredients: " + ingredients + ".";
                    food_doc["dish"] = std::get<1>(doc_tuple);
                    food_doc["cuisine"] = (i % 3 == 0) ? "Italian" : ((i % 3 == 1) ? "Japanese" : "Mexican");
                    food_doc["ingredients"] = ingredients;

                    if (client.UpsertDocumentWithFieldsLocal(collection_name, food_doc))
                    {
                         food_updated++;
                    }
               }

               if (verbose)
               {
                    LogOutput("  ↳ Added realistic ingredients to " + std::to_string(food_updated) + " food documents.\n");
               }
          }
          else if (spec.Name == "universities")
          {
               size_t universities_updated = 0;
               const std::vector<UniversityBenchmarkSeed> &university_profiles = GetUniversityBenchmarkSeeds();
               for (size_t i = 0; i < docs.size() && i < university_profiles.size(); ++i)
               {
                    const auto &doc_tuple = docs[i];
                    const auto &profile = university_profiles[i];
                    const int rank = static_cast<int>(i + 1U);
                    const std::vector<std::string> location_aliases = BuildUniversityLocationAliases(profile);
                    const std::string location_alias_text = JoinTextValues(location_aliases, " | ");

                    nlohmann::json university_doc = enriched_docs[i];
                    university_doc["id"] = std::get<0>(doc_tuple);
                    university_doc["title"] = std::get<1>(doc_tuple);
                    university_doc["content"] = std::get<2>(doc_tuple);
                    university_doc["description"] = university_doc.value<std::string>("description", "");
                    university_doc["state"] = profile.State;
                    university_doc["city"] = profile.City;
                    university_doc["city_aliases"] = location_alias_text;
                    university_doc["location_labels"] = location_alias_text;
                    university_doc["search_aliases"] = std::string(profile.Name) + " | " + profile.City + " | " + profile.State + " | " + location_alias_text;
                    university_doc["institution_type"] = profile.Type;
                    university_doc["rank"] = rank;
                    university_doc["rank_signal"] = static_cast<double>(university_profiles.size() - i) / static_cast<double>(university_profiles.size());
                    university_doc["rank_source"] = "webometrics_world_rank";
                    university_doc["rank_scope"] = "benchmark";
                    university_doc["rank_edition"] = "January 2026";
                    university_doc["webometrics_country_rank"] = rank;
                    university_doc["webometrics_world_rank"] = rank;
                    university_doc["webometrics_impact_rank"] = 1 + static_cast<int>((i * 37U) % 100U);
                    university_doc["webometrics_openness_rank"] = 1 + static_cast<int>((i * 53U) % 100U);
                    university_doc["webometrics_excellence_rank"] = 1 + static_cast<int>((i * 71U) % 100U);

                    nlohmann::json label_list = nlohmann::json::array();
                    std::unordered_set<std::string> seen_labels;
                    for (const auto &label : BuildBenchmarkLabels(spec.Name, profile.State, std::get<1>(doc_tuple), std::get<2>(doc_tuple)))
                    {
                         const std::string normalized = Slugify(label);
                         if (!normalized.empty() && seen_labels.insert(normalized).second)
                         {
                              label_list.push_back(normalized);
                         }
                    }

                    for (const auto &alias : location_aliases)
                    {
                         const std::string normalized = Slugify(alias);
                         if (!normalized.empty() && seen_labels.insert(normalized).second)
                         {
                              label_list.push_back(normalized);
                         }
                    }
                    university_doc["labels"] = label_list.dump();

                    if (client.UpsertDocumentWithFieldsLocal(collection_name, university_doc))
                    {
                         universities_updated++;
                    }
               }

               if (verbose)
               {
                    LogOutput("  ↳ Added campus and benchmark ranking metadata to " + std::to_string(universities_updated) + " university documents.\n");
               }
          }
          else if (spec.Name == "people")
          {
               size_t people_updated = 0;

               for (size_t i = 0; i < docs.size(); ++i)
               {
                    const PersonBenchmarkSeed person = BuildPersonBenchmarkSeed(i);
                    const std::string full_name = person.FirstName + " " + person.MiddleName + " " + person.LastName;
                    nlohmann::json person_doc = enriched_docs[i];
                    person_doc["id"] = std::get<0>(docs[i]);
                    person_doc["title"] = full_name;
                    person_doc["content"] = RemoveCommas(person.Biography);
                    person_doc["first_name"] = person.FirstName;
                    person_doc["middle_name"] = person.MiddleName;
                    person_doc["last_name"] = person.LastName;
                    person_doc["full_name"] = full_name;
                    person_doc["biography"] = RemoveCommas(person.Biography);

                    if (client.UpsertDocumentWithFieldsLocal(collection_name, person_doc))
                    {
                         people_updated++;
                    }
               }

               if (verbose)
               {
                    LogOutput("  ↳ Added first, middle, and last names with biographies to " + std::to_string(people_updated) + " people documents.\n");
               }
          }
          else if (spec.Name == "math")
          {
               size_t math_updated = 0;

               for (size_t i = 0; i < docs.size(); ++i)
               {
                    const auto &doc_tuple = docs[i];
                    const std::string &tag = spec.Tags[i % spec.Tags.size()];
                    const double value = static_cast<double>((i + 1) * 11);
                    const double value_b = static_cast<double>((i + 2) * (i + 3));
                    const double value_c = value + value_b + (static_cast<double>(i) / 2.0);
                    const int equation_index = static_cast<int>((i + 1) * 7);
                    const int prime_candidate = 101 + static_cast<int>(i) * 2;

                    nlohmann::json math_doc = enriched_docs[i];
                    math_doc["id"] = std::get<0>(doc_tuple);
                    math_doc["title"] = std::get<1>(doc_tuple);
                    math_doc["content"] = std::get<2>(doc_tuple) + " Values: " +
                                          std::to_string(static_cast<int>(value)) + ", " +
                                          std::to_string(static_cast<int>(value_b)) + ", " +
                                          std::to_string(value_c) + ", " +
                                          std::to_string(equation_index) + ", " +
                                          std::to_string(prime_candidate) + ".";
                    math_doc["topic"] = tag;
                    math_doc["value"] = value;
                    math_doc["value_b"] = value_b;
                    math_doc["value_c"] = value_c;
                    math_doc["equation_index"] = equation_index;
                    math_doc["prime_candidate"] = prime_candidate;

                    if (client.UpsertDocumentWithFieldsLocal(collection_name, math_doc))
                    {
                         math_updated++;
                    }
               }

               if (verbose)
               {
                    LogOutput("  ↳ Added numeric fields to " + std::to_string(math_updated) + " math documents.\n");
               }
          }
          else if (spec.Name == "stocks")
          {
               size_t stocks_updated = 0;

               for (size_t i = 0; i < docs.size(); ++i)
               {
                    const auto &doc_tuple = docs[i];
                    const std::string &ticker = spec.Tags[i % spec.Tags.size()];
                    const std::string cashtag = "$" + std::string(ticker);

                    nlohmann::json stock_doc = enriched_docs[i];
                    stock_doc["id"] = std::get<0>(doc_tuple);
                    stock_doc["title"] = std::get<1>(doc_tuple);
                    stock_doc["content"] = std::get<2>(doc_tuple) + " Demo search examples include " + cashtag + ", " + ticker + ", ticker " + ticker + ", and cashtag " + cashtag + ".";
                    stock_doc["ticker"] = ticker;
                    stock_doc["cashtag"] = cashtag;
                    stock_doc["asset_class"] = (i < 4) ? "equity_etf" : ((i < 7) ? "commodity_or_bond_etf" : "single_stock");
                    stock_doc["watchlist"] = "stocks tickers cashtags finance demo";
                    stock_doc["source"] = "synthetic_demo_news";

                    if (client.UpsertDocumentWithFieldsLocal(collection_name, stock_doc))
                    {
                         stocks_updated++;
                    }
               }

               if (verbose)
               {
                    LogOutput("  ↳ Added ticker and cashtag metadata to " + std::to_string(stocks_updated) + " stock news documents.\n");
               }
          }

          LogOutput("✓ Inserted " + std::to_string(inserted) + " fake documents into '" + collection_name + "'.\n");
          if (inserted != docs.size())
          {
               std::cerr << "✗ Fake collection '" << collection_name << "' imported " << inserted << " of " << docs.size() << " local documents.\n";
          }
          if (inserted > 0)
          {
               inserted_fake_collections.push_back(collection_name);
          }
          if (verbose)
          {
               LogOutput("  ↳ Enriched " + std::to_string(enriched_updated) + " fake documents with description and labels.\n");
          }

          size_t collection_synonyms_added = 0;
          const std::vector<FakeSynonymSeed> &collection_synonyms = GetFakeCollectionSynonyms(spec.Name);
          for (size_t local_index = 0; local_index < collection_synonyms.size(); ++local_index)
          {
               if (g_benchmark_should_stop.load())
               {
                    return false;
               }

               const FakeSynonymSeed &seed = collection_synonyms[local_index];
               const std::string synonym_id = "fake_syn_" + spec.Name + "_" + std::to_string(local_index + 1);
               const bool synonym_added = client.AddSynonym(collection_name, synonym_id, seed.Root, seed.Synonyms);

               if (g_benchmark_should_stop.load())
               {
                    return false;
               }

               if (synonym_added)
               {
                    collection_synonyms_added++;
               }
               else
               {
                    std::cerr << "✗ Failed to add fake synonym '" << synonym_id << "' to collection '" << spec.Name << "'.\n";
               }
          }

          size_t collection_stopwords_added = 0;
          const std::vector<std::string> collection_stopwords = GetFakeCollectionStopwords(spec.Name);
          for (const auto &word : collection_stopwords)
          {
               if (g_benchmark_should_stop.load())
               {
                    return false;
               }

               const bool stopword_added = client.AddStopword(collection_name, word);

               if (g_benchmark_should_stop.load())
               {
                    return false;
               }

               if (stopword_added)
               {
                    collection_stopwords_added++;
               }
               else
               {
                    std::cerr << "✗ Failed to add fake stopword '" << word << "' to collection '" << spec.Name << "'.\n";
               }
          }

          if (verbose && (collection_synonyms_added > 0 || collection_stopwords_added > 0))
          {
               LogOutput("  ↳ Added " + std::to_string(collection_synonyms_added) +
                         " fake synonym group(s) and " + std::to_string(collection_stopwords_added) +
                         " fake stopword(s) to '" + spec.Name + "'.\n");
          }
     }

     size_t aliases_added = 0;
     const size_t alias_limit = std::min<size_t>(3, inserted_fake_collections.size());
     for (size_t i = 0; i < alias_limit; ++i)
     {
          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          const std::string &target_collection = inserted_fake_collections[i];
          const std::string alias_name = "fake_alias_" + std::to_string(i + 1);
          const bool alias_created = client.CreateAlias(alias_name, target_collection);

          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          if (alias_created)
          {
               aliases_added++;
          }
          else
          {
               std::cerr << "✗ Failed to create fake alias '" << alias_name << "' for collection '" << target_collection << "'.\n";
          }
     }

     if (aliases_added > 0)
     {
          LogOutput("✓ Added " + std::to_string(aliases_added) + " fake aliases for inserted fake collections.\n");
     }

     size_t global_synonyms_added = 0;
     for (size_t i = 0; i < kFakeGlobalSynonymSeeds.size(); ++i)
     {
          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          const FakeSynonymSeed &seed = kFakeGlobalSynonymSeeds[i];
          const std::string synonym_id = "fake_global_syn_" + std::to_string(i + 1);
          const bool synonym_added = client.AddGlobalSynonym(synonym_id, seed.Root, seed.Synonyms);

          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          if (synonym_added)
          {
               global_synonyms_added++;
          }
          else
          {
               std::cerr << "✗ Failed to add global fake synonym '" << synonym_id << "'.\n";
          }
     }

     size_t global_stopwords_added = 0;
     for (const std::string &word : kFakeGlobalStopwords)
     {
          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          const bool stopword_added = client.AddGlobalStopword(word);

          if (g_benchmark_should_stop.load())
          {
               return false;
          }

          if (stopword_added)
          {
               global_stopwords_added++;
          }
          else
          {
               std::cerr << "✗ Failed to add global fake stopword '" << word << "'.\n";
          }
     }

     LogOutput("✓ Added " + std::to_string(global_synonyms_added) +
               " global fake synonym group(s) and " + std::to_string(global_stopwords_added) +
               " global fake stopword(s).\n");

     return true;
}

bool ExtractConfigValue(const std::string &line, const std::string &key, std::string &value)
{
     size_t pos = line.find(key);
     if (pos == std::string::npos)
     {
          return false;
     }

     size_t eq = line.find('=', pos + key.size());
     if (eq == std::string::npos)
     {
          return false;
     }

     size_t first_quote = line.find('"', eq);
     if (first_quote == std::string::npos)
     {
          return false;
     }

     size_t second_quote = line.find('"', first_quote + 1);
     if (second_quote == std::string::npos)
     {
          return false;
     }

     value = line.substr(first_quote + 1, second_quote - first_quote - 1);

     return true;
}

bool LoadDurabilityConfig(const std::string &path, DurabilityConfig &config)
{
     std::ifstream file(path);
     if (!file.is_open())
     {
          return false;
     }

     std::string line;

     while (std::getline(file, line))
     {
          std::string trimmed = TrimWhitespace(line);

          if (trimmed.empty() || trimmed[0] == '#')
          {
               continue;
          }

          std::string value;

          if (ExtractConfigValue(trimmed, "wal_sync_mode", value))
          {
               config.WalSyncMode = value;
               continue;
          }

          if (ExtractConfigValue(trimmed, "wal_bytes_per_sync", value))
          {
               config.WalBytesPerSync = value;
               continue;
          }

          if (ExtractConfigValue(trimmed, "manual_wal_flush", value))
          {
               config.ManualWalFlush = value;
               continue;
          }
     }

     return true;
}

int64_t CountBenchmarkDocuments(const AdvancedMetrics &metrics, int num_collections)
{
     int64_t total = 0;

     for (int i = 0; i < num_collections; i++)
     {
          const auto it = metrics.FinalPerCollectionCounts.find(MakeBenchmarkCollectionName(i));

          if (it != metrics.FinalPerCollectionCounts.end())
          {
               total += it->second;
          }
     }

     return total;
}

bool RunSanitySearch(BenchmarkClient &client, const std::string &collection, const std::string &query, bool verbose)
{
     HTTPResponse resp = client.Search(collection, query);

     if (resp.StatusCode != 200)
     {
          if (verbose)
          {
               std::cerr << "  [VERIFY] Search '" << query << "' failed with status " << resp.StatusCode << ".\n";
          }

          return false;
     }

     try
     {
          nlohmann::json result = nlohmann::json::parse(resp.Body);
          int found = 0;

          if (result.contains("found"))
          {
               found = result["found"].get<int>();
          }
          else if (result.contains("hits") && result["hits"].is_array())
          {
               found = static_cast<int>(result["hits"].size());
          }

          if (verbose)
          {
               std::cout << "  [VERIFY] Search '" << query << "': found " << found << " hit(s).\n";
          }

          return found > 0;
     }
     catch (...)
     {
          if (verbose)
          {
               std::cerr << "  [VERIFY] Search '" << query << "' returned invalid JSON.\n";
          }
     }

     return false;
}

static void PrintBenchmarkHelp(const char *program_name)
{
     std::cout << "Usage: " << program_name << " [options]\n"
               << "Options:\n"
               << "  --url URL          Server URL (default: http://127.0.0.1:9200)\n"
               << "  --host HOST        Server host (default: 127.0.0.1)\n"
               << "  --port PORT        Server port (default: 9200)\n"
               << "  --auth TOKEN      Authentication token\n"
               << "  --ssl-auth        Over HTTPS, send token as both Authorization and X-API-Key\n"
               << "  --collections N   Number of collections to create (default: 2)\n"
               << "  --documents N     Total number of documents to insert (default: 50000 per collection)\n"
               << "  --threads N        Number of threads (default: 8)\n"
               << "  --batch-size N     Documents per bulk insert batch (default: 2000)\n"
               << "  --advanced [FILE]  Output detailed JSON metrics (default: adv.json)\n"
               << "  --detailed [FILE] Run comprehensive benchmark testing ALL routes\n"
               << "                    and functionalities (includes --advanced)\n"
               << "  --Search           Run search benchmark on previously inserted data\n"
               << "  --dump             Dump all collections and their documents\n"
               << "  --fake             Insert realistic sample data plus local/global synonyms and stopwords for testing\n"
               << "  --flood            Flood server with continuous random data generation for stress testing\n"
               << "                    (runs until stopped with Ctrl+C, randomly creates collections and documents)\n"
               << "  --id ID            Run UUID/ID for correlation (default: auto-generated)\n"
               << "  --seed SEED        Seed for deterministic runs\n"
               << "  --no-fake-collections  Disable fake helper collections (people, food, stocks, music, sports, etc.)\n"
               << "  --verify-after-restart   Verify counts after server restart\n"
               << "  --verify-final-counts    Verify benchmark collection counts before printing results\n"
               << "  --check-consistency      Check consistency of /status, /stats, /metrics, /doctotal\n"
               << "  --dry-run          Generate collections/docs in memory but don't send to server\n"
               << "  --cleanup          Delete all benchmark-tagged collections at end\n"
               << "  --prefix PREFIX    Custom prefix for benchmark collections (default: bench_{runid}_)\n"
               << "  --durability-config PATH  Load durability settings from config (e.g., run/conf/database.conf)\n"
               << "  --reuse-collections Reuse existing collections instead of deleting/recreating them\n"
               << "  --skip-auth-check  Skip authentication requirement check (useful when auth is disabled)\n"
               << "  --unorganized      Create an 'unorganized' collection with non-standard schema for testing\n"
               << "  --log-file FILE    Structured log file (JSON lines format)\n"
               << "  --verbose, -v      Show detailed progress information\n"
               << "  --help, -h         Show this help message\n";
}

/* Main entry point for the benchmark tool. */

int main(int argc, char *argv[])
{
     try
     {
          /* Install signal handlers to allow graceful shutdown. */

          signal(SIGINT, BenchmarkSignalHandler);
          signal(SIGTERM, BenchmarkSignalHandler);

          g_benchmark_should_stop.store(false);

          /* Reset global benchmark counters before parsing inputs. */

          ResetGlobalStats();

          /* Default benchmark configuration values. */

          std::string base_url = "http://127.0.0.1:9200";
          std::string host = "127.0.0.1";
          int port = 9200;
          bool host_set = false;
          bool port_set = false;
          std::string auth_token = "";

          int num_collections = 2;
          const int default_docs_per_collection = 50000;
          int num_documents = num_collections * default_docs_per_collection;
          int num_threads = 8;
          int batch_size = 2000;
          bool documents_explicitly_set = false;

          bool default_limits_applied = false;
          bool advanced_mode = false;

          std::string advanced_output_file = "adv.json";

          bool search_mode_val = false;
          bool dump_mode = false;
          bool detailed_mode = false;
          bool fake_mode = false;
          bool flood_mode = false;

          verbose_mode = false;

          std::string run_id_val = "";
          std::string run_seed_val = "";

          bool no_fake_collections = false;
          bool verify_after_restart = false;
          bool verify_final_counts_val = false;
          bool check_consistency_val = false;
          bool dry_run_val = false;

          (void)no_fake_collections;
          (void)verify_after_restart;
          (void)dry_run_val;

          bool cleanup_benchmark_val = false;
          bool reuse_collections = false;
          std::string log_file_val = "";

          bool skip_auth_check = false;
          bool create_unorganized_val = false;
          bool ssl_auth_mode = false;

          std::string custom_prefix_val = "";

          std::string durability_config_path = "";

          auto RequireNextValue = [&](int &index, const std::string &option) -> std::string
          {
               if (index + 1 >= argc)
               {
                    std::cerr << "Error: Missing value for " << option << ".\n\n";
                    PrintBenchmarkHelp(argv[0]);
                    throw std::runtime_error("__benchmark_help_shown__");
               }

               return argv[++index];
          };

          /* Parse CLI flags and override defaults. */

          for (int i = 1; i < argc; i++)
          {
               std::string arg = argv[i];

               if (arg == "--url")
               {
                    base_url = RequireNextValue(i, arg);
               }
               else if (arg == "--host")
               {
                    host = RequireNextValue(i, arg);
                    host_set = true;
               }
               else if (arg == "--port")
               {
                    port = std::stoi(RequireNextValue(i, arg));
                    port_set = true;
               }
               else if (arg == "--auth")
               {
                    auth_token = RequireNextValue(i, arg);
               }
               else if (arg == "--ssl-auth")
               {
                    ssl_auth_mode = true;
               }
               else if (arg == "--prefix")
               {
                    custom_prefix_val = RequireNextValue(i, arg);
                    g_collection_prefix = custom_prefix_val;
               }
               else if (arg == "--collections")
               {
                    num_collections = std::stoi(RequireNextValue(i, arg));
               }
               else if (arg == "--documents")
               {
                    num_documents = std::stoi(RequireNextValue(i, arg));
                    documents_explicitly_set = true;
               }
               else if (arg == "--threads")
               {
                    num_threads = std::stoi(RequireNextValue(i, arg));
               }
               else if (arg == "--batch-size")
               {
                    batch_size = std::stoi(RequireNextValue(i, arg));
               }
               else if (arg == "--advanced")
               {
                    advanced_mode = true;

                    if (i + 1 < argc && argv[i + 1][0] != '-')
                    {
                         advanced_output_file = argv[++i];
                    }
               }
               else if (arg == "--Search")
               {
                    search_mode_val = true;
               }
               else if (arg == "--dump")
               {
                    dump_mode = true;
               }
               else if (arg == "--detailed")
               {
                    detailed_mode = true;
                    advanced_mode = true;

                    if (i + 1 < argc && argv[i + 1][0] != '-')
                    {
                         advanced_output_file = argv[++i];
                    }
                    else
                    {
                         advanced_output_file = "ad.json";
                    }
               }
               else if (arg == "--verbose" || arg == "-v")
               {
                    verbose_mode = true;
               }
               else if (arg == "--fake")
               {
                    fake_mode = true;
               }
               else if (arg == "--flood")
               {
                    flood_mode = true;
               }
               else if (arg == "--id")
               {
                    run_id_val = RequireNextValue(i, arg);
               }
               else if (arg == "--seed")
               {
                    run_seed_val = RequireNextValue(i, arg);
               }
               else if (arg == "--no-fake-collections")
               {
                    no_fake_collections = true;
               }
               else if (arg == "--verify-after-restart")
               {
                    verify_after_restart = true;
               }
               else if (arg == "--verify-final-counts")
               {
                    verify_final_counts_val = true;
               }
               else if (arg == "--check-consistency")
               {
                    check_consistency_val = true;
               }
               else if (arg == "--dry-run")
               {
                    dry_run_val = true;
               }
               else if (arg == "--cleanup")
               {
                    cleanup_benchmark_val = true;
               }
               else if (arg == "--reuse-collections")
               {
                    reuse_collections = true;
               }               else if (arg == "--log-file")
               {
                    log_file_val = RequireNextValue(i, arg);
               }
               else if (arg == "--durability-config")
               {
                    durability_config_path = RequireNextValue(i, arg);
               }
               else if (arg == "--skip-auth-check")
               {
                    skip_auth_check = true;
               }
               else if (arg == "--unorganized")
               {
                    create_unorganized_val = true;
               }
               else if (arg == "--help" || arg == "-h")
               {
                    PrintBenchmarkHelp(argv[0]);

                    return 0;
               }
               else
               {
                    std::cerr << "Error: Unknown argument: " << arg << ".\n\n";
                    PrintBenchmarkHelp(argv[0]);
                    return 1;
               }
          }

          if (host_set || port_set)
          {
               base_url = "http://" + host + ":" + std::to_string(port);
          }

          if (!documents_explicitly_set)
          {
               num_documents = num_collections * default_docs_per_collection;
          }

          num_threads = std::max(1, num_threads);
          batch_size = std::max(1, batch_size);

          BenchmarkClient::SetGlobalSSLAuthMode(ssl_auth_mode);

          if (!log_file_val.empty())
          {
               log_file_stream = new std::ofstream(log_file_val, std::ios::app);

               if (!log_file_stream->is_open())
               {
                    std::cerr << "Warning: Could not open log file '" << log_file_val << "' - logging to stderr only.\n";
                    std::cerr << "  Error: " << strerror(errno) << ".\n";

                    delete log_file_stream;

                    log_file_stream = nullptr;
               }
               else
               {
                    *log_file_stream << "\n=== Benchmark started at " << Time() << " ===\n";
                    log_file_stream->flush();

                    std::cout << "Logging to file: " << log_file_val << ".\n";

                    if (!log_file_stream->good())
                    {
                         std::cerr << "Warning: Log file stream is not in good state after opening.\n";
                    }
               }
          }

          PrintBenchmarkTitle("HLQuery Benchmark");
          PrintBenchmarkSection("Preflight");

          if (!skip_auth_check)
          {
               PrintBenchmarkStatus("Status/auth", "checking.");

               BenchmarkClient status_client(base_url, auth_token);

               HTTPResponse status_response = status_client.MakeRequest("GET", "/status", "", 1);

               if (status_response.StatusCode == 401 || status_response.StatusCode == 403)
               {
                    std::cerr << "\nERROR: Authentication required!.\n";
                    std::cerr << "   Server returned HTTP " << status_response.StatusCode;

                    if (status_response.StatusCode == 401)
                    {
                         std::cerr << " (Unauthorized)";
                    }
                    else
                    {
                         std::cerr << " (Forbidden)";
                    }

                    std::cerr << "\n";

                    if (auth_token.empty())
                    {
                         std::cerr << "\n   No authentication token provided.\n";
                         std::cerr << "   Please provide a token using: --auth <token>.\n";
                         std::cerr << "   Or use --skip-auth-check if authentication is disabled.\n";
                    }
                    else
                    {
                         std::cerr << "\n   The provided authentication token is invalid or expired.\n";
                         std::cerr << "   Please check your token and try again.\n";
                    }

                    return 1;
               }

               if (status_response.StatusCode == 200 && !status_response.Body.empty())
               {
                    try
                    {
                         nlohmann::json status_json = nlohmann::json::parse(status_response.Body);

                         if (status_json.contains("auth_required") && status_json["auth_required"].get<bool>())
                         {
                              if (auth_token.empty())
                              {
                                   std::cerr << "\nERROR: Authentication required!.\n";
                                   std::cerr << "   Server is configured to require authentication.\n";
                                   std::cerr << "\n   No authentication token provided.\n";
                                   std::cerr << "   Please provide a token using: --auth <token>.\n";
                                   std::cerr << "   Or use --skip-auth-check if authentication is disabled.\n";

                                   return 1;
                              }
                         }
                    }
                    catch (const std::exception &)
                    {
                         /* Ignore. */
                    }
               }
          }
          else
          {
               PrintBenchmarkStatus("Status/auth", "skipped by flag.");
          }

          PrintBenchmarkStatus("Health", "checking.");

          BenchmarkClient health_client(base_url, auth_token);

          HTTPResponse health_response = health_client.MakeRequest("GET", "/health", "", 1, true, 5000);

          if (!skip_auth_check && (health_response.StatusCode == 401 || health_response.StatusCode == 403))
          {
               std::cerr << "\nERROR: Authentication required!.\n";
               std::cerr << "   Server returned HTTP " << health_response.StatusCode;

               if (health_response.StatusCode == 401)
               {
                    std::cerr << " (Unauthorized)";
               }
               else
               {
                    std::cerr << " (Forbidden)";
               }

               std::cerr << "\n";

               if (auth_token.empty())
               {
                    std::cerr << "\n   No authentication token provided.\n";
                    std::cerr << "   Please provide a token using: --auth <token>.\n";
                    std::cerr << "   Or use --skip-auth-check if authentication is disabled.\n";
               }
               else
               {
                    std::cerr << "\n   The provided authentication token is invalid or expired.\n";
                    std::cerr << "   Please check your token and try again.\n";
               }

               return 1;
          }

          if (health_response.StatusCode == -1)
          {
               std::cerr << "\nERROR: Server is not responding!.\n";
               std::cerr << "   " << (health_response.ErrorMessage.empty() ? "Could not connect to " + base_url : health_response.ErrorMessage) << "\n";
               std::cerr << "\n   Please ensure the hlquery server is running on " << base_url << ".\n";

               return 1;
          }
          else if (health_response.StatusCode != 200 && health_response.StatusCode != 503)
          {
               std::cerr << "\nERROR: Server health check failed!.\n";
               std::cerr << "   Health check returned status code: " << health_response.StatusCode << ".\n";
               std::cerr << "\n   Please ensure the hlquery server is running properly on " << base_url << ".\n";

               return 1;
          }
          else if (health_response.StatusCode == 503)
          {
               std::cout << "   Warning: Server is in DEGRADED state (collections may not be fully loaded).\n";
               std::cout << "   Continuing anyway - benchmarks will create their own collections...\n";
          }
          else
          {
               PrintBenchmarkStatus("Health", "ready.");
          }

          health_client.Reset();

          PrintBenchmarkStatus("Stability recheck", "running.");

          HTTPResponse second_health = health_client.MakeRequest("GET", "/health");

          if (!skip_auth_check && (second_health.StatusCode == 401 || second_health.StatusCode == 403))
          {
               std::cerr << "\nERROR: Authentication required!.\n";
               std::cerr << "   Server returned HTTP " << second_health.StatusCode;

               if (second_health.StatusCode == 401)
               {
                    std::cerr << " (Unauthorized)";
               }
               else
               {
                    std::cerr << " (Forbidden)";
               }

               std::cerr << "\n";

               if (auth_token.empty())
               {
                    std::cerr << "\n   No authentication token provided.\n";
                    std::cerr << "   Please provide a token using: --auth <token>.\n";
                    std::cerr << "   Or use --skip-auth-check if authentication is disabled.\n";
               }
               else
               {
                    std::cerr << "\n   The provided authentication token is invalid or expired.\n";
                    std::cerr << "   Please check your token and try again.\n";
               }

               return 1;
          }

          if (second_health.StatusCode == -1)
          {
               std::cerr << "\nERROR: Server became unresponsive after delay!.\n";
               std::cerr << "   This may indicate resource exhaustion from previous benchmark run.\n";
               std::cerr << "   Please restart the server before running another benchmark.\n";

               return 1;
          }
          else if (second_health.StatusCode != 200 && second_health.StatusCode != 503)
          {
               std::cerr << "\nERROR: Server became unhealthy after delay (status: " << second_health.StatusCode << ")!.\n";
               std::cerr << "   This may indicate resource exhaustion from previous benchmark run.\n";
               std::cerr << "   Please restart the server before running another benchmark.\n";

               return 1;
          }

          PrintBenchmarkStatus("Stability recheck", "ready.");

          health_client.Reset();

          if (create_unorganized_val)
          {
               LogOutput("\n");
               LogOutput("CREATING UNORGANIZED COLLECTION\n");
               LogOutput("----------------------------------------------------------------\n");

               BenchmarkClient unorganized_client(base_url, auth_token, reuse_collections);

               std::string conn_error_val = unorganized_client.TestConnection();

               if (!conn_error_val.empty())
               {
                    std::cerr << "✗ Cannot connect to server for unorganized collection: " << conn_error_val << ".\n";
                    create_unorganized_val = false;
               }

               if (create_unorganized_val)
               {
                    nlohmann::json unorganized_fields = nlohmann::json::array();

                    unorganized_fields.push_back({{"name", "name"}, {"type", "string"}});
                    unorganized_fields.push_back({{"name", "identifier"}, {"type", "string"}});
                    unorganized_fields.push_back({{"name", "info"}, {"type", "string"}});
                    unorganized_fields.push_back({{"name", "extra"}, {"type", "string"}});
                    unorganized_fields.push_back({{"name", "score"}, {"type", "float"}});
                    unorganized_fields.push_back({{"name", "group"}, {"type", "string"}});
                    unorganized_fields.push_back({{"name", "tags"}, {"type", "string"}});

                    bool created = unorganized_client.CreateCollectionWithSchema("unorganized", unorganized_fields, "");

                    if (created)
                    {
                         LogOutput("✓ unorganized collection created.\n");

                         LogOutput("Inserting 100 test documents into unorganized collection...\n");

                         nlohmann::json payload_json;

                         payload_json["documents"] = nlohmann::json::array();

                         for (int i = 1; i <= 100; i++)
                         {
                              nlohmann::json doc;

                              doc["id"] = std::to_string(10000 + i);
                              doc["document_id"] = doc["id"];
                              doc["name"] = "Unorganized Item #" + std::to_string(i);
                              doc["identifier"] = "item_" + std::to_string(i);
                              doc["info"] = "Unorganized info entry " + std::to_string(i) + ". This document tests Hanalyzer ability to adapt to non-standard schemas where title and content are missing. It should still display meaningful information from other fields.";
                              doc["extra"] = "Extra metadata for item " + std::to_string(i) + " providing additional context for Search testing.";
                              doc["score"] = static_cast<float>(rand() % 1000) / 10.0f;
                              doc["group"] = (i % 5 == 0) ? "Alpha" : ((i % 5 == 1) ? "Beta" : ((i % 5 == 2) ? "Gamma" : "Delta"));
                              doc["tags"] = "tag" + std::to_string(i % 10) + " test unorganized";

                              if (i % 10 == 0)
                              {
                                   doc["special_field_" + std::to_string(i)] = "Special value " + std::to_string(i * 7);
                              }

                              payload_json["documents"].push_back(doc);
                         }

                         std::string json_str_val = payload_json.dump();

                         HTTPResponse response = unorganized_client.MakeRequest("POST", "/collections/unorganized/documents/import", json_str_val, 3);

                         if (response.StatusCode == 200 || response.StatusCode == 201 || response.StatusCode == 207)
                         {
                              try
                              {
                                   nlohmann::json result = nlohmann::json::parse(response.Body);

                                   int imported = result.contains("imported") ? result["imported"].get<int>() : 100;

                                   LogOutput("✓ Inserted " + std::to_string(imported) + " test documents into unorganized collection.\n");

                                   if (response.StatusCode == 207)
                                   {
                                        LogOutput("  (Some documents may have failed to insert, status 207 Multi-Status).\n");
                                   }
                              }
                              catch (...)
                              {
                                   LogOutput("✓ Inserted 100 test documents into unorganized collection.\n");
                              }
                         }
                         else
                         {
                              std::cerr << "✗ Failed to insert documents into unorganized collection (HTTP " << response.StatusCode << "): " << response.Body << ".\n";
                         }
                    }
                    else
                    {
                         std::cerr << "✗ Failed to create unorganized collection.\n";
                    }
               }

               LogOutput("\n");
          }

          if (fake_mode)
          {
               LogOutput("CREATING FAKE COLLECTIONS\n");
               LogOutput("----------------------------------------------------------------\n");

               if (run_id_val.empty())
               {
                    auto now = Now();
                    auto run_id_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

                    static std::random_device rd;
                    static std::mt19937 gen(rd());
                    std::uniform_int_distribution<> dis(1000, 9999);

                    run_id_val = std::to_string(run_id_timestamp) + "_" + std::to_string(dis(gen));
               }

               if (custom_prefix_val.empty() && !reuse_collections)
               {
                    g_collection_prefix = "bench_" + run_id_val + "_";
               }

               bool fake_ok = CreateFakeCollections(base_url, auth_token, reuse_collections, verbose_mode);

               LogOutput("\n");

               bool fake_only = !detailed_mode && !search_mode_val && !dump_mode && !flood_mode && !advanced_mode;

               if (fake_only)
               {
                    if (fake_ok)
                    {
                         std::cout << "✓ Fake sample data inserted. Skipping benchmark (use --detailed or omit --fake to run benchmarks).\n";
                         return 0;
                    }

                    std::cerr << "✗ Failed to insert fake sample data. Skipping benchmark.\n";
                    return 1;
               }
          }

          if (dump_mode)
          {
               DumpAllCollections(base_url, auth_token);

               return 0;
          }

          if (detailed_mode)
          {
               RunDetailedBenchmark(base_url, auth_token, num_collections, num_documents, num_threads, batch_size, reuse_collections);
               WriteAdvancedJSON(advanced_output_file, advanced_metrics);

               return 0;
          }

          if (search_mode_val)
          {
               RunSearches(base_url, auth_token);

               return 0;
          }

          if (flood_mode)
          {
               int flood_threads_val = std::min(num_threads, 4);

               if (num_threads > 4 && verbose_mode)
               {
                    std::cout << "Note: Limiting flood mode to " << flood_threads_val << " threads to prevent server overload.\n";
               }

               RunFloodBenchmark(base_url, auth_token, flood_threads_val, verbose_mode, reuse_collections);

               return 0;
          }

          if (!default_limits_applied && num_collections > 1000)
          {
               if (verbose_mode)
               {
                    std::cout << "Note: Limiting collections to 1000 (use --flood for unlimited).\n";
               }

               num_collections = 1000;
          }

          if (!default_limits_applied && num_documents > 1000000)
          {
               if (verbose_mode)
               {
                    std::cout << "Note: Limiting documents to 1000000 (use --flood for unlimited).\n";
               }

               num_documents = 1000000;
          }

          setvbuf(stdout, nullptr, _IONBF, 0);

          const int active_collection_threads = std::max(1, std::min(num_threads, num_collections));
          const int active_document_threads = std::max(1, num_threads);

          if (run_id_val.empty())
          {
               auto now = Now();

               auto run_id_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

               static std::random_device rd;
               static std::mt19937 gen(rd());

               std::uniform_int_distribution<> dis(1000, 9999);

               run_id_val = std::to_string(run_id_timestamp) + "_" + std::to_string(dis(gen));
          }

          if (custom_prefix_val.empty() && !reuse_collections)
          {
               g_collection_prefix = "bench_" + run_id_val + "_";
          }

          PrintBenchmarkSection("Run plan");
          PrintBenchmarkValue("Endpoint", base_url);
          PrintBenchmarkValue("Collections", std::to_string(num_collections));
          PrintBenchmarkValue("Documents", std::to_string(num_documents));
          PrintBenchmarkValue("Workers", std::to_string(num_threads) + " requested, " + std::to_string(active_document_threads) + " ingest, " + std::to_string(active_collection_threads) + " collection");
          PrintBenchmarkValue("Batch size", std::to_string(batch_size));
          PrintBenchmarkValue("Collection prefix", g_collection_prefix);

          if (advanced_mode)
          {
               PrintBenchmarkValue("Advanced output", advanced_output_file);
          }

          if (verbose_mode)
          {
               PrintBenchmarkValue("Verbose mode", "on");
          }

          if (advanced_mode)
          {
               advanced_metrics.ConfigURL = base_url;
               advanced_metrics.ConfigAuthToken = auth_token.empty() ? "" : "***";
               advanced_metrics.ConfigCollections = num_collections;
               advanced_metrics.ConfigDocuments = num_documents;
               advanced_metrics.ConfigThreads = num_threads;
               advanced_metrics.ConfigBatchSize = batch_size;
          }

          auto start_time_val = Now();

          BenchmarkClient control_client(base_url, auth_token);
          if (advanced_mode)
          {
               advanced_metrics.RunID = run_id_val;
               advanced_metrics.RunSeed = run_seed_val;
          }

          DurabilityConfig durability_config;
          bool durability_config_loaded = false;
          std::string durability_path_used;

          if (!durability_config_path.empty())
          {
               durability_config_loaded = LoadDurabilityConfig(durability_config_path, durability_config);
               durability_path_used = durability_config_path;
          }
          else
          {
               const std::vector<std::string> default_paths = {
                    "run/conf/database.conf",
                    "run/conf/hlquery.conf",
                    "conf/database.conf",
                    "conf/hlquery.conf",
                    "/etc/hlquery/hlquery.conf",
                    "/etc/hlquery/database.conf"};

               for (const auto &candidate : default_paths)
               {
                    if (LoadDurabilityConfig(candidate, durability_config))
                    {
                         durability_config_loaded = true;
                         durability_path_used = candidate;
                         break;
                    }
               }
          }

          if (advanced_mode)
          {
               advanced_metrics.DurabilityConfigPath = durability_config_loaded ? durability_path_used : "";
               advanced_metrics.WalSyncMode = durability_config.WalSyncMode;
               advanced_metrics.WalBytesPerSync = durability_config.WalBytesPerSync;
               advanced_metrics.ManualWalFlush = durability_config.ManualWalFlush;
          }

               int collections_per_thread_val = (num_collections + active_collection_threads - 1) / active_collection_threads;

          if (!reuse_collections && verbose_mode)
          {
               std::cout << "Phase 0: Cleaning up existing benchmark collections...\n";
          }

          std::set<std::string> existing_set_val;

          if (!reuse_collections)
          {
               try
               {
                    BenchmarkClient check_client(base_url, auth_token);

                    std::vector<std::string> existing_collections_val = check_client.ListCollections();

                    existing_set_val.insert(existing_collections_val.begin(), existing_collections_val.end());

                    if (!existing_set_val.empty() && verbose_mode)
                    {
                         std::cout << "  Found " << existing_set_val.size() << " existing collections to clean up.\n";
                    }
               }
               catch (const std::exception &e)
               {
                    if (verbose_mode)
                    {
                         std::cout << "  Note: Could not list collections (may not exist): " << e.what() << ".\n";
                    }
               }

               if (!existing_set_val.empty())
               {
                    std::set<std::string> bench_collections_val;

                    for (const auto &col : existing_set_val)
                    {
                         if (IsBenchmarkCollectionNameForCurrentPrefix(col) || col.find("random_") == 0)
                         {
                              bench_collections_val.insert(col);
                         }
                    }

                    if (!bench_collections_val.empty())
                    {
                         if (verbose_mode)
                         {
                              std::cout << "  Found " << bench_collections_val.size() << " existing benchmark collections.\n";
                              std::cout << "  Note: Skipping bulk deletion (too slow - scans all LSM keys).\n";
                              std::cout << "  Collections will be deleted individually during creation if needed.\n";
                         }
                    }
                    else if (verbose_mode)
                    {
                         std::cout << "  No benchmark collections found in existing collections.\n";
                    }
               }
               else if (verbose_mode)
               {
                    std::cout << "  No existing collections to clean up.\n";
               }
          }
          else if (verbose_mode)
          {
               std::cout << "Phase 0: Reusing existing collections (--reuse-collections enabled).\n";
          }

          PrintBenchmarkSection("Progress");

          ResetProgressBar();

          if (verbose_mode)
          {
               PrintBenchmarkValue("Phase 1", "creating " + std::to_string(num_collections) + " collections");
          }
          else
          {
               PrintBenchmarkValue("Collections", "creating");
          }

          if (advanced_mode)
          {
               advanced_metrics.Phase1StartMS = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time_val).count();
          }

          if (g_benchmark_should_stop.load())
          {
               std::cerr << "\n[INTERRUPT] Benchmark interrupted before Phase 1.\n";
               return 1;
          }

          std::vector<std::thread> collection_threads_vec;

          try
          {
               for (int i = 0; i < active_collection_threads; i++)
               {
                    if (g_benchmark_should_stop.load())
                    {
                         break;
                    }

                    int start_val = i * collections_per_thread_val;
                    int end_val = std::min(start_val + collections_per_thread_val, num_collections);

                    if (start_val < num_collections)
                    {
                         collection_threads_vec.emplace_back(CreateCollectionsThread, base_url, auth_token, start_val, end_val, advanced_mode, num_collections, reuse_collections);
                    }
               }

               for (auto &t : collection_threads_vec)
               {
                    if (t.joinable())
                    {
                         if (g_benchmark_should_stop.load())
                         {
                              t.detach();
                         }
                         else
                         {
                              t.join();
                         }
                    }
               }

               if (g_benchmark_should_stop.load())
               {
                    std::cerr << "\n[INTERRUPT] Benchmark interrupted during Phase 1.\n";
                    return 1;
               }
          }
          catch (const std::exception &e)
          {
               std::cerr << "\n[CRITICAL] Exception in Phase 1 (collection creation): " << e.what() << ".\n";
               std::cerr << "   Attempting to join remaining threads...\n";

               for (auto &t : collection_threads_vec)
               {
                    if (t.joinable())
                    {
                         try
                         {
                              t.join();
                         }
                         catch (...)
                         {
                              /* Ignore. */
                         }
                    }
               }

               std::cerr << "   Phase 1 failed - benchmark may be incomplete.\n";
          }
          catch (...)
          {
               std::cerr << "\n[CRITICAL] Unknown exception in Phase 1 (collection creation).\n";
               std::cerr << "   Attempting to join remaining threads...\n";

               for (auto &t : collection_threads_vec)
               {
                    if (t.joinable())
                    {
                         try
                         {
                              t.join();
                         }
                         catch (...)
                         {
                              /* Ignore. */
                         }
                    }
               }

               std::cerr << "   Phase 1 failed - benchmark may be incomplete.\n";
          }

          if (!verbose_mode)
          {
               std::cout << "\n";
          }

          if (advanced_mode)
          {
               advanced_metrics.Phase1EndMS = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time_val).count();
               advanced_metrics.Phase1CollectionsCreated = collections_created.load();
               advanced_metrics.Phase1CollectionsSkipped = collections_skipped.load();

               int64_t phase1_duration_val = advanced_metrics.Phase1EndMS - advanced_metrics.Phase1StartMS;

               if (phase1_duration_val > 0)
               {
                    advanced_metrics.Phase1ThroughputCollectionsPerSec = (collections_created.load() * 1000.0) / phase1_duration_val;
               }
          }

          if (verbose_mode)
          {
               std::cout << "\n";
               std::cout << "Collections created: " << collections_created.load() << ".\n";
               std::cout << "Collections skipped: " << collections_skipped.load() << ".\n";
          }

          if (g_benchmark_should_stop.load())
          {
               std::cerr << "\n[INTERRUPT] Benchmark interrupted before Phase 2.\n";
               return 1;
          }

          ResetProgressBar();

          int docs_per_collection_val = num_documents / num_collections;
          int remaining_docs_val = num_documents % num_collections;
          auto ingest_start_time_val = Now();
          auto ingest_end_time_val = ingest_start_time_val;
          int64_t ingest_duration_ms = 0;

          if (verbose_mode)
          {
               std::cout << "\nPhase 2: Inserting " << num_documents << " documents across " << num_collections << " collections...\n";
               std::cout << "Documents per collection: " << docs_per_collection_val << ".\n";

               if (remaining_docs_val > 0)
               {
                    std::cout << " (+ " << remaining_docs_val << " extra docs in first " << remaining_docs_val << " collections).\n";
               }

               std::cout << "\n";
          }
          else
          {
               PrintBenchmarkValue("Documents", "inserting");
          }

          if (advanced_mode)
          {
               advanced_metrics.Phase2StartMS = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time_val).count();
               advanced_metrics.IngestStartMS = advanced_metrics.Phase2StartMS;
          }

          std::vector<std::thread> document_threads_vec;

          try
          {
               for (int i = 0; i < active_document_threads; i++)
               {
                    if (g_benchmark_should_stop.load())
                    {
                         break;
                    }

                    document_threads_vec.emplace_back(InsertDocumentsThread, base_url, auth_token, num_collections, docs_per_collection_val, remaining_docs_val, i, active_document_threads, batch_size, advanced_mode, num_documents, run_id_val, reuse_collections);
               }

               for (auto &t : document_threads_vec)
               {
                    if (t.joinable())
                    {
                         if (g_benchmark_should_stop.load())
                         {
                              t.detach();
                         }
                         else
                         {
                              t.join();
                         }
                    }
               }

               if (g_benchmark_should_stop.load())
               {
                    std::cerr << "\n[INTERRUPT] Benchmark interrupted during Phase 2.\n";
                    return 1;
               }
          }
          catch (const std::exception &e)
          {
               std::cerr << "\n[CRITICAL] Exception in Phase 2 (document insertion): " << e.what() << ".\n";
               std::cerr << "   Attempting to join remaining threads...\n";

               for (auto &t : document_threads_vec)
               {
                    if (t.joinable())
                    {
                         try
                         {
                              t.join();
                         }
                         catch (...)
                         {
                              /* Ignore. */
                         }
                    }
               }

               std::cerr << "   Phase 2 failed - benchmark may be incomplete.\n";
          }
          catch (...)
          {
               std::cerr << "\n[CRITICAL] Unknown exception in Phase 2 (document insertion).\n";
               std::cerr << "   Attempting to join remaining threads...\n";

               for (auto &t : document_threads_vec)
               {
                    if (t.joinable())
                    {
                         try
                         {
                              t.join();
                         }
                         catch (...)
                         {
                              /* Ignore. */
                         }
                    }
               }

               std::cerr << "   Phase 2 failed - benchmark may be incomplete.\n";
          }

          if (!verbose_mode)
          {
               std::cout << "\n";
          }

          if (advanced_mode)
          {
               advanced_metrics.Phase2EndMS = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time_val).count();
               advanced_metrics.Phase2DocumentsInserted = documents_inserted.load();
               advanced_metrics.Phase2DocumentsSkipped = documents_skipped.load();

               int64_t phase2_duration_val = advanced_metrics.Phase2EndMS - advanced_metrics.Phase2StartMS;

               if (phase2_duration_val > 0)
               {
                    advanced_metrics.Phase2ThroughputDocsPerSec = (documents_inserted.load() * 1000.0) / phase2_duration_val;
               }
          }

          if (g_benchmark_should_stop.load())
          {
               std::cerr << "\n[INTERRUPT] Benchmark interrupted before Phase 2b.\n";
               return 1;
          }

          int additional_docs_per_collection_val = 0;
          int total_additional_docs_val = num_collections * additional_docs_per_collection_val;

          if (total_additional_docs_val > 0)
          {
               if (verbose_mode)
               {
                    PrintBenchmarkValue("Phase 2b", "inserting " + std::to_string(additional_docs_per_collection_val) + " additional documents per collection (" + std::to_string(total_additional_docs_val) + " total)");
               }
               else
               {
                    PrintBenchmarkValue("Additional docs", "inserting");
               }

               std::vector<std::thread> additional_document_threads_vec;

               try
               {
                    for (int i = 0; i < active_document_threads; i++)
                    {
                         if (g_benchmark_should_stop.load())
                         {
                              break;
                         }

                         additional_document_threads_vec.emplace_back(InsertAdditionalDocumentsThread, base_url, auth_token, num_collections, docs_per_collection_val, additional_docs_per_collection_val, i, active_document_threads, batch_size, advanced_mode, total_additional_docs_val, run_id_val, reuse_collections);
                    }

                    for (auto &t : additional_document_threads_vec)
                    {
                         if (t.joinable())
                         {
                              if (g_benchmark_should_stop.load())
                              {
                                   t.detach();
                              }
                              else
                              {
                                   t.join();
                              }
                         }
                    }

                    if (g_benchmark_should_stop.load())
                    {
                         std::cerr << "\n[INTERRUPT] Benchmark interrupted during Phase 2b.\n";
                         return 1;
                    }
               }
               catch (const std::exception &e)
               {
                    std::cerr << "\n[CRITICAL] Exception in Phase 2b (additional document insertion): " << e.what() << ".\n";
                    std::cerr << "   Attempting to join remaining threads...\n";

                    for (auto &t : additional_document_threads_vec)
                    {
                         if (t.joinable())
                         {
                              try
                              {
                                   t.join();
                              }
                              catch (...)
                              {
                                   /* Ignore. */
                              }
                         }
                    }

                    std::cerr << "   Phase 2b failed - benchmark may be incomplete.\n";
               }
               catch (...)
               {
                    std::cerr << "\n[CRITICAL] Unknown exception in Phase 2b (additional document insertion).\n";
                    std::cerr << "   Attempting to join remaining threads...\n";

                    for (auto &t : additional_document_threads_vec)
                    {
                         if (t.joinable())
                         {
                              try
                              {
                                   t.join();
                              }
                              catch (...)
                              {
                                   /* Ignore. */
                              }
                         }
                    }

                    std::cerr << "   Phase 2b failed - benchmark may be incomplete.\n";
               }
          }
          else if (verbose_mode)
          {
               PrintBenchmarkValue("Phase 2b", "skipped additional document insertion");
          }

          ingest_end_time_val = Now();
          ingest_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ingest_end_time_val - ingest_start_time_val).count();

          if (advanced_mode)
          {
               advanced_metrics.IngestEndMS = std::chrono::duration_cast<std::chrono::milliseconds>(ingest_end_time_val - start_time_val).count();
               advanced_metrics.IngestDurationMS = ingest_duration_ms;
          }

          if (verbose_mode)
          {
               std::cout << "\nFlushing pending requests...\n";
          }
          else
          {
               std::cout << "\n";
               PrintBenchmarkValue("Commit", "update-counters");
          }

          int64_t flush_duration_ms = 0;
          int flush_status_code = 0;
          auto commit_end_time_val = ingest_end_time_val;

          {
               BenchmarkClient flush_client(base_url, auth_token);

               auto flush_start = Now();
               HTTPResponse flush_resp = flush_client.FlushSync(g_collection_prefix);
               auto flush_end = Now();

               flush_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(flush_end - flush_start).count();
               flush_status_code = flush_resp.StatusCode;
               commit_end_time_val = flush_end;

               if (verbose_mode)
               {
                    std::cout << "  Commit duration: " << flush_duration_ms << " ms.\n";
               }

               if (flush_resp.StatusCode != 200 && flush_resp.StatusCode != 201 && verbose_mode)
               {
                    std::cerr << "  Warning: Commit returned status " << flush_resp.StatusCode << ".\n";
               }
          }

          bool sanity_search_ok = true;
          bool sanity_search_ok2 = true;
          bool sanity_search_ran = false;
          bool enable_sanity_search = false;

          if (enable_sanity_search && num_collections > 0)
          {
               std::string sanity_collection = MakeBenchmarkCollectionName(0);
               BenchmarkClient sanity_client(base_url, auth_token);

               if (verbose_mode)
               {
                    std::cout << "\nForcing index build via sanity search...\n";
               }

               const int max_attempts = 6;
               for (int attempt = 1; attempt <= max_attempts; attempt++)
               {
                    PrintSpinner("Sanity search", attempt, max_attempts, false);
                    sanity_search_ok = RunSanitySearch(sanity_client, sanity_collection, "Lorem", verbose_mode);
                    sanity_search_ok2 = RunSanitySearch(sanity_client, sanity_collection, "Topic", verbose_mode);

                    if (sanity_search_ok && sanity_search_ok2)
                    {
                         break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
               }
               PrintSpinner("Sanity search", max_attempts, max_attempts, true);
               sanity_search_ran = true;
          }

          auto end_time_val = Now();

          auto ingest_commit_duration_val = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - ingest_start_time_val);
          auto duration_val = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - start_time_val);
          auto setup_duration_val = std::chrono::duration_cast<std::chrono::milliseconds>(ingest_start_time_val - start_time_val);

          if (advanced_mode)
          {
               advanced_metrics.CommitStartMS = std::chrono::duration_cast<std::chrono::milliseconds>(ingest_end_time_val - start_time_val).count();
               advanced_metrics.CommitEndMS = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - start_time_val).count();
               advanced_metrics.CommitDurationMS = flush_duration_ms;
               advanced_metrics.CommitStatusCode = flush_status_code;
               advanced_metrics.TotalEndMS = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_val - start_time_val).count();

               if (duration_val.count() > 0)
               {
                    advanced_metrics.TotalThroughputDocsPerSec = (documents_inserted.load() * 1000.0) / duration_val.count();
               }
          }

          BenchmarkClient count_client_val(base_url, auth_token);

          if (verify_final_counts_val)
          {
               if (!verbose_mode)
               {
                    PrintBenchmarkValue("Final counts", "checking");
               }

               GetFinalCounts(count_client_val, advanced_metrics, verbose_mode, num_collections);

               if (advanced_metrics.FinalDocumentsCount > 0 && advanced_metrics.FinalDocumentsCount < documents_inserted.load())
               {
                    std::cerr << "\nERROR: Server reports " << advanced_metrics.FinalDocumentsCount << " documents but benchmark inserted " << documents_inserted.load() << ".\n";
                    std::cerr << "  This may indicate data loss or counting issues!.\n";

                    return 1;
               }

               int64_t expected_docs_val = static_cast<int64_t>(num_documents) + static_cast<int64_t>(total_additional_docs_val);
               int64_t benchmark_docs_val = CountBenchmarkDocuments(advanced_metrics, num_collections);

               if (benchmark_docs_val != expected_docs_val)
               {
                    std::cerr << "\nERROR: Benchmark collections report " << benchmark_docs_val << " documents, expected " << expected_docs_val << ".\n";
                    std::cerr << "  This indicates missing or extra documents in benchmark collections.\n";

                    return 1;
               }
          }
          else
          {
               advanced_metrics.FinalCollectionsCount = static_cast<int64_t>(collections_created.load() + collections_skipped.load());
               advanced_metrics.FinalDocumentsCount = documents_inserted.load();
               advanced_metrics.FinalCollectionNames.clear();
               advanced_metrics.FinalPerCollectionCounts.clear();
               for (int i = 0; i < num_collections; i++)
               {
                    advanced_metrics.FinalCollectionNames.push_back(MakeBenchmarkCollectionName(i));
               }
          }

          std::cout << "\n";
          PrintBenchmarkTitle("Benchmark complete");
          PrintBenchmarkSection("Dataset");
          PrintBenchmarkValue("Collections created", std::to_string(collections_created.load()));
          PrintBenchmarkValue("Collections skipped", std::to_string(collections_skipped.load()));
          if (total_additional_docs_val > 0)
          {
               PrintBenchmarkValue("Target documents", std::to_string(num_documents) + " base + " + std::to_string(total_additional_docs_val) + " additional");
          }
          else
          {
               PrintBenchmarkValue("Target documents", std::to_string(num_documents));
          }
          int64_t total_inserted = documents_inserted.load();
          int64_t additional_inserted = additional_documents_inserted.load();
          int64_t base_inserted = total_inserted - additional_inserted;
          if (total_additional_docs_val > 0 || additional_inserted > 0)
          {
               PrintBenchmarkValue("Documents inserted", std::to_string(total_inserted) + " (base: " + std::to_string(base_inserted) + ", additional: " + std::to_string(additional_inserted) + ")");
          }
          else
          {
               PrintBenchmarkValue("Documents inserted", std::to_string(total_inserted));
          }
          PrintBenchmarkValue("Documents skipped", std::to_string(documents_skipped.load()));

          PrintBenchmarkSection("Timing");
          PrintBenchmarkValue("Ingest", std::to_string(ingest_duration_ms) + " ms");
          PrintBenchmarkValue("Commit", std::to_string(flush_duration_ms) + " ms");
          PrintBenchmarkValue("Ingest + commit", std::to_string(ingest_commit_duration_val.count()) + " ms");
          if (verbose_mode)
          {
               PrintBenchmarkValue("Setup", std::to_string(setup_duration_val.count()) + " ms (cleanup + create)");
          }
          if (ingest_commit_duration_val.count() > 0)
          {
               PrintBenchmarkValue("Ingest rate", FormatBenchmarkRate(documents_inserted.load() * 1000.0 / ingest_commit_duration_val.count()) + " docs/sec");
          }
          else
          {
               PrintBenchmarkValue("Ingest rate", "0 docs/sec");
          }
          PrintBenchmarkValue("Searchable", std::to_string(duration_val.count()) + " ms");
          if (duration_val.count() > 0)
          {
               PrintBenchmarkValue("Searchable rate", FormatBenchmarkRate(documents_inserted.load() * 1000.0 / duration_val.count()) + " docs/sec");
          }
          else
          {
               PrintBenchmarkValue("Searchable rate", "0 docs/sec");
          }
          std::string fsync_mode = (durability_config.WalSyncMode == "none") ? "off" : "on";
          std::string durability_source = durability_config_loaded ? ("config: " + durability_path_used) : "config: not found";

          PrintBenchmarkSection("Storage");
          PrintBenchmarkValue("WAL sync mode", durability_config.WalSyncMode);
          PrintBenchmarkValue("WAL bytes/sync", durability_config.WalBytesPerSync);
          PrintBenchmarkValue("Manual WAL flush", durability_config.ManualWalFlush);
          PrintBenchmarkValue("fsync", fsync_mode);
          PrintBenchmarkValue("Config", durability_source);
          PrintBenchmarkValue("Commit policy", "update-counters after ingest (status " + std::to_string(flush_status_code) + ")");
          PrintBenchmarkValue("Run ID", run_id_val);

          AdvancedMetrics before_metrics_val;

          if (verify_after_restart)
          {
               if (verbose_mode)
               {
                    std::cout << "\nRecording initial counts before benchmark...\n";
               }

               BenchmarkClient before_client(base_url, auth_token);

               GetFinalCounts(before_client, before_metrics_val, verbose_mode, num_collections);

               if (verbose_mode)
               {
                    std::cout << "  Initial collections: " << before_metrics_val.FinalCollectionsCount << ".\n";
                    std::cout << "  Initial documents: " << before_metrics_val.FinalDocumentsCount << ".\n";
               }
          }

          if (enable_sanity_search && num_collections > 0)
          {
               if (!sanity_search_ran)
               {
                    std::string sanity_collection = MakeBenchmarkCollectionName(0);
                    sanity_search_ok = RunSanitySearch(count_client_val, sanity_collection, "Lorem", verbose_mode);
                    sanity_search_ok2 = RunSanitySearch(count_client_val, sanity_collection, "Topic", verbose_mode);
                    sanity_search_ran = true;
               }

               if (!sanity_search_ok || !sanity_search_ok2)
               {
                    std::string sanity_collection = MakeBenchmarkCollectionName(0);
                    std::cerr << "\nERROR: Sanity search verification failed (collection: " << sanity_collection << ").\n";
                    std::cerr << "  This indicates documents are not searchable after commit.\n";

                    return 1;
               }
          }

          if (verbose_mode)
          {
               std::cout << "\nExpected final state:.\n";
               std::cout << "  Collections: " << (collections_created.load() + collections_skipped.load()) << " total (" << collections_created.load() << " created, " << collections_skipped.load() << " reused).\n";
               std::cout << "  Documents: " << documents_inserted.load() << " written (includes new + updated).\n";

               if (!advanced_metrics.FinalCollectionNames.empty())
               {
                    std::cout << "  Collection names:.\n";

                    for (size_t i = 0; i < std::min(static_cast<size_t>(10), advanced_metrics.FinalCollectionNames.size()); i++)
                    {
                         std::cout << "    - " << advanced_metrics.FinalCollectionNames[i] << ".\n";
                    }

                    if (advanced_metrics.FinalCollectionNames.size() > 10)
                    {
                         std::cout << "    ... and " << (advanced_metrics.FinalCollectionNames.size() - 10) << " more.\n";
                    }
               }
          }

          if (check_consistency_val)
          {
               CheckConsistency(count_client_val, verbose_mode, num_collections);
          }

          if (verify_after_restart)
          {
               if (verbose_mode)
               {
                    std::cout << "\n=== VERIFY AFTER RESTART ===\n";
                    std::cout << "Please restart the server now, then press Enter to continue verification...\n";
               }
               else
               {
                    std::cout << "\nPlease restart the server, then press Enter to continue...\n";
               }

               std::cin.get();

               BenchmarkClient restart_client(base_url, auth_token);

               AdvancedMetrics after_metrics_val;

               if (verbose_mode)
               {
                    std::cout << "Verifying counts after restart...\n";
               }

               std::this_thread::sleep_for(std::chrono::milliseconds(500));

               GetFinalCounts(restart_client, after_metrics_val, verbose_mode, num_collections);

               std::cout << "\n=== RESTART VERIFICATION RESULTS ===\n";
               std::cout << "Before restart:.\n";
               std::cout << "  Collections: " << before_metrics_val.FinalCollectionsCount << ".\n";
               std::cout << "  Documents: " << before_metrics_val.FinalDocumentsCount << ".\n";
               std::cout << "\nAfter restart:.\n";
               std::cout << "  Collections: " << after_metrics_val.FinalCollectionsCount << ".\n";
               std::cout << "  Documents: " << after_metrics_val.FinalDocumentsCount << ".\n";

               bool mismatch_val = false;

               if (before_metrics_val.FinalCollectionsCount != after_metrics_val.FinalCollectionsCount)
               {
                    std::cerr << "\nERROR: Collection count mismatch after restart!.\n";
                    std::cerr << "  Before: " << before_metrics_val.FinalCollectionsCount << ".\n";
                    std::cerr << "  After: " << after_metrics_val.FinalCollectionsCount << ".\n";

                    mismatch_val = true;
               }

               if (before_metrics_val.FinalDocumentsCount != after_metrics_val.FinalDocumentsCount)
               {
                    std::cerr << "\nERROR: Document count mismatch after restart!.\n";
                    std::cerr << "  Before: " << before_metrics_val.FinalDocumentsCount << ".\n";
                    std::cerr << "  After: " << after_metrics_val.FinalDocumentsCount << ".\n";

                    mismatch_val = true;
               }

               if (!mismatch_val)
               {
                    std::cout << "\n✓ Counts match after restart - verification passed!.\n";
               }
               else
               {
                    std::cerr << "\n✗ Counts do not match after restart - verification failed!.\n";
                    std::cerr << "  This indicates a counter persistence or recovery issue.\n";
               }

               if (check_consistency_val)
               {
                    std::cout << "\nChecking consistency after restart...\n";

                    CheckConsistency(restart_client, verbose_mode, num_collections);
               }
          }

          if (advanced_mode && !advanced_metrics.BatchTimings.empty())
          {
               advanced_metrics.LatencyPercentiles = CalculatePercentiles(advanced_metrics.BatchTimings);
          }

          if (advanced_mode)
          {
               WriteAdvancedJSON(advanced_output_file, advanced_metrics);
          }

          if (cleanup_benchmark_val)
          {
               CleanupBenchmarkCollections(count_client_val, verbose_mode);
          }

          return 0;
     }
     catch (const std::exception &e)
     {
          if (std::string(e.what()) == "__benchmark_help_shown__")
          {
               return 1;
          }

          std::cerr << "\n[FATAL] Benchmark crashed with exception: " << e.what() << ".\n";

          return 1;
     }
     catch (...)
     {
          std::cerr << "\n[FATAL] Benchmark crashed with unknown exception.\n";

          return 1;
     }
}

/* Signal handler for Ctrl+C. */

void BenchmarkSignalHandler(int signal)
{
     (void)signal;

     g_benchmark_should_stop.store(true);

     if (log_file_stream && log_file_stream->is_open())
     {
          try
          {
               log_file_stream->close();

               delete log_file_stream;

               log_file_stream = nullptr;
          }
          catch (...)
          {
               /* Ignore. */
          }
     }

     std::cerr << "\n[INTERRUPT] Ctrl+C received - stopping benchmark...\n";
}

/* Reset global statistics. */

void ResetGlobalStats()
{
     collections_created = 0;
     documents_inserted = 0;
     additional_documents_inserted = 0;
     collections_skipped = 0;
     documents_skipped = 0;
     additional_documents_skipped = 0;
}
