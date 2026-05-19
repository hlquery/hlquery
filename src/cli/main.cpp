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
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <regex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "app.h"
#include "utils/consolewriter.h"
#include "vendor/json/json.hpp"

/* Trims leading and trailing whitespace from a string value. */

static std::string TrimWhitespace(const std::string &value)
{
     size_t start = 0;

     while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
     {
          ++start;
     }

     size_t end = value.size();

     while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
     {
          --end;
     }

     if (end <= start)
     {
          return "";
     }

     return value.substr(start, end - start);
}

static std::string NormalizeHostValue(const std::string &value)
{
     std::string normalized = TrimWhitespace(value);

     std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
                    { return static_cast<char>(std::tolower(character)); });

     return normalized;
}

static bool IsValidPortValue(int port)
{
     return port > 0 && port <= 65535;
}

static bool IsValidIPAddress(const std::string &value)
{
     sockaddr_in sa4{};
     sockaddr_in6 sa6{};

     return inet_pton(AF_INET, value.c_str(), &sa4) == 1 ||
            inet_pton(AF_INET6, value.c_str(), &sa6) == 1;
}

static bool IsValidHostnameValue(const std::string &value)
{
     if (value.empty() || value.size() > 253 || value.front() == '.' || value.back() == '.')
     {
          return false;
     }

     bool saw_letter = false;
     bool saw_non_dot = false;
     size_t label_length = 0;
     char previous = '\0';

     for (char character : value)
     {
          const unsigned char uc = static_cast<unsigned char>(character);

          if (character == '.')
          {
               if (label_length == 0 || previous == '-')
               {
                    return false;
               }

               label_length = 0;
               previous = character;
               continue;
          }

          if (std::isalnum(uc) == 0 && character != '-')
          {
               return false;
          }

          if (label_length == 0 && character == '-')
          {
               return false;
          }

          ++label_length;

          if (label_length > 63)
          {
               return false;
          }

          if (std::isalpha(uc) != 0)
          {
               saw_letter = true;
          }

          saw_non_dot = true;
          previous = character;
     }

     if (!saw_non_dot || label_length == 0 || previous == '-')
     {
          return false;
     }

     if (!saw_letter && value.find('.') != std::string::npos)
     {
          return false;
     }

     return true;
}

static bool IsValidHostValue(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     return IsValidIPAddress(value) || IsValidHostnameValue(value);
}

static bool IsUnsignedIntegerValue(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     return std::all_of(value.begin(), value.end(), [](unsigned char character)
                        {
                             return std::isdigit(character) != 0;
                        });
}

/* Splits a comma-separated string into trimmed non-empty values. */

static std::vector<std::string> ParseCommaSeparatedList(const std::string &value)
{
     std::vector<std::string> parts;

     size_t pos = 0;

     while (pos < value.size())
     {
          size_t comma = value.find(',', pos);
          std::string token = (comma == std::string::npos) ? value.substr(pos) : value.substr(pos, comma - pos);
          std::string trimmed = TrimWhitespace(token);

          if (!trimmed.empty())
          {
               parts.push_back(trimmed);
          }

          if (comma == std::string::npos)
          {
               break;
          }

          pos = comma + 1;
     }

     return parts;
}

/* Stores parsed search flags and positional search parameters. */

struct SearchCLIOptions
{
     int Limit = 10000;
     int Offset = 0;
     std::string Sort;
     std::string Route;
     bool ExactMatch = false;
     bool Highlight = false;
     bool All = false;
     bool JsonOutput = false;
     bool Skip = false;
     std::string HighlightFields;
     std::string Distributed;
     std::vector<std::string> Collections;
     int MaybeMin = -1;
     int MaybeLimit = -1;
};

/* Stores fallback defaults for the maybe command loaded from config. */

struct CLIMaybeDefaults
{
     std::string Query;
     std::string Collection;
     int Limit = 5;
     int MinResults = 5;
     bool HasConfig = false;
};

/* Resolves the CLI configuration path from the environment or default install path. */

static std::string ResolveCLIConfigPath()
{
     const char *env_config = std::getenv("HLQ_CONFIG");

     if (env_config && *env_config)
     {
          return env_config;
     }

     return HLQUERY_CONFIG_DIR "/hlquery.conf";
}

/* Loads optional defaults for the maybe command from the CLI config section. */

static CLIMaybeDefaults LoadCLIMaybeDefaults(const std::string &config_path)
{
     CLIMaybeDefaults defaults;

     if (config_path.empty() || !std::filesystem::exists(config_path))
     {
          return defaults;
     }

     std::ifstream input(config_path);

     if (!input.is_open())
     {
          return defaults;
     }

     std::ostringstream buffer;
     std::string line;

     while (std::getline(input, line))
     {
          /* Ignore shell-style comments so only XML-like content is parsed. */

          if (TrimWhitespace(line).rfind("#", 0) == 0)
          {
               continue;
          }

          buffer << line << '\n';
     }

     const std::string content = buffer.str();

     std::smatch tag_match;
     const std::regex tag_regex(R"(<cli_maybe\b([^>]*)>)", std::regex::icase);

     if (!std::regex_search(content, tag_match, tag_regex))
     {
          return defaults;
     }

     const std::string attr_text = tag_match[1].str();
     const std::regex attr_regex(R"ATTR(([A-Za-z_][A-Za-z0-9_\-]*)\s*=\s*"([^"]*)")ATTR");
     std::unordered_map<std::string, std::string> attrs;

     /* Normalize attribute names to lowercase so config input stays forgiving. */

     for (std::sregex_iterator it(attr_text.begin(), attr_text.end(), attr_regex), end; it != end; ++it)
     {
          std::string key = (*it)[1].str();
          std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                         {
                              return static_cast<char>(std::tolower(c));
                         });
          attrs[key] = (*it)[2].str();
     }

     defaults.HasConfig = true;
     defaults.Query = TrimWhitespace(attrs["query"]);
     defaults.Collection = TrimWhitespace(attrs["collection"]);

     try
     {
          if (attrs.count("limit") != 0)
          {
               defaults.Limit = std::stoi(attrs["limit"]);
          }
     }
     catch (...)
     {
     }

     try
     {
          if (attrs.count("min_results") != 0)
          {
               defaults.MinResults = std::stoi(attrs["min_results"]);
          }
     }
     catch (...)
     {
     }

     defaults.Limit = std::max(1, std::min(defaults.Limit, 20));
     defaults.MinResults = std::max(1, std::min(defaults.MinResults, 20));

     return defaults;
}

/* Parses command-specific search options that follow the search query arguments. */

static void ParseSearchCLIOptions(const std::vector<std::string> &args, size_t start_index, SearchCLIOptions &options)
{
     for (size_t i = start_index; i < args.size(); ++i)
     {
          const std::string &arg = args[i];

          if (arg == "--exact" || arg == "-e")
          {
               options.ExactMatch = true;
               continue;
          }

          if (arg == "--highlight" || arg == "-h")
          {
               options.Highlight = true;
               continue;
          }

          if (arg == "--all")
          {
               options.All = true;
               continue;
          }

          if (arg == "--json")
          {
               options.JsonOutput = true;
               continue;
          }

          if (arg == "--skip")
          {
               options.Skip = true;
               continue;
          }

          if (arg == "--distributed")
          {
               options.Distributed = "on";
               continue;
          }

          if (arg.rfind("--distributed=", 0) == 0)
          {
               options.Distributed = arg.substr(14);
               std::transform(options.Distributed.begin(), options.Distributed.end(), options.Distributed.begin(), [](unsigned char c)
                              {
                                   return static_cast<char>(std::tolower(c));
                              });
               continue;
          }

          if (arg.rfind("--collections=", 0) == 0)
          {
               options.Collections = ParseCommaSeparatedList(arg.substr(14));
               continue;
          }

          if (arg.rfind("--route=", 0) == 0)
          {
               options.Route = TrimWhitespace(arg.substr(8));
               continue;
          }

          if (arg.rfind("--fields=", 0) == 0)
          {
               options.HighlightFields = arg.substr(9);
               continue;
          }

          if (arg.rfind("--maybe=", 0) == 0)
          {
               std::string maybe_val = arg.substr(8);
               for (char &c : maybe_val)
               {
                    if (c == ':' || c == ';' || c == '|')
                    {
                         c = ',';
                    }
               }

               std::vector<std::string> parts = ParseCommaSeparatedList(maybe_val);
               if (!parts.empty())
               {
                    try
                    {
                         options.MaybeMin = std::stoi(parts[0]);
                    }
                    catch (...)
                    {
                    }
               }
               if (parts.size() >= 2)
               {
                    try
                    {
                         options.MaybeLimit = std::stoi(parts[1]);
                    }
                    catch (...)
                    {
                    }
               }
               continue;
          }

          if (arg.rfind("--maybe-min=", 0) == 0)
          {
               try
               {
                    options.MaybeMin = std::stoi(arg.substr(12));
               }
               catch (...)
               {
               }
               continue;
          }

          if (arg.rfind("--maybe-limit=", 0) == 0)
          {
               try
               {
                    options.MaybeLimit = std::stoi(arg.substr(14));
               }
               catch (...)
               {
               }
               continue;
          }

          size_t relative = i - start_index;

          if (relative == 0)
          {
               /* The first trailing positional value maps to limit for legacy syntax. */

               try
               {
                    options.Limit = std::stoi(arg);
               }
               catch (...)
               {
                    /* Ignore. */
               }
               continue;
          }

          if (relative == 1)
          {
               /* The second trailing positional value maps to offset for legacy syntax. */

               try
               {
                    options.Offset = std::stoi(arg);
               }
               catch (...)
               {
                    /* Ignore. */
               }
               continue;
          }

          if (relative == 2)
          {
               /* The third trailing positional value maps to sort for legacy syntax. */

               options.Sort = arg;
               continue;
          }
     }
}

/* Joins query tokens and infers phrase search when the shell preserved one quoted multi-word token. */

static std::string JoinSearchQueryTokens(const std::vector<std::string> &tokens)
{
     std::string query;

     for (size_t i = 0; i < tokens.size(); ++i)
     {
          if (i != 0)
          {
               query += ' ';
          }

          query += tokens[i];
     }

     return query;
}

/* Extract search query tokens while preserving trailing limit/offset/sort positional arguments. */

static bool ExtractSearchCLIQuery(const std::vector<std::string> &args,
                                  size_t start_index,
                                  std::string &query,
                                  size_t &options_start_index,
                                  bool &phrase_query)
{
     std::vector<std::string> positional;
     size_t index = start_index;

     while (index < args.size() && args[index].rfind("--", 0) != 0 && args[index] != "-e" && args[index] != "-h")
     {
          positional.push_back(args[index]);
          ++index;
     }

     options_start_index = index;

     if (positional.empty())
     {
          return false;
     }

     size_t query_token_count = positional.size();

     if (query_token_count >= 4 &&
         IsUnsignedIntegerValue(positional[query_token_count - 3]) &&
         IsUnsignedIntegerValue(positional[query_token_count - 2]))
     {
          query_token_count -= 3;
     }
     else if (query_token_count >= 3 &&
              IsUnsignedIntegerValue(positional[query_token_count - 2]) &&
              IsUnsignedIntegerValue(positional[query_token_count - 1]))
     {
          query_token_count -= 2;
     }
     else if (query_token_count >= 2 && IsUnsignedIntegerValue(positional[query_token_count - 1]))
     {
          --query_token_count;
     }

     if (query_token_count == 0)
     {
          query_token_count = 1;
     }

     std::vector<std::string> query_tokens(positional.begin(), positional.begin() + static_cast<long>(query_token_count));
     query = JoinSearchQueryTokens(query_tokens);
     phrase_query = (query_tokens.size() == 1 && query_tokens[0].find(' ') != std::string::npos);
     options_start_index = start_index + query_token_count;

     return !query.empty();
}

static bool ContainsQueryKeyword(const std::string &query, const std::string &keyword)
{
     if (keyword.empty() || query.size() < keyword.size())
     {
          return false;
     }

     for (size_t i = 0; i + keyword.size() <= query.size(); ++i)
     {
          bool matches = true;

          for (size_t j = 0; j < keyword.size(); ++j)
          {
               if (std::toupper(static_cast<unsigned char>(query[i + j])) != keyword[j])
               {
                    matches = false;
                    break;
               }
          }

          if (!matches)
          {
               continue;
          }

          const bool left_ok = (i == 0) || !std::isalnum(static_cast<unsigned char>(query[i - 1]));
          const bool right_ok = (i + keyword.size() == query.size()) || !std::isalnum(static_cast<unsigned char>(query[i + keyword.size()]));

          if (left_ok && right_ok)
          {
               return true;
          }
     }

     return false;
}

static bool LooksLikeStructuredSearchQuery(const std::string &query)
{
     if (query.empty())
     {
          return false;
     }

     if (query.find_first_of("\"*?~^[]()") != std::string::npos)
     {
          return true;
     }

     if (query.find("&&") != std::string::npos || query.find("||") != std::string::npos)
     {
          return true;
     }

     if (query.front() == '!')
     {
          return true;
     }

     if (query.find(':') != std::string::npos)
     {
          return true;
     }

     if (ContainsQueryKeyword(query, "AND") || ContainsQueryKeyword(query, "OR") || ContainsQueryKeyword(query, "NOT") || ContainsQueryKeyword(query, "TO"))
     {
          return true;
     }

     return false;
}

static std::string BuildCLIQueryForServer(const std::string &query, bool phrase_query)
{
     std::string trimmed = TrimWhitespace(query);

     if (trimmed.empty())
     {
          return query;
     }

     if (trimmed.size() >= 2 && trimmed.front() == '\'' && trimmed.back() == '\'')
     {
          return "\"" + trimmed.substr(1, trimmed.size() - 2) + "\"";
     }

     if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
     {
          return trimmed;
     }

     const bool structured_query = LooksLikeStructuredSearchQuery(trimmed);

     if (!structured_query)
     {
          return "\"" + trimmed + "\"";
     }

     return trimmed;
}

/* UpdateHostPortFromURL parses the base URL and extracts host and port. */

static void UpdateHostPortFromURL(const std::string &BaseURL, std::string &HostValue, int &PortValue)
{
     size_t start_pos = BaseURL.find("://");

     if (start_pos != std::string::npos)
     {
          start_pos += 3;

          size_t colon_pos = BaseURL.find(":", start_pos);
          size_t slash_pos = BaseURL.find("/", start_pos);

          if (colon_pos != std::string::npos && colon_pos < (slash_pos != std::string::npos ? slash_pos : BaseURL.length()))
          {
               HostValue = BaseURL.substr(start_pos, colon_pos - start_pos);

               try
               {
                    PortValue = std::stoi(BaseURL.substr(colon_pos + 1, (slash_pos != std::string::npos ? slash_pos : BaseURL.length()) - colon_pos - 1));
               }
               catch (...)
               {
                    /* Ignore invalid port values. */
               }
          }
          else if (slash_pos != std::string::npos)
          {
               HostValue = BaseURL.substr(start_pos, slash_pos - start_pos);
          }
          else
          {
               HostValue = BaseURL.substr(start_pos);
          }
     }
}

/* URLUsesHTTPS returns true when the URL explicitly uses the https scheme. */

static bool URLUsesHTTPS(const std::string &BaseURL)
{
     size_t scheme_pos = BaseURL.find("://");

     if (scheme_pos == std::string::npos)
     {
          return false;
     }

     std::string scheme = BaseURL.substr(0, scheme_pos);

     std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                    [](unsigned char c)
                    {
                         return static_cast<char>(std::tolower(c));
                    });

     return scheme == "https";
}

/* ExtractProgramName resolves the display name from argv[0]. */

static std::string ExtractProgramName(const char *argv0)
{
     if (!argv0 || !*argv0)
     {
          return "hlquery-cli";
     }

     std::string program_name = argv0;

     size_t last_slash = program_name.find_last_of("/\\");

     if (last_slash != std::string::npos && last_slash + 1 < program_name.size())
     {
          program_name = program_name.substr(last_slash + 1);
     }

     if (program_name.empty())
     {
          program_name = "hlquery-cli";
     }

     return program_name;
}

/* Main entry point for the hlquery CLI. */

int main(int argc, char *argv[])
{
     try
     {
          /* Load the optional CLI defaults before any environment or argv overrides are applied. */

          const std::string cli_config_path = ResolveCLIConfigPath();
          const CLIMaybeDefaults cli_maybe_defaults = LoadCLIMaybeDefaults(cli_config_path);

          /* Default connection values for local development. */

          std::string base_url = "http://localhost:9200";
          std::string host_val = "localhost";

          int port_val = 9200;
          bool use_https_val = false;

          /* CLI settings with safe defaults. */

          bool raw_mode_val = false;
          bool ssl_auth_val = false;

          std::string auth_token_val = "";

          std::vector<std::string> args_vec;

          /* Pull base URL from environment when available. */

          const char *env_base_url = std::getenv("HLQ_BASE_URL");

          if (env_base_url && *env_base_url)
          {
               base_url = env_base_url;
               UpdateHostPortFromURL(base_url, host_val, port_val);
               use_https_val = URLUsesHTTPS(base_url);
          }

          /* Load the auth token from the environment if provided. */

          const char *env_auth = std::getenv("HLQ_AUTH_TOKEN");

          if (env_auth && *env_auth)
          {
               auth_token_val = env_auth;
          }

          const char *env_ssl_auth = std::getenv("HLQ_SSL_AUTH");

          if (env_ssl_auth && *env_ssl_auth)
          {
               std::string ssl_env_val = env_ssl_auth;
               std::transform(ssl_env_val.begin(), ssl_env_val.end(), ssl_env_val.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               ssl_auth_val = (ssl_env_val == "1" || ssl_env_val == "true" || ssl_env_val == "yes" || ssl_env_val == "on");
          }

          int timeout_seconds_val = 30;
          (void)host_val;
          (void)port_val;
          (void)base_url;
          (void)auth_token_val;
          (void)raw_mode_val;
          (void)ssl_auth_val;

          /* Parse CLI arguments and collect positional values. */

          std::vector<std::string> normalized_args;

          normalized_args.reserve(static_cast<size_t>(argc));

          normalized_args.push_back(argv[0]);

          std::string program_name = ExtractProgramName(argv[0]);

          /* Normalize older short aliases into the long option names expected by getopt. */

          for (int i = 1; i < argc; i++)
          {
               std::string arg = argv[i];

               if (arg == "-url")
               {
                    arg = "--url";
               }
               else if (arg == "-auth")
               {
                    arg = "--auth";
               }

               normalized_args.push_back(arg);
          }

          std::vector<char *> getopt_argv;

          getopt_argv.reserve(normalized_args.size());

          for (auto &arg : normalized_args)
          {
               getopt_argv.push_back(const_cast<char *>(arg.c_str()));
          }

          int getopt_argc = static_cast<int>(getopt_argv.size());
          bool request_dry_run = false;
          bool request_dry_run_curl = false;

          static struct option long_options[] =
               {
                    {"url", required_argument, 0, 0},
                    {"host", required_argument, 0, 0},
                    {"port", required_argument, 0, 0},
                    {"auth", required_argument, 0, 0},
                    {"ssl-auth", no_argument, 0, 0},
                    {"raw", no_argument, 0, 0},
                    {"timeout", required_argument, 0, 0},
                    {"dry-run-request", no_argument, 0, 0},
                    {"curl", no_argument, 0, 0},
                    {"help", no_argument, 0, 'h'},
                    {"examples", no_argument, 0, 'e'},
                    {"routes", no_argument, 0, 'r'},
                    {0, 0, 0, 0}};

          int option_index = 0;

          int opt_val = 0;

          opterr = 0;

          /* Parse global transport and output flags before command dispatch begins. */

          while ((opt_val = getopt_long(getopt_argc, getopt_argv.data(), "+her", long_options, &option_index)) != -1)
          {
               if (opt_val == 0)
               {
                    std::string option_name = long_options[option_index].name;

                    if (option_name == "url")
                    {
                         base_url = optarg;
                         UpdateHostPortFromURL(base_url, host_val, port_val);
                         use_https_val = URLUsesHTTPS(base_url);
                    }
                    else if (option_name == "host")
                    {
                         host_val = optarg;
                         base_url = std::string(use_https_val ? "https://" : "http://") + host_val + ":" + std::to_string(port_val);
                    }
                    else if (option_name == "port")
                    {
                         try
                         {
                              port_val = std::stoi(optarg);
                              base_url = std::string(use_https_val ? "https://" : "http://") + host_val + ":" + std::to_string(port_val);
                         }
                         catch (...)
                         {
                              ConsoleWriter::WriteError("Error: Invalid port value: " + std::string(optarg) + ".", true);

                              return 1;
                         }
                    }
                    else if (option_name == "auth")
                    {
                         auth_token_val = optarg;
                    }
                    else if (option_name == "raw")
                    {
                         raw_mode_val = true;
                    }
                    else if (option_name == "dry-run-request")
                    {
                         request_dry_run = true;
                    }
                    else if (option_name == "curl")
                    {
                         request_dry_run = true;
                         request_dry_run_curl = true;
                    }
                    else if (option_name == "ssl-auth")
                    {
                         ssl_auth_val = true;
                         use_https_val = true;
                         base_url = "https://" + host_val + ":" + std::to_string(port_val);
                    }
                    else if (option_name == "timeout")
                    {
                         try
                         {
                              timeout_seconds_val = std::stoi(optarg);
                         }
                         catch (...)
                         {
                              ConsoleWriter::WriteError("Error: Invalid timeout value: " + std::string(optarg) + ".", true);

                              return 1;
                         }
                    }
               }
               else if (opt_val == 'h')
               {
                    HLQueryCLI cli_help(base_url, raw_mode_val, auth_token_val, program_name, ssl_auth_val);

                    cli_help.ShowHelp();

                    return 0;
               }
               else if (opt_val == 'e')
               {
                    HLQueryCLI cli_examples(base_url, raw_mode_val, auth_token_val, program_name, ssl_auth_val);

                    cli_examples.ShowExamples();

                    return 0;
               }
               else if (opt_val == 'r')
               {
                    HLQueryCLI cli_routes(base_url, raw_mode_val, auth_token_val, program_name, ssl_auth_val);

                    cli_routes.ShowRoutes();

                    return 0;
               }
               else
               {
                    continue;
               }
          }

          host_val = NormalizeHostValue(host_val);

          if (!IsValidHostValue(host_val))
          {
               ConsoleWriter::WriteError("Error: Invalid host value: " + host_val + ".", true);
               return 1;
          }

          if (!IsValidPortValue(port_val))
          {
               ConsoleWriter::WriteError("Error: Invalid port value: " + std::to_string(port_val) + ".", true);
               return 1;
          }

          if (ssl_auth_val)
          {
               use_https_val = true;
          }

          base_url = std::string(use_https_val ? "https://" : "http://") + host_val + ":" + std::to_string(port_val);

          for (int i = optind; i < getopt_argc; i++)
          {
               args_vec.push_back(getopt_argv[i]);
          }

          /* Show a short quick-start when no arguments are provided. */

          if (args_vec.empty())
          {
               std::cout << program_name << " v1.0.0 - Command Line Interface for hlquery search engine.\n";
               std::cout << "\n\n";
               std::cout << "Quick Start:.\n";
               std::cout << "  " << program_name << " cols                    # List collections\n";
               std::cout << "  " << program_name << " show cols               # List collections (alias)\n";
               std::cout << "  " << program_name << " info <collection>       # Show collection details\n";
               std::cout << "  " << program_name << " docs <collection>       # List documents\n";
               std::cout << "  " << program_name << " search <col> <query> [limit] [offset] [sort] [--all] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]    # Search one collection locally by default; add --all for cluster nodes\n";
               std::cout << "  " << program_name << " search --all <query> [limit] [offset] [sort] [--exact] [--highlight] [--collections=col1,col2] [--fields=f1,f2] [--maybe=min,limit] [--json]    # Search across all collections on master + cluster nodes\n";
               std::cout << "  " << program_name << " vector-search <col> <vector> [field] [limit] [--json]    # Vector similarity search in one collection\n";
               std::cout << "  " << program_name << " maybe <query> [collection] [limit] [min_results] [--json]    # Suggest likely intended phrases only when result count is low\n";
               std::cout << "  " << program_name << " colsearch <query> [limit] [offset] [sort] [--all] [--maybe=min,limit] [--json]    # Search collection names locally by default; add --all for cluster nodes\n";
               std::cout << "  " << program_name << " doctor                  # Run a compact readiness/health/config diagnosis\n";
               std::cout << "  " << program_name << " update <field> <col>/<doc> <value>    # Partially update one document field\n";
               std::cout << "  " << program_name << " migrate <source> <target> [--drop-old]    # Clone a collection into a new name and copy documents\n";
               std::cout << "  " << program_name << " stats                   # Check server stats\n";
               std::cout << "  " << program_name << " links                   # List distributed links\n";
               std::cout << "  " << program_name << " links ping              # Ping all distributed links\n";
               std::cout << "  " << program_name << " links search <col> <query> [opts]        # Search one collection across distributed links\n";
               std::cout << "  " << program_name << " links search --all <query> [opts]        # Search across all collections on distributed links\n";
               std::cout << "  " << program_name << " links colsearch <query> [opts]            # Distributed collection-name search\n";
               std::cout << "  " << program_name << " adv                     # Show advanced server information\n";
               std::cout << "  " << program_name << " uptime [-d]             # Show server uptime\n";
               std::cout << "  " << program_name << " tx [kb|mb|gb]           # Show total bytes transferred\n";
               std::cout << "  " << program_name << " lsm                    # Show deep LSM statistics\n";
               std::cout << "  " << program_name << " doctotal                # Show total documents and collections\n";
               std::cout << "  " << program_name << " flush                   # Flush all data (destructive)\n";
               std::cout << "  " << program_name << " modules [1|0]           # List loaded modules, core only with 1, optional only with 0\n";
               std::cout << "  " << program_name << " module info            # List loaded modules with routes\n";
               std::cout << "  " << program_name << " module <name> info      # Show module routes and parameter info\n";
               std::cout << "  " << program_name << " module <name> syntax    # Show module command syntax\n";
               std::cout << "  " << program_name << " module <name> <route> [--json] [--key=value] [args...]    # Run a module command\n";
               std::cout << "  " << program_name << " module <name> <free text prompt> [--json]    # For natural-language modules, bare text is passed as arguments when it is not a declared route\n";
               std::cout << "  " << program_name << " loadmodule <name>      # Load one runtime module\n";
               std::cout << "  " << program_name << " unloadmodule <name>    # Unload one runtime module\n";
               std::cout << "  " << program_name << " syn list                # Show synonym counts per collection\n";
               std::cout << "  " << program_name << " global syn list         # List global synonyms\n";
               std::cout << "  " << program_name << " stop list               # Show stopword counts per collection\n";
               std::cout << "  " << program_name << " global stop list        # List global stopwords\n\n";
               std::cout << "For detailed help and examples, run: " << program_name << " help.\n\n";

               return 0;
          }

          /* Build the CLI instance with the resolved connection settings. */

          HLQueryCLI cli_instance(base_url, raw_mode_val, auth_token_val, program_name, ssl_auth_val);
          cli_instance.SetDefaultTimeoutSeconds(timeout_seconds_val);

          std::vector<std::string> filtered_args;
          filtered_args.reserve(args_vec.size());

          for (const auto &arg : args_vec)
          {
               if (arg == "--dry-run-request")
               {
                    request_dry_run = true;
                    continue;
               }

               if (arg == "--curl")
               {
                    request_dry_run = true;
                    request_dry_run_curl = true;
                    continue;
               }

               filtered_args.push_back(arg);
          }

          args_vec = std::move(filtered_args);
          cli_instance.SetRequestDryRunMode(request_dry_run, request_dry_run_curl);

          /* The first positional argument is always the command. */

          std::string command_str = args_vec[0];

          if (command_str == "show" && args_vec.size() >= 2 && (args_vec[1] == "cols" || args_vec[1] == "collections"))
          {
               args_vec.erase(args_vec.begin());
               command_str = "cols";
          }

          /* Dispatch CLI commands to their handlers. */

          if (command_str == "lang")
          {
               bool json_output_flag = false;
               std::vector<std::string> positional;

               for (size_t i = 1; i < args_vec.size(); i++)
               {
                    if (args_vec[i] == "--json")
                    {
                         json_output_flag = true;
                         continue;
                    }

                    positional.push_back(args_vec[i]);
               }

               if (positional.size() != 1)
               {
                    ConsoleWriter::WriteError("Usage: " + program_name + " lang <collection> [--json].", true);
                    return 1;
               }

               cli_instance.ShowCollectionLanguage(positional[0], json_output_flag);
               return cli_instance.GetExitCode();
          }

          if (command_str == "cols")
          {
               /* cols accepts both explicit flags and the older positional offset/limit form. */

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
               cli_instance.ShowInfo(args_vec[1]);
          }
          else if (command_str == "docs" && args_vec.size() >= 2)
          {
               int offset_val = 0;
               int limit_val = 20;

               if (args_vec.size() == 3)
               {
                    try
                    {
                         limit_val = std::stoi(args_vec[2]);
                    }
                    catch (...)
                    {
                         std::cout << "Error: Invalid limit: " << args_vec[2] << ".\n"
                                   << std::endl;

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
                         std::cout << "Error: Invalid offset or limit.\n"
                                   << std::endl;

                         return 1;
                    }
               }

               cli_instance.ListDocuments(args_vec[1], offset_val, limit_val);
          }
          else if (command_str == "stats")
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
          else if (command_str == "links" || command_str == "llinks")
          {
               /* Distributed commands mirror local search syntax but force distributed mode on. */

               if (args_vec.size() >= 2 && (args_vec[1] == "ping" || args_vec[1] == "check"))
               {
                    cli_instance.ShowLinks(true);
               }
               else if (args_vec.size() >= 3 && args_vec[1] == "search")
               {
                    SearchCLIOptions opts;
                    std::string collection_str;
                    std::string query_str;

                    if (args_vec[2] == "--all")
                    {
                         if (args_vec.size() < 4)
                         {
                              ConsoleWriter::WriteError("Error: 'links search --all' requires a query.", true);
                              ConsoleWriter::WriteError("Usage: " + program_name + " links search --all <query> [limit] [offset] [sort] [--exact] [--highlight] [--collections=col1,col2] [--fields=f1,f2] [--maybe=min,limit].", true);
                              return 1;
                         }

                         query_str = args_vec[3];
                         ParseSearchCLIOptions(args_vec, 4, opts);
                         opts.All = true;
                    }
                    else
                    {
                         if (args_vec.size() < 4)
                         {
                              ConsoleWriter::WriteError("Error: 'links search' requires collection and query.", true);
                              ConsoleWriter::WriteError("Usage: " + program_name + " links search <col> <query> [limit] [offset] [sort] [--all] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit].", true);
                              return 1;
                         }

                         collection_str = args_vec[2];
                         query_str = args_vec[3];
                         ParseSearchCLIOptions(args_vec, 4, opts);
                    }

                    if (opts.Distributed.empty())
                    {
                         opts.Distributed = "on";
                    }

                    if (opts.All)
                    {
                         cli_instance.SearchAcrossCollections(query_str, opts.Collections, opts.Limit, opts.Offset, opts.Sort, opts.ExactMatch, opts.Highlight, opts.HighlightFields, opts.Distributed, opts.Route, true, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
                    }
                    else
                    {
                         cli_instance.SearchDocuments(collection_str, query_str, opts.Limit, opts.Offset, opts.Sort, opts.ExactMatch, opts.Highlight, opts.HighlightFields, opts.Distributed, opts.Route, 30, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
                    }
               }
               else if (args_vec.size() >= 3 && args_vec[1] == "colsearch")
               {
                    std::string query_str = args_vec[2];

                    SearchCLIOptions opts;
                    ParseSearchCLIOptions(args_vec, 3, opts);
                    if (opts.Distributed.empty())
                    {
                         opts.Distributed = "on";
                    }

                    cli_instance.SearchCollections(query_str, opts.Limit, opts.Offset, opts.Sort, opts.Distributed, opts.Route, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
               }
               else
               {
                    cli_instance.ShowLinks(false);
               }
          }
          else if (command_str == "sam")
          {
               if (args_vec.size() >= 2 && args_vec[1] == "search")
               {
                    SearchCLIOptions opts;
                    std::string collection_str;
                    std::string query_str;

                    if (args_vec.size() >= 3 && args_vec[2] == "--all")
                    {
                         if (args_vec.size() < 4)
                         {
                              ConsoleWriter::WriteError("Error: 'sam search --all' requires a query.", true);
                              ConsoleWriter::WriteError("Usage: " + program_name + " sam search --all <query> [limit] [--collections=col1,col2] [--distributed=on|off] [--route=local|host[:port]] [--skip] [--json].", true);
                              return 1;
                         }

                         query_str = args_vec[3];
                         ParseSearchCLIOptions(args_vec, 4, opts);
                         opts.All = true;
                    }
                    else
                    {
                         if (args_vec.size() < 4)
                         {
                              ConsoleWriter::WriteError("Error: 'sam search' requires collection and query.", true);
                              ConsoleWriter::WriteError("Usage: " + program_name + " sam search <collection> <query> [limit] [--all] [--distributed=on|off] [--route=local|host[:port]] [--skip] [--json].", true);
                              return 1;
                         }

                         collection_str = args_vec[2];
                         query_str = args_vec[3];
                         ParseSearchCLIOptions(args_vec, 4, opts);
                    }

                    cli_instance.SearchSAM(collection_str,
                                           query_str,
                                           opts.Limit,
                                           opts.JsonOutput,
                                           opts.All,
                                           opts.Collections,
                                           opts.Distributed,
                                           opts.Route,
                                           opts.Skip);
               }
               else
               {
                    ConsoleWriter::WriteError("Usage: " + program_name + " sam search <collection> <query> [limit] [--all] [--collections=col1,col2] [--distributed=on|off] [--route=local|host[:port]] [--skip] [--json].", true);
                    ConsoleWriter::WriteError("   or: " + program_name + " sam search --all <query> [limit] [--collections=col1,col2] [--distributed=on|off] [--route=local|host[:port]] [--skip] [--json].", true);
                    return 1;
               }
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
                              ConsoleWriter::WriteError("Error: Invalid offset: " + args_vec[i] + ".", true);

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
                              ConsoleWriter::WriteError("Error: Invalid limit: " + args_vec[i] + ".", true);

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
          else if (command_str == "modules")
          {
               if (args_vec.size() > 2)
               {
                    ConsoleWriter::WriteError("Error: 'modules' accepts at most one optional filter argument.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " modules [1|0].", true);
                    return 1;
               }

               cli_instance.ListModules(args_vec.size() == 2 ? args_vec[1] : "");
          }
          else if (command_str == "session" || command_str == "sessions")
          {
               std::string module_name = "llama";
               std::string action = "list";
               size_t cursor = 1;

               if (cursor < args_vec.size() &&
                   args_vec[cursor] == "llama")
               {
                    module_name = args_vec[cursor];
                    ++cursor;
               }

               if (cursor < args_vec.size() && args_vec[cursor].rfind("--", 0) != 0)
               {
                    action = args_vec[cursor];
                    std::transform(action.begin(), action.end(), action.begin(),
                                   [](unsigned char C)
                                   {
                                        return static_cast<char>(std::tolower(C));
                                   });
                    ++cursor;
               }

               std::vector<std::string> session_args;
               session_args.push_back(action);
               for (size_t i = cursor; i < args_vec.size(); ++i)
               {
                    session_args.push_back(args_vec[i]);
               }

               cli_instance.RunModuleCommand(module_name, "session", session_args);
          }
          else if (command_str == "module")
          {
               /* module supports metadata lookup, explicit routes, and free-text prompts. */

               if (args_vec.size() < 2)
               {
                    ConsoleWriter::WriteError("Error: 'module' command requires a module name.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " module info | module <name> [info|syntax|<route>|<free text>] [--key=value] [args...].", true);
                    return 1;
               }

               const std::string module_name = args_vec[1];

               if (module_name == "info" && args_vec.size() == 2)
               {
                    cli_instance.ListModules();
               }
               else if (args_vec.size() == 2 && module_name == "dump")
               {
                    cli_instance.RunModuleCommand("dump", "dump", {});
               }
               else if (args_vec.size() == 2 || args_vec[2] == "syntax" || args_vec[2] == "info")
               {
                    cli_instance.ShowModuleSyntax(module_name);
               }
               else
               {
                    const std::string route_str = args_vec[2];
                    std::vector<std::string> module_args;

                    for (size_t i = 3; i < args_vec.size(); ++i)
                    {
                         module_args.push_back(args_vec[i]);
                    }

                    cli_instance.RunModuleCommand(module_name, route_str, module_args);
               }
          }
          else if (command_str == "loadmodule")
          {
               if (args_vec.size() != 2)
               {
                    ConsoleWriter::WriteError("Error: 'loadmodule' command requires a module name.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " loadmodule <name>.", true);
                    return 1;
               }

               cli_instance.LoadModule(args_vec[1]);
          }
          else if (command_str == "unloadmodule")
          {
               if (args_vec.size() != 2)
               {
                    ConsoleWriter::WriteError("Error: 'unloadmodule' command requires a module name.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " unloadmodule <name>.", true);
                    return 1;
               }

               cli_instance.UnloadModule(args_vec[1]);
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
                    ConsoleWriter::WriteError("Error: 'create' command requires a collection name and at least one searchable field.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " create <collection> <field1> [field2 ...].", true);

                    return 1;
               }

               std::vector<std::string> searchable_fields_vec(args_vec.begin() + 2, args_vec.end());

               cli_instance.CreateCollection(args_vec[1], searchable_fields_vec);
          }
          else if (command_str == "migrate")
          {
               if (args_vec.size() < 3)
               {
                    ConsoleWriter::WriteError("Error: 'migrate' command requires source and target collection names.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " migrate <source> <target> [--drop-old].", true);
                    return 1;
               }

               bool drop_old_flag = false;

               for (size_t i = 3; i < args_vec.size(); ++i)
               {
                    if (args_vec[i] == "--drop-old")
                    {
                         drop_old_flag = true;
                    }
               }

               cli_instance.MigrateCollection(args_vec[1], args_vec[2], drop_old_flag);
          }
          else if (command_str == "delete")
          {
               if (args_vec.size() < 2)
               {
                    ConsoleWriter::WriteError("Error: 'delete' command requires a collection name.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " delete <collection> [document_id] [--filter=<query>].", true);
                    ConsoleWriter::WriteError("       " + program_name + " delete <collection>/<document_id>.", true);

                    return 1;
               }

               std::string collection_str = args_vec[1];

               if (args_vec.size() == 2)
               {
                    size_t slash_pos = collection_str.find('/');

                    if (slash_pos != std::string::npos && slash_pos > 0 && slash_pos + 1 < collection_str.size())
                    {
                         std::string parsed_collection_str = collection_str.substr(0, slash_pos);
                         std::string parsed_document_id_str = collection_str.substr(slash_pos + 1);

                         cli_instance.DeleteDocument(parsed_collection_str, parsed_document_id_str);
                    }
                    else
                    {
                         cli_instance.DeleteCollection(collection_str);
                    }
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
                    ConsoleWriter::WriteError("Error: 'add' command requires collection, id, title, and content.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " add <col> <id> <title> <content> [field=value...].", true);

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
               /* search supports one-collection mode and cluster-wide --all mode. */

               SearchCLIOptions opts;
               std::string collection_str;
               std::string query_str;
               std::string server_query_str;
               size_t options_start_index = 0;
               bool phrase_query = false;

               if (args_vec.size() >= 2 && args_vec[1] == "--all")
               {
                    if (!ExtractSearchCLIQuery(args_vec, 2, query_str, options_start_index, phrase_query))
                    {
                         ConsoleWriter::WriteError("Error: 'search --all' requires a query.", true);
                         ConsoleWriter::WriteError("Usage: " + program_name + " search --all <query> [limit] [offset] [sort] [--exact] [--highlight] [--collections=col1,col2] [--fields=f1,f2] [--maybe=min,limit].", true);
                         return 1;
                    }

                    ParseSearchCLIOptions(args_vec, options_start_index, opts);
                    opts.All = true;
                }
               else if (args_vec.size() >= 3)
               {
                    collection_str = args_vec[1];

                    if (!ExtractSearchCLIQuery(args_vec, 2, query_str, options_start_index, phrase_query))
                    {
                         ConsoleWriter::WriteError("Error: 'search' command requires a query.", true);
                         ConsoleWriter::WriteError("Usage: " + program_name + " search <col> <query> [limit] [offset] [sort] [--all] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit].", true);
                         return 1;
                    }

                    ParseSearchCLIOptions(args_vec, options_start_index, opts);
               }
               else
               {
                    ConsoleWriter::WriteError("Error: 'search' command requires collection and query.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " search <col> <query> [limit] [offset] [sort] [--all] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit].", true);
                    ConsoleWriter::WriteError("   or: " + program_name + " search --all <query> [limit] [offset] [sort] [--exact] [--highlight] [--collections=col1,col2] [--fields=f1,f2] [--maybe=min,limit].", true);

                    return 1;
               }

               server_query_str = BuildCLIQueryForServer(query_str, phrase_query);

               if (opts.All)
               {
                    if (opts.Distributed.empty())
                    {
                         opts.Distributed = "on";
                    }

                    if (!collection_str.empty())
                    {
                         opts.Collections = {collection_str};
                    }

                    cli_instance.SearchAcrossCollections(server_query_str, opts.Collections, opts.Limit, opts.Offset, opts.Sort, opts.ExactMatch, opts.Highlight, opts.HighlightFields, opts.Distributed, opts.Route, true, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
                    return 0;
               }

               if (opts.Distributed.empty())
               {
                    opts.Distributed = "off";
               }

               cli_instance.SearchDocuments(collection_str, server_query_str, opts.Limit, opts.Offset, opts.Sort, opts.ExactMatch, opts.Highlight, opts.HighlightFields, opts.Distributed, opts.Route, 30, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
          }
          else if (command_str == "maybe")
          {
               /* maybe can consume values from argv or fall back to config-driven defaults. */

               bool json_output_flag = false;
               std::vector<std::string> positional_args;

               for (size_t i = 1; i < args_vec.size(); ++i)
               {
                    if (args_vec[i] == "--json")
                    {
                         json_output_flag = true;
                         continue;
                    }

                    positional_args.push_back(args_vec[i]);
               }

               std::string query_str = cli_maybe_defaults.Query;
               std::string collection_str = cli_maybe_defaults.Collection;
               int limit_val = cli_maybe_defaults.Limit;
               int min_results_val = cli_maybe_defaults.MinResults;

               if (positional_args.size() >= 1)
               {
                    query_str = positional_args[0];
               }

               if (positional_args.size() >= 2)
               {
                    collection_str = positional_args[1];
               }

               if (positional_args.size() >= 2)
               {
                    /* Swap collection and query when the first argument is clearly a collection name. */

                    const bool first_is_collection = cli_instance.CollectionExists(positional_args[0]);
                    const bool second_is_collection = cli_instance.CollectionExists(positional_args[1]);

                    if (first_is_collection && !second_is_collection)
                    {
                         collection_str = positional_args[0];
                         query_str = positional_args[1];
                    }
               }

               if (positional_args.size() >= 3)
               {
                    try
                    {
                         limit_val = std::stoi(positional_args[2]);
                    }
                    catch (...)
                    {
                         ConsoleWriter::WriteError("Error: Invalid limit: " + positional_args[2] + ".", true);
                         return 1;
                    }
               }

               if (positional_args.size() >= 4)
               {
                    try
                    {
                         min_results_val = std::stoi(positional_args[3]);
                    }
                    catch (...)
                    {
                         ConsoleWriter::WriteError("Error: Invalid min_results: " + positional_args[3] + ".", true);
                         return 1;
                    }
               }

               if (query_str.empty())
               {
                    ConsoleWriter::WriteError("Error: 'maybe' command requires a query or a configured <cli_maybe query=\"...\"> default.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " maybe <query> [collection] [limit] [min_results].", true);
                    return 1;
               }

               if (collection_str.empty())
               {
                    ConsoleWriter::WriteError("Error: 'maybe' command requires a collection or a configured <cli_maybe collection=\"...\"> default.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " maybe <query> [collection] [limit] [min_results].", true);
                    return 1;
               }

               cli_instance.MaybeSuggest(query_str, collection_str, limit_val, min_results_val, json_output_flag);
          }
          else if (command_str == "colsearch")
          {
               if (args_vec.size() < 2)
               {
                    ConsoleWriter::WriteError("Error: 'colsearch' command requires a query.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " colsearch <query> [limit] [offset] [sort] [--all] [--maybe=min,limit].", true);

                    return 1;
               }

               std::string query_str = args_vec[1];

               SearchCLIOptions opts;
               ParseSearchCLIOptions(args_vec, 2, opts);
               if (opts.Distributed.empty())
               {
                    opts.Distributed = opts.All ? "on" : "off";
               }
               cli_instance.SearchCollections(query_str, opts.Limit, opts.Offset, opts.Sort, opts.Distributed, opts.Route, opts.MaybeMin, opts.MaybeLimit, opts.JsonOutput);
          }
          else if (command_str == "vector-search")
          {
               /* vector-search keeps field and limit optional after the vector argument. */

               if (args_vec.size() < 3)
               {
                    ConsoleWriter::WriteError("Error: 'vector-search' command requires collection and vector.", true);
                    ConsoleWriter::WriteError("Usage: " + program_name + " vector-search <col> <vector> [field] [limit].", true);

                    return 1;
               }

               std::string col_str = args_vec[1];
               std::string vector_str = args_vec[2];
               bool json_output_flag = false;
               std::vector<std::string> positional_args;

               for (size_t i = 3; i < args_vec.size(); ++i)
               {
                    if (args_vec[i] == "--json")
                    {
                         json_output_flag = true;
                         continue;
                    }

                    positional_args.push_back(args_vec[i]);
               }

               std::string field_str = positional_args.size() >= 1 ? positional_args[0] : "embedding";
               int limit_val = positional_args.size() >= 2 ? std::stoi(positional_args[1]) : 10;

               cli_instance.VectorSearch(col_str, vector_str, field_str, limit_val, json_output_flag);
          }
          else if (command_str == "open" && args_vec.size() >= 2)
          {
               /* open resolves a collection/document pair and chooses the requested output format. */

               std::string format_val = "table";
               std::string col_doc_str = args_vec[1];
               std::string route_val;

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
                    else if (args_vec[i].rfind("--route=", 0) == 0)
                    {
                         route_val = TrimWhitespace(args_vec[i].substr(8));
                    }
               }

               size_t slash_pos = col_doc_str.find('/');

               if (slash_pos == std::string::npos)
               {
                    ConsoleWriter::WriteError("Error: Invalid format. Use: open <collection>/<document_id> [--table|--json|--text]", true);
                    return 1;
               }

               std::string collection_str = col_doc_str.substr(0, slash_pos);
               std::string document_id_str = col_doc_str.substr(slash_pos + 1);

               cli_instance.OpenDocument(collection_str, document_id_str, format_val, route_val);
          }
          else if (command_str == "select" && args_vec.size() >= 3)
          {
               std::string field_name_str = args_vec[1];
               std::string col_doc_str = args_vec[2];

               size_t slash_pos = col_doc_str.find('/');

               if (slash_pos == std::string::npos)
               {
                    ConsoleWriter::WriteError("Error: Invalid format. Use: select <field> <collection>/<document_id>", true);
                    return 1;
               }

               std::string collection_str = col_doc_str.substr(0, slash_pos);
               std::string document_id_str = col_doc_str.substr(slash_pos + 1);

               cli_instance.SelectField(field_name_str, collection_str, document_id_str);
          }
          else if (command_str == "update" && args_vec.size() >= 4)
          {
               std::string field_name_str = args_vec[1];
               std::string col_doc_str = args_vec[2];
               std::string field_value_str = args_vec[3];

               size_t slash_pos = col_doc_str.find('/');

               if (slash_pos == std::string::npos)
               {
                    ConsoleWriter::WriteError("Error: Invalid format. Use: update <field> <collection>/<document_id> <value>", true);
                    return 1;
               }

               std::string collection_str = col_doc_str.substr(0, slash_pos);
               std::string document_id_str = col_doc_str.substr(slash_pos + 1);

               cli_instance.UpdateDocumentField(collection_str, document_id_str, field_name_str, field_value_str);
          }
          else if (command_str == "health")
          {
               /* health treats both /health and /stats as required indicators of server readiness. */

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
                    ConsoleWriter::WriteError("✗ Server health check failed", true);

                    if (!healthy)
                    {
                         ConsoleWriter::WriteError("  /health returned: " + std::to_string(health_resp.StatusCode), false);
                    }

                    if (!stats_ok)
                    {
                         ConsoleWriter::WriteError("  /stats returned: " + std::to_string(stats_resp.StatusCode), false);
                    }

                   return 2;
              }
          }
          else if (command_str == "doctor")
          {
               cli_instance.ShowDoctor();
               return cli_instance.GetExitCode();
          }
          else if (command_str == "verify")
          {
               /* verify runs a lightweight consistency sweep against key read endpoints. */

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

                              for (const auto &col : cols_json["collections"])
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
                         ConsoleWriter::WriteError("  Error sampling documents", true);
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
               /* benchmark-state compares collection list counters with collection detail counters. */

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

                              for (const auto &col : cols_json["collections"])
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
                         ConsoleWriter::WriteError("Error parsing collections", true);
                         return 2;
                    }
               }
          }
          else if (command_str == "global" && args_vec.size() >= 3)
          {
               if (args_vec[1] == "syn" || args_vec[1] == "syns")
               {
                    if (args_vec[2] == "list")
                    {
                         cli_instance.ListSynonyms("global");
                    }
                    else if (args_vec[2] == "add" && args_vec.size() >= 6)
                    {
                         std::string id_str = args_vec[3];
                         std::string root_str = args_vec[4];
                         std::vector<std::string> syns_vec;
                         std::istringstream ss(args_vec[5]);
                         std::string s_str;
                         while (std::getline(ss, s_str, ','))
                         {
                              syns_vec.push_back(s_str);
                         }
                         cli_instance.AddSynonym("global", id_str, root_str, syns_vec);
                    }
                    else if ((args_vec[2] == "del" || args_vec[2] == "delete") && args_vec.size() >= 4)
                    {
                         cli_instance.DeleteSynonym("global", args_vec[3]);
                    }
               }
               else if (args_vec[1] == "stop" || args_vec[1] == "stopwords")
               {
                    if (args_vec[2] == "list")
                    {
                         cli_instance.ListStopwords("global");
                    }
                    else if (args_vec[2] == "add" && args_vec.size() >= 4)
                    {
                         cli_instance.AddStopword("global", args_vec[3]);
                    }
                    else if ((args_vec[2] == "del" || args_vec[2] == "delete") && args_vec.size() >= 4)
                    {
                         cli_instance.DeleteStopword("global", args_vec[3]);
                    }
               }
          }
          else if (command_str == "syn" || command_str == "syns" || command_str == "addsyn")
          {
               if (command_str == "addsyn")
               {
                    if (args_vec.size() < 5)
                    {
                         ConsoleWriter::WriteError("Error: 'addsyn' requires collection, id, root, and synonyms.", true);
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
                         ConsoleWriter::WriteError("Error: 'addstop' requires collection and word.", true);
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
               /* keys groups create, update, delete, and list operations under one command root. */

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
                         if (args_vec[i].substr(0, 7) == "--desc=")
                         {
                              desc = args_vec[i].substr(7);
                         }
                         else if (args_vec[i].substr(0, 7) == "--cols=")
                         {
                              std::string c_str = args_vec[i].substr(7);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   cols.push_back(s);
                              }
                         }
                         else if (args_vec[i].substr(0, 10) == "--actions=")
                         {
                              std::string a_str = args_vec[i].substr(10);
                              std::istringstream ss(a_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   actions.push_back(s);
                              }
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

                    if (actions.empty())
                    {
                         actions = {"search"};
                    }

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
                         if (args_vec[i].substr(0, 7) == "--desc=")
                         {
                              desc = args_vec[i].substr(7);
                         }
                         else if (args_vec[i].substr(0, 7) == "--cols=")
                         {
                              std::string c_str = args_vec[i].substr(7);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   cols.push_back(s);
                              }
                         }
                         else if (args_vec[i].substr(0, 11) == "--add-cols=")
                         {
                              std::string c_str = args_vec[i].substr(11);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   add_cols.push_back(s);
                              }
                         }
                         else if (args_vec[i].substr(0, 14) == "--remove-cols=")
                         {
                              std::string c_str = args_vec[i].substr(14);
                              std::istringstream ss(c_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   remove_cols.push_back(s);
                              }
                         }
                         else if (args_vec[i].substr(0, 10) == "--actions=")
                         {
                              std::string a_str = args_vec[i].substr(10);
                              std::istringstream ss(a_str);
                              std::string s;
                              while (std::getline(ss, s, ','))
                              {
                                   actions.push_back(s);
                              }
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
                    std::cout << "Usage: " << program_name << " keys list\n";
                    std::cout << "       " << program_name << " keys create [--desc=...] [--cols=c1,c2] [--actions=a1,a2] [--hanalyzer]\n";
                    std::cout << "       " << program_name << " keys delete <id>\n";
                    std::cout << "       " << program_name << " keys update <id> [--desc=...] [--cols=c1,c2] [--actions=a1,a2] [--hanalyzer] [--add-cols=c1,c2] [--remove-cols=c1,c2]\n";
               }
          }
          else if (command_str == "help")
          {
               cli_instance.ShowHelp();
          }
          else
          {
               ConsoleWriter::WriteError("Unknown command: " + command_str + ".", true);
               ConsoleWriter::WriteError("Run '" + program_name + " help' for usage info.", true);

               return 1;
          }

          return cli_instance.GetExitCode();
     }
     catch (const std::exception &e)
     {
          ConsoleWriter::WriteError("Error: " + std::string(e.what()) + ".", true);

          return 1;
     }
}
