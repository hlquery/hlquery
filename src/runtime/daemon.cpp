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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <mutex>
#include <sys/resource.h>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "api/searchapi.h"
#include "common/actionlist.h"
#include "common/searchpool.h"
#include "core/config.h"
#include "core/helpers.h"
#include "runtime/daemon.h"
#include "runtime/exitmanager.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "search/cstore.h"
#include "sam/sam.h"
#include "search/storageengine.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"

/* Indicates a graceful shutdown was requested. */

volatile sig_atomic_t ShuttingDown = 0;

/* Tracks total SIGINT count for escalation policies. */

volatile sig_atomic_t SigintCount = 0;

/* Indicates a forced exit was requested. */

volatile sig_atomic_t ForceExit = 0;

/* Guards against re-entrant signal handling. */

volatile sig_atomic_t InSignalHandler = 0;

/* Holds a pending shutdown signal to be processed on the main loop. */

volatile sig_atomic_t PendingShutdownSignal = 0;

/* Set while cleanup is running; escalation signals can then exit immediately. */

static volatile sig_atomic_t CleanupInProgress = 0;

/* PID file handle to keep the lock active while running. */

static int PIDFileFD = -1;
static bool NoPIDFile = false;

/* Pipe used to synchronize daemon fork and parent exit. */

static int DaemonSyncPipe[2] = {-1, -1};

/* Self-pipe used to wake the main loop after signal delivery. */

static int SignalWakePipe[2] = {-1, -1};

/* Ensures shutdown processing is performed once. */

static std::atomic<bool> ShutdownProcessingValue{false};

/* Allows skipping authentication for controlled scenarios. */

bool SkipAuthentication = false;

/* Resolves logical server id used for PID file naming. */

static std::string ResolveEffectiveServerId()
{
     std::string ServerID = "001";

     if (Instance && Instance->Config)
     {
          ServerID = Instance->Config->GetServerId();
          const std::string &ConfigFile = Instance->Config->GetConfigFile();

          if (ServerID.empty() || ServerID == "001")
          {
               if (ConfigFile.find("server2") != std::string::npos ||
                   ConfigFile.find("/2nd/") != std::string::npos ||
                   ConfigFile.find("\\2nd\\") != std::string::npos)
               {
                    ServerID = "002";
               }
          }
     }

     return ServerID;
}

/* Resolves the PID file path for the active server instance. */

static std::string ResolvePIDFilePath()
{
     std::string PIDFileName = "hlquery.pid";
     std::string ServerID = ResolveEffectiveServerId();

     if (!ServerID.empty() && ServerID != "001")
     {
          PIDFileName = "hlquery-" + ServerID + ".pid";
     }

     return std::string(HLQUERY_PID_DIR) + "/" + PIDFileName;
}

/* Closes a file descriptor and resets the tracked value. */

static void CloseTrackedFD(int &FDValue)
{
     if (FDValue >= 0)
     {
          close(FDValue);
          FDValue = -1;
     }
}

/* Attempts to unlink a PID file after taking an exclusive lock. */

static bool RemovePIDFileIfUnlocked(const std::string &PIDFilePath)
{
     int LockFileHandle = open(PIDFilePath.c_str(), O_RDWR);

     if (LockFileHandle < 0)
     {
          return (unlink(PIDFilePath.c_str()) == 0 || errno == ENOENT);
     }

     struct flock lock;
     lock.l_type = F_WRLCK;
     lock.l_whence = SEEK_SET;
     lock.l_start = 0;
     lock.l_len = 0;

     bool Removed = false;

     if (fcntl(LockFileHandle, F_SETLK, &lock) == 0)
     {
          Removed = (unlink(PIDFilePath.c_str()) == 0 || errno == ENOENT);
     }

     close(LockFileHandle);

     return Removed;
}

/* Releases the active PID lock and removes the corresponding PID file. */

static void ReleasePIDFile()
{
     if (NoPIDFile)
     {
          return;
     }

     if (PIDFileFD >= 0)
     {
          struct flock UnLockValue;
          UnLockValue.l_type = F_UNLCK;
          UnLockValue.l_whence = SEEK_SET;
          UnLockValue.l_start = 0;
          UnLockValue.l_len = 0;

          fcntl(PIDFileFD, F_SETLK, &UnLockValue);

          close(PIDFileFD);
          PIDFileFD = -1;
     }

     (void)RemovePIDFileIfUnlocked(ResolvePIDFilePath());
}

/* Writes a byte to the signal wake pipe to break blocking waits. */

static void NotifySignalWakeup()
{
     if (SignalWakePipe[1] >= 0)
     {
          const uint8_t WakeByte = 1;
          ssize_t WriteResult = write(SignalWakePipe[1], &WakeByte, sizeof(WakeByte));
          (void)WriteResult;
     }
}

/* Drains any bytes queued on the signal wake pipe. */

static void DrainSignalWakePipe()
{
     if (SignalWakePipe[0] < 0)
     {
          return;
     }

     uint8_t Buffer[32];

     while (read(SignalWakePipe[0], Buffer, sizeof(Buffer)) > 0)
     {

     }
}

/* Configures a pipe for non-blocking, close-on-exec behavior. */

static bool ConfigurePipeFD(int FDValue)
{
     int Flags = fcntl(FDValue, F_GETFL, 0);

     if (Flags >= 0 && fcntl(FDValue, F_SETFL, Flags | O_NONBLOCK) < 0)
     {
          return false;
     }

     int FDFlags = fcntl(FDValue, F_GETFD, 0);

     if (FDFlags >= 0 && fcntl(FDValue, F_SETFD, FDFlags | FD_CLOEXEC) < 0)
     {
          return false;
     }

     return true;
}

/* Creates the signal wake pipe if it has not been initialized yet. */

static bool EnsureSignalWakePipe()
{
     if (SignalWakePipe[0] >= 0 && SignalWakePipe[1] >= 0)
     {
          return true;
     }

     int PipeFDs[2] = {-1, -1};

     if (pipe(PipeFDs) < 0)
     {
          return false;
     }

     if (!ConfigurePipeFD(PipeFDs[0]) || !ConfigurePipeFD(PipeFDs[1]))
     {
          CloseTrackedFD(PipeFDs[0]);
          CloseTrackedFD(PipeFDs[1]);
          return false;
     }

     SignalWakePipe[0] = PipeFDs[0];
     SignalWakePipe[1] = PipeFDs[1];

     return true;
}

/* Installs a signal handler using sigaction semantics. */

static bool InstallSignalHandler(int SignalNum, void (*Handler)(int))
{
     struct sigaction ActionValue;
     memset(&ActionValue, 0, sizeof(ActionValue));
     ActionValue.sa_handler = Handler;
     sigemptyset(&ActionValue.sa_mask);
     ActionValue.sa_flags = 0;

     return sigaction(SignalNum, &ActionValue, nullptr) == 0;
}

/* Installs an ignored signal using sigaction semantics. */

static bool IgnoreSignal(int SignalNum)
{
     struct sigaction ActionValue;
     memset(&ActionValue, 0, sizeof(ActionValue));
     ActionValue.sa_handler = SIG_IGN;
     sigemptyset(&ActionValue.sa_mask);
     ActionValue.sa_flags = 0;

     return sigaction(SignalNum, &ActionValue, nullptr) == 0;
}

/* Determines whether a PID refers to a zombie process. */

static bool IsZombieProcess(pid_t PIDValue)
{
     if (PIDValue <= 1)
     {
          return false;
     }

     std::ifstream StatFile("/proc/" + std::to_string(PIDValue) + "/stat");

     if (!StatFile.is_open())
     {
          return false;
     }

     std::string StatLine;

     if (!std::getline(StatFile, StatLine))
     {
          return false;
     }

     auto RParenPos = StatLine.rfind(')');

     if (RParenPos == std::string::npos || RParenPos + 2 >= StatLine.size())
     {
          return false;
     }

     char StateValue = StatLine[RParenPos + 2];

     return (StateValue == 'Z' || StateValue == 'X');
}

/* Determines whether a PID points at an hlquery process, not just any process. */

static bool IsHLQueryProcess(pid_t PIDValue)
{
     if (PIDValue <= 1)
     {
          return false;
     }

     std::ifstream CommFile("/proc/" + std::to_string(PIDValue) + "/comm");

     if (CommFile.is_open())
     {
          std::string CommValue;

          if (std::getline(CommFile, CommValue) && CommValue.find("hlquery") != std::string::npos)
          {
               return true;
          }
     }

     char ExePathBuffer[4096] = {0};
     std::string ExeLinkPath = "/proc/" + std::to_string(PIDValue) + "/exe";
     ssize_t ExePathLength = readlink(ExeLinkPath.c_str(), ExePathBuffer, sizeof(ExePathBuffer) - 1);

     if (ExePathLength > 0)
     {
          ExePathBuffer[ExePathLength] = '\0';
          std::string ExePathValue(ExePathBuffer);

          if (ExePathValue.find("hlquery") != std::string::npos)
          {
               return true;
          }
     }

     std::ifstream CmdLineFile("/proc/" + std::to_string(PIDValue) + "/cmdline", std::ios::binary);

     if (CmdLineFile.is_open())
     {
          std::string CmdLineValue;

          if (std::getline(CmdLineFile, CmdLineValue, '\0') && CmdLineValue.find("hlquery") != std::string::npos)
          {
               return true;
          }
     }

     return false;
}
std::atomic<int> DaemonHandler::AdaptiveSleepMS(0);

std::atomic<int> DaemonHandler::ConsecutiveBusyIterations(0);

std::atomic<int> DaemonHandler::ConsecutiveIdleIterations(0);

std::atomic<int> DaemonHandler::HighThroughputModeValue(0);

std::atomic<int> DaemonHandler::LastEventCount(0);

/* Use uint64_t to prevent overflow during long-running process life cycles */

std::atomic<uint64_t> DaemonHandler::LazyProcessingCounter(0);

std::atomic<int> DaemonHandler::BatchSize(10000);

/* Handles HLQuery-style adaptive sleep and high-throughput optimization logic */

void DaemonHandler::ProcessSocketEngineOptimization()
{
     /*
      * Check for pending network work FIRST before executing any sleep logic.
      * If there is any pending work, immediately disable adaptive sleep and enable
      * HighThroughputModeValue to ensure the server remains responsive.
      */

     bool HasPendingWork = SocketEngine::HasPendingWork();

     /* Pending work means the loop should stay in an aggressively responsive mode. */

     if (HasPendingWork)
     {
          AdaptiveSleepMS.store(0, std::memory_order_relaxed);
          HighThroughputModeValue.store(1, std::memory_order_relaxed);
          ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
          ConsecutiveBusyIterations.fetch_add(1, std::memory_order_relaxed);

          return;
     }

     /* Track activity levels based on socket event deltas */

     int CurrentEventCount = SocketEngine::GetEventCount();

     int LastCount = LastEventCount.load(std::memory_order_relaxed);

     int EventsDelta = CurrentEventCount - LastCount;

     /* Any positive delta means the engine is still actively consuming socket work. */

     if (EventsDelta > 0)
     {
          int BusyCount = ConsecutiveBusyIterations.fetch_add(1, std::memory_order_relaxed);

          ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);

          /*
           * Reset sleep time IMMEDIATELY when any activity is detected.
           * This ensures subsequent operations do not suffer from accumulated
           * sleep time from previous idle periods.
           */

          AdaptiveSleepMS.store(0, std::memory_order_relaxed);

          HighThroughputModeValue.store(1, std::memory_order_relaxed);

          if (BusyCount > 100)
          {
               ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);
          }
     }
     else
     {
          int IdleCount = ConsecutiveIdleIterations.fetch_add(1, std::memory_order_relaxed);

          ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);

          /*
           * Require a significant number of idle iterations before introducing sleep.
           * This keeps HighThroughputModeValue active longer during processing bursts.
           */

          if (IdleCount > 1000)
          {
               int CurrentSleep = AdaptiveSleepMS.load(std::memory_order_relaxed);

               /* Increment sleep time gradually up to a 1ms maximum cap */

               int NewSleep = std::min(CurrentSleep + 1, 1);
               AdaptiveSleepMS.store(NewSleep, std::memory_order_relaxed);
               HighThroughputModeValue.store(0, std::memory_order_relaxed);

               ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
          }
     }

     LastEventCount.store(CurrentEventCount, std::memory_order_relaxed);

     /*
      * The actual sleep/yield logic is handled externally based on these flags.
      * We ensure that memory ordering is preserved for these optimization signals.
      */

     (void)HighThroughputModeValue.load(std::memory_order_relaxed);

     (void)AdaptiveSleepMS.load(std::memory_order_relaxed);
}

/* Manages ultra-lazy processing tasks for deferred or expensive operations */

void DaemonHandler::ProcessLazyOperations()
{
     uint64_t Counter = LazyProcessingCounter.fetch_add(1, std::memory_order_relaxed);

     /* Protect against counter overflow by resetting when approaching limits */

     if (Counter >= UINT64_MAX - 10000)
     {
          LazyProcessingCounter.store(0, std::memory_order_relaxed);
          Counter = 0;
     }

     /* Trigger background database maintenance tasks at configured intervals */

     if (Instance && Instance->Database && (Counter % BatchSize.load(std::memory_order_relaxed) == 0))
     {
          /* Sampling time here preserves the historical hook without forcing extra work. */

          (void)time(nullptr);

          /*
           * Background maintenance like compaction and expiration is handled
           * automatically by the underlying LSM storage engine.
           */
     }
}

/* Resets all internal optimization counters and state flags */

void DaemonHandler::ResetOptimizationState()
{
     AdaptiveSleepMS.store(0, std::memory_order_relaxed);
     ConsecutiveBusyIterations.store(0, std::memory_order_relaxed);
     ConsecutiveIdleIterations.store(0, std::memory_order_relaxed);
     HighThroughputModeValue.store(0, std::memory_order_relaxed);
     LastEventCount.store(0, std::memory_order_relaxed);
     LazyProcessingCounter.store(0, std::memory_order_relaxed);
}

/* Retrieves current snapshots of optimization and performance statistics */

DaemonHandler::OptimizationStats DaemonHandler::GetOptimizationStats()
{
     OptimizationStats Stats;
     Stats.adaptive_sleep_ms = AdaptiveSleepMS.load(std::memory_order_relaxed);
     Stats.consecutive_busy_iterations = ConsecutiveBusyIterations.load(std::memory_order_relaxed);
     Stats.consecutive_idle_iterations = ConsecutiveIdleIterations.load(std::memory_order_relaxed);
     Stats.high_throughput_mode = HighThroughputModeValue.load(std::memory_order_relaxed);
     Stats.last_event_count = LastEventCount.load(std::memory_order_relaxed);
     Stats.current_event_count = SocketEngine::GetEventCount();
     Stats.events_delta = Stats.current_event_count - Stats.last_event_count;
     Stats.lazy_processing_counter = LazyProcessingCounter.load(std::memory_order_relaxed);
     Stats.batch_size = BatchSize.load(std::memory_order_relaxed);

     return Stats;
}

/* Parse command line arguments and update the server configuration state. */

void hlquery::ParseArgs()
{
     try
     {
          /* The server config owns the canonical command-line snapshot for this process. */

          if (!Config)
          {
               return;
          }

          const auto &CmdLineVal = Config->GetCommandLine();

          int ArgcCount = CmdLineVal.argc;

          char **ArgvList = CmdLineVal.argv;

          if (ArgcCount == 0 || ArgvList == nullptr)
          {
               return;
          }

          int OptChar;

          static struct option LongOptions[] = {
               {"test", no_argument, 0, 't'},
               {"nofork", no_argument, 0, 'n'},
               {"nopid", no_argument, 0, 'p'},
               {"debug", no_argument, 0, 'd'},
               {"verbose", no_argument, 0, 'v'},
               {"config", required_argument, 0, 'c'},
               {"skip-auth", no_argument, 0, 's'},
               {"forcestop", no_argument, 0, 'f'},
               {"help", no_argument, 0, 'h'},
               {0, 0, 0, 0}};

          int OptionIndex = 0;

          auto PrintHelp = [&]()
          {
               std::cout << "Usage: " << ArgvList[0] << " [options]" << std::endl;
               newline();
               std::cout << "Options:" << std::endl;
               std::cout << "  -t, --test          Run in test mode" << std::endl;
               std::cout << "  -n, --nofork        Run in foreground (prevents daemonization)" << std::endl;
               std::cout << "  -p, --nopid         Do not create or remove a PID file" << std::endl;
               std::cout << "  -d, --debug         Enable debug logging to log files only (not terminal)" << std::endl;
               std::cout << "  -v, --verbose       Print debug/verbose messages to terminal AND log to files" << std::endl;
               std::cout << "  -c, --config FILE   Use specified config file" << std::endl;
               std::cout << "  -s, --skip-auth     Skip password authentication (dev/testing only)" << std::endl;
               std::cout << "  -f, --forcestop     Force stop running daemon and remove PID file" << std::endl;
               std::cout << "  -h, --help          Show this help message" << std::endl;
               newline();
               std::cout << "Note: --debug and --verbose are separate options:" << std::endl;
               std::cout << "  --debug:   logs debug messages to log files only (no terminal output)" << std::endl;
               std::cout << "  --verbose: logs debug messages to files AND prints them to terminal (both)" << std::endl;
               std::cout << "  They can be used independently or together." << std::endl;
               newline();
          };

          optind = 1;

          optopt = 0;

          opterr = 0;

          /* getopt_long advances through both short and long options in a single loop. */

          while ((OptChar = getopt_long(ArgcCount, ArgvList, "tnpc:hsdfv", LongOptions, &OptionIndex)) != -1)
          {
               switch (OptChar)
               {
                    case 't':
                    {
                         Config->SetTestMode(true);
                         break;
                    }

                    case 'n':
                    {
                         Config->SetNoForkMode(true);
                         break;
                    }

                    case 'p':
                    {
                         NoPIDFile = true;
                         break;
                    }

                    case 'd':
                    {
                         Config->SetDebugMode(true);
                         break;
                    }

                    case 'v':
                    {
                         Config->SetVerboseMode(true);
                         break;
                    }

                    case 'c':
                    {
                         Config->SetConfigFile(optarg);

                         if (Config->GetDebugMode())
                         {
                              ConsoleWriter::WriteDebug("ParseArgs: Set config file to: " + std::string(optarg) + ".");
                         }

                         break;
                    }

                    case 's':
                    {
                         SkipAuthentication = true;
                         break;
                    }

                    case 'f':
                    {
                         ForceStop();
                         ExitManager::Exit(0);
                    }

                    case 'h':
                    {
                         PrintHelp();
                         ExitManager::Exit(0);
                    }

                    case '?':
                    {
                         PrintHelp();
                         ExitManager::Exit(1);
                    }

                    default:
                    {
                         break;
                    }
               }
          }
     }
     catch (...)
     {
          /* Silently swallow exceptions to prevent process termination during arg parsing. */
     }
}

/* Perform an async-signal-safe write operation to the standard error stream. */

static void SafeWrite(const char *Msg)
{
     size_t LenVal = 0;

     while (Msg[LenVal])
     {
          LenVal++;
     }

     ssize_t WriteResult = write(STDERR_FILENO, Msg, LenVal);
     (void)WriteResult;
}

/* Primary signal handler function for management of process life cycle signals. */

void hlquery::SetSignal(int SignalNum)
{
     if (InSignalHandler)
     {
          ExitManager::EmergencyExit(1);
     }

     InSignalHandler = 1;

     if (SignalNum == SIGINT)
     {
          /* SIGINT is treated as an immediate shutdown request from an interactive user. */

          if (CleanupInProgress)
          {
               ExitManager::EmergencyExit(0);
          }

          PendingShutdownSignal = SignalNum;

          ShuttingDown = 1;
          ForceExit = 1;
          NotifySignalWakeup();
          InSignalHandler = 0;

          return;
     }
     else if (SignalNum == SIGTERM)
     {
          /* SIGTERM keeps the shutdown path graceful so cleanup can run on the main loop. */

          PendingShutdownSignal = SignalNum;
          ShuttingDown = 1;
          NotifySignalWakeup();
          InSignalHandler = 0;

          return;
     }
     else if (SignalNum == SIGALRM)
     {
          /* SIGALRM is only used as a last-resort timeout for blocked shutdown paths. */

          SafeWrite("\nSave timeout, forcing exit!\n");
          ExitManager::EmergencyExit(1);
     }
     else if (SignalNum == SIGUSR1)
     {
          /* SIGUSR1 reuses the same deferred shutdown path as the main termination signals. */

          PendingShutdownSignal = SignalNum;
          ShuttingDown = 1;
          NotifySignalWakeup();
          InSignalHandler = 0;

          return;
     }
     else
     {
          /* All other managed signals still wake the main loop for centralized handling. */

          PendingShutdownSignal = SignalNum;
          ShuttingDown = 1;
          NotifySignalWakeup();
          InSignalHandler = 0;
     }
}

/* Handles crash signals like SIGSEGV and SIGABRT for emergency logging and exit. */

static void CrashSignalHandler(int SigNum)
{
     static volatile sig_atomic_t InCrashHandler = 0;

     if (InCrashHandler)
     {
          ExitManager::EmergencyExit(1);
     }

     InCrashHandler = 1;

     int CrashFd = open(HLQUERY_LOG_DIR "/hlquery_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

     if (CrashFd >= 0)
     {
          const char *CrashMsg = SigNum == SIGSEGV ? "[CRASH] SIGSEGV - Segmentation fault\n" : "[CRASH] SIGABRT - Abort signal\n";

          size_t MsgLen = 0;

          while (CrashMsg[MsgLen] != '\0')
          {
               MsgLen++;
          }

          ssize_t WriteResult = write(CrashFd, CrashMsg, MsgLen);
          (void)WriteResult;
          close(CrashFd);
     }

     if (SigNum == SIGSEGV)
     {
          SafeWrite("\n[CRASH] SIGSEGV received - segmentation fault detected!\n");
          SafeWrite("[CRASH] Check " HLQUERY_LOG_DIR "/hlquery_crash.log for details.\n");
     }
     else if (SigNum == SIGABRT)
     {
          SafeWrite("\n[CRASH] SIGABRT received - abort detected!\n");
          SafeWrite("[CRASH] Check " HLQUERY_LOG_DIR "/hlquery_crash.log for details.\n");
     }

     ExitManager::EmergencyExit(1);
}

/* Configures and installs all necessary signal handlers for the process. */

void hlquery::SetupSignalHandlers()
{
     try
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("daemon", "Installing signal handlers.");
          }

          if (!EnsureSignalWakePipe())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Failed to create signal wake pipe.");
               }
          }

          InstallSignalHandler(SIGTERM, SetSignal);
          InstallSignalHandler(SIGINT, SetSignal);
          InstallSignalHandler(SIGQUIT, SetSignal);
          InstallSignalHandler(SIGHUP, SetSignal);
          InstallSignalHandler(SIGUSR1, SetSignal);
          InstallSignalHandler(SIGUSR2, SetSignal);
          IgnoreSignal(SIGPIPE);
          InstallSignalHandler(SIGALRM, SetSignal);
          InstallSignalHandler(SIGSEGV, CrashSignalHandler);
          InstallSignalHandler(SIGABRT, CrashSignalHandler);
     }
     catch (...)
     {
          /* Ignore registration failures during early process setup. */
     }
}

/* Executes deferred signal handling operations from the primary server loop. */

void hlquery::ProcessDeferredSignals()
{
     /* Drain the self-pipe first so repeated signals do not keep the loop artificially hot. */

     DrainSignalWakePipe();

     if (PendingShutdownSignal == 0)
     {
          return;
     }

     bool Expected = false;

     if (!ShutdownProcessingValue.compare_exchange_strong(Expected, true))
     {
          return;
     }

     const sig_atomic_t SignalNum = PendingShutdownSignal;
     PendingShutdownSignal = 0;

     /* Logging is deferred here because the actual signal handler must stay async-signal-safe. */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("daemon",
                                 "Processing shutdown signal " + std::to_string(SignalNum) +
                                      " (cleanup deferred to main shutdown path).");
     }
}

/* Returns true if a shutdown request has been initiated. */

bool hlquery::ShouldShutdown()
{
     return ShuttingDown != 0;
}

/* Returns true if a force exit request has been initiated. */

bool hlquery::ShouldForceExit()
{
     return ForceExit != 0;
}

/* Reset all signals and shutdown counters to their default state. */

void hlquery::ResetSignalCounters()
{
     SigintCount 		= 0;
     ShuttingDown 		= 0;
     ForceExit 			= 0;
     PendingShutdownSignal 	= 0;
     InSignalHandler 		= 0;
     
     ShutdownProcessingValue.store(false);
}

/* Set the internal shutdown flag to begin graceful termination. */

void hlquery::SetShutdownFlag()
{
     try
     {
          ShuttingDown = 1;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("daemon", "Shutdown flag set - graceful shutdown initiated.");
          }
     }
     catch (...)
     {
          ShuttingDown = 1;
     }
}

/* Checks if another instance of the server is currently active. */

bool hlquery::CheckExistingProcess()
{
     try
     {
          std::string PIDFilePath = ResolvePIDFilePath();

          /* A missing PID file means there is no persisted ownership record to inspect. */

          int PIDFileHandle = open(PIDFilePath.c_str(), O_RDONLY);

          if (PIDFileHandle < 0)
          {
               return false;
          }

          struct FdGuard
          {
               int FDValue;

               bool Released;

               FdGuard(int fd) : FDValue(fd), Released(false)
               {

               }

               ~FdGuard()
               {
                    if (!Released && FDValue >= 0)
                    {
                         close(FDValue);
                    }
               }

               void release()
               {
                    Released = true;
               }

               int get()
               {
                    return FDValue;
               }
          } PIDFdGuard(PIDFileHandle);

          struct flock PIDFileLock;

          PIDFileLock.l_type = F_WRLCK;
          PIDFileLock.l_whence = SEEK_SET;
          PIDFileLock.l_start = 0;
          PIDFileLock.l_len = 0;

          /* An active write lock indicates another live process still owns this PID file. */

          if (fcntl(PIDFdGuard.get(), F_GETLK, &PIDFileLock) == 0 && PIDFileLock.l_type != F_UNLCK)
          {
               if (PIDFileLock.l_pid > 1 && !IsHLQueryProcess(PIDFileLock.l_pid))
               {
                    unlink(PIDFilePath.c_str());

                    return false;
               }

               return true;
          }

          struct flock PIDReadLock;

          PIDReadLock.l_type = F_RDLCK;
          PIDReadLock.l_whence = SEEK_SET;
          PIDReadLock.l_start = 0;
          PIDReadLock.l_len = 0;

          if (fcntl(PIDFdGuard.get(), F_SETLK, &PIDReadLock) < 0)
          {
               return true;
          }

          if (lseek(PIDFdGuard.get(), 0, SEEK_SET) < 0)
          {
               PIDReadLock.l_type = F_UNLCK;

               fcntl(PIDFdGuard.get(), F_SETLK, &PIDReadLock);

               unlink(PIDFilePath.c_str());

               return false;
          }

          char PIDBuffer[32] = {0};
          ssize_t BytesReadCount = read(PIDFdGuard.get(), PIDBuffer, sizeof(PIDBuffer) - 1);
          PIDReadLock.l_type = F_UNLCK;
          fcntl(PIDFdGuard.get(), F_SETLK, &PIDReadLock);

          if (BytesReadCount <= 0)
          {
               unlink(PIDFilePath.c_str());

               return false;
          }

          PIDBuffer[sizeof(PIDBuffer) - 1] = '\0';

          try
          {
               pid_t ExistingPIDValue = std::stoi(PIDBuffer);

               /* Reject obviously invalid PIDs before probing the process table. */

               if (ExistingPIDValue <= 1 || ExistingPIDValue > 4194304)
               {
                    unlink(PIDFilePath.c_str());

                    return false;
               }

               int KillSignalResult = kill(ExistingPIDValue, 0);

               /* kill(pid, 0) checks reachability without delivering a real signal. */

               if (KillSignalResult == 0)
               {
                    if (ExistingPIDValue != getpid())
                    {
                         if (IsZombieProcess(ExistingPIDValue) || !IsHLQueryProcess(ExistingPIDValue))
                         {
                              unlink(PIDFilePath.c_str());
                              return false;
                         }

                         PIDFdGuard.release();
                         return true;
                    }

                    return false;
               }
               else if (errno == EPERM)
               {
                    if (ExistingPIDValue != getpid())
                    {
                         if (IsZombieProcess(ExistingPIDValue))
                         {
                              unlink(PIDFilePath.c_str());
                              return false;
                         }

                         PIDFdGuard.release();
                         return true;
                    }

                    return false;
               }
               else
               {
                    unlink(PIDFilePath.c_str());
                    return false;
               }
          }
          catch (const std::exception &)
          {
               unlink(PIDFilePath.c_str());
          }

          return false;
     }
     catch (...)
     {
          return false;
     }
}

/* Write the process ID to the PID file for instance management. */

bool hlquery::WritePID()
{
     if (NoPIDFile)
     {
          return true;
     }

     if (PIDFileFD >= 0)
     {
          close(PIDFileFD);
          PIDFileFD = -1;
     }

     try
     {
          try
          {
               /* Ensure the runtime PID directory exists before opening the lock file. */

               std::filesystem::create_directories(HLQUERY_PID_DIR);
          }
          catch (const std::filesystem::filesystem_error &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Failed to create PID directory: " + std::string(e.what()) + ".");
               }

               return false;
          }

          std::string PIDFilePath = ResolvePIDFilePath();
          int PIDFileHandle = open(PIDFilePath.c_str(), O_CREAT | O_WRONLY, 0644);

          if (PIDFileHandle < 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Failed to open PID file: " + PIDFilePath + ": " + std::string(strerror(errno)) + ".");
               }

               return false;
          }

          struct FdGuard
          {
               int FDValue;
               bool Released;
               bool LockAcquired;

               FdGuard(int fd) : FDValue(fd), Released(false), LockAcquired(false)
               {
               
               }

               ~FdGuard()
               {
                    if (!Released && FDValue >= 0)
                    {
                         if (LockAcquired)
                         {
                              struct flock lock;
                              lock.l_type = F_UNLCK;
                              lock.l_whence = SEEK_SET;
                              lock.l_start = 0;
                              lock.l_len = 0;
                              fcntl(FDValue, F_SETLK, &lock);
                         }

                         close(FDValue);
                    }
               }

               void release()
               {
                    Released = true;
               }

               int get()
               {
                    return FDValue;
               }

               void set_lock_acquired(bool val)
               {
                    LockAcquired = val;
               }
          } PIDFdGuard(PIDFileHandle);

          try
          {
               struct flock lock;
               lock.l_type = F_WRLCK;
               lock.l_whence = SEEK_SET;
               lock.l_start = 0;
               lock.l_len = 0;

               /* Keep the lock held for the entire lifetime of the daemon process. */

               if (fcntl(PIDFdGuard.get(), F_SETLK, &lock) < 0)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("daemon", "Failed to lock PID file: " + PIDFilePath + ": " + std::string(strerror(errno)) + ".");
                    }

                    return false;
               }

               PIDFdGuard.set_lock_acquired(true);

               if (ftruncate(PIDFdGuard.get(), 0) < 0)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("daemon", "Failed to truncate PID file: " + PIDFilePath + ": " + std::string(strerror(errno)) + ".");
                    }

                    return false;
               }

               std::string CurrentPIDStr = std::to_string(getpid());

               /* The PID file only stores the numeric PID, with the lock carrying ownership. */

               ssize_t BytesWrittenCount = write(PIDFdGuard.get(), CurrentPIDStr.c_str(), CurrentPIDStr.length());

               if (BytesWrittenCount < 0 || static_cast<size_t>(BytesWrittenCount) != CurrentPIDStr.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("daemon", "Failed to write PID file: " + PIDFilePath + ".");
                    }

                    return false;
               }

               if (fsync(PIDFdGuard.get()) < 0)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("daemon", "Warning: fsync() failed on PID file: " + std::string(strerror(errno)) + " - PID may not be persisted to disk.");
                    }
               }

               PIDFileFD = PIDFdGuard.get();
               PIDFdGuard.release();

               return true;
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Exception during PID file creation - continuing anyway.");
               }

               return false;
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("daemon", "Outer exception during PID file creation - continuing anyway.");
          }

          return false;
     }

     return false;
}

/* Force stop a running daemon instance and remove its associated PID file. */

void hlquery::ForceStop()
{
     std::string PIDFilePath = ResolvePIDFilePath();
     std::cout << "hlquery forcestop: Attempting to stop daemon..." << std::endl;

     /* Check if the process ID file exists. */

     int PIDFileHandle = open(PIDFilePath.c_str(), O_RDONLY);

     if (PIDFileHandle < 0)
     {
          if (errno == ENOENT)
          {
               std::cout << "hlquery forcestop: No PID file found at " << PIDFilePath << std::endl;
               std::cout << "hlquery forcestop: Daemon is not running (or PID file was removed)." << std::endl;
               ExitManager::Exit(0);
          }
          else
          {
               ConsoleWriter::WriteError("hlquery forcestop: Error opening PID file: " + std::string(strerror(errno)) + ".", true);
               ExitManager::Exit(1);
          }
     }

     if (lseek(PIDFileHandle, 0, SEEK_SET) < 0)
     {
          close(PIDFileHandle);
          ConsoleWriter::WriteError("hlquery forcestop: Error seeking PID file: " + std::string(strerror(errno)) + ".", true);
          ExitManager::Exit(1);
     }

     char PIDBuffer[32] = {0};
     ssize_t BytesReadCount = read(PIDFileHandle, PIDBuffer, sizeof(PIDBuffer) - 1);
     close(PIDFileHandle);
     PIDBuffer[sizeof(PIDBuffer) - 1] = '\0';

     if (BytesReadCount <= 0)
     {
          ConsoleWriter::WriteError("hlquery forcestop: PID file is empty or unreadable.", true);
          RemovePIDFileIfUnlocked(PIDFilePath);
          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }

     pid_t TargetPIDValue = 0;

     try
     {
          TargetPIDValue = std::stoi(PIDBuffer);
     }
     catch (const std::exception &e)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Invalid PID in file: " + std::string(PIDBuffer) + ".", true);
          RemovePIDFileIfUnlocked(PIDFilePath);
          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }

     if (TargetPIDValue <= 1 || TargetPIDValue > 4194304)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Invalid PID value: " + std::to_string(TargetPIDValue) + ".", true);
          RemovePIDFileIfUnlocked(PIDFilePath);

          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }

     std::cout << "hlquery forcestop: Found PID " << TargetPIDValue << " in PID file." << std::endl;

     std::string ProcCmdLinePath = "/proc/" + std::to_string(TargetPIDValue) + "/cmdline";
     std::ifstream CmdLineFileStream(ProcCmdLinePath);
     bool IsHLQueryProcess = false;

     /* Best-effort cmdline validation reduces the chance of killing an unrelated process. */

     if (CmdLineFileStream.is_open())
     {
          std::string CmdLineContent;
          std::getline(CmdLineFileStream, CmdLineContent);

          if (CmdLineContent.find("hlquery") != std::string::npos)
          {
               IsHLQueryProcess = true;
          }

          CmdLineFileStream.close();
     }

     if (kill(TargetPIDValue, 0) != 0)
     {
          if (errno == ESRCH)
          {
               std::cout << "hlquery forcestop: Process " << TargetPIDValue << " is not running (stale PID file)." << std::endl;

               RemovePIDFileIfUnlocked(PIDFilePath);

               std::cout << "hlquery forcestop: Removed stale PID file." << std::endl;
               ExitManager::Exit(0);
          }
          else if (errno == EPERM)
          {
               ConsoleWriter::WriteError("hlquery forcestop: Permission denied to signal process " + std::to_string(TargetPIDValue) + ".", true);
               ConsoleWriter::WriteError("hlquery forcestop: Try running with sudo.", true);
               ExitManager::Exit(1);
          }
          else
          {
               ConsoleWriter::WriteError("hlquery forcestop: Error checking process " + std::to_string(TargetPIDValue) + ": " + std::string(strerror(errno)) + ".", true);
               ExitManager::Exit(1);
          }
     }

     if (!IsHLQueryProcess && kill(TargetPIDValue, 0) == 0)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Could not verify process " + std::to_string(TargetPIDValue) + " is hlquery.", true);
          ConsoleWriter::WriteError("hlquery forcestop: Refusing to signal an unverified process.", true);
          ExitManager::Exit(1);
     }

     if (kill(TargetPIDValue, 0) != 0)
     {
          if (errno == ESRCH)
          {
               std::cout << "hlquery forcestop: Process " << TargetPIDValue << " exited between check and signal." << std::endl;

               RemovePIDFileIfUnlocked(PIDFilePath);

               ExitManager::Exit(0);
          }
     }

     std::cout << "hlquery forcestop: Sending SIGTERM to process " << TargetPIDValue << "..." << std::endl;

     if (kill(TargetPIDValue, SIGTERM) != 0)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Failed to send SIGTERM: " + std::string(strerror(errno)) + ".", true);
          ExitManager::Exit(1);
     }

     std::cout << "hlquery forcestop: Waiting for process to exit..." << std::endl;

     const int MaxWaitSeconds = 5;

     const int PollIntervalMS = 100;

     int WaitedMSCount = 0;

     bool ProcessHasExited = false;

     /* Poll briefly after SIGTERM so a healthy daemon can flush state and exit cleanly. */

     while (WaitedMSCount < MaxWaitSeconds * 1000)
     {
          int KillSignalCheckResult = kill(TargetPIDValue, 0);

          if (KillSignalCheckResult != 0)
          {
               if (errno == ESRCH)
               {
                    std::cout << "hlquery forcestop: Process exited gracefully." << std::endl;

                    RemovePIDFileIfUnlocked(PIDFilePath);
                    std::cout << "hlquery forcestop: Removed PID file." << std::endl;
                    std::cout << "hlquery forcestop: SUCCESS - Daemon stopped." << std::endl;
                    ProcessHasExited = true;
                    
                    break;
               }
               else if (errno == EPERM)
               {
                    /* Handle permission issues if they arise during polling. */
               }
               else
               {
                    ProcessHasExited = true;
                    break;
               }
          }

          struct timespec SleepTimeSpec;
          SleepTimeSpec.tv_sec = 0;
          SleepTimeSpec.tv_nsec = PollIntervalMS * 1000000;
          nanosleep(&SleepTimeSpec, nullptr);
          WaitedMSCount += PollIntervalMS;
     }

     if (ProcessHasExited)
     {
          ExitManager::Exit(0);
     }

     std::cout << "hlquery forcestop: Process didn't exit, sending SIGKILL..." << std::endl;

     if (kill(TargetPIDValue, SIGKILL) != 0)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Failed to send SIGKILL: " + std::string(strerror(errno)) + ".", true);
          ExitManager::Exit(1);
     }

     if (kill(TargetPIDValue, 0) == 0)
     {
          ConsoleWriter::WriteError("hlquery forcestop: Process " + std::to_string(TargetPIDValue) + " still running after SIGKILL!", false);
          ConsoleWriter::WriteError("hlquery forcestop: This should never happen - process may be in uninterruptible state.", true);
          ExitManager::Exit(1);
     }

     std::cout << "hlquery forcestop: Process killed." << std::endl;

     RemovePIDFileIfUnlocked(PIDFilePath);

     std::cout << "hlquery forcestop: Removed PID file." << std::endl;
     std::cout << "hlquery forcestop: SUCCESS - Daemon force-stopped." << std::endl;
     ExitManager::Exit(0);
}

/* Configures file descriptors for various operational modes. */

void hlquery::SetupFileDescriptors()
{
     try
     {
          if (Config && !Config->GetNoForkMode())
          {
               /* A detached daemon reads from /dev/null instead of inheriting the terminal. */

               fflush(stdout);
               fflush(stderr);
               int NullFdValue = open("/dev/null", O_RDWR);

               if (NullFdValue >= 0)
               {
                    int ResultInValue = dup2(NullFdValue, STDIN_FILENO);

                    (void)ResultInValue;

                    if (NullFdValue != STDIN_FILENO && NullFdValue != STDOUT_FILENO && NullFdValue != STDERR_FILENO)
                    {
                         close(NullFdValue);
                    }
               }
          }
          else
          {
               /* Foreground mode keeps stdout available for direct operator feedback. */

               fflush(stderr);
               fflush(stdout);
          }
     }
     catch (...)
     {
          /* Swallow exceptions during FD setup to avoid early process crash. */
     }
}

/* Completes the final daemon configuration steps after the fork operation. */

void hlquery::CompleteDaemonSetup()
{
     if (Config && !Config->GetNoForkMode() && !Config->GetTestMode())
     {
          /* Parent and child synchronize so startup errors are reported before detaching. */

          fflush(stdout);
          fflush(stderr);
          std::cout.flush();
          std::cerr.flush();
          fflush(NULL);

          if (DaemonSyncPipe[1] >= 0)
          {
               fflush(stdout);

               fflush(stderr);
               std::cout.flush();
               std::cerr.flush();
               fflush(NULL);
               ssize_t WriteResult = write(DaemonSyncPipe[1], "1", 1);
               (void)WriteResult;
               close(DaemonSyncPipe[1]);
               DaemonSyncPipe[1] = -1;

               /* Give the parent a short window to receive the success byte and exit cleanly. */

               struct timespec DelayTimeSpec;
               DelayTimeSpec.tv_sec = 0;
               DelayTimeSpec.tv_nsec = 50000000;
               nanosleep(&DelayTimeSpec, nullptr);
          }

          int NullFdFinal = open("/dev/null", O_RDWR);

          if (NullFdFinal >= 0)
          {
               dup2(NullFdFinal, STDIN_FILENO);
               dup2(NullFdFinal, STDOUT_FILENO);
               dup2(NullFdFinal, STDERR_FILENO);
               close(NullFdFinal);
          }
     }
}

/* Transitions the process into a daemon by forking into the background. */

bool hlquery::Daemonize()
{
     DaemonSyncPipe[0] = -1;
     DaemonSyncPipe[1] = -1;

     try
     {
          /* The sync pipe lets the parent wait until the daemon child has finished setup. */

          if (pipe(DaemonSyncPipe) < 0)
          {
               DaemonSyncPipe[0] = -1;
               DaemonSyncPipe[1] = -1;

               return false;
          }

          pid_t ForkPidValueVal = fork();

          if (ForkPidValueVal < 0)
          {
               close(DaemonSyncPipe[0]);
               close(DaemonSyncPipe[1]);
               DaemonSyncPipe[0] = -1;
               DaemonSyncPipe[1] = -1;

               return false;
          }
          else if (ForkPidValueVal > 0)
          {
               /* The original parent exits only after the child confirms successful detach. */

               close(DaemonSyncPipe[1]);
               char SyncDummyChar;
               ssize_t SyncReadCount = read(DaemonSyncPipe[0], &SyncDummyChar, 1);

               if (SyncReadCount <= 0 || SyncDummyChar == 'E')
               {
                    close(DaemonSyncPipe[0]);
                    DaemonSyncPipe[0] = -1;
                    ExitManager::EmergencyExit(1);
               }

               close(DaemonSyncPipe[0]);
               DaemonSyncPipe[0] = -1;
               print_ok("Now detaching.");
               newline();
               fflush(stdout);
               ExitManager::EmergencyExit(0);
          }

          close(DaemonSyncPipe[0]);

          /* setsid creates a new session so the daemon stops being tied to the terminal. */

          if (setsid() < 0)
          {
               if (DaemonSyncPipe[1] >= 0)
               {
                    ssize_t ErrorWriteResult = write(DaemonSyncPipe[1], "E", 1);
                    (void)ErrorWriteResult;
                    close(DaemonSyncPipe[1]);
                    DaemonSyncPipe[1] = -1;
               }

               ExitManager::EmergencyExit(1);
          }

          ForkPidValueVal = fork();

          /* The second fork prevents the daemon from reacquiring a controlling terminal. */

          if (ForkPidValueVal < 0)
          {
               if (DaemonSyncPipe[1] >= 0)
               {
                    ssize_t ErrorWriteResultFinal = write(DaemonSyncPipe[1], "E", 1);
                    (void)ErrorWriteResultFinal;
                    close(DaemonSyncPipe[1]);
                    DaemonSyncPipe[1] = -1;
               }

               ExitManager::EmergencyExit(1);
          }
          else if (ForkPidValueVal > 0)
          {
               ExitManager::EmergencyExit(0);
          }

          int ChdirResultValueVal = chdir("/");

          /* The daemon should not pin the caller's working directory after detach. */

          (void)ChdirResultValueVal;
          return true;
     }
     catch (const std::exception &)
     {
          if (DaemonSyncPipe[1] >= 0)
          {
               close(DaemonSyncPipe[1]);
               DaemonSyncPipe[1] = -1;
          }

          ExitManager::EmergencyExit(1);
     }
     catch (...)
     {
          if (DaemonSyncPipe[1] >= 0)
          {
               close(DaemonSyncPipe[1]);
               DaemonSyncPipe[1] = -1;
          }

          ExitManager::EmergencyExit(1);
     }
}

/* Configures the core dump size limit to the maximum allowable value. */

void hlquery::IncreaseCoreDumpSize()
{
     try
     {
          errno = 0;

          /* Expanding the core limit improves post-mortem debugging for hard crashes. */

          rlimit RLimitValue;

          if (getrlimit(RLIMIT_CORE, &RLimitValue) == -1)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("daemon", "Warning: Unable to increase core dump size: getrlimit(RLIMIT_CORE) failed: " + std::string(strerror(errno)) + ".");
               }

               return;
          }

          RLimitValue.rlim_cur = RLimitValue.rlim_max;

          if (setrlimit(RLIMIT_CORE, &RLimitValue) == -1)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("daemon", "Warning: Unable to increase core dump size: setrlimit(RLIMIT_CORE) failed: " + std::string(strerror(errno)) + ".");
               }
          }
     }
     catch (...)
     {
          /* Ignore failures during resource limit configuration. */
     }
}

/* Performs comprehensive cleanup of server resources and persistent state. */

void hlquery::Cleanup()
{
     static std::atomic<bool> CleanupCalledFlagValue{false};
     bool ExpectedFlag = false;

     if (!CleanupCalledFlagValue.compare_exchange_strong(ExpectedFlag, true))
     {
          return;
     }

     CleanupInProgress = 1;

     if (Instance)
     {
          Instance->SetShutdownInProgress(true);
     }

     auto LogCleanupStage = [](const std::string& Stage)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("daemon", "Cleanup: " + Stage + ".");
          }
     };

     LogCleanupStage("begin");

     /* Stop modules before tearing down shared executors they may depend on. */

     if (Instance)
     {
          if (Instance->Modules)
          {
               LogCleanupStage("unloading modules");

               try
               {
                    Instance->Modules->OnUnloadModules();
                    Instance->Modules->UnloadAll();
               }
               catch (...)
               {
                    /* Ignore module teardown failures during process exit. */
               }

               Instance->Modules.reset();
               LogCleanupStage("modules unloaded");
          }

          if (Instance->Timers)
          {
               Instance->Timers.reset();
          }
     }

     /* Stop listeners before tearing down APIs, storage, and executors they may use. */

     if (Instance)
     {
          LogCleanupStage("Stopping http servers");

          for (auto *server : Instance->HTTPServers)
          {
               ShutdownHttpServer(server);
          }
          Instance->HTTPServers.clear();
          LogCleanupStage("http servers stopped");
     }

     /* Shut down critical subsystems to join all background threads early. */

     if (Instance && Instance->API)
     {
          LogCleanupStage("stopping api");
          Instance->API->Shutdown();
          Instance->API = nullptr;
          LogCleanupStage("api stopped");
     }
     else
     {
          LogCleanupStage("stopping singleton api");
          SearchAPI::GetInstance().Shutdown();
          LogCleanupStage("singleton api stopped");
     }

     if (Instance && Instance->Sam)
     {
          LogCleanupStage("stopping sam");
          Instance->Sam->Shutdown();
          Instance->Sam.reset();
          LogCleanupStage("sam stopped");
     }

     LogCleanupStage("stopping storage");
     HybridStorageManagerInstance().Shutdown();
     LogCleanupStage("storage stopped");

     if (Instance && Instance->ThreadPools)
     {
          LogCleanupStage("stopping thread pools");
          Instance->ThreadPools->Shutdown();
          Instance->ThreadPools = nullptr;
          LogCleanupStage("thread pools stopped");
     }
     else
     {
          LogCleanupStage("stopping singleton thread pools");
          ThreadPoolManager::GetInstance().Shutdown();
          LogCleanupStage("singleton thread pools stopped");
     }

     std::vector<std::thread> ThreadsToJoin;

     if (Instance)
     {
          ThreadsToJoin = Instance->TakeBackgroundThreads();
     }

     /* Joining outside the mutex prevents long shutdown stalls from blocking other owners. */

     for (auto &ThreadPtr : ThreadsToJoin)
     {
          if (ThreadPtr.joinable())
          {
               ThreadPtr.join();
          }
     }

     LogCleanupStage("background threads stopped");

     try
     {
          const char *TerminationMsg = "hlquery shutting down...\n";

          size_t MsgLenVal = 0;

          while (TerminationMsg[MsgLenVal] != '\0')
          {
               MsgLenVal++;
          }

          ssize_t FinalWriteResult = write(STDOUT_FILENO, TerminationMsg, MsgLenVal);

          (void)FinalWriteResult;
     }
     catch (...)
     {
          /* Swallow exceptions during final message output. */
     }

     try
     {
          if (Instance && Instance->Database && Instance->Database->IsOpen())
          {
               try
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", "Cleanup: Attempting database flush.");
                    }

                    Instance->Database->Flush();

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", "Cleanup: Database flush completed successfully.");
                    }
               }
               catch (const std::exception &e)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", "Cleanup: Database flush failed: " + std::string(e.what()) + ".");
                    }
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", "Cleanup: Database flush failed with unknown exception.");
                    }
               }
          }

          std::string PIDFilePathCleanup = ResolvePIDFilePath();

          /* Release the advisory lock before deleting the file so stale state is not left behind. */

          ReleasePIDFile();

          if (access(PIDFilePathCleanup.c_str(), F_OK) != 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("shutdown", "PID file removed: " + PIDFilePathCleanup + ".");
               }
          }

          Listeners.clear();

          try
          {
               /* Pending actions are cleared last because other shutdown steps may still enqueue work. */

               ActionList::ClearActions();
          }
          catch (...)
          {
               /* Ignore failures when clearing the action queue. */
          }

          CloseTrackedFD(DaemonSyncPipe[0]);
          CloseTrackedFD(DaemonSyncPipe[1]);
          CloseTrackedFD(SignalWakePipe[0]);
          CloseTrackedFD(SignalWakePipe[1]);
     }
     catch (const std::exception &e)
     {
          try
          {
               ConsoleWriter::WriteError("Exception during cleanup: " + std::string(e.what()) + ".", true);
          }
          catch (...)
          {
               /* Ignore double-faults during error reporting. */
          }
     }
     catch (...)
     {
          try
          {
               ConsoleWriter::WriteError("Unknown exception during cleanup.", true);
          }
          catch (...)
          {
               /* Ignore double-faults during error reporting. */
          }
     }
}

/* Terminates the server process with the specified exit code. */

void hlquery::Exit(int ExitCodeVal)
{
     ExitManager::Exit(ExitCodeVal);
}
