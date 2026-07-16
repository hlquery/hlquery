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
#include <cstddef>
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

     /* Ordered parameter descriptions shown to API callers. */

     std::vector<ModuleAPIParameterSpec> Parameters;

     /* Sample invocations that illustrate valid usage. */

     std::vector<std::string> Examples;

     /* Hard runtime requirements needed before the endpoint is enabled. */

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

     /* Short summary shown in shared command help output. */

     std::string Summary;

     /* Example command syntax for quick operator reference. */

     std::string Syntax;

     /* Minimum accepted argument count for the command. */

     unsigned int MinParameters = 0;

     /* Maximum accepted argument count for the command. */

     unsigned int MaxParameters = 0;

     /* Ordered parameter specifications used for docs and validation. */

     std::vector<ModuleCommandParameterSpec> Parameters;

     /* Example command invocations for operators and tests. */

     std::vector<std::string> Examples;
};

/* Normalized request object passed into shared module command handlers. */

struct ModuleCommandRequest
{
     /* Transport identifier such as http or cli. */

     std::string Transport;

     /* Route requested under the shared module command surface. */

     std::string Route;

     /* Raw ordered parameters exactly as they were supplied. */

     std::vector<std::string> Parameters;

     /* Positional parameters after named argument extraction. */

     std::vector<std::string> PositionalParameters;

     /* Named key/value parameters resolved from the request. */

     std::map<std::string, std::string> NamedParameters;

     /* Optional raw request body forwarded to the command handler. */

     std::string Body;

     /* Whether the requester passed any accepted authentication. */

     bool Authenticated = false;

     /* Whether the requester holds administrator privileges. */

     bool IsAdmin = false;

     /* Whether the request authenticated with an API key. */

     bool IsAPIKey = false;

     /* Raw bearer token or equivalent credential when available. */

     std::string AuthToken;

     /* Resolved authenticated user name when available. */

     std::string RequesterUser;

     /* Stable API key identifier when the request used one. */

     std::string APIKeyID;

     /* Remote client address for auditing and policy checks. */

     std::string RemoteAddress;

     /* Cooperative cancellation probe for long-running commands. */

     std::function<bool()> IsCancelled;
};

/* Normalized response returned by shared module command handlers. */

struct ModuleCommandResponse
{
     /* Whether the command completed successfully. */

     bool Success = false;

     /* HTTP-style status code returned to the caller. */

     signed int StatusCode = 200;

     /* Response content type sent back to the transport layer. */

     std::string ContentType = "application/json";

     /* Short summary message describing the command result. */

     std::string Message;

     /* Serialized response payload returned to the caller. */

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

/* Shared payload emitted when analytics creates a flush snapshot. */

struct AnalyticsSnapshotEvent
{
     /* Module or subsystem that produced the snapshot. */

     std::string Source;

     /* Inclusive snapshot window start in Unix epoch milliseconds. */

     uint64_t WindowStartMS = 0;

     /* Exclusive snapshot window end in Unix epoch milliseconds. */

     uint64_t WindowEndMS = 0;

     /* Number of aggregate buckets included in the snapshot. */

     uint64_t BucketCount = 0;

     /* Number of individual query/click rows included in the snapshot. */

     uint64_t QueryEventCount = 0;

     /* Total counted requests represented by the aggregate buckets. */

     uint64_t TotalRequests = 0;

     /* Serialized payload size in bytes, when available. */

     uint64_t PayloadBytes = 0;

     /* Serialized analytics payload, when the producing module exposes one. */

     std::string PayloadJSON;
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

     /* HTTP status reported when the pre-check blocks the operation. */

     signed int HttpStatus = 400;

     /* Protocol-specific error code returned to API clients. */

     signed int ProtocolCode = 24000;

     /* Human-readable explanation for the denial result. */

     std::string Message;

     /* Optional machine-readable details for clients or logs. */

     std::string Details;
};

/* Enumerates every lifecycle and policy hook supported by RuntimeModule. */

enum class ModuleHook : size_t
{
     OnStartup,
     OnThreadPoolsReady,
     OnEveryOneMinute,
     OnIdleTick,
     OnNewTimer,
     OnRequestAnalytics,
     OnAuthenticatedRequest,
     OnPingRequest,
     OnDBRequest,
     OnStatsRequest,
     OnMetricsRequest,
     OnCacheRequest,
     OnSearchCollection,
     OnSearchDocument,
     ComputeSearchWeightMultiplier,
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
     OnSnapshot,

     /* Sentinel used to size hook tracking storage. */

     OnCount
};

/* Base class for runtime-loadable modules. */

class RuntimeModule
{
   private:

     /* The runtime module name exposed to the loader. */

     std::string module_name;

     /* Whether the module exposes routes under /modules. */

     bool api_route_enabled = false;

     /* Bitmask of startup requirements declared by the module. */

     uint32_t requirement_flags = ModuleRequirementNone;

     /* Marks which hooks this module explicitly handles. */

     std::array<bool, static_cast<size_t>(ModuleHook::OnCount)> attached_hooks{};

   protected:

     /* Number of hook slots tracked by the module base class. */

     static constexpr size_t HookCount = static_cast<size_t>(ModuleHook::OnCount);

     /* Auto-detects which hooks Derived overrides relative to RuntimeModule. */

     template <typename Derived>
     static constexpr std::array<bool, HookCount> BuildAutomaticHookMap()
     {
          std::array<bool, HookCount> Hooks{};

          Hooks[static_cast<size_t>(ModuleHook::OnStartup)] =
               !std::is_same_v<decltype(&Derived::OnStartup), decltype(&RuntimeModule::OnStartup)>;
          Hooks[static_cast<size_t>(ModuleHook::OnThreadPoolsReady)] =
               !std::is_same_v<decltype(&Derived::OnThreadPoolsReady), decltype(&RuntimeModule::OnThreadPoolsReady)>;
          Hooks[static_cast<size_t>(ModuleHook::OnEveryOneMinute)] =
               !std::is_same_v<decltype(&Derived::OnEveryOneMinute), decltype(&RuntimeModule::OnEveryOneMinute)>;
          Hooks[static_cast<size_t>(ModuleHook::OnIdleTick)] =
               !std::is_same_v<decltype(&Derived::OnIdleTick), decltype(&RuntimeModule::OnIdleTick)>;
          Hooks[static_cast<size_t>(ModuleHook::OnNewTimer)] =
               !std::is_same_v<decltype(&Derived::OnNewTimer), decltype(&RuntimeModule::OnNewTimer)>;
          Hooks[static_cast<size_t>(ModuleHook::OnRequestAnalytics)] =
               !std::is_same_v<decltype(&Derived::OnRequestAnalytics), decltype(&RuntimeModule::OnRequestAnalytics)>;
          Hooks[static_cast<size_t>(ModuleHook::OnAuthenticatedRequest)] =
               !std::is_same_v<decltype(&Derived::OnAuthenticatedRequest), decltype(&RuntimeModule::OnAuthenticatedRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPingRequest)] =
               !std::is_same_v<decltype(&Derived::OnPingRequest), decltype(&RuntimeModule::OnPingRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDBRequest)] =
               !std::is_same_v<decltype(&Derived::OnDBRequest), decltype(&RuntimeModule::OnDBRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnStatsRequest)] =
               !std::is_same_v<decltype(&Derived::OnStatsRequest), decltype(&RuntimeModule::OnStatsRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnMetricsRequest)] =
               !std::is_same_v<decltype(&Derived::OnMetricsRequest), decltype(&RuntimeModule::OnMetricsRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnCacheRequest)] =
               !std::is_same_v<decltype(&Derived::OnCacheRequest), decltype(&RuntimeModule::OnCacheRequest)>;
          Hooks[static_cast<size_t>(ModuleHook::OnSearchCollection)] =
               !std::is_same_v<decltype(&Derived::OnSearchCollection), decltype(&RuntimeModule::OnSearchCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnSearchDocument)] =
               !std::is_same_v<decltype(&Derived::OnSearchDocument), decltype(&RuntimeModule::OnSearchDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::ComputeSearchWeightMultiplier)] =
               !std::is_same_v<decltype(&Derived::ComputeSearchWeightMultiplier), decltype(&RuntimeModule::ComputeSearchWeightMultiplier)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreCreateCollection)] =
               !std::is_same_v<decltype(&Derived::OnPreCreateCollection), decltype(&RuntimeModule::OnPreCreateCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpdateCollection)] =
               !std::is_same_v<decltype(&Derived::OnPreUpdateCollection), decltype(&RuntimeModule::OnPreUpdateCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteCollection)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteCollection), decltype(&RuntimeModule::OnPreDeleteCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreAddDocument)] =
               !std::is_same_v<decltype(&Derived::OnPreAddDocument), decltype(&RuntimeModule::OnPreAddDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpdateDocument)] =
               !std::is_same_v<decltype(&Derived::OnPreUpdateDocument), decltype(&RuntimeModule::OnPreUpdateDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreBulkImportDocuments)] =
               !std::is_same_v<decltype(&Derived::OnPreBulkImportDocuments), decltype(&RuntimeModule::OnPreBulkImportDocuments)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteDocument)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteDocument), decltype(&RuntimeModule::OnPreDeleteDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteDocuments)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteDocuments), decltype(&RuntimeModule::OnPreDeleteDocuments)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpdateByQuery)] =
               !std::is_same_v<decltype(&Derived::OnPreUpdateByQuery), decltype(&RuntimeModule::OnPreUpdateByQuery)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteByQuery)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteByQuery), decltype(&RuntimeModule::OnPreDeleteByQuery)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpsertAlias)] =
               !std::is_same_v<decltype(&Derived::OnPreUpsertAlias), decltype(&RuntimeModule::OnPreUpsertAlias)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteAlias)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteAlias), decltype(&RuntimeModule::OnPreDeleteAlias)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpsertSynonym)] =
               !std::is_same_v<decltype(&Derived::OnPreUpsertSynonym), decltype(&RuntimeModule::OnPreUpsertSynonym)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteSynonym)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteSynonym), decltype(&RuntimeModule::OnPreDeleteSynonym)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreCreateStopword)] =
               !std::is_same_v<decltype(&Derived::OnPreCreateStopword), decltype(&RuntimeModule::OnPreCreateStopword)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteStopword)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteStopword), decltype(&RuntimeModule::OnPreDeleteStopword)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpsertOverride)] =
               !std::is_same_v<decltype(&Derived::OnPreUpsertOverride), decltype(&RuntimeModule::OnPreUpsertOverride)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteOverride)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteOverride), decltype(&RuntimeModule::OnPreDeleteOverride)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreCreateUser)] =
               !std::is_same_v<decltype(&Derived::OnPreCreateUser), decltype(&RuntimeModule::OnPreCreateUser)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpdateUser)] =
               !std::is_same_v<decltype(&Derived::OnPreUpdateUser), decltype(&RuntimeModule::OnPreUpdateUser)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteUser)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteUser), decltype(&RuntimeModule::OnPreDeleteUser)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreCreateKey)] =
               !std::is_same_v<decltype(&Derived::OnPreCreateKey), decltype(&RuntimeModule::OnPreCreateKey)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreUpdateKey)] =
               !std::is_same_v<decltype(&Derived::OnPreUpdateKey), decltype(&RuntimeModule::OnPreUpdateKey)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreDeleteKey)] =
               !std::is_same_v<decltype(&Derived::OnPreDeleteKey), decltype(&RuntimeModule::OnPreDeleteKey)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreLinksConnect)] =
               !std::is_same_v<decltype(&Derived::OnPreLinksConnect), decltype(&RuntimeModule::OnPreLinksConnect)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreLinksDisconnect)] =
               !std::is_same_v<decltype(&Derived::OnPreLinksDisconnect), decltype(&RuntimeModule::OnPreLinksDisconnect)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreFlush)] =
               !std::is_same_v<decltype(&Derived::OnPreFlush), decltype(&RuntimeModule::OnPreFlush)>;
          Hooks[static_cast<size_t>(ModuleHook::OnPreRepair)] =
               !std::is_same_v<decltype(&Derived::OnPreRepair), decltype(&RuntimeModule::OnPreRepair)>;
          Hooks[static_cast<size_t>(ModuleHook::OnCreateCollection)] =
               !std::is_same_v<decltype(&Derived::OnCreateCollection), decltype(&RuntimeModule::OnCreateCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpdateCollection)] =
               !std::is_same_v<decltype(&Derived::OnUpdateCollection), decltype(&RuntimeModule::OnUpdateCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteCollection)] =
               !std::is_same_v<decltype(&Derived::OnDeleteCollection), decltype(&RuntimeModule::OnDeleteCollection)>;
          Hooks[static_cast<size_t>(ModuleHook::OnAddDocument)] =
               !std::is_same_v<decltype(&Derived::OnAddDocument), decltype(&RuntimeModule::OnAddDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpdateDocument)] =
               !std::is_same_v<decltype(&Derived::OnUpdateDocument), decltype(&RuntimeModule::OnUpdateDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteDocument)] =
               !std::is_same_v<decltype(&Derived::OnDeleteDocument), decltype(&RuntimeModule::OnDeleteDocument)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteDocuments)] =
               !std::is_same_v<decltype(&Derived::OnDeleteDocuments), decltype(&RuntimeModule::OnDeleteDocuments)>;
          Hooks[static_cast<size_t>(ModuleHook::OnBulkImportDocuments)] =
               !std::is_same_v<decltype(&Derived::OnBulkImportDocuments), decltype(&RuntimeModule::OnBulkImportDocuments)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpdateByQuery)] =
               !std::is_same_v<decltype(&Derived::OnUpdateByQuery), decltype(&RuntimeModule::OnUpdateByQuery)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteByQuery)] =
               !std::is_same_v<decltype(&Derived::OnDeleteByQuery), decltype(&RuntimeModule::OnDeleteByQuery)>;
          Hooks[static_cast<size_t>(ModuleHook::OnGlobalSynAdd)] =
               !std::is_same_v<decltype(&Derived::OnGlobalSynAdd), decltype(&RuntimeModule::OnGlobalSynAdd)>;
          Hooks[static_cast<size_t>(ModuleHook::OnGlobalSynDel)] =
               !std::is_same_v<decltype(&Derived::OnGlobalSynDel), decltype(&RuntimeModule::OnGlobalSynDel)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpsertSynonym)] =
               !std::is_same_v<decltype(&Derived::OnUpsertSynonym), decltype(&RuntimeModule::OnUpsertSynonym)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteSynonym)] =
               !std::is_same_v<decltype(&Derived::OnDeleteSynonym), decltype(&RuntimeModule::OnDeleteSynonym)>;
          Hooks[static_cast<size_t>(ModuleHook::OnGlobalStopwordAdd)] =
               !std::is_same_v<decltype(&Derived::OnGlobalStopwordAdd), decltype(&RuntimeModule::OnGlobalStopwordAdd)>;
          Hooks[static_cast<size_t>(ModuleHook::OnCreateStopword)] =
               !std::is_same_v<decltype(&Derived::OnCreateStopword), decltype(&RuntimeModule::OnCreateStopword)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteStopword)] =
               !std::is_same_v<decltype(&Derived::OnDeleteStopword), decltype(&RuntimeModule::OnDeleteStopword)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpsertOverride)] =
               !std::is_same_v<decltype(&Derived::OnUpsertOverride), decltype(&RuntimeModule::OnUpsertOverride)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteOverride)] =
               !std::is_same_v<decltype(&Derived::OnDeleteOverride), decltype(&RuntimeModule::OnDeleteOverride)>;
          Hooks[static_cast<size_t>(ModuleHook::OnUpsertAlias)] =
               !std::is_same_v<decltype(&Derived::OnUpsertAlias), decltype(&RuntimeModule::OnUpsertAlias)>;
          Hooks[static_cast<size_t>(ModuleHook::OnDeleteAlias)] =
               !std::is_same_v<decltype(&Derived::OnDeleteAlias), decltype(&RuntimeModule::OnDeleteAlias)>;
          Hooks[static_cast<size_t>(ModuleHook::OnFlush)] =
               !std::is_same_v<decltype(&Derived::OnFlush), decltype(&RuntimeModule::OnFlush)>;
          Hooks[static_cast<size_t>(ModuleHook::OnLinksConnect)] =
               !std::is_same_v<decltype(&Derived::OnLinksConnect), decltype(&RuntimeModule::OnLinksConnect)>;
          Hooks[static_cast<size_t>(ModuleHook::OnLinksDisconnect)] =
               !std::is_same_v<decltype(&Derived::OnLinksDisconnect), decltype(&RuntimeModule::OnLinksDisconnect)>;
          Hooks[static_cast<size_t>(ModuleHook::OnRepair)] =
               !std::is_same_v<decltype(&Derived::OnRepair), decltype(&RuntimeModule::OnRepair)>;
          Hooks[static_cast<size_t>(ModuleHook::OnAnalyticsClick)] =
               !std::is_same_v<decltype(&Derived::OnAnalyticsClick), decltype(&RuntimeModule::OnAnalyticsClick)>;
          Hooks[static_cast<size_t>(ModuleHook::OnSnapshot)] =
               !std::is_same_v<decltype(&Derived::OnSnapshot), decltype(&RuntimeModule::OnSnapshot)>;

          return Hooks;
     }

     /* Populates the attached hook map using the derived module type. */

     template <typename Derived>
     void AutoAttachHooks()
     {
          attached_hooks = BuildAutomaticHookMap<Derived>();
     }

   public:
     /* Public module interface begins here. */

     /* Constructs a runtime module with a fixed exported name. */

     explicit RuntimeModule(const std::string &Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
         : module_name(Name),
           api_route_enabled(EnableAPIRoute),
           requirement_flags(RequirementFlags)
     {
     }

     /* Virtual destructor for safe unloading through base pointers. */

     virtual ~RuntimeModule();

     /* Returns the configured module name. */

     const std::string &GetName() const
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

     /*
      * Returns the concrete objects that should receive one hook.
      * Composite modules override this to expose owned subcomponents while
      * the loader still treats the shared object as one configured module.
      */

     virtual std::vector<RuntimeModule *> GetHookTargets(ModuleHook Hook)
     {
          std::vector<RuntimeModule *> Targets;

          if (HandlesHook(Hook))
          {
               Targets.push_back(this);
          }

          return Targets;
     }

     /* Returns hard requirements that must be satisfied before the module starts. */

     virtual uint32_t GetRequirementFlags() const
     {
          return requirement_flags;
     }

     /* Returns the storage prefix reserved for this module. */

     std::string GetStoragePrefix() const;

     /* Builds one fully-qualified storage key for this module. */

     std::string MakeStorageKey(const std::string &Key) const;

     /* Stores one module-scoped value in the shared database. */

     bool SetStorageValue(const std::string &Key, const std::string &Value) const;

     /* Retrieves one module-scoped value from the shared database. */

     std::string GetStorageValue(const std::string &Key) const;

     /* Deletes one module-scoped value from the shared database. */

     bool DeleteStorageValue(const std::string &Key) const;

     /* Lists module-scoped keys relative to the module namespace. */

     std::vector<std::string> ListStorageKeys(const std::string &Pattern = "*") const;

     /* Deletes every stored key owned by this module. */

     size_t ClearStorage() const;

     /* Starts the module after configuration has been loaded. */

     virtual bool Start(const ServerConfig &Config, std::string &ErrorMessage)
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

     virtual void OnRequestAnalytics(const HttpRequest &, const HttpResponse &, RouteAction)
     {
     }

     /* Called after an authenticated request has completed so modules can inspect auth usage. */

     virtual void OnAuthenticatedRequest(const HttpRequest &, RouteAction)
     {

     }

     /* Called after a ping request has been handled successfully. */

     virtual void OnPingRequest(const HttpRequest &)
     {

     }

     /* Called after a database status request has been handled successfully. */

     virtual void OnDBRequest(const HttpRequest &)
     {

     }

     /* Called after a stats request has been handled successfully. */

     virtual void OnStatsRequest(const HttpRequest &)
     {

     }

     /* Called after a metrics request has been handled successfully. */

     virtual void OnMetricsRequest(const HttpRequest &)
     {
     }

     /* Called after a cache-inspection request has been handled successfully. */

     virtual void OnCacheRequest(const HttpRequest &)
     {
     }

     /* Search observation hooks begin here. */

     /* Called after a collection-level search has completed successfully. */

     virtual void OnSearchCollection(const SearchEvent &Event)
     {
          (void)Event;
     }

     /* Called after a document or vector search has completed successfully. */

     virtual void OnSearchDocument(const SearchEvent &Event)
     {
          (void)Event;
     }

     /* Returns a multiplier used to adjust one hit after retrieval and before final ranking. */

     virtual float ComputeSearchWeightMultiplier(const std::string &Collection,
                                                 const std::string &Query,
                                                 const std::string &RankingMode,
                                                 const SearchHit &,
                                                 float BaseScore)
     {
          (void)Collection;
          (void)Query;
          (void)RankingMode;
          (void)BaseScore;
          return 1.0f;
     }

     /* Pre-check policy hooks begin here. */

     /* Called before a collection is created. */

     virtual ModulePreCheckResult OnPreCreateCollection(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a collection has been updated. */

     virtual ModulePreCheckResult OnPreUpdateCollection(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a collection has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteCollection(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been added. */

     virtual ModulePreCheckResult OnPreAddDocument(const std::string &, const Document &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been updated. */

     virtual ModulePreCheckResult OnPreUpdateDocument(const std::string &, const Document &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before documents have been bulk imported. */

     virtual ModulePreCheckResult OnPreBulkImportDocuments(const std::string &, uint64_t, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a document has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteDocument(const std::string &, const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before multiple documents have been deleted. */

     virtual ModulePreCheckResult OnPreDeleteDocuments(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before update-by-query operations. */

     virtual ModulePreCheckResult OnPreUpdateByQuery(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before delete-by-query operations. */

     virtual ModulePreCheckResult OnPreDeleteByQuery(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an alias has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertAlias(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an alias has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteAlias(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a synonym group has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertSynonym(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a synonym group has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteSynonym(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before one or more stopwords have been added. */

     virtual ModulePreCheckResult OnPreCreateStopword(const std::string &, const std::vector<std::string> &, bool, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before one stopword has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteStopword(const std::string &, const std::string &, bool, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an override has been created or updated. */

     virtual ModulePreCheckResult OnPreUpsertOverride(const std::string &, const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an override has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteOverride(const std::string &, const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been created. */

     virtual ModulePreCheckResult OnPreCreateUser(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been updated. */

     virtual ModulePreCheckResult OnPreUpdateUser(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before a user has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteUser(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been created. */

     virtual ModulePreCheckResult OnPreCreateKey(const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been updated. */

     virtual ModulePreCheckResult OnPreUpdateKey(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before an API key has been deleted. */

     virtual ModulePreCheckResult OnPreDeleteKey(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before runtime link changes. */

     virtual ModulePreCheckResult OnPreLinksConnect(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before runtime links are disconnected. */

     virtual ModulePreCheckResult OnPreLinksDisconnect(const std::string &, const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before maintenance actions. */

     virtual ModulePreCheckResult OnPreFlush(const std::string &, const std::string &, bool)
     {
          return ModulePreCheckResult();
     }

     /* Called before repair actions. */

     virtual ModulePreCheckResult OnPreRepair(const std::string &, const std::string &, bool)
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

     virtual void OnCreateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a collection has been updated successfully. */

     virtual void OnUpdateCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a collection has been deleted successfully. */

     virtual void OnDeleteCollection(const std::string &Collection, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a document has been added successfully. */

     virtual void OnAddDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)DocumentID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a document has been updated successfully. */

     virtual void OnUpdateDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)DocumentID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a document has been deleted successfully. */

     virtual void OnDeleteDocument(const std::string &Collection, const std::string &DocumentID, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)DocumentID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after multiple documents have been deleted successfully. */

     virtual void OnDeleteDocuments(const std::string &Collection, uint64_t Count, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)Count;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a bulk document import has completed successfully. */

     virtual void OnBulkImportDocuments(const std::string &Collection, uint64_t ImportedCount, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)Collection;
          (void)ImportedCount;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after an update-by-query operation has completed successfully. */

     virtual void OnUpdateByQuery(const std::string &Collection,
                                  uint64_t UpdatedCount,
                                  const std::string &RequesterIP,
                                  const std::string &RequesterUser,
                                  bool Authenticated)
     {
          (void)Collection;
          (void)UpdatedCount;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a delete-by-query operation has completed successfully. */

     virtual void OnDeleteByQuery(const std::string &Collection,
                                  uint64_t DeletedCount,
                                  const std::string &RequesterIP,
                                  const std::string &RequesterUser,
                                  bool Authenticated)
     {
          (void)Collection;
          (void)DeletedCount;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a global synonym group has been created or updated successfully. */

     virtual void OnGlobalSynAdd(const std::string &SynonymID,
                                 const std::string &RequesterIP,
                                 const std::string &RequesterUser,
                                 bool Authenticated)
     {
          (void)SynonymID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a global synonym group has been deleted successfully. */

     virtual void OnGlobalSynDel(const std::string &SynonymID,
                                 const std::string &RequesterIP,
                                 const std::string &RequesterUser,
                                 bool Authenticated)
     {
          (void)SynonymID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a synonym group has been created or updated successfully. */

     virtual void OnUpsertSynonym(const std::string &Collection,
                                  const std::string &SynonymID,
                                  bool GlobalScope,
                                  const std::string &RequesterIP,
                                  const std::string &RequesterUser,
                                  bool Authenticated)
     {
          (void)Collection;
          (void)SynonymID;
          (void)GlobalScope;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a synonym group has been deleted successfully. */

     virtual void OnDeleteSynonym(const std::string &Collection,
                                  const std::string &SynonymID,
                                  bool GlobalScope,
                                  const std::string &RequesterIP,
                                  const std::string &RequesterUser,
                                  bool Authenticated)
     {
          (void)Collection;
          (void)SynonymID;
          (void)GlobalScope;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after one or more global stopwords have been added successfully. */

     virtual void OnGlobalStopwordAdd(uint64_t Count,
                                      const std::string &RequesterIP,
                                      const std::string &RequesterUser,
                                      bool Authenticated)
     {
          (void)Count;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after one or more stopwords have been added successfully. */

     virtual void OnCreateStopword(const std::string &Collection,
                                   uint64_t Count,
                                   bool GlobalScope,
                                   const std::string &RequesterIP,
                                   const std::string &RequesterUser,
                                   bool Authenticated)
     {
          (void)Collection;
          (void)Count;
          (void)GlobalScope;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a stopword has been deleted successfully. */

     virtual void OnDeleteStopword(const std::string &Collection,
                                   const std::string &Word,
                                   bool GlobalScope,
                                   const std::string &RequesterIP,
                                   const std::string &RequesterUser,
                                   bool Authenticated)
     {
          (void)Collection;
          (void)Word;
          (void)GlobalScope;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after an override has been created or updated successfully. */

     virtual void OnUpsertOverride(const std::string &Collection,
                                   const std::string &OverrideID,
                                   const std::string &RequesterIP,
                                   const std::string &RequesterUser,
                                   bool Authenticated)
     {
          (void)Collection;
          (void)OverrideID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after an override has been deleted successfully. */

     virtual void OnDeleteOverride(const std::string &Collection,
                                   const std::string &OverrideID,
                                   const std::string &RequesterIP,
                                   const std::string &RequesterUser,
                                   bool Authenticated)
     {
          (void)Collection;
          (void)OverrideID;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after an alias has been created or updated successfully. */

     virtual void OnUpsertAlias(const std::string &AliasName,
                                const std::string &TargetCollection,
                                const std::string &RequesterIP,
                                const std::string &RequesterUser,
                                bool Authenticated)
     {
          (void)AliasName;
          (void)TargetCollection;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after an alias has been deleted successfully. */

     virtual void OnDeleteAlias(const std::string &AliasName,
                                const std::string &RequesterIP,
                                const std::string &RequesterUser,
                                bool Authenticated)
     {
          (void)AliasName;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a full database flush has completed successfully. */

     virtual void OnFlush(uint64_t CollectionsDeleted, const std::string &RequesterIP, const std::string &RequesterUser, bool Authenticated)
     {
          (void)CollectionsDeleted;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a runtime cluster link has been added successfully. */

     virtual void OnLinksConnect(const std::string &Endpoint,
                                 const std::string &RequesterIP,
                                 const std::string &RequesterUser,
                                 bool Authenticated)
     {
          (void)Endpoint;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a runtime cluster link has been removed successfully. */

     virtual void OnLinksDisconnect(const std::string &Endpoint,
                                    const std::string &RequesterIP,
                                    const std::string &RequesterUser,
                                    bool Authenticated)
     {
          (void)Endpoint;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called after a repair action has completed successfully. */

     virtual void OnRepair(const std::string &RequesterIP,
                           const std::string &RequesterUser,
                           bool Authenticated)
     {
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called when the analytics click endpoint records a click event. */

     virtual void OnAnalyticsClick(const std::string &Collection,
                                   const std::string &Query,
                                   const std::string &DocumentID,
                                   signed int Rank,
                                   const std::string &RequesterIP,
                                   const std::string &RequesterUser,
                                   bool Authenticated)
     {
          (void)Collection;
          (void)Query;
          (void)DocumentID;
          (void)Rank;
          (void)RequesterIP;
          (void)RequesterUser;
          (void)Authenticated;
     }

     /* Called when a module produces a durable analytics snapshot. */

     virtual void OnSnapshot(const AnalyticsSnapshotEvent &Event)
     {
          (void)Event;
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

     virtual ModuleCommandResponse HandleCommand(const ModuleCommandRequest &)
     {
          ModuleCommandResponse Response;

          Response.StatusCode = 404;
          Response.Success = false;
          Response.Message = "Route not found.";
          Response.Body = "{\"error\":\"Route not found.\"}";

          return Response;
     }

     /* Handles a module HTTP API request. */

     virtual HttpResponse HandleAPIRequest(const HttpRequest &, const std::string &) const;
};

template <typename Derived>
class AutoRuntimeModule : public RuntimeModule
{
   protected:
     /* Constructs a module that automatically marks overridden hooks. */

     explicit AutoRuntimeModule(const std::string &Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
         : RuntimeModule(Name, EnableAPIRoute, RequirementFlags)
     {
          this->template AutoAttachHooks<Derived>();
     }
};

class CompositeRuntimeModule : public RuntimeModule
{
   private:
     std::vector<RuntimeModule *> Components;

   protected:
     /*
      * Registers an owned component for hook and lifecycle forwarding.
      * The composite does not take ownership; components should normally be
      * member fields declared before the parent constructor body registers them.
      */

     void AddComponent(RuntimeModule &Component);

   public:
     explicit CompositeRuntimeModule(const std::string &Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
         : RuntimeModule(Name, EnableAPIRoute, RequirementFlags)
     {
     }

     std::vector<RuntimeModule *> GetHookTargets(ModuleHook Hook) override;

     bool Start(const ServerConfig &Config, std::string &ErrorMessage) override;
     void Stop() override;
     void OnUnloadModule() override;
};

template <typename Derived>
class AutoCompositeRuntimeModule : public CompositeRuntimeModule
{
   protected:
     /* Constructs a composite module that automatically marks overridden hooks. */

     explicit AutoCompositeRuntimeModule(const std::string &Name, bool EnableAPIRoute = false, uint32_t RequirementFlags = ModuleRequirementNone)
         : CompositeRuntimeModule(Name, EnableAPIRoute, RequirementFlags)
     {
          this->template AutoAttachHooks<Derived>();
     }
};

/* Signature exported by shared modules for runtime construction. */

using CreateRuntimeModuleFn = RuntimeModule *(*)();

/* Exports the standard module factory symbol expected by the loader. */

#define MODULE_LOAD(ModuleType)                      \
     extern "C" RuntimeModule *CreateRuntimeModule() \
     {                                               \
          return new ModuleType();                   \
     }
