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

#include <initializer_list>
#include <string_view>
#include <algorithm>
#include <unordered_map>

#include "api/httpserver.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"

/* Defines HTTP route matching for API request dispatch. */

static bool MatchesMethod(const std::string &Method, std::initializer_list<const char *> Candidates)
{
     for (const char *Candidate : Candidates)
     {
          if (Method == Candidate)
          {
               return true;
          }
     }

     return false;
}

/* Implements the prefix route helper. */

static bool PrefixRoute(const std::string &Path,
                        const std::string &Method,
                        const std::string &Prefix,
                        std::initializer_list<const char *> Methods,
                        bool RequireSuffix = true)
{
     if (Path.rfind(Prefix, 0) != 0)
     {
          return false;
     }

     if (RequireSuffix && Path.size() <= Prefix.size())
     {
          return false;
     }

     return MatchesMethod(Method, Methods);
}

/* Implements the single child route helper. */

static bool SingleChildRoute(const std::string &Path,
                             const std::string &Method,
                             const std::string &Prefix,
                             std::initializer_list<const char *> Methods)
{
     if (!PrefixRoute(Path, Method, Prefix, Methods))
     {
          return false;
     }

     return Path.find('/', Prefix.size()) == std::string::npos;
}

struct CollectionRouteInfo
{
     std::vector<std::string_view> Segments;
     bool IsCollectionPath = false;
     bool IsCollectionRoot = false;
     bool IsCollectionLang = false;
     bool IsCollectionUpdate = false;
     bool IsDocumentsRoot = false;
     bool IsDocumentsChild = false;
     bool IsDocumentsSearch = false;
     bool IsDocumentsImport = false;
     bool IsDocumentsFacetCounts = false;
     bool IsDocumentsExport = false;
     bool IsDocumentsMaybe = false;
     bool IsDocumentContext = false;
     bool IsDocumentsUpdateByQuery = false;
     bool IsDocumentsDeleteByQuery = false;
     bool IsSynonymsRoot = false;
     bool IsSynonymsChild = false;
     bool IsStopwordsRoot = false;
     bool IsStopwordsChild = false;
     bool IsOverridesRoot = false;
     bool IsOverridesChild = false;
     bool IsCurationsRoot = false;
     bool IsCurationsChild = false;
     bool IsVectorSearchAlias = false;

     bool IsReservedDocumentOperation() const
     {
          return IsDocumentsSearch || IsDocumentsImport || IsDocumentsFacetCounts ||
                 IsDocumentsExport || IsDocumentsMaybe || IsDocumentsUpdateByQuery ||
                 IsDocumentsDeleteByQuery;
     }

     bool SegmentEquals(size_t Index, std::string_view Value) const
     {
          return Index < Segments.size() && Segments[Index] == Value;
     }
};

/* Normalizes route path values. */

static std::string NormalizeRoutePath(const std::string &Path)
{
     std::string NormalizedPath = Path;

     const size_t QueryPos = NormalizedPath.find('?');

     if (QueryPos != std::string::npos)
     {
          NormalizedPath = NormalizedPath.substr(0, QueryPos);
     }

     if (NormalizedPath.size() > 1 && NormalizedPath.back() == '/')
     {
          NormalizedPath.pop_back();
     }

     return NormalizedPath;
}

/* Implements the split route segments helper. */

static std::vector<std::string_view> SplitRouteSegments(std::string_view Path)
{
     std::vector<std::string_view> Segments;

     size_t Start = 0;

     while (Start < Path.size())
     {
          size_t SlashPos = Path.find('/', Start);

          if (SlashPos == std::string::npos)
          {
               SlashPos = Path.size();
          }

          if (SlashPos > Start)
          {
               Segments.push_back(Path.substr(Start, SlashPos - Start));
          }

          Start = SlashPos + 1;
     }

     return Segments;
}

/* Builds collection route info data. */

static CollectionRouteInfo BuildCollectionRouteInfo(std::string_view NormalizedPath)
{
     CollectionRouteInfo Info;

     if (NormalizedPath.rfind("/collections/", 0) != 0)
     {
          return Info;
     }

     Info.Segments = SplitRouteSegments(NormalizedPath);

     Info.IsCollectionPath = Info.Segments.size() >= 2 && Info.SegmentEquals(0, "collections");
     Info.IsCollectionRoot = Info.IsCollectionPath && Info.Segments.size() == 2;
     Info.IsCollectionLang = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "lang");
     Info.IsCollectionUpdate = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "update");
     Info.IsDocumentsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "documents");
     Info.IsDocumentsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "documents");
     Info.IsDocumentsSearch = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "search");
     Info.IsDocumentsImport = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "import");
     Info.IsDocumentsFacetCounts = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "facet_counts");
     Info.IsDocumentsExport = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "export");
     Info.IsDocumentsMaybe = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "maybe");
     Info.IsDocumentContext = Info.IsCollectionPath && Info.Segments.size() == 5 && Info.SegmentEquals(2, "documents") && Info.SegmentEquals(4, "context");
     Info.IsDocumentsUpdateByQuery = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "_update_by_query");
     Info.IsDocumentsDeleteByQuery = Info.IsDocumentsChild && Info.Segments.size() == 4 && Info.SegmentEquals(3, "_delete_by_query");
     Info.IsSynonymsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && (Info.SegmentEquals(2, "synonyms") || Info.SegmentEquals(2, "synonym_sets"));
     Info.IsSynonymsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && (Info.SegmentEquals(2, "synonyms") || Info.SegmentEquals(2, "synonym_sets"));
     Info.IsStopwordsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && (Info.SegmentEquals(2, "stopwords") || Info.SegmentEquals(2, "stopword_sets"));
     Info.IsStopwordsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && (Info.SegmentEquals(2, "stopwords") || Info.SegmentEquals(2, "stopword_sets"));
     Info.IsOverridesRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "overrides");
     Info.IsOverridesChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "overrides");
     Info.IsCurationsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && (Info.SegmentEquals(2, "curations") || Info.SegmentEquals(2, "curation_sets"));
     Info.IsCurationsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && (Info.SegmentEquals(2, "curations") || Info.SegmentEquals(2, "curation_sets"));
     Info.IsVectorSearchAlias = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "search");

     return Info;
}

bool IsPublicHttpRouteAction(RouteAction ActionVal)
{
     return ActionVal == RouteAction::Health || ActionVal == RouteAction::Ready ||
            ActionVal == RouteAction::Status || ActionVal == RouteAction::SearchConfig ||
            ActionVal == RouteAction::Ping || ActionVal == RouteAction::LinksList ||
            ActionVal == RouteAction::LinksPing || ActionVal == RouteAction::Etc;
}

bool IsAdminOnlyHttpRouteAction(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::ListKeys: case RouteAction::CreateKey: case RouteAction::GetKey:
          case RouteAction::DeleteKey: case RouteAction::UpdateKey: case RouteAction::ConfigFiles:
          case RouteAction::ListPresets: case RouteAction::UpsertPreset: case RouteAction::GetPreset:
          case RouteAction::DeletePreset: case RouteAction::ListUsers: case RouteAction::CreateUser:
          case RouteAction::GetUser: case RouteAction::DeleteUser: case RouteAction::UpdateUser:
          case RouteAction::LinksConnect: case RouteAction::LinksDisconnect: case RouteAction::Flush:
          case RouteAction::Repair: case RouteAction::Cache: case RouteAction::ModuleLoad:
          case RouteAction::ModuleUnload: case RouteAction::StorageStatus:
               return true;
          default:
               return false;
     }
}

/* The catalog is also consumed by /etc; exact paths in it drive exact routing. */

const std::vector<HttpRouteDescription> &GetHttpRouteDescriptions()
{
     static const auto Describe = [](RouteAction Action, const char *Name,
                                     std::initializer_list<const char *> Methods,
                                     std::initializer_list<const char *> Paths,
                                     const char *RequestType = "")
     {
          HttpRouteDescription Result;
          Result.Action = Action;
          Result.Name = Name;
          for (const char *Method : Methods) Result.Methods.emplace_back(Method);
          for (const char *Path : Paths) Result.Paths.emplace_back(Path);
          Result.Access = IsPublicHttpRouteAction(Action) ? "public" :
                          (IsAdminOnlyHttpRouteAction(Action) ? "admin" : "authenticated");
          Result.RequestContentType = RequestType;
          Result.ResponseContentType = "application/json";
          return Result;
     };

     static const std::vector<HttpRouteDescription> Routes = {
          Describe(RouteAction::Status, "status", {"GET"}, {"/", "/status", "/query"}),
          Describe(RouteAction::SearchConfig, "search_config", {"GET"}, {"/search-config"}),
          Describe(RouteAction::ConfigFiles, "config_files", {"GET"}, {"/config-files"}),
          Describe(RouteAction::Health, "health", {"GET"}, {"/health"}),
          Describe(RouteAction::Ready, "ready", {"GET"}, {"/ready"}),
          Describe(RouteAction::Ping, "ping", {"GET"}, {"/ping"}),
          Describe(RouteAction::Stats, "stats", {"GET"}, {"/stats"}),
          Describe(RouteAction::Metrics, "metrics", {"GET"}, {"/metrics", "/metrics.json"}),
          Describe(RouteAction::MetricsHistory, "metrics_history", {"GET"}, {"/metrics-history", "/metrics/history"}),
          Describe(RouteAction::Cache, "cache", {"GET"}, {"/cache"}),
          Describe(RouteAction::Connections, "connections", {"GET"}, {"/connections"}),
          Describe(RouteAction::RocksDB, "rocksdb", {"GET"}, {"/rocksdb", "/_rocksdb"}),
          Describe(RouteAction::DocTotal, "document_total", {"GET"}, {"/doctotal"}),
          Describe(RouteAction::Flush, "flush", {"POST"}, {"/flush"}, "application/json"),
          Describe(RouteAction::UpdateCounters, "update_counters", {"GET", "POST"}, {"/update-counters"}),
          Describe(RouteAction::DebugCounters, "debug_counters", {"GET"}, {"/debug/counters"}),
          Describe(RouteAction::Repair, "repair", {"GET", "POST"}, {"/repair"}, "application/json"),
          Describe(RouteAction::Startup, "startup", {"GET"}, {"/startup", "/boot-status"}),
          Describe(RouteAction::Integrity, "integrity", {"GET"}, {"/integrity", "/consistency"}),
          Describe(RouteAction::SelfCheck, "self_check", {"GET"}, {"/self-check"}),
          Describe(RouteAction::StorageStatus, "storage_status", {"GET"}, {"/admin/storage_status"}),
          Describe(RouteAction::Etc, "etc", {"GET"}, {"/etc"}),
          Describe(RouteAction::ListCollections, "collections_list", {"GET"}, {"/collections"}),
          Describe(RouteAction::ListCollectionsDistributed, "collections_distributed", {"GET"}, {"/collections/distributed"}),
          Describe(RouteAction::CreateCollection, "collection_create", {"POST"}, {"/collections"}, "application/json"),
          Describe(RouteAction::GetCollection, "collection_get", {"GET"}, {"/collections/{collection}"}),
          Describe(RouteAction::GetCollectionLanguage, "collection_language", {"GET"}, {"/collections/{collection}/lang"}),
          Describe(RouteAction::UpdateCollection, "collection_update", {"POST"}, {"/collections/{collection}/update"}, "application/json"),
          Describe(RouteAction::DeleteCollection, "collection_delete", {"DELETE"}, {"/collections/{collection}"}),
          Describe(RouteAction::DocumentSearch, "document_search", {"GET", "POST"}, {"/collections/{collection}/documents/search", "/sql"}, "application/json"),
          Describe(RouteAction::VectorSearch, "vector_search", {"GET", "POST"}, {"/collections/{collection}/vector_search", "/collections/{collection}/search"}, "application/json"),
          Describe(RouteAction::MultiSearch, "multi_search", {"GET", "POST"}, {"/multi_search"}, "application/json"),
          Describe(RouteAction::GlobalSearch, "global_search", {"GET", "POST"}, {"/search"}, "application/json"),
          Describe(RouteAction::ListDocuments, "documents_list", {"GET"}, {"/collections/{collection}/documents"}),
          Describe(RouteAction::GetDocument, "document_get", {"GET"}, {"/collections/{collection}/documents/{document}"}),
          Describe(RouteAction::GetDocumentContext, "document_context", {"GET"}, {"/collections/{collection}/documents/{document}/context"}),
          Describe(RouteAction::AddDocument, "document_add", {"POST"}, {"/collections/{collection}/documents"}, "application/json"),
          Describe(RouteAction::BulkImportDocuments, "documents_import", {"POST"}, {"/collections/{collection}/documents/import"}, "application/x-ndjson"),
          Describe(RouteAction::UpdateDocument, "document_update", {"PUT"}, {"/collections/{collection}/documents/{document}"}, "application/json"),
          Describe(RouteAction::DeleteDocument, "document_delete", {"DELETE"}, {"/collections/{collection}/documents/{document}"}),
          Describe(RouteAction::DeleteDocumentsByFilter, "documents_delete", {"DELETE"}, {"/collections/{collection}/documents"}),
          Describe(RouteAction::UpdateByQuery, "documents_update_by_query", {"POST"}, {"/collections/{collection}/documents/_update_by_query"}, "application/json"),
          Describe(RouteAction::DeleteByQuery, "documents_delete_by_query", {"POST"}, {"/collections/{collection}/documents/_delete_by_query"}, "application/json"),
          Describe(RouteAction::FacetCounts, "facet_counts", {"GET", "POST"}, {"/collections/{collection}/documents/facet_counts"}, "application/json"),
          Describe(RouteAction::ExportDocuments, "documents_export", {"GET", "POST"}, {"/collections/{collection}/documents/export"}, "application/json"),
          Describe(RouteAction::MaybeSuggest, "maybe_suggest", {"GET", "POST"}, {"/collections/{collection}/documents/maybe"}, "application/json"),
          Describe(RouteAction::ListSynonyms, "synonyms_list", {"GET"}, {"/collections/{collection}/synonyms", "/collections/{collection}/synonym_sets"}),
          Describe(RouteAction::ListAllSynonyms, "synonyms_list_all", {"GET"}, {"/synonyms", "/synonym_sets"}),
          Describe(RouteAction::UpsertSynonym, "synonym_upsert", {"POST", "PUT"}, {"/collections/{collection}/synonyms/{synonym}", "/collections/{collection}/synonym_sets/{synonym}"}, "application/json"),
          Describe(RouteAction::GetSynonym, "synonym_get", {"GET"}, {"/collections/{collection}/synonyms/{synonym}", "/collections/{collection}/synonym_sets/{synonym}"}),
          Describe(RouteAction::DeleteSynonym, "synonym_delete", {"DELETE"}, {"/collections/{collection}/synonyms/{synonym}", "/collections/{collection}/synonym_sets/{synonym}"}),
          Describe(RouteAction::ListGlobalSynonyms, "global_synonyms_list", {"GET"}, {"/synonyms/global", "/synonym_sets/global"}),
          Describe(RouteAction::UpsertGlobalSynonym, "global_synonym_upsert", {"POST", "PUT"}, {"/synonyms/global/{synonym}", "/synonym_sets/global/{synonym}", "/synonym_sets/global/items/{synonym}"}, "application/json"),
          Describe(RouteAction::GetGlobalSynonym, "global_synonym_get", {"GET"}, {"/synonyms/global/{synonym}", "/synonym_sets/global/{synonym}", "/synonym_sets/global/items/{synonym}"}),
          Describe(RouteAction::DeleteGlobalSynonym, "global_synonym_delete", {"DELETE"}, {"/synonyms/global/{synonym}", "/synonym_sets/global/{synonym}", "/synonym_sets/global/items/{synonym}"}),
          Describe(RouteAction::ListStopwords, "stopwords_list", {"GET"}, {"/collections/{collection}/stopwords", "/collections/{collection}/stopword_sets"}),
          Describe(RouteAction::ListAllStopwords, "stopwords_list_all", {"GET"}, {"/stopwords", "/stopword_sets"}),
          Describe(RouteAction::CreateStopword, "stopword_create", {"POST"}, {"/collections/{collection}/stopwords", "/collections/{collection}/stopword_sets"}, "application/json"),
          Describe(RouteAction::DeleteStopword, "stopword_delete", {"DELETE"}, {"/collections/{collection}/stopwords/{stopword}", "/collections/{collection}/stopword_sets/{stopword}"}),
          Describe(RouteAction::ListGlobalStopwords, "global_stopwords_list", {"GET"}, {"/stopwords/global", "/stopword_sets/global"}),
          Describe(RouteAction::CreateGlobalStopword, "global_stopword_create", {"POST"}, {"/stopwords/global", "/stopword_sets/global"}, "application/json"),
          Describe(RouteAction::DeleteGlobalStopword, "global_stopword_delete", {"DELETE"}, {"/stopwords/global/{stopword}", "/stopword_sets/global/{stopword}", "/stopword_sets/global/items/{stopword}"}),
          Describe(RouteAction::ListOverrides, "overrides_list", {"GET"}, {"/collections/{collection}/overrides", "/collections/{collection}/curations", "/collections/{collection}/curation_sets"}),
          Describe(RouteAction::UpsertOverride, "override_upsert", {"POST", "PUT"}, {"/collections/{collection}/overrides/{override}", "/collections/{collection}/curations/{override}", "/collections/{collection}/curation_sets/{override}"}, "application/json"),
          Describe(RouteAction::GetOverride, "override_get", {"GET"}, {"/collections/{collection}/overrides/{override}", "/collections/{collection}/curations/{override}", "/collections/{collection}/curation_sets/{override}"}),
          Describe(RouteAction::DeleteOverride, "override_delete", {"DELETE"}, {"/collections/{collection}/overrides/{override}", "/collections/{collection}/curations/{override}", "/collections/{collection}/curation_sets/{override}"}),
          Describe(RouteAction::ListAliases, "aliases_list", {"GET"}, {"/aliases", "/collections/{collection}/aliases"}),
          Describe(RouteAction::UpsertAlias, "alias_upsert", {"POST", "PUT"}, {"/aliases/{alias}"}, "application/json"),
          Describe(RouteAction::GetAlias, "alias_get", {"GET"}, {"/aliases/{alias}"}),
          Describe(RouteAction::DeleteAlias, "alias_delete", {"DELETE"}, {"/aliases/{alias}"}),
          Describe(RouteAction::LinksList, "links_list", {"GET"}, {"/links"}),
          Describe(RouteAction::LinksPing, "links_ping", {"GET"}, {"/links/ping"}),
          Describe(RouteAction::LinksConnect, "links_connect", {"POST"}, {"/links/connect"}, "application/json"),
          Describe(RouteAction::LinksDisconnect, "links_disconnect", {"POST"}, {"/links/disconnect"}, "application/json"),
          Describe(RouteAction::ListUsers, "users_list", {"GET"}, {"/users"}),
          Describe(RouteAction::CreateUser, "user_create", {"POST"}, {"/users"}, "application/json"),
          Describe(RouteAction::GetUser, "user_get", {"GET"}, {"/users/{user}"}),
          Describe(RouteAction::UpdateUser, "user_update", {"PUT"}, {"/users/{user}"}, "application/json"),
          Describe(RouteAction::DeleteUser, "user_delete", {"DELETE"}, {"/users/{user}"}),
          Describe(RouteAction::ListKeys, "keys_list", {"GET"}, {"/keys"}),
          Describe(RouteAction::CreateKey, "key_create", {"POST"}, {"/keys"}, "application/json"),
          Describe(RouteAction::GetKey, "key_get", {"GET"}, {"/keys/{key}"}),
          Describe(RouteAction::UpdateKey, "key_update", {"PUT"}, {"/keys/{key}"}, "application/json"),
          Describe(RouteAction::DeleteKey, "key_delete", {"DELETE"}, {"/keys/{key}"}),
          Describe(RouteAction::ListPresets, "presets_list", {"GET"}, {"/presets"}),
          Describe(RouteAction::UpsertPreset, "preset_upsert", {"POST", "PUT"}, {"/presets/{preset}"}, "application/json"),
          Describe(RouteAction::GetPreset, "preset_get", {"GET"}, {"/presets/{preset}"}),
          Describe(RouteAction::DeletePreset, "preset_delete", {"DELETE"}, {"/presets/{preset}"}),
          Describe(RouteAction::AnalyticsClick, "analytics_click", {"POST"}, {"/analytics/click"}, "application/json"),
          Describe(RouteAction::ListModules, "modules_list", {"GET"}, {"/modules"}),
          Describe(RouteAction::GetModuleSyntax, "module_syntax", {"GET"}, {"/modules/{module}/syntax"}),
          Describe(RouteAction::ModuleLoad, "module_load", {"POST"}, {"/loadmodule", "/loadmodule/{module}", "/modules/load", "/modules/load/{module}"}, "application/json"),
          Describe(RouteAction::ModuleUnload, "module_unload", {"POST"}, {"/unloadmodule", "/unloadmodule/{module}", "/modules/unload", "/modules/unload/{module}"}, "application/json"),
          Describe(RouteAction::ModuleAPI, "module_api", {"GET", "POST", "PUT", "PATCH", "DELETE"}, {"/modules/{module}/{path}"}, "application/json")
     };

     return Routes;
}

static bool MatchesCatalogPath(std::string_view Pattern, std::string_view Path)
{
     const auto PatternSegments = SplitRouteSegments(Pattern);
     const auto PathSegments = SplitRouteSegments(Path);

     if (PatternSegments.size() != PathSegments.size())
     {
          return false;
     }

     for (size_t I = 0; I < PatternSegments.size(); ++I)
     {
          const std::string_view Segment = PatternSegments[I];
          const bool Placeholder = Segment.size() >= 2 && Segment.front() == '{' && Segment.back() == '}';
          if (!Placeholder && Segment != PathSegments[I])
          {
               return false;
          }
     }

     return true;
}

std::vector<std::string> GetAllowedHttpMethods(const std::string &Path)
{
     const std::string NormalizedPath = NormalizeRoutePath(Path);
     std::vector<std::string> Result;

     for (const auto &Description : GetHttpRouteDescriptions())
     {
          bool Matches = false;
          for (const auto &Pattern : Description.Paths)
          {
               if (MatchesCatalogPath(Pattern, NormalizedPath))
               {
                    Matches = true;
                    break;
               }
          }

          if (!Matches) continue;
          for (const auto &Method : Description.Methods)
          {
               if (std::find(Result.begin(), Result.end(), Method) == Result.end()) Result.push_back(Method);
          }
     }

     std::sort(Result.begin(), Result.end());
     return Result;
}

nlohmann::json BuildHttpRouteDiscoveryJSON()
{
     nlohmann::json Routes = nlohmann::json::array();
     for (const auto &Description : GetHttpRouteDescriptions())
     {
          nlohmann::json Route;
          Route["name"] = Description.Name;
          Route["methods"] = Description.Methods;
          Route["paths"] = Description.Paths;
          Route["access"] = Description.Access;
          Route["response_content_type"] = Description.ResponseContentType;
          if (!Description.RequestContentType.empty()) Route["request_content_type"] = Description.RequestContentType;

          std::vector<std::string> PathParameters;
          for (const auto &Path : Description.Paths)
          {
               size_t Open = 0;
               while ((Open = Path.find('{', Open)) != std::string::npos)
               {
                    const size_t Close = Path.find('}', Open + 1);
                    if (Close == std::string::npos) break;
                    const std::string Parameter = Path.substr(Open + 1, Close - Open - 1);
                    if (std::find(PathParameters.begin(), PathParameters.end(), Parameter) == PathParameters.end())
                    {
                         PathParameters.push_back(Parameter);
                    }
                    Open = Close + 1;
               }
          }
          Route["path_parameters"] = PathParameters;
          Routes.push_back(std::move(Route));
     }
     return Routes;
}

/* Resolves exact route values. */

static const std::unordered_map<std::string, std::unordered_map<std::string, RouteAction>> &GetExactRoutes()
{
     static const auto Routes = []
     {
          std::unordered_map<std::string, std::unordered_map<std::string, RouteAction>> Result;
          for (const auto &Description : GetHttpRouteDescriptions())
          {
               for (const auto &Path : Description.Paths)
               {
                    if (Path.find('{') != std::string::npos) continue;
                    for (const auto &Method : Description.Methods) Result[Method][Path] = Description.Action;
               }
          }
          return Result;
     }();
     return Routes;
}

static RouteAction ResolveExactRoute(const std::string &Path, const std::string &Method)
{
     const auto MethodIt = GetExactRoutes().find(Method);
     if (MethodIt == GetExactRoutes().end()) return RouteAction::NotFound;
     const auto PathIt = MethodIt->second.find(Path);
     return PathIt == MethodIt->second.end() ? RouteAction::NotFound : PathIt->second;
}

/* Resolves HTTP route values. */

RouteAction ResolveHttpRoute(const HttpRequest &Request)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("http_routes", "ResolveHttpRoute CALLED with path: '" + Request.Path + "' method: '" + Request.Method + "'.");
     }

     try
     {
          const std::string NormalizedPath = NormalizeRoutePath(Request.Path);
          const std::string &Method = Request.Method;
          const std::string &Path = NormalizedPath;

          const RouteAction ExactAction = ResolveExactRoute(Path, Method);
          if (ExactAction != RouteAction::NotFound)
          {
               return ExactAction;
          }

          if (SingleChildRoute(Path, Method, "/loadmodule/", {"POST"}))
          {
               return RouteAction::ModuleLoad;
          }

          if (SingleChildRoute(Path, Method, "/unloadmodule/", {"POST"}))
          {
               return RouteAction::ModuleUnload;
          }

          if (PrefixRoute(Path, Method, "/modules/", {"GET", "POST", "PUT", "DELETE", "PATCH"}))
          {
               const std::vector<std::string_view> ModuleSegments = SplitRouteSegments(Path);

               if (ModuleSegments.size() == 3 && ModuleSegments[2] == "syntax" && Method == "GET")
               {
                    return RouteAction::GetModuleSyntax;
               }

               if (Path.rfind("/modules/load/", 0) == 0 && Method == "POST")
               {
                    return SingleChildRoute(Path, Method, "/modules/load/", {"POST"})
                                ? RouteAction::ModuleLoad
                                : RouteAction::NotFound;
               }

               if (Path.rfind("/modules/unload/", 0) == 0 && Method == "POST")
               {
                    return SingleChildRoute(Path, Method, "/modules/unload/", {"POST"})
                                ? RouteAction::ModuleUnload
                                : RouteAction::NotFound;
               }

               return RouteAction::ModuleAPI;
          }

          if (SingleChildRoute(Path, Method, "/synonyms/global/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/synonym_sets/global/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertGlobalSynonym;
          }

          if (PrefixRoute(Path, Method, "/synonym_sets/global/items/", {"POST", "PUT"}) &&
              SplitRouteSegments(Path).size() == 4)
          {
               return RouteAction::UpsertGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/synonyms/global/", {"GET"}))
          {
               return RouteAction::GetGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/synonym_sets/global/", {"GET"}))
          {
               return RouteAction::GetGlobalSynonym;
          }

          if (PrefixRoute(Path, Method, "/synonym_sets/global/items/", {"GET"}) &&
              SplitRouteSegments(Path).size() == 4)
          {
               return RouteAction::GetGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/synonyms/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/synonym_sets/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalSynonym;
          }

          if (PrefixRoute(Path, Method, "/synonym_sets/global/items/", {"DELETE"}) &&
              SplitRouteSegments(Path).size() == 4)
          {
               return RouteAction::DeleteGlobalSynonym;
          }

          if (SingleChildRoute(Path, Method, "/stopwords/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalStopword;
          }

          if (SingleChildRoute(Path, Method, "/stopword_sets/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalStopword;
          }

          if (PrefixRoute(Path, Method, "/stopword_sets/global/items/", {"DELETE"}) &&
              SplitRouteSegments(Path).size() == 4)
          {
               return RouteAction::DeleteGlobalStopword;
          }

          const CollectionRouteInfo RouteInfo = BuildCollectionRouteInfo(NormalizedPath);

          if (RouteInfo.IsCollectionPath && RouteInfo.Segments.size() == 3 && RouteInfo.SegmentEquals(2, "aliases") && Method == "GET")
          {
               return RouteAction::ListAliases;
          }

          if (RouteInfo.IsCollectionPath && RouteInfo.Segments.size() == 3 &&
              (RouteInfo.SegmentEquals(2, "vector_search") || RouteInfo.IsVectorSearchAlias) &&
              (Method == "GET" || Method == "POST"))
          {
               return RouteAction::VectorSearch;
          }

          if (RouteInfo.IsSynonymsRoot && Method == "GET")
          {
               return RouteAction::ListSynonyms;
          }

          if (RouteInfo.IsSynonymsChild && RouteInfo.Segments.size() == 4 && (Method == "POST" || Method == "PUT"))
          {
               return RouteAction::UpsertSynonym;
          }

          if (RouteInfo.IsSynonymsChild && RouteInfo.Segments.size() == 4 && Method == "GET")
          {
               return RouteAction::GetSynonym;
          }

          if (RouteInfo.IsSynonymsChild && RouteInfo.Segments.size() == 4 && Method == "DELETE")
          {
               return RouteAction::DeleteSynonym;
          }

          if (RouteInfo.IsDocumentsSearch && (Method == "GET" || Method == "POST"))
          {
               return RouteAction::DocumentSearch;
          }

          if (RouteInfo.IsCollectionLang && Method == "GET")
          {
               return RouteAction::GetCollectionLanguage;
          }

          if (RouteInfo.IsCollectionRoot && Method == "GET")
          {
               return RouteAction::GetCollection;
          }

          if (RouteInfo.IsCollectionRoot && Method == "DELETE")
          {
               return RouteAction::DeleteCollection;
          }

          if (RouteInfo.IsCollectionUpdate && Method == "POST")
          {
               return RouteAction::UpdateCollection;
          }

          if (RouteInfo.IsDocumentsRoot && Method == "GET")
          {
               return RouteAction::ListDocuments;
          }

          if (RouteInfo.IsDocumentContext && Method == "GET")
          {
               return RouteAction::GetDocumentContext;
          }

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 &&
              !RouteInfo.IsReservedDocumentOperation() && Method == "GET")
          {
               return RouteAction::GetDocument;
          }

          if (RouteInfo.IsDocumentsImport && Method == "POST")
          {
               return RouteAction::BulkImportDocuments;
          }

          if (RouteInfo.IsDocumentsRoot && Method == "POST")
          {
               return RouteAction::AddDocument;
          }

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 &&
              !RouteInfo.IsReservedDocumentOperation() && Method == "PUT")
          {
               return RouteAction::UpdateDocument;
          }

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 &&
              !RouteInfo.IsReservedDocumentOperation() && Method == "DELETE")
          {
               return RouteAction::DeleteDocument;
          }

          if (RouteInfo.IsDocumentsRoot && Method == "DELETE")
          {
               return RouteAction::DeleteDocumentsByFilter;
          }

          if (RouteInfo.IsDocumentsUpdateByQuery && Method == "POST")
          {
               return RouteAction::UpdateByQuery;
          }

          if (RouteInfo.IsDocumentsDeleteByQuery && Method == "POST")
          {
               return RouteAction::DeleteByQuery;
          }

          if (RouteInfo.IsDocumentsFacetCounts && (Method == "GET" || Method == "POST"))
          {
               return RouteAction::FacetCounts;
          }

          if (RouteInfo.IsDocumentsExport && (Method == "GET" || Method == "POST"))
          {
               return RouteAction::ExportDocuments;
          }

          if (RouteInfo.IsDocumentsMaybe && (Method == "GET" || Method == "POST"))
          {
               return RouteAction::MaybeSuggest;
          }

          if (RouteInfo.IsStopwordsRoot && Method == "GET")
          {
               return RouteAction::ListStopwords;
          }

          if (RouteInfo.IsStopwordsRoot && Method == "POST")
          {
               return RouteAction::CreateStopword;
          }

          if (RouteInfo.IsStopwordsChild && RouteInfo.Segments.size() == 4 && Method == "DELETE")
          {
               return RouteAction::DeleteStopword;
          }

          if ((RouteInfo.IsOverridesRoot || RouteInfo.IsCurationsRoot) && Method == "GET")
          {
               return RouteAction::ListOverrides;
          }

          if ((RouteInfo.IsOverridesChild || RouteInfo.IsCurationsChild) && RouteInfo.Segments.size() == 4 && (Method == "POST" || Method == "PUT"))
          {
               return RouteAction::UpsertOverride;
          }

          if ((RouteInfo.IsOverridesChild || RouteInfo.IsCurationsChild) && RouteInfo.Segments.size() == 4 && Method == "GET")
          {
               return RouteAction::GetOverride;
          }

          if ((RouteInfo.IsOverridesChild || RouteInfo.IsCurationsChild) && RouteInfo.Segments.size() == 4 && Method == "DELETE")
          {
               return RouteAction::DeleteOverride;
          }

          if (SingleChildRoute(Path, Method, "/aliases/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertAlias;
          }

          if (SingleChildRoute(Path, Method, "/aliases/", {"GET"}))
          {
               return RouteAction::GetAlias;
          }

          if (SingleChildRoute(Path, Method, "/aliases/", {"DELETE"}))
          {
               return RouteAction::DeleteAlias;
          }

          if (SingleChildRoute(Path, Method, "/users/", {"GET"}))
          {
               return RouteAction::GetUser;
          }

          if (SingleChildRoute(Path, Method, "/users/", {"DELETE"}))
          {
               return RouteAction::DeleteUser;
          }

          if (SingleChildRoute(Path, Method, "/users/", {"PUT"}))
          {
               return RouteAction::UpdateUser;
          }

          if (SingleChildRoute(Path, Method, "/keys/", {"GET"}))
          {
               return RouteAction::GetKey;
          }

          if (SingleChildRoute(Path, Method, "/keys/", {"DELETE"}))
          {
               return RouteAction::DeleteKey;
          }

          if (SingleChildRoute(Path, Method, "/keys/", {"PUT"}))
          {
               return RouteAction::UpdateKey;
          }

          if (SingleChildRoute(Path, Method, "/presets/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertPreset;
          }

          if (SingleChildRoute(Path, Method, "/presets/", {"GET"}))
          {
               return RouteAction::GetPreset;
          }

          if (SingleChildRoute(Path, Method, "/presets/", {"DELETE"}))
          {
               return RouteAction::DeletePreset;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("http_routes", "NO MATCH - returning NotFound for path: " + Request.Path + ".");
          }

          return RouteAction::NotFound;
     }
     catch (const std::exception &E)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_routes", "EXCEPTION in ResolveHttpRoute: " + std::string(E.what()) + ".");
          }

          return RouteAction::NotFound;
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("http_routes", "UNKNOWN EXCEPTION in ResolveHttpRoute.");
          }

          return RouteAction::NotFound;
     }
}

/* Returns the readable name for a route action. */

const char *RouteActionName(RouteAction ActionVal)
{
     switch (ActionVal)
     {
          case RouteAction::Status:
               return "Status";
          case RouteAction::SearchConfig:
               return "SearchConfig";
          case RouteAction::ConfigFiles:
               return "ConfigFiles";
          case RouteAction::Health:
               return "Health";
          case RouteAction::Ready:
               return "Ready";
          case RouteAction::Ping:
               return "Ping";
          case RouteAction::Stats:
               return "Stats";
          case RouteAction::Metrics:
               return "Metrics";
          case RouteAction::MetricsHistory:
               return "MetricsHistory";
          case RouteAction::Cache:
               return "Cache";
          case RouteAction::Connections:
               return "Connections";
          case RouteAction::RocksDB:
               return "RocksDB";
          case RouteAction::DocTotal:
               return "DocTotal";
          case RouteAction::Flush:
               return "Flush";
          case RouteAction::UpdateCounters:
               return "UpdateCounters";
          case RouteAction::DebugCounters:
               return "DebugCounters";
          case RouteAction::Repair:
               return "Repair";
          case RouteAction::Startup:
               return "Startup";
          case RouteAction::Integrity:
               return "Integrity";
          case RouteAction::SelfCheck:
               return "SelfCheck";
          case RouteAction::StorageStatus:
               return "StorageStatus";
          case RouteAction::Etc:
               return "Etc";
          case RouteAction::Root:
               return "Root";
          case RouteAction::ListCollections:
               return "ListCollections";
          case RouteAction::ListCollectionsDistributed:
               return "ListCollectionsDistributed";
          case RouteAction::CreateCollection:
               return "CreateCollection";
          case RouteAction::GetCollection:
               return "GetCollection";
          case RouteAction::GetCollectionLanguage:
               return "GetCollectionLanguage";
          case RouteAction::UpdateCollection:
               return "UpdateCollection";
          case RouteAction::DeleteCollection:
               return "DeleteCollection";
          case RouteAction::DocumentSearch:
               return "DocumentSearch";
          case RouteAction::VectorSearch:
               return "VectorSearch";
          case RouteAction::MultiSearch:
               return "MultiSearch";
          case RouteAction::GlobalSearch:
               return "GlobalSearch";
          case RouteAction::ListDocuments:
               return "ListDocuments";
          case RouteAction::GetDocument:
               return "GetDocument";
          case RouteAction::GetDocumentContext:
               return "GetDocumentContext";
          case RouteAction::AddDocument:
               return "AddDocument";
          case RouteAction::BulkImportDocuments:
               return "BulkImportDocuments";
          case RouteAction::UpdateDocument:
               return "UpdateDocument";
          case RouteAction::DeleteDocument:
               return "DeleteDocument";
          case RouteAction::DeleteDocumentsByFilter:
               return "DeleteDocumentsByFilter";
          case RouteAction::UpdateByQuery:
               return "UpdateByQuery";
          case RouteAction::DeleteByQuery:
               return "DeleteByQuery";
          case RouteAction::FacetCounts:
               return "FacetCounts";
          case RouteAction::ExportDocuments:
               return "ExportDocuments";
          case RouteAction::MaybeSuggest:
               return "MaybeSuggest";
          case RouteAction::ListSynonyms:
               return "ListSynonyms";
          case RouteAction::ListAllSynonyms:
               return "ListAllSynonyms";
          case RouteAction::UpsertSynonym:
               return "UpsertSynonym";
          case RouteAction::GetSynonym:
               return "GetSynonym";
          case RouteAction::DeleteSynonym:
               return "DeleteSynonym";
          case RouteAction::ListGlobalSynonyms:
               return "ListGlobalSynonyms";
          case RouteAction::UpsertGlobalSynonym:
               return "UpsertGlobalSynonym";
          case RouteAction::GetGlobalSynonym:
               return "GetGlobalSynonym";
          case RouteAction::DeleteGlobalSynonym:
               return "DeleteGlobalSynonym";
          case RouteAction::ListStopwords:
               return "ListStopwords";
          case RouteAction::ListAllStopwords:
               return "ListAllStopwords";
          case RouteAction::CreateStopword:
               return "CreateStopword";
          case RouteAction::DeleteStopword:
               return "DeleteStopword";
          case RouteAction::ListGlobalStopwords:
               return "ListGlobalStopwords";
          case RouteAction::CreateGlobalStopword:
               return "CreateGlobalStopword";
          case RouteAction::DeleteGlobalStopword:
               return "DeleteGlobalStopword";
          case RouteAction::ListOverrides:
               return "ListOverrides";
          case RouteAction::UpsertOverride:
               return "UpsertOverride";
          case RouteAction::GetOverride:
               return "GetOverride";
          case RouteAction::DeleteOverride:
               return "DeleteOverride";
          case RouteAction::ListAliases:
               return "ListAliases";
          case RouteAction::UpsertAlias:
               return "UpsertAlias";
          case RouteAction::GetAlias:
               return "GetAlias";
          case RouteAction::DeleteAlias:
               return "DeleteAlias";
          case RouteAction::LinksList:
               return "LinksList";
          case RouteAction::LinksPing:
               return "LinksPing";
          case RouteAction::LinksConnect:
               return "LinksConnect";
          case RouteAction::LinksDisconnect:
               return "LinksDisconnect";
          case RouteAction::ListUsers:
               return "ListUsers";
          case RouteAction::CreateUser:
               return "CreateUser";
          case RouteAction::GetUser:
               return "GetUser";
          case RouteAction::UpdateUser:
               return "UpdateUser";
          case RouteAction::DeleteUser:
               return "DeleteUser";
          case RouteAction::ListKeys:
               return "ListKeys";
          case RouteAction::CreateKey:
               return "CreateKey";
          case RouteAction::GetKey:
               return "GetKey";
          case RouteAction::UpdateKey:
               return "UpdateKey";
          case RouteAction::DeleteKey:
               return "DeleteKey";
          case RouteAction::ListPresets:
               return "ListPresets";
          case RouteAction::UpsertPreset:
               return "UpsertPreset";
          case RouteAction::GetPreset:
               return "GetPreset";
          case RouteAction::DeletePreset:
               return "DeletePreset";
          case RouteAction::AnalyticsClick:
               return "AnalyticsClick";
          case RouteAction::ListModules:
               return "ListModules";
          case RouteAction::GetModuleSyntax:
               return "GetModuleSyntax";
          case RouteAction::ModuleLoad:
               return "ModuleLoad";
          case RouteAction::ModuleUnload:
               return "ModuleUnload";
          case RouteAction::ModuleAPI:
               return "ModuleAPI";
          case RouteAction::NotFound:
               return "NotFound";
          default:
               return "Unknown";
     }
}
