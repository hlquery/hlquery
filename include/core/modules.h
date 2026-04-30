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

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "api/httpserver.h"

/* Forward declaration for document-oriented module callbacks. */

struct Document;

/* Forward declaration for search hit inspection and ranking hooks. */

struct SearchHit;

/* Describes one parameter exposed by a module HTTP API endpoint. */

struct ModuleAPIParameterSpec
{
     /* Public parameter name shown in docs and help output. */

     std::string Name;

     /* Logical value type expected by the endpoint. */

     std::string Type;

     /* Human-readable explanation for callers. */

     std::string Description;

     /* Whether the parameter must be supplied by the caller. */

     bool Required = false;
};

/* Bit flags describing hard runtime dependencies for one module. */

enum ModuleRequirementFlags : uint32_t
{
     /* Module has no special startup requirements. */

     ModuleRequirementNone = 0,
};

/* Describes the external HTTP API surface exported by one module. */

struct ModuleAPIDescription
{
     /* Stable module name used in discovery and routing output. */

     std::string Name;

     /* Short summary shown in docs or help text. */

     std::string Summary;

     /* Example syntax string for quick reference. */

     std::string Syntax;

     /* Minimum accepted parameter count for this endpoint. */

     unsigned int MinParameters = 0;

     /* Maximum accepted parameter count for this endpoint. */

     unsigned int MaxParameters = 0;

     std::vector<ModuleAPIParameterSpec> Parameters;

     std::vector<std::string> Examples;

     uint32_t RequirementFlags = ModuleRequirementNone;
};

/* Describes one parameter accepted by a shared module command. */

struct ModuleCommandParameterSpec
{
     /* Public parameter name accepted by the command. */

     std::string Name;

     /* Expected value type for validation and docs. */

     std::string Type;

     /* Help text describing the meaning of the parameter. */

     std::string Description;

     /* Whether the command requires this parameter. */

     bool Required = false;
};

/* Describes one shared command routed into a runtime module. */

struct ModuleCommandSpec
{
     /* Command route segment such as stats, reset, or export. */

     std::string Route;

     std::string Summary;

     std::string Syntax;

     unsigned int MinParameters = 0;

     unsigned int MaxParameters = 0;

     std::vector<ModuleCommandParameterSpec> Parameters;

     std::vector<std::string> Examples;
};

/* Normalized request object passed into shared module command handlers. */

struct ModuleCommandRequest
{
     /* Transport identifier such as http or cli. */

     std::string Transport;

     std::string Route;

     std::vector<std::string> Parameters;

     std::vector<std::string> PositionalParameters;

     std::map<std::string, std::string> NamedParameters;

     std::string Body;

     bool Authenticated = false;

     bool IsAdmin = false;

     bool IsAPIKey = false;

     std::string AuthToken;

     std::string RequesterUser;

     std::string APIKeyID;

     std::string RemoteAddress;

     /* Cooperative cancellation probe for long-running commands. */

     std::function<bool()> IsCancelled;
};

/* Normalized response returned by shared module command handlers. */

struct ModuleCommandResponse
{
     /* Whether the command completed successfully. */

     bool Success = false;

     signed int StatusCode = 200;

     std::string ContentType = "application/json";

     std::string Message;

     std::string Body;
};

/* Shared payload for search-based module hooks. */

struct SearchEvent
{
     /* Original user query string. */

     std::string Query;

     /* Optional analytics tag forwarded by the request. */

     std::string AnalyticsTag;

     /* Collection targeted by the search. */

     std::string Collection;

     /* Measured server-side search latency in milliseconds. */

     uint64_t SearchTimeMS = 0;

     /* Number of matches found before pagination. */

     uint64_t Found = 0;

     /* Number of hits returned to the client. */

     uint64_t Returned = 0;

     /* Remote IP recorded for analytics and audit hooks. */

     std::string RequesterIP;

     /* Authenticated username when available. */

     std::string RequesterUser;

     /* Whether the search was authenticated. */

     bool Authenticated = false;

     /* Whether the result came from distributed execution. */

     bool Distributed = false;
};

/* Describes whether a pre-check hook permits or blocks an operation. */

enum class ModulePreCheckAction
{
     /* Allow the core operation to continue. */

     Pass,

     /* Deny the operation and return the supplied error. */

     Deny
};

/* Structured denial payload returned by pre-check hooks. */

struct ModulePreCheckResult
{
     /* Final decision taken by the module hook. */

     ModulePreCheckAction Action = ModulePreCheckAction::Pass;
     signed int HttpStatus = 400;
     signed int ProtocolCode = 24000;

     std::string Message;

     std::string Details;
};

/* Enumerates every lifecycle and policy hook supported by RuntimeModule. */

enum class ModuleHook : size_t
{
     /* Startup and periodic lifecycle hooks. */

     OnStartup,
     OnThreadPoolsReady,
     OnEveryOneMinute,
     OnIdleTick,
     OnNewTimer,

     /* Passive observation hooks for requests and searches. */

     OnRequestAnalytics,
     OnAuthenticatedRequest,
     OnPingRequest,
     OnDBRequest,
     OnSearchCollection,
     OnSearchDocument,
     ComputeSearchWeightMultiplier,

     /* Pre-flight policy hooks for mutating operations. */

     OnPreCreateCollection,
     OnPreUpdateCollection,
     OnPreDeleteCollection,
     OnPreAddDocument,
     OnPreUpdateDocument,
     OnPreBulkImportDocuments,
     OnPreDeleteDocument,
     OnPreDeleteDocuments,
     OnPreUpdateByQuery,
     OnPreDeleteByQuery,
     OnPreUpsertAlias,
     OnPreDeleteAlias,
     OnPreUpsertSynonym,
     OnPreDeleteSynonym,
     OnPreCreateStopword,
     OnPreDeleteStopword,
     OnPreUpsertOverride,
     OnPreDeleteOverride,
     OnPreCreateUser,
     OnPreUpdateUser,
     OnPreDeleteUser,
     OnPreCreateKey,
     OnPreUpdateKey,
     OnPreDeleteKey,
     OnPreLinksConnect,
     OnPreLinksDisconnect,
     OnPreFlush,
     OnPreRepair,

     /* Post-success notification hooks for completed operations. */

     OnCreateCollection,
     OnUpdateCollection,
     OnDeleteCollection,
     OnAddDocument,
     OnUpdateDocument,
     OnDeleteDocument,
     OnDeleteDocuments,
     OnBulkImportDocuments,
     OnUpdateByQuery,
     OnDeleteByQuery,
     OnGlobalSynAdd,
     OnGlobalSynDel,
     OnUpsertSynonym,
     OnDeleteSynonym,
     OnGlobalStopwordAdd,
     OnCreateStopword,
     OnDeleteStopword,
     OnUpsertOverride,
     OnDeleteOverride,
     OnUpsertAlias,
     OnDeleteAlias,
     OnFlush,
     OnLinksConnect,
     OnLinksDisconnect,
     OnRepair,
     OnAnalyticsClick,

     /* Sentinel used to size hook tracking storage. */

     OnCount
};

#define HLQUERY_MODULE_HOOK_METHODS(X)                                                                                 \
     X(OnStartup)                                                                                                       \
     X(OnThreadPoolsReady)                                                                                              \
     X(OnEveryOneMinute)                                                                                                \
     X(OnIdleTick)                                                                                                      \
     X(OnNewTimer)                                                                                                      \
     X(OnRequestAnalytics)                                                                                              \
     X(OnAuthenticatedRequest)                                                                                          \
     X(OnPingRequest)                                                                                                   \
     X(OnDBRequest)                                                                                                     \
     X(OnSearchCollection)                                                                                              \
     X(OnSearchDocument)                                                                                                \
     X(ComputeSearchWeightMultiplier)                                                                                   \
     X(OnPreCreateCollection)                                                                                           \
     X(OnPreUpdateCollection)                                                                                           \
     X(OnPreDeleteCollection)                                                                                           \
     X(OnPreAddDocument)                                                                                                \
     X(OnPreUpdateDocument)                                                                                             \
     X(OnPreBulkImportDocuments)                                                                                        \
     X(OnPreDeleteDocument)                                                                                             \
     X(OnPreDeleteDocuments)                                                                                            \
     X(OnPreUpdateByQuery)                                                                                              \
     X(OnPreDeleteByQuery)                                                                                              \
     X(OnPreUpsertAlias)                                                                                                \
     X(OnPreDeleteAlias)                                                                                                \
     X(OnPreUpsertSynonym)                                                                                              \
     X(OnPreDeleteSynonym)                                                                                              \
     X(OnPreCreateStopword)                                                                                             \
     X(OnPreDeleteStopword)                                                                                             \
     X(OnPreUpsertOverride)                                                                                             \
     X(OnPreDeleteOverride)                                                                                             \
     X(OnPreCreateUser)                                                                                                 \
     X(OnPreUpdateUser)                                                                                                 \
     X(OnPreDeleteUser)                                                                                                 \
     X(OnPreCreateKey)                                                                                                  \
     X(OnPreUpdateKey)                                                                                                  \
     X(OnPreDeleteKey)                                                                                                  \
     X(OnPreLinksConnect)                                                                                               \
     X(OnPreLinksDisconnect)                                                                                            \
     X(OnPreFlush)                                                                                                      \
     X(OnPreRepair)                                                                                                     \
     X(OnCreateCollection)                                                                                              \
     X(OnUpdateCollection)                                                                                              \
     X(OnDeleteCollection)                                                                                              \
     X(OnAddDocument)                                                                                                   \
     X(OnUpdateDocument)                                                                                                \
     X(OnDeleteDocument)                                                                                                \
     X(OnDeleteDocuments)                                                                                               \
     X(OnBulkImportDocuments)                                                                                           \
     X(OnUpdateByQuery)                                                                                                 \
     X(OnDeleteByQuery)                                                                                                 \
     X(OnGlobalSynAdd)                                                                                                  \
     X(OnGlobalSynDel)                                                                                                  \
     X(OnUpsertSynonym)                                                                                                 \
     X(OnDeleteSynonym)                                                                                                 \
     X(OnGlobalStopwordAdd)                                                                                             \
     X(OnCreateStopword)                                                                                                \
     X(OnDeleteStopword)                                                                                                \
     X(OnUpsertOverride)                                                                                                \
     X(OnDeleteOverride)                                                                                                \
     X(OnUpsertAlias)                                                                                                   \
     X(OnDeleteAlias)                                                                                                   \
     X(OnFlush)                                                                                                         \
     X(OnLinksConnect)                                                                                                  \
     X(OnLinksDisconnect)                                                                                               \
     X(OnRepair)                                                                                                        \
     X(OnAnalyticsClick)

/* Base class for runtime-loadable modules. */

class RuntimeModule
{
   protected:
     /* Number of hook slots tracked by the module base class. */

     static constexpr size_t HookCount = static_cast<size_t>(ModuleHook::OnCount);

   private:

     /* The runtime module name exposed to the loader. */

     std::string module_name;

     /* Whether the module exposes routes under /modules. */

     bool api_route_enabled = false;

     /* Bitmask of startup requirements declared by the module. */

     uint32_t requirement_flags = ModuleRequirementNone;

     /* Marks which hooks this module explicitly handles. */

     std::array<bool, HookCount> attached_hooks {};

   protected:

     template <typename Derived>
     static constexpr std::array<bool, HookCount> BuildAutomaticHookMap()
     {
          std::array<bool, HookCount> Hooks{};

 #define HLQUERY_SET_AUTO_HOOK(Method)                                                                                  \
          Hooks[static_cast<size_t>(ModuleHook::Method)] =                                                              \
               !std::is_same_v<decltype(&Derived::Method), decltype(&RuntimeModule::Method)>;
          HLQUERY_MODULE_HOOK_METHODS(HLQUERY_SET_AUTO_HOOK)
 #undef HLQUERY_SET_AUTO_HOOK

          return Hooks;
     }

     template <typename Derived>
     void AutoAttachHooks()
     {
          attached_hooks = BuildAutomaticHookMap<Derived>();
     }

  public:

     /* Public module interface begins here. */

     /* Constructs a runtime module with a fixed exported name. */

     explicit RuntimeModule(const std::string& Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
          : module_name(Name),
            api_route_enabled(EnableAPIRoute),
            requirement_flags(RequirementFlags)
     {

     }

     /* Virtual destructor for safe unloading through base pointers. */

     virtual ~RuntimeModule();

     /* Returns the configured module name. */

     const std::string& GetName() const
     {
          return module_name;
     }

     /* Returns whether /modules routing is enabled for this module. */

     bool IsAPIRouteEnabled() const
     {
          return api_route_enabled;
     }

     /* Returns whether the module attached one specific hook. */

     bool HandlesHook(ModuleHook Hook) const
     {
          return attached_hooks[static_cast<size_t>(Hook)];
     }

     /* Returns hard requirements that must be satisfied before the module starts. */

     virtual uint32_t GetRequirementFlags() const
     {
          return requirement_flags;
     }

     /* Returns the storage prefix reserved for this module. */

     std::string GetStoragePrefix() const;

     /* Builds one fully-qualified storage key for this module. */

     std::string MakeStorageKey(const std::string& Key) const;

     /* Stores one module-scoped value in the shared database. */

     bool SetStorageValue(const std::string& Key, const std::string& Value) const;

     /* Retrieves one module-scoped value from the shared database. */

     std::string GetStorageValue(const std::string& Key) const;

     /* Deletes one module-scoped value from the shared database. */

     bool DeleteStorageValue(const std::string& Key) const;

     /* Lists module-scoped keys relative to the module namespace. */

     std::vector<std::string> ListStorageKeys(const std::string& Pattern = "*") const;

     /* Deletes every stored key owned by this module. */

     size_t ClearStorage() const;

     /* Starts the module after configuration has been loaded. */

     virtual bool Start(const ServerConfig& Config, std::string& ErrorMessage)
     {
          (void)Config;
          (void)ErrorMessage;
          return true;
     }

     /* Stops the module before it is unloaded. */

     virtual void Stop() = 0;

     /* Lifecycle hooks begin here. */

     /* Called once before module shutdown/unload begins. */

     virtual void OnUnloadModule()
     {

     }

     /* Called once after initialization completes and before the main loop starts. */

     virtual void OnStartup()
     {

     }

     /* Called once shared thread pools are initialized and ready for use. */

     virtual void OnThreadPoolsReady()
     {

     }

    /* Called once per wall-clock minute from the main loop. */

    virtual void OnEveryOneMinute()
    {

    }

    /* Called every idle tick (main loop wakeup) with the current timestamp. */

    virtual void OnIdleTick(time_t NowTime)
    {
         (void)NowTime;
    }

     /* Called after a new timer has been added to the timer manager. */

     virtual void OnNewTimer(uint64_t DelayMS, bool Repeating, size_t TotalTimers)
     {
          (void)DelayMS;
          (void)Repeating;
          (void)TotalTimers;
     }

     /* Called after an HTTP request has completed so modules can observe request activity. */

     virtual void OnRequestAnalytics(const HttpRequest&, const HttpResponse&, RouteAction)
     {

     }

     /* Called after an authenticated request has completed so modules can inspect auth usage. */

     virtual void OnAuthenticatedRequest(const HttpRequest&, RouteAction)
     {

     }

     /* Called after a ping request has been handled successfully. */

     virtual void OnPingRequest(const HttpRequest&)
     {

     }

     /* Called after a database status request has been handled successfully. */

     virtual void OnDBRequest(const HttpRequest&)
     {

     }

     /* Search observation hooks begin here. */

    /* Called after a collection-level search has completed successfully. */

    virtual void OnSearchCollection(const SearchEvent& Event)
    {

    }

    /* Called after a document or vector search has completed successfully. */

    virtual void OnSearchDocument(const SearchEvent& Event)
    {

    }

     /* Returns a multiplier used to adjust one hit after retrieval and before final ranking. */

     virtual float ComputeSearchWeightMultiplier(const std::string& Collection,
                                                 const std::string& Query,
                                                 const std::string& RankingMode,
                                                 const SearchHit&,
                                                 float BaseScore)
     {
          (void)Collection;
          (void)Query;
          (void)RankingMode;
          (void)BaseScore;
          return 1.0f;
     }

     /* Pre-check policy hooks begin here. */

     /* Called after a collection has been created successfully. */

     virtual ModulePreCheckResult OnPreCreateCollection(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a collection has been updated. */

     virtual ModulePreCheckResult OnPreUpdateCollection(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a collection has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteCollection(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been added. */

     virtual ModulePreCheckResult OnPreAddDocument(const std::string&, const Document&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been updated. */

     virtual ModulePreCheckResult OnPreUpdateDocument(const std::string&, const Document&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before documents have been bulk imported. */

     virtual ModulePreCheckResult OnPreBulkImportDocuments(const std::string&, uint64_t, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteDocument(const std::string&, const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before multiple documents have been deleted. */

     virtual ModulePreCheckResult OnPreDeleteDocuments(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before update-by-query operations. */

     virtual ModulePreCheckResult OnPreUpdateByQuery(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before delete-by-query operations. */

     virtual ModulePreCheckResult OnPreDeleteByQuery(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an alias has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertAlias(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an alias has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteAlias(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a synonym group has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertSynonym(const std::string&, const std::string&, bool, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a synonym group has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteSynonym(const std::string&, const std::string&, bool, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before one or more stopwords have been added. */

     virtual ModulePreCheckResult OnPreCreateStopword(const std::string&, const std::vector<std::string>&, bool, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before one stopword has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteStopword(const std::string&, const std::string&, bool, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an override has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertOverride(const std::string&, const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an override has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteOverride(const std::string&, const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been created. */

     virtual ModulePreCheckResult OnPreCreateUser(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been updated. */

     virtual ModulePreCheckResult OnPreUpdateUser(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteUser(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been created. */

     virtual ModulePreCheckResult OnPreCreateKey(const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been updated. */

     virtual ModulePreCheckResult OnPreUpdateKey(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteKey(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before runtime link changes. */

     virtual ModulePreCheckResult OnPreLinksConnect(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before runtime links are disconnected. */

     virtual ModulePreCheckResult OnPreLinksDisconnect(const std::string&, const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before maintenance actions. */

     virtual ModulePreCheckResult OnPreFlush(const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before repair actions. */

     virtual ModulePreCheckResult OnPreRepair(const std::string&, const std::string&, bool)
     {
          return ModulePreCheckResult();
     }

     /* Returns whether this module currently enables demo mode. */

     virtual bool IsDemoModeEnabled() const
     {
          return false;
     }

     /* Returns the user-facing demo mode message. */

     virtual std::string GetDemoModeMessage() const
     {
          return "";
     }

     /* Post-success notification hooks begin here. */

     /* Called after a collection has been created successfully. */

     virtual void OnCreateCollection(const std::string& Collection, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {
     }

     /* Called after a collection has been updated successfully. */

     virtual void OnUpdateCollection(const std::string& Collection, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {
     }

     /* Called after a collection has been deleted successfully. */

     virtual void OnDeleteCollection(const std::string& Collection, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after a document has been added successfully. */

     virtual void OnAddDocument(const std::string& Collection, const std::string& DocumentID, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after a document has been updated successfully. */

     virtual void OnUpdateDocument(const std::string& Collection, const std::string& DocumentID, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after a document has been deleted successfully. */

     virtual void OnDeleteDocument(const std::string& Collection, const std::string& DocumentID, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after multiple documents have been deleted successfully. */

     virtual void OnDeleteDocuments(const std::string& Collection, uint64_t Count, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after a bulk document import has completed successfully. */

     virtual void OnBulkImportDocuments(const std::string& Collection, uint64_t ImportedCount, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after an update-by-query operation has completed successfully. */

     virtual void OnUpdateByQuery(const std::string& Collection,
                                  uint64_t UpdatedCount,
                                  const std::string& RequesterIP,
                                  const std::string& RequesterUser,
                                  bool Authenticated)
     {

     }

     /* Called after a delete-by-query operation has completed successfully. */

     virtual void OnDeleteByQuery(const std::string& Collection,
                                  uint64_t DeletedCount,
                                  const std::string& RequesterIP,
                                  const std::string& RequesterUser,
                                  bool Authenticated)
     {

     }

     /* Called after a global synonym group has been created or updated successfully. */

     virtual void OnGlobalSynAdd(const std::string& SynonymID,
                                 const std::string& RequesterIP,
                                 const std::string& RequesterUser,
                                 bool Authenticated)
     {

     }

     /* Called after a global synonym group has been deleted successfully. */

     virtual void OnGlobalSynDel(const std::string& SynonymID,
                                 const std::string& RequesterIP,
                                 const std::string& RequesterUser,
                                 bool Authenticated)
     {

     }

     /* Called after a synonym group has been created or updated successfully. */

     virtual void OnUpsertSynonym(const std::string& Collection,
                                  const std::string& SynonymID,
                                  bool GlobalScope,
                                  const std::string& RequesterIP,
                                  const std::string& RequesterUser,
                                  bool Authenticated)
     {

     }

     /* Called after a synonym group has been deleted successfully. */

     virtual void OnDeleteSynonym(const std::string& Collection,
                                  const std::string& SynonymID,
                                  bool GlobalScope,
                                  const std::string& RequesterIP,
                                  const std::string& RequesterUser,
                                  bool Authenticated)
     {

     }

     /* Called after one or more global stopwords have been added successfully. */

     virtual void OnGlobalStopwordAdd(uint64_t Count,
                                      const std::string& RequesterIP,
                                      const std::string& RequesterUser,
                                      bool Authenticated)
     {

     }

     /* Called after one or more stopwords have been added successfully. */

     virtual void OnCreateStopword(const std::string& Collection,
                                   uint64_t Count,
                                   bool GlobalScope,
                                   const std::string& RequesterIP,
                                   const std::string& RequesterUser,
                                   bool Authenticated)
     {

     }

     /* Called after a stopword has been deleted successfully. */

     virtual void OnDeleteStopword(const std::string& Collection,
                                   const std::string& Word,
                                   bool GlobalScope,
                                   const std::string& RequesterIP,
                                   const std::string& RequesterUser,
                                   bool Authenticated)
     {
     }

     /* Called after an override has been created or updated successfully. */

     virtual void OnUpsertOverride(const std::string& Collection,
                                   const std::string& OverrideID,
                                   const std::string& RequesterIP,
                                   const std::string& RequesterUser,
                                   bool Authenticated)
     {
     }

     /* Called after an override has been deleted successfully. */

     virtual void OnDeleteOverride(const std::string& Collection,
                                   const std::string& OverrideID,
                                   const std::string& RequesterIP,
                                   const std::string& RequesterUser,
                                   bool Authenticated)
     {

     }

     /* Called after an alias has been created or updated successfully. */

     virtual void OnUpsertAlias(const std::string& AliasName,
                                const std::string& TargetCollection,
                                const std::string& RequesterIP,
                                const std::string& RequesterUser,
                                bool Authenticated)
     {
     }

     /* Called after an alias has been deleted successfully. */

     virtual void OnDeleteAlias(const std::string& AliasName,
                                const std::string& RequesterIP,
                                const std::string& RequesterUser,
                                bool Authenticated)
     {

     }

     /* Called after a full database flush has completed successfully. */

     virtual void OnFlush(uint64_t CollectionsDeleted, const std::string& RequesterIP, const std::string& RequesterUser, bool Authenticated)
     {

     }

     /* Called after a runtime cluster link has been added successfully. */

     virtual void OnLinksConnect(const std::string& Endpoint,
                                 const std::string& RequesterIP,
                                 const std::string& RequesterUser,
                                 bool Authenticated)
     {

     }

     /* Called after a runtime cluster link has been removed successfully. */

     virtual void OnLinksDisconnect(const std::string& Endpoint,
                                    const std::string& RequesterIP,
                                    const std::string& RequesterUser,
                                    bool Authenticated)
     {

     }

     /* Called after a repair action has completed successfully. */

     virtual void OnRepair(const std::string& RequesterIP,
                           const std::string& RequesterUser,
                           bool Authenticated)
     {

     }

     /* Called when the analytics click endpoint records a click event. */

     virtual void OnAnalyticsClick(const std::string& Collection,
                                   const std::string& Query,
                                   const std::string& DocumentID,
                                   signed int Rank,
                                   const std::string& RequesterIP,
                                   const std::string& RequesterUser,
                                   bool Authenticated)
     {

     }

     /* Returns metadata for the module HTTP API surface. */

     virtual ModuleAPIDescription GetAPIDescription() const
     {
          ModuleAPIDescription Description;

          Description.Name = GetName();
          Description.MinParameters = 0;
          Description.MaxParameters = 0;

          return Description;
     }

     /* Returns supported shared module commands. */

     virtual std::vector<ModuleCommandSpec> GetCommandSpecs() const
     {
          return std::vector<ModuleCommandSpec>();
     }

     /* Handles one shared module command. */

     virtual ModuleCommandResponse HandleCommand(const ModuleCommandRequest&)
     {
          ModuleCommandResponse Response;

          Response.StatusCode = 404;
          Response.Success = false;
          Response.Message = "Route not found.";
          Response.Body = "{\"error\":\"Route not found.\"}";

          return Response;
     }

     /* Handles a module HTTP API request. */

     virtual HttpResponse HandleAPIRequest(const HttpRequest&, const std::string&) const;
};

template <typename Derived>
class AutoRuntimeModule : public RuntimeModule
{
   protected:

     explicit AutoRuntimeModule(const std::string& Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
          : RuntimeModule(Name, EnableAPIRoute, RequirementFlags)
     {
          this->template AutoAttachHooks<Derived>();
     }
};

/* Signature exported by shared modules for runtime construction. */

using CreateRuntimeModuleFn = RuntimeModule* (*)();

/* Exports the standard module factory symbol expected by the loader. */

#define MODULE_LOAD(ModuleType)                                       \
     extern "C" RuntimeModule* CreateRuntimeModule()                  \
     {                                                                \
          return new ModuleType();                                    \
     }
