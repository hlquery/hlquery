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
#include <condition_variable>
#include <ctime>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/forwards.h"
#include "core/modules.h"

/* Calls a module manager dispatcher when modules are available. */

#define FOREACH_MOD(Method, ...)                                            \
     do                                                                     \
     {                                                                      \
          if (Instance && Instance->Modules)                                \
          {                                                                 \
               try                                                          \
               {                                                            \
                    Instance->Modules->NotifyModules(                       \
                         ModuleHook::Method,                                \
                         #Method,                                           \
                         std::function<void(RuntimeModule&)>(               \
                              [&](RuntimeModule& Module)                   \
                              {                                             \
                                   Module.Method(__VA_ARGS__);             \
                              }));                                         \
               }                                                            \
               catch (...)                                                  \
               {                                                            \
               }                                                            \
          }                                                                 \
     } while (0)

#define RUN_MODULE_PRECHECK(Method, ...)                                   \
     ((Instance && Instance->Modules)                                      \
          ? Instance->Modules->RunPreCheck(                                \
                 ModuleHook::Method,                                        \
                 #Method,                                                   \
                 [&](RuntimeModule& Module)                                 \
                 {                                                          \
                      return Module.Method(__VA_ARGS__);                    \
                 })                                                         \
          : ModulePreCheckResult())

/* 
 * Manages the lifecycle of runtime modules.
 * This class loads modules, tracks hook subscribers,
 * dispatches events, and coordinates module shutdown.
 */

class ModuleManager
{
  private:

    /* 
     * Tracks serialized callback execution for one loaded module instance.
     * This prevents concurrent hook entry into the same module and lets unload
     * wait for in-flight callbacks to drain before Stop() runs.
     */

    struct ModuleExecutionState
    {
         std::mutex Mutex;
         std::condition_variable Condition;
         unsigned int ActiveCallbacks = 0;
         bool DispatchInProgress = false;
         bool Stopping = false;
    };

    /* 
     * Pairs one runtime module instance with its execution state.
     * Snapshots carry both so dispatch remains safe after registry updates.
     */

    struct ModuleReference
    {
         std::shared_ptr<RuntimeModule> Instance;
         std::shared_ptr<ModuleExecutionState> ExecutionState;
    };

    /* 
     * Snapshot of currently referenced runtime modules.
     * Snapshots are used to iterate safely without holding write access.
     */

    using ModuleSnapshot = std::vector<ModuleReference>;

    /* 
     * Maps each hook identifier to the modules subscribed to that hook.
     * The array index matches the ModuleHook enumeration value.
     */

    using HookSubscribers = std::array<ModuleSnapshot, static_cast<size_t>(ModuleHook::OnCount)>;

     /* 
      * Stores one loaded module entry.
      * This keeps the module name, library handle, and runtime instance.
      */

     struct LoadedModule
     {
          /* Registered module name */

          std::string Name;

          /* Filesystem path used to load the shared library */

          std::string Path;

          /* Native shared-library handle */

          void* Handle = nullptr;

          /* Live runtime module instance */

          std::shared_ptr<RuntimeModule> Instance;

          /* Serialized callback state associated with the runtime instance */

          std::shared_ptr<ModuleExecutionState> ExecutionState;

          /* Tracks whether the unload hook has already been sent */

          bool UnloadNotified = false;
     };

     /* Currently active loaded modules */

     std::vector<LoadedModule> Modules;

     /* Modules waiting for final handle cleanup */

     std::vector<LoadedModule> RetiredModules;

     /* Cached subscriber lists grouped by hook */

     HookSubscribers SubscribersByHook;

    /* Protects the loaded module state and subscriber registries */

    mutable std::shared_mutex ModulesMutex;

    /* Protects demo mode state updates */

    mutable std::mutex DemoStateMutex;

    /* Indicates whether demo mode restrictions are active */

    bool DemoModeActive = false;

    /* User-visible reason describing the current demo mode state */

    std::string DemoModeMessage;

    /* 
     * Releases retired modules whose handles can now be cleaned up.
     * This finalizes deferred unload work after the active lists are updated.
     */

    void ReapRetiredModules(LogManager* Logger);

    /* 
     * Unloads a specific list of modules using the provided logger.
     * The supplied list is detached from the active registry before cleanup.
     */

    void UnloadModuleList(std::vector<LoadedModule> ModulesToUnload, LogManager* Logger);

     /* 
      * Captures the subscribers registered for one specific hook.
      * The returned snapshot can be iterated without holding the write lock.
      */

     ModuleSnapshot GetHookSnapshot(ModuleHook Hook) const;

     /* 
      * Rebuilds all cached hook subscriber lists from the loaded modules.
      * This must be called after module load or unload state changes.
      */

     void RebuildHookRegistriesLocked();

     /* 
      * Dispatches an event callback to all currently loaded modules.
      * The manager resolves the current subscriber snapshot before dispatch.
      */

     void DispatchEvent(ModuleHook Hook, const char* EventName, const std::function<void(RuntimeModule&)> &Invoke);

     /*
      *  Dispatches an event callback to the modules contained in a snapshot.
      * Snapshot dispatch isolates iteration from concurrent registry updates.
      */

     static void DispatchModuleEvent(const ModuleSnapshot& Modules,
                                     const char* EventName,
                                     const std::function<void(RuntimeModule&)> &Invoke);

     /* 
      * Attempts to enter one module callback region.
      * Returns false when the module is stopping and should no longer accept work.
      */

     static bool BeginModuleCallback(const ModuleReference& Module);

     /* 
      * Leaves one module callback region and wakes unload waiters if needed.
      */

     static void EndModuleCallback(const ModuleReference& Module);

     /* 
      * Prevents new callbacks from starting and waits for in-flight work to finish.
      */

     static void QuiesceModuleCallbacks(const ModuleReference& Module);

     /* 
      * Finds one loaded module and returns both the instance and execution state.
      */

     bool GetModuleReference(const std::string& Name, ModuleReference* Module) const;

     /*
      * Runs a pre-check callback for each module in a snapshot.
      * The first deny result is returned immediately.
      */

     template <typename Callback>
     static ModulePreCheckResult DispatchPreCheckEvent(const ModuleSnapshot& Modules,
                                                       const char* EventName,
                                                       Callback&& Invoke)
     {
          for (const auto& Module : Modules)
          {
               if (!BeginModuleCallback(Module))
               {
                    continue;
               }

               try
               {
                    ModulePreCheckResult Result = Invoke(*Module.Instance);

                    EndModuleCallback(Module);

                    if (Result.Action == ModulePreCheckAction::Deny)
                    {
                         return Result;
                    }
               }
               catch (const std::exception& Ex)
               {
                    EndModuleCallback(Module);
                    const std::string ModuleName = Module.Instance ? Module.Instance->GetName() : "unknown";
                    std::cerr << "Module '" << ModuleName << "' threw during " << EventName << ": " << Ex.what() << std::endl;
               }
               catch (...)
               {
                    EndModuleCallback(Module);
                    const std::string ModuleName = Module.Instance ? Module.Instance->GetName() : "unknown";
                    std::cerr << "Module '" << ModuleName << "' threw during " << EventName << ": unknown exception" << std::endl;
               }
          }

          return ModulePreCheckResult();
     }

     /* 
      * Resolves the filesystem path for a module entry from the server configuration.
      * This converts configuration data into the concrete path used for loading.
      */

     std::string ResolveModulePath(const ServerConfig& Config, const ServerConfig::ModuleLoadEntry& ModuleEntry) const;

   public:

     /*
      * Validates the logical module name used for config entries and runtime
      * load/unload requests. Paths belong in ModuleLoadEntry::Path, not Name.
      */

     static bool IsValidModuleName(const std::string& Name);

     /* 
      * Destroys the module manager and releases any remaining resources.
      * Remaining modules are unloaded before process shutdown completes.
      */

     ~ModuleManager();

     /* 
      * Waits for storage readiness and then loads all configured runtime modules.
      * This is the top-level entry point used during server startup.
      */

     bool LoadModules(const ServerConfig& Config, LogManager* Logger, std::string& ErrorMessage);

     /* Loads one runtime module into the active registry without replacing the rest. */

     bool LoadModule(const ServerConfig& Config,
                     const std::string& ModuleName,
                     LogManager* Logger,
                     std::string& ErrorMessage,
                     const std::string& ExplicitPath = "");

     /* 
      * Loads all configured modules and reports any failure details.
      * This performs the actual configuration-driven module load pass.
      */

     bool LoadConfiguredModules(const ServerConfig& Config, LogManager* Logger, std::string& ErrorMessage);

     /* 
      * Unloads every currently loaded module.
      * Active registries are cleared before native handles are retired.
      */

     void UnloadAll(LogManager* Logger);

     /* 
      * Finds a loaded module by name.
      * Returns a shared pointer to the live runtime instance when found.
      */

     std::shared_ptr<RuntimeModule> Find(const std::string& Name) const;

     /* Returns the names of all loaded modules.
      * The result includes both core and optional modules that are active.
      */

     std::vector<std::string> GetLoadedModuleNames() const;

     /* 
      * Returns the names of loaded core modules.
      * Only modules marked as core are included in the returned list.
      */

     std::vector<std::string> GetLoadedCoreModuleNames() const;

     /* 
      * Returns the names of loaded optional modules.
      * This excludes modules that are part of the required core set.
      */

     std::vector<std::string> GetLoadedOptionalModuleNames() const;

     /* 
      * Returns the API descriptions exposed by loaded modules.
      * Each description is gathered from modules that publish API metadata.
      */

     std::vector<ModuleAPIDescription> GetModuleAPIDescriptions() const;

     /* 
      * Retrieves the API description for a single module.
      * Returns false when the module is missing or exposes no API metadata.
      */

     bool GetModuleAPIDescription(const std::string& ModuleName, ModuleAPIDescription* Description) const;

     /* 
      * Retrieves the command specifications exposed by a module.
      * The output vector is filled only for modules that publish commands.
      */

     bool GetModuleCommandSpecs(const std::string& ModuleName, std::vector<ModuleCommandSpec>* Commands) const;

     /* 
      * Handles a module command request for the specified module.
      * The request is routed to the matching module implementation.
      */

     bool HandleModuleCommand(const std::string& ModuleName, const ModuleCommandRequest& Request, ModuleCommandResponse* Response) const;

     /* 
      * Routes an HTTP API request to the specified module.
      * The sub-path is forwarded so the target module can resolve routing.
      */

     HttpResponse HandleModuleAPIRequest(const std::string& ModuleName, const HttpRequest& Request, const std::string& SubPath) const;

     /* 
      * Notifies modules that they are about to be unloaded.
      * This gives modules a chance to release resources before final unload.
      */

     void OnUnloadModules();

     /* Unloads one named runtime module from the active registry. */

     bool UnloadModule(const std::string& ModuleName, LogManager* Logger, std::string& ErrorMessage);

     /*  
      * Computes a module-adjusted multiplier for a search hit score.
      * Loaded modules may raise or lower the base score contribution.
      */

     float ComputeSearchWeightMultiplier(const std::string& Collection,
                                         const std::string& Query,
                                         const std::string& RankingMode,
                                         const SearchHit& Hit,
                                         float BaseScore) const;

    /* 
     * Returns whether demo mode is currently enabled.
     * This state is shared across module-managed demo restrictions.
     */

    bool IsDemoModeEnabled() const;

    /* 
     * Returns the current demo mode message.
     * The message explains why demo mode is active or what it affects.
     */

    std::string GetDemoModeMessage() const;

    /* 
     * Updates the current demo mode state and message.
     * Modules can use this to publish centralized demo mode restrictions.
     */

    void SetDemoModeState(bool Active, const std::string& Message);

     /* 
      * Runs a callback across the currently loaded modules.
      * The callback is dispatched to the subscribers for the requested hook.
      */

     void NotifyModules(ModuleHook Hook, const char* EventName, const std::function<void(RuntimeModule&)>& Invoke);

     /* 
      * Runs a pre-check callback across the subscribers registered for one hook.
      * The first module that denies the action stops the evaluation.
      */

     template <typename Callback>
     ModulePreCheckResult RunPreCheck(ModuleHook Hook, const char* EventName, Callback&& Invoke) const
     {
          return DispatchPreCheckEvent(GetHookSnapshot(Hook), EventName, std::forward<Callback>(Invoke));
     }

};
