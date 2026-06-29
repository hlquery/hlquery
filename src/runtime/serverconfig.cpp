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
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <system_error>
#include <vector>

#include "core/config.h"
#include "runtime/configreader.h"
#include "runtime/exitmanager.h"
#include "core/hlquery.h"
#include "core/logmanager.h"
#include "core/modulemanager.h"
#include "runtime/serverconfig.h"
#include "utils/consolewriter.h"
#include "utils/tools.h"

static std::string NormalizeClusterEndpoint(const std::string &Raw, std::string *OutError);
static std::string ClusterTrimCopy(const std::string &Value);

static int ResolveAdaptiveMaxThreads()
{
     unsigned int HardwareThreads = std::thread::hardware_concurrency();

     if (HardwareThreads == 0)
     {
          HardwareThreads = static_cast<unsigned int>(HLQUERY_MAX_THREADS);
     }

     int AdaptiveThreads = static_cast<int>(std::max(8U, HardwareThreads / 2));
     AdaptiveThreads = std::min(AdaptiveThreads, HLQUERY_MAX_THREADS);

#if defined(__linux__)
     double LoadAverages[3] = {0.0, 0.0, 0.0};

     if (getloadavg(LoadAverages, 3) > 0)
     {
          if (LoadAverages[0] >= static_cast<double>(HardwareThreads))
          {
               AdaptiveThreads = std::max(8, AdaptiveThreads / 2);
          }
     }

     std::ifstream LoadAverageFile("/proc/loadavg");

     if (LoadAverageFile.good())
     {
          std::string Load1Min;
          std::string Load5Min;
          std::string Load15Min;
          std::string RunningThreads;

          if (LoadAverageFile >> Load1Min >> Load5Min >> Load15Min >> RunningThreads)
          {
               std::size_t SlashPos = RunningThreads.find('/');

               if (SlashPos != std::string::npos)
               {
                    int TotalSystemThreads = std::stoi(RunningThreads.substr(SlashPos + 1));

                    if (TotalSystemThreads > static_cast<int>(HardwareThreads) * 64)
                    {
                         AdaptiveThreads = std::max(8, AdaptiveThreads / 2);
                    }
               }
          }
     }
#endif

     return std::max(1, AdaptiveThreads);
}

/* ServerConfig constructor initializing the command line arguments */

ServerConfig::ServerConfig(int ArgcCount, char **ArgvList) : Valid(false)
{
     CmdLine.argc = ArgcCount;

     CmdLine.argv = ArgvList;
}

/* ServerConfig destructor */

ServerConfig::~ServerConfig()
{
}

/* Loads the server configuration from a specified file path */

bool ServerConfig::LoadConfig(const std::string &ConfigFilePath)
{
     Valid = false;

     ErrorMsg.clear();

     /*
      * Reset lazy-loaded RocksDB options when the configuration is reloaded.
      * This ensures that any subsequent access uses the newly loaded parameters.
      */

     {
          std::lock_guard<std::mutex> Lock(RocksDBOptionsMutex);

          RocksDBOptionsLoaded.store(false, std::memory_order_release);

          RocksDBOptionsValue.reset();
     }

     if (Instance && Instance->Config && Instance->Config->GetDebugMode())
      {
           ConsoleWriter::WriteDebug("ServerConfig::LoadConfig: Loading config file: " + ConfigFilePath + ".");
      }

     {
          std::error_code Ec;
          std::filesystem::path Candidate(ConfigFilePath);

          if (!Candidate.is_absolute())
          {
               Candidate = std::filesystem::absolute(Candidate, Ec);
               Ec.clear();
          }

          ConfigFile = Candidate.empty() ? ConfigFilePath : Candidate.string();
     }

     if (!ConfigReaderValue.LoadFile(ConfigFilePath))
     {
          std::string ConfigErrorDetails = ConfigReaderValue.GetError();

          if (!ConfigErrorDetails.empty())
          {
               ErrorMsg = ConfigErrorDetails;
          }
          else
          {
               ErrorMsg = "Failed to load configuration file: " + ConfigFilePath;

               ErrorMsg += "\n  Check that the file exists and is readable";

               ConsoleWriter::WriteError("Failed to load configuration file: " + ConfigFilePath + ".");

               ConsoleWriter::WriteInfo("Check that the file exists and is readable.");
          }

          return false;
     }

     try
     {
          ApplyConfiguration();

          Valid = true;

          return true;
     }
     catch (const std::exception &e)
     {
          ErrorMsg = "Configuration error";

          ErrorMsg += "\n  File: " + ConfigFilePath;

          ErrorMsg += "\n  Error: " + std::string(e.what());

          ErrorMsg += "\n  Check configuration values and syntax";

          ConsoleWriter::WriteError("Configuration error in file: " + ConfigFilePath + ".");

          ConsoleWriter::WriteError("Error: " + std::string(e.what()) + ".");

          ConsoleWriter::WriteInfo("Check configuration values and syntax.");

          return false;
     }
}

/* Applies settings from the internal config reader to the server state */

void ServerConfig::ApplyConfiguration()
{
     /*
      * Default behavior is to daemonize unless the no-fork flag is provided.
      */

     NoForkMode = false;

     /* Process command line arguments to override file-based configuration */

     for (int i = 1; i < CmdLine.argc; i++)
     {
          std::string ArgumentStr = CmdLine.argv[i];

          if (ArgumentStr == "--nofork" || ArgumentStr == "-n")
          {
               NoForkMode = true;
          }
          else if (ArgumentStr == "--test" || ArgumentStr == "-t")
          {
               TestMode = true;
          }
          else if (ArgumentStr == "--debug" || ArgumentStr == "-d")
          {
               DebugMode = true;
          }
     }

     /* Retrieve general server identification and metadata settings */

     auto ServerSettingsTag = ConfigReaderValue.GetTag("server");

     if (ServerSettingsTag)
     {
          ServerName = ServerSettingsTag->GetString("name", ServerName);

          ServerID = ServerSettingsTag->GetString("id", ServerID);
     }

     ModuleLoads.clear();

     auto ModuleTags = ConfigReaderValue.GetTags("module");

     auto AddModuleIfMissing = [&](const std::string &ModuleName)
     {
          const auto ExistingIt = std::find_if(ModuleLoads.begin(), ModuleLoads.end(),
                                               [&](const ModuleLoadEntry &Existing)
                                               {
                                                    return Existing.Name == ModuleName;
                                               });

          if (ExistingIt != ModuleLoads.end())
          {
               return;
          }

          ModuleLoadEntry Entry;
          Entry.Name = ModuleName;
          ModuleLoads.push_back(std::move(Entry));
     };

     for (const auto &ModuleTag : ModuleTags)
     {
          std::string ModuleName = ModuleTag->GetStringNonEmpty("name", "");
          std::string ModulePath;

          if (ModuleName.empty())
          {
               ModuleName = ModuleTag->GetStringNonEmpty("load", "");
          }

          if (ModuleName.empty())
          {
               throw std::runtime_error("Invalid <module> tag: missing required 'name' attribute.");
          }

          if (!ModuleManager::IsValidModuleName(ModuleName))
          {
               throw std::runtime_error("Invalid <module> tag: module name '" + ModuleName + "' contains unsupported characters.");
          }

          const auto ExistingIt = std::find_if(ModuleLoads.begin(), ModuleLoads.end(),
                                               [&](const ModuleLoadEntry &Existing)
                                               {
                                                    return Existing.Name == ModuleName;
                                               });

          if (ExistingIt != ModuleLoads.end())
          {
               continue;
          }

          ModulePath = ModuleTag->GetString("path", "");

          ModuleLoadEntry Entry;
          Entry.Name = ModuleName;
          Entry.Path = ModulePath;

          ModuleLoads.push_back(Entry);
     }

     static const std::vector<std::string> CoreModuleNames = {"core_timers"};

     for (const auto &CoreName : CoreModuleNames)
     {
          AddModuleIfMissing(CoreName);
     }

     std::filesystem::path ConfigDirectory;

     if (!ConfigFile.empty())
     {
          ConfigDirectory = std::filesystem::path(ConfigFile).parent_path();
     }

     auto AITag = ConfigReaderValue.GetTag("ai");
     bool ResolveAIPathsRelativeToConfig = false;
     bool AutoFindModel = true;

     AIModelName.clear();
     AIModelPath.clear();

     std::string ModelPathOverride;
     std::string ModelFileOverride;

     auto ReadModelName = [](const std::shared_ptr<ConfigTag> &Tag,
                             const std::string &Fallback) -> std::string
     {
          if (!Tag)
          {
               return Fallback;
          }

          std::string Value = Tag->GetString("model_name", "");

          if (!Value.empty())
          {
               return Value;
          }

          return Tag->GetString("model", Fallback);
     };

     if (AITag)
     {
          AIEnabled = AITag->GetBool("enabled", AIEnabled);
          AIModelsDirectory = AITag->GetString("models_dir", AIModelsDirectory);
          AIModelName = ReadModelName(AITag, AIModelName);
          ModelPathOverride = AITag->GetString("model_path", "");
          ModelFileOverride = AITag->GetString("model_file", ModelFileOverride);
          AutoFindModel = AITag->GetBool("auto_find", AutoFindModel);
          ResolveAIPathsRelativeToConfig = AITag->GetBool("relative", ResolveAIPathsRelativeToConfig);
     }
     AIModelCatalog.clear();

     auto ModelTags = ConfigReaderValue.GetTags("model");

     for (const auto &ModelTag : ModelTags)
     {
          std::string ModelName = ModelTag->GetString("name", "");
          std::string ModelFile = ModelTag->GetString("file", "");

          if (ModelName.empty() || ModelFile.empty())
          {
               continue;
          }

          bool IsDefaultModel = ModelTag->GetBool("default", false);

          AIModelCatalog.push_back({ModelName, ModelFile, IsDefaultModel});
     }

     if (AIModelCatalog.empty())
     {
          AIModelCatalog.push_back({"qwen_0_5", "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf", false});
          AIModelCatalog.push_back({"qwen_1_5", "Qwen2.5-1.5B-Instruct-Q4_K_M.gguf", true});
          AIModelCatalog.push_back({"qwen_3", "Qwen2.5-3B-Instruct-Q4_K_M.gguf", false});
          AIModelCatalog.push_back({"qwen_coder_1_5", "Qwen2.5.1-Coder-1.5B-Instruct-Q4_K_M.gguf", false});
     }

    auto ResolveRelativePath = [&](const std::filesystem::path &RawPath) -> std::filesystem::path
    {
         if (RawPath.empty() || RawPath.is_absolute())
         {
              return RawPath;
         }

         std::error_code Ec;
         const std::filesystem::path WorkingCandidate = std::filesystem::absolute(RawPath, Ec);

         if (!Ec && std::filesystem::exists(WorkingCandidate))
         {
              return WorkingCandidate;
         }

         if (!ConfigDirectory.empty())
         {
              std::filesystem::path RepoRootCandidateBase = std::filesystem::path(ConfigDirectory);
              std::filesystem::path RunDir;
              std::filesystem::path RepoRootDir;

              if (RepoRootCandidateBase.filename() == "conf")
              {
                   RunDir = RepoRootCandidateBase.parent_path();

                   if (RunDir.filename() == "run")
                   {
                        RepoRootDir = RunDir.parent_path();

                        if (!RepoRootDir.empty())
                        {
                             const std::filesystem::path LocalSharedModelsDir = RepoRootDir / "run" / "models";
                             const std::filesystem::path ParentSharedModelsDir = RepoRootDir.parent_path() / "run" / "models";

                             if (!std::filesystem::exists(LocalSharedModelsDir) &&
                                 std::filesystem::exists(ParentSharedModelsDir))
                             {
                                  RepoRootDir = RepoRootDir.parent_path();
                             }
                        }
                   }
              }

              const std::filesystem::path ConfigCandidate =
                   std::filesystem::absolute(std::filesystem::path(ConfigDirectory) / RawPath, Ec);

              if (!Ec && std::filesystem::exists(ConfigCandidate))
              {
                   return ConfigCandidate;
              }

              if (!ResolveAIPathsRelativeToConfig && !RepoRootDir.empty())
              {
                   std::error_code RepoRootEC;
                   const std::filesystem::path NormalizedRaw = RawPath.lexically_normal();
                   const std::string NormalizedRawString = NormalizedRaw.generic_string();

                   if (NormalizedRawString == "../models" ||
                       NormalizedRawString.rfind("../models/", 0) == 0)
                   {
                        std::filesystem::path SharedModelsPath = RepoRootDir / "run" / "models";
                        static const std::string SharedModelsPrefix = "../models/";

                        if (NormalizedRawString.rfind(SharedModelsPrefix, 0) == 0 &&
                            NormalizedRawString.size() > SharedModelsPrefix.size())
                        {
                             SharedModelsPath /= NormalizedRawString.substr(SharedModelsPrefix.size());
                        }

                        const std::filesystem::path SharedModelsCandidate =
                             std::filesystem::absolute(SharedModelsPath, RepoRootEC);

                        if (!RepoRootEC && std::filesystem::exists(SharedModelsCandidate))
                        {
                             return SharedModelsCandidate;
                        }
                   }
              }

              if (RawPath.has_relative_path())
              {
                   if (!RepoRootDir.empty())
                   {
                        const std::filesystem::path RepoRootCandidate =
                             std::filesystem::absolute(RepoRootDir / RawPath, Ec);

                        if (!RunDir.empty() && RawPath.begin() != RawPath.end() && *RawPath.begin() == "data" && !Ec)
                        {
                             return std::filesystem::absolute(RunDir / RawPath, Ec);
                        }

                        /* Paths like run/data/... in run/conf/hlquery.conf are project-root-relative,
                         * not relative to run/conf. Resolve them to <repo>/run/... even before the
                         * target exists so runtime directories do not get created under run/conf/run/. */
                        if (RawPath.begin() != RawPath.end() && *RawPath.begin() == "run" && !Ec)
                        {
                             return RepoRootCandidate;
                        }

                        if (!Ec && std::filesystem::exists(RepoRootCandidate))
                        {
                             return RepoRootCandidate;
                        }
                   }
              }

              if (!Ec)
              {
                   return ConfigCandidate;
              }
         }

         return WorkingCandidate;
    };

    auto ResolveModelPath = [&](const ServerConfig::AIModelDescriptor &Descriptor) -> std::string
    {
         std::filesystem::path Candidate(Descriptor.File);

         if (!Candidate.is_absolute() && !AIModelsDirectory.empty())
         {
              Candidate = std::filesystem::path(AIModelsDirectory) / Candidate;
         }

         return ResolveRelativePath(Candidate).string();
    };

    auto FindFirstModel = [&]() -> std::string
    {
         if (AIModelsDirectory.empty())
         {
              return "";
         }

         const std::filesystem::path ModelsDirectory =
              ResolveRelativePath(std::filesystem::path(AIModelsDirectory));
         std::error_code Ec;

         if (!std::filesystem::exists(ModelsDirectory, Ec) ||
             !std::filesystem::is_directory(ModelsDirectory, Ec))
         {
              return "";
         }

         std::vector<std::filesystem::path> Candidates;

         for (std::filesystem::directory_iterator It(ModelsDirectory, Ec), End;
              !Ec && It != End;
              It.increment(Ec))
         {
              if (!It->is_regular_file(Ec))
              {
                   continue;
              }

              std::string Extension = It->path().extension().string();
              std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                             [](unsigned char C)
                             {
                                  return static_cast<char>(std::tolower(C));
                             });

              if (Extension == ".gguf")
              {
                   Candidates.push_back(It->path());
              }
         }

         if (Ec || Candidates.empty())
         {
              return "";
         }

         std::sort(Candidates.begin(), Candidates.end());
         return Candidates.front().string();
    };

    auto PickDefaultModel = [&]() -> std::string
    {
         if (AIModelCatalog.empty())
         {
              return "";
         }

         auto DefaultIt = std::find_if(AIModelCatalog.begin(), AIModelCatalog.end(),
                                       [](const ServerConfig::AIModelDescriptor &Entry)
                                       {
                                            return Entry.IsDefault;
                                       });

         if (DefaultIt != AIModelCatalog.end())
         {
              return DefaultIt->Name;
         }

         return AIModelCatalog.front().Name;
    };

    std::string AutoFoundModelPath;

    if (AutoFindModel &&
        ModelFileOverride.empty() &&
        ModelPathOverride.empty() &&
        AIModelName.empty())
    {
         AutoFoundModelPath = FindFirstModel();
    }

    if (!ModelFileOverride.empty())
    {
         std::filesystem::path FilePath(ModelFileOverride);

         if (!FilePath.is_absolute() && !AIModelsDirectory.empty())
         {
              FilePath = std::filesystem::path(AIModelsDirectory) / FilePath;
         }

         AIModelPath = ResolveRelativePath(FilePath).string();
    }
    else if (!ModelPathOverride.empty())
    {
         std::filesystem::path OverridePath(ModelPathOverride);

         if (!OverridePath.is_absolute() && !AIModelsDirectory.empty())
         {
              OverridePath = std::filesystem::path(AIModelsDirectory) / OverridePath;
         }

         AIModelPath = ResolveRelativePath(OverridePath).string();
    }
    else if (!AutoFoundModelPath.empty())
    {
         AIModelPath = AutoFoundModelPath;
         AIModelName = std::filesystem::path(AIModelPath).filename().string();
    }
    else if (!AIModelCatalog.empty())
    {
         if (AIModelName.empty())
         {
              AIModelName = PickDefaultModel();
         }

         auto SelectedIt = std::find_if(AIModelCatalog.begin(), AIModelCatalog.end(),
                                        [&](const ServerConfig::AIModelDescriptor &Entry)
                                        {
                                             return Entry.Name == AIModelName;
                                        });

         if (SelectedIt != AIModelCatalog.end())
         {
              AIModelPath = ResolveModelPath(*SelectedIt);
         }
         else
         {
              AIModelPath.clear();
         }
    }
    else
    {
         AIModelPath.clear();
    }
    const bool HasExplicitAIConfig = (AITag != nullptr);

    if (HasExplicitAIConfig && AIEnabled)
    {
         if (AIModelPath.empty())
         {
              throw std::runtime_error("AI model is configured but no model path could be resolved.");
         }

         std::error_code ModelEC;

         if (!std::filesystem::exists(AIModelPath, ModelEC) || std::filesystem::is_directory(AIModelPath, ModelEC))
         {
              throw std::runtime_error("Configured AI model file does not exist: " + AIModelPath);
         }

     }

     /* Handle network binding configurations for multiple listeners */

     Binds.clear();

     auto BindTagsList = ConfigReaderValue.GetTags("bind");

     if (BindTagsList.size() > 0)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Found " + std::to_string(BindTagsList.size()) + " bind tags.");
          }
     }

     if (BindTagsList.empty())
     {
          /* Fallback to default network bindings if none are specified */

          BindConfig DefaultBindInstance;

          Binds.push_back(DefaultBindInstance);

          if (Instance && Instance->Logs)
          {
               if (Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("serverconfig", "Using default bind: " + DefaultBindInstance.address + ":" + std::to_string(DefaultBindInstance.port) + ".");
               }
          }
     }
     else
     {
          for (const auto &BindTagEntry : BindTagsList)
          {
               BindConfig NewBindInstance;

               NewBindInstance.address = BindTagEntry->GetString("address", "0.0.0.0");

               NewBindInstance.port = BindTagEntry->GetInt("port", 9200);

               if (NewBindInstance.port < 1 || NewBindInstance.port > 65535)
               {
                    throw std::runtime_error("Invalid bind port '" + std::to_string(NewBindInstance.port) +
                                             "' for address '" + NewBindInstance.address +
                                             "'. Valid TCP ports are 1-65535.");
               }

               NewBindInstance.type = BindTagEntry->GetString("type", "clients");
               std::transform(NewBindInstance.type.begin(), NewBindInstance.type.end(), NewBindInstance.type.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });

               /* Compatibility alias: "server" behaves as standard HTTP listener. */

               if (NewBindInstance.type == "server")
               {
                    NewBindInstance.type = "http";
               }

               if (NewBindInstance.type == "https")
               {
                    NewBindInstance.ssl = true;
               }

               NewBindInstance.ssl_cert = BindTagEntry->GetString("ssl_cert", "");
               NewBindInstance.ssl_key = BindTagEntry->GetString("ssl_key", "");
               NewBindInstance.ssl_protocols = BindTagEntry->GetString("ssl_protocols", "TLSv1.2 TLSv1.3");
               NewBindInstance.ssl_ciphers = BindTagEntry->GetString("ssl_ciphers", "HIGH:!aNULL:!MD5");

               auto ResolvePath = [this](const std::string &RawPath) -> std::string
               {
                    if (RawPath.empty())
                    {
                         return RawPath;
                    }

                    std::filesystem::path PathValue(RawPath);

                    if (PathValue.is_absolute())
                    {
                         return RawPath;
                    }

                    std::error_code Ec;

                    std::filesystem::path ConfigPathValue(this->ConfigFile);

                    if (!ConfigPathValue.is_absolute())
                    {
                         ConfigPathValue = std::filesystem::absolute(ConfigPathValue, Ec);
                         Ec.clear();
                    }

                    std::filesystem::path ConfigDirValue = ConfigPathValue.parent_path();

                    std::vector<std::filesystem::path> BaseDirs;

                    if (!ConfigDirValue.empty())
                    {
                         BaseDirs.push_back(ConfigDirValue);
                         BaseDirs.push_back(ConfigDirValue.parent_path());
                         BaseDirs.push_back(ConfigDirValue.parent_path().parent_path());
                    }

                    std::filesystem::path ConfigDirFallback(HLQUERY_CONFIG_DIR);

                    if (!ConfigDirFallback.empty())
                    {
                         BaseDirs.push_back(ConfigDirFallback);
                         BaseDirs.push_back(ConfigDirFallback.parent_path());
                         BaseDirs.push_back(ConfigDirFallback.parent_path().parent_path());
                    }

                    for (const auto &BaseDir : BaseDirs)
                    {
                         if (BaseDir.empty())
                         {
                              continue;
                         }

                         std::filesystem::path Candidate = BaseDir / PathValue;

                         if (std::filesystem::exists(Candidate, Ec))
                         {
                              return Candidate.string();
                         }
                    }

                    if (!ConfigDirValue.empty())
                    {
                         return (ConfigDirValue / PathValue).string();
                    }

                    if (!ConfigDirFallback.empty())
                    {
                         return (ConfigDirFallback / PathValue).string();
                    }

                    return RawPath;
               };

               if (NewBindInstance.ssl)
               {
                    NewBindInstance.ssl_cert = ResolvePath(NewBindInstance.ssl_cert);
                    NewBindInstance.ssl_key = ResolvePath(NewBindInstance.ssl_key);
               }

               Binds.push_back(NewBindInstance);

               if (Instance && Instance->Logs)
               {
                    if (Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("serverconfig", "Configured bind: " + NewBindInstance.address + ":" + std::to_string(NewBindInstance.port) + " type=" + NewBindInstance.type + ".");
                    }
               }
          }
     }

     /* Apply internal database engine and partitioning settings */

     auto DatabaseSettingsTag = ConfigReaderValue.GetTag("database");

     if (DatabaseSettingsTag)
     {
          DBEngine = DatabaseSettingsTag->GetString("engine", DBEngine);

          std::string DBShardsStrVal = DatabaseSettingsTag->GetString("shards", "");

          if (DBShardsStrVal == "max")
          {
               DBShards = HLQUERY_MAX_THREADS;
          }
          else
          {
               DBShards = DatabaseSettingsTag->GetInt("shards", DBShards);
          }

          DBCompaction = DatabaseSettingsTag->GetBool("compaction", DBCompaction);

          DBCacheSize = DatabaseSettingsTag->GetString("cache_size", DBCacheSize);
     }

     /* Configure thread pools and general execution performance parameters */

     auto PerformanceSettingsTags = ConfigReaderValue.GetTags("performance");

     for (const auto &PerformanceSettingsTag : PerformanceSettingsTags)
     {
          if (!PerformanceSettingsTag)
          {
               continue;
          }

          bool HasRuntimePerformanceSettings = PerformanceSettingsTag->HasAttribute("threads") ||
                                               PerformanceSettingsTag->HasAttribute("max_threads") ||
                                               PerformanceSettingsTag->HasAttribute("queue_size") ||
                                               PerformanceSettingsTag->HasAttribute("max_candidates") ||
                                               PerformanceSettingsTag->HasAttribute("snippet_step") ||
                                               PerformanceSettingsTag->HasAttribute("max_connections") ||
                                               PerformanceSettingsTag->HasAttribute("search_pool_threads") ||
                                               PerformanceSettingsTag->HasAttribute("http_pool_threads") ||
                                               PerformanceSettingsTag->HasAttribute("write_pool_threads") ||
                                               PerformanceSettingsTag->HasAttribute("management_pool_threads");

          if (!HasRuntimePerformanceSettings)
          {
               continue;
          }

          ThreadCount = PerformanceSettingsTag->GetInt("threads", ThreadCount);

          std::string MaxThreadsStrValue = PerformanceSettingsTag->GetString("max_threads", "");

          if (MaxThreadsStrValue == "auto")
          {
               MaxThreads = ResolveAdaptiveMaxThreads();
          }
          else if (MaxThreadsStrValue == "max")
          {
               MaxThreads = HLQUERY_MAX_THREADS;
          }
          else
          {
               MaxThreads = PerformanceSettingsTag->GetInt("max_threads", MaxThreads);
          }

          QueueSize = PerformanceSettingsTag->GetInt("queue_size", QueueSize);

          MaxCandidates = PerformanceSettingsTag->GetInt("max_candidates", MaxCandidates);

          SnippetStep = PerformanceSettingsTag->GetInt("snippet_step", SnippetStep);

          MaxConnections = PerformanceSettingsTag->GetInt("max_connections", MaxConnections);

          auto ParsePoolThreads = [&](const std::string &AttributeName, int DefaultThreads) -> int
          {
               std::string ThreadCountStr = PerformanceSettingsTag->GetString(AttributeName, "");

               if (ThreadCountStr == "max")
               {
                    return 0;
               }

               return PerformanceSettingsTag->GetInt(AttributeName, DefaultThreads);
          };

          SearchPoolThreads = ParsePoolThreads("search_pool_threads", SearchPoolThreads);

          HTTPPoolThreads = ParsePoolThreads("http_pool_threads", HTTPPoolThreads);

          WritePoolThreads = ParsePoolThreads("write_pool_threads", WritePoolThreads);

          ManagementPoolThreads = ParsePoolThreads("management_pool_threads", ManagementPoolThreads);
     }

     /* Configure search behavior and relevance algorithm parameters */

     auto SearchSettingsTag = ConfigReaderValue.GetTag("search");

     if (SearchSettingsTag)
     {
          auto NormalizeAlgorithm = [](std::string Value) -> std::string
          {
               std::transform(Value.begin(), Value.end(), Value.begin(),
                              [](unsigned char c)
                              {
                                   return std::tolower(c);
                              });
               return Value;
          };

          auto IsValidAlgorithm = [](const std::string &Value) -> bool
          {
               return Value == "bm25" || Value == "bm25+" || Value == "tfidf" || Value == "hybrid";
          };

          bool AlgorithmProvidedFlag = SearchSettingsTag->HasAttribute("algorithm");

          std::string SelectedAlgorithmStr;

          if (AlgorithmProvidedFlag)
          {
               SelectedAlgorithmStr = SearchSettingsTag->GetString("algorithm", "");

               if (SelectedAlgorithmStr.empty())
               {
                    ConsoleWriter::WriteError("Search algorithm attribute is empty.");

                    ConsoleWriter::WriteError("Valid algorithms are: bm25, bm25+, tfidf, hybrid.");

                    ExitManager::Exit(1);
               }

               std::string AlgoLowerValue = NormalizeAlgorithm(SelectedAlgorithmStr);

               if (IsValidAlgorithm(AlgoLowerValue))
               {
                    SearchAlgorithm = AlgoLowerValue;
               }
               else
               {
                    ConsoleWriter::WriteError("Invalid search algorithm specified: '" + SelectedAlgorithmStr + "'.");

                    ConsoleWriter::WriteError("Valid algorithms are: bm25, bm25+, tfidf, hybrid.");

                    ExitManager::Exit(1);
               }
          }
          else
          {
               SelectedAlgorithmStr = SearchSettingsTag->GetString("default_ranking", DefaultRanking);

               if (SelectedAlgorithmStr.empty())
               {
                    ConsoleWriter::WriteError("Search algorithm attribute is empty.");

                    ConsoleWriter::WriteError("Valid algorithms are: bm25, bm25+, tfidf, hybrid.");

                    ExitManager::Exit(1);
               }

               std::string AlgoLowerValue = NormalizeAlgorithm(SelectedAlgorithmStr);

               if (IsValidAlgorithm(AlgoLowerValue))
               {
                    SearchAlgorithm = AlgoLowerValue;
               }
               else
               {
                    ConsoleWriter::WriteError("Invalid search algorithm specified: '" + SelectedAlgorithmStr + "'.");

                    ConsoleWriter::WriteError("Valid algorithms are: bm25, bm25+, tfidf, hybrid.");

                    ExitManager::Exit(1);
               }
          }

          DefaultRanking = SearchSettingsTag->GetString("default_ranking", DefaultRanking);

          FuzzyEnabled = SearchSettingsTag->GetBool("fuzzy", FuzzyEnabled);

          MaxEditDistance = SearchSettingsTag->GetInt("max_edit_distance", MaxEditDistance);

          {
               std::string MatchModeValue = SearchSettingsTag->GetString("match_mode", SearchMatchMode);
               std::transform(MatchModeValue.begin(), MatchModeValue.end(), MatchModeValue.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });

               if (MatchModeValue == "and" || MatchModeValue == "or" || MatchModeValue == "min_should_match")
               {
                    SearchMatchMode = MatchModeValue;
               }
               else if (!MatchModeValue.empty())
               {
                    ConsoleWriter::WriteError("Invalid search match_mode specified: '" + MatchModeValue + "'.");

                    ConsoleWriter::WriteError("Valid match modes are: and, or, min_should_match.");

                    ExitManager::Exit(1);
               }
          }

          SearchMinShouldMatch = SearchSettingsTag->GetIntRange("min_should_match", SearchMinShouldMatch, 1, 1000);

          SearchCandidatePruneMultiplier = SearchSettingsTag->GetIntRange("candidate_prune_multiplier", SearchCandidatePruneMultiplier, 0, 1000);

          HighlightStart = SearchSettingsTag->GetString("highlight_start", HighlightStart);

          HighlightEnd = SearchSettingsTag->GetString("highlight_end", HighlightEnd);
     }

     if (DebugMode)
     {
          ConsoleWriter::WriteDebug("Relevance settings: algo=" + SearchAlgorithm + ", k1=" + std::to_string(RankingK1) + ", b=" + std::to_string(RankingB) + ", delta=" + std::to_string(RankingDelta) + ".");
     }

     /* Load fine-grained ranking parameters for the selected algorithm */

     auto RankingParamsTag = ConfigReaderValue.GetTag("params");

     if (RankingParamsTag)
     {
          RankingK1 = RankingParamsTag->GetDoubleRange("k1", RankingK1, 0.1, 10.0);

          RankingB = RankingParamsTag->GetDoubleRange("b", RankingB, 0.0, 1.0);

          RankingDelta = RankingParamsTag->GetDoubleRange("delta", RankingDelta, 0.0, 10.0);

          RankingIDFSmooth = RankingParamsTag->GetDouble("idf_smooth", RankingIDFSmooth);

          RankingNormalize = RankingParamsTag->GetBool("normalize", RankingNormalize);

          {
               std::string IdfModeValue = RankingParamsTag->GetString("idf_mode", RankingIdfMode);
               std::transform(IdfModeValue.begin(), IdfModeValue.end(), IdfModeValue.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (IdfModeValue == "legacy" || IdfModeValue == "smooth")
               {
                    RankingIdfMode = IdfModeValue;
               }
          }

          RankingIdfClampNegative = RankingParamsTag->GetBool("idf_clamp_negative", RankingIdfClampNegative);

          RankingIdfFloorFactor = RankingParamsTag->GetDoubleRange("idf_floor_factor", RankingIdfFloorFactor, 0.0, 1.0);

          RankingBM25Weight = RankingParamsTag->GetDoubleRange("bm25_weight", RankingBM25Weight, 0.0, 1.0);

          RankingTFIDFWeight = RankingParamsTag->GetDoubleRange("tfidf_weight", RankingTFIDFWeight, 0.0, 1.0);

          UrlTokenBoost = RankingParamsTag->GetDoubleRange("url_token_boost", UrlTokenBoost, 0.5, 5.0);

          UrlTldWeight = RankingParamsTag->GetDoubleRange("url_tld_weight", UrlTldWeight, 0.0, 1.0);

          TitleLikeBoost = RankingParamsTag->GetDoubleRange("title_like_boost", TitleLikeBoost, 0.5, 5.0);

          TagLikeBoost = RankingParamsTag->GetDoubleRange("tag_like_boost", TagLikeBoost, 0.5, 3.0);

          ExactMatchBoost = RankingParamsTag->GetDoubleRange("exact_match_boost", ExactMatchBoost, 0.5, 5.0);

          TitleExactBoost = RankingParamsTag->GetDoubleRange("title_exact_boost", TitleExactBoost, 0.5, 10.0);

          ProximityBoostScale = RankingParamsTag->GetDoubleRange("proximity_boost_scale", ProximityBoostScale, 0.1, 5.0);

          ProximityBoostMax = RankingParamsTag->GetDoubleRange("proximity_boost_max", ProximityBoostMax, 1.0, 10.0);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Loaded ranking parameters: k1=" + std::to_string(RankingK1) + ", b=" + std::to_string(RankingB) + ", delta=" + std::to_string(RankingDelta) + ", idf_smooth=" + std::to_string(RankingIDFSmooth) + ", idf_mode=" + RankingIdfMode + ", idf_clamp_negative=" + std::string(RankingIdfClampNegative ? "true" : "false") + ", idf_floor_factor=" + std::to_string(RankingIdfFloorFactor) + ", normalize=" + std::string(RankingNormalize ? "true" : "false") + ", bm25_weight=" + std::to_string(RankingBM25Weight) + ", tfidf_weight=" + std::to_string(RankingTFIDFWeight) + ", url_token_boost=" + std::to_string(UrlTokenBoost) + ", url_tld_weight=" + std::to_string(UrlTldWeight) + ", title_like_boost=" + std::to_string(TitleLikeBoost) + ", tag_like_boost=" + std::to_string(TagLikeBoost) + ", exact_match_boost=" + std::to_string(ExactMatchBoost) + ", title_exact_boost=" + std::to_string(TitleExactBoost) + ", proximity_boost_scale=" + std::to_string(ProximityBoostScale) + ", proximity_boost_max=" + std::to_string(ProximityBoostMax) + ".");
          }
     }

     /* Configure hybrid merge and rerank behavior. */

     auto HybridMergeTag = ConfigReaderValue.GetTag("hybrid_merge");

     if (HybridMergeTag)
     {
          {
               std::string MergeMethodValue = HybridMergeTag->GetString("method", HybridMergeMethod);
               std::transform(MergeMethodValue.begin(), MergeMethodValue.end(), MergeMethodValue.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (MergeMethodValue == "linear" || MergeMethodValue == "rrf")
               {
                    HybridMergeMethod = MergeMethodValue;
               }
          }

          HybridNormalizeComponentScores = HybridMergeTag->GetBool("normalize_component_scores", HybridNormalizeComponentScores);

          {
               std::string NormalizationMethodValue = HybridMergeTag->GetString("normalization_method", HybridNormalizationMethod);
               std::transform(NormalizationMethodValue.begin(), NormalizationMethodValue.end(), NormalizationMethodValue.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });
               if (NormalizationMethodValue == "minmax" || NormalizationMethodValue == "zscore")
               {
                    HybridNormalizationMethod = NormalizationMethodValue;
               }
          }

          HybridRrfK = HybridMergeTag->GetIntRange("rrf_k", HybridRrfK, 1, 100000);
          HybridDynamicAlphaEnabled = HybridMergeTag->GetBool("dynamic_alpha", HybridDynamicAlphaEnabled);
          HybridShortQueryTerms = HybridMergeTag->GetIntRange("short_query_terms", HybridShortQueryTerms, 1, 1000);
          HybridLongQueryTerms = HybridMergeTag->GetIntRange("long_query_terms", HybridLongQueryTerms, 1, 1000);
          HybridAlphaShort = HybridMergeTag->GetDoubleRange("alpha_short", HybridAlphaShort, 0.0, 1.0);
          HybridAlphaMedium = HybridMergeTag->GetDoubleRange("alpha_medium", HybridAlphaMedium, 0.0, 1.0);
          HybridAlphaLong = HybridMergeTag->GetDoubleRange("alpha_long", HybridAlphaLong, 0.0, 1.0);
          HybridVectorCandidateLimit = HybridMergeTag->GetIntRange("vector_candidate_limit", HybridVectorCandidateLimit, 1, 10000000);
          HybridRerankEnabled = HybridMergeTag->GetBool("rerank_enabled", HybridRerankEnabled);
          HybridRerankTopK = HybridMergeTag->GetIntRange("rerank_top_k", HybridRerankTopK, 1, 100000);
          HybridRerankLexicalWeight = HybridMergeTag->GetDoubleRange("rerank_lexical_weight", HybridRerankLexicalWeight, 0.0, 10.0);
          HybridRerankVectorWeight = HybridMergeTag->GetDoubleRange("rerank_vector_weight", HybridRerankVectorWeight, 0.0, 10.0);
          HybridRerankCoverageBoost = HybridMergeTag->GetDoubleRange("rerank_coverage_boost", HybridRerankCoverageBoost, 0.0, 10.0);

          if (HybridLongQueryTerms < HybridShortQueryTerms)
          {
               HybridLongQueryTerms = HybridShortQueryTerms;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Loaded hybrid merge: method=" + HybridMergeMethod + ", normalize_component_scores=" + std::string(HybridNormalizeComponentScores ? "true" : "false") + ", normalization_method=" + HybridNormalizationMethod + ", rrf_k=" + std::to_string(HybridRrfK) + ", dynamic_alpha=" + std::string(HybridDynamicAlphaEnabled ? "true" : "false") + ", alpha_short=" + std::to_string(HybridAlphaShort) + ", alpha_medium=" + std::to_string(HybridAlphaMedium) + ", alpha_long=" + std::to_string(HybridAlphaLong) + ", vector_candidate_limit=" + std::to_string(HybridVectorCandidateLimit) + ", rerank_enabled=" + std::string(HybridRerankEnabled ? "true" : "false") + ", rerank_top_k=" + std::to_string(HybridRerankTopK) + ".");
          }
     }

     /* Apply indexing-specific optimizations and feature flags */

     auto IndexingSettingsTags = ConfigReaderValue.GetTags("indexing");

     for (const auto &IndexingSettingsTag : IndexingSettingsTags)
     {
          if (!IndexingSettingsTag)
          {
               continue;
          }

          IndexingEnableWildcards = IndexingSettingsTag->GetBool("enable_wildcards", IndexingEnableWildcards);

          IndexingEnableCollectionWildcards = IndexingSettingsTag->GetBool("enable_collection_wildcards", IndexingEnableCollectionWildcards);

          IndexingEnablePrefixMatching = IndexingSettingsTag->GetBool("enable_prefix_matching", IndexingEnablePrefixMatching);

          IndexingMaxWildcardExpansions = IndexingSettingsTag->GetIntRange("max_wildcard_expansions", IndexingMaxWildcardExpansions, 1, 10000);

          IndexingIndexFieldsSeparately = IndexingSettingsTag->GetBool("index_fields_separately", IndexingIndexFieldsSeparately);

          IndexingStorePositions = IndexingSettingsTag->GetBool("store_positions", IndexingStorePositions);

          IndexingStoreOffsets = IndexingSettingsTag->GetBool("store_offsets", IndexingStoreOffsets);
     }

     /* Configure query parsing and execution constraints */

     auto QuerySettingsTag = ConfigReaderValue.GetTag("query_settings");

     if (QuerySettingsTag)
     {
          QuerySettingsMaxQueryLength = QuerySettingsTag->GetIntRange("max_query_length", QuerySettingsMaxQueryLength, 1, 100000);

          QuerySettingsMaxQueryTerms = QuerySettingsTag->GetIntRange("max_query_terms", QuerySettingsMaxQueryTerms, 1, 1000);

          QuerySettingsMinQueryLength = QuerySettingsTag->GetIntRange("min_query_length", QuerySettingsMinQueryLength, 0, 1000);

          QuerySettingsEnableStemming = QuerySettingsTag->GetBool("enable_stemming", QuerySettingsEnableStemming);

          QuerySettingsEnableSynonyms = QuerySettingsTag->GetBool("enable_synonyms", QuerySettingsEnableSynonyms);

          QuerySettingsEnableStopwords = QuerySettingsTag->GetBool("enable_stopwords", QuerySettingsEnableStopwords);

          QuerySettingsEnableFuzzy = QuerySettingsTag->GetBool("enable_fuzzy", QuerySettingsEnableFuzzy);

          QuerySettingsFuzzyMaxDistance = QuerySettingsTag->GetIntRange("fuzzy_max_distance", QuerySettingsFuzzyMaxDistance, 1, 5);

          QuerySettingsRequireExactIdentifierTokens = QuerySettingsTag->GetBool("require_exact_identifier_tokens", QuerySettingsRequireExactIdentifierTokens);
     }

     /* Configure score normalization and precision settings */

     auto ScoringSettingsTag = ConfigReaderValue.GetTag("scoring");

     if (ScoringSettingsTag)
     {
          ScoringMinScoreThreshold = ScoringSettingsTag->GetDoubleRange("min_score_threshold", ScoringMinScoreThreshold, 0.0, 100.0);

          ScoringNormalizeScores = ScoringSettingsTag->GetBool("normalize_scores", ScoringNormalizeScores);

          ScoringScorePrecision = ScoringSettingsTag->GetIntRange("score_precision", ScoringScorePrecision, 0, 15);

          ScoringEnableScoreExplanation = ScoringSettingsTag->GetBool("enable_score_explanation", ScoringEnableScoreExplanation);
     }

     /* Apply execution time limits and candidate pool constraints */

     auto TimeoutSettingsTag = ConfigReaderValue.GetTag("timeouts");

     if (TimeoutSettingsTag)
     {
          TimeoutsQueryTimeoutMS = TimeoutSettingsTag->GetIntRange("query_timeout_ms", TimeoutsQueryTimeoutMS, 100, 300000);

          TimeoutsIndexingTimeoutMS = TimeoutSettingsTag->GetIntRange("indexing_timeout_ms", TimeoutsIndexingTimeoutMS, 1000, 600000);

          TimeoutsMaxCandidates = TimeoutSettingsTag->GetIntRange("max_candidates", TimeoutsMaxCandidates, 10, 1000000);

          TimeoutsMinCandidates = TimeoutSettingsTag->GetIntRange("min_candidates", TimeoutsMinCandidates, 1, 1000);
     }

     /* Configure logging verbosity and filtering for search operations */

     auto LoggingSettingsTag = ConfigReaderValue.GetTag("logging");

     if (LoggingSettingsTag)
     {
          LoggingLogQueries = LoggingSettingsTag->GetBool("log_queries", LoggingLogQueries);

          LoggingLogSlowQueries = LoggingSettingsTag->GetBool("log_slow_queries", LoggingLogSlowQueries);

          LoggingSlowQueryThresholdMS = LoggingSettingsTag->GetIntRange("slow_query_threshold_ms", LoggingSlowQueryThresholdMS, 1, 60000);

          LoggingLogScoringDetails = LoggingSettingsTag->GetBool("log_scoring_details", LoggingLogScoringDetails);

          LoggingLogLevel = LoggingSettingsTag->GetIntRange("log_level", LoggingLogLevel, 0, 3);
     }

     /* Apply miscellaneous search flags and behavioral options */

     auto SearchOptionsTag = ConfigReaderValue.GetTag("search_options");

     if (SearchOptionsTag)
     {
          SearchOptionsTrackTotalHits = SearchOptionsTag->GetBool("track_total_hits", SearchOptionsTrackTotalHits);

          SearchOptionsTrackScores = SearchOptionsTag->GetBool("track_scores", SearchOptionsTrackScores);

          SearchOptionsExplain = SearchOptionsTag->GetBool("explain", SearchOptionsExplain);

          SearchOptionsStoredFields = SearchOptionsTag->GetBool("stored_fields", SearchOptionsStoredFields);

          SearchOptionsVersion = SearchOptionsTag->GetBool("version", SearchOptionsVersion);

          SearchOptionsPreference = SearchOptionsTag->GetString("preference", SearchOptionsPreference);

          SearchOptionsRequestCache = SearchOptionsTag->GetBool("request_cache", SearchOptionsRequestCache);

          SearchOptionsAllowPartialSearchResults = SearchOptionsTag->GetBool("allow_partial_search_results", SearchOptionsAllowPartialSearchResults);

          SearchOptionsBatchedReduceSize = SearchOptionsTag->GetIntRange("batched_reduce_size", SearchOptionsBatchedReduceSize, 1, 10000);

          SearchOptionsTypedKeys = SearchOptionsTag->GetBool("typed_keys", SearchOptionsTypedKeys);
     }

     /* Handle search-specific performance caching settings */

     auto PerformanceTagsList = ConfigReaderValue.GetTags("performance");

     for (const auto &PerformanceTagItem : PerformanceTagsList)
     {
          if (PerformanceTagItem->HasAttribute("idf_cache"))
          {
               PerformanceIDFCache = PerformanceTagItem->GetBool("idf_cache", PerformanceIDFCache);

               PerformanceDocLengthCache = PerformanceTagItem->GetBool("doc_length_cache", PerformanceDocLengthCache);

               PerformanceAvgLengthCache = PerformanceTagItem->GetBool("avg_length_cache", PerformanceAvgLengthCache);

               PerformanceMaxCacheSizeMB = PerformanceTagItem->GetIntRange("max_cache_size_mb", PerformanceMaxCacheSizeMB, 1, 16384);

               PerformanceCacheTTLSeconds = PerformanceTagItem->GetIntRange("cache_ttl_seconds", PerformanceCacheTTLSeconds, 1, 86400);

               break;
          }
     }

     /* Apply result set and offset limits for search operations */

     auto SearchLimitsTagsList = ConfigReaderValue.GetTags("limits");

     for (const auto &LimitsTagItem : SearchLimitsTagsList)
     {
          if (LimitsTagItem->HasAttribute("default_limit"))
          {
               LimitsDefaultLimit = LimitsTagItem->GetIntRange("default_limit", LimitsDefaultLimit, 0, 100000);

               LimitsMaxLimit = LimitsTagItem->GetIntRange("max_limit", LimitsMaxLimit, 1, 100000);

               LimitsMinLimit = LimitsTagItem->GetIntRange("min_limit", LimitsMinLimit, 1, 1000);

               LimitsDefaultOffset = LimitsTagItem->GetIntRange("default_offset", LimitsDefaultOffset, 0, 1000000);

               LimitsMaxOffset = LimitsTagItem->GetIntRange("max_offset", LimitsMaxOffset, 0, 10000000);

               break;
          }
     }

     /* Configure recency decay modeling for time-sensitive relevance */

     auto RecencyDecayTag = ConfigReaderValue.GetTag("recency_decay");

     if (RecencyDecayTag)
     {
          RecencyDecayEnabled = RecencyDecayTag->GetBool("enabled", RecencyDecayEnabled);

          RecencyDecayModel = RecencyDecayTag->GetString("decay_model", RecencyDecayModel);

          RecencyBaseWeight = RecencyDecayTag->GetDoubleRange("base_weight", RecencyBaseWeight, 0.0, 2.0);

          RecencyMaxBoost = RecencyDecayTag->GetDoubleRange("max_boost", RecencyMaxBoost, 1.0, 5.0);

          RecencyHalfLifeDays = RecencyDecayTag->GetDoubleRange("half_life_days", RecencyHalfLifeDays, 0.1, 1000.0);

          RecencyDecayRate = RecencyDecayTag->GetDoubleRange("decay_rate", RecencyDecayRate, 0.0001, 1.0);

          RecencySigmoidSteepness = RecencyDecayTag->GetDoubleRange("sigmoid_steepness", RecencySigmoidSteepness, 0.001, 10.0);

          RecencySigmoidCenter = RecencyDecayTag->GetDoubleRange("sigmoid_center", RecencySigmoidCenter, 0.0, 1000.0);

          RecencyPowerExponent = RecencyDecayTag->GetDoubleRange("power_exponent", RecencyPowerExponent, 0.1, 10.0);

          RecencyGaussianMean = RecencyDecayTag->GetDoubleRange("gaussian_mean", RecencyGaussianMean, -1000.0, 1000.0);

          RecencyGaussianStddev = RecencyDecayTag->GetDoubleRange("gaussian_stddev", RecencyGaussianStddev, 0.1, 1000.0);

          RecencyMinDecayFactor = RecencyDecayTag->GetDoubleRange("min_decay_factor", RecencyMinDecayFactor, 0.0, 1.0);

          RecencyUseTimestampField = RecencyDecayTag->GetBool("use_timestamp_field", RecencyUseTimestampField);

          RecencyNormalizeByCollection = RecencyDecayTag->GetBool("normalize_by_collection", RecencyNormalizeByCollection);

          RecencyQueryDependent = RecencyDecayTag->GetBool("query_dependent", RecencyQueryDependent);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Loaded recency decay: enabled=" + std::string(RecencyDecayEnabled ? "true" : "false") + ", model=" + RecencyDecayModel + ", base_weight=" + std::to_string(RecencyBaseWeight) + ", max_boost=" + std::to_string(RecencyMaxBoost) + ", half_life=" + std::to_string(RecencyHalfLifeDays) + " days.");
          }
     }

     /* Apply durability, persistence, and encryption settings */

     auto DurabilitySettingsTag = ConfigReaderValue.GetTag("durability");

     if (DurabilitySettingsTag)
     {
          AOFFsyncEnabled = DurabilitySettingsTag->GetBool("aof_fsync_enabled", AOFFsyncEnabled);

          AOFFsyncPolicy = DurabilitySettingsTag->GetString("aof_fsync_policy", AOFFsyncPolicy);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("config", "AOF fsync enabled: " + std::string(AOFFsyncEnabled ? "true" : "false") + ", policy: " + AOFFsyncPolicy + ".");
          }

          RDBEncryptionEnabled = DurabilitySettingsTag->GetBool("rdb_encryption_enabled", RDBEncryptionEnabled);

          RDBEncryptionKeySource = DurabilitySettingsTag->GetString("rdb_encryption_key_source", RDBEncryptionKeySource);

          ClientOutbufSoft = static_cast<size_t>(DurabilitySettingsTag->GetInt("client_outbuf_soft", static_cast<int>(ClientOutbufSoft)));

          ClientOutbufHard = static_cast<size_t>(DurabilitySettingsTag->GetInt("client_outbuf_hard", static_cast<int>(ClientOutbufHard)));

          AOFMaxSizeBytes = static_cast<size_t>(DurabilitySettingsTag->GetInt("aof_max_size_bytes", static_cast<int>(AOFMaxSizeBytes)));

          AOFRewriteMinSizeBytes = static_cast<size_t>(DurabilitySettingsTag->GetInt("aof_rewrite_min_size_bytes", static_cast<int>(AOFRewriteMinSizeBytes)));

          AOFRDBSyncEnabled = DurabilitySettingsTag->GetBool("aof_rdb_sync_enabled", AOFRDBSyncEnabled);

          AOFRewriteThresholdPercent = DurabilitySettingsTag->GetInt("aof_rewrite_threshold_percent", AOFRewriteThresholdPercent);
     }

     /* Configure the global logging streams and targets */

     LogConfigs.clear();

     auto LogTagsList = ConfigReaderValue.GetTags("log");
     std::filesystem::path ResolvedLogDir = HLQUERY_LOG_DIR;
     {
          std::error_code Ec;
          std::filesystem::path ConfigPathValue(ConfigFile);
          if (!ConfigPathValue.empty())
          {
               if (!ConfigPathValue.is_absolute())
               {
                    ConfigPathValue = std::filesystem::absolute(ConfigPathValue, Ec);
                    Ec.clear();
               }

               const std::filesystem::path ConfigDirValue = ConfigPathValue.parent_path();
               if (!ConfigDirValue.empty())
               {
                    /* Development configs live under run/conf and should log to run/logs.
                     * System packages are configured with HLQUERY_LOG_DIR=/var/log/hlquery;
                     * do not derive /etc/hlquery/logs from /etc/hlquery/hlquery.conf. */
                    if (ConfigDirValue.filename() == "conf" && !ConfigDirValue.parent_path().empty())
                    {
                         ResolvedLogDir = ConfigDirValue.parent_path() / "logs";
                    }
               }
          }
     }

     if (LogTagsList.empty())
     {
          LogConfig DefaultLogConfig;

          DefaultLogConfig.method = "file";

          DefaultLogConfig.type = "*";

          DefaultLogConfig.level = LogLevel::LOG_NORMAL;

          DefaultLogConfig.target = (ResolvedLogDir / "hlquery.log").string();

          LogConfigs.push_back(DefaultLogConfig);

          LogConfig ConsoleLogConfig;

          ConsoleLogConfig.method = "console";

          ConsoleLogConfig.type = "*";

          ConsoleLogConfig.level = LogLevel::LOG_NORMAL;

          ConsoleLogConfig.target = "console";

          LogConfigs.push_back(ConsoleLogConfig);
     }
     else
     {
          auto ParseRotationInterval = [](const std::shared_ptr<ConfigTag> &LogTagItem) -> int
          {
               if (!LogTagItem->HasAttribute("rotate_interval"))
               {
                    return 0;
               }

               std::string RotationIntervalValue = LogTagItem->GetString("rotate_interval", "");
               std::transform(RotationIntervalValue.begin(), RotationIntervalValue.end(), RotationIntervalValue.begin(),
                              [](unsigned char Ch)
                              {
                                   return static_cast<char>(std::tolower(Ch));
                              });

               if (RotationIntervalValue == "daily" || RotationIntervalValue == "day")
               {
                    return -1;
               }

               if (RotationIntervalValue == "weekly" || RotationIntervalValue == "week")
               {
                    return -2;
               }

               return LogTagItem->GetInt("rotate_interval", 0);
          };

          for (const auto &LogTagItem : LogTagsList)
          {
               LogConfig NewLogConfig;

               NewLogConfig.method = LogTagItem->GetString("method", "file");

               NewLogConfig.type = LogTagItem->GetString("type", "*");

               NewLogConfig.level = LogManager::StringToLogLevel(LogTagItem->GetString("level", "normal"));

               NewLogConfig.target = LogTagItem->GetString("target", (ResolvedLogDir / "hlquery.log").string());

               NewLogConfig.max_size = LogTagItem->GetSize("rotate_size", 0);

               NewLogConfig.rotation_interval = ParseRotationInterval(LogTagItem);

               NewLogConfig.max_rotated_files = LogTagItem->HasAttribute("max_rotated_files")
                    ? static_cast<size_t>(LogTagItem->GetUnsignedInt("max_rotated_files", static_cast<unsigned int>(NewLogConfig.max_rotated_files)))
                    : NewLogConfig.max_rotated_files;

               NewLogConfig.max_age_days = LogTagItem->HasAttribute("max_age_days")
                    ? static_cast<size_t>(LogTagItem->GetUnsignedInt("max_age_days", 0))
                    : 0;

               if (NewLogConfig.method == "file")
               {
                    std::filesystem::path TargetPath(NewLogConfig.target);

                    if (!TargetPath.is_absolute())
                    {
                         TargetPath = ResolvedLogDir / TargetPath;

                         NewLogConfig.target = TargetPath.string();
                    }
               }

               LogConfigs.push_back(NewLogConfig);
          }
     }

     /* Handle global results limits from the primary configuration section */

     auto MainLimitsTagsList = ConfigReaderValue.GetTags("limits");

     for (const auto &MainLimitTagItem : MainLimitsTagsList)
     {
          if (MainLimitTagItem->HasAttribute("max_results") && !MainLimitTagItem->HasAttribute("default_limit"))
          {
               MaxResults = MainLimitTagItem->GetInt("max_results", MaxResults);

               break;
          }
     }

     /* Configure IP access control and authorization policies */

     HostDenyDefined = false;

     auto AccessAllowTags = ConfigReaderValue.GetTags("allow");
     auto AccessDenyTags = ConfigReaderValue.GetTags("deny");
     auto AccessHostTags = ConfigReaderValue.GetTags("host");
     std::vector<std::string> AllowEntries;
     std::vector<std::string> DenyEntries;

     for (const auto &AllowTagItem : AccessAllowTags)
     {
          if (AllowTagItem->HasAttribute("allow"))
          {
               AllowEntries.push_back(AllowTagItem->GetString("allow", ""));
          }
          else if (AllowTagItem->HasAttribute("allowed"))
          {
               AllowEntries.push_back(AllowTagItem->GetString("allowed", ""));
          }
     }

     for (const auto &HostTagItem : AccessHostTags)
     {
          if (HostTagItem->HasAttribute("allow"))
          {
               AllowEntries.push_back(HostTagItem->GetString("allow", ""));
          }
          if (HostTagItem->HasAttribute("deny"))
          {
               std::string HostDenyValue = HostTagItem->GetString("deny", "");

               DenyEntries.push_back(HostDenyValue);

               if (!Tools::Trim(HostDenyValue).empty())
               {
                    HostDenyDefined = true;
               }
          }
     }

     for (const auto &DenyTagItem : AccessDenyTags)
     {
          if (DenyTagItem->HasAttribute("deny"))
          {
               DenyEntries.push_back(DenyTagItem->GetString("deny", ""));
          }
          else if (DenyTagItem->HasAttribute("denied"))
          {
               DenyEntries.push_back(DenyTagItem->GetString("denied", ""));
          }
     }

     if (!AllowEntries.empty())
     {
          std::string MergedAllow;

          for (size_t I = 0; I < AllowEntries.size(); ++I)
          {
               if (AllowEntries[I].empty())
               {
                    continue;
               }

               if (!MergedAllow.empty())
               {
                    MergedAllow += ",";
               }

               MergedAllow += AllowEntries[I];
          }

          IPAllow = MergedAllow;
     }
     else
     {
          IPAllow = "";

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("serverconfig", "IP allow: no <allow> or <host> tags found - deny all (default).");
          }
     }

     if (!DenyEntries.empty())
     {
          std::string MergedDeny;

          for (size_t I = 0; I < DenyEntries.size(); ++I)
          {
               if (DenyEntries[I].empty())
               {
                    continue;
               }

               if (!MergedDeny.empty())
               {
                    MergedDeny += ",";
               }

               MergedDeny += DenyEntries[I];
          }

          IPDeny = MergedDeny;
     }
     else
     {
          IPDeny = "";
     }

     /* Configure IP filter DNS cache settings */

     DNSCacheMaxSize = DNS_CACHE_MAX_SIZE;

     auto IPFilterTags = ConfigReaderValue.GetTags("ip_filter");
     auto LegacyIPTFilterTags = ConfigReaderValue.GetTags("iptfilter");

     IPFilterTags.insert(IPFilterTags.end(), LegacyIPTFilterTags.begin(), LegacyIPTFilterTags.end());

     for (const auto &IPFilterTag : IPFilterTags)
     {
          if (!IPFilterTag->HasAttribute("dns_cache_size"))
          {
               continue;
          }

          unsigned int CacheSizeVal = IPFilterTag->GetUnsignedInt("dns_cache_size", static_cast<unsigned int>(DNSCacheMaxSize));

          if (CacheSizeVal == 0)
          {
               CacheSizeVal = static_cast<unsigned int>(DNSCacheMaxSize);
          }

          DNSCacheMaxSize = CacheSizeVal;
     }

     if (!IPFilterTags.empty() && Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("serverconfig",
                                "Loaded " + std::to_string(IPFilterTags.size()) +
                                " ip_filter/iptfilter tag(s): dns_cache_size=" +
                                std::to_string(DNSCacheMaxSize) + ".");
     }

     /* Configure pivot-based normalization settings */

     auto PivotNormTag = ConfigReaderValue.GetTag("pivot_norm");

     if (PivotNormTag)
     {
          PivotNormEnabled = PivotNormTag->GetBool("enabled", PivotNormEnabled);

          PivotNormPivot = PivotNormTag->GetDoubleRange("pivot", PivotNormPivot, 0.0, 1.0);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Loaded pivot normalization: enabled=" + std::string(PivotNormEnabled ? "true" : "false") + ", pivot=" + std::to_string(PivotNormPivot) + ".");
          }
     }

     /* Configure clustering and distributed search settings */

     {
          std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
          ClusterNodes.clear();
          SlaveNodes.clear();
          ClusterPeerTokens.clear();
          SlavePeerTokens.clear();
     }

     auto ClusterSettingsTag = ConfigReaderValue.GetTag("cluster");

     if (ClusterSettingsTag)
     {
          ClusterEnabled = ClusterSettingsTag->GetBool("enabled", ClusterEnabled);

          ClusterPort = ClusterSettingsTag->GetInt("port", ClusterPort);

          std::string NodesStr = ClusterSettingsTag->GetString("nodes", "");

          if (!NodesStr.empty())
          {
               std::stringstream ss(NodesStr);
               std::string node;

               while (std::getline(ss, node, ','))
               {
                    node.erase(0, node.find_first_not_of(" \t\r\n"));
                    node.erase(node.find_last_not_of(" \t\r\n") + 1);

                    if (!node.empty())
                    {
                         std::string Error;
                         std::string Normalized = NormalizeClusterEndpoint(node, &Error);
                         if (!Normalized.empty())
                         {
                              ClusterNodes.push_back(Normalized);
                         }
                    }
               }
          }
     }

     /* Load links.conf nodes as cluster nodes. */

     auto LegacyPeerTags = ConfigReaderValue.GetTags("peer");
     if (!LegacyPeerTags.empty())
     {
          throw std::runtime_error("links.conf uses deprecated <peer ...> tags. Replace them with <node ...>.");
     }

     auto ParseNodeRole = [](const std::string &RawRole, bool *OutCluster, bool *OutSlave, std::string *OutError)
     {
          *OutCluster = false;
          *OutSlave = false;

          std::string Role = ClusterTrimCopy(RawRole);
          if (Role.empty())
          {
               *OutCluster = true;
               return true;
          }

          std::transform(Role.begin(), Role.end(), Role.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          /* role= must be a single value, not a comma/pipe/whitespace-separated list. */
          for (unsigned char C : Role)
          {
               if (C == ',' || C == '|' || std::isspace(C))
               {
                    if (OutError)
                    {
                         *OutError = "Node role must be a single value (distributed/search/master or slave/replica). If you need both purposes, add two <node ...> entries (or use legacy <slave ...> for replication).";
                    }
                    return false;
               }
          }

          if (Role == "distributed" || Role == "search" || Role == "master")
          {
               *OutCluster = true;
          }
          else if (Role == "slave" || Role == "replica")
          {
               *OutSlave = true;
          }
          else if (Role == "both" || Role == "all")
          {
               if (OutError)
               {
                    *OutError = "Node role '" + Role + "' is not supported; use either distributed/search/master or slave/replica";
               }
               return false;
          }
          else
          {
               if (OutError)
               {
                    *OutError = "Unknown node role '" + Role + "'";
               }
               return false;
          }

          return true;
     };

     auto ReadLinkEndpoint = [](const std::shared_ptr<ConfigTag> &Tag,
                                const std::string &TagName,
                                std::string *OutNormalized,
                                std::pair<std::string, std::string> *OutTokens)
     {
          std::string Address = Tag->GetString("address", Tag->GetString("host", ""));
          int Port = Tag->GetInt("port", 0);
          if (Address.empty() || Port <= 0 || Port > 65535)
          {
               throw std::runtime_error("Invalid <" + TagName + " ...> entry in links.conf: host/address and port are required.");
          }

          std::string Error;
          std::string Normalized = NormalizeClusterEndpoint(Address + ":" + std::to_string(Port), &Error);
          if (Normalized.empty())
          {
               throw std::runtime_error("Invalid <" + TagName + " ...> endpoint '" + Address + ":" + std::to_string(Port) + "': " + Error + ".");
          }

          std::string PrimaryToken = ClusterTrimCopy(Tag->GetString("token",
                                                                    Tag->GetString("api_key",
                                                                                   Tag->GetString("pass",
                                                                                                  Tag->GetString("password",
                                                                                                                 Tag->GetString("passwd", ""))))));
          std::string SecondaryToken = ClusterTrimCopy(Tag->GetString("token2",
                                                                      Tag->GetString("pass2", "")));

          if (OutNormalized)
          {
               *OutNormalized = Normalized;
          }

          if (OutTokens)
          {
               *OutTokens = std::make_pair(PrimaryToken, SecondaryToken);
          }
     };

     auto NodeTagsList = ConfigReaderValue.GetTags("node");
     if (!NodeTagsList.empty())
     {
          for (const auto &NodeTag : NodeTagsList)
          {
               if (!NodeTag)
               {
                    continue;
               }

               bool IncludeCluster = false;
               bool IncludeSlave = false;
               std::string RoleError;
               if (!ParseNodeRole(NodeTag->GetString("role", ""), &IncludeCluster, &IncludeSlave, &RoleError))
               {
                    throw std::runtime_error("Invalid <node ...> entry in links.conf: " + RoleError + ".");
               }

               std::string Normalized;
               std::pair<std::string, std::string> Tokens;
               ReadLinkEndpoint(NodeTag, "node", &Normalized, &Tokens);

               if (IncludeCluster)
               {
                    ClusterNodes.push_back(Normalized);
                    if (!Tokens.first.empty() || !Tokens.second.empty())
                    {
                         ClusterPeerTokens[Normalized] = Tokens;
                    }
                    ClusterEnabled = true;
               }

               if (IncludeSlave)
               {
                    SlaveNodes.push_back(Normalized);
                    if (!Tokens.first.empty() || !Tokens.second.empty())
                    {
                         SlavePeerTokens[Normalized] = Tokens;
                    }
               }
          }
     }

     if (!ClusterNodes.empty())
     {
          std::sort(ClusterNodes.begin(), ClusterNodes.end());
          ClusterNodes.erase(std::unique(ClusterNodes.begin(), ClusterNodes.end()), ClusterNodes.end());
     }

     auto SlaveTagsList = ConfigReaderValue.GetTags("slave");
     if (!SlaveTagsList.empty())
     {
          for (const auto &SlaveTag : SlaveTagsList)
          {
               if (!SlaveTag)
               {
                    continue;
               }

               std::string Normalized;
               std::pair<std::string, std::string> Tokens;
               ReadLinkEndpoint(SlaveTag, "slave", &Normalized, &Tokens);
               SlaveNodes.push_back(Normalized);
               if (!Tokens.first.empty() || !Tokens.second.empty())
               {
                    SlavePeerTokens[Normalized] = Tokens;
               }
          }

          std::sort(SlaveNodes.begin(), SlaveNodes.end());
          SlaveNodes.erase(std::unique(SlaveNodes.begin(), SlaveNodes.end()), SlaveNodes.end());
     }

     /* Configure distributed query routing settings */

     auto DistributedSettingsTag = ConfigReaderValue.GetTag("distributed_search");

     if (DistributedSettingsTag)
     {
          DistributedSearchEnabled = DistributedSettingsTag->GetBool("enabled", DistributedSearchEnabled);

          DistributedSearchMode = DistributedSettingsTag->GetString("mode", DistributedSearchMode);

          std::transform(DistributedSearchMode.begin(), DistributedSearchMode.end(), DistributedSearchMode.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (DistributedSearchMode != "disabled" &&
              DistributedSearchMode != "local_first" &&
              DistributedSearchMode != "remote_only" &&
              DistributedSearchMode != "strict_remote")
          {
               DistributedSearchMode = "disabled";
          }

          DistributedSearchPreferLocal = DistributedSettingsTag->GetBool("prefer_local", DistributedSearchPreferLocal);

          DistributedSearchTimeoutMS = DistributedSettingsTag->GetInt("timeout_ms", DistributedSearchTimeoutMS);

          if (DistributedSearchTimeoutMS < 100)
          {
               DistributedSearchTimeoutMS = 100;
          }
          else if (DistributedSearchTimeoutMS > 60000)
          {
               DistributedSearchTimeoutMS = 60000;
          }

          DistributedPersistentTransport = DistributedSettingsTag->GetBool("persistent", DistributedPersistentTransport);
          DistributedTransportBurst = DistributedSettingsTag->GetInt("burst", DistributedTransportBurst);
          DistributedAutoReconnect = DistributedSettingsTag->GetBool("autoconnect", DistributedAutoReconnect);
          DistributedReconnectMS = DistributedSettingsTag->GetInt("reconnect_ms", DistributedReconnectMS);
          if (DistributedTransportBurst < 1)
          {
               DistributedTransportBurst = 1;
          }
          else if (DistributedTransportBurst > 1000)
          {
               DistributedTransportBurst = 1000;
          }
          if (DistributedReconnectMS < 100)
          {
               DistributedReconnectMS = 100;
          }
          else if (DistributedReconnectMS > 60000)
          {
               DistributedReconnectMS = 60000;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("serverconfig", "Loaded distributed_search: enabled=" + std::string(DistributedSearchEnabled ? "true" : "false") + ", mode=" + DistributedSearchMode + ", prefer_local=" + std::string(DistributedSearchPreferLocal ? "true" : "false") + ", timeout_ms=" + std::to_string(DistributedSearchTimeoutMS) + ", persistent=" + std::string(DistributedPersistentTransport ? "true" : "false") + ", burst=" + std::to_string(DistributedTransportBurst) + ", autoconnect=" + std::string(DistributedAutoReconnect ? "true" : "false") + ", reconnect_ms=" + std::to_string(DistributedReconnectMS) + ".");
          }
     }

     auto ReplicationSettingsTag = ConfigReaderValue.GetTag("replication");
     ReplicationEnabled = !SlaveNodes.empty();

     if (ReplicationSettingsTag)
     {
          ReplicationEnabled = ReplicationSettingsTag->GetBool("enabled", ReplicationEnabled);
          ReplicationMode = ReplicationSettingsTag->GetString("mode", ReplicationMode);

          std::transform(ReplicationMode.begin(), ReplicationMode.end(), ReplicationMode.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (ReplicationMode != "async" &&
              ReplicationMode != "sync_one" &&
              ReplicationMode != "quorum" &&
              ReplicationMode != "all")
          {
               ReplicationMode = "sync_one";
          }

          ReplicationTimeoutMS = ReplicationSettingsTag->GetInt("timeout_ms", ReplicationTimeoutMS);
          if (ReplicationTimeoutMS < 100)
          {
               ReplicationTimeoutMS = 100;
          }
          else if (ReplicationTimeoutMS > 60000)
          {
               ReplicationTimeoutMS = 60000;
          }

          ReplicationFailOnError = ReplicationSettingsTag->GetBool("fail_on_error", ReplicationFailOnError);
     }
     else if (!SlaveNodes.empty())
     {
          ReplicationMode = "sync_one";
          ReplicationTimeoutMS = 2000;
     }

     auto ReplicaModeTag = ConfigReaderValue.GetTag("replica");

     if (ReplicaModeTag)
     {
          ReplicaModeEnabled = ReplicaModeTag->GetBool("enabled", ReplicaModeEnabled);
          ReplicaAllowWrites = ReplicaModeTag->GetBool("allow_writes", ReplicaAllowWrites);
     }

     /* Do not probe remote link reachability during startup. */
}

void ServerConfig::ReportSearchAlgorithmOnce()
{
     if (!AlgorithmMessagePrinted)
     {
          ConsoleWriter::WriteStartup("Using search algorithm: " + SearchAlgorithm + ".", true);

          AlgorithmMessagePrinted = true;
     }
}

/* Returns the RocksDB options, performing lazy initialization if necessary */

const RocksDBOptions &ServerConfig::GetRocksDBOptions() const
{
     if (!RocksDBOptionsLoaded.load(std::memory_order_acquire))
     {
          std::lock_guard<std::mutex> Lock(RocksDBOptionsMutex);

          if (!RocksDBOptionsLoaded.load(std::memory_order_relaxed))
          {
               try
               {
                    if (Valid && ConfigReaderValue.IsValid())
                    {
                         auto LSMSettingsTag = ConfigReaderValue.GetTag("lsm");

                         if (LSMSettingsTag)
                         {
                              RocksDBOptionsValue = std::make_unique<RocksDBOptions>(
                                   RocksDBOptions::LoadFromConfigReader(ConfigReaderValue));

                              RocksDBOptionsLoaded.store(true, std::memory_order_release);

                              if (RocksDBOptionsValue)
                              {
                                   return *RocksDBOptionsValue;
                              }
                         }
                    }
               }
               catch (...)
               {
                    /* Suppress all exceptions and fallback to safe defaults */
               }

               RocksDBOptionsValue = std::make_unique<RocksDBOptions>(
                    RocksDBOptions::Default());

               RocksDBOptionsLoaded.store(true, std::memory_order_release);
          }
     }

     if (!RocksDBOptionsLoaded.load(std::memory_order_acquire) || !RocksDBOptionsValue)
     {
          static RocksDBOptions EmergencyDefaultOptions = RocksDBOptions::Default();

          return EmergencyDefaultOptions;
     }

     return *RocksDBOptionsValue;
}

static std::string ClusterTrimCopy(const std::string &Value)
{
     size_t Start = Value.find_first_not_of(" \t\r\n");
     if (Start == std::string::npos)
     {
          return "";
     }
     size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

static std::string NormalizeClusterEndpoint(const std::string &Raw, std::string *OutError)
{
     std::string Node = ClusterTrimCopy(Raw);

     if (Node.empty())
     {
          if (OutError)
          {
               *OutError = "Empty endpoint";
          }
          return "";
     }

     std::string Scheme;

     if (Node.rfind("http://", 0) == 0)
     {
          Scheme = "http";
          Node = Node.substr(7);
     }
     else if (Node.rfind("https://", 0) == 0)
     {
          Scheme = "https";
          Node = Node.substr(8);
     }

     size_t SlashPos = Node.find('/');

     if (SlashPos != std::string::npos)
     {
          Node = Node.substr(0, SlashPos);
     }

     std::string Host = Node;
     int Port = 9200;
     size_t ColonPos = Node.rfind(':');

     if (!Node.empty() && Node.front() == '[')
     {
          size_t BracketPos = Node.find(']');
          if (BracketPos == std::string::npos)
          {
               if (OutError)
               {
                    *OutError = "Invalid bracketed IPv6 host";
               }
               return "";
          }

          Host = Node.substr(1, BracketPos - 1);
          std::string Rest = ClusterTrimCopy(Node.substr(BracketPos + 1));

          if (!Rest.empty())
          {
               if (Rest.front() != ':')
               {
                    if (OutError)
                    {
                         *OutError = "Invalid endpoint suffix";
                    }
                    return "";
               }

               std::string PortStr = Rest.substr(1);
               if (PortStr.empty())
               {
                    if (OutError)
                    {
                         *OutError = "Invalid port";
                    }
                    return "";
               }

               auto [Ptr, EC] = std::from_chars(PortStr.data(), PortStr.data() + PortStr.size(), Port);
               if (EC != std::errc() || Ptr != PortStr.data() + PortStr.size())
               {
                    if (OutError)
                    {
                         *OutError = "Invalid port";
                    }
                    return "";
               }
          }
     }
     else if (ColonPos != std::string::npos)
     {
          if (Node.find(':') != ColonPos)
          {
               Host = Node;
          }
          else
          {
               Host = Node.substr(0, ColonPos);
               std::string PortStr = Node.substr(ColonPos + 1);
               auto [Ptr, EC] = std::from_chars(PortStr.data(), PortStr.data() + PortStr.size(), Port);
               if (EC != std::errc() || Ptr != PortStr.data() + PortStr.size())
               {
                    if (OutError)
                    {
                         *OutError = "Invalid port";
                    }
                    return "";
               }
          }
     }

     Host = ClusterTrimCopy(Host);
     if (Host.empty() || Port <= 0 || Port > 65535)
     {
          if (OutError)
          {
               *OutError = "Invalid host or port";
          }
          return "";
     }

     std::string LowerHost = Host;
     std::transform(LowerHost.begin(), LowerHost.end(), LowerHost.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     if (LowerHost == "localhost" || LowerHost == "127.0.0.1" || LowerHost == "::1" || LowerHost == "0.0.0.0")
     {
          Host = "127.0.0.1";
     }

     if (Scheme == "https")
     {
          return "https://" + Host + ":" + std::to_string(Port);
     }

     /*
      * Treat bare host:port and explicit http://host:port as the same endpoint.
      * Keep https:// distinct because transport semantics differ.
      */

     if (Host.find(':') != std::string::npos && !(Host.size() >= 2 && Host.front() == '[' && Host.back() == ']'))
     {
          return "[" + Host + "]:" + std::to_string(Port);
     }

     return Host + ":" + std::to_string(Port);
}

bool ServerConfig::AddClusterNode(const std::string &Endpoint, std::string *OutError)
{
     std::string Error;
     std::string Normalized = NormalizeClusterEndpoint(Endpoint, &Error);
     if (Normalized.empty())
     {
          if (OutError)
          {
               *OutError = Error.empty() ? "Invalid endpoint" : Error;
          }
          return false;
     }

     std::lock_guard<std::mutex> Lock(ClusterNodesMutex);

     for (const auto &Existing : ClusterNodes)
     {
          if (NormalizeClusterEndpoint(Existing, nullptr) == Normalized)
          {
               return true;
          }
     }

     ClusterNodes.push_back(Normalized);
     std::sort(ClusterNodes.begin(), ClusterNodes.end());
     ClusterNodes.erase(std::unique(ClusterNodes.begin(), ClusterNodes.end()), ClusterNodes.end());
     ClusterEnabled = true;
     return true;
}

bool ServerConfig::RemoveClusterNode(const std::string &Endpoint, std::string *OutError)
{
     std::string Error;
     std::string Normalized = NormalizeClusterEndpoint(Endpoint, &Error);
     if (Normalized.empty())
     {
          if (OutError)
          {
               *OutError = Error.empty() ? "Invalid endpoint" : Error;
          }
          return false;
     }

     std::lock_guard<std::mutex> Lock(ClusterNodesMutex);

     bool Removed = false;
     std::vector<std::string> Remaining;
     Remaining.reserve(ClusterNodes.size());

     for (const auto &Existing : ClusterNodes)
     {
          if (NormalizeClusterEndpoint(Existing, nullptr) == Normalized)
          {
               Removed = true;
               continue;
          }
          Remaining.push_back(Existing);
     }

     if (Removed)
     {
          ClusterNodes.swap(Remaining);
          ClusterPeerTokens.erase(Normalized);
          if (ClusterNodes.empty())
          {
               ClusterEnabled = false;
          }
     }

     return Removed;
}

void ServerConfig::ClearClusterNodes()
{
     std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
     ClusterNodes.clear();
     ClusterPeerTokens.clear();
     ClusterEnabled = false;
}

bool ServerConfig::GetClusterPeerTokens(const std::string &Endpoint,
                                        std::string *OutPrimaryToken,
                                        std::string *OutSecondaryToken) const
{
     std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
     auto It = ClusterPeerTokens.find(Endpoint);
     if (It == ClusterPeerTokens.end())
     {
          if (OutPrimaryToken)
          {
               OutPrimaryToken->clear();
          }
          if (OutSecondaryToken)
          {
               OutSecondaryToken->clear();
          }
          return false;
     }

     if (OutPrimaryToken)
     {
          *OutPrimaryToken = It->second.first;
     }
     if (OutSecondaryToken)
     {
          *OutSecondaryToken = It->second.second;
     }
     return true;
}

bool ServerConfig::GetSlavePeerTokens(const std::string &Endpoint,
                                      std::string *OutPrimaryToken,
                                      std::string *OutSecondaryToken) const
{
     std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
     auto It = SlavePeerTokens.find(Endpoint);

     if (It == SlavePeerTokens.end())
     {
          if (OutPrimaryToken)
          {
               OutPrimaryToken->clear();
          }

          if (OutSecondaryToken)
          {
               OutSecondaryToken->clear();
          }
          
          return false;
     }

     if (OutPrimaryToken)
     {
          *OutPrimaryToken = It->second.first;
     }

     if (OutSecondaryToken)
     {
          *OutSecondaryToken = It->second.second;
     }

     return true;
}
