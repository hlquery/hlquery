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

#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

class HLQueryCLI
{
   public:

     /* HTTPResponse struct represents a response from the server. */

     struct HTTPResponse
     {
          int StatusCode;

          std::string Body;

          std::map<std::string, std::string> Headers;
     };

     HLQueryCLI(const std::string &url = "http://localhost:9200",
                bool raw = false,
                const std::string &token = "",
                const std::string &program_name = "hlquery-cli",
                bool ssl_auth = false);

     /* Gets the exit code. */

     int GetExitCode() const;

     /* Gets the program display name. */

     const std::string &GetProgramName() const;

     /* Makes an HTTP request. */

     HTTPResponse MakeRequest(const std::string &method, const std::string &path, const std::string &body = "", int timeout_seconds = -1);

     /* Enables or disables request dry-run mode for debugging. */

     void SetRequestDryRunMode(bool enabled, bool print_curl = false);

     /* Lists collections. */

     void ListCollections(int offset = 0, int limit = 10000, bool json_output = false);

     /* Searches collection names. */

     void SearchCollections(const std::string &query, int limit = 10000, int offset = 0, const std::string &sort = "name:asc", const std::string &distributed = "", const std::string &route = "", int maybe_min = -1, int maybe_limit = -1, bool json_output = false);

     /* Shows information about a collection. */

     void ShowCollectionInfo(const std::string &collection_name);

     /* Prints detected language for a collection. */

     void ShowCollectionLanguage(const std::string &collection_name, bool json_output = false);

     /* Shows collection or document information depending on the target string. */

     void ShowInfo(const std::string &target);

     /* Shows information for a specific document. */

     void ShowDocumentInfo(const std::string &collection_name, const std::string &document_id);

     /* Lists documents in a collection. */

     void ListDocuments(const std::string &collection_name, int offset = 0, int limit = 20);

     /* Gets available fields in a collection. */

     std::string GetAvailableFields(const std::string &collection_name);

     /* Shows transfer statistics. */

     void ShowTransferStats(const std::string &unit = "kb");

     /* Shows active connections. */

     void ShowConnections();

     /* Shows LSM information. */

     void ShowLSM();

     /* Shows database size. */

     void ShowDatabaseSize(const std::string &unit = "mb");

     /* Shows uptime. */

     void ShowUptime(bool detailed_format = false);

     /* Shows total document count and checks consistency. */

     void ShowDocTotal(int offset = 0, int limit = 0);

     /* Rebuilds document counters. */

     void RebuildCounters(const std::string &collection_name = "", bool rebuild_index = false);

     /* Prints per-collection WAL statistics. */

     void ShowWALStats(const std::string &collection_name = "");

     /* Shows ping. */

     void ShowPing();

     /* Shows distributed links from the connected server. */

     void ShowLinks(bool ping_all = false);

     /* Shows server status. */

     void ShowStatus();

     /* Runs a compact diagnostics sweep against the configured server. */

     void ShowDoctor();

     /* Shows advanced server information. */

     void ShowAdvanced();

     /* Checks if data is too messy for a table. */

     bool IsDataTooMessyForTable(const nlohmann::json &doc);

     /* Opens a document. */

     void OpenDocument(const std::string &collection_name, const std::string &document_id, const std::string &format = "table", const std::string &route = "");

     /* Selects a field from a document. */

     void SelectField(const std::string &field_names, const std::string &collection_name, const std::string &document_id);

     /* Updates a single field in a document. */

     void UpdateDocumentField(const std::string &collection_name, const std::string &document_id, const std::string &field_name, const std::string &field_value);

     /* Searches documents in a collection. */

     void SearchDocuments(const std::string &collection_name, const std::string &query, int limit = 10000, int offset = 0, const std::string &sort = "", bool exact_match = false, bool highlight = false, const std::string &highlight_fields = "", const std::string &distributed = "", const std::string &route = "", int snippet_threshold = 30, int maybe_min = -1, int maybe_limit = -1, bool json_output = false);

     /* Executes a SQL-style search against the daemon. */

     void SearchSQL(const std::string &sql, const std::string &collection_name = "", bool json_output = false);

     /* Suggests likely intended phrases for a query in a collection. */

     void MaybeSuggest(const std::string &query, const std::string &collection_name, int limit = 5, int min_results = 3, bool json_output = false);

     /* Shows contextual lookup phrases for one document. */

     void ShowDocumentContext(const std::string &collection_name, const std::string &document_id, bool json_output = false);
     /* Searches across multiple collections via the global search endpoint. */

     void SearchAcrossCollections(const std::string &query, const std::vector<std::string> &collections, int limit = 10000, int offset = 0, const std::string &sort = "", bool exact_match = false, bool highlight = false, const std::string &highlight_fields = "", const std::string &distributed = "", const std::string &route = "", bool distributed_collections = false, int maybe_min = -1, int maybe_limit = -1, bool json_output = false);

     /* Performs a vector search. */

     void VectorSearch(const std::string &collection_name, const std::string &vector_str, const std::string &field_name = "embedding", int limit = 10, bool json_output = false);

     /* Lists synonym counts across collections. */

     void ListSynonymsCounts();

     /* Lists synonyms. */

     void ListSynonyms(const std::string &collection_name = "");

     /* Adds a synonym. */

     void AddSynonym(const std::string &collection_name, const std::string &synonym_id, const std::string &root_term, const std::vector<std::string> &synonyms);

     /* Deletes a synonym. */

     void DeleteSynonym(const std::string &collection_name, const std::string &synonym_id);

     /* Lists stopword counts across collections. */

     void ListStopwordsCounts();

     /* Lists stopwords. */

     void ListStopwords(const std::string &collection_name = "");

     /* Adds a stopword. */

     void AddStopword(const std::string &collection_name, const std::string &word);

     /* Deletes a stopword. */

     void DeleteStopword(const std::string &collection_name, const std::string &word);

     /* Creates a collection. */

     void CreateCollection(const std::string &name, const std::vector<std::string> &searchable_fields, const std::vector<std::string> &filterable_fields = {}, const std::vector<std::string> &sortable_fields = {});

     /* Migrates one collection into a new collection name by copying schema and documents. */

     void MigrateCollection(const std::string &source_name, const std::string &target_name, bool drop_source = false);

     /* Copies one collection into a new collection name (schema + documents). */

     void CopyCollection(const std::string &source_name, const std::string &target_name);

     /* Confirms a destructive action. */

     bool ConfirmDestructiveAction(const std::string &action, const std::string &target);

     /* Deletes a collection. */

     void DeleteCollection(const std::string &name);

     /* Deletes a document. */

     void DeleteDocument(const std::string &collection_name, const std::string &id);

     /* API Key management. */

     /* Lists API keys. */

     void ListKeys();

     /* Creates an API key. */

     void CreateKey(const std::string &description, const std::vector<std::string> &collections, const std::vector<std::string> &actions, int expires_at = 0, const std::string &embedded_filters = "", bool allow_hanalyzer = false);

     /* Deletes an API key. */

     void DeleteKey(const std::string &key_id);

     /* Updates an API key. */

     void UpdateKey(const std::string &key_id, const std::string &description, const std::vector<std::string> &collections, const std::vector<std::string> &actions, const std::string &embedded_filters = "", bool allow_hanalyzer = false, const std::vector<std::string> &add_collections = {}, const std::vector<std::string> &remove_collections = {});

     /* Deletes documents by filter. */

     void DeleteDocumentsByFilter(const std::string &collection_name, const std::string &filter);

     /* Flushes all data. */

     void FlushAll(bool skip_confirmation = false);

     /* Adds a document. */

     void AddDocument(const std::string &collection_name, const std::string &id, const std::string &title, const std::string &content, const std::map<std::string, std::string> &fields = {});

     /* Copies one document into a new document id within the same collection. */

     void CopyDocument(const std::string &collection_name, const std::string &source_id, const std::string &target_id);

     /* Shows usage examples. */

     void ShowExamples();

     /* Shows all available API routes. */

     void ShowRoutes();

     /* Lists loaded modules, optionally filtered to core or optional entries. */

     void ListModules(const std::string &filter = "");

     /* Shows syntax for one module. */

     void ShowModuleSyntax(const std::string &module_name);

     /* Executes a module command through the shared module API. */

     void RunModuleCommand(const std::string &module_name, const std::string &route, const std::vector<std::string> &args = {});

     /* Loads one runtime module through the shared module API. */

     void LoadModule(const std::string &module_name);

     /* Unloads one runtime module through the shared module API. */

     void UnloadModule(const std::string &module_name);

     /* Checks whether one collection exists. */

     bool CollectionExists(const std::string &collection_name);

     /* Shows help information. */

     void ShowHelp();

     /* Sets default request timeout (seconds). */

     void SetDefaultTimeoutSeconds(int timeout_seconds);

     /* Reconfigures the base connection URL used by future requests. */

     void ReconfigureConnection(const std::string &url);

     /* Prints a table of data. */

     void PrintTable(const std::vector<std::string> &headers, const std::vector<std::vector<std::string>> &rows);

   private:
     std::string BaseURL;

     int Port;

     std::string Host;

     bool RawMode;

     std::string AuthToken;
     bool SSLAuthMode = false;
     bool RequestDryRunMode = false;
     bool RequestDryRunPrintCurl = false;
     std::string ProgramName;

     int DefaultTimeoutSeconds = 30;

     std::atomic<bool> Cancelled;

     int ExitCodeValue = 0;

     /* Gets the current timestamp. */

     std::string GetCurrentTimestamp();

     /* Sets the exit code. */

     void SetExitCode(int code);

     /* Prints an error message. */

     void PrintError(const std::string &message, const std::string &details = "");

     /* Prints a success message. */

     void PrintSuccess(const std::string &message);

     /* Prints an info message. */

     void PrintInfo(const std::string &message);

     /* Checks if a request failed. */

     bool CheckRequestFailed(const HTTPResponse &response, bool silent_on_connection_failure = false, const std::string &endpoint = "");

     /* Checks if the server is loading. */

     bool IsServerLoading();

     /* Prints maybe suggestions when a response includes them. */

     void PrintMaybeSuggestions(const nlohmann::json &root, const std::string &query, const std::string &collection_name);

     /* Helper to format bytes. */

     std::string FormatBytes(uint64_t bytes);
};
