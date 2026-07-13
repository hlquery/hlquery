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

/* $CompilerFlags: find_compiler_flags("sqlite3", "SQLite3", "") */
/* $LinkerFlags: find_linker_flags("sqlite3", "SQLite3", "") */
/* $PackageInfo: require_system("darwin") sqlite3 pkg-config */
/* $PackageInfo: require_system("debian~") libsqlite3-dev pkg-config */
/* $PackageInfo: require_system("rhel~") sqlite-devel pkgconf-pkg-config */

#define HLQUERY_HAVE_SQLITE3 1

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "core/config.h"
#include "core/hlquery.h"
#include "core/modules.h"
#include "runtime/configreader.h"
#include "utils/jsonbuilder.h"
#include "vendor/json/json.hpp"

#if HLQUERY_HAVE_SQLITE3

struct sqlite3;
struct sqlite3_stmt;

using sqlite3_int64 = long long;
using sqlite3_callback = int (*)(void *, int, char **, char **);
using sqlite3_destructor_type = void (*)(void *);

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ERROR = 1;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
#define SQLITE_TRANSIENT reinterpret_cast<sqlite3_destructor_type>(-1)

class SQLiteDynamicAPI
{
   public:
     using OpenFn = int (*)(const char *, sqlite3 **);
     using CloseFn = int (*)(sqlite3 *);
     using ErrmsgFn = const char *(*)(sqlite3 *);
     using ExecFn = int (*)(sqlite3 *, const char *, sqlite3_callback, void *, char **);
     using FreeFn = void (*)(void *);
     using PrepareFn = int (*)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
     using StepFn = int (*)(sqlite3_stmt *);
     using FinalizeFn = int (*)(sqlite3_stmt *);
     using BindIntFn = int (*)(sqlite3_stmt *, int, int);
     using BindInt64Fn = int (*)(sqlite3_stmt *, int, sqlite3_int64);
     using BindTextFn = int (*)(sqlite3_stmt *, int, const char *, int, sqlite3_destructor_type);
     using ColumnInt64Fn = sqlite3_int64 (*)(sqlite3_stmt *, int);
     using ColumnTextFn = const unsigned char *(*)(sqlite3_stmt *, int);
     using LibversionFn = const char *(*)();

     OpenFn Open = nullptr;
     CloseFn Close = nullptr;
     ErrmsgFn Errmsg = nullptr;
     ExecFn Exec = nullptr;
     FreeFn Free = nullptr;
     PrepareFn Prepare = nullptr;
     StepFn Step = nullptr;
     FinalizeFn Finalize = nullptr;
     BindIntFn BindInt = nullptr;
     BindInt64Fn BindInt64 = nullptr;
     BindTextFn BindText = nullptr;
     ColumnInt64Fn ColumnInt64 = nullptr;
     ColumnTextFn ColumnText = nullptr;
     LibversionFn Libversion = nullptr;

     static SQLiteDynamicAPI &Instance()
     {
          static SQLiteDynamicAPI API;
          return API;
     }

     bool Load()
     {
          if (Loaded)
          {
               return true;
          }

          if (LoadAttempted)
          {
               return false;
          }

          LoadAttempted = true;

          for (const char *LibraryName : {"libsqlite3.so.0", "libsqlite3.so", "libsqlite3.dylib"})
          {
               Handle = dlopen(LibraryName, RTLD_LAZY | RTLD_LOCAL);

               if (Handle)
               {
                    break;
               }
          }

          if (!Handle)
          {
               LastError = dlerror() ? dlerror() : "libsqlite3 could not be loaded";
               return false;
          }

          Open = LoadSymbol<OpenFn>("sqlite3_open");
          Close = LoadSymbol<CloseFn>("sqlite3_close");
          Errmsg = LoadSymbol<ErrmsgFn>("sqlite3_errmsg");
          Exec = LoadSymbol<ExecFn>("sqlite3_exec");
          Free = LoadSymbol<FreeFn>("sqlite3_free");
          Prepare = LoadSymbol<PrepareFn>("sqlite3_prepare_v2");
          Step = LoadSymbol<StepFn>("sqlite3_step");
          Finalize = LoadSymbol<FinalizeFn>("sqlite3_finalize");
          BindInt = LoadSymbol<BindIntFn>("sqlite3_bind_int");
          BindInt64 = LoadSymbol<BindInt64Fn>("sqlite3_bind_int64");
          BindText = LoadSymbol<BindTextFn>("sqlite3_bind_text");
          ColumnInt64 = LoadSymbol<ColumnInt64Fn>("sqlite3_column_int64");
          ColumnText = LoadSymbol<ColumnTextFn>("sqlite3_column_text");
          Libversion = LoadSymbol<LibversionFn>("sqlite3_libversion");

          Loaded = Open && Close && Errmsg && Exec && Free && Prepare && Step && Finalize &&
                   BindInt && BindInt64 && BindText && ColumnInt64 && ColumnText && Libversion;

          if (!Loaded)
          {
               LastError = "libsqlite3 is missing required symbols";
          }

          return Loaded;
     }

     const char *Error() const
     {
          return LastError.c_str();
     }

   private:
     void *Handle = nullptr;
     bool Loaded = false;
     bool LoadAttempted = false;
     std::string LastError = "libsqlite3 is unavailable";

     template <typename FunctionType>
     FunctionType LoadSymbol(const char *Name)
     {
          return reinterpret_cast<FunctionType>(dlsym(Handle, Name));
     }
};

int sqlite3_open(const char *Path, sqlite3 **Database)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Open(Path, Database) : SQLITE_ERROR;
}

int sqlite3_close(sqlite3 *Database)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Close(Database) : SQLITE_ERROR;
}

const char *sqlite3_errmsg(sqlite3 *Database)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Errmsg(Database) : API.Error();
}

int sqlite3_exec(sqlite3 *Database, const char *SQL, sqlite3_callback Callback, void *CallbackData, char **ErrorMessage)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Exec(Database, SQL, Callback, CallbackData, ErrorMessage) : SQLITE_ERROR;
}

void sqlite3_free(void *Pointer)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     if (API.Load())
     {
          API.Free(Pointer);
     }
}

int sqlite3_prepare_v2(sqlite3 *Database, const char *SQL, int ByteCount, sqlite3_stmt **Statement, const char **Tail)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Prepare(Database, SQL, ByteCount, Statement, Tail) : SQLITE_ERROR;
}

int sqlite3_step(sqlite3_stmt *Statement)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Step(Statement) : SQLITE_ERROR;
}

int sqlite3_finalize(sqlite3_stmt *Statement)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Finalize(Statement) : SQLITE_ERROR;
}

int sqlite3_bind_int(sqlite3_stmt *Statement, int Index, int Value)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.BindInt(Statement, Index, Value) : SQLITE_ERROR;
}

int sqlite3_bind_int64(sqlite3_stmt *Statement, int Index, sqlite3_int64 Value)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.BindInt64(Statement, Index, Value) : SQLITE_ERROR;
}

int sqlite3_bind_text(sqlite3_stmt *Statement, int Index, const char *Value, int ByteCount, sqlite3_destructor_type Destructor)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.BindText(Statement, Index, Value, ByteCount, Destructor) : SQLITE_ERROR;
}

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *Statement, int Column)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.ColumnInt64(Statement, Column) : 0;
}

const unsigned char *sqlite3_column_text(sqlite3_stmt *Statement, int Column)
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.ColumnText(Statement, Column) : nullptr;
}

const char *sqlite3_libversion()
{
     SQLiteDynamicAPI &API = SQLiteDynamicAPI::Instance();
     return API.Load() ? API.Libversion() : "unavailable";
}

class SQLiteRuntimeModule final : public AutoRuntimeModule<SQLiteRuntimeModule>
{
   private:
     sqlite3 *Database = nullptr;
     std::string DatabasePath = ":memory:";
     uint64_t SnapshotsRecorded = 0;
     uint64_t SearchEventsRecorded = 0;
     mutable std::mutex DatabaseMutex;

     void LogSQLiteError(const std::string &Operation) const
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sqlite", Operation + ": " + std::string(Database ? sqlite3_errmsg(Database) : "database is not open") + ".");
          }
     }

     std::filesystem::path GetRuntimeDataDir() const
     {
          const char *EnvDataDir = std::getenv("HLQUERY_DATA_DIR");

          if (EnvDataDir && *EnvDataDir)
          {
               return std::filesystem::path(EnvDataDir);
          }

          return std::filesystem::path(HLQUERY_DATA_DIR);
     }

     std::string GetDefaultDatabasePath() const
     {
          return (GetRuntimeDataDir() / "sqlite" / "hlquery.sqlite3").string();
     }

     std::string ResolveDatabasePath(const std::string &ConfiguredPath) const
     {
          if (ConfiguredPath.empty())
          {
               return GetDefaultDatabasePath();
          }

          if (ConfiguredPath == ":memory:")
          {
               return ConfiguredPath;
          }

          std::filesystem::path PathValue(ConfiguredPath);

          if (PathValue.is_absolute())
          {
               return PathValue.lexically_normal().string();
          }

          return (GetRuntimeDataDir() / PathValue).lexically_normal().string();
     }

     bool EnsureDatabaseDirectory(std::string *ErrorMessage)
     {
          if (DatabasePath.empty() || DatabasePath == ":memory:")
          {
               return true;
          }

          const std::filesystem::path ParentPath = std::filesystem::path(DatabasePath).parent_path();

          if (ParentPath.empty())
          {
               return true;
          }

          std::error_code ErrorCode;
          std::filesystem::create_directories(ParentPath, ErrorCode);

          if (ErrorCode)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "could not create SQLite data directory '" + ParentPath.string() + "': " + ErrorCode.message();
               }

               return false;
          }

          return true;
     }

     bool ExecuteSQL(const std::string &SQL, std::string *ErrorMessage = nullptr)
     {
          char *SQLiteError = nullptr;
          const int Result = sqlite3_exec(Database, SQL.c_str(), nullptr, nullptr, &SQLiteError);

          if (Result != SQLITE_OK)
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = SQLiteError ? SQLiteError : sqlite3_errmsg(Database);
               }

               if (SQLiteError)
               {
                    sqlite3_free(SQLiteError);
               }

               return false;
          }

          return true;
     }

     uint64_t CountSnapshotRows() const
     {
          if (!Database)
          {
               return 0;
          }

          sqlite3_stmt *Statement = nullptr;
          uint64_t Count = 0;

          if (sqlite3_prepare_v2(Database, "SELECT COUNT(*) FROM analytics_snapshots;", -1, &Statement, nullptr) == SQLITE_OK)
          {
               if (sqlite3_step(Statement) == SQLITE_ROW)
               {
                    Count = static_cast<uint64_t>(sqlite3_column_int64(Statement, 0));
               }
          }

          if (Statement)
          {
               sqlite3_finalize(Statement);
          }

          return Count;
     }

     uint64_t CountSearchEventRows() const
     {
          if (!Database)
          {
               return 0;
          }

          sqlite3_stmt *Statement = nullptr;
          uint64_t Count = 0;

          if (sqlite3_prepare_v2(Database, "SELECT COUNT(*) FROM analytics_search_events;", -1, &Statement, nullptr) == SQLITE_OK)
          {
               if (sqlite3_step(Statement) == SQLITE_ROW)
               {
                    Count = static_cast<uint64_t>(sqlite3_column_int64(Statement, 0));
               }
          }

          if (Statement)
          {
               sqlite3_finalize(Statement);
          }

          return Count;
     }

     uint64_t LastInsertRowID() const
     {
          if (!Database)
          {
               return 0;
          }

          sqlite3_stmt *Statement = nullptr;
          uint64_t RowID = 0;

          if (sqlite3_prepare_v2(Database, "SELECT last_insert_rowid();", -1, &Statement, nullptr) == SQLITE_OK)
          {
               if (sqlite3_step(Statement) == SQLITE_ROW)
               {
                    RowID = static_cast<uint64_t>(sqlite3_column_int64(Statement, 0));
               }
          }

          if (Statement)
          {
               sqlite3_finalize(Statement);
          }

          return RowID;
     }

     static std::string JSONStringOrEmpty(const nlohmann::json &Object, const char *Key)
     {
          if (!Object.contains(Key) || Object[Key].is_null())
          {
               return "";
          }

          if (Object[Key].is_string())
          {
               return Object[Key].get<std::string>();
          }

          return Object[Key].dump();
     }

     static uint64_t JSONUIntOrZero(const nlohmann::json &Object, const char *Key)
     {
          if (!Object.contains(Key) || Object[Key].is_null())
          {
               return 0;
          }

          if (Object[Key].is_number_unsigned())
          {
               return Object[Key].get<uint64_t>();
          }

          if (Object[Key].is_number_integer())
          {
               const int64_t Value = Object[Key].get<int64_t>();
               return Value > 0 ? static_cast<uint64_t>(Value) : 0;
          }

          return 0;
     }

     static bool JSONBoolOrFalse(const nlohmann::json &Object, const char *Key)
     {
          return Object.contains(Key) && Object[Key].is_boolean() && Object[Key].get<bool>();
     }

     int ParseLimit(const ModuleCommandRequest &Request) const
     {
          std::string LimitValue;

          if (!Request.PositionalParameters.empty())
          {
               LimitValue = Request.PositionalParameters[0];
          }
          else
          {
               const auto It = Request.NamedParameters.find("limit");
               if (It != Request.NamedParameters.end())
               {
                    LimitValue = It->second;
               }
          }

          if (LimitValue.empty())
          {
               return 10;
          }

          try
          {
               return std::clamp(std::stoi(LimitValue), 1, 100);
          }
          catch (const std::exception &)
          {
               return 10;
          }
     }

     nlohmann::json BuildCommandSpecsJSON(const std::string &RouteFilter = "") const
     {
          nlohmann::json Commands = nlohmann::json::array();

          for (const ModuleCommandSpec &Command : GetCommandSpecs())
          {
               if (!RouteFilter.empty() && Command.Route != RouteFilter)
               {
                    continue;
               }

               nlohmann::json CommandJSON;
               CommandJSON["route"] = Command.Route;
               CommandJSON["summary"] = Command.Summary;
               CommandJSON["syntax"] = Command.Syntax;
               CommandJSON["min_parameters"] = Command.MinParameters;
               CommandJSON["max_parameters"] = Command.MaxParameters;
               CommandJSON["parameters"] = nlohmann::json::array();
               CommandJSON["examples"] = nlohmann::json::array();

               for (const ModuleCommandParameterSpec &Parameter : Command.Parameters)
               {
                    CommandJSON["parameters"].push_back({{"name", Parameter.Name},
                                                         {"type", Parameter.Type},
                                                         {"description", Parameter.Description},
                                                         {"required", Parameter.Required}});
               }

               for (const std::string &Example : Command.Examples)
               {
                    CommandJSON["examples"].push_back(Example);
               }

               Commands.push_back(CommandJSON);
          }

          return Commands;
     }

     nlohmann::json ReadLastSnapshots(int Limit) const
     {
          nlohmann::json Rows = nlohmann::json::array();

          if (!Database)
          {
               return Rows;
          }

          sqlite3_stmt *Statement = nullptr;
          const char *SQL =
               "SELECT id, created_at_ms, source, window_start_ms, window_end_ms, bucket_count, "
               "query_event_count, total_requests, payload_bytes "
               "FROM analytics_snapshots ORDER BY id DESC LIMIT ?;";

          if (sqlite3_prepare_v2(Database, SQL, -1, &Statement, nullptr) != SQLITE_OK)
          {
               LogSQLiteError("SQLite module could not prepare latest snapshots query");
               return Rows;
          }

          sqlite3_bind_int(Statement, 1, Limit);

          while (sqlite3_step(Statement) == SQLITE_ROW)
          {
               const unsigned char *SourceText = sqlite3_column_text(Statement, 2);

               Rows.push_back({{"id", static_cast<uint64_t>(sqlite3_column_int64(Statement, 0))},
                               {"created_at_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 1))},
                               {"source", SourceText ? reinterpret_cast<const char *>(SourceText) : ""},
                               {"window_start_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 3))},
                               {"window_end_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 4))},
                               {"bucket_count", static_cast<uint64_t>(sqlite3_column_int64(Statement, 5))},
                               {"query_event_count", static_cast<uint64_t>(sqlite3_column_int64(Statement, 6))},
                               {"total_requests", static_cast<uint64_t>(sqlite3_column_int64(Statement, 7))},
                               {"payload_bytes", static_cast<uint64_t>(sqlite3_column_int64(Statement, 8))}});
          }

          sqlite3_finalize(Statement);
          return Rows;
     }

     nlohmann::json ReadLastSearchEvents(int Limit) const
     {
          nlohmann::json Rows = nlohmann::json::array();

          if (!Database)
          {
               return Rows;
          }

          sqlite3_stmt *Statement = nullptr;
          const char *SQL =
               "SELECT id, snapshot_id, created_at_ms, window_start_ms, window_end_ms, action, collection, query, "
               "document_id, requester_ip, requester_user, authenticated, search_time_ms, found, returned, document_count "
               "FROM analytics_search_events ORDER BY id DESC LIMIT ?;";

          if (sqlite3_prepare_v2(Database, SQL, -1, &Statement, nullptr) != SQLITE_OK)
          {
               LogSQLiteError("SQLite module could not prepare latest search events query");
               return Rows;
          }

          sqlite3_bind_int(Statement, 1, Limit);

          while (sqlite3_step(Statement) == SQLITE_ROW)
          {
               const auto TextColumn = [Statement](int Column) -> std::string
               {
                    const unsigned char *Text = sqlite3_column_text(Statement, Column);
                    return Text ? reinterpret_cast<const char *>(Text) : "";
               };

               Rows.push_back({{"id", static_cast<uint64_t>(sqlite3_column_int64(Statement, 0))},
                               {"snapshot_id", static_cast<uint64_t>(sqlite3_column_int64(Statement, 1))},
                               {"created_at_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 2))},
                               {"window_start_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 3))},
                               {"window_end_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 4))},
                               {"action", TextColumn(5)},
                               {"collection", TextColumn(6)},
                               {"query", TextColumn(7)},
                               {"document_id", TextColumn(8)},
                               {"requester_ip", TextColumn(9)},
                               {"requester_user", TextColumn(10)},
                               {"authenticated", sqlite3_column_int64(Statement, 11) != 0},
                               {"search_time_ms", static_cast<uint64_t>(sqlite3_column_int64(Statement, 12))},
                               {"found", static_cast<uint64_t>(sqlite3_column_int64(Statement, 13))},
                               {"returned", static_cast<uint64_t>(sqlite3_column_int64(Statement, 14))},
                               {"document_count", static_cast<uint64_t>(sqlite3_column_int64(Statement, 15))}});
          }

          sqlite3_finalize(Statement);
          return Rows;
     }

     uint64_t StoreSearchEvents(uint64_t SnapshotID, const AnalyticsSnapshotEvent &Event)
     {
          if (!Database || Event.PayloadJSON.empty())
          {
               return 0;
          }

          nlohmann::json Payload;

          try
          {
               Payload = nlohmann::json::parse(Event.PayloadJSON);
          }
          catch (const std::exception &)
          {
               return 0;
          }

          if (!Payload.contains("searches") || !Payload["searches"].is_array())
          {
               return 0;
          }

          sqlite3_stmt *Statement = nullptr;
          const char *SQL =
               "INSERT INTO analytics_search_events "
               "(snapshot_id, created_at_ms, window_start_ms, window_end_ms, action, collection, query, document_id, "
               "requester_ip, requester_user, authenticated, search_time_ms, found, returned, document_count) "
               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

          if (sqlite3_prepare_v2(Database, SQL, -1, &Statement, nullptr) != SQLITE_OK)
          {
               LogSQLiteError("SQLite module could not prepare analytics search event insert");
               return 0;
          }

          uint64_t Inserted = 0;

          for (const auto &Search : Payload["searches"])
          {
               if (!Search.is_object())
               {
                    continue;
               }

               const std::string Action = JSONStringOrEmpty(Search, "action");
               const std::string Collection = JSONStringOrEmpty(Search, "collection");
               const std::string Query = JSONStringOrEmpty(Search, "query");
               const std::string DocumentID = JSONStringOrEmpty(Search, "document_id");
               const std::string RequesterIP = JSONStringOrEmpty(Search, "requester_ip");
               const std::string RequesterUser = JSONStringOrEmpty(Search, "requester_user");

               sqlite3_bind_int64(Statement, 1, static_cast<sqlite3_int64>(SnapshotID));
               sqlite3_bind_int64(Statement, 2, static_cast<sqlite3_int64>(Event.WindowEndMS));
               sqlite3_bind_int64(Statement, 3, static_cast<sqlite3_int64>(Event.WindowStartMS));
               sqlite3_bind_int64(Statement, 4, static_cast<sqlite3_int64>(Event.WindowEndMS));
               sqlite3_bind_text(Statement, 5, Action.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(Statement, 6, Collection.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(Statement, 7, Query.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(Statement, 8, DocumentID.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(Statement, 9, RequesterIP.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(Statement, 10, RequesterUser.c_str(), -1, SQLITE_TRANSIENT);
               sqlite3_bind_int(Statement, 11, JSONBoolOrFalse(Search, "authenticated") ? 1 : 0);
               sqlite3_bind_int64(Statement, 12, static_cast<sqlite3_int64>(JSONUIntOrZero(Search, "search_time_ms")));
               sqlite3_bind_int64(Statement, 13, static_cast<sqlite3_int64>(JSONUIntOrZero(Search, "found")));
               sqlite3_bind_int64(Statement, 14, static_cast<sqlite3_int64>(JSONUIntOrZero(Search, "returned")));
               sqlite3_bind_int64(Statement, 15, static_cast<sqlite3_int64>(JSONUIntOrZero(Search, "document_count")));

               if (sqlite3_step(Statement) == SQLITE_DONE)
               {
                    Inserted++;
               }

               sqlite3_finalize(Statement);
               Statement = nullptr;

               if (sqlite3_prepare_v2(Database, SQL, -1, &Statement, nullptr) != SQLITE_OK)
               {
                    break;
               }
          }

          if (Statement)
          {
               sqlite3_finalize(Statement);
          }

          return Inserted;
     }

   public:
     SQLiteRuntimeModule()
         : AutoRuntimeModule("sqlite", true)
     {
     }

     bool Start(const ServerConfig &Config, std::string &ErrorMessage) override
     {
          auto Tag = Config.GetConfigReader().GetTag("sqlite");
          std::string ConfiguredPath;

          if (Tag)
          {
               ConfiguredPath = Tag->GetPath("path", "");
          }

          DatabasePath = ResolveDatabasePath(ConfiguredPath);

          if (!EnsureDatabaseDirectory(&ErrorMessage))
          {
               ErrorMessage = "SQLite module " + ErrorMessage;
               return false;
          }

          const int OpenResult = sqlite3_open(DatabasePath.c_str(), &Database);

          if (OpenResult != SQLITE_OK)
          {
               ErrorMessage = "SQLite module could not open database '" + DatabasePath + "': ";
               ErrorMessage += Database ? sqlite3_errmsg(Database) : "unknown error";

               if (Database)
               {
                    sqlite3_close(Database);
                    Database = nullptr;
               }

               return false;
          }

          const std::string CreateSQL =
               "CREATE TABLE IF NOT EXISTS analytics_snapshots ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "created_at_ms INTEGER NOT NULL,"
               "source TEXT NOT NULL,"
               "window_start_ms INTEGER NOT NULL,"
               "window_end_ms INTEGER NOT NULL,"
               "bucket_count INTEGER NOT NULL,"
               "query_event_count INTEGER NOT NULL,"
               "total_requests INTEGER NOT NULL,"
               "payload_bytes INTEGER NOT NULL"
               ");";

          if (!ExecuteSQL(CreateSQL, &ErrorMessage))
          {
               ErrorMessage = "SQLite module could not create analytics_snapshots table: " + ErrorMessage;
               sqlite3_close(Database);
               Database = nullptr;
               return false;
          }

          const std::string CreateSearchEventsSQL =
               "CREATE TABLE IF NOT EXISTS analytics_search_events ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "snapshot_id INTEGER NOT NULL,"
               "created_at_ms INTEGER NOT NULL,"
               "window_start_ms INTEGER NOT NULL,"
               "window_end_ms INTEGER NOT NULL,"
               "action TEXT NOT NULL,"
               "collection TEXT NOT NULL,"
               "query TEXT NOT NULL,"
               "document_id TEXT NOT NULL,"
               "requester_ip TEXT NOT NULL,"
               "requester_user TEXT NOT NULL,"
               "authenticated INTEGER NOT NULL,"
               "search_time_ms INTEGER NOT NULL,"
               "found INTEGER NOT NULL,"
               "returned INTEGER NOT NULL,"
               "document_count INTEGER NOT NULL,"
               "FOREIGN KEY(snapshot_id) REFERENCES analytics_snapshots(id)"
               ");";

          if (!ExecuteSQL(CreateSearchEventsSQL, &ErrorMessage))
          {
               ErrorMessage = "SQLite module could not create analytics_search_events table: " + ErrorMessage;
               sqlite3_close(Database);
               Database = nullptr;
               return false;
          }

          if (!ExecuteSQL("CREATE INDEX IF NOT EXISTS idx_analytics_search_events_snapshot_id ON analytics_search_events(snapshot_id);", &ErrorMessage) ||
              !ExecuteSQL("CREATE INDEX IF NOT EXISTS idx_analytics_search_events_collection ON analytics_search_events(collection);", &ErrorMessage))
          {
               ErrorMessage = "SQLite module could not create analytics_search_events indexes: " + ErrorMessage;
               sqlite3_close(Database);
               Database = nullptr;
               return false;
          }

          SnapshotsRecorded = CountSnapshotRows();
          SearchEventsRecorded = CountSearchEventRows();

          return true;
     }

     void Stop() override
     {
          std::lock_guard<std::mutex> Lock(DatabaseMutex);

          if (Database)
          {
               sqlite3_close(Database);
               Database = nullptr;
          }
     }

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription Description;

          Description.Name = "sqlite";
          Description.Summary = "Stores module-owned SQLite state, including analytics snapshot records emitted by OnSnapshot.";
          Description.Syntax = "GET /modules/sqlite | POST /modules/sqlite/status | POST /modules/sqlite/routes | POST /modules/sqlite/last | POST /modules/sqlite/queries";
          Description.MinParameters = 0;
          Description.MaxParameters = 0;
          Description.Examples.push_back("hlquery-cli module sqlite status");
          Description.Examples.push_back("hlquery-cli module sqlite routes");
          Description.Examples.push_back("hlquery-cli module sqlite last 10");
          Description.Examples.push_back("hlquery-cli module sqlite queries 10");

          return Description;
     }

     void OnSnapshot(const AnalyticsSnapshotEvent &Event) override
     {
          std::lock_guard<std::mutex> Lock(DatabaseMutex);

          if (!Database)
          {
               return;
          }

          sqlite3_stmt *Statement = nullptr;
          const char *SQL =
               "INSERT INTO analytics_snapshots "
               "(created_at_ms, source, window_start_ms, window_end_ms, bucket_count, query_event_count, total_requests, payload_bytes) "
               "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

          if (sqlite3_prepare_v2(Database, SQL, -1, &Statement, nullptr) != SQLITE_OK)
          {
               LogSQLiteError("SQLite module could not prepare analytics snapshot insert");
               return;
          }

          sqlite3_bind_int64(Statement, 1, static_cast<sqlite3_int64>(Event.WindowEndMS));
          sqlite3_bind_text(Statement, 2, Event.Source.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64(Statement, 3, static_cast<sqlite3_int64>(Event.WindowStartMS));
          sqlite3_bind_int64(Statement, 4, static_cast<sqlite3_int64>(Event.WindowEndMS));
          sqlite3_bind_int64(Statement, 5, static_cast<sqlite3_int64>(Event.BucketCount));
          sqlite3_bind_int64(Statement, 6, static_cast<sqlite3_int64>(Event.QueryEventCount));
          sqlite3_bind_int64(Statement, 7, static_cast<sqlite3_int64>(Event.TotalRequests));
          sqlite3_bind_int64(Statement, 8, static_cast<sqlite3_int64>(Event.PayloadBytes));

          const int StepResult = sqlite3_step(Statement);

          if (StepResult == SQLITE_DONE)
          {
               SnapshotsRecorded++;
               SearchEventsRecorded += StoreSearchEvents(LastInsertRowID(), Event);
          }
          else
          {
               LogSQLiteError("SQLite module could not insert analytics snapshot");
          }

          sqlite3_finalize(Statement);
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          ModuleCommandSpec StatusCommand;

          StatusCommand.Route = "status";
          StatusCommand.Summary = "Shows SQLite module state and snapshot counters.";
          StatusCommand.Syntax = "module sqlite status";
          StatusCommand.MinParameters = 0;
          StatusCommand.MaxParameters = 0;
          StatusCommand.Examples.push_back("hlquery-cli module sqlite status");

          ModuleCommandSpec RoutesCommand;

          RoutesCommand.Route = "routes";
          RoutesCommand.Summary = "Lists SQLite module routes, descriptions, parameters, and examples.";
          RoutesCommand.Syntax = "module sqlite routes";
          RoutesCommand.MinParameters = 0;
          RoutesCommand.MaxParameters = 0;
          RoutesCommand.Examples.push_back("hlquery-cli module sqlite routes");

          ModuleCommandSpec LastCommand;

          LastCommand.Route = "last";
          LastCommand.Summary = "Returns the most recent analytics snapshot rows stored by the SQLite module.";
          LastCommand.Syntax = "module sqlite last [limit]";
          LastCommand.MinParameters = 0;
          LastCommand.MaxParameters = 1;
          LastCommand.Parameters.push_back({"limit", "int", "Maximum number of snapshot rows to return, clamped to 1..100. Defaults to 10.", false});
          LastCommand.Examples.push_back("hlquery-cli module sqlite last");
          LastCommand.Examples.push_back("hlquery-cli module sqlite last 10");

          ModuleCommandSpec QueriesCommand;

          QueriesCommand.Route = "queries";
          QueriesCommand.Summary = "Returns the most recent detailed analytics search/query rows stored by the SQLite module.";
          QueriesCommand.Syntax = "module sqlite queries [limit]";
          QueriesCommand.MinParameters = 0;
          QueriesCommand.MaxParameters = 1;
          QueriesCommand.Parameters.push_back({"limit", "int", "Maximum number of detailed rows to return, clamped to 1..100. Defaults to 10.", false});
          QueriesCommand.Examples.push_back("hlquery-cli module sqlite queries");
          QueriesCommand.Examples.push_back("hlquery-cli module sqlite queries 10");

          return {StatusCommand, RoutesCommand, LastCommand, QueriesCommand};
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string Route = Request.Route.empty() ? "status" : Request.Route;

          if (Route == "routes")
          {
               const std::string RouteFilter = Request.PositionalParameters.empty() ? "" : Request.PositionalParameters.front();

               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "routes";
               Body["summary"] = "SQLite module routes.";
               Body["commands"] = BuildCommandSpecsJSON(RouteFilter);

               if (!RouteFilter.empty())
               {
                    Body["filter"] = RouteFilter;
               }

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route == "last" || Route == "snapshots")
          {
               const int Limit = ParseLimit(Request);
               std::lock_guard<std::mutex> Lock(DatabaseMutex);
               nlohmann::json Snapshots = ReadLastSnapshots(Limit);

               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "last";
               Body["limit"] = Limit;
               Body["count"] = Snapshots.size();
               Body["snapshots"] = Snapshots;

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route == "queries" || Route == "searches")
          {
               const int Limit = ParseLimit(Request);
               std::lock_guard<std::mutex> Lock(DatabaseMutex);
               nlohmann::json SearchEvents = ReadLastSearchEvents(Limit);

               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "queries";
               Body["limit"] = Limit;
               Body["count"] = SearchEvents.size();
               Body["queries"] = SearchEvents;

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route != "status")
          {
               return RuntimeModule::HandleCommand(Request);
          }

          ModuleCommandResponse Response;

          std::lock_guard<std::mutex> Lock(DatabaseMutex);

          Response.Success = true;
          Response.StatusCode = 200;
          Response.Body = JsonBuilder()
                               .Add("module", "sqlite")
                               .Add("loaded", Database != nullptr)
                               .Add("path", DatabasePath)
                               .Add("sqlite_version", sqlite3_libversion())
                               .Add("analytics_snapshots_recorded", static_cast<unsigned long long>(SnapshotsRecorded))
                               .Add("analytics_snapshot_rows", static_cast<unsigned long long>(CountSnapshotRows()))
                               .Add("analytics_search_events_recorded", static_cast<unsigned long long>(SearchEventsRecorded))
                               .Add("analytics_search_event_rows", static_cast<unsigned long long>(CountSearchEventRows()))
                               .ToString();

          return Response;
     }
};

#else

class SQLiteRuntimeModule final : public AutoRuntimeModule<SQLiteRuntimeModule>
{
   public:
     SQLiteRuntimeModule()
         : AutoRuntimeModule("sqlite", true)
     {
     }

     void Stop() override
     {
     }

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription Description;

          Description.Name = "sqlite";
          Description.Summary = "SQLite-backed module state. This build lacks sqlite3 headers, so storage is unavailable.";
          Description.Syntax = "GET /modules/sqlite | POST /modules/sqlite/status | POST /modules/sqlite/routes | POST /modules/sqlite/last | POST /modules/sqlite/queries";
          Description.MinParameters = 0;
          Description.MaxParameters = 0;
          Description.Examples.push_back("hlquery-cli module sqlite status");
          Description.Examples.push_back("hlquery-cli module sqlite routes");
          Description.Examples.push_back("hlquery-cli module sqlite last 10");
          Description.Examples.push_back("hlquery-cli module sqlite queries 10");

          return Description;
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          ModuleCommandSpec StatusCommand;

          StatusCommand.Route = "status";
          StatusCommand.Summary = "Shows SQLite module availability.";
          StatusCommand.Syntax = "module sqlite status";
          StatusCommand.MinParameters = 0;
          StatusCommand.MaxParameters = 0;
          StatusCommand.Examples.push_back("hlquery-cli module sqlite status");

          ModuleCommandSpec RoutesCommand;

          RoutesCommand.Route = "routes";
          RoutesCommand.Summary = "Lists SQLite module routes, descriptions, parameters, and examples.";
          RoutesCommand.Syntax = "module sqlite routes";
          RoutesCommand.MinParameters = 0;
          RoutesCommand.MaxParameters = 0;
          RoutesCommand.Examples.push_back("hlquery-cli module sqlite routes");

          ModuleCommandSpec LastCommand;

          LastCommand.Route = "last";
          LastCommand.Summary = "Returns recent analytics snapshots when SQLite support is available.";
          LastCommand.Syntax = "module sqlite last [limit]";
          LastCommand.MinParameters = 0;
          LastCommand.MaxParameters = 1;
          LastCommand.Parameters.push_back({"limit", "int", "Maximum number of snapshot rows to return. Defaults to 10.", false});
          LastCommand.Examples.push_back("hlquery-cli module sqlite last");
          LastCommand.Examples.push_back("hlquery-cli module sqlite last 10");

          ModuleCommandSpec QueriesCommand;

          QueriesCommand.Route = "queries";
          QueriesCommand.Summary = "Returns recent detailed analytics search/query rows when SQLite support is available.";
          QueriesCommand.Syntax = "module sqlite queries [limit]";
          QueriesCommand.MinParameters = 0;
          QueriesCommand.MaxParameters = 1;
          QueriesCommand.Parameters.push_back({"limit", "int", "Maximum number of detailed rows to return. Defaults to 10.", false});
          QueriesCommand.Examples.push_back("hlquery-cli module sqlite queries");
          QueriesCommand.Examples.push_back("hlquery-cli module sqlite queries 10");

          return {StatusCommand, RoutesCommand, LastCommand, QueriesCommand};
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string Route = Request.Route.empty() ? "status" : Request.Route;

          if (Route == "routes")
          {
               const std::string RouteFilter = Request.PositionalParameters.empty() ? "" : Request.PositionalParameters.front();
               nlohmann::json Commands = nlohmann::json::array();

               for (const ModuleCommandSpec &Command : GetCommandSpecs())
               {
                    if (!RouteFilter.empty() && Command.Route != RouteFilter)
                    {
                         continue;
                    }

                    nlohmann::json CommandJSON;
                    CommandJSON["route"] = Command.Route;
                    CommandJSON["summary"] = Command.Summary;
                    CommandJSON["syntax"] = Command.Syntax;
                    CommandJSON["min_parameters"] = Command.MinParameters;
                    CommandJSON["max_parameters"] = Command.MaxParameters;
                    CommandJSON["parameters"] = nlohmann::json::array();
                    CommandJSON["examples"] = nlohmann::json::array();

                    for (const ModuleCommandParameterSpec &Parameter : Command.Parameters)
                    {
                         CommandJSON["parameters"].push_back({{"name", Parameter.Name},
                                                              {"type", Parameter.Type},
                                                              {"description", Parameter.Description},
                                                              {"required", Parameter.Required}});
                    }

                    for (const std::string &Example : Command.Examples)
                    {
                         CommandJSON["examples"].push_back(Example);
                    }

                    Commands.push_back(CommandJSON);
               }

               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "routes";
               Body["available"] = false;
               Body["commands"] = Commands;

               if (!RouteFilter.empty())
               {
                    Body["filter"] = RouteFilter;
               }

               ModuleCommandResponse Response;
               Response.Success = true;
               Response.StatusCode = 200;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route == "last" || Route == "snapshots")
          {
               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "last";
               Body["available"] = false;
               Body["count"] = 0;
               Body["snapshots"] = nlohmann::json::array();
               Body["error"] = "SQLite support is unavailable because this build was compiled without sqlite3 headers.";

               ModuleCommandResponse Response;
               Response.Success = false;
               Response.StatusCode = 503;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route == "queries" || Route == "searches")
          {
               nlohmann::json Body;
               Body["module"] = "sqlite";
               Body["route"] = "queries";
               Body["available"] = false;
               Body["count"] = 0;
               Body["queries"] = nlohmann::json::array();
               Body["error"] = "SQLite support is unavailable because this build was compiled without sqlite3 headers.";

               ModuleCommandResponse Response;
               Response.Success = false;
               Response.StatusCode = 503;
               Response.Body = Body.dump();
               return Response;
          }

          if (Route != "status")
          {
               return RuntimeModule::HandleCommand(Request);
          }

          ModuleCommandResponse Response;

          Response.Success = true;
          Response.StatusCode = 200;
          Response.Body = JsonBuilder()
                               .Add("module", "sqlite")
                               .Add("loaded", true)
                               .Add("available", false)
                               .Add("error", "SQLite support is unavailable because this build was compiled without sqlite3 headers.")
                               .ToString();

          return Response;
     }
};

#endif

MODULE_LOAD(SQLiteRuntimeModule)
