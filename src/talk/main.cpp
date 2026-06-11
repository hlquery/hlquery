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
 * hlquery talk - interactive command shell for lightweight server inspection.
 */

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <set>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>
#include <vector>

#include "cli/app.h"
#include "cli/cliutils.h"
#include "core/helpers.h"
#include "talk/session.h"
#include "vendor/json/json.hpp"

std::string ToLower(std::string value);
bool IsBuiltinTalkCommand(const std::string &command);
bool IsUnsignedInteger(const std::string &value);
bool ResolveAliasedTalkTarget(const TalkState &state,
                              HLQueryCLI &cli,
                              const std::string &command,
                              std::string &resolved_target,
                              bool &target_is_collection,
                              std::string &error_message);
bool ExecuteTalkCommand(const std::string &line,
                        HLQueryCLI &cli,
                        TalkState &state,
                        std::string &host,
                        std::string &port);

/* Search command runtime flags. */

struct SearchTalkOptions
{
     int Limit = 10000;
     int Offset = 0;
     std::string Sort;
     std::string Route;
     bool ExactMatch = false;
     bool Highlight = false;
     bool JsonOutput = false;
     std::string HighlightFields;
     std::string Distributed;
     int MaybeMin = -1;
     int MaybeLimit = -1;
};

static void PruneAliasesForDeletedCollection(TalkState &state, const std::string &collection_name)
{
     const std::string lowered_collection = ToLower(collection_name);
     std::unordered_set<std::string> removed_targets;
     removed_targets.insert(lowered_collection);

     bool changed = true;

     while (changed)
     {
          changed = false;

          for (auto it = state.CommandAliases.begin(); it != state.CommandAliases.end();)
          {
               const std::string alias_target = ToLower(it->second);

               if (removed_targets.find(alias_target) != removed_targets.end())
               {
                    removed_targets.insert(ToLower(it->first));
                    it = state.CommandAliases.erase(it);
                    changed = true;
                    continue;
               }

               ++it;
          }
     }
}

static bool ResolveTalkAliasCollection(const TalkState &state,
                                      HLQueryCLI &cli,
                                      const std::string &name,
                                      std::string &resolved_collection_name)
{
     const std::string lowered = ToLower(name);
     const auto alias_it = state.CommandAliases.find(lowered);

     if (alias_it == state.CommandAliases.end())
     {
          return false;
     }

     std::string resolved_target;
     bool target_is_collection = false;
     std::string error_message;

     if (!ResolveAliasedTalkTarget(state, cli, lowered, resolved_target, target_is_collection, error_message))
     {
          return false;
     }

     if (!target_is_collection)
     {
          return false;
     }

     resolved_collection_name = resolved_target;
     return true;
}

static std::string RewriteSQLFromAlias(const TalkState &state, HLQueryCLI &cli, const std::string &sql)
{
     std::string lowered;
     lowered.reserve(sql.size());

     for (char c : sql)
     {
          lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
     }

     const std::string needle = " from ";
     const size_t from_pos = lowered.find(needle);

     if (from_pos == std::string::npos)
     {
          return sql;
     }

     size_t name_start = from_pos + needle.size();

     while (name_start < sql.size() && std::isspace(static_cast<unsigned char>(sql[name_start])) != 0)
     {
          ++name_start;
     }

     size_t name_end = name_start;

     while (name_end < sql.size())
     {
          const unsigned char current = static_cast<unsigned char>(sql[name_end]);

          if (std::isalnum(current) != 0 || sql[name_end] == '_' || sql[name_end] == '-' )
          {
               ++name_end;
               continue;
          }

          break;
     }

     if (name_end <= name_start)
     {
          return sql;
     }

     const std::string from_name = sql.substr(name_start, name_end - name_start);
     std::string resolved_collection;

     if (!ResolveTalkAliasCollection(state, cli, from_name, resolved_collection))
     {
          return sql;
     }

     std::string rewritten = sql;
     rewritten.replace(name_start, name_end - name_start, resolved_collection);
     return rewritten;
}

static bool ParseRunCommand(const std::string &line,
                            int &repeat_count,
                            std::string &nested_command,
                            std::string &error_message)
{
     repeat_count = 0;
     nested_command.clear();
     error_message.clear();

     std::istringstream parser(line);
     std::string command_name;
     std::string count_text;

     if (!(parser >> command_name >> count_text))
     {
          error_message = "Usage: run <count> <command>";
          return false;
     }

     if (!IsUnsignedInteger(count_text))
     {
          error_message = "Run count must be a positive integer";
          return false;
     }

     repeat_count = std::stoi(count_text);

     if (repeat_count <= 0)
     {
          error_message = "Run count must be greater than 0";
          return false;
     }

     std::string remaining;
     std::getline(parser, remaining);
     nested_command = TrimWhitespace(remaining);

     if (nested_command.empty())
     {
          error_message = "Usage: run <count> <command>";
          return false;
     }

     return true;
}

std::string ResolveHistoryFilePath()
{
     namespace fs = std::filesystem;

     std::error_code error;
     const char *xdg_state_home = std::getenv("XDG_STATE_HOME");

     if (xdg_state_home != nullptr && *xdg_state_home != '\0')
     {
          const fs::path history_dir = fs::path(xdg_state_home) / "hlquery";
          fs::create_directories(history_dir, error);

          if (!error)
          {
               return (history_dir / "talk_history").string();
          }

          error.clear();
     }

     const char *home = std::getenv("HOME");

     if (home != nullptr && *home != '\0')
     {
          const fs::path history_dir = fs::path(home) / ".local" / "state" / "hlquery";
          fs::create_directories(history_dir, error);

          if (!error)
          {
               return (history_dir / "talk_history").string();
          }
     }

     return ".talk_history";
}

std::string ResolveAliasesFilePath()
{
     namespace fs = std::filesystem;

     std::error_code error;
     const char *xdg_state_home = std::getenv("XDG_STATE_HOME");

     if (xdg_state_home != nullptr && *xdg_state_home != '\0')
     {
          const fs::path state_dir = fs::path(xdg_state_home) / "hlquery";
          fs::create_directories(state_dir, error);

          if (!error)
          {
               return (state_dir / "talk_aliases").string();
          }

          error.clear();
     }

     const char *home = std::getenv("HOME");

     if (home != nullptr && *home != '\0')
     {
          const fs::path state_dir = fs::path(home) / ".local" / "state" / "hlquery";
          fs::create_directories(state_dir, error);

          if (!error)
          {
               return (state_dir / "talk_aliases").string();
          }
     }

     return ".talk_aliases";
}

void LoadTalkAliases(TalkState &state)
{
     const std::string alias_file = ResolveAliasesFilePath();

     std::ifstream input(alias_file);

     if (!input.is_open())
     {
          return;
     }

     std::string line;

     while (std::getline(input, line))
     {
          line = TrimWhitespace(line);

          if (line.empty() || line.front() == '#')
          {
               continue;
          }

          std::istringstream parser(line);
          std::string alias_name;
          std::string target_name;

          if (!(parser >> alias_name >> target_name))
          {
               continue;
          }

          alias_name = ToLower(alias_name);
          target_name = ToLower(target_name);

          if (alias_name.empty() || target_name.empty())
          {
               continue;
          }

          if (IsBuiltinTalkCommand(alias_name))
          {
               continue;
          }

          state.CommandAliases[alias_name] = target_name;
     }
}

void SaveTalkAliases(const TalkState &state)
{
     const std::string alias_file = ResolveAliasesFilePath();
     std::ofstream output(alias_file, std::ios::trunc);

     if (!output.is_open())
     {
          return;
     }

     std::set<std::string> alias_names;

     for (const auto &entry : state.CommandAliases)
     {
          alias_names.insert(entry.first);
     }

     for (const std::string &alias_name : alias_names)
     {
          const auto existing_alias = state.CommandAliases.find(alias_name);

          if (existing_alias == state.CommandAliases.end())
          {
               continue;
          }

          if (existing_alias->first.empty() || existing_alias->second.empty())
          {
               continue;
          }

          output << existing_alias->first << " " << existing_alias->second << "\n";
     }
}

void TalkPrintLine(const std::string &message)
{
     if (message.empty())
     {
          return;
     }

     std::cout << message;

     if (message.back() != '.')
     {
          std::cout << '.';
     }

     newline();
}

void TalkPrintError(const std::string &message)
{
     TalkPrintLine(message);
}

void TalkPrintInfo(const std::string &message)
{
     TalkPrintLine(message);
}

void TalkPrintSuccess(const std::string &message)
{
     TalkPrintLine(message);
}

/* Trim leading and trailing ASCII whitespace. */

std::string TrimWhitespace(const std::string &value)
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

     return value.substr(start, end - start);
}

std::string NormalizeHostValue(const std::string &value)
{
     std::string normalized = TrimWhitespace(value);

     std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
                    { return static_cast<char>(std::tolower(character)); });

     return normalized;
}

bool IsValidPortValue(int port)
{
     return port > 0 && port <= 65535;
}

bool IsValidIPAddress(const std::string &value)
{
     sockaddr_in sa4{};
     sockaddr_in6 sa6{};

     return inet_pton(AF_INET, value.c_str(), &sa4) == 1 ||
            inet_pton(AF_INET6, value.c_str(), &sa6) == 1;
}

bool IsValidHostnameValue(const std::string &value)
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

bool IsValidHostValue(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     return IsValidIPAddress(value) || IsValidHostnameValue(value);
}

std::string BuildBaseURL(const std::string &host, const std::string &port)
{
     return "http://" + host + ":" + port;
}

bool ParseEndpointValue(const std::string &value, std::string &host_out, std::string &port_out)
{
     host_out.clear();
     port_out.clear();

     std::string normalized = TrimWhitespace(value);
     if (normalized.empty())
     {
          return false;
     }

     const std::string http_prefix = "http://";
     const std::string https_prefix = "https://";
     if (normalized.rfind(http_prefix, 0) == 0)
     {
          normalized = normalized.substr(http_prefix.size());
     }
     else if (normalized.rfind(https_prefix, 0) == 0)
     {
          normalized = normalized.substr(https_prefix.size());
     }

     const size_t slash_pos = normalized.find('/');
     if (slash_pos != std::string::npos)
     {
          normalized = normalized.substr(0, slash_pos);
     }

     const size_t colon_pos = normalized.rfind(':');
     if (colon_pos == std::string::npos)
     {
          return false;
     }

     host_out = NormalizeHostValue(normalized.substr(0, colon_pos));
     port_out = TrimWhitespace(normalized.substr(colon_pos + 1));

     if (host_out.empty() || port_out.empty() || !IsValidHostValue(host_out))
     {
          return false;
     }

     try
     {
          const int port_value = std::stoi(port_out);
          if (!IsValidPortValue(port_value))
          {
               return false;
          }

          port_out = std::to_string(port_value);
     }
     catch (...)
     {
          return false;
     }

     return true;
}

bool IsHealthyTalkEndpoint(const std::string &host, const std::string &port)
{
     HLQueryCLI probe_cli(BuildBaseURL(host, port), false, "", "talk");
     probe_cli.SetDefaultTimeoutSeconds(1);
     const HLQueryCLI::HTTPResponse response = probe_cli.MakeRequest("GET", "/health", "", 1);
     return response.StatusCode == 200 || response.StatusCode == 503;
}

std::vector<std::pair<std::string, std::string>> FetchTalkCandidateEndpoints(HLQueryCLI &cli,
                                                                             const std::string &current_host,
                                                                             const std::string &current_port)
{
     std::vector<std::pair<std::string, std::string>> endpoints;
     std::set<std::string> seen;

     const auto add_endpoint = [&](const std::string &host, const std::string &port)
     {
          const std::string key = host + ":" + port;
          if (!host.empty() && !port.empty() && seen.insert(key).second)
          {
               endpoints.emplace_back(host, port);
          }
     };

     add_endpoint(current_host, current_port);

     const HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", "/links");
     if (response.StatusCode != 200)
     {
          return endpoints;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(response.Body);

          const auto collect = [&](const char *field_name)
          {
               if (!root.contains(field_name) || !root[field_name].is_array())
               {
                    return;
               }

               for (const auto &entry : root[field_name])
               {
                    if (!entry.is_object() || !entry.contains("endpoint") || !entry["endpoint"].is_string())
                    {
                         continue;
                    }

                    std::string host;
                    std::string port;
                    if (ParseEndpointValue(entry["endpoint"].get<std::string>(), host, port))
                    {
                         add_endpoint(host, port);
                    }
               }
          };

          collect("nodes");
          collect("slaves");
     }
     catch (...)
     {
     }

     return endpoints;
}

bool ConnectTalkToEndpoint(HLQueryCLI &cli,
                           const std::string &target_host,
                           const std::string &target_port,
                           std::string &current_host,
                           std::string &current_port)
{
     if (!IsHealthyTalkEndpoint(target_host, target_port))
     {
          return false;
     }

     current_host = target_host;
     current_port = target_port;
     cli.ReconfigureConnection(BuildBaseURL(current_host, current_port));
     cli.SetDefaultTimeoutSeconds(30);
     return true;
}

/* Lowercase a command token for dispatch. */

std::string ToLower(std::string value)
{
     std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                    { return static_cast<char>(std::tolower(character)); });

     return value;
}

bool ExtractExplicitSQLStatement(const std::string &line, std::string &sql)
{
     const std::string trimmed = TrimWhitespace(line);

     if (trimmed.size() < 5)
     {
          return false;
     }

     const std::string lowered = ToLower(trimmed);

     if (lowered.rfind("sql:", 0) == 0)
     {
          sql = TrimWhitespace(trimmed.substr(4));
          return !sql.empty();
     }

     if (lowered.rfind("exec:", 0) != 0)
     {
          return false;
     }

     sql = TrimWhitespace(trimmed.substr(5));
     return !sql.empty();
}

bool LooksLikeSelectSQLWithoutPrefix(const std::string &line)
{
     const std::string trimmed = TrimWhitespace(line);

     if (trimmed.size() < 6 || ToLower(trimmed.substr(0, 6)) != "select")
     {
          return false;
     }

     return ToLower(trimmed).find(" from ") != std::string::npos;
}

bool HasRecognizedSQLStatementStart(const std::string &sql)
{
     const std::string trimmed = TrimWhitespace(sql);

     if (trimmed.empty())
     {
          return false;
     }

     size_t end = 0;

     while (end < trimmed.size() && std::isalpha(static_cast<unsigned char>(trimmed[end])) != 0)
     {
          ++end;
     }

     if (end == 0)
     {
          return false;
     }

     const std::string keyword = ToLower(trimmed.substr(0, end));
     if (keyword == "show")
     {
          const std::string rest = ToLower(TrimWhitespace(trimmed.substr(end)));
          return rest.rfind("cols", 0) == 0 || rest.rfind("collections", 0) == 0;
     }

     return keyword == "select" || keyword == "insert" || keyword == "delete" || keyword == "update" || keyword == "drop";
}

/* Parse a comma-separated CLI list while skipping empty items. */

std::vector<std::string> ParseCommaSeparatedList(const std::string &value)
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

/* Tokenize REPL input while supporting quoted values and escaped characters. */

std::vector<std::string> TokenizeInput(const std::string &line)
{
     std::vector<std::string> parts;
     std::string current;
     char quote_character = '\0';
     bool escape_next = false;

     for (char character : line)
     {
          if (escape_next)
          {
               current.push_back(character);
               escape_next = false;
               continue;
          }

          if (character == '\\')
          {
               escape_next = true;
               continue;
          }

          if (quote_character != '\0')
          {
               if (character == quote_character)
               {
                    quote_character = '\0';
               }
               else
               {
                    current.push_back(character);
               }

               continue;
          }

          if (character == '"' || character == '\'')
          {
               quote_character = character;
               continue;
          }

          if (std::isspace(static_cast<unsigned char>(character)) != 0)
          {
               if (!current.empty())
               {
                    parts.push_back(current);
                    current.clear();
               }

               continue;
          }

          current.push_back(character);
     }

     if (escape_next)
     {
          current.push_back('\\');
     }

     if (!current.empty())
     {
          parts.push_back(current);
     }

     return parts;
}

static std::vector<std::string> TokenizeInputPreserveQuotes(const std::string &line)
{
     std::vector<std::string> parts;
     std::string current;
     char quote_character = '\0';
     bool escape_next = false;

     for (char character : line)
     {
          if (escape_next)
          {
               current.push_back(character);
               escape_next = false;
               continue;
          }

          if (character == '\\')
          {
               escape_next = true;
               current.push_back(character);
               continue;
          }

          if (quote_character != '\0')
          {
               current.push_back(character);

               if (character == quote_character)
               {
                    quote_character = '\0';
               }

               continue;
          }

          if (character == '"' || character == '\'')
          {
               quote_character = character;
               current.push_back(character);
               continue;
          }

          if (std::isspace(static_cast<unsigned char>(character)) != 0)
          {
               if (!current.empty())
               {
                    parts.push_back(current);
                    current.clear();
               }

               continue;
          }

          current.push_back(character);
     }

     if (escape_next)
     {
          current.push_back('\\');
     }

     if (!current.empty())
     {
          parts.push_back(current);
     }

     return parts;
}

static bool IsQuotedToken(const std::string &token)
{
     return token.size() >= 2 &&
            ((token.front() == '"' && token.back() == '"') ||
             (token.front() == '\'' && token.back() == '\''));
}

static std::string StripOuterQuotes(const std::string &value)
{
     if (!IsQuotedToken(value))
     {
          return value;
     }

     return value.substr(1, value.size() - 2);
}

/* Validate an unsigned integer token used for list and ID references. */

bool IsUnsignedInteger(const std::string &value)
{
     if (value.empty())
     {
          return false;
     }

     return std::all_of(value.begin(), value.end(), [](unsigned char character)
                        { return std::isdigit(character) != 0; });
}

/* Parse optional list pagination arguments. */

bool ParseListRange(const std::vector<std::string> &parts, int &offset, int &limit, std::string &error_message)
{
     offset = 0;
     limit = 1000;
     error_message.clear();

     if (parts.size() == 1)
     {
          return true;
     }

     if (parts.size() != 3 || !IsUnsignedInteger(parts[1]) || !IsUnsignedInteger(parts[2]))
     {
          error_message = "Usage: ls [offset limit]";
          return false;
     }

     try
     {
          offset = std::stoi(parts[1]);
          limit = std::stoi(parts[2]);
     }
     catch (...)
     {
          error_message = "Usage: ls [offset limit]";
          return false;
     }

     if (limit <= 0)
     {
          error_message = "Limit must be greater than 0";
          return false;
     }

     return true;
}

/* Parse search command flags after the query token. */

void ParseSearchTalkOptions(const std::vector<std::string> &args, size_t start_index, SearchTalkOptions &options)
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

          if (arg == "--json")
          {
               options.JsonOutput = true;
               continue;
          }

          if (arg.rfind("--distributed=", 0) == 0)
          {
               options.Distributed = arg.substr(14);
               std::transform(options.Distributed.begin(), options.Distributed.end(), options.Distributed.begin(),
                              [](unsigned char character)
                              {
                                   return static_cast<char>(std::tolower(character));
                              });
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

               for (char &character : maybe_val)
               {
                    if (character == ':' || character == ';' || character == '|')
                    {
                         character = ',';
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
               try
               {
                    options.Limit = std::stoi(arg);
               }
               catch (...)
               {
               }
               continue;
          }

          if (relative == 1)
          {
               try
               {
                    options.Offset = std::stoi(arg);
               }
               catch (...)
               {
               }
               continue;
          }

          if (relative == 2)
          {
               options.Sort = arg;
               continue;
          }
     }
}

static std::string JoinTalkSearchQueryTokens(const std::vector<std::string> &tokens)
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

static bool ExtractSearchTalkQuery(const std::vector<std::string> &parts,
                                   size_t start_index,
                                   std::string &query,
                                   size_t &options_start_index,
                                   bool &phrase_query)
{
     std::vector<std::string> positional;
     size_t index = start_index;

     while (index < parts.size() && parts[index].rfind("--", 0) != 0 && parts[index] != "-e" && parts[index] != "-h")
     {
          positional.push_back(parts[index]);
          ++index;
     }

     options_start_index = index;

     if (positional.empty())
     {
          return false;
     }

     size_t query_token_count = positional.size();

     if (query_token_count >= 3)
     {
          query_token_count -= 3;
     }
     else
     {
          if (query_token_count >= 2 && IsUnsignedInteger(positional[query_token_count - 1]))
          {
               --query_token_count;
          }

          if (query_token_count >= 2 && IsUnsignedInteger(positional[query_token_count - 1]))
          {
               --query_token_count;
          }
     }

     if (query_token_count == 0)
     {
          query_token_count = 1;
     }

     std::vector<std::string> query_tokens(positional.begin(), positional.begin() + static_cast<long>(query_token_count));
     query = JoinTalkSearchQueryTokens(query_tokens);
     phrase_query = (query_tokens.size() == 1 && query_tokens[0].find(' ') != std::string::npos);

     return !query.empty();
}

static bool ContainsTalkQueryKeyword(const std::string &query, const std::string &keyword)
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

static bool LooksLikeStructuredTalkQuery(const std::string &query)
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

     if (ContainsTalkQueryKeyword(query, "AND") || ContainsTalkQueryKeyword(query, "OR") || ContainsTalkQueryKeyword(query, "NOT") || ContainsTalkQueryKeyword(query, "TO"))
     {
          return true;
     }

     return false;
}

static std::string BuildTalkQueryForServer(const std::string &query, bool phrase_query)
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

     if (phrase_query || !LooksLikeStructuredTalkQuery(trimmed))
     {
          return "\"" + trimmed + "\"";
     }

     return trimmed;
}

/* Fetch visible document IDs to support numeric references in the REPL. */

std::vector<std::string> FetchDocumentIds(HLQueryCLI &cli, const std::string &collection_name, int offset = 0, int limit = 1000)
{
     std::vector<std::string> document_ids;

     if (collection_name.empty())
     {
          return document_ids;
     }

     std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/documents";
     path += "?offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return document_ids;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array())
          {
               return document_ids;
          }

          for (const auto &doc : root["documents"])
          {
               if (doc.contains("id") && doc["id"].is_string())
               {
                    document_ids.push_back(doc["id"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return document_ids;
}

bool FetchAndPrintDocumentList(HLQueryCLI &cli,
                               TalkState &state,
                               const std::string &collection_name,
                               int offset,
                               int limit)
{
     state.LastListedDocumentIds.clear();

     if (collection_name.empty())
     {
          TalkPrintError("Collection name is required");
          return false;
     }

     std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/documents";
     path += "?offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);

     TalkPrintInfo("Fetching documents for '" + collection_name + "'");
     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode == 404)
     {
          TalkPrintError("Collection not found: " + collection_name);
          return false;
     }

     if (response.StatusCode != 200)
     {
          TalkPrintError("Failed to list documents for collection '" + collection_name + "'");
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array())
          {
               TalkPrintError("Invalid documents data");
               return false;
          }

          const nlohmann::json &documents = root["documents"];
          const size_t display_count = documents.size();
          size_t total_count = display_count;

          if (root.contains("total") && root["total"].is_number_unsigned())
          {
               total_count = root["total"].get<size_t>();
          }
          else if (root.contains("total") && root["total"].is_number_integer())
          {
               total_count = static_cast<size_t>(std::max<long long>(0, root["total"].get<long long>()));
          }

          if (display_count == 0)
          {
               TalkPrintInfo("No documents found in collection '" + collection_name + "'");
               return true;
          }

          std::cout << "Found " << display_count << " document(s)";

          if (offset > 0 || static_cast<size_t>(limit) != display_count)
          {
               std::cout << " (showing " << (offset + 1) << "-" << (offset + display_count) << " of " << total_count << ").";
          }
          else if (total_count > display_count)
          {
               std::cout << " (showing first " << display_count << " of " << total_count << ").";
          }

          std::cout << " in collection '" << collection_name << "':\n\n";

          std::vector<std::vector<std::string>> rows;
          rows.reserve(display_count);

          for (size_t index = 0; index < display_count; ++index)
          {
               const nlohmann::json &document = documents[index];

               if (!document.contains("id") || !document["id"].is_string())
               {
                    continue;
               }

               int field_count = 0;

               for (const auto &entry : document.items())
               {
                    const std::string &key = entry.key();
                    const nlohmann::json &value = entry.value();

                    if (key == "id" || key == "score" || key == "timestamp" || key == "collection_id")
                    {
                         continue;
                    }

                    if (value.is_null())
                    {
                         continue;
                    }

                    if (value.is_string() && value.get<std::string>().empty())
                    {
                         continue;
                    }

                    ++field_count;
               }

               const std::string document_id = document["id"].get<std::string>();
               state.LastListedDocumentIds.push_back(document_id);
               rows.push_back({std::to_string(index + 1), document_id, std::to_string(field_count) + " fields"});
          }

          cli.PrintTable({"#", "Document ID", "Fields"}, rows);
          return true;
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse documents response");
          return false;
     }
}

std::vector<std::string> FetchSAMDocumentIds(HLQueryCLI &cli, const std::string &collection_name, int offset = 0, int limit = 1000)
{
     std::vector<std::string> document_ids;

     if (collection_name.empty())
     {
          return document_ids;
     }

     std::string path = "/sam/documents?collection=" + hlquery_cli::UrlEncode(collection_name);
     path += "&offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return document_ids;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array())
          {
               return document_ids;
          }

          for (const auto &doc : root["documents"])
          {
               if (doc.contains("id") && doc["id"].is_string())
               {
                    document_ids.push_back(doc["id"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return document_ids;
}

std::vector<std::string> FetchSAMSearchDocumentIds(HLQueryCLI &cli,
                                                   const std::string &collection_name,
                                                   const std::string &query,
                                                   int limit = 1000)
{
     std::vector<std::string> document_ids;

     if (collection_name.empty() || query.empty())
     {
          return document_ids;
     }

     std::string path = "/sam/search?collection=" + hlquery_cli::UrlEncode(collection_name) +
                        "&q=" + hlquery_cli::UrlEncode(query) +
                        "&limit=" + std::to_string(limit);

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return document_ids;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("hits") || !root["hits"].is_array())
          {
               return document_ids;
          }

          for (const auto &hit : root["hits"])
          {
               if (hit.contains("id") && hit["id"].is_string())
               {
                    document_ids.push_back(hit["id"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return document_ids;
}

std::vector<std::string> FetchSearchDocumentIds(HLQueryCLI &cli,
                                                const std::string &collection_name,
                                                const std::string &query,
                                                int limit = 1000)
{
     std::vector<std::string> document_ids;

     if (collection_name.empty() || query.empty())
     {
          return document_ids;
     }

     std::string path = "/collections/" + collection_name + "/documents/search";
     path += "?q=" + hlquery_cli::UrlEncode(query);
     path += "&query_by=" + hlquery_cli::UrlEncode("title,content");
     path += "&limit=" + std::to_string(limit);
     path += "&offset=0";

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return document_ids;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("hits") || !root["hits"].is_array())
          {
               return document_ids;
          }

          for (const auto &hit : root["hits"])
          {
               if (hit.contains("document") && hit["document"].is_object() &&
                   hit["document"].contains("id") && hit["document"]["id"].is_string())
               {
                    document_ids.push_back(hit["document"]["id"].get<std::string>());
               }
               else if (hit.contains("id") && hit["id"].is_string())
               {
                    document_ids.push_back(hit["id"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return document_ids;
}

static std::string NormalizeTalkLLMQuery(std::string query)
{
     query = TrimWhitespace(query);

     if (query.size() >= 2 &&
         ((query.front() == '"' && query.back() == '"') ||
          (query.front() == '\'' && query.back() == '\'')))
     {
          query = query.substr(1, query.size() - 2);
          query = TrimWhitespace(query);
     }

     std::string lowered = ToLower(query);
     const std::vector<std::string> prefixes = {
          "find documents about ",
          "find docs about ",
          "search documents about ",
          "search docs about ",
          "documents about ",
          "docs about ",
          "find documents ",
          "find docs ",
          "search documents ",
          "search docs "
     };

     for (const std::string &prefix : prefixes)
     {
          if (lowered.rfind(prefix, 0) == 0 && query.size() > prefix.size())
          {
               return TrimWhitespace(query.substr(prefix.size()));
          }
     }

     return query;
}

static bool CollectionLikelyMultiLanguage(HLQueryCLI &cli,
                                          const std::string &collection_name,
                                          std::string &language_out,
                                          int max_documents_to_sample = 200)
{
     language_out.clear();

     if (collection_name.empty() || max_documents_to_sample <= 0)
     {
          return false;
     }

     std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/documents";
     path += "?offset=0&limit=" + std::to_string(max_documents_to_sample);

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array())
          {
               return false;
          }

          std::string first_lang;
          bool saw_any_lang = false;

          for (const auto &doc : root["documents"])
          {
               if (!doc.is_object())
               {
                    continue;
               }

               if (!doc.contains("lang") || !doc["lang"].is_string())
               {
                    continue;
               }

               std::string lang = doc["lang"].get<std::string>();
               lang = TrimWhitespace(lang);

               if (lang.empty() || lang == "und")
               {
                    continue;
               }

               if (!saw_any_lang)
               {
                    first_lang = lang;
                    saw_any_lang = true;
                    continue;
               }

               if (!first_lang.empty() && lang != first_lang)
               {
                    language_out = "multi";
                    return true;
               }
          }

          if (saw_any_lang && !first_lang.empty())
          {
               language_out = first_lang;
               return true;
          }
     }
     catch (...)
     {
          return false;
     }

     return false;
}

/* Fetch one document ID by absolute 1-based position within a collection listing. */

bool FetchDocumentIdAtPosition(HLQueryCLI &cli,
                               const std::string &collection_name,
                               size_t document_index,
                               std::string &document_id)
{
     document_id.clear();

     if (collection_name.empty())
     {
          return false;
     }

     std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/documents";
     path += "?offset=" + std::to_string(document_index) + "&limit=1";

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array() || root["documents"].empty())
          {
               return false;
          }

          const nlohmann::json &document = root["documents"][0];

          if (!document.contains("id") || !document["id"].is_string())
          {
               return false;
          }

          document_id = document["id"].get<std::string>();
          return true;
     }
     catch (...)
     {
          return false;
     }
}

bool FetchSAMDocumentIdAtPosition(HLQueryCLI &cli,
                                  const std::string &collection_name,
                                  size_t document_index,
                                  std::string &document_id)
{
     document_id.clear();

     if (collection_name.empty())
     {
          return false;
     }

     std::string path = "/sam/documents?collection=" + hlquery_cli::UrlEncode(collection_name);
     path += "&offset=" + std::to_string(document_index) + "&limit=1";

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("documents") || !root["documents"].is_array() || root["documents"].empty())
          {
               return false;
          }

          const nlohmann::json &document = root["documents"][0];

          if (!document.contains("id") || !document["id"].is_string())
          {
               return false;
          }

          document_id = document["id"].get<std::string>();
          return true;
     }
     catch (...)
     {
          return false;
     }
}

/* Fetch collection names to support numeric selection in the REPL. */

std::vector<std::string> FetchCollectionNames(HLQueryCLI &cli,
                                              int offset = 0,
                                              int limit = 1000,
                                              std::string *error_message = nullptr,
                                              int timeout_seconds = -1)
{
     std::vector<std::string> collection_names;

     std::string path = "/collections";
     path += "?offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit) + "&names_only=1";

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path, "", timeout_seconds);

     if (response.StatusCode != 200)
     {
          if (error_message != nullptr)
          {
               *error_message = response.StatusCode == -1 ? TrimWhitespace(response.Body)
                                                          : "HTTP " + std::to_string(response.StatusCode);
          }

          return collection_names;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("collections") || !root["collections"].is_array())
          {
               return collection_names;
          }

          for (const auto &collection : root["collections"])
          {
               if (collection.contains("name") && collection["name"].is_string())
               {
                    collection_names.push_back(collection["name"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return collection_names;
}

bool TalkCollectionExists(HLQueryCLI &cli, const std::string &collection_name, int timeout_seconds = -1)
{
     if (collection_name.empty() || collection_name.find_first_of(" \t\n\r") != std::string::npos)
     {
          return false;
     }

     const HLQueryCLI::HTTPResponse response =
          cli.MakeRequest("GET", "/collections/" + hlquery_cli::UrlEncode(collection_name), "", timeout_seconds);
     return response.StatusCode == 200;
}

bool FetchAndPrintCollectionList(HLQueryCLI &cli,
                                 TalkState &state,
                                 int offset,
                                 int limit)
{
     std::string path = "/collections";
     path += "?offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);

     TalkPrintInfo("Fetching collections");
     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          TalkPrintError(response.StatusCode == -1 ? "Failed to fetch collections: " + TrimWhitespace(response.Body)
                                                   : "Failed to fetch collections");
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("collections") || !root["collections"].is_array())
          {
               TalkPrintError("Invalid collections data");
               return false;
          }

          const nlohmann::json &collections = root["collections"];
          const size_t display_count = collections.size();
          const size_t total_count = root.contains("total") && root["total"].is_number_unsigned()
                                      ? root["total"].get<size_t>()
                                      : display_count;
          const size_t found_count = root.contains("found") && root["found"].is_number_unsigned()
                                      ? root["found"].get<size_t>()
                                      : display_count;

          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();

          if (display_count == 0)
          {
               TalkPrintInfo("No collections found");
               return true;
          }

          if (root.contains("total"))
          {
               std::cout << "Showing " << display_count << " out of " << found_count << " matched collection(s) (total " << total_count << " in system).";
          }
          else
          {
               std::cout << "Found " << display_count << " collection(s).";
          }

          if (offset > 0)
          {
               std::cout << " (from offset " << offset << ").";
          }

          if (display_count < found_count || (display_count == 1000 && limit >= 1000))
          {
               std::cout << "\nNote: More collections available. Use 'ls <offset> <limit>' to see more.";
          }

          std::cout << ":\n\n";

          std::vector<std::vector<std::string>> rows;
          rows.reserve(display_count);

          for (size_t index = 0; index < display_count; ++index)
          {
               const nlohmann::json &collection = collections[index];

               if (!collection.contains("name") || !collection["name"].is_string())
               {
                    continue;
               }

               const std::string collection_name = collection["name"].get<std::string>();
               int document_count = 0;

               if (collection.contains("num_documents") && collection["num_documents"].is_number_integer())
               {
                    document_count = collection["num_documents"].get<int>();
               }

               state.LastListedCollections.push_back(collection_name);
               const std::string document_count_text =
                    collection.contains("num_documents") && collection["num_documents"].is_number_integer()
                         ? std::to_string(document_count) + " docs"
                         : "-";
               rows.push_back({std::to_string(offset + index + 1), collection_name, document_count_text});
          }

          cli.PrintTable({"#", "Collection Name", "Documents"}, rows);
          return true;
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse collections response");
          return false;
     }
}

/* Fetch collection names from a collection-name search result page. */

std::vector<std::string> FetchSearchCollectionNames(HLQueryCLI &cli,
                                                    const std::string &query,
                                                    const SearchTalkOptions &options)
{
     std::vector<std::string> collection_names;

     if (query.empty())
     {
          return collection_names;
     }

     std::string distributed_mode = options.Distributed;
     std::transform(distributed_mode.begin(), distributed_mode.end(), distributed_mode.begin(),
                    [](unsigned char character)
                    {
                         return static_cast<char>(std::tolower(character));
                    });

     const bool use_distributed =
          (distributed_mode == "on" || distributed_mode == "true" || distributed_mode == "1" ||
           distributed_mode == "force" || distributed_mode == "remote");

     std::string path = use_distributed ? "/collections/distributed" : "/collections";
     path += "?offset=" + std::to_string(std::max(0, options.Offset));

     if (options.Limit > 0)
     {
          path += "&limit=" + std::to_string(options.Limit);
     }

     if (!options.Sort.empty())
     {
          path += "&sort_by=" + hlquery_cli::UrlEncode(options.Sort);
     }

     if (!options.Distributed.empty() && !use_distributed)
     {
          path += "&distributed=" + hlquery_cli::UrlEncode(options.Distributed);
     }

     if (!options.Route.empty())
     {
          path += "&route=" + hlquery_cli::UrlEncode(options.Route);
     }

     if (options.MaybeMin >= 0)
     {
          path += "&maybe_min=" + std::to_string(options.MaybeMin);
     }

     if (options.MaybeLimit > 0)
     {
          path += "&maybe_limit=" + std::to_string(options.MaybeLimit);
     }

     if (query.find('*') != std::string::npos || query.find('?') != std::string::npos)
     {
          path += "&pattern=" + hlquery_cli::UrlEncode(query);
     }
     else
     {
          path += "&search=" + hlquery_cli::UrlEncode(query);
     }

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          return collection_names;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("collections") || !root["collections"].is_array())
          {
               return collection_names;
          }

          for (const auto &collection : root["collections"])
          {
               if (collection.contains("name") && collection["name"].is_string())
               {
                    collection_names.push_back(collection["name"].get<std::string>());
               }
          }
     }
     catch (...)
     {
     }

     return collection_names;
}

/* List collection aliases through the daemon alias endpoint. */

bool ListAliases(HLQueryCLI &cli, const std::string &collection_name = "")
{
     std::string path = "/aliases";

     if (!collection_name.empty())
     {
          path += "?collection=" + hlquery_cli::UrlEncode(collection_name);
     }

     const HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          TalkPrintError("Failed to list aliases");
          return false;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("aliases") || !root["aliases"].is_array())
          {
               TalkPrintError("Invalid alias list response");
               return false;
          }

          const nlohmann::json &aliases = root["aliases"];

          if (aliases.empty())
          {
               TalkPrintInfo("No aliases found");
               return true;
          }

          std::vector<std::vector<std::string>> rows;
          rows.reserve(aliases.size());

          for (const auto &alias : aliases)
          {
               const std::string name =
                    alias.contains("name") && alias["name"].is_string()
                         ? alias["name"].get<std::string>()
                         : "";
               const std::string collection =
                    alias.contains("collection_name") && alias["collection_name"].is_string()
                         ? alias["collection_name"].get<std::string>()
                         : (alias.contains("collection") && alias["collection"].is_string()
                                 ? alias["collection"].get<std::string>()
                                 : "");
               const std::string updated_at =
                    alias.contains("updated_at") && alias["updated_at"].is_string()
                         ? alias["updated_at"].get<std::string>()
                         : "";

               rows.push_back({name, collection, updated_at});
          }

          cli.PrintTable({"Alias", "Collection", "Updated"}, rows);
          return true;
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse alias list");
          return false;
     }
}

bool ShowSAMSearchJobs(HLQueryCLI &cli, TalkState &state, const std::string &job_id = "")
{
     state.LastListedDocumentIds.clear();
     state.LastListedSAMDocumentIds.clear();

     const bool list_all = job_id.empty() || job_id == "all";
     std::string resolved_job_id = job_id;

     if (!list_all && IsUnsignedInteger(job_id))
     {
          if (state.LastListedSAMSearchJobIds.empty())
          {
               TalkPrintError("Run 'see' first, then choose a job number from the table.");
               return false;
          }

          const size_t job_index = static_cast<size_t>(std::stoul(job_id));

          if (job_index == 0 || job_index > state.LastListedSAMSearchJobIds.size())
          {
               TalkPrintError("SAM search job number out of range. Run 'see' and choose a number between 1 and " +
                              std::to_string(state.LastListedSAMSearchJobIds.size()));
               return false;
          }

          resolved_job_id = state.LastListedSAMSearchJobIds[job_index - 1];
     }

     const std::string path = list_all
          ? "/sam/search_jobs?limit=50"
          : "/sam/search_jobs/" + hlquery_cli::UrlEncode(resolved_job_id);

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path, "", 30);

     if (response.StatusCode != 200)
     {
          TalkPrintError(list_all ? "Failed to list SAM search jobs" : "Failed to fetch SAM search job '" + resolved_job_id + "'");
          return false;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(response.Body);

          if (list_all)
          {
               state.LastListedSAMSearchJobIds.clear();
               const nlohmann::json jobs = root.value("jobs", nlohmann::json::array());

               if (!jobs.is_array() || jobs.empty())
               {
                    TalkPrintInfo("No async SAM search jobs found");
                    return true;
               }

               std::vector<std::vector<std::string>> rows;
               size_t index = 1;

               for (const auto &job : jobs)
               {
                    const std::string id = job.value("id", "");
                    if (!id.empty())
                    {
                         state.LastListedSAMSearchJobIds.push_back(id);
                    }

                    rows.push_back({
                         std::to_string(index++),
                         id,
                         job.value("collection", ""),
                         job.value("query", ""),
                         job.value("state", ""),
                         std::to_string(job.value("status_code", 0))
                    });
               }

               cli.PrintTable({"#", "Job ID", "Collection", "Query", "State", "HTTP"}, rows);
               return true;
          }

          if (!root.contains("job") || !root["job"].is_object())
          {
               TalkPrintError("Invalid SAM search job response");
               return false;
          }

          const nlohmann::json &job = root["job"];
          std::vector<std::vector<std::string>> rows;
          rows.push_back({"id", job.value("id", "")});
          rows.push_back({"collection", job.value("collection", "")});
          rows.push_back({"query", job.value("query", "")});
          rows.push_back({"state", job.value("state", "")});
          rows.push_back({"http", std::to_string(job.value("status_code", 0))});
          rows.push_back({"created_ms", std::to_string(job.value("created_ms", static_cast<uint64_t>(0)))});
          rows.push_back({"started_ms", std::to_string(job.value("started_ms", static_cast<uint64_t>(0)))});
          rows.push_back({"finished_ms", std::to_string(job.value("finished_ms", static_cast<uint64_t>(0)))});
          cli.PrintTable({"Field", "Value"}, rows);

          if (!job.contains("response") || !job["response"].is_object())
          {
               return true;
          }

          const nlohmann::json &job_response = job["response"];

          if (!job_response.contains("hits") || !job_response["hits"].is_array() || job_response["hits"].empty())
          {
               TalkPrintInfo("Job has no hits yet");
               return true;
          }

          std::vector<std::vector<std::string>> hit_rows;
          size_t index = 1;

          for (const auto &hit : job_response["hits"])
          {
               const std::string id = hit.value("id", "");

               if (!id.empty())
               {
                    state.LastListedDocumentIds.push_back(id);
                    state.LastListedSAMDocumentIds.push_back(id);
               }

               std::ostringstream score_stream;
               score_stream << std::fixed << std::setprecision(2) << hit.value("score", 0.0);

               hit_rows.push_back({
                    std::to_string(index++),
                    id,
                    hit.value("title", ""),
                    hit.value("term", ""),
                    hit.value("source", ""),
                    score_stream.str()
               });
          }

          cli.PrintTable({"#", "ID", "Title", "Matched Term", "Source", "Score"}, hit_rows);
          TalkPrintInfo("Use 'open 1' or 'sam open 1' to inspect a result.");
          return true;
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse SAM search job response");
          return false;
     }
}

/* Delete a document locally to avoid distributed deletes from the REPL. */

void DeleteLocalDocument(HLQueryCLI &cli, const std::string &collection_name, const std::string &document_id)
{
     if (collection_name.empty() || document_id.empty())
     {
          TalkPrintError("Invalid arguments: collection name and document ID are required");
          return;
     }

     const std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) +
                              "/documents/" + hlquery_cli::UrlEncode(document_id) +
                              "?distributed=off";

     HLQueryCLI::HTTPResponse response = cli.MakeRequest("DELETE", path);

     if (response.StatusCode == 200)
     {
          TalkPrintSuccess("Document '" + document_id + "' deleted from collection '" + collection_name + "'");
          return;
     }
     else if (response.StatusCode == 404)
     {
          TalkPrintError("Document '" + document_id + "' not found in collection '" + collection_name + "'");
     }
     else
     {
          TalkPrintError("Delete request failed (HTTP " + std::to_string(response.StatusCode) + ")");
     }
}

/* Resolve a numeric document reference against the absolute collection position. */

bool ResolveCollectionDocumentReference(HLQueryCLI &cli,
                                        const std::string &collection_name,
                                        const std::string &value,
                                        const std::vector<std::string> &listed_document_ids,
                                        std::string &document_id,
                                        std::string &error_message)
{
     document_id = value;
     error_message.clear();

     if (!IsUnsignedInteger(value))
     {
          return true;
     }

     const size_t document_index = static_cast<size_t>(std::stoul(value));

     if (document_index == 0)
     {
          error_message = "Document number must be greater than 0";
          return false;
     }

     if (document_index <= listed_document_ids.size())
     {
          document_id = listed_document_ids[document_index - 1];
          return true;
     }

     if (FetchDocumentIdAtPosition(cli, collection_name, document_index - 1, document_id))
     {
          return true;
     }

     error_message = "Document number out of range for collection '" + collection_name + "'";
     return false;
}

bool ResolveSAMDocumentReference(HLQueryCLI &cli,
                                 const std::string &collection_name,
                                 const std::string &value,
                                 const std::vector<std::string> &listed_document_ids,
                                 std::string &document_id,
                                 std::string &error_message)
{
     document_id = value;
     error_message.clear();

     if (!IsUnsignedInteger(value))
     {
          return true;
     }

     const size_t document_index = static_cast<size_t>(std::stoul(value));

     if (document_index == 0)
     {
          error_message = "Document number must be greater than 0";
          return false;
     }

     if (document_index <= listed_document_ids.size())
     {
          document_id = listed_document_ids[document_index - 1];
          return true;
     }

     if (FetchSAMDocumentIdAtPosition(cli, collection_name, document_index - 1, document_id))
     {
          return true;
     }

     error_message = "SAM document number out of range for collection '" + collection_name + "'";
     return false;
}

bool ResolveSAMCollectionReference(const TalkState &state,
                                   HLQueryCLI &cli,
                                   const std::string &value,
                                   std::string &collection_name,
                                   std::string &error_message)
{
     collection_name = value;
     error_message.clear();

     if (!state.CurrentCollection.empty() && value == state.CurrentCollection)
     {
          return true;
     }

     if (IsUnsignedInteger(value) && !state.LastListedCollections.empty())
     {
          const size_t collection_index = static_cast<size_t>(std::stoul(value));
          if (collection_index == 0 || collection_index > state.LastListedCollections.size())
          {
               error_message = "Collection number out of range. Run 'ls' and choose a number between 1 and " + std::to_string(state.LastListedCollections.size());
               return false;
          }

          collection_name = state.LastListedCollections[collection_index - 1];
          return true;
     }

     std::string fetch_error;
     const std::vector<std::string> collection_names = FetchCollectionNames(cli, 0, 1000, &fetch_error);
     if (std::find(collection_names.begin(), collection_names.end(), value) != collection_names.end())
     {
          return true;
     }

     if (ResolveTalkAliasCollection(state, cli, value, collection_name))
     {
          return true;
     }

     error_message = "Collection not found: " + value;
     return false;
}

bool ParseSAMDocumentTarget(const std::string &value,
                            const std::string &active_collection,
                            std::string &collection_name,
                            std::string &document_id,
                            std::string &error_message)
{
     collection_name.clear();
     document_id.clear();
     error_message.clear();

     const size_t slash_pos = value.find('/');

     if (slash_pos == std::string::npos)
     {
          if (active_collection.empty())
          {
               error_message = "Usage: sam open <document-id> or sam open <collection>/<document-id>";
               return false;
          }

          collection_name = active_collection;
          document_id = value;
          return !document_id.empty();
     }

     if (slash_pos == 0 || slash_pos + 1 >= value.size())
     {
          error_message = "Usage: sam open <document-id> or sam open <collection>/<document-id>";
          return false;
     }

     collection_name = value.substr(0, slash_pos);
     document_id = value.substr(slash_pos + 1);
     return true;
}

bool StreamSAMDebug(HLQueryCLI &cli,
                    const std::string &collection_name,
                    int limit)
{
     uint64_t since_sequence = 0;
     int idle_polls = 0;

     TalkPrintInfo("Streaming SAM debug for '" + collection_name + "'");

     while (true)
     {
          const std::string path = "/sam/debug?collection=" + hlquery_cli::UrlEncode(collection_name) +
                                   "&since=" + std::to_string(since_sequence) +
                                   "&limit=" + std::to_string(limit);
          HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

          if (response.StatusCode != 200)
          {
               TalkPrintError("Failed to fetch SAM debug events");
               return false;
          }

          nlohmann::json root;

          try
          {
               root = nlohmann::json::parse(response.Body);
          }
          catch (const std::exception &)
          {
               TalkPrintError("Failed to parse SAM debug response");
               return false;
          }

          bool saw_event = false;

          if (root.contains("events") && root["events"].is_array())
          {
               for (const auto &event : root["events"])
               {
                    const uint64_t sequence = event.value("sequence", static_cast<uint64_t>(0));
                    const std::string message = event.value("message", "");

                    if (sequence > since_sequence)
                    {
                         since_sequence = sequence;
                    }

                    if (!message.empty())
                    {
                         std::cout << "[sam] " << message << "\n";
                         saw_event = true;
                    }
               }
          }

          const bool running = root.value("running", false);

          if (saw_event)
          {
               idle_polls = 0;
          }
          else
          {
               ++idle_polls;
          }

          if (!running && idle_polls >= 2)
          {
               TalkPrintInfo("SAM debug stream ended for '" + collection_name + "'");
               return true;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(700));
     }
}

bool ShowSAMLast(HLQueryCLI &cli,
                 const std::string &collection_name,
                 int limit)
{
     const std::string path = "/sam/debug?since=0&limit=512" +
                              (collection_name.empty() ? std::string() :
                               "&collection=" + hlquery_cli::UrlEncode(collection_name));
     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          TalkPrintError("Failed to fetch SAM activity");
          return false;
     }

     nlohmann::json root;

     try
     {
          root = nlohmann::json::parse(response.Body);
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse SAM activity response");
          return false;
     }

     std::vector<nlohmann::json> events;

     if (root.contains("events") && root["events"].is_array())
     {
          for (const auto &event : root["events"])
          {
               if (!event.value("message", "").empty())
               {
                    events.push_back(event);
               }
          }
     }

     if (events.empty())
     {
          if (collection_name.empty())
          {
               TalkPrintInfo("No recent SAM activity.");
          }
          else
          {
               TalkPrintInfo("No recent SAM activity for '" + collection_name + "'.");
          }

          return true;
     }

     if (collection_name.empty())
     {
          TalkPrintInfo("Latest SAM activity:");
     }
     else
     {
          TalkPrintInfo("Latest SAM activity for '" + collection_name + "':");
     }

     const size_t count = static_cast<size_t>(std::max(1, limit));
     const size_t start = events.size() > count ? events.size() - count : 0;

     for (size_t index = start; index < events.size(); ++index)
     {
          const auto &event = events[index];
          const uint64_t sequence = event.value("sequence", static_cast<uint64_t>(0));
          const std::string event_collection = event.value("collection", "");
          const std::string message = event.value("message", "");

          std::cout << "[sam #" << sequence;

          if (!event_collection.empty())
          {
               std::cout << " " << event_collection;
          }

          std::cout << "] " << message << "\n";
     }

     return true;
}

/* Resolve a numeric collection reference against the last listed names. */

bool EnsureCachedCollectionNames(HLQueryCLI &cli,
                                TalkState &state,
                                const std::string &value,
                                std::string &error_message)
{
     error_message.clear();

     if (!IsUnsignedInteger(value) || !state.LastListedCollections.empty())
     {
          return true;
     }

     TalkPrintInfo("Resolving numeric collection reference '" + value + "'");
     TalkPrintInfo("Fetching collection list for reference lookup");
     std::string fetch_error;
     state.LastListedCollections = FetchCollectionNames(cli, 0, 1000, &fetch_error);

     if (state.LastListedCollections.empty())
     {
          error_message = "Failed to fetch collections; cannot resolve numeric reference. Run 'ls' to retry, use a collection name, or raise --timeout.";
          if (!fetch_error.empty())
          {
               error_message += " (" + fetch_error + ")";
          }

          return false;
     }

     return true;
}

bool ResolveCollectionReference(const std::string &value,
                                const std::vector<std::string> &listed_collections,
                                std::string &collection_name,
                                std::string &error_message)
{
     collection_name = value;
     error_message.clear();

     if (!IsUnsignedInteger(value) || listed_collections.empty())
     {
          return true;
     }

     const size_t collection_index = static_cast<size_t>(std::stoul(value));

     if (collection_index == 0 || collection_index > listed_collections.size())
     {
          error_message = "Collection number out of range. Run 'ls' and choose a number between 1 and " + std::to_string(listed_collections.size());
          return false;
     }

     collection_name = listed_collections[collection_index - 1];
     return true;
}

/* Print the document count for one collection using the existing collection stats route. */

bool PrintCollectionDocumentCount(HLQueryCLI &cli, const std::string &collection_name)
{
     if (collection_name.empty())
     {
          TalkPrintError("Collection name is required");
          return false;
     }

     const std::string path = "/collections/" + hlquery_cli::UrlEncode(collection_name) + "/stats";
     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", path);

     if (response.StatusCode != 200)
     {
          TalkPrintError("Failed to get document count for collection '" + collection_name + "'");
          return false;
     }

     try
     {
          const nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("num_documents"))
          {
               TalkPrintError("Document count not available for collection '" + collection_name + "'");
               return false;
          }

          if (root["num_documents"].is_number_unsigned())
          {
               std::cout << root["num_documents"].get<size_t>() << "\n";
               return true;
          }

          if (root["num_documents"].is_number_integer())
          {
               std::cout << root["num_documents"].get<long long>() << "\n";
               return true;
          }

          TalkPrintError("Invalid document count for collection '" + collection_name + "'");
          return false;
     }
     catch (const std::exception &)
     {
          TalkPrintError("Failed to parse document count for collection '" + collection_name + "'");
          return false;
     }
}

/* Print the available REPL commands. */

void PrintHelp()
{
     std::cout << "Available commands:\n";
     std::cout << "  alias [NAME TARGET]  List aliases or point NAME to an existing command or collection\n";
     std::cout << "  uname [-a]  Show the server name and id\n";
     std::cout << "  id       Show the server id\n";
     std::cout << "  use COL|#  Select a collection context\n";
     std::cout << "  use      Show the active collection\n";
     std::cout << "  lang [COL|#]  Print the detected language for a collection\n";
     std::cout << "  pwd      Show the current location\n";
     std::cout << "  cd [COL|#|..]  Change collection context or go back\n";
     std::cout << "  back     Go back to the previous collection\n";
     std::cout << "  l        Alias for ls\n";
     std::cout << "  ls       List collections, or documents in the active collection\n";
     std::cout << "  ls COL|# List documents for one collection without changing context\n";
     std::cout << "  als      Alias for alias (list command aliases)\n";
     std::cout << "  aliases [COL]  List all collection aliases, or only aliases pointing to COL\n";
     std::cout << "  show cols [offset limit]  List all collections (alias for ls when no collection is active)\n";
     std::cout << "  search QUERY [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]\n";
     std::cout << "           Search documents in the active collection, or collection names when no collection is active\n";
     std::cout << "  search COL|# QUERY [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]\n";
     std::cout << "           Search documents in one collection without changing context when no collection is active\n";
     std::cout << "           Use quotes for multi-word queries, for example: search \"lime juice\"\n";
     std::cout << "  maybe QUERY [collection] [limit] [min_results]\n";
     std::cout << "           Suggest likely intended phrases through the daemon maybe endpoint\n";
     std::cout << "  sam help  Show SAM subcommands\n";
     std::cout << "  sam ID  Show contextual lookup phrases for one document in the active collection\n";
	     std::cout << "  sam run COL  Start background SAM indexing for one collection\n";
	     std::cout << "  sam index [COL]  Start background SAM indexing for COL or the active collection\n";
     std::cout << "  sam search [COL] QUERY [limit]  Search SAM phrases in the current or specified collection\n";
     std::cout << "  sam status [COL]  Show current SAM background indexing status\n";
     std::cout << "  sam history [COL] [limit]  Show recent SAM search history\n";
     std::cout << "  sam int|inst|interactions [COL] [limit]  Show learned SAM interaction refinements\n";
     std::cout << "  sam improve [limit] [--force]  Run a SAM improvement pass now\n";
     std::cout << "  sam last [COL] [limit]  Show latest SAM activity events\n";
     std::cout << "  sam debug [COL] [limit]  Stream live SAM debug events for one collection\n";
     std::cout << "  sam list [COL] [offset limit]  List SAM-indexed documents for one collection\n";
     std::cout << "  sam open ID|COL/ID  Open one SAM-indexed document\n";
     std::cout << "  connect [HOST:PORT|URL]  Connect to one endpoint, or retry the current node plus all /links endpoints\n";
     std::cout << "  run N COMMAND  Execute one talk command N times, for example: run 5 sql: select * from books\n";
     std::cout << "  sql: SELECT ... FROM ...  Run a SQL query through the daemon /sql route\n";
     std::cout << "  sql: show cols;  List all collections through the daemon SQL route\n";
     std::cout << "  sql: INSERT INTO ... (...) VALUES (...)  Insert one document through the daemon SQL route\n";
     std::cout << "  sql: DELETE FROM ... WHERE ...  Delete matching documents through the daemon SQL route\n";
     std::cout << "  sql: DROP <collection>  Delete a collection through the daemon SQL route\n";
     std::cout << "  exec: INSERT INTO ... (...) VALUES (...)  Alias for SQL execution, useful for write statements\n";
     std::cout << "  exec INSERT INTO ... (...) VALUES (...)   Command form without the colon\n";
     std::cout << "  algorithm  Show the server search algorithm and default ranking\n";
     std::cout << "  open ID  Open a document from the active collection\n";
     std::cout << "  select ID FIELD  Show one field from a document in the active collection\n";
     std::cout << "  update ID FIELD VALUE  Update one field in a document from the active collection\n";
     std::cout << "  count [COL|#]  Show the document count for one collection\n";
     std::cout << "  migrate COL NEWCOL [--drop-old]  Copy one collection into a new name\n";
     std::cout << "  copy COL NEWCOL  Copy one collection into a new name\n";
     std::cout << "  copy ID NEWID  Copy one document in the active collection\n";
     std::cout << "  delete ID  Delete a document from the active collection\n";
     std::cout << "  delete COL|#  Delete a collection when no collection is active\n";
     std::cout << "  links    Show distributed links\n";
     std::cout << "  bw [kb|mb|gb]  Show total bandwidth transferred\n";
     std::cout << "  modules [1|0]  List loaded modules, core only with 1, optional only with 0\n";
     std::cout << "  llm      Show active LLM runtime information\n";
     std::cout << "  llm QUERY  Search the active collection with LLM-assisted document intent, for example: llm find docs about boston\n";
     std::cout << "  see all  List async LLM/SAM search jobs\n";
     std::cout << "  see JOB  Show one async LLM/SAM search job and its hits when complete\n";
     std::cout << "  module NAME [info|syntax|ROUTE [args...]]  Run one module command\n";
     std::cout << "  load NAME  Load one runtime module\n";
     std::cout << "  unload NAME  Unload one runtime module\n";
     std::cout << "  loadmodule NAME  Alias for load\n";
     std::cout << "  unloadmodule NAME  Alias for unload\n";
     std::cout << "  dbsize [kb|mb|gb]  Show database size\n";
     std::cout << "  flush    Flush all data\n";
     std::cout << "  reset    Clear the terminal\n";
     std::cout << "  unalias NAME  Remove one command alias\n";
     std::cout << "  stats    Show server status and metrics\n";
     std::cout << "  ping     Measure server latency in ms\n";
     std::cout << "  uptime   Show server uptime\n";
     std::cout << "  help     Show this help\n";
     std::cout << "  exit     Quit the shell\n";
     std::cout << "  quit     Quit the shell\n";
}

void PrintSAMHelp()
{
     std::cout << "SAM commands:\n";
     std::cout << "  sam help  Show SAM subcommands\n";
     std::cout << "  sam ID  Show contextual lookup phrases for one document in the active collection\n";
	     std::cout << "  sam run COL  Start background SAM indexing for one collection\n";
	     std::cout << "  sam index [COL]  Start background SAM indexing for COL or the active collection\n";
     std::cout << "  sam search [COL] QUERY [limit]  Search SAM phrases in the current or specified collection\n";
     std::cout << "  sam status [COL]  Show current SAM background indexing status\n";
     std::cout << "  sam history [COL] [limit]  Show recent SAM search history\n";
     std::cout << "  sam int|inst|interactions [COL] [limit]  Show learned SAM interaction refinements\n";
     std::cout << "  sam improve [limit] [--force]  Run a SAM improvement pass now\n";
     std::cout << "  sam last [COL] [limit]  Show latest SAM activity events\n";
     std::cout << "  sam debug [COL] [limit]  Stream live SAM debug events for one collection\n";
     std::cout << "  sam list [COL] [offset limit]  List SAM-indexed documents for one collection\n";
     std::cout << "  sam open ID|COL/ID  Open one SAM-indexed document\n";
}

/* Print syntax guidance for the overloaded select command. */

void PrintSelectSyntax()
{
     TalkPrintError("Usage: select <document-id|number> <field>");
     TalkPrintInfo("SQL form: sql: SELECT ... FROM ...");
}

void PrintSelectSyntaxForLine(const std::string &line)
{
     PrintSelectSyntax();

     if (!LooksLikeSelectSQLWithoutPrefix(line))
     {
          return;
     }

     TalkPrintInfo("Did you mean: sql: " + TrimWhitespace(line));
}

/* Build the interactive prompt from the current location. */

std::string BuildPrompt(const std::string &host, const std::string &port, const TalkState &state)
{
     std::string prompt = host + ":" + port;

     if (!state.CurrentCollection.empty())
     {
          prompt += "|" + state.CurrentCollection;
     }

     prompt += "> ";

     return prompt;
}

void SetCurrentCollection(TalkState &state, const std::string &collection_name)
{
     if (!state.CurrentCollection.empty() && state.CurrentCollection != collection_name)
     {
          state.CollectionHistory.push_back(state.CurrentCollection);
     }

     state.CurrentCollection = collection_name;
     state.LastListedDocumentIds.clear();
     state.LastListedSAMDocumentIds.clear();
     state.LastListedSAMSearchJobIds.clear();
     state.LastSAMSearchCollection.clear();
     state.LastSAMSearchQuery.clear();
}

bool GoBackCollection(TalkState &state)
{
     if (state.CollectionHistory.empty())
     {
          if (state.CurrentCollection.empty())
          {
               return false;
          }

          state.CurrentCollection.clear();
          state.LastListedDocumentIds.clear();
          state.LastListedSAMDocumentIds.clear();
          return true;
     }

     state.CurrentCollection = state.CollectionHistory.back();
     state.CollectionHistory.pop_back();
     state.LastListedDocumentIds.clear();
     state.LastListedSAMDocumentIds.clear();
     return true;
}

/* Render the current logical location shown by pwd. */

std::string GetCurrentLocation(const TalkState &state)
{
     if (state.CurrentCollection.empty())
     {
          return ".";
     }

     return state.CurrentCollection;
}

std::vector<std::string> GetTalkCommands()
{
     return {
         "alias",
         "aliases",
         "uname",
         "id",
         "help",
         "connect",
         "run",
         "use",
         "lang",
         "pwd",
         "back",
         "cd",
         "l",
         "ls",
         "als",
         "show",
         "search",
         "exec",
         "algorithm",
         "open",
         "select",
         "update",
         "count",
         "migrate",
         "copy",
         "delete",
         "maybe",
         "sam",
         "stats",
         "ping",
         "links",
         "bw",
         "modules",
         "llm",
         "see",
         "module",
         "load",
         "loadmodule",
         "unload",
         "unloadmodule",
         "dbsize",
         "flush",
         "reset",
         "unalias",
         "uptime",
         "exit",
         "quit",
         "sql:"};
}

std::vector<std::string> GetTalkSAMCommands()
{
     return {
         "help",
         "run",
         "search",
         "status",
         "history",
         "int",
         "inst",
         "interactions",
         "improve",
         "last",
         "debug",
         "ls",
         "list",
         "open"};
}

bool IsBuiltinTalkCommand(const std::string &command)
{
     const std::string lowered = ToLower(command);
     const std::vector<std::string> commands = GetTalkCommands();

     return std::find(commands.begin(), commands.end(), lowered) != commands.end();
}

bool ResolveAliasedTalkTarget(const TalkState &state,
                              HLQueryCLI &cli,
                              const std::string &command,
                              std::string &resolved_target,
                              bool &target_is_collection,
                              std::string &error_message)
{
     std::string current = ToLower(command);
     std::unordered_set<std::string> seen;

     while (true)
     {
          if (!seen.insert(current).second)
          {
               error_message = "Alias loop detected for command: " + command;
               return false;
          }

          if (IsBuiltinTalkCommand(current))
          {
               resolved_target = current;
               target_is_collection = false;
               error_message.clear();
               return true;
          }

          if (cli.CollectionExists(current))
          {
               resolved_target = current;
               target_is_collection = true;
               error_message.clear();
               return true;
          }

          const auto alias_it = state.CommandAliases.find(current);

          if (alias_it == state.CommandAliases.end())
          {
               error_message = "Alias target does not exist: " + current;
               return false;
          }

          current = ToLower(alias_it->second);
     }
}

bool WouldCreateAliasLoop(const TalkState &state,
                          const std::string &alias_name,
                          HLQueryCLI &cli,
                          const std::string &target_name)
{
     std::string current = ToLower(target_name);
     std::unordered_set<std::string> seen;
     const std::string lowered_alias_name = ToLower(alias_name);

     while (true)
     {
          if (current == lowered_alias_name)
          {
               return true;
          }

          if (IsBuiltinTalkCommand(current))
          {
               return false;
          }

          if (cli.CollectionExists(current))
          {
               return false;
          }

          const auto alias_it = state.CommandAliases.find(current);

          if (alias_it == state.CommandAliases.end())
          {
               return false;
          }

          if (!seen.insert(current).second)
          {
               return true;
          }

          current = ToLower(alias_it->second);
     }
}

bool FetchServerIdentity(HLQueryCLI &cli, std::string &server_name, std::string &server_id, std::string &error_message)
{
     HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", "/stats");

     if (response.StatusCode != 200)
     {
          error_message = "Failed to get server identity";
          return false;
     }

     try
     {
          nlohmann::json root = nlohmann::json::parse(response.Body);

          if (!root.contains("server") || !root["server"].is_object())
          {
               error_message = "Server identity is not available";
               return false;
          }

          const nlohmann::json &server = root["server"];

          if (!server.contains("name") || !server["name"].is_string() ||
              !server.contains("id") || !server["id"].is_string())
          {
               error_message = "Server identity is incomplete";
               return false;
          }

          server_name = server["name"].get<std::string>();
          server_id = server["id"].get<std::string>();
          return true;
     }
     catch (const std::exception &)
     {
          error_message = "Failed to parse server identity";
          return false;
     }
}

/* Complete the current token using the supplied command candidates. */

int CompleteTalkCandidate(const std::string &current,
                          const std::vector<std::string> &candidates,
                          char *buffer,
                          size_t buffer_size)
{
     const std::string current_lower = ToLower(current);
     std::vector<std::string> matches;

     for (const std::string &command : candidates)
     {
          if (command.rfind(current_lower, 0) == 0)
          {
               matches.push_back(command);
          }
     }

     if (matches.empty())
     {
          return 0;
     }

     std::string completion = matches.front();

     if (matches.size() == 1)
     {
          completion += ' ';
     }
     else
     {
          for (size_t index = 1; index < matches.size(); ++index)
          {
               size_t prefix_length = 0;

               while (prefix_length < completion.size() &&
                      prefix_length < matches[index].size() &&
                      completion[prefix_length] == matches[index][prefix_length])
               {
                    ++prefix_length;
               }

               completion.resize(prefix_length);
          }
     }

     if (completion.size() <= current.size() || completion.size() >= buffer_size)
     {
          return 0;
     }

     std::memcpy(buffer, completion.c_str(), completion.size() + 1);
     return 1;
}

int CompleteTalkCommandLine(const char *line, char *buffer, size_t buffer_size)
{
     if (line == nullptr || buffer == nullptr || buffer_size == 0)
     {
          return 0;
     }

     const std::string current = line;

     if (current.empty())
     {
          return 0;
     }

     if (current.find_first_of(" \t") == std::string::npos)
     {
          return CompleteTalkCandidate(current, GetTalkCommands(), buffer, buffer_size);
     }

     std::istringstream parser(current);
     std::string command;
     std::string subcommand;

     parser >> command;

     if (ToLower(command) != "sam")
     {
          return 0;
     }

     parser >> subcommand;

     std::string remaining;
     std::getline(parser, remaining);

     if (!remaining.empty())
     {
          return 0;
     }

     if (std::isspace(static_cast<unsigned char>(current.back())) != 0)
     {
          subcommand.clear();
     }

     char subcommand_buffer[256] = {0};

     if (CompleteTalkCandidate(subcommand, GetTalkSAMCommands(), subcommand_buffer, sizeof(subcommand_buffer)) == 0)
     {
          return 0;
     }

     const std::string completion = "sam " + std::string(subcommand_buffer);

     if (completion.size() <= current.size() || completion.size() >= buffer_size)
     {
          return 0;
     }

     std::memcpy(buffer, completion.c_str(), completion.size() + 1);
     return 1;
}

bool ExecuteTalkCommand(const std::string &line,
                        HLQueryCLI &cli,
                        TalkState &state,
                        std::string &host,
                        std::string &port)
{
     std::string explicit_sql;

     if (ExtractExplicitSQLStatement(line, explicit_sql))
     {
          if (!HasRecognizedSQLStatementStart(explicit_sql))
          {
               TalkPrintError("Invalid syntax");
               return true;
          }

          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();

          const std::string rewritten_sql = RewriteSQLFromAlias(state, cli, explicit_sql);
          cli.SearchSQL(rewritten_sql, state.CurrentCollection);
          return true;
     }

     const std::vector<std::string> parts = TokenizeInput(line);

     if (parts.empty())
     {
          return true;
     }

     std::string command = ToLower(parts.front());
     const auto alias_it = state.CommandAliases.find(command);

     if (alias_it != state.CommandAliases.end())
     {
          std::string resolved_target;
          bool target_is_collection = false;
          std::string error_message;

          if (!ResolveAliasedTalkTarget(state, cli, command, resolved_target, target_is_collection, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (target_is_collection)
          {
               SetCurrentCollection(state, resolved_target);
               TalkPrintSuccess("Using collection '" + state.CurrentCollection + "'");
               return true;
          }

          command = resolved_target;
     }

     if (command == "exit" || command == "quit")
     {
          return false;
     }

     if (command == "alias" || command == "als")
     {
          if (parts.size() == 1)
          {
               if (state.CommandAliases.empty())
               {
                    TalkPrintInfo("No command aliases defined");
                    return true;
               }

               std::set<std::string> alias_names;

               for (const auto &entry : state.CommandAliases)
               {
                    alias_names.insert(entry.first);
               }

               for (const std::string &alias_name : alias_names)
               {
                    const auto existing_alias = state.CommandAliases.find(alias_name);
                    std::cout << alias_name << " -> " << existing_alias->second << "\n";
               }

               return true;
          }

          if (parts.size() != 3)
          {
               TalkPrintError("Usage: alias [name target]");
               return true;
          }

          const std::string alias_name = ToLower(parts[1]);
          const std::string target_name = ToLower(parts[2]);

          if (IsBuiltinTalkCommand(alias_name))
          {
               TalkPrintError("Cannot replace built-in command: " + alias_name);
               return true;
          }

          if (WouldCreateAliasLoop(state, alias_name, cli, target_name))
          {
               TalkPrintError("Alias loop detected for command: " + alias_name);
               return true;
          }

          std::string resolved_target;
          bool target_is_collection = false;
          std::string error_message;

          if (!ResolveAliasedTalkTarget(state, cli, target_name, resolved_target, target_is_collection, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          state.CommandAliases[alias_name] = target_name;
          SaveTalkAliases(state);
          TalkPrintSuccess("Alias created: " + alias_name + " -> " + target_name);
          return true;
     }

     if (command == "help")
     {
          if (parts.size() == 2 && parts[1] == "sam")
          {
               PrintSAMHelp();
               return true;
          }

          if (parts.size() != 1)
          {
               TalkPrintError("Usage: help or help sam");
               return true;
          }

          PrintHelp();
          return true;
     }

     if (command == "run")
     {
          int repeat_count = 0;
          std::string nested_command;
          std::string error_message;

          if (!ParseRunCommand(line, repeat_count, nested_command, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          for (int iteration = 0; iteration < repeat_count; ++iteration)
          {
               if (!ExecuteTalkCommand(nested_command, cli, state, host, port))
               {
                    return false;
               }
          }

          return true;
     }

     if (command == "uname")
     {
          if (parts.size() > 2 || (parts.size() == 2 && parts[1] != "-a"))
          {
               TalkPrintError("Usage: uname [-a]");
               return true;
          }

          std::string server_name;
          std::string server_id;
          std::string error_message;

          if (!FetchServerIdentity(cli, server_name, server_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          std::cout << server_name << " " << server_id << "\n";
          return true;
     }

     if (command == "id")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: id");
               return true;
          }

          std::string server_name;
          std::string server_id;
          std::string error_message;

          if (!FetchServerIdentity(cli, server_name, server_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          std::cout << server_id << "\n";
          return true;
     }

     if (command == "connect")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: connect [host:port|url]");
               return true;
          }

          std::vector<std::pair<std::string, std::string>> candidates;

          if (parts.size() == 2)
          {
               std::string target_host;
               std::string target_port;
               if (!ParseEndpointValue(parts[1], target_host, target_port))
               {
                    TalkPrintError("Invalid endpoint. Use host:port or http://host:port");
                    return true;
               }

               candidates.emplace_back(target_host, target_port);
          }
          else
          {
               candidates = FetchTalkCandidateEndpoints(cli, host, port);
          }

          if (candidates.empty())
          {
               TalkPrintError("No endpoints available to connect");
               return true;
          }

          bool connected = false;

          for (const auto &candidate : candidates)
          {
               if (ConnectTalkToEndpoint(cli, candidate.first, candidate.second, host, port))
               {
                    connected = true;
                    break;
               }
          }

          if (!connected)
          {
               TalkPrintError("Failed to connect to any available endpoint");
               return true;
          }

          state.CurrentCollection.clear();
          state.CollectionHistory.clear();
          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();
          TalkPrintSuccess("Connected to " + host + ":" + port);
          return true;
     }

     if (command == "use")
     {
          if (parts.size() == 1)
          {
               if (state.CurrentCollection.empty())
               {
                    TalkPrintInfo("No active collection");
               }
               else
               {
                    TalkPrintInfo("Active collection: " + state.CurrentCollection);
               }

               return true;
          }

          const std::string collection_name = parts[1];
          TalkPrintInfo("Selecting collection '" + collection_name + "'");

          std::string resolved_collection_name;
          std::string error_message;

          if (!EnsureCachedCollectionNames(cli, state, collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (!ResolveCollectionReference(collection_name, state.LastListedCollections, resolved_collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          TalkPrintInfo("Verifying collection '" + resolved_collection_name + "'");
          if (!TalkCollectionExists(cli, resolved_collection_name))
          {
               std::string alias_collection;

               if (ResolveTalkAliasCollection(state, cli, resolved_collection_name, alias_collection))
               {
                    resolved_collection_name = alias_collection;
               }
               else
               {
                    TalkPrintError("Collection not found: " + resolved_collection_name);
                    return true;
               }
          }

          SetCurrentCollection(state, resolved_collection_name);
          TalkPrintSuccess("Using collection '" + state.CurrentCollection + "'");
          return true;
     }

     if (command == "lang")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: lang [COL|#]");
               return true;
          }

          std::string collection_name = state.CurrentCollection;

          if (parts.size() == 2)
          {
               collection_name = parts[1];
          }

          if (collection_name.empty())
          {
               TalkPrintError("No active collection. Usage: lang COL|#");
               return true;
          }

          std::string resolved_collection_name;
          std::string error_message;

          if (!EnsureCachedCollectionNames(cli, state, collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (!ResolveCollectionReference(collection_name, state.LastListedCollections, resolved_collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (!TalkCollectionExists(cli, resolved_collection_name))
          {
               std::string alias_collection;

               if (ResolveTalkAliasCollection(state, cli, resolved_collection_name, alias_collection))
               {
                    resolved_collection_name = alias_collection;
               }
               else
               {
                    TalkPrintError("Collection not found: " + resolved_collection_name);
                    return true;
               }
          }

          HLQueryCLI::HTTPResponse response = cli.MakeRequest(
               "GET",
               "/collections/" + hlquery_cli::UrlEncode(resolved_collection_name) + "/lang");

          if (response.StatusCode == 200)
          {
               try
               {
                    nlohmann::json root = nlohmann::json::parse(response.Body);
                    if (root.contains("lang") && root["lang"].is_string())
                    {
                         std::string language = root["lang"].get<std::string>();
                         language = TrimWhitespace(language);
                         if (language.empty())
                         {
                              language = "und";
                         }
                         std::cout << language << "\n";
                         return true;
                    }
               }
               catch (...)
               {
               }
          }

          std::string likely_language;
          if (CollectionLikelyMultiLanguage(cli, resolved_collection_name, likely_language))
          {
               if (likely_language.empty())
               {
                    likely_language = "und";
               }

               std::cout << likely_language << "\n";
               return true;
          }

          response = cli.MakeRequest("GET", "/collections/" + hlquery_cli::UrlEncode(resolved_collection_name));

          if (response.StatusCode == 404)
          {
               TalkPrintError("Collection not found: " + resolved_collection_name);
               return true;
          }
          else if (response.StatusCode != 200)
          {
               TalkPrintError("Request failed (" + std::to_string(response.StatusCode) + ")");
               return true;
          }

          nlohmann::json col;

          try
          {
               col = nlohmann::json::parse(response.Body);
          }
          catch (...)
          {
               TalkPrintError("Failed to parse JSON response");
               return true;
          }

          std::string language = "und";

          if (col.contains("metadata") && col["metadata"].is_object())
          {
               const auto &meta = col["metadata"];

               if (meta.contains("_lang") && meta["_lang"].is_string())
               {
                    language = meta["_lang"].get<std::string>();
               }
          }

          if (language.empty())
          {
               language = "und";
          }

          std::cout << language << "\n";
          return true;
     }

     if (command == "pwd")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: pwd");
               return true;
          }

          std::cout << GetCurrentLocation(state) << "\n";
          return true;
     }

     if (command == "back")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: back");
               return true;
          }

          if (state.CurrentCollection.empty() && state.CollectionHistory.empty())
          {
               TalkPrintError("Already at .");
               return true;
          }

          if (!GoBackCollection(state))
          {
               TalkPrintError("Already at .");
               return true;
          }

          if (state.CurrentCollection.empty())
          {
               TalkPrintInfo("Cleared active collection");
          }
          else
          {
               TalkPrintInfo("Using collection '" + state.CurrentCollection + "'");
          }
          return true;
     }

     if (command == "cd")
     {
          if (parts.size() == 1 || (parts.size() == 2 && parts[1] == ".."))
          {
               if (state.CurrentCollection.empty() && state.CollectionHistory.empty())
               {
                    TalkPrintInfo("Already at .");
                    return true;
               }

               if (!GoBackCollection(state))
               {
                    TalkPrintInfo("Already at .");
                    return true;
               }

               if (state.CurrentCollection.empty())
               {
                    TalkPrintInfo("Cleared active collection");
               }
               else
               {
                    TalkPrintInfo("Using collection '" + state.CurrentCollection + "'");
               }
               return true;
          }

          if (parts.size() != 2)
          {
               TalkPrintError("Usage: cd [COL|#|..]");
               return true;
          }

          std::string collection_name = parts[1];
          std::string resolved_collection_name;
          std::string error_message;

          if (!EnsureCachedCollectionNames(cli, state, collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (!ResolveCollectionReference(collection_name, state.LastListedCollections, resolved_collection_name, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (!TalkCollectionExists(cli, resolved_collection_name))
          {
               TalkPrintError("Collection not found: " + resolved_collection_name);
               return true;
          }

          SetCurrentCollection(state, resolved_collection_name);
          TalkPrintSuccess("Using collection '" + state.CurrentCollection + "'");
          return true;
     }

     if (command == "ls" || command == "l")
     {
          int offset = 0;
          int limit = 1000;
          std::string error_message;

          if (state.CurrentCollection.empty() && parts.size() == 2)
          {
               std::string collection_name;

               if (!ResolveCollectionReference(parts[1], state.LastListedCollections, collection_name, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }

               if (!cli.CollectionExists(collection_name))
               {
                    TalkPrintError("Collection not found: " + collection_name);
                    return true;
               }

               FetchAndPrintDocumentList(cli, state, collection_name, offset, limit);
               return true;
          }

          if (!ParseListRange(parts, offset, limit, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          if (state.CurrentCollection.empty())
          {
               FetchAndPrintCollectionList(cli, state, offset, limit);
               return true;
          }

          FetchAndPrintDocumentList(cli, state, state.CurrentCollection, offset, limit);
          return true;
     }

     if (command == "aliases")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: aliases [collection]");
               return true;
          }

          const std::string collection_name = parts.size() == 2 ? parts[1] : "";
          ListAliases(cli, collection_name);
          return true;
     }

     if (command == "show" && parts.size() >= 2 && (parts[1] == "cols" || parts[1] == "collections"))
     {
          std::vector<std::string> list_parts = {"ls"};
          for (size_t i = 2; i < parts.size(); ++i)
          {
               list_parts.push_back(parts[i]);
          }

          int offset = 0;
          int limit = 1000;
          std::string error_message;

          if (!ParseListRange(list_parts, offset, limit, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          FetchAndPrintCollectionList(cli, state, offset, limit);
          return true;
     }

     if (command == "algorithm")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: algorithm");
               return true;
          }

          HLQueryCLI::HTTPResponse response = cli.MakeRequest("GET", "/search-config");

          if (response.StatusCode != 200)
          {
               TalkPrintError("Failed to get search configuration");
               return true;
          }

          try
          {
               nlohmann::json root = nlohmann::json::parse(response.Body);
               const std::string algorithm = root.contains("algorithm") && root["algorithm"].is_string() ? root["algorithm"].get<std::string>() : "unknown";
               const std::string default_ranking = root.contains("default_ranking") && root["default_ranking"].is_string() ? root["default_ranking"].get<std::string>() : "unknown";

               std::cout << "algorithm: " << algorithm << "\n";
               std::cout << "default_ranking: " << default_ranking << "\n";
          }
          catch (const std::exception &)
          {
               TalkPrintError("Failed to parse search configuration response");
          }

          return true;
     }

     if (command == "delete")
     {
          if (parts.size() != 2)
          {
               TalkPrintError(state.CurrentCollection.empty() ? "Usage: delete <collection-name|number>" : "Usage: delete <document-id|number>");
               return true;
          }

          std::string error_message;

          if (state.CurrentCollection.empty())
          {
               std::string collection_name;

               if (!ResolveCollectionReference(parts[1], state.LastListedCollections, collection_name, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }

               std::string resolved_collection_name = collection_name;

               if (!cli.CollectionExists(resolved_collection_name))
               {
                    std::string alias_collection;

                    if (ResolveTalkAliasCollection(state, cli, resolved_collection_name, alias_collection))
                    {
                         resolved_collection_name = alias_collection;
                    }
               }

               const bool existed_before = cli.CollectionExists(resolved_collection_name);
               cli.DeleteCollection(resolved_collection_name);
               if (existed_before && !cli.CollectionExists(resolved_collection_name))
               {
                    PruneAliasesForDeletedCollection(state, resolved_collection_name);
                    SaveTalkAliases(state);
               }
               state.LastListedCollections.clear();
               return true;
          }

          std::string document_id;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          DeleteLocalDocument(cli, state.CurrentCollection, document_id);
          state.LastListedDocumentIds.clear();
          return true;
     }

     if (command == "migrate")
     {
          if (parts.size() < 3 || parts.size() > 4)
          {
               TalkPrintError("Usage: migrate <collection-name|number> <new-collection-name> [--drop-old]");
               return true;
          }

          std::string source_collection;
          std::string error_message;

          if (!ResolveCollectionReference(parts[1], state.LastListedCollections, source_collection, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          bool drop_old = false;

          if (parts.size() == 4)
          {
               if (parts[3] != "--drop-old")
               {
                    TalkPrintError("Usage: migrate <collection-name|number> <new-collection-name> [--drop-old]");
                    return true;
               }

               drop_old = true;
          }

          cli.MigrateCollection(source_collection, parts[2], drop_old);
          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();

          if (state.CurrentCollection == source_collection && drop_old)
          {
               state.CurrentCollection.clear();
               state.CollectionHistory.clear();
          }

          return true;
     }

     if (command == "copy")
     {
          if (state.CurrentCollection.empty())
          {
               if (parts.size() != 3)
               {
                    TalkPrintError("Usage: copy <collection-name|number> <new-collection-name>");
                    return true;
               }

               std::string source_collection;
               std::string error_message;

               if (!ResolveCollectionReference(parts[1], state.LastListedCollections, source_collection, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }

               cli.CopyCollection(source_collection, parts[2]);
               state.LastListedCollections.clear();
               state.LastListedDocumentIds.clear();
               return true;
          }

          if (parts.size() != 3)
          {
               TalkPrintError("Usage: copy <document-id|number> <new-document-id>");
               return true;
          }

          std::string source_document_id;
          std::string error_message;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, source_document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          cli.CopyDocument(state.CurrentCollection, source_document_id, parts[2]);
          state.LastListedDocumentIds.clear();
          return true;
     }

     if (command == "search")
     {
          if (parts.size() < 2)
          {
               TalkPrintError("Usage: search <query> [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]");
               return true;
          }

          std::string query;
          std::string server_query;
          SearchTalkOptions options;
          size_t options_start_index = 0;
          bool phrase_query = false;

          if (state.CurrentCollection.empty())
          {
               std::string explicit_collection_name;
               std::string reference_error;
               bool explicit_document_search = false;

               if (parts.size() >= 3 &&
                   parts[2].rfind("--", 0) != 0 &&
                   !IsUnsignedInteger(parts[2]) &&
                   ResolveCollectionReference(parts[1], state.LastListedCollections, explicit_collection_name, reference_error))
               {
                    std::string aliased_collection_name;

                    if (ResolveTalkAliasCollection(state, cli, explicit_collection_name, aliased_collection_name))
                    {
                         explicit_collection_name = aliased_collection_name;
                    }

                    if (cli.CollectionExists(explicit_collection_name))
                    {
                         if (!ExtractSearchTalkQuery(parts, 2, query, options_start_index, phrase_query))
                         {
                              TalkPrintError("Usage: search COL|# QUERY [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]");
                              return true;
                         }

                         ParseSearchTalkOptions(parts, options_start_index, options);
                         explicit_document_search = true;
                    }
               }

               if (explicit_document_search)
               {
                    if (options.Distributed.empty())
                    {
                         options.Distributed = "off";
                    }

                    server_query = BuildTalkQueryForServer(query, phrase_query);
                    state.LastListedCollections.clear();
                    state.LastListedDocumentIds.clear();
                    cli.SearchDocuments(explicit_collection_name, server_query, options.Limit, options.Offset, options.Sort, options.ExactMatch, options.Highlight, options.HighlightFields, options.Distributed, options.Route, 30, options.MaybeMin, options.MaybeLimit, options.JsonOutput);
                    return true;
               }

               if (!ExtractSearchTalkQuery(parts, 1, query, options_start_index, phrase_query))
               {
                    TalkPrintError("Usage: search <query> [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]");
                    return true;
               }

               ParseSearchTalkOptions(parts, options_start_index, options);

               if (options.Distributed.empty())
               {
                    options.Distributed = "off";
               }

               server_query = BuildTalkQueryForServer(query, phrase_query);
               state.LastListedCollections = FetchSearchCollectionNames(cli, query, options);
               state.LastListedDocumentIds.clear();
               cli.SearchCollections(server_query, options.Limit, options.Offset, options.Sort, options.Distributed, options.Route, options.MaybeMin, options.MaybeLimit, options.JsonOutput);
               return true;
          }

          if (!ExtractSearchTalkQuery(parts, 1, query, options_start_index, phrase_query))
          {
               TalkPrintError("Usage: search <query> [limit] [offset] [sort] [--exact] [--highlight] [--fields=f1,f2] [--maybe=min,limit] [--json]");
               return true;
          }

          ParseSearchTalkOptions(parts, options_start_index, options);

          if (options.Distributed.empty())
          {
               options.Distributed = "off";
          }

          server_query = BuildTalkQueryForServer(query, phrase_query);
          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();
          cli.SearchDocuments(state.CurrentCollection, server_query, options.Limit, options.Offset, options.Sort, options.ExactMatch, options.Highlight, options.HighlightFields, options.Distributed, options.Route, 30, options.MaybeMin, options.MaybeLimit, options.JsonOutput);
          return true;
     }

     if (command == "exec")
     {
          std::string sql;

          if (parts.size() == 2)
          {
               sql = parts[1];
          }
          else
          {
               sql = TrimWhitespace(line.substr(parts.front().size()));
          }

          if (sql.empty())
          {
               TalkPrintError("Usage: exec <SQL statement>");
               return true;
          }

          if (!HasRecognizedSQLStatementStart(sql))
          {
               TalkPrintError("Invalid syntax");
               return true;
          }

          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();
          cli.SearchSQL(sql, state.CurrentCollection);
          return true;
     }

     if (command == "open")
     {
          if (state.CurrentCollection.empty())
          {
               TalkPrintError("No active collection. Use 'use <collection>' first");
               return true;
          }

          if (parts.size() != 2)
          {
               TalkPrintError("Usage: open <document-id|number>");
               return true;
          }

          std::string document_id;
          std::string error_message;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          cli.OpenDocument(state.CurrentCollection, document_id);
          return true;
     }

     if (command == "select")
     {
          if (parts.size() == 1)
          {
               PrintSelectSyntaxForLine(line);
               return true;
          }

          if (state.CurrentCollection.empty())
          {
               TalkPrintError("No active collection. Use 'use <collection>' first");
               return true;
          }

          if (parts.size() != 3)
          {
               PrintSelectSyntaxForLine(line);
               return true;
          }

          std::string document_id;
          std::string error_message;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          cli.SelectField(parts[2], state.CurrentCollection, document_id);
          return true;
     }

     if (command == "update")
     {
          if (state.CurrentCollection.empty())
          {
               TalkPrintError("No active collection. Use 'use <collection>' first");
               return true;
          }

          if (parts.size() != 4)
          {
               TalkPrintError("Usage: update <document-id|number> <field> <value>");
               return true;
          }

          std::string document_id;
          std::string error_message;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          cli.UpdateDocumentField(state.CurrentCollection, document_id, parts[2], parts[3]);
          return true;
     }

     if (command == "count")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: count [collection-name|number]");
               return true;
          }

          std::string collection_name;
          std::string error_message;

          if (parts.size() == 1)
          {
               if (state.CurrentCollection.empty())
               {
                    TalkPrintError("Usage: count <collection-name|number> or use 'use <collection>' first");
                    return true;
               }

               collection_name = state.CurrentCollection;
          }
          else
          {
               if (!ResolveCollectionReference(parts[1], state.LastListedCollections, collection_name, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }
          }

          if (!cli.CollectionExists(collection_name))
          {
               TalkPrintError("Collection not found: " + collection_name);
               return true;
          }

          PrintCollectionDocumentCount(cli, collection_name);
          return true;
     }

     if (command == "maybe")
     {
          if (parts.size() < 2)
          {
               TalkPrintError("Usage: maybe <query> [collection] [limit] [min_results]");
               return true;
          }

          std::string query_str;
          std::string collection_name = state.CurrentCollection;
          int limit_val = 5;
          int min_results_val = 5;
          size_t next_index = 2;

          if (parts.size() >= 3)
          {
               const bool first_is_collection = cli.CollectionExists(parts[1]);
               const bool second_is_collection = cli.CollectionExists(parts[2]);

               if (first_is_collection && !second_is_collection)
               {
                    collection_name = parts[1];
                    query_str = parts[2];
                    next_index = 3;
               }
               else
               {
                    query_str = parts[1];
                    collection_name = parts[2];
               }
          }
          else
          {
               query_str = parts[1];
          }

          if (query_str.empty())
          {
               TalkPrintError("Usage: maybe <query> [collection] [limit] [min_results]");
               return true;
          }

          if (collection_name.empty())
          {
               TalkPrintError("No active collection. Use 'use <collection>' first or pass a collection name.");
               return true;
          }

          if (next_index < parts.size())
          {
               try
               {
                    limit_val = std::stoi(parts[next_index]);
               }
               catch (...)
               {
                    TalkPrintError("Invalid limit: " + parts[next_index]);
                    return true;
               }
               ++next_index;
          }

          if (next_index < parts.size())
          {
               try
               {
                    min_results_val = std::stoi(parts[next_index]);
               }
               catch (...)
               {
                    TalkPrintError("Invalid min_results: " + parts[next_index]);
                    return true;
               }
               ++next_index;
          }

          if (next_index < parts.size())
          {
               TalkPrintError("Usage: maybe <query> [collection] [limit] [min_results]");
               return true;
          }

          cli.MaybeSuggest(query_str, collection_name, limit_val, min_results_val, false);
          return true;
     }

     if (command == "sam")
     {
          const std::vector<std::string> raw_parts = TokenizeInputPreserveQuotes(line);

          if (parts.size() >= 2 && parts[1] == "help")
          {
               if (parts.size() != 2)
               {
                    TalkPrintError("Usage: sam help");
                    return true;
               }

               PrintSAMHelp();
               return true;
          }

	          if (parts.size() >= 2 && (parts[1] == "run" || parts[1] == "index"))
	          {
	               std::string collection_token;

	               if (parts[1] == "run")
	               {
	                    if (parts.size() != 3)
	                    {
	                         TalkPrintError("Usage: sam run <collection>");
	                         return true;
	                    }

	                    collection_token = parts[2];
	               }
	               else if (parts.size() == 2)
	               {
	                    if (state.CurrentCollection.empty())
	                    {
	                         TalkPrintError("No active collection. Use 'use <collection>' first or pass a collection name.");
	                         return true;
	                    }

	                    collection_token = state.CurrentCollection;
	               }
	               else if (parts.size() == 3)
	               {
	                    collection_token = parts[2];
	               }
	               else
	               {
	                    TalkPrintError("Usage: sam index [collection]");
	                    return true;
	               }

	               std::string collection_name;
	               std::string error_message;

	               if (!ResolveSAMCollectionReference(state, cli, collection_token, collection_name, error_message))
	               {
	                    TalkPrintError(error_message);
	                    return true;
               }

               cli.RebuildSAMCollection(collection_name);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "search")
          {
               std::string collection_name;
               std::string query_text;
               int limit_val = 20;
               std::string error_message;
               size_t query_start_index = 0;
               size_t query_end_index = parts.size();

               if (state.CurrentCollection.empty())
               {
                    if (parts.size() < 4)
                    {
                         TalkPrintError("Usage: sam search <collection> <query> [limit]");
                         return true;
                    }

                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    query_start_index = 3;
               }
               else
               {
                    if (parts.size() < 3)
                    {
                         TalkPrintError("Usage: sam search <query> [limit]");
                         return true;
                    }

                    collection_name = state.CurrentCollection;
                    query_start_index = 2;
               }

               if ((query_end_index - query_start_index) >= 2 && IsUnsignedInteger(parts[query_end_index - 1]))
               {
                    limit_val = std::stoi(parts[query_end_index - 1]);
                    query_end_index--;
               }

               if (query_end_index <= query_start_index)
               {
                    TalkPrintError(state.CurrentCollection.empty()
                         ? "Usage: sam search <collection> <query> [limit]"
                         : "Usage: sam search <query> [limit]");
                    return true;
               }

               if ((query_end_index - query_start_index) == 1 &&
                   raw_parts.size() > query_start_index &&
                   IsQuotedToken(raw_parts[query_start_index]))
               {
                    query_text = StripOuterQuotes(raw_parts[query_start_index]);
               }
               else
               {
                    for (size_t index = query_start_index; index < query_end_index; ++index)
                    {
                         if (!query_text.empty())
                         {
                              query_text.push_back(' ');
                         }

                         query_text += parts[index];
                    }
               }

               query_text = TrimWhitespace(query_text);

               if (limit_val <= 0)
               {
                    TalkPrintError("SAM search limit must be a positive integer");
                    return true;
               }

               state.LastSAMSearchCollection = collection_name;
               state.LastSAMSearchQuery = query_text;
               state.LastListedDocumentIds.clear();
               cli.SearchSAM(collection_name,
                             query_text,
                             limit_val,
                             false,
                             false,
                             {},
                             "",
                             "",
                             false,
                             &state.LastListedSAMDocumentIds,
                             false,
                             false);
               return true;
          }

	          if (parts.size() >= 2 && (parts[1] == "status" || parts[1] == "stats"))
	          {
	               std::string collection_name;
	               std::string error_message;

               if (parts.size() == 2)
               {
                    collection_name = state.CurrentCollection;
               }
               else if (parts.size() == 3)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }
               }
	               else
	               {
	                    TalkPrintError("Usage: sam status [collection]");
	                    return true;
	               }

               cli.ShowSAMStatus(collection_name);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "history")
          {
               std::string collection_name;
               std::string error_message;
               int limit = 100;

               if (parts.size() == 2)
               {
                    collection_name = state.CurrentCollection;
               }
               else if (parts.size() == 3)
               {
                    if (IsUnsignedInteger(parts[2]))
                    {
                         limit = std::stoi(parts[2]);
                         collection_name = state.CurrentCollection;
                    }
                    else if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }
               }
               else if (parts.size() == 4)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]))
                    {
                         TalkPrintError("SAM history limit must be a positive integer");
                         return true;
                    }

                    limit = std::stoi(parts[3]);
               }
               else
               {
                    TalkPrintError("Usage: sam history [collection] [limit]");
                    return true;
               }

               cli.ShowSAMHistory(collection_name, limit);
               return true;
          }

          if (parts.size() >= 2 &&
              (parts[1] == "int" || parts[1] == "inst" || parts[1] == "interactions"))
          {
               std::string collection_name;
               std::string error_message;
               int limit = 100;

               if (parts.size() == 2)
               {
                    collection_name = state.CurrentCollection;
               }
               else if (parts.size() == 3)
               {
                    if (IsUnsignedInteger(parts[2]))
                    {
                         limit = std::stoi(parts[2]);
                         collection_name = state.CurrentCollection;
                    }
                    else if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }
               }
               else if (parts.size() == 4)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]))
                    {
                         TalkPrintError("SAM interaction limit must be a positive integer");
                         return true;
                    }

                    limit = std::stoi(parts[3]);
               }
               else
               {
                    TalkPrintError("Usage: sam int [collection] [limit]");
                    return true;
               }

               cli.ShowSAMHistory(collection_name, limit, false, true);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "improve")
          {
               size_t limit = 0;
               bool force = false;

               for (size_t index = 2; index < parts.size(); ++index)
               {
                    if (parts[index] == "--force")
                    {
                         force = true;
                         continue;
                    }

                    if (!IsUnsignedInteger(parts[index]))
                    {
                         TalkPrintError("Usage: sam improve [limit] [--force]");
                         return true;
                    }

                    limit = static_cast<size_t>(std::stoull(parts[index]));
               }

               cli.ImproveSAM(limit, force);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "last")
          {
               std::string collection_name;
               int limit = 20;
               std::string error_message;

               if (parts.size() == 3)
               {
                    if (IsUnsignedInteger(parts[2]))
                    {
                         limit = std::stoi(parts[2]);
                    }
                    else if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }
               }
               else if (parts.size() == 4)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]) || std::stoi(parts[3]) <= 0)
                    {
                         TalkPrintError("SAM last limit must be a positive integer");
                         return true;
                    }

                    limit = std::stoi(parts[3]);
               }
               else if (parts.size() > 4)
               {
                    TalkPrintError("Usage: sam last [collection] [limit]");
                    return true;
               }

               if (limit <= 0)
               {
                    limit = 20;
               }

               ShowSAMLast(cli, collection_name, limit);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "debug")
          {
               std::string collection_name;
               int limit = 100;
               std::string error_message;

               if (parts.size() == 2)
               {
                    if (state.CurrentCollection.empty())
                    {
                         TalkPrintError("Usage: sam debug [collection] [limit]");
                         return true;
                    }

                    collection_name = state.CurrentCollection;
               }
               else if (parts.size() == 3)
               {
                    if (IsUnsignedInteger(parts[2]))
                    {
                         if (state.CurrentCollection.empty())
                         {
                              TalkPrintError("Usage: sam debug [collection] [limit]");
                              return true;
                         }

                         collection_name = state.CurrentCollection;
                         limit = std::stoi(parts[2]);
                    }
                    else
                    {
                         if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                         {
                              TalkPrintError(error_message);
                              return true;
                         }
                    }
               }
               else if (parts.size() == 4)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]) || std::stoi(parts[3]) <= 0)
                    {
                         TalkPrintError("SAM debug limit must be a positive integer");
                         return true;
                    }

                    limit = std::stoi(parts[3]);
               }
               else
               {
                    TalkPrintError("Usage: sam debug [collection] [limit]");
                    return true;
               }

               if (limit <= 0)
               {
                    limit = 100;
               }

               StreamSAMDebug(cli, collection_name, limit);
               return true;
          }

          if (parts.size() >= 2 && (parts[1] == "ls" || parts[1] == "list"))
          {
               std::string collection_name;
               int offset = 0;
               int limit = 20;
               std::string error_message;

               if (parts.size() == 2)
               {
                    if (state.CurrentCollection.empty())
                    {
                         TalkPrintError("Usage: sam list [collection] [offset limit]");
                         return true;
                    }

                    collection_name = state.CurrentCollection;
               }
               else if (parts.size() == 3)
               {
                    if (IsUnsignedInteger(parts[2]))
                    {
                         if (state.CurrentCollection.empty())
                         {
                              TalkPrintError("Usage: sam list [collection] [offset limit]");
                              return true;
                         }

                         collection_name = state.CurrentCollection;
                         offset = std::stoi(parts[2]);
                    }
                    else
                    {
                         if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                         {
                              TalkPrintError(error_message);
                              return true;
                         }
                    }
               }
               else if (parts.size() == 4)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]))
                    {
                         TalkPrintError("SAM list offset must be a non-negative integer");
                         return true;
                    }

                    offset = std::stoi(parts[3]);
               }
               else if (parts.size() == 5)
               {
                    if (!ResolveSAMCollectionReference(state, cli, parts[2], collection_name, error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    if (!IsUnsignedInteger(parts[3]) || !IsUnsignedInteger(parts[4]))
                    {
                         TalkPrintError("SAM list offset and limit must be non-negative integers");
                         return true;
                    }

                    offset = std::stoi(parts[3]);
                    limit = std::stoi(parts[4]);
               }
               else
               {
                    TalkPrintError("Usage: sam list [collection] [offset limit]");
                    return true;
               }

               state.LastListedSAMDocumentIds = FetchSAMDocumentIds(cli, collection_name, offset, limit);
               state.LastSAMSearchCollection.clear();
               state.LastSAMSearchQuery.clear();
               cli.ListSAMDocuments(collection_name, offset, limit);
               return true;
          }

          if (parts.size() >= 2 && parts[1] == "open")
          {
               if (parts.size() != 3)
               {
                    TalkPrintError("Usage: sam open <document-id> or sam open <collection>/<document-id>");
                    return true;
               }

               std::string collection_name;
               std::string document_id;
               std::string error_message;

               if (!ParseSAMDocumentTarget(parts[2], state.CurrentCollection, collection_name, document_id, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }

               std::string resolved_collection;

               if (!ResolveSAMCollectionReference(state, cli, collection_name, resolved_collection, error_message))
               {
                    TalkPrintError(error_message);
                    return true;
               }

               if (IsUnsignedInteger(document_id) &&
                   !state.LastListedSAMDocumentIds.empty())
               {
                    std::string resolved_document_id;
                    if (!ResolveSAMDocumentReference(cli,
                                                     resolved_collection,
                                                     document_id,
                                                     state.LastListedSAMDocumentIds,
                                                     resolved_document_id,
                                                     error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    document_id = resolved_document_id;
               }
               else if (IsUnsignedInteger(document_id) &&
                        resolved_collection == state.CurrentCollection)
               {
                    std::string resolved_document_id;

                    if (!ResolveCollectionDocumentReference(cli,
                                                            resolved_collection,
                                                            document_id,
                                                            state.LastListedDocumentIds,
                                                            resolved_document_id,
                                                            error_message))
                    {
                         TalkPrintError(error_message);
                         return true;
                    }

                    document_id = resolved_document_id;
               }

               std::string interaction_query;

               if (resolved_collection == state.LastSAMSearchCollection)
               {
                    interaction_query = state.LastSAMSearchQuery;
               }

               cli.OpenSAMDocument(resolved_collection, document_id, false, interaction_query);
               return true;
          }

          if (state.CurrentCollection.empty())
          {
               TalkPrintError("No active collection. Use 'use <collection>' first");
               return true;
          }

          if (parts.size() != 2)
          {
               TalkPrintError("Usage: sam <document-id|number>");
               return true;
          }

          std::string document_id;
          std::string error_message;

          if (!ResolveCollectionDocumentReference(cli, state.CurrentCollection, parts[1], state.LastListedDocumentIds, document_id, error_message))
          {
               TalkPrintError(error_message);
               return true;
          }

          cli.ShowDocumentContext(state.CurrentCollection, document_id);
          return true;
     }

     if (command == "stats")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: stats");
               return true;
          }

          cli.ShowStatus();
          return true;
     }

     if (command == "ping")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: ping");
               return true;
          }

          cli.ShowPing();
          return true;
     }

     if (command == "links")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: links");
               return true;
          }

          cli.ShowLinks(false);
          return true;
     }

     if (command == "bw")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: bw [kb|mb|gb]");
               return true;
          }

          std::string unit = "mb";

          if (parts.size() == 2)
          {
               unit = ToLower(parts[1]);

               if (unit != "kb" && unit != "mb" && unit != "gb")
               {
                    TalkPrintError("Usage: bw [kb|mb|gb]");
                    return true;
               }
          }

          cli.ShowTransferStats(unit);
          return true;
     }

     if (command == "dbsize")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: dbsize [kb|mb|gb]");
               return true;
          }

          std::string unit = "mb";

          if (parts.size() == 2)
          {
               unit = ToLower(parts[1]);

               if (unit != "kb" && unit != "mb" && unit != "gb")
               {
                    TalkPrintError("Usage: dbsize [kb|mb|gb]");
                    return true;
               }
          }

          cli.ShowDatabaseSize(unit);
          return true;
     }

     if (command == "modules")
     {
          if (parts.size() > 2)
          {
               TalkPrintError("Usage: modules [1|0]");
               return true;
          }

          if (parts.size() == 2 && parts[1] != "1" && parts[1] != "0")
          {
               TalkPrintError("Usage: modules [1|0]");
               return true;
          }

          cli.ListModules(parts.size() == 2 ? parts[1] : "");
          return true;
     }

     if (command == "llm")
     {
          if (parts.size() == 1)
          {
               cli.ShowLLMInfo();
               return true;
          }

          if (state.CurrentCollection.empty())
          {
               TalkPrintError("Usage: llm <question> requires an active collection");
               TalkPrintInfo("Select one first with: use <collection>");
               return true;
          }

          std::string query = NormalizeTalkLLMQuery(line.substr(parts.front().size()));

          if (query.empty())
          {
               TalkPrintError("Usage: llm <question>");
               return true;
          }

          const int limit = 20;

          state.LastSAMSearchCollection = state.CurrentCollection;
          state.LastSAMSearchQuery = query;
          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();

          bool found = cli.SearchSAM(state.CurrentCollection,
                                     query,
                                     limit,
                                     false,
                                     false,
                                     {},
                                     "",
                                     "",
                                     false,
                                     &state.LastListedDocumentIds,
                                     true,
                                     true,
                                     true);

          state.LastListedSAMDocumentIds = state.LastListedDocumentIds;

          if (found)
          {
               TalkPrintInfo("Use 'open 1' or 'sam open 1' to inspect a result.");
          }

          return true;
     }

     if (command == "see")
     {
          if (parts.size() == 1)
          {
               ShowSAMSearchJobs(cli, state, "all");
               return true;
          }

          if (parts.size() == 2)
          {
               ShowSAMSearchJobs(cli, state, parts[1]);
               return true;
          }

          TalkPrintError("Usage: see [all|job-id]");
          return true;
     }

     if (command == "module")
     {
          if (parts.size() < 2)
          {
               TalkPrintError("Usage: module <name> [info|syntax|<route> [args...]]");
               return true;
          }

          const std::string module_name = parts[1];

          if (module_name == "info" && parts.size() == 2)
          {
               cli.ListModules();
               return true;
          }

          if (parts.size() == 2 || parts[2] == "info" || parts[2] == "syntax")
          {
               cli.ShowModuleSyntax(module_name);
               return true;
          }

          std::vector<std::string> module_args;

          for (size_t index = 3; index < parts.size(); ++index)
          {
               module_args.push_back(parts[index]);
          }

          cli.RunModuleCommand(module_name, parts[2], module_args);
          return true;
     }

     if (command == "load" || command == "loadmodule")
     {
          if (parts.size() != 2)
          {
               TalkPrintError((command == "loadmodule") ? "Usage: loadmodule <name>" : "Usage: load <name>");
               return true;
          }

          cli.LoadModule(parts[1]);
          return true;
     }

     if (command == "unload" || command == "unloadmodule")
     {
          if (parts.size() != 2)
          {
               TalkPrintError((command == "unloadmodule") ? "Usage: unloadmodule <name>" : "Usage: unload <name>");
               return true;
          }

          cli.UnloadModule(parts[1]);
          return true;
     }

     if (command == "flush")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: flush");
               return true;
          }

          cli.FlushAll(true);
          state.LastListedCollections.clear();
          state.LastListedDocumentIds.clear();
          return true;
     }

     if (command == "reset")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: reset");
               return true;
          }

          std::cout << "\x1b[2J\x1b[H";
          std::cout.flush();
          return true;
     }

     if (command == "unalias")
     {
          if (parts.size() != 2)
          {
               TalkPrintError("Usage: unalias <name>");
               return true;
          }

          const std::string alias_name = ToLower(parts[1]);
          const auto existing_alias = state.CommandAliases.find(alias_name);

          if (existing_alias == state.CommandAliases.end())
          {
               TalkPrintError("Alias not found: " + alias_name);
               return true;
          }

          state.CommandAliases.erase(existing_alias);
          SaveTalkAliases(state);
          TalkPrintSuccess("Alias removed: " + alias_name);
          return true;
     }

     if (command == "uptime")
     {
          if (parts.size() != 1)
          {
               TalkPrintError("Usage: uptime");
               return true;
          }

          cli.ShowUptime(false);
          return true;
     }

     TalkPrintError("Unknown command: " + parts.front());
     return true;
}
