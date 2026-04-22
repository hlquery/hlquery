/*
 * hlquery - Search beyond keywords.
 * Initializer Implementation
 *
 * Provides organized initialization functions for hlquery server subsystems.
 * Breaks down the large Initialize() function into logical, manageable sections.
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <unistd.h>

#include "api/httpserver.h"
#include "api/ipfilter.h"
#include "api/searchapi.h"
#include "api/userauth.h"
#include "common/actionlist.h"
#include "common/health.h"
#include "common/searchpool.h"
#include "core/config.h"
#include "runtime/daemon.h"
#include "core/modulemanager.h"
#include "vendor/json/json.hpp"

/* Static container to store background threads for joining at shutdown */

std::vector<std::thread> BackgroundThreads;

std::mutex BackgroundThreadsMutex;

#include "core/hlquery.h"
#include "core/socketengine.h"
#include "runtime/threadlimit.h"
#include "search/cstore.h"
#include "search/lindex.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"
#include "utils/tools.h"

/* Helper macro to safely call LogManager methods */

#define SAFE_LOG(Level, Type, Msg)                                           \
     do                                                                      \
     {                                                                       \
          if (Logs)                                                          \
          {                                                                  \
               LogManager *LogsPtr = Logs.get();                             \
               if (LogsPtr)                                                  \
               {                                                             \
                    try                                                      \
                    {                                                        \
                         LogManager::SafeLog(LogsPtr, Level, Type, Msg);     \
                    }                                                        \
                    catch (...)                                              \
                    {                                                        \
                         /* Silently fail - don't crash on logging errors */ \
                    }                                                        \
               }                                                             \
          }                                                                  \
     } while (0)

static void PrintStartupModuleList(const std::string &Heading, const std::vector<std::string> &ModuleNames)
{
     ConsoleWriter::WriteStartup(Heading + ":", true, false);

     if (ModuleNames.empty())
     {
          ConsoleWriter::WriteStartup("No optional modules loaded.", true, false);
          return;
     }

     for (const auto &ModuleName : ModuleNames)
     {
          ConsoleWriter::WriteStartupPlain("       - " + ModuleName, false);
     }
}
/* Handles core server logic initialization and early setup tasks */

bool hlquery::InitializeServer()
{
     /* Verify Config exists before starting initialization */

     if (!Config)
     {
          ConsoleWriter::WriteError("[FATAL] Config is null at start of InitializeServer().", true);

          return false;
     }

     /*
      * Set stdout/stderr to line-buffered mode early to ensure all messages appear immediately.
      * This prevents messages from being buffered in the output streams.
      */

     setvbuf(stdout, nullptr, _IOLBF, 0);

     setvbuf(stderr, nullptr, _IOLBF, 0);

     /* Initialize startup state tracking structures */

     {
          /* No-op, already initialized */
     }

     /*
      * Load config file early to get server ID for PID file check.
      * This allows multiple server instances to use different PID files reliably.
      */

     std::string ServerIDForPID = "001";

     if (Config)
     {
          const std::string &ConfigFileLoc = Config->GetConfigFile();

          if (!ConfigFileLoc.empty())
          {
               /* Try to load config just to extract the server ID */

               try
               {
                    if (Config->LoadConfig(ConfigFileLoc))
                    {
                         ServerIDForPID = Config->GetServerId();
                    }
                    else
                    {
                         /* If load fails, infer the server ID from the config path */

                         if (ConfigFileLoc.find("server2") != std::string::npos)
                         {
                              ServerIDForPID = "002";
                         }
                    }
               }
               catch (...)
               {
                    /* If anything fails, fallback to path inference */

                    if (ConfigFileLoc.find("server2") != std::string::npos)
                    {
                         ServerIDForPID = "002";
                    }
               }
          }
     }

     /* Check for dirty shutdown using the runtime data directory for this instance. */

     {
          std::string ShutdownMarker = RuntimePaths::ResolveRuntimeDataDir(Config.get()) + "/.clean_shutdown";

          bool CleanShutdownFlag = std::filesystem::exists(ShutdownMarker);

          if (!CleanShutdownFlag)
          {
               StatsVal.SetDirtyShutdown(true);
          }
          else
          {
               try
               {
                    std::filesystem::remove(ShutdownMarker);
               }
               catch (...)
               {
               }
          }

          StatsVal.IncrementRestartCount();
     }

     if (!CheckExistingProcessInternal())
     {
          /* Determine the PID file path based on the server ID */

          std::string PIDFileName = "hlquery.pid";

          if (!ServerIDForPID.empty() && ServerIDForPID != "001")
          {
               PIDFileName = "hlquery-" + ServerIDForPID + ".pid";
          }

          std::string PIDFilePath = std::string(HLQUERY_PID_DIR) + "/" + PIDFileName;

          std::string ExistingPIDInfo = "";

          try
          {
               if (std::filesystem::exists(PIDFilePath))
               {
                    std::ifstream PIDFileStream(PIDFilePath);

                    if (PIDFileStream.is_open())
                    {
                         std::string PIDStrValue;

                         std::getline(PIDFileStream, PIDStrValue);

                         PIDFileStream.close();

                         if (!PIDStrValue.empty())
                         {
                              ExistingPIDInfo = " (PID: " + PIDStrValue + ")";
                         }
                    }
               }
          }
          catch (...)
          {
               /* Ignore errors during PID file inspection */
          }

          ConsoleWriter::WriteError("[FATAL] CheckExistingProcessInternal() failed - another instance may be running" + ExistingPIDInfo + ".", true);
          ConsoleWriter::WriteError("[FATAL] To stop the existing instance, run: ./etc/scripts/debug_daemon.sh stop.", true);
          ConsoleWriter::WriteError("[FATAL] Or: sudo ./run/bin/hlquery --forcestop.", true);

          /* Allow bypassing checks in debug/nofork mode for development flexibility */

          if (Config && Config->GetDebugMode() && Config->GetNoForkMode())
          {
               ConsoleWriter::WriteWarning("[WARNING] Debug mode detected - you may want to stop the existing instance first.", true);
               ConsoleWriter::WriteWarning("[WARNING] Continuing anyway may cause conflicts...", true);
          }

          return false;
     }

     /* Initialize logging using LogManager's factory method */

     Logs = LogManager::CreateAndInitialize(Config.get());

     if (!Logs)
     {
          ConsoleWriter::WriteError("[FATAL] LogManager::CreateAndInitialize() failed.", true);

          return false;
     }

     if (Logs->GetLogCount() > 0)
     {
          print_ok("Opened {} logs", Logs->GetLogCount());
     }
     else
     {
          print_ok("No logs opened.");
     }

     if (Config)
     {
          Config->ReportSearchAlgorithmOnce();
     }

     if (!InitializeCoreSystems())
     {
          ConsoleWriter::WriteError("[FATAL] InitializeCoreSystems() failed.", true);

          return false;
     }

     /* Initialize process threading limits and management */

     if (!ThreadLimit::Initialize(Config.get()))
     {
          ConsoleWriter::WriteError("[FATAL] ThreadLimit::Initialize() failed.", true);

          return false;
     }

     /*
     * Initialize the socket engine before starting any higher-level APIs.
     */

     try
     {
          SocketEngine::Init();
          print_ok("Using socket engine: {}.", SOCKETENGINE_NAME);
     }
     catch (...)
     {
          /* Continue even if socket engine initialization fails; higher levels will retry */
     }

     /* Initialize the Hybrid Storage Manager and storage engine */

     if (!HybridStorageManager::GetInstance().Start())
     {
          ConsoleWriter::WriteError("[FATAL] Failed to start HybridStorageManager!", false);
          SetShutdownFlag();

          return false;
     }

     /* Initialize the primary Search API and authentication handlers */

     API = &SearchAPI::GetInstance();

     if (!API->Start())
     {
          ConsoleWriter::WriteError("[FATAL] Failed to start SearchAPI!", false);
          SetShutdownFlag();

          return false;
     }

     Modules = std::make_unique<ModuleManager>();

     if (!Modules)
     {
          if (Logs)
          {
               Logs->Normal("modules", "Failed to allocate ModuleManager.");
          }

          ConsoleWriter::WriteError("[FATAL] Failed to create ModuleManager.", true);
          SetShutdownFlag();
          return false;
     }

     std::string ModuleError;

     if (!Modules->LoadModules(*Config, Logs.get(), ModuleError))
     {
          if (Logs)
          {
               Logs->Normal("modules", "Module loading failed: " + ModuleError + ".");
          }

          ConsoleWriter::WriteError("[FATAL] " + ModuleError, true);
          SetShutdownFlag();
          return false;
     }

     PrintStartupModuleList("Loaded optional modules", Modules->GetLoadedOptionalModuleNames());
     PrintStartupModuleList("Loaded core modules", Modules->GetLoadedCoreModuleNames());

     /* Handle special validation modes if requested via configuration or flags */

     {
          std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

          if (StatsVal.StartupStateInfo.ValidationMode)
          {
               if (Logs)
               {
                    Logs->Normal("hlquery", "Validation mode: Validating data directory.");
               }

               /* Execute data directory validation checks */

               bool ValidationPassedFlag = true;

               std::string ValidationErrorsMsg;

               std::string DataDirPath = std::string(HLQUERY_DATA_DIR);

               if (!std::filesystem::exists(DataDirPath))
               {
                    ValidationPassedFlag = false;

                    ValidationErrorsMsg += "Data directory does not exist: " + DataDirPath + "\n";
               }
               else if (!std::filesystem::is_directory(DataDirPath))
               {
                    ValidationPassedFlag = false;

                    ValidationErrorsMsg += "Data directory is not a directory: " + DataDirPath + "\n";
               }

               std::string CollectionsDirPath = DataDirPath + "/collections";

               if (std::filesystem::exists(CollectionsDirPath) && !std::filesystem::is_directory(CollectionsDirPath))
               {
                    ValidationPassedFlag = false;

                    ValidationErrorsMsg += "Collections path is not a directory: " + CollectionsDirPath + "\n";
               }

               if (ValidationPassedFlag)
               {
                    if (Logs)
                    {
                         Logs->Normal("hlquery", "Validation mode: Data directory validation PASSED.");
                    }

                    if (Logs)
                    {
                         Logs->Normal("hlquery", "Validation mode: Data directory is valid.");
                    }

                    return true;
               }
               else
               {
                    std::string ErrorMsgValueFinal = "Validation mode: Data directory validation FAILED:\n" + ValidationErrorsMsg;

                    ConsoleWriter::WriteInitCriticalSafe(STDERR_FILENO, ErrorMsgValueFinal.c_str());

                    if (Logs)
                    {
                         Logs->Critical("hlquery", "Validation mode: Data directory validation FAILED:\n" + ValidationErrorsMsg + ".");
                    }

                    return false;
               }
          }
     }

     if (!InitializeNoForkMode())
     {
          ConsoleWriter::WriteError("[FATAL] InitializeNoForkMode() failed.", true);

          return false;
     }

     if (Config && Config->GetDebugMode() && !Config->GetVerboseMode())
     {
          print_ok("Running with debug logging (--debug)");
     }

     DisplayBindingInfo();

     DisplaySSLInfo();

     /* Print the primary server ready message */

     print_ok("Now up and running");

     if (Config && Config->GetNoForkMode())
     {
          print_ok("Running in foreground (--nofork)");
     }

     /*
     * Execute robust flushes to ensure all binding information is visible.
     */

     fflush(stdout);
     fflush(stderr);
     std::cout.flush();
     std::cerr.flush();
     fflush(NULL);

     /*
     * Transition the process into its final operational state (e.g. backgrounding).
     */

     CompleteDaemonSetup();

     return true;
}

/* Verifies that another instance of the server is not already executing */

bool hlquery::CheckExistingProcessInternal()
{
     if (hlquery::CheckExistingProcess())
     {
          return false;
     }

     return true;
}

/* Initializes core server systems including timers and metrics tracking */

bool hlquery::InitializeCoreSystems()
{
     Timers = std::make_unique<TimerManager>();

     if (Logs)
     {
          Logs->Debug("hlquery", "TimerManager created.");
     }

     if (Logs)
     {
          Logs->Normal("startup", "TimerManager initialized.");
     }

     if (Logs)
     {
          Logs->Debug("hlquery", "Metrics storage initialized.");
     }

     if (Logs)
     {
          Logs->Normal("startup", "Metrics storage initialized.");
     }

     /* Initialize and configure the IP allow filter */

     IPFilter = std::make_unique<class IPFilter>();

     if (Config)
     {
          IPFilter->SetDNSCacheMaxSize(Config->GetDNSCacheMaxSize());

          std::string AllowedIPsList = Config->GetIPAllow();
          std::string DeniedIPsList = Config->GetIPDeny();

          if (!IPFilter->Initialize(AllowedIPsList, DeniedIPsList))
          {
               if (Logs)
               {
                    Logs->Normal("ip_allow", "Failed to initialize IP allow filter, defaulting to deny all.");
               }

               IPFilter->Initialize("", "");
          }
          else
          {
               if (Logs)
               {
                    Logs->Normal("ip_allow", "IP allow filter initialized successfully.");
               }
          }
     }
     else
     {
          IPFilter->Initialize("", "");
     }

     /* Print and log IP filtering configuration state */

     if (IPFilter)
     {
          if (IPFilter->IsEnabled())
          {
               auto OriginalEntriesList = IPFilter->GetOriginalEntries();

               auto ResolvedIPsList = IPFilter->GetAllowedIPs();

               auto DeniedEntriesList = IPFilter->GetDeniedEntries();

               auto DeniedResolvedList = IPFilter->GetDeniedIPs();
               bool HasHostDeny = (Config && Config->HasHostDeny());

               if (!OriginalEntriesList.empty())
               {
                    std::string IPListStr = "Allowing connections from: ";

                    for (size_t i = 0; i < OriginalEntriesList.size(); ++i)
                    {
                         if (i > 0)
                         {
                              IPListStr += ", ";
                         }

                         IPListStr += OriginalEntriesList[i];
                    }

                    /* Show resolved IPs if they differ from the original input */

                    if (ResolvedIPsList.size() > OriginalEntriesList.size())
                    {
                         IPListStr += " (resolved: ";

                         for (size_t i = 0; i < ResolvedIPsList.size(); ++i)
                         {
                              if (i > 0)
                              {
                                   IPListStr += ", ";
                              }

                              IPListStr += ResolvedIPsList[i];
                         }

                         IPListStr += ")";
                    }

                    if (Logs)
                    {
                         Logs->Normal("startup", IPListStr + ".");
                    }

                    print_ok_nd("Allowing connections from:");

                    for (const auto &Entry : OriginalEntriesList)
                    {
                         std::string EntryLine = "       - " + Entry;
                         ConsoleWriter::WriteStartupPlainSafe(STDOUT_FILENO, EntryLine.c_str(), false);
                    }
               }

               if (HasHostDeny && !DeniedEntriesList.empty())
               {
                    std::string DenyListStr = "Denying connections from: ";

                    for (size_t i = 0; i < DeniedEntriesList.size(); ++i)
                    {
                         if (i > 0)
                         {
                              DenyListStr += ", ";
                         }

                         DenyListStr += DeniedEntriesList[i];
                    }

                    if (DeniedResolvedList.size() > DeniedEntriesList.size())
                    {
                         DenyListStr += " (resolved: ";

                         for (size_t i = 0; i < DeniedResolvedList.size(); ++i)
                         {
                              if (i > 0)
                              {
                                   DenyListStr += ", ";
                              }

                              DenyListStr += DeniedResolvedList[i];
                         }

                         DenyListStr += ")";
                    }

                    if (Logs)
                    {
                         Logs->Normal("startup", DenyListStr + ".");
                    }

                    print_ok_nd("Denying connections from:");

                    for (const auto &Entry : DeniedEntriesList)
                    {
                         std::string EntryLine = "       - " + Entry;
                         ConsoleWriter::WriteStartupPlainSafe(STDOUT_FILENO, EntryLine.c_str(), false);
                    }
               }
          }
          else if (IPFilter->IsEnabled())
          {
               /* Reject all connections if enabled but empty */

               if (Logs)
               {
                    Logs->Normal("startup", "Allowing connections from: (none - deny all).");
               }

               print_ok_nd("Allowing connections from:");
               ConsoleWriter::WriteStartupPlainSafe(STDOUT_FILENO, "       - (none - deny all)", false);
          }
          else
          {
               /* Accept all connections if the filter is disabled */

               if (Logs)
               {
                    Logs->Normal("startup", "Allowing connections from: * (all IPs).");
               }

               print_ok_nd("Allowing connections from:");
               ConsoleWriter::WriteStartupPlainSafe(STDOUT_FILENO, "       - * (all IPs)", false);
          }
     }

     return true;
}

/* Displays detailed network binding information during startup */

void hlquery::DisplayBindingInfo()
{
     const auto &StartupBindsList = Config->GetBindConfigs();

     print_ok_nd("Server binding to:");

     for (const auto &BindConfigInstance : StartupBindsList)
     {
          std::string BindLine;

          if (BindConfigInstance.type == "http" || BindConfigInstance.type == "https")
          {
               BindLine = "       - " + BindConfigInstance.type + "://" + BindConfigInstance.address + ":" + std::to_string(BindConfigInstance.port);
          }
          else
          {
               BindLine = "       - " + BindConfigInstance.address + ":" + std::to_string(BindConfigInstance.port) + " (" + BindConfigInstance.type + ")";
          }

          ConsoleWriter::WriteStartupPlain(BindLine, false);
     }
}

/* Displays SSL details during startup (only when SSL is actually configured) */

void hlquery::DisplaySSLInfo()
{
     const auto &StartupBindsList = Config->GetBindConfigs();

     bool HasSSL = false;

     for (const auto &BindConfigInstance : StartupBindsList)
     {
          if (BindConfigInstance.ssl)
          {
               HasSSL = true;
               break;
          }
     }

     if (!HasSSL)
     {
          return;
     }

     print_ok_nd("SSL loaded:");

     for (const auto &BindConfigInstance : StartupBindsList)
     {
          if (!BindConfigInstance.ssl)
          {
               continue;
          }

          std::string CertName = BindConfigInstance.ssl_cert.empty()
                                      ? "<unset>"
                                      : std::filesystem::path(BindConfigInstance.ssl_cert).filename().string();

          std::string KeyName = BindConfigInstance.ssl_key.empty()
                                     ? "<unset>"
                                     : std::filesystem::path(BindConfigInstance.ssl_key).filename().string();

          std::string BindLine = "       - " + std::to_string(BindConfigInstance.port) +
                                 " (cert=" + CertName +
                                 ", key=" + KeyName;

          if (!BindConfigInstance.ssl_protocols.empty())
          {
               BindLine += ", protocols=" + BindConfigInstance.ssl_protocols;
          }

          BindLine += ")";

          ConsoleWriter::WriteStartupPlain(BindLine, false);
     }
}

/* Initializes the server in no-fork (foreground) mode */

bool hlquery::InitializeNoForkMode()
{
     const auto &BindConfigsList = Config->GetBindConfigs();

     /* Initialize shared thread pools before creating HTTP servers. */

     ThreadPools = &ThreadPoolManager::GetInstance();

     if (!ThreadPools->Initialize())
     {
          if (Logs)
          {
               Logs->Critical("hlquery", "Failed to initialize thread pools.");
          }
          ConsoleWriter::WriteError("[FATAL] Failed to initialize thread pools.", true);

          SetShutdownFlag();

          return false;
     }

     if (Logs)
     {
          Logs->Normal("hlquery", "Thread pools initialized successfully.");
     }

     /* Locate the primary HTTP/HTTPS bind configurations */

     for (const auto &BindConfigValueInstance : BindConfigsList)
     {
          if (BindConfigValueInstance.type == "http" || BindConfigValueInstance.type == "https")
          {
               if (Logs)
               {
                    Logs->Debug("hlquery", "HTTP/HTTPS server will start on " + BindConfigValueInstance.address + ":" + std::to_string(BindConfigValueInstance.port) + " (" + BindConfigValueInstance.type + ").");
               }

               HttpServer *new_server = nullptr;
            
               if (!InitializeHttpServer(BindConfigValueInstance, new_server, Logs.get()))
               {
                    SetShutdownFlag();
                    return false;
               }

               if (new_server)
               {
                    new_server->SetLoading(true);
                    new_server->SetReadyToAccept(true);
                    HTTPServers.push_back(new_server);
               }
          }
     }

     if (HTTPServers.empty())
     {
          if (Logs)
          {
               Logs->Critical("hlquery", "No HTTP/HTTPS bind configurations found!");
          }
          ConsoleWriter::WriteError("[FATAL] No HTTP/HTTPS bind configurations found! Check your configuration file.", true);
          SetShutdownFlag();
          return false;
     }

     NOTIFY_MODULES(OnThreadPoolsReady);

     /* Synchronize with background metadata scan before finishing setup */

     WaitForMetadataScan();

     return true;
}

/* Handles post-fork setup tasks for both daemon and foreground modes */

void hlquery::SetupPostFork()
{
     if (Config && Config->GetNoForkMode())
     {
          try
          {
               SetupFileDescriptors();
          }
          catch (...)
          {
               /* Continue anyway; core systems will retry if FD setup was incomplete */
          }
     }

     try
     {
          SetupSignalHandlers();
     }
     catch (...)
     {
          /* Log failure but continue execution */
     }
}

/* Waits for the initial metadata scan to complete before allowing query processing */

void hlquery::WaitForMetadataScan()
{
     if (HTTPServers.empty())
     {
          return;
     }

     /* Record the exact time the metadata scan synchronization begins */

     {
          std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

          StatsVal.StartupStateInfo.MetadataScanStart = Instance ? Instance->Now() : std::chrono::steady_clock::now();
     }

     /* Inspect and apply strict startup configuration flags */

     bool StrictStartupFlagValue = false;

     if (Config)
     {
          const CommandLine &CmdLineArgsValue = Config->GetCommandLine();

          for (int i = 0; i < CmdLineArgsValue.argc; i++)
          {
               if (CmdLineArgsValue.argv && CmdLineArgsValue.argv[i])
               {
                    std::string ArgumentStrVal = CmdLineArgsValue.argv[i];

                    if (ArgumentStrVal == "--strict-startup")
                    {
                         StrictStartupFlagValue = true;

                         {
                              std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                              StatsVal.StartupStateInfo.StrictStartupMode = true;
                         }

                         if (Logs)
                         {
                              Logs->Normal("hlquery", "Strict startup mode enabled - server will not accept queries if startup fails.");
                         }

                         break;
                    }
                    else if (ArgumentStrVal == "--readonly")
                    {
                         {
                              std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                              StatsVal.StartupStateInfo.ReadonlyMode = true;
                         }

                         if (Logs)
                         {
                              Logs->Normal("hlquery", "Readonly mode enabled - server will reject writes until integrity checks complete.");
                         }
                    }
                    else if (ArgumentStrVal == "--validate-startup")
                    {
                         {
                              std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                              StatsVal.StartupStateInfo.ValidationMode = true;
                         }

                         if (Logs)
                         {
                              Logs->Normal("hlquery", "Startup validation mode enabled - will validate data directory then exit.");
                         }
                    }
               }
          }
     }

     /* Check if the metadata scan has already concluded */

     if (HybridStorageManagerInstance().IsMetadataScanComplete())
     {
          {
               std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

               StatsVal.StartupStateInfo.MetadataScanEnd = Instance ? Instance->Now() : std::chrono::steady_clock::now();

               StatsVal.StartupStateInfo.MetadataScanComplete = true;
          }

          if (Logs)
          {
               Logs->Normal("hlquery", "Metadata scan already complete - attempting to load collections.");
          }

          bool CollectionsLoadedFlagFinal = false;

          {
               std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

               StatsVal.StartupStateInfo.CollectionLoadStart = Instance ? Instance->Now() : std::chrono::steady_clock::now();
          }

          try
          {
               /* Synchronously load collections from the storage engine */

               if (Logs)
               {
                    Logs->Normal("hlquery", "Loading all collections from LSM (this may take a while).");
               }

               CollectionsLoadedFlagFinal = HybridStorageManagerInstance().LoadCollectionsFromRocksDB();

               /* Force a counter update even if primary loading failed */

               if (!CollectionsLoadedFlagFinal)
               {
                    if (Logs)
                    {
                         Logs->Normal("hlquery", "LoadCollectionsFromRocksDB failed - forcing counter update to ensure accuracy.");
                    }

                    HybridStorageManagerInstance().UpdateCollectionCounters(true);
               }

               {
                    std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                    StatsVal.StartupStateInfo.CollectionLoadEnd = Instance ? Instance->Now() : std::chrono::steady_clock::now();

                    StatsVal.StartupStateInfo.CollectionsLoaded = CollectionsLoadedFlagFinal;

                    if (!CollectionsLoadedFlagFinal)
                    {
                         StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                         StatsVal.StartupStateInfo.CollectionLoadError = "LoadCollectionsFromRocksDB returned false.";
                    }
               }

               if (Logs)
               {
                    if (CollectionsLoadedFlagFinal)
                    {
                         Logs->Normal("hlquery", "Collections loaded successfully - HTTP server ready.");
                    }
                    else
                    {
                         Logs->Normal("hlquery", "Collection loading returned false - HTTP server enabled anyway.");

                         if (StrictStartupFlagValue)
                         {
                              Logs->Critical("hlquery", "STRICT STARTUP: Collection loading failed - server will not accept queries.");

                              Instance->StatsVal.SetHealthDegraded(true, "Collection loading failed in strict startup mode");
                         }
                         else
                         {
                              Instance->StatsVal.SetHealthDegraded(true, "Collection loading returned false - lazy loading fallback");
                         }
                    }
               }
          }
          catch (const std::exception &e)
          {
               {
                    std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                    StatsVal.StartupStateInfo.CollectionLoadEnd = Instance ? Instance->Now() : std::chrono::steady_clock::now();

                    StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                    StatsVal.StartupStateInfo.CollectionLoadError = std::string("Exception: ") + e.what() + ".";
               }

               if (Logs)
               {
                    Logs->Critical("hlquery",
                                   "Exception loading collections: " + std::string(e.what()) +
                                        " - HTTP server enabled but collections may not be fully loaded.");
               }

               CollectionsLoadedFlagFinal = false;

               Instance->StatsVal.SetHealthDegraded(true, "Exception during collection loading: " + std::string(e.what()));
          }
          catch (...)
          {
               {
                    std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

                    StatsVal.StartupStateInfo.CollectionLoadEnd = Instance ? Instance->Now() : std::chrono::steady_clock::now();

                    StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                    StatsVal.StartupStateInfo.CollectionLoadError = "Unknown exception.";
               }

               if (Logs)
               {
                    Logs->Critical("hlquery",
                                   "Unknown exception loading collections - HTTP server enabled but collections may not be fully loaded.");
               }

               CollectionsLoadedFlagFinal = false;

               Instance->StatsVal.SetHealthDegraded(true, "Unknown exception during collection loading");
          }

          /* Reject queries if strict startup was requested but loading failed */

          if (StrictStartupFlagValue && !CollectionsLoadedFlagFinal)
          {
               if (Logs)
               {
                    Logs->Critical("hlquery", "STRICT STARTUP: Collection loading failed - server will not accept queries.");
               }

               return;
          }

          /* Transition server into active query mode if loading was successful */

          if (CollectionsLoadedFlagFinal)
          {
               for (auto *server : HTTPServers)
               {
                    server->SetLoading(false);
               }

               if (Logs)
               {
                    Logs->Normal("hlquery", "Collections loaded successfully - queries enabled.");
               }
          }
          else
          {
               if (Logs)
               {
                    Logs->Normal("hlquery", "Collection loading failed - queries will remain blocked until collections load.");
               }
          }

          for (auto *server : HTTPServers)
          {
               server->SetReadyToAccept(true);
          }

          {
               std::lock_guard<std::mutex> Lock(StatsVal.StartupStateMutex);

               StatsVal.StartupStateInfo.ReadyTime = Instance ? Instance->Now() : std::chrono::steady_clock::now();
          }

          return;
     }

     bool RegisteredStatusVal = false;

     /* Attempt to provision a background thread for monitoring the scan status */

     if (ThreadLimit::GetCurrentThreadCount() < ThreadLimit::GetMaxThreads())
     {
          ThreadLimit::IncrementThreadCount();

          RegisteredStatusVal = true;
     }

     if (RegisteredStatusVal)
     {
          /* Start background thread to await scan completion without blocking main thread */

          std::thread WaitThreadInstance([]()
                                         {
                                              if (Instance && Instance->Logs)
                                              {
                                                   Instance->Logs->Normal("hlquery", "Waiting for metadata scan to complete before accepting requests.");
                                              }

                                              const int MaxWaitSecondsCount = METADATA_SCAN_MAX_WAIT_SECONDS;

                                              const int SleepMSValueCount = METADATA_SCAN_SLEEP_MS;

                                              int WaitedMSValueCount = 0;

                                              int LogIntervalMSCount = 5000;

                                              int LastLoggedMSValueCount = 0;

                                              if (Instance && Instance->Logs)
                                              {
                                                   Instance->Logs->Normal("hlquery", "Waiting for metadata scan (max wait: " + std::to_string(MaxWaitSecondsCount) + "s).");
                                              }

                                              while (!HybridStorageManagerInstance().IsMetadataScanComplete() &&
                                                     WaitedMSValueCount < MaxWaitSecondsCount * 1000)
                                              {
                                                   std::this_thread::sleep_for(std::chrono::milliseconds(SleepMSValueCount));

                                                   WaitedMSValueCount += SleepMSValueCount;

                                                   if (WaitedMSValueCount - LastLoggedMSValueCount >= LogIntervalMSCount)
                                                   {
                                                        LastLoggedMSValueCount = WaitedMSValueCount;

                                                        int RemainingSecondsLeft = (MaxWaitSecondsCount * 1000 - WaitedMSValueCount) / 1000;

                                                        if (Instance && Instance->Logs)
                                                        {
                                                             Instance->Logs->Normal("hlquery", "Still waiting for metadata scan (" + std::to_string(WaitedMSValueCount / 1000) + "s elapsed, " + std::to_string(RemainingSecondsLeft) + "s remaining).");
                                                        }
                                                   }
                                              }

                                              {
                                                   std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                   Instance->StatsVal.StartupStateInfo.MetadataScanEnd = Instance->Now();

                                                   Instance->StatsVal.StartupStateInfo.MetadataScanComplete =
                                                        HybridStorageManagerInstance().IsMetadataScanComplete();
                                              }

                                              if (!HybridStorageManagerInstance().IsMetadataScanComplete())
                                              {
                                                   if (Instance && Instance->Logs)
                                                   {
                                                        Instance->Logs->Critical("hlquery",
                                                                                 "Metadata scan wait timeout after " + std::to_string(MaxWaitSecondsCount) +
                                                                                      " seconds - server may have incomplete metadata.");

                                                        Instance->StatsVal.SetHealthDegraded(true, "Metadata scan timeout after " + std::to_string(MaxWaitSecondsCount) + "s");
                                                   }
                                              }

                                              bool MetadataCompleteStatus = HybridStorageManagerInstance().IsMetadataScanComplete();

                                              {
                                                   std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                   Instance->StatsVal.StartupStateInfo.SyncStart = Instance->Now();
                                              }

                                              bool SyncCompleteStatusFlag = true;

                                              if (Instance)
                                              {
                                                   SyncCompleteStatusFlag = !Instance->IsSyncInProgress();
                                              }

                                              /* Attempt to force sync lock release if it appears stalled */

                                              if (!SyncCompleteStatusFlag && Instance)
                                              {
                                                   if (Instance->Logs)
                                                   {
                                                        Instance->Logs->Normal("hlquery", "Sync appears to be in progress from previous session - clearing sync lock to allow server to start.");
                                                   }

                                                   Instance->SetSyncInProgress(false);

                                                   SyncCompleteStatusFlag = true;
                                              }

                                              int SyncWaitMSTracker = 0;

                                              const int MaxSyncWaitMSTracker = 5000;

                                              while (!SyncCompleteStatusFlag && SyncWaitMSTracker < MaxSyncWaitMSTracker)
                                              {
                                                   std::this_thread::sleep_for(std::chrono::milliseconds(100));

                                                   SyncWaitMSTracker += 100;

                                                   if (!Instance)
                                                   {
                                                        break;
                                                   }

                                                   SyncCompleteStatusFlag = !Instance->IsSyncInProgress();
                                              }

                                              if (!SyncCompleteStatusFlag && Instance)
                                              {
                                                   if (Instance->Logs)
                                                   {
                                                        Instance->Logs->Normal("hlquery", "Sync timeout after " + std::to_string(MaxSyncWaitMSTracker) + "ms - forcing sync complete to allow server to start.");
                                                   }

                                                   Instance->SetSyncInProgress(false);

                                                   SyncCompleteStatusFlag = true;
                                              }

                                              {
                                                   std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                   Instance->StatsVal.StartupStateInfo.SyncEnd = Instance->Now();

                                                   Instance->StatsVal.StartupStateInfo.SyncComplete = SyncCompleteStatusFlag;

                                                   if (!SyncCompleteStatusFlag)
                                                   {
                                                        Instance->StatsVal.StartupStateInfo.SyncError = "Sync timeout after " + std::to_string(MaxSyncWaitMSTracker) + "ms";
                                                   }
                                              }

                                              if (!SyncCompleteStatusFlag && Instance)
                                              {
                                                   SyncCompleteStatusFlag = true;

                                                   {
                                                        std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                        Instance->StatsVal.StartupStateInfo.SyncComplete = true;
                                                   }
                                              }

                                              /* Monitor system health based on initialization status */

                                              if (!MetadataCompleteStatus || !SyncCompleteStatusFlag)
                                              {
                                                   std::string ReasonValueMsg = "";

                                                   if (!MetadataCompleteStatus)
                                                   {
                                                        ReasonValueMsg += "metadata scan incomplete";
                                                   }

                                                   if (!SyncCompleteStatusFlag)
                                                   {
                                                        if (!ReasonValueMsg.empty())
                                                        {
                                                             ReasonValueMsg += ", ";
                                                        }

                                                        ReasonValueMsg += "sync incomplete";
                                                   }

                                                   if (Instance)
                                                   {
                                                        Instance->StatsVal.SetHealthDegraded(true, ReasonValueMsg);
                                                   }
                                              }

                                              bool CollectionsAlreadyLoadedStatusFlag = false;

                                              if (Instance && !Instance->HTTPServers.empty())
                                              {
                                                   bool all_not_loading = true;
                                                   for (auto *server : Instance->HTTPServers)
                                                   {
                                                        if (server->IsLoading())
                                                        {
                                                             all_not_loading = false;
                                                             break;
                                                        }
                                                   }

                                                   if (all_not_loading)
                                                   {
                                                        try
                                                        {
                                                             auto ExistingCollectionsMap = HybridStorageManagerInstance().ListCollections();

                                                             if (!ExistingCollectionsMap.empty())
                                                             {
                                                                  CollectionsAlreadyLoadedStatusFlag = true;

                                                                  if (Instance->Logs)
                                                                  {
                                                                       Instance->Logs->Normal("hlquery", "Collections already loaded (" + std::to_string(ExistingCollectionsMap.size()) + " collections) - skipping background loading in WaitForMetadataScan().");
                                                                  }
                                                             }
                                                        }
                                                        catch (...)
                                                        {
                                                             /* Ignore errors during status check */
                                                        }
                                                   }
                                              }

                                              if (!CollectionsAlreadyLoadedStatusFlag && MetadataCompleteStatus && SyncCompleteStatusFlag && Instance && Instance->Database)
                                              {
                                                   if (Instance && Instance->Logs)
                                                   {
                                                        Instance->Logs->Normal("hlquery", "Metadata scan and database sync completed - loading collections in background (collection creation allowed).");
                                                   }

                                                   {
                                                        std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                        Instance->StatsVal.StartupStateInfo.CollectionLoadStart = Instance->Now();
                                                   }

                                                   std::thread LoadThreadInstanceFinal([]()
                                                                                       {
                                                                                            bool InternalLoadedStatusFlag = false;

                                                                                            std::vector<std::string> FailedCollectionsListFinal;

                                                                                            try
                                                                                            {
                                                                                                 if (Instance && Instance->Logs)
                                                                                                 {
                                                                                                      Instance->Logs->Normal("hlquery", "Starting collection loading from LSM (queries blocked until complete).");
                                                                                                 }

                                                                                                 InternalLoadedStatusFlag = HybridStorageManagerInstance().LoadCollectionsFromRocksDB();

                                                                                                 size_t CollectionCountFinalValue = 0;

                                                                                                 if (Instance && Instance->Database && InternalLoadedStatusFlag)
                                                                                                 {
                                                                                                      try
                                                                                                      {
                                                                                                           auto CurrentCollectionsFinalList = HybridStorageManagerInstance().ListCollections();

                                                                                                           CollectionCountFinalValue = CurrentCollectionsFinalList.size();
                                                                                                      }
                                                                                                      catch (...)
                                                                                                      {
                                                                                                           /* Ignore counting errors */
                                                                                                      }
                                                                                                 }

                                                                                                 {
                                                                                                      std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionLoadEnd = Instance->Now();

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionsLoaded = InternalLoadedStatusFlag;

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionsLoadedCount = CollectionCountFinalValue;

                                                                                                      Instance->StatsVal.StartupStateInfo.FailedCollections = FailedCollectionsListFinal;

                                                                                                      if (!InternalLoadedStatusFlag)
                                                                                                      {
                                                                                                           Instance->StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                                                                                                           Instance->StatsVal.StartupStateInfo.CollectionLoadError = "LoadCollectionsFromRocksDB returned false";
                                                                                                      }
                                                                                                 }

                                                                                                 if (Instance && Instance->Logs)
                                                                                                 {
                                                                                                      if (InternalLoadedStatusFlag)
                                                                                                      {
                                                                                                           Instance->Logs->Normal("hlquery", "Collections loaded successfully (" + std::to_string(CollectionCountFinalValue) + " collections found) - queries now allowed.");
                                                                                                      }
                                                                                                      else
                                                                                                      {
                                                                                                           Instance->Logs->Normal("hlquery", "Collection loading returned false - queries enabled but collections may not be fully loaded.");
                                                                                                      }
                                                                                                 }

                                                                                                 if (!InternalLoadedStatusFlag)
                                                                                                 {
                                                                                                      if (Instance)
                                                                                                      {
                                                                                                           Instance->StatsVal.SetHealthDegraded(true, "Collection loading failed - lazy loading fallback");

                                                                                                           {
                                                                                                                std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                                                                                Instance->StatsVal.StartupStateInfo.LazyLoadingFallback = true;
                                                                                                           }

                                                                                                           if (Instance->Logs)
                                                                                                           {
                                                                                                                Instance->Logs->Normal("hlquery", "Lazy loading fallback mode: Collections will load on first access.");
                                                                                                           }
                                                                                                      }
                                                                                                 }

                                                                                                 if (Instance && !Instance->HTTPServers.empty())
                                                                                                 {
                                                                                                      for (auto *server : Instance->HTTPServers)
                                                                                                      {
                                                                                                           server->SetLoading(false);
                                                                                                      }

                                                                                                      {
                                                                                                           std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                                                                           Instance->StatsVal.StartupStateInfo.ReadyTime = Instance->Now();
                                                                                                      }

                                                                                                      if (Instance->Logs)
                                                                                                      {
                                                                                                           auto StateInfoFinalVal = Instance->StatsVal.GetStartupState();

                                                                                                           int64_t TimeToReadyFinalVal = 0;

                                                                                                           if (StateInfoFinalVal.ReadyTime.time_since_epoch().count() > 0)
                                                                                                           {
                                                                                                                TimeToReadyFinalVal = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                                                                           StateInfoFinalVal.ReadyTime - StateInfoFinalVal.StartTime)
                                                                                                                                           .count();
                                                                                                           }

                                                                                                           std::string StartupSummaryFinalMsg = "STARTUP_COMPLETE: ready_ms=" + std::to_string(TimeToReadyFinalVal) +
                                                                                                                                                " collections=" + std::to_string(CollectionCountFinalValue) +
                                                                                                                                                " restart_count=" + std::to_string(Instance->StatsVal.GetRestartCount()) +
                                                                                                                                                (Instance->StatsVal.IsDirtyShutdown() ? " dirty_shutdown=yes" : " dirty_shutdown=no") +
                                                                                                                                                (InternalLoadedStatusFlag ? " status=ok" : " status=degraded");

                                                                                                           Instance->Logs->Normal("hlquery", StartupSummaryFinalMsg + ".");

                                                                                                           Instance->Logs->Normal("hlquery", "Collection loading completed (" + std::to_string(CollectionCountFinalValue) + " collections) - HTTP servers now ready to accept queries.");
                                                                                                      }
                                                                                                 }
                                                                                            }
                                                                                            catch (const std::exception &e)
                                                                                            {
                                                                                                 {
                                                                                                      std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionLoadEnd = Instance->Now();

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionLoadError = std::string("Exception: ") + e.what();

                                                                                                      Instance->StatsVal.StartupStateInfo.FailedCollections = FailedCollectionsListFinal;
                                                                                                 }

                                                                                                 if (Instance && Instance->Logs)
                                                                                                 {
                                                                                                      Instance->Logs->Critical("hlquery",
                                                                                                                               "Exception loading collections: " + std::string(e.what()) +
                                                                                                                                    " - Queries enabled but collections may not be fully loaded.");
                                                                                                 }

                                                                                                 if (Instance)
                                                                                                 {
                                                                                                      Instance->StatsVal.SetHealthDegraded(true, "Exception during collection loading: " + std::string(e.what()));
                                                                                                 }

                                                                                                 if (Instance && !Instance->HTTPServers.empty() && Instance->Logs)
                                                                                                 {
                                                                                                      Instance->Logs->Normal("hlquery", "Collection loading exception - queries remain blocked, collections will load on-demand.");
                                                                                                 }
                                                                                            }
                                                                                            catch (...)
                                                                                            {
                                                                                                 {
                                                                                                      std::lock_guard<std::mutex> Lock(Instance->StatsVal.StartupStateMutex);

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionLoadEnd = Instance->Now();

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionsLoadFailed = true;

                                                                                                      Instance->StatsVal.StartupStateInfo.CollectionLoadError = "Unknown exception";

                                                                                                      Instance->StatsVal.StartupStateInfo.FailedCollections = FailedCollectionsListFinal;
                                                                                                 }

                                                                                                 if (Instance && Instance->Logs)
                                                                                                 {
                                                                                                      Instance->Logs->Critical("hlquery",
                                                                                                                               "Unknown exception loading collections - Queries enabled but collections may not be fully loaded.");
                                                                                                 }

                                                                                                 if (Instance)
                                                                                                 {
                                                                                                      Instance->StatsVal.SetHealthDegraded(true, "Unknown exception during collection loading");
                                                                                                 }

                                                                                                 if (Instance && !Instance->HTTPServers.empty() && Instance->Logs)
                                                                                                 {
                                                                                                      Instance->Logs->Normal("hlquery", "Collection loading unknown exception - queries remain blocked, collections will load on-demand.");
                                                                                                 }
                                                                                            }
                                                                                       });

                                                   {
                                                        std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);

                                                        BackgroundThreads.push_back(std::move(LoadThreadInstanceFinal));
                                                   }
                                              }
                                              else
                                              {
                                                   if (Instance && Instance->Logs)
                                                   {
                                                        std::string MissingItemsListFinalStr = "";

                                                        if (!MetadataCompleteStatus)
                                                        {
                                                             MissingItemsListFinalStr += "metadata scan";
                                                        }

                                                        if (!SyncCompleteStatusFlag)
                                                        {
                                                             if (!MissingItemsListFinalStr.empty())
                                                             {
                                                                  MissingItemsListFinalStr += ", ";
                                                             }

                                                             MissingItemsListFinalStr += "database sync";
                                                        }

                                                        if (!Instance || !Instance->Database)
                                                        {
                                                             if (!MissingItemsListFinalStr.empty())
                                                             {
                                                                  MissingItemsListFinalStr += ", ";
                                                             }

                                                             MissingItemsListFinalStr += "database not available";
                                                        }

                                                        Instance->Logs->Normal("hlquery", "Cannot load collections - " + MissingItemsListFinalStr + " not complete - Server will enable with lazy loading.");
                                                   }

                                                   if (Instance && !Instance->HTTPServers.empty())
                                                   {
                                                        for (auto *server : Instance->HTTPServers)
                                                        {
                                                             server->SetReadyToAccept(true);
                                                        }
                                                   }
                                              }

                                              if (MetadataCompleteStatus && SyncCompleteStatusFlag && Instance && !Instance->HTTPServers.empty())
                                              {
                                                   if (Instance->Logs && Instance->Logs->GetDebugMode())
                                                   {
                                                        Instance->Logs->Debug("hlquery", "WaitForMetadataScan: Enabling HTTP servers to accept connections (MetadataComplete=" + std::string(MetadataCompleteStatus ? "true" : "false") + ", SyncCompleteStatusFlag=" + std::string(SyncCompleteStatusFlag ? "true" : "false") + ", collections loading in background).");
                                                   }

                                                   for (auto *server : Instance->HTTPServers)
                                                   {
                                                        server->SetReadyToAccept(true);
                                                   }

                                                   if (Instance->Logs)
                                                   {
                                                        Instance->Logs->Normal("hlquery", "Metadata scan and database sync completed - HTTP servers accepting connections (collection creation allowed, queries blocked until collections load).");
                                                   }
                                              }
                                              else
                                              {
                                                   if (Instance && !Instance->HTTPServers.empty())
                                                   {
                                                        for (auto *server : Instance->HTTPServers)
                                                        {
                                                             server->SetReadyToAccept(true);
                                                        }

                                                        if (Instance->Logs)
                                                        {
                                                             std::string ReasonMsgFinalText = "";

                                                             if (!MetadataCompleteStatus)
                                                             {
                                                                  ReasonMsgFinalText += "metadata scan";
                                                             }

                                                             if (!SyncCompleteStatusFlag)
                                                             {
                                                                  if (!ReasonMsgFinalText.empty())
                                                                  {
                                                                       ReasonMsgFinalText += " and ";
                                                                  }

                                                                  ReasonMsgFinalText += "database sync";
                                                             }

                                                             Instance->Logs->Critical("hlquery",
                                                                                      "Wait timeout - HTTP servers enabled (may have incomplete data: " + ReasonMsgFinalText +
                                                                                           " not complete) - QUERIES MAY RETURN INCOMPLETE RESULTS.");
                                                        }
                                                   }
                                              }

                                              ThreadLimit::DecrementThreadCount();
                                         });

          {
               std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);

               BackgroundThreads.push_back(std::move(WaitThreadInstance));
          }
     }
     else
     {
          if (Logs)
          {
               Logs->Normal("hlquery", "Thread limit reached, skipping metadata wait thread - checking scan status.");
          }

          if (HybridStorageManagerInstance().IsMetadataScanComplete())
          {
               bool CollectionsLoadedFlagFinalValue = false;

               try
               {
                    if (Logs)
                    {
                         Logs->Normal("hlquery", "Thread limit reached - Loading collections from LSM (this may take a while).");
                    }

                    CollectionsLoadedFlagFinalValue = HybridStorageManagerInstance().LoadCollectionsFromRocksDB();

                    if (Logs)
                    {
                         if (CollectionsLoadedFlagFinalValue)
                         {
                              Logs->Normal("hlquery", "Thread limit reached - Collections loaded, HTTP server enabled.");
                         }
                         else
                         {
                              Logs->Normal("hlquery", "Thread limit reached - Collection loading returned false, HTTP server enabled anyway.");
                         }
                    }
               }
               catch (const std::exception &e)
               {
                    CollectionsLoadedFlagFinalValue = false;

                    if (Logs)
                    {
                         Logs->Critical("hlquery",
                                        "Thread limit reached - Exception loading collections: " + std::string(e.what()) +
                                             " - Server enabled but collections may not be fully loaded.");
                    }
               }
               catch (...)
               {
                    CollectionsLoadedFlagFinalValue = false;

                    if (Logs)
                    {
                         Logs->Critical("hlquery",
                                        "Thread limit reached - Unknown exception loading collections - Server enabled but collections may not be fully loaded.");
                    }
               }

               if (CollectionsLoadedFlagFinalValue)
               {
                    for (auto *server : HTTPServers)
                    {
                         server->SetLoading(false);
                    }

                    if (Logs)
                    {
                         Logs->Normal("hlquery", "Thread limit reached - Collections loaded, queries enabled.");
                    }
               }
               else
               {
                    if (Logs)
                    {
                         Logs->Normal("hlquery",
                                      "Thread limit reached - Collections not loaded, queries remain blocked (collections will load on-demand).");
                    }
               }

               for (auto *server : HTTPServers)
               {
                    server->SetReadyToAccept(true);
               }
          }
          else
          {
               if (Logs)
               {
                    Logs->Normal("hlquery",
                                 "Thread limit reached - HTTP server enabled (metadata scan incomplete, collections not loaded, queries blocked).");
               }

               for (auto *server : HTTPServers)
               {
                    server->SetReadyToAccept(true);
               }
          }
     }
}
