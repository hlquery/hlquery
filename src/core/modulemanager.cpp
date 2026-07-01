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
#include <cctype>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>

#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "core/logmanager.h"
#include "runtime/serverconfig.h"
#include "core/modules.h"
#include "api/searchapi.h"

/* Records one module hook failure without assuming logging is always initialized. */

static void LogModuleDispatchFailure(const RuntimeModule *Module, const char *EventName, const std::string &ErrorMessage)
{
     const std::string ModuleName = Module ? Module->GetName() : "unknown";
     const std::string Message = "Module '" + ModuleName + "' threw during " + EventName + ": " + ErrorMessage;

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Critical("modules", Message);
          return;
     }

     std::cerr << Message << std::endl;
}

/* Fallback wrapper used when a module throws a non-standard exception type. */

static void LogUnknownModuleDispatchFailure(const RuntimeModule *Module, const char *EventName)
{
     LogModuleDispatchFailure(Module, EventName, "unknown exception");
}

void ModuleManager::LogDispatchFailure(const RuntimeModule *Module, const char *EventName, const std::string &ErrorMessage)
{
     LogModuleDispatchFailure(Module, EventName, ErrorMessage);
}

void ModuleManager::LogUnknownDispatchFailure(const RuntimeModule *Module, const char *EventName)
{
     LogUnknownModuleDispatchFailure(Module, EventName);
}

/* Appends one candidate path only once after normalizing path syntax. */

static void PushUniquePath(std::vector<std::filesystem::path> &Paths, const std::filesystem::path &PathValue)
{
     if (PathValue.empty())
     {
          return;
     }

     const std::filesystem::path NormalizedPath = PathValue.lexically_normal();

     if (std::find(Paths.begin(), Paths.end(), NormalizedPath) == Paths.end())
     {
          Paths.push_back(NormalizedPath);
     }
}

/* Normalizes one path and resolves it against the current process directory when needed. */

static std::filesystem::path MakeAbsolutePath(std::filesystem::path PathValue)
{
     std::error_code EC;

     if (!PathValue.empty() && !PathValue.is_absolute())
     {
          PathValue = std::filesystem::absolute(PathValue, EC);
     }

     return PathValue;
}

/* Resolves one relative module path from the active configuration file directory. */

static std::filesystem::path ResolveRelativeToConfig(const ServerConfig &Config, const std::filesystem::path &RelativePath)
{
     const std::filesystem::path ConfigDir = MakeAbsolutePath(std::filesystem::path(Config.GetConfigFile())).parent_path();

     if (ConfigDir.empty())
     {
          return RelativePath.lexically_normal();
     }

     return (ConfigDir / RelativePath).lexically_normal();
}

static bool ModuleRuntimeNameMatchesRequest(const RuntimeModule &Module, const std::string &RequestedName)
{
     return Module.GetName() == RequestedName;
}

/* Stores the process-wide demo mode status derived from loaded modules. */

void ModuleManager::SetDemoModeState(bool Active, const std::string &Message)
{
     std::lock_guard<std::mutex> Lock(DemoStateMutex);

     bool &ActiveState = DemoModeStaging ? StagedDemoModeActive : DemoModeActive;
     std::string &MessageState = DemoModeStaging ? StagedDemoModeMessage : DemoModeMessage;

     ActiveState = Active;

     if (Active)
     {
          MessageState = Message;
     }
     else
     {
          MessageState.clear();
     }
}

/* Ensures unload notifications and handle teardown happen during destruction. */

ModuleManager::~ModuleManager()
{
     OnUnloadModules();
     UnloadAll();
}

bool ModuleManager::IsValidModuleName(const std::string &Name)
{
     if (Name.empty())
     {
          return false;
     }

     for (unsigned char Char : Name)
     {
          if (std::isalnum(Char) || Char == '_' || Char == '-')
          {
               continue;
          }

          return false;
     }

     return true;
}

/* Waits for shared storage before starting any runtime module. */

bool ModuleManager::LoadModules(const ServerConfig &Config, std::string &ErrorMessage)
{
     const auto Deadline = Now() + std::chrono::seconds(5);

     while (!HybridStorageManagerInstance().IsInitialized())
     {
          if (Now() >= Deadline)
          {
               ErrorMessage = "Hybrid storage manager did not finish initializing before modules were loaded.";
               return false;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(100));
     }

     return LoadConfiguredModules(Config, ErrorMessage);
}

bool ModuleManager::LoadModule(const ServerConfig &Config,
                               const std::string &ModuleName,
                               std::string &ErrorMessage,
                               const std::string &ExplicitPath)
{
     if (ModuleName.empty())
     {
          ErrorMessage = "Module name is required.";
          return false;
     }

     if (!IsValidModuleName(ModuleName))
     {
          ErrorMessage = "Module name '" + ModuleName + "' contains unsupported characters.";
          return false;
     }

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);
          ReapRetiredModules();

          for (const auto &Loaded : Modules)
          {
               if (Loaded.Name == ModuleName && Loaded.Instance)
               {
                    ErrorMessage = "Module '" + ModuleName + "' is already loaded.";
                    return false;
               }
          }
     }

     ServerConfig::ModuleLoadEntry ModuleEntry;
     ModuleEntry.Name = ModuleName;
     ModuleEntry.Path = ExplicitPath;

     const std::string ModulePath = ResolveModulePath(Config, ModuleEntry);
     const bool IsCoreModule = ModuleName.rfind("core_", 0) == 0;

     std::error_code EC;

     if (!std::filesystem::exists(ModulePath, EC))
     {
          if (EC)
          {
               ErrorMessage = "Configured module '" + ModuleName + "' could not be inspected: " + ModulePath + " (" + EC.message() + ")";
          }
          else
          {
               ErrorMessage = "Configured module '" + ModuleName + "' could not be found: " + ModulePath;
          }

          return false;
     }

     if (Instance && Instance->Logs)
     {
          const std::string ModuleType = IsCoreModule ? "core module" : "module";
          Instance->Logs->Normal("modules", "Loading " + ModuleType + " '" + ModuleName + "' from " + ModulePath + ".");
     }

     void *Handle = dlopen(ModulePath.c_str(), RTLD_NOW | RTLD_LOCAL);

     if (!Handle)
     {
          ErrorMessage = "Failed to load module '" + ModuleName + "' from " + ModulePath + ": " + dlerror();
          return false;
     }

     dlerror();

     auto *CreateFn = reinterpret_cast<CreateRuntimeModuleFn>(dlsym(Handle, "CreateRuntimeModule"));
     const char *SymbolError = dlerror();

     if (SymbolError)
     {
          dlclose(Handle);
          ErrorMessage = "Module '" + ModuleName + "' is missing CreateRuntimeModule(): " + std::string(SymbolError);
          return false;
     }

     std::shared_ptr<RuntimeModule> Module;

     try
     {
          Module.reset(CreateFn());
     }
     catch (const std::exception &Ex)
     {
          dlclose(Handle);
          ErrorMessage = "Module '" + ModuleName + "' threw during creation: " + std::string(Ex.what());
          return false;
     }
     catch (...)
     {
          dlclose(Handle);
          ErrorMessage = "Module '" + ModuleName + "' threw during creation: unknown exception.";
          return false;
     }

     if (!Module)
     {
          dlclose(Handle);
          ErrorMessage = "Module '" + ModuleName + "' returned a null module instance.";
          return false;
     }

     if (!ModuleRuntimeNameMatchesRequest(*Module, ModuleName))
     {
          const std::string ActualName = Module->GetName();
          Module.reset();
          dlclose(Handle);
          ErrorMessage = "Module '" + ModuleName + "' created runtime module '" + ActualName + "'.";
          return false;
     }

     std::string StartError;
     bool Started = false;

     try
     {
          Started = Module->Start(Config, StartError);
     }
     catch (const std::exception &Ex)
     {
          StartError = "Module '" + ModuleName + "' threw during start: " + std::string(Ex.what());
          Started = false;
     }
     catch (...)
     {
          StartError = "Module '" + ModuleName + "' threw during start: unknown exception.";
          Started = false;
     }

     if (!Started)
     {
          try
          {
               Module->Stop();
          }
          catch (...)
          {

          }

          Module.reset();
          dlclose(Handle);
          ErrorMessage = StartError.empty() ? "Module '" + ModuleName + "' failed to start." : StartError;
          return false;
     }

     LoadedModule Loaded;
     Loaded.Name = ModuleName;
     Loaded.Path = ModulePath;
     Loaded.Handle = Handle;
     Loaded.Instance = std::move(Module);
     Loaded.ExecutionState = std::make_shared<ModuleExecutionState>();
     Loaded.UnloadNotified = false;

     std::unique_lock<std::shared_mutex> Lock(ModulesMutex);

     for (const auto &Existing : Modules)
     {
          if (Existing.Name == ModuleName && Existing.Instance)
          {
               Lock.unlock();

               try
               {
                    Loaded.Instance->Stop();
               }
               catch (...)
               {

               }

               Loaded.Instance.reset();
               dlclose(Loaded.Handle);
               ErrorMessage = "Module '" + ModuleName + "' is already loaded.";
               return false;
          }
     }

     Modules.push_back(std::move(Loaded));
     RebuildHookRegistriesLocked();
     Lock.unlock();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("modules", "Loaded module '" + ModuleName + "' from " + ModulePath + ".");
     }

     return true;
}

/* Finalizes retired modules once no external shared_ptr references remain. */

void ModuleManager::ReapRetiredModules()
{
     auto It = RetiredModules.begin();

     while (It != RetiredModules.end())
     {
          if (It->Instance && It->Instance.use_count() > 1)
          {
               ++It;
               continue;
          }

          if (It->Instance)
          {
               It->Instance.reset();
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("modules", "Finalized retired module '" + It->Name + "'.");
          }

          if (It->Handle)
          {
               dlclose(It->Handle);
               It->Handle = nullptr;
          }

          It = RetiredModules.erase(It);
     }
}

/* Builds a stable copy of hook subscribers so callbacks can run without holding the registry lock. */

ModuleManager::ModuleSnapshot ModuleManager::GetHookSnapshot(ModuleHook Hook) const
{
     std::shared_lock<std::shared_mutex> Lock(ModulesMutex);
     return SubscribersByHook[static_cast<size_t>(Hook)];
}

/* Enters one serialized callback region for a module unless shutdown has started. */

bool ModuleManager::BeginModuleCallback(const ModuleReference &Module)
{
     if (!Module.Instance || !Module.ExecutionState)
     {
          return false;
     }

     std::unique_lock<std::mutex> Lock(Module.ExecutionState->Mutex);

     while (Module.ExecutionState->DispatchInProgress && !Module.ExecutionState->Stopping)
     {
          Module.ExecutionState->Condition.wait(Lock);
     }

     if (Module.ExecutionState->Stopping)
     {
          return false;
     }

     Module.ExecutionState->DispatchInProgress = true;
     ++Module.ExecutionState->ActiveCallbacks;
     return true;
}

/* Leaves one serialized callback region and wakes threads blocked on shutdown or entry. */

void ModuleManager::EndModuleCallback(const ModuleReference &Module)
{
     if (!Module.ExecutionState)
     {
          return;
     }

     std::lock_guard<std::mutex> Lock(Module.ExecutionState->Mutex);

     if (Module.ExecutionState->ActiveCallbacks > 0)
     {
          --Module.ExecutionState->ActiveCallbacks;
     }

     Module.ExecutionState->DispatchInProgress = false;
     Module.ExecutionState->Condition.notify_all();
}

/* Blocks new callback entry and waits until all in-flight work for the module has drained. */

void ModuleManager::QuiesceModuleCallbacks(const ModuleReference &Module)
{
     if (!Module.ExecutionState)
     {
          return;
     }

     std::unique_lock<std::mutex> Lock(Module.ExecutionState->Mutex);
     Module.ExecutionState->Stopping = true;

     while (Module.ExecutionState->DispatchInProgress || Module.ExecutionState->ActiveCallbacks > 0)
     {
          Module.ExecutionState->Condition.wait(Lock);
     }
}

/* Recomputes the per-hook subscriber lists from the currently loaded modules. */

void ModuleManager::RebuildHookRegistriesLocked()
{
     for (auto &Subscribers : SubscribersByHook)
     {
          Subscribers.clear();
     }

     for (const auto &Loaded : Modules)
     {
          if (!Loaded.Instance)
          {
               continue;
          }

          for (size_t HookIndex = 0; HookIndex < static_cast<size_t>(ModuleHook::OnCount); ++HookIndex)
          {
               const ModuleHook Hook = static_cast<ModuleHook>(HookIndex);

               if (Loaded.Instance->HandlesHook(Hook))
               {
                    SubscribersByHook[HookIndex].push_back({Loaded.Instance, Loaded.ExecutionState});
               }
          }
     }
}

/* Dispatches one hook by first capturing a lock-free snapshot of subscribers. */

void ModuleManager::DispatchEvent(ModuleHook Hook, const char *EventName, const std::function<void(RuntimeModule &)> &Invoke)
{
     DispatchModuleEvent(GetHookSnapshot(Hook), EventName, Invoke);
}

/* Invokes one callback across a pre-captured module list with exception isolation. */

void ModuleManager::DispatchModuleEvent(const ModuleSnapshot &Modules,
                                        const char *EventName,
                                        const std::function<void(RuntimeModule &)> &Invoke)
{
     for (const auto &Module : Modules)
     {
          if (!BeginModuleCallback(Module))
          {
               continue;
          }

          try
          {
               Invoke(*Module.Instance);
               EndModuleCallback(Module);
          }
          catch (const std::exception &Ex)
          {
               EndModuleCallback(Module);
               LogModuleDispatchFailure(Module.Instance.get(), EventName, Ex.what());
          }
          catch (...)
          {
               EndModuleCallback(Module);
               LogUnknownModuleDispatchFailure(Module.Instance.get(), EventName);
          }
     }
}

/* Convenience wrapper used by the many simple notification methods in the class. */

void ModuleManager::NotifyModules(ModuleHook Hook, const char *EventName, const std::function<void(RuntimeModule &)> &Invoke)
{
     DispatchEvent(Hook, EventName, Invoke);
}

/* Resolves the on-disk shared library path for one configured module entry. */

std::string ModuleManager::ResolveModulePath(const ServerConfig &Config, const ServerConfig::ModuleLoadEntry &ModuleEntry) const
{
     if (!ModuleEntry.Path.empty())
     {
          std::filesystem::path ExplicitPath(ModuleEntry.Path);
          const std::filesystem::path CompileTimeModuleDir = MakeAbsolutePath(std::filesystem::path(HLQUERY_MODULE_DIR));

          if (ExplicitPath.is_absolute())
          {
               return ExplicitPath.string();
          }

          std::error_code EC;
          const std::filesystem::path ModuleDirResolvedPath = (CompileTimeModuleDir / ExplicitPath).lexically_normal();

          if (!CompileTimeModuleDir.empty() && std::filesystem::exists(ModuleDirResolvedPath, EC))
          {
               return ModuleDirResolvedPath.string();
          }

          return ResolveRelativeToConfig(Config, ExplicitPath).string();
     }

     const std::string &ModuleName = ModuleEntry.Name;
     std::vector<std::string> FileNames;

     if (ModuleName.rfind("core_", 0) == 0)
     {
          FileNames.push_back(ModuleName + ".so");
          FileNames.push_back("m_" + ModuleName + ".so");
     }
     else
     {
          FileNames.push_back("m_" + ModuleName + ".so");
          FileNames.push_back(ModuleName + ".so");
     }

     std::vector<std::filesystem::path> Candidates;
     const std::filesystem::path CompileTimeModuleDir = MakeAbsolutePath(std::filesystem::path(HLQUERY_MODULE_DIR));

     for (const auto &FileName : FileNames)
     {
          if (!CompileTimeModuleDir.empty())
          {
               PushUniquePath(Candidates, CompileTimeModuleDir / FileName);
          }
     }

     std::error_code EC;

     /* The first existing candidate wins; otherwise return the preferred default location for the error path. */

     for (const auto &Candidate : Candidates)
     {
          if (std::filesystem::exists(Candidate, EC))
          {
               return Candidate.string();
       }
     }

     return Candidates.front().string();
}

/* Loads the configured module set atomically so partial reloads do not leak into the active registry. */

bool ModuleManager::LoadConfiguredModules(const ServerConfig &Config, std::string &ErrorMessage)
{
     std::vector<LoadedModule> StagedModules;

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);
          ReapRetiredModules();
     }

     {
          std::lock_guard<std::mutex> DemoLock(DemoStateMutex);
          DemoModeStaging = true;
          StagedDemoModeActive = false;
          StagedDemoModeMessage.clear();
     }

     /* Any failure during staging rolls back only the newly opened handles. */

     auto RollbackStagedModules = [&]()
     {
          for (auto It = StagedModules.rbegin(); It != StagedModules.rend(); ++It)
          {
               if (It->Instance)
               {
                    try
                    {
                         It->Instance->Stop();
                    }
                    catch (...)
                    {
             
                    }

                    It->Instance.reset();
               }

               if (It->Handle)
               {
                    dlclose(It->Handle);
                    It->Handle = nullptr;
               }
          }

          StagedModules.clear();

          std::lock_guard<std::mutex> DemoLock(DemoStateMutex);
          DemoModeStaging = false;
          StagedDemoModeActive = false;
          StagedDemoModeMessage.clear();
     };

     /* Modules are created, started, and validated off to the side before replacing the live registry. */

     for (const auto &ModuleEntry : Config.GetModuleLoads())
     {
          const std::string &ModuleName = ModuleEntry.Name;
          std::string ModulePath = ResolveModulePath(Config, ModuleEntry);
          const bool IsCoreModule = ModuleName.rfind("core_", 0) == 0;

          if (!IsValidModuleName(ModuleName))
          {
               ErrorMessage = "Configured module '" + ModuleName + "' contains unsupported characters.";
               RollbackStagedModules();
               return false;
          }

          const auto ExistingIt = std::find_if(StagedModules.begin(), StagedModules.end(),
                                               [&](const LoadedModule &Existing)
                                               {
                                                    return Existing.Name == ModuleName;
                                               });

          if (ExistingIt != StagedModules.end())
          {
               ErrorMessage = "Configured module '" + ModuleName + "' is listed more than once.";
               RollbackStagedModules();
               return false;
          }

          std::error_code EC;

          if (!std::filesystem::exists(ModulePath, EC))
          {
               if (EC)
               {
                    ErrorMessage = "Configured module '" + ModuleName + "' could not be inspected: " + ModulePath + " (" + EC.message() + ")";
               }
               else
               {
                    ErrorMessage = "Configured module '" + ModuleName + "' could not be found: " + ModulePath;
               }

               RollbackStagedModules();
               return false;
          }

          void *Handle = dlopen(ModulePath.c_str(), RTLD_NOW | RTLD_LOCAL);

          if (Instance && Instance->Logs)
          {
               const std::string module_type = (IsCoreModule ? "core module" : "module");
               Instance->Logs->Normal("modules", "Loading " + module_type + " '" + ModuleName + "' from " + ModulePath + ".");
          }

          if (!Handle)
          {
               ErrorMessage = "Failed to load module '" + ModuleName + "' from " + ModulePath + ": " + dlerror();
               RollbackStagedModules();
               return false;
          }

          dlerror();

          auto *CreateFn = reinterpret_cast<CreateRuntimeModuleFn>(dlsym(Handle, "CreateRuntimeModule"));
          const char *SymbolError = dlerror();

          if (SymbolError)
          {
               dlclose(Handle);
               ErrorMessage = "Module '" + ModuleName + "' is missing CreateRuntimeModule(): " + std::string(SymbolError);
               RollbackStagedModules();
               return false;
          }

          std::shared_ptr<RuntimeModule> Module;

          try
          {
               Module.reset(CreateFn());
          }
          catch (const std::exception &Ex)
          {
               dlclose(Handle);
               ErrorMessage = "Module '" + ModuleName + "' threw during creation: " + std::string(Ex.what());
               RollbackStagedModules();
               return false;
          }
          catch (...)
          {
               dlclose(Handle);
               ErrorMessage = "Module '" + ModuleName + "' threw during creation: unknown exception.";
               RollbackStagedModules();
               return false;
          }

          if (!Module)
          {
               dlclose(Handle);
               ErrorMessage = "Module '" + ModuleName + "' returned a null module instance.";
               RollbackStagedModules();
               return false;
          }

          if (!ModuleRuntimeNameMatchesRequest(*Module, ModuleName))
          {
               const std::string ActualName = Module->GetName();
               Module.reset();
               dlclose(Handle);
               ErrorMessage = "Configured module '" + ModuleName + "' created runtime module '" + ActualName + "'.";
               RollbackStagedModules();
               return false;
          }

          std::string StartError;
          bool Started = false;

          try
          {
               Started = Module->Start(Config, StartError);
          }
          catch (const std::exception &Ex)
          {
               StartError = "Module '" + ModuleName + "' threw during start: " + std::string(Ex.what());
               Started = false;
          }
          catch (...)
          {
               StartError = "Module '" + ModuleName + "' threw during start: unknown exception.";
               Started = false;
          }

         if (!Started)
         {
               if (Instance && Instance->Logs)
               {
                    const std::string StartMessage = StartError.empty() ? "unknown failure" : StartError;
                    Instance->Logs->Normal("modules", "Module '" + ModuleName + "' Start() failed: " + StartMessage);
               }
               try
               {
                    Module->Stop();
               }
               catch (...)
               {

               }

               Module.reset();

               dlclose(Handle);
               ErrorMessage = StartError.empty() ? "Module '" + ModuleName + "' failed to start." : StartError;
               RollbackStagedModules();
               return false;
         }

         if (Instance && Instance->Logs)
         {
              Instance->Logs->Normal("modules", "Loaded module '" + ModuleName + "' from " + ModulePath + ".");
         }

         LoadedModule Loaded;
         Loaded.Name = ModuleName;
         Loaded.Path = ModulePath;
         Loaded.Handle = Handle;
         Loaded.Instance = std::move(Module);
         Loaded.ExecutionState = std::make_shared<ModuleExecutionState>();
         Loaded.UnloadNotified = false;

          StagedModules.push_back(std::move(Loaded));
     }

     /* Swap in the new module set under lock, then tear down the previous set after releasing the registry lock. */

     std::vector<LoadedModule> PreviousModules;

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);
          PreviousModules = std::move(Modules);
          Modules = std::move(StagedModules);
          RebuildHookRegistriesLocked();

          std::lock_guard<std::mutex> DemoLock(DemoStateMutex);
          DemoModeActive = StagedDemoModeActive;
          DemoModeMessage = StagedDemoModeActive ? StagedDemoModeMessage : "";
          DemoModeStaging = false;
          StagedDemoModeActive = false;
          StagedDemoModeMessage.clear();
     }

     UnloadModuleList(std::move(PreviousModules));

     return true;
}

/* Multiplies together all valid module-provided ranking weights and ignores invalid values. */

float ModuleManager::ComputeSearchWeightMultiplier(const std::string &Collection,
                                                   const std::string &Query,
                                                   const std::string &RankingMode,
                                                   const SearchHit &Hit,
                                                   float BaseScore) const
{
     float combined_multiplier = 1.0f;
     ModuleSnapshot Snapshot = GetHookSnapshot(ModuleHook::ComputeSearchWeightMultiplier);

     for (const auto &Module : Snapshot)
     {
          if (!BeginModuleCallback(Module))
          {
               continue;
          }

          try
          {
               const float module_multiplier = Module.Instance->ComputeSearchWeightMultiplier(Collection, Query, RankingMode, Hit, BaseScore);
               EndModuleCallback(Module);

               if (std::isfinite(module_multiplier) && module_multiplier > 0.0f)
               {
                    combined_multiplier *= module_multiplier;
               }
          }
          catch (const std::exception &Ex)
          {
               EndModuleCallback(Module);
               LogModuleDispatchFailure(Module.Instance.get(), "ComputeSearchWeightMultiplier", Ex.what());
          }
          catch (...)
          {
               EndModuleCallback(Module);
               LogUnknownModuleDispatchFailure(Module.Instance.get(), "ComputeSearchWeightMultiplier");
          }
     }

     if (!std::isfinite(combined_multiplier) || combined_multiplier <= 0.0f)
     {
          return 1.0f;
     }

     return combined_multiplier;
}

/* Sends the one-time unload notification to live modules before teardown begins. */

void ModuleManager::OnUnloadModules()
{
     ModuleSnapshot ModulesToNotify;

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);

          ModulesToNotify.reserve(Modules.size());

          for (auto &Loaded : Modules)
          {
               if (!Loaded.Instance || Loaded.UnloadNotified)
               {
                    continue;
               }

               Loaded.UnloadNotified = true;
               ModulesToNotify.push_back({Loaded.Instance, Loaded.ExecutionState});
          }
     }

     for (const auto &Module : ModulesToNotify)
     {
          QuiesceModuleCallbacks(Module);

          try
          {
               Module.Instance->OnUnloadModule();
          }
          catch (const std::exception &Ex)
          {
               LogModuleDispatchFailure(Module.Instance.get(), "OnUnloadModule", Ex.what());
          }
          catch (...)
          {
               LogUnknownModuleDispatchFailure(Module.Instance.get(), "OnUnloadModule");
          }
     }
}

/* Removes the active module set and rebuilds empty hook registries before teardown. */

void ModuleManager::UnloadAll()
{
     std::vector<LoadedModule> ModulesToUnload;

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);
          ModulesToUnload = std::move(Modules);
          Modules.clear();
          RebuildHookRegistriesLocked();
          ReapRetiredModules();
     }

     UnloadModuleList(std::move(ModulesToUnload));
}

bool ModuleManager::UnloadModule(const std::string &ModuleName, std::string &ErrorMessage)
{
     if (ModuleName.empty())
     {
          ErrorMessage = "Module name is required.";
          return false;
     }

     std::vector<LoadedModule> ModulesToUnload;

     {
          std::unique_lock<std::shared_mutex> Lock(ModulesMutex);

          auto It = std::find_if(Modules.begin(), Modules.end(), [&](const LoadedModule &Loaded)
          {
                return Loaded.Name == ModuleName;
          });

          if (It == Modules.end())
          {
               ErrorMessage = "Module '" + ModuleName + "' is not loaded.";
               return false;
          }

          ModulesToUnload.push_back(std::move(*It));
          Modules.erase(It);
          RebuildHookRegistriesLocked();
          ReapRetiredModules();
     }

     UnloadModuleList(std::move(ModulesToUnload));
     return true;
}

/* Stops modules in reverse load order and defers dlclose() when external references are still alive. */

void ModuleManager::UnloadModuleList(std::vector<LoadedModule> ModulesToUnload)
{
     for (auto It = ModulesToUnload.rbegin(); It != ModulesToUnload.rend(); ++It)
     {
          ModuleReference ModuleRef{It->Instance, It->ExecutionState};
          QuiesceModuleCallbacks(ModuleRef);
          ModuleRef.Instance.reset();
          ModuleRef.ExecutionState.reset();

          if (It->Instance && !It->UnloadNotified)
          {
               try
               {
                    It->Instance->OnUnloadModule();
               }
               catch (const std::exception &Ex)
               {
                    LogModuleDispatchFailure(It->Instance.get(), "OnUnloadModule", Ex.what());
               }
               catch (...)
               {
                    LogUnknownModuleDispatchFailure(It->Instance.get(), "OnUnloadModule");
               }

               It->UnloadNotified = true;
          }

          if (It->Instance)
          {
               try
               {
                    It->Instance->Stop();
               }
               catch (...)
               {
      
               }

               if (It->Instance.use_count() > 1)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("modules",
                                                "Module '" + It->Name + "' still has external references during unload; deferring final destruction and dlclose() until those references are released.");
                    }

                    std::unique_lock<std::shared_mutex> Lock(ModulesMutex);
                    RetiredModules.push_back(std::move(*It));
                    continue;
               }

               It->Instance.reset();
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("modules", "Unloaded module '" + It->Name + "'.");
          }

          if (It->Handle)
          {
               dlclose(It->Handle);
               It->Handle = nullptr;
          }
     }
}

/* Returns the names of all loaded modules exactly as registered. */

std::vector<std::string> ModuleManager::GetLoadedModuleNames() const
{
     std::shared_lock<std::shared_mutex> Lock(ModulesMutex);

     std::vector<std::string> Names;
     Names.reserve(Modules.size());

     for (const auto &Module : Modules)
     {
          Names.push_back(Module.Name);
     }

     return Names;
}

/* Returns only core module names, trimmed to the public name without the core_ prefix. */

std::vector<std::string> ModuleManager::GetLoadedCoreModuleNames() const
{
     std::shared_lock<std::shared_mutex> Lock(ModulesMutex);

     std::vector<std::string> Names;

     for (const auto &Module : Modules)
     {
          if (Module.Name.rfind("core_", 0) != 0)
          {
               continue;
          }

          Names.push_back(Module.Name.substr(5));
     }

     return Names;
}

/* Returns only optional module names and excludes the core_ set. */

std::vector<std::string> ModuleManager::GetLoadedOptionalModuleNames() const
{
     std::shared_lock<std::shared_mutex> Lock(ModulesMutex);

     std::vector<std::string> Names;

     for (const auto &Module : Modules)
     {
          if (Module.Name.rfind("core_", 0) == 0)
          {
               continue;
          }

          Names.push_back(Module.Name);
     }

     return Names;
}

/* Looks up one loaded module by name and returns the shared runtime instance. */

std::shared_ptr<RuntimeModule> ModuleManager::Find(const std::string &Name) const
{
     ModuleReference Module;
     return GetModuleReference(Name, &Module) ? Module.Instance : nullptr;
}

/* Finds one loaded module and returns both its instance and execution state. */

bool ModuleManager::GetModuleReference(const std::string &Name, ModuleReference *Module) const
{
     if (!Module)
     {
          return false;
     }

     std::shared_lock<std::shared_mutex> Lock(ModulesMutex);

     for (const auto &Loaded : Modules)
     {
          if (Loaded.Name == Name && Loaded.Instance)
          {
               Module->Instance = Loaded.Instance;
               Module->ExecutionState = Loaded.ExecutionState;
               return true;
          }
     }

     return false;
}

/* Collects API descriptions only from modules that opted into /modules routing. */

std::vector<ModuleAPIDescription> ModuleManager::GetModuleAPIDescriptions() const
{
     struct NamedModuleReference
     {
          std::string Name;
          ModuleReference Reference;
     };

     std::vector<NamedModuleReference> Snapshot;

     {
          std::shared_lock<std::shared_mutex> Lock(ModulesMutex);

          Snapshot.reserve(Modules.size());

          for (const auto &Module : Modules)
          {
               ModuleReference ModuleRef{Module.Instance, Module.ExecutionState};

               if (!ModuleRef.Instance || !ModuleRef.Instance->IsAPIRouteEnabled())
               {
                    continue;
               }

               Snapshot.push_back({Module.Name, ModuleRef});
          }
     }

     std::vector<ModuleAPIDescription> Descriptions;
     Descriptions.reserve(Snapshot.size());

     for (const auto &Module : Snapshot)
     {
          const ModuleReference &ModuleRef = Module.Reference;

          if (!ModuleRef.Instance)
          {
               continue;
          }

          if (!BeginModuleCallback(ModuleRef))
          {
               continue;
          }

          ModuleAPIDescription Description;

          try
          {
               Description = ModuleRef.Instance->GetAPIDescription();
               Description.RequirementFlags = ModuleRef.Instance->GetRequirementFlags();
               EndModuleCallback(ModuleRef);
          }
          catch (const std::exception &Ex)
          {
               EndModuleCallback(ModuleRef);
               LogModuleDispatchFailure(ModuleRef.Instance.get(), "GetAPIDescription", Ex.what());
               continue;
          }
          catch (...)
          {
               EndModuleCallback(ModuleRef);
               LogUnknownModuleDispatchFailure(ModuleRef.Instance.get(), "GetAPIDescription");
               continue;
          }

          if (Description.Name.empty())
          {
               Description.Name = Module.Name;
          }

          Descriptions.push_back(std::move(Description));
     }

     return Descriptions;
}

/* Retrieves one module API description when the module exposes an HTTP surface. */

bool ModuleManager::GetModuleAPIDescription(const std::string &ModuleName, ModuleAPIDescription *Description) const
{
     if (!Description)
     {
          return false;
     }

     ModuleReference ModuleRef;

     if (!GetModuleReference(ModuleName, &ModuleRef) || !ModuleRef.Instance)
     {
          return false;
     }

     if (!ModuleRef.Instance->IsAPIRouteEnabled())
     {
          return false;
     }

     if (!BeginModuleCallback(ModuleRef))
     {
          return false;
     }

     try
     {
          *Description = ModuleRef.Instance->GetAPIDescription();
          Description->RequirementFlags = ModuleRef.Instance->GetRequirementFlags();
          EndModuleCallback(ModuleRef);
     }
     catch (const std::exception &Ex)
     {
          EndModuleCallback(ModuleRef);
          LogModuleDispatchFailure(ModuleRef.Instance.get(), "GetAPIDescription", Ex.what());
          return false;
     }
     catch (...)
     {
          EndModuleCallback(ModuleRef);
          LogUnknownModuleDispatchFailure(ModuleRef.Instance.get(), "GetAPIDescription");
          return false;
     }

     if (Description->Name.empty())
     {
          Description->Name = ModuleName;
     }

     return true;
}

/* Returns the shared command surface declared by one API-enabled module. */

bool ModuleManager::GetModuleCommandSpecs(const std::string &ModuleName, std::vector<ModuleCommandSpec> *Commands) const
{
     if (!Commands)
     {
          return false;
     }

     ModuleReference ModuleRef;

     if (!GetModuleReference(ModuleName, &ModuleRef) || !ModuleRef.Instance)
     {
          return false;
     }

     if (!ModuleRef.Instance->IsAPIRouteEnabled())
     {
          return false;
     }

     if (!BeginModuleCallback(ModuleRef))
     {
          return false;
     }

     try
     {
          *Commands = ModuleRef.Instance->GetCommandSpecs();
          EndModuleCallback(ModuleRef);
     }
     catch (const std::exception &Ex)
     {
          EndModuleCallback(ModuleRef);
          LogModuleDispatchFailure(ModuleRef.Instance.get(), "GetCommandSpecs", Ex.what());
          return false;
     }
     catch (...)
     {
          EndModuleCallback(ModuleRef);
          LogUnknownModuleDispatchFailure(ModuleRef.Instance.get(), "GetCommandSpecs");
          return false;
     }

     return true;
}

/* Forwards one normalized shared command request into the target runtime module. */

bool ModuleManager::HandleModuleCommand(const std::string &ModuleName, const ModuleCommandRequest &Request, ModuleCommandResponse *Response) const
{
     if (!Response)
     {
          return false;
     }

     ModuleReference Module;

     if (!GetModuleReference(ModuleName, &Module))
     {
          return false;
     }

     if (!BeginModuleCallback(Module))
     {
          return false;
     }

     try
     {
          *Response = Module.Instance->HandleCommand(Request);
          EndModuleCallback(Module);
          return true;
     }
     catch (const std::exception &Ex)
     {
          EndModuleCallback(Module);
          LogModuleDispatchFailure(Module.Instance.get(), "HandleModuleCommand", Ex.what());
          return false;
     }
     catch (...)
     {
          EndModuleCallback(Module);
          LogUnknownModuleDispatchFailure(Module.Instance.get(), "HandleModuleCommand");
          return false;
     }
}

/* Forwards one /modules HTTP request to the named module after basic eligibility checks. */

HttpResponse ModuleManager::HandleModuleAPIRequest(const std::string &ModuleName, const HttpRequest &Request, const std::string &SubPath) const
{
     ModuleReference Module;

     if (!GetModuleReference(ModuleName, &Module))
     {
          return HttpResponse(404, "Not Found", "application/json");
     }

     if (!Module.Instance->IsAPIRouteEnabled())
     {
          return HttpResponse(404, "Not Found", "application/json");
     }

     if (!BeginModuleCallback(Module))
     {
          return HttpResponse(503, "Service Unavailable", "application/json");
     }

     try
     {
          HttpResponse Response = Module.Instance->HandleAPIRequest(Request, SubPath);
          EndModuleCallback(Module);
          return Response;
     }
     catch (const std::exception &Ex)
     {
          EndModuleCallback(Module);
          LogModuleDispatchFailure(Module.Instance.get(), "HandleModuleAPIRequest", Ex.what());
          return HttpResponse(500, "Internal Server Error", "application/json");
     }
     catch (...)
     {
          EndModuleCallback(Module);
          LogUnknownModuleDispatchFailure(Module.Instance.get(), "HandleModuleAPIRequest");
          return HttpResponse(500, "Internal Server Error", "application/json");
     }
}

/* Reads the aggregated demo mode flag protected by its own mutex. */

bool ModuleManager::IsDemoModeEnabled() const
{
     std::lock_guard<std::mutex> Lock(DemoStateMutex);
     return DemoModeActive;
}

/* Returns the current demo mode explanation string. */

std::string ModuleManager::GetDemoModeMessage() const
{
     std::lock_guard<std::mutex> Lock(DemoStateMutex);
     return DemoModeMessage;
}
