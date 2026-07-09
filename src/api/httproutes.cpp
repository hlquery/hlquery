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
     Info.IsSynonymsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "synonyms");
     Info.IsSynonymsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "synonyms");
     Info.IsStopwordsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "stopwords");
     Info.IsStopwordsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "stopwords");
     Info.IsOverridesRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "overrides");
     Info.IsOverridesChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "overrides");
     Info.IsCurationsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && (Info.SegmentEquals(2, "curations") || Info.SegmentEquals(2, "curation_sets"));
     Info.IsCurationsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && (Info.SegmentEquals(2, "curations") || Info.SegmentEquals(2, "curation_sets"));
     Info.IsVectorSearchAlias = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "search");

     return Info;
}

static const std::unordered_map<std::string_view, RouteAction> &GetExactGetRoutes()
{
     static const std::unordered_map<std::string_view, RouteAction> Routes = {
          {"/", RouteAction::Status},
          {"/admin/storage_status", RouteAction::StorageStatus},
          {"/aliases", RouteAction::ListAliases},
          {"/boot-status", RouteAction::Startup},
          {"/cache", RouteAction::Cache},
          {"/collections", RouteAction::ListCollections},
          {"/collections/distributed", RouteAction::ListCollectionsDistributed},
          {"/connections", RouteAction::Connections},
          {"/config-files", RouteAction::ConfigFiles},
          {"/consistency", RouteAction::Integrity},
          {"/debug/counters", RouteAction::DebugCounters},
          {"/doctotal", RouteAction::DocTotal},
          {"/etc", RouteAction::Etc},
          {"/health", RouteAction::Health},
          {"/integrity", RouteAction::Integrity},
          {"/keys", RouteAction::ListKeys},
          {"/links", RouteAction::LinksList},
          {"/links/ping", RouteAction::LinksPing},
          {"/metrics", RouteAction::Metrics},
          {"/metrics-history", RouteAction::MetricsHistory},
          {"/metrics.json", RouteAction::Metrics},
          {"/metrics/history", RouteAction::MetricsHistory},
          {"/modules", RouteAction::ListModules},
          {"/multi_search", RouteAction::MultiSearch},
          {"/ping", RouteAction::Ping},
          {"/presets", RouteAction::ListPresets},
          {"/query", RouteAction::Status},
          {"/ready", RouteAction::Ready},
          {"/repair", RouteAction::Repair},
          {"/rocksdb", RouteAction::RocksDB},
          {"/search", RouteAction::GlobalSearch},
          {"/search-config", RouteAction::SearchConfig},
          {"/self-check", RouteAction::SelfCheck},
          {"/sql", RouteAction::DocumentSearch},
          {"/startup", RouteAction::Startup},
          {"/stats", RouteAction::Stats},
          {"/status", RouteAction::Status},
          {"/stopwords", RouteAction::ListAllStopwords},
          {"/stopwords/global", RouteAction::ListGlobalStopwords},
          {"/stopword_sets", RouteAction::ListAllStopwords},
          {"/stopword_sets/global", RouteAction::ListGlobalStopwords},
          {"/synonyms", RouteAction::ListAllSynonyms},
          {"/synonyms/global", RouteAction::ListGlobalSynonyms},
          {"/synonym_sets", RouteAction::ListAllSynonyms},
          {"/synonym_sets/global", RouteAction::ListGlobalSynonyms},
          {"/update-counters", RouteAction::UpdateCounters},
          {"/users", RouteAction::ListUsers},
          {"/_rocksdb", RouteAction::RocksDB},
     };

     return Routes;
}

static const std::unordered_map<std::string_view, RouteAction> &GetExactPostRoutes()
{
     static const std::unordered_map<std::string_view, RouteAction> Routes = {
          {"/analytics/click", RouteAction::AnalyticsClick},
          {"/collections", RouteAction::CreateCollection},
          {"/flush", RouteAction::Flush},
          {"/keys", RouteAction::CreateKey},
          {"/links/connect", RouteAction::LinksConnect},
          {"/links/disconnect", RouteAction::LinksDisconnect},
          {"/loadmodule", RouteAction::ModuleLoad},
          {"/multi_search", RouteAction::MultiSearch},
          {"/repair", RouteAction::Repair},
          {"/search", RouteAction::GlobalSearch},
          {"/sql", RouteAction::DocumentSearch},
          {"/stopword_sets/global", RouteAction::CreateGlobalStopword},
          {"/stopwords/global", RouteAction::CreateGlobalStopword},
          {"/unloadmodule", RouteAction::ModuleUnload},
          {"/update-counters", RouteAction::UpdateCounters},
          {"/users", RouteAction::CreateUser},
     };

     return Routes;
}

static RouteAction ResolveExactRoute(std::string_view Path, const std::string &Method)
{
     const std::unordered_map<std::string_view, RouteAction> *Routes = nullptr;

     if (Method == "GET")
     {
          Routes = &GetExactGetRoutes();
     }
     else if (Method == "POST")
     {
          Routes = &GetExactPostRoutes();
     }
     else
     {
          return RouteAction::NotFound;
     }

     const auto RouteIt = Routes->find(Path);
     if (RouteIt == Routes->end())
     {
          return RouteAction::NotFound;
     }

     return RouteIt->second;
}

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

          if (PrefixRoute(Path, Method, "/loadmodule/", {"POST"}))
          {
               return RouteAction::ModuleLoad;
          }

          if (PrefixRoute(Path, Method, "/unloadmodule/", {"POST"}))
          {
               return RouteAction::ModuleUnload;
          }

          if (PrefixRoute(Path, Method, "/modules/", {"GET", "POST", "PUT", "DELETE", "PATCH"}))
          {
               if (Path.rfind("/syntax") == Path.size() - 7 && Method == "GET")
               {
                    return RouteAction::GetModuleSyntax;
               }

               if (PrefixRoute(Path, Method, "/modules/load/", {"POST"}))
               {
                    return RouteAction::ModuleLoad;
               }

               if (PrefixRoute(Path, Method, "/modules/unload/", {"POST"}))
               {
                    return RouteAction::ModuleUnload;
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
