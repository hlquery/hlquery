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

#include "api/httpserver.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"

namespace
{
static bool MatchesAnyPath(const std::string &Path, std::initializer_list<const char *> Candidates)
{
     for (const char *Candidate : Candidates)
     {
          if (Path == Candidate)
          {
               return true;
          }
     }

     return false;
}

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

static bool ExactRoute(const std::string &Path,
                       const std::string &Method,
                       std::initializer_list<const char *> Paths,
                       std::initializer_list<const char *> Methods)
{
     return MatchesAnyPath(Path, Paths) && MatchesMethod(Method, Methods);
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

struct CollectionRouteInfo
{
     std::vector<std::string> Segments;
     bool IsCollectionPath = false;
     bool IsCollectionRoot = false;
     bool IsCollectionUpdate = false;
     bool IsDocumentsRoot = false;
     bool IsDocumentsChild = false;
     bool IsDocumentsSearch = false;
     bool IsDocumentsImport = false;
     bool IsDocumentsFacetCounts = false;
     bool IsDocumentsExport = false;
     bool IsDocumentsMaybe = false;
     bool IsDocumentsUpdateByQuery = false;
     bool IsDocumentsDeleteByQuery = false;
     bool IsSynonymsRoot = false;
     bool IsSynonymsChild = false;
     bool IsStopwordsRoot = false;
     bool IsStopwordsChild = false;
     bool IsOverridesRoot = false;
     bool IsOverridesChild = false;
     bool IsVectorSearchAlias = false;

     bool SegmentEquals(size_t Index, const std::string &Value) const
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

static std::vector<std::string> SplitRouteSegments(const std::string &Path)
{
     std::vector<std::string> Segments;

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

static CollectionRouteInfo BuildCollectionRouteInfo(const std::string &NormalizedPath)
{
     CollectionRouteInfo Info;
     Info.Segments = SplitRouteSegments(NormalizedPath);

     Info.IsCollectionPath = Info.Segments.size() >= 2 && Info.SegmentEquals(0, "collections");
     Info.IsCollectionRoot = Info.IsCollectionPath && Info.Segments.size() == 2;
     Info.IsCollectionUpdate = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "update");
     Info.IsDocumentsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "documents");
     Info.IsDocumentsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "documents");
     Info.IsDocumentsSearch = Info.IsDocumentsChild && Info.SegmentEquals(3, "search");
     Info.IsDocumentsImport = Info.IsDocumentsChild && Info.SegmentEquals(3, "import");
     Info.IsDocumentsFacetCounts = Info.IsDocumentsChild && Info.SegmentEquals(3, "facet_counts");
     Info.IsDocumentsExport = Info.IsDocumentsChild && Info.SegmentEquals(3, "export");
     Info.IsDocumentsMaybe = Info.IsDocumentsChild && Info.SegmentEquals(3, "maybe");
     Info.IsDocumentsUpdateByQuery = Info.IsDocumentsChild && Info.SegmentEquals(3, "_update_by_query");
     Info.IsDocumentsDeleteByQuery = Info.IsDocumentsChild && Info.SegmentEquals(3, "_delete_by_query");
     Info.IsSynonymsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "synonyms");
     Info.IsSynonymsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "synonyms");
     Info.IsStopwordsRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "stopwords");
     Info.IsStopwordsChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "stopwords");
     Info.IsOverridesRoot = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "overrides");
     Info.IsOverridesChild = Info.IsCollectionPath && Info.Segments.size() >= 4 && Info.SegmentEquals(2, "overrides");
     Info.IsVectorSearchAlias = Info.IsCollectionPath && Info.Segments.size() == 3 && Info.SegmentEquals(2, "search");

     return Info;
}
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

          if ((NormalizedPath == "/etc" || Request.Path == "/etc") && Request.Method == "GET")
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_routes", "MATCHED /etc route - returning protocol codes.");
               }

               return RouteAction::Etc;
          }

          if ((NormalizedPath == "/flush" || Request.Path == "/flush") && Request.Method == "POST")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_routes", "MATCHED /flush route EARLY - normalized_path='" + NormalizedPath + "' path='" + Request.Path + "' method='" + Request.Method + "'.");
               }

               return RouteAction::Flush;
          }

          const CollectionRouteInfo RouteInfo = BuildCollectionRouteInfo(NormalizedPath);

          const std::string &Method = Request.Method;

          const std::string &Path = NormalizedPath;

          if (ExactRoute(Path, Method, {"/status", "/query"}, {"GET"}))
          {
               return RouteAction::Status;
          }

          if (ExactRoute(Path, Method, {"/sql"}, {"GET", "POST"}))
          {
               return RouteAction::DocumentSearch;
          }

          if (ExactRoute(Path, Method, {"/search-config"}, {"GET"}))
          {
               return RouteAction::SearchConfig;
          }

          if (ExactRoute(Path, Method, {"/links"}, {"GET"}))
          {
               return RouteAction::LinksList;
          }

          if (ExactRoute(Path, Method, {"/links/ping"}, {"GET"}))
          {
               return RouteAction::LinksPing;
          }

          if (ExactRoute(Path, Method, {"/links/connect"}, {"POST"}))
          {
               return RouteAction::LinksConnect;
          }

          if (ExactRoute(Path, Method, {"/links/disconnect"}, {"POST"}))
          {
               return RouteAction::LinksDisconnect;
          }

          if (ExactRoute(Path, Method, {"/analytics/click"}, {"POST"}))
          {
               return RouteAction::AnalyticsClick;
          }

          if (ExactRoute(Path, Method, {"/modules"}, {"GET"}))
          {
               return RouteAction::ListModules;
          }

          if (PrefixRoute(Path, Method, "/modules/", {"GET", "POST", "PUT", "DELETE"}))
          {
               if (Path.rfind("/syntax") == Path.size() - 7 && Method == "GET")
               {
                    return RouteAction::GetModuleSyntax;
               }

               return RouteAction::ModuleAPI;
          }

          if (ExactRoute(Path, Method, {"/startup", "/boot-status"}, {"GET"}))
          {
               return RouteAction::Startup;
          }

          if (ExactRoute(Path, Method, {"/integrity", "/consistency"}, {"GET"}))
          {
               return RouteAction::Integrity;
          }

          if (ExactRoute(Path, Method, {"/self-check"}, {"GET"}))
          {
               return RouteAction::SelfCheck;
          }

          if (ExactRoute(Path, Method, {"/admin/storage_status"}, {"GET"}))
          {
               return RouteAction::StorageStatus;
          }

          if (ExactRoute(Path, Method, {"/synonyms/global"}, {"GET"}))
          {
               return RouteAction::ListGlobalSynonyms;
          }

          if (PrefixRoute(Path, Method, "/synonyms/global/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertGlobalSynonym;
          }

          if (PrefixRoute(Path, Method, "/synonyms/global/", {"GET"}))
          {
               return RouteAction::GetGlobalSynonym;
          }

          if (PrefixRoute(Path, Method, "/synonyms/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalSynonym;
          }

          if (ExactRoute(Path, Method, {"/synonyms"}, {"GET"}))
          {
               return RouteAction::ListAllSynonyms;
          }

          if (NormalizedPath == "/health")
          {
               return RouteAction::Health;
          }

          if (NormalizedPath == "/ping" && Method == "GET")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_routes", "MATCHED /ping route - normalized_path='" + NormalizedPath + "' method='" + Method + "' path='" + Request.Path + "'.");
               }

               return RouteAction::Ping;
          }

          if (ExactRoute(Path, Method, {"/rocksdb", "/_rocksdb"}, {"GET"}))
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("http_routes", "MATCHED RocksDB route! normalized_path=" + NormalizedPath + ".");
               }

               return RouteAction::RocksDB;
          }

          if (ExactRoute(Path, Method, {"/stats"}, {"GET"}))
          {
               return RouteAction::Stats;
          }

          if (ExactRoute(Path, Method, {"/metrics", "/metrics.json"}, {"GET"}))
          {
               return RouteAction::Metrics;
          }

          if (ExactRoute(Path, Method, {"/metrics/history", "/metrics-history"}, {"GET"}))
          {
               return RouteAction::MetricsHistory;
          }

          if (ExactRoute(Path, Method, {"/connections"}, {"GET"}))
          {
               return RouteAction::Connections;
          }

          if (ExactRoute(Path, Method, {"/doctotal"}, {"GET"}))
          {
               return RouteAction::DocTotal;
          }

          if (NormalizedPath == "/flush" && Method == "POST")
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("http_routes", "MATCHED /flush route - normalized_path='" + NormalizedPath + "' path='" + Path + "' method='" + Method + "'.");
               }

               return RouteAction::Flush;
          }

          if (ExactRoute(Path, Method, {"/update-counters"}, {"GET", "POST"}))
          {
               return RouteAction::UpdateCounters;
          }

          if (ExactRoute(Path, Method, {"/debug/counters"}, {"GET"}))
          {
               return RouteAction::DebugCounters;
          }

          if (ExactRoute(Path, Method, {"/repair"}, {"GET", "POST"}))
          {
               return RouteAction::Repair;
          }

          if (ExactRoute(Path, Method, {"/stopwords"}, {"GET"}))
          {
               return RouteAction::ListAllStopwords;
          }

          if (ExactRoute(Path, Method, {"/stopwords/global"}, {"GET"}))
          {
               return RouteAction::ListGlobalStopwords;
          }

          if (ExactRoute(Path, Method, {"/stopwords/global"}, {"POST"}))
          {
               return RouteAction::CreateGlobalStopword;
          }

          if (PrefixRoute(Path, Method, "/stopwords/global/", {"DELETE"}))
          {
               return RouteAction::DeleteGlobalStopword;
          }

          if (ExactRoute(Path, Method, {"/collections/distributed"}, {"GET"}))
          {
               return RouteAction::ListCollectionsDistributed;
          }

          if (ExactRoute(Path, Method, {"/collections"}, {"GET"}))
          {
               return RouteAction::ListCollections;
          }

          if (ExactRoute(Path, Method, {"/collections"}, {"POST"}))
          {
               return RouteAction::CreateCollection;
          }

          if (RouteInfo.IsCollectionPath && (RouteInfo.SegmentEquals(2, "vector_search") || RouteInfo.IsVectorSearchAlias) && (Method == "GET" || Method == "POST"))
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

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 && !RouteInfo.IsDocumentsSearch && !RouteInfo.IsDocumentsMaybe && Method == "GET")
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

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 && !RouteInfo.IsDocumentsSearch && !RouteInfo.IsDocumentsMaybe && Method == "PUT")
          {
               return RouteAction::UpdateDocument;
          }

          if (RouteInfo.IsDocumentsChild && RouteInfo.Segments.size() == 4 && !RouteInfo.IsDocumentsSearch && !RouteInfo.IsDocumentsMaybe && Method == "DELETE")
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

          if (RouteInfo.IsOverridesRoot && Method == "GET")
          {
               return RouteAction::ListOverrides;
          }

          if (RouteInfo.IsOverridesChild && RouteInfo.Segments.size() == 4 && (Method == "POST" || Method == "PUT"))
          {
               return RouteAction::UpsertOverride;
          }

          if (RouteInfo.IsOverridesChild && RouteInfo.Segments.size() == 4 && Method == "GET")
          {
               return RouteAction::GetOverride;
          }

          if (RouteInfo.IsOverridesChild && RouteInfo.Segments.size() == 4 && Method == "DELETE")
          {
               return RouteAction::DeleteOverride;
          }

          if (ExactRoute(Path, Method, {"/aliases"}, {"GET"}))
          {
               return RouteAction::ListAliases;
          }

          if (PrefixRoute(Path, Method, "/aliases/", {"POST", "PUT"}))
          {
               return RouteAction::UpsertAlias;
          }

          if (PrefixRoute(Path, Method, "/aliases/", {"GET"}))
          {
               return RouteAction::GetAlias;
          }

          if (PrefixRoute(Path, Method, "/aliases/", {"DELETE"}))
          {
               return RouteAction::DeleteAlias;
          }

          if (ExactRoute(Path, Method, {"/multi_search"}, {"GET", "POST"}))
          {
               return RouteAction::MultiSearch;
          }

          if (ExactRoute(Path, Method, {"/search"}, {"GET", "POST"}))
          {
               return RouteAction::GlobalSearch;
          }

          if (ExactRoute(Path, Method, {"/users"}, {"GET"}))
          {
               return RouteAction::ListUsers;
          }

          if (ExactRoute(Path, Method, {"/users"}, {"POST"}))
          {
               return RouteAction::CreateUser;
          }

          if (PrefixRoute(Path, Method, "/users/", {"GET"}))
          {
               return RouteAction::GetUser;
          }

          if (PrefixRoute(Path, Method, "/users/", {"DELETE"}))
          {
               return RouteAction::DeleteUser;
          }

          if (PrefixRoute(Path, Method, "/users/", {"PUT"}))
          {
               return RouteAction::UpdateUser;
          }

          if (ExactRoute(Path, Method, {"/keys"}, {"GET"}))
          {
               return RouteAction::ListKeys;
          }

          if (ExactRoute(Path, Method, {"/keys"}, {"POST"}))
          {
               return RouteAction::CreateKey;
          }

          if (PrefixRoute(Path, Method, "/keys/", {"GET"}))
          {
               return RouteAction::GetKey;
          }

          if (PrefixRoute(Path, Method, "/keys/", {"DELETE"}))
          {
               return RouteAction::DeleteKey;
          }

          if (PrefixRoute(Path, Method, "/keys/", {"PUT"}))
          {
               return RouteAction::UpdateKey;
          }

          if (ExactRoute(Path, Method, {"/"}, {"GET"}))
          {
               return RouteAction::Status;
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
          case RouteAction::Health:
               return "Health";
          case RouteAction::Ping:
               return "Ping";
          case RouteAction::Stats:
               return "Stats";
          case RouteAction::Metrics:
               return "Metrics";
          case RouteAction::MetricsHistory:
               return "MetricsHistory";
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
          case RouteAction::AnalyticsClick:
               return "AnalyticsClick";
          case RouteAction::ListModules:
               return "ListModules";
          case RouteAction::GetModuleSyntax:
               return "GetModuleSyntax";
          case RouteAction::ModuleAPI:
               return "ModuleAPI";
          case RouteAction::NotFound:
               return "NotFound";
          default:
               return "Unknown";
     }
}
