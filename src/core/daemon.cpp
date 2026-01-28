/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
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
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <mutex>
#include <sys/resource.h>
#include <sys/select.h>
#include <time.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include "api/search_api.h"
#include "common/action_list.h"
#include "core/config.h"
#include "core/exitmanager.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "rocksdb/hybrid_storage.h"
#include "common/hlquery_search_thread_pool.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"

volatile sig_atomic_t ShuttingDown = 0;

volatile sig_atomic_t SigintCount = 0;

volatile sig_atomic_t ForceExit = 0;

volatile sig_atomic_t InSignalHandler = 0;

volatile sig_atomic_t PendingShutdownSignal = 0;

static int PIDFileFD = -1;

static int DaemonSyncPipe[2] = {-1, -1};

static std::atomic<bool> ShutdownProcessingValue{false};

bool SkipAuthentication = false;

/* Returns true if authentication is skipped for the current process */

bool IsAuthenticationSkipped()
{
     try
     {
          return SkipAuthentication;
     }
     catch (...)
     {
          return false;
     }
}

/* Parse command line arguments and update the server configuration state */

void hlquery::ParseArgs()
{
     try
     {
          if (!Config)
          {
               return;
          }
        
          const auto& CmdLineVal = Config->GetCommandLine();

          int ArgcCount = CmdLineVal.argc;

          char** ArgvList = CmdLineVal.argv;
        
          if (ArgcCount == 0 || ArgvList == nullptr)
          {
               return;
          }
        
          int OptChar;

          static struct option LongOptions[] = {
              {"test",      no_argument,       0, 't'},
              {"nofork",    no_argument,       0, 'n'},
              {"debug",     no_argument,       0, 'd'},
              {"verbose",   no_argument,       0, 'v'},
              {"config",    required_argument, 0, 'c'},
              {"skip-auth", no_argument,       0, 's'},
              {"forcestop", no_argument,       0, 'f'},
              {"help",      no_argument,       0, 'h'},
              {0,           0,                 0,  0 }
          };

          int OptionIndex = 0;

          optind = 1;
          optopt = 0;
          opterr = 1;

          while ((OptChar = getopt_long(ArgcCount, ArgvList, "tnc:hsdfv", LongOptions, &OptionIndex)) != -1)
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
                         std::cout << "Usage: " << ArgvList[0] << " [options]" << std::endl;
                         std::cout << std::endl;
                         std::cout << "Options:" << std::endl;
                         std::cout << "  -t, --test          Run in test mode" << std::endl;
                         std::cout << "  -n, --nofork        Run in foreground (prevents daemonization)" << std::endl;
                         std::cout << "  -d, --debug         Enable debug logging to log files only (not terminal)" << std::endl;
                         std::cout << "  -v, --verbose       Print debug/verbose messages to terminal AND log to files" << std::endl;
                         std::cout << "  -c, --config FILE   Use specified config file" << std::endl;
                         std::cout << "  -s, --skip-auth     Skip password authentication (dev/testing only)" << std::endl;
                         std::cout << "  -f, --forcestop     Force stop running daemon and remove PID file" << std::endl;
                         std::cout << "  -h, --help          Show this help message" << std::endl;
                         std::cout << std::endl;
                         std::cout << "Note: --debug and --verbose are separate options:" << std::endl;
                         std::cout << "  --debug:   logs debug messages to log files only (no terminal output)" << std::endl;
                         std::cout << "  --verbose: logs debug messages to files AND prints them to terminal (both)" << std::endl;
                         std::cout << "  They can be used independently or together." << std::endl;
                         std::cout << std::endl;
                         ExitManager::Exit(0);
                    }
                    
                    case '?':
                    {
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
          /* Silently swallow exceptions to prevent process termination during arg parsing */
     }
}

/* Perform an async-signal-safe write operation to the standard error stream */

static void SafeWrite(const char* Msg)
{
     size_t LenVal = 0;

     while (Msg[LenVal])
     {
          LenVal++;
     }
    
     ssize_t WriteResult = write(STDERR_FILENO, Msg, LenVal);

     (void)WriteResult;
}

/* Primary signal handler function for management of process life cycle signals */

void hlquery::SetSignal(int SignalNum)
{
     if (InSignalHandler)
     {
          ExitManager::EmergencyExit(1);
     }

     InSignalHandler = 1;
    
     if (SignalNum == SIGINT)
     {
//          SafeWrite("\nShutdown signal received, cleaning up...\n");

          PendingShutdownSignal = SignalNum;

          ShuttingDown = 1;

          ForceExit = 1;

          InSignalHandler = 0;

          return;
     }
     else if (SignalNum == SIGTERM)
     {
//          SafeWrite("\nShutdown signal received, cleaning up...\n");

          PendingShutdownSignal = SignalNum;

          ShuttingDown = 1;

          ForceExit = 1;

          InSignalHandler = 0;

          return;
     }
     else if (SignalNum == SIGALRM)
     {
          SafeWrite("\nSave timeout, forcing exit!\n");

          ExitManager::EmergencyExit(1);
     }
     else if (SignalNum == SIGUSR1)
     {
          ShuttingDown = 1;

          InSignalHandler = 0;

          return;
     }
     else
     {
          ShuttingDown = 1;

          InSignalHandler = 0;
     }
}

/* Handles crash signals like SIGSEGV and SIGABRT for emergency logging and exit */

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
          const char* CrashMsg = SigNum == SIGSEGV ? 
              "[CRASH] SIGSEGV - Segmentation fault\n" :
              "[CRASH] SIGABRT - Abort signal\n";

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

/* Configures and installs all necessary signal handlers for the process */

void hlquery::SetupSignalHandlers()
{
     try
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("daemon", "Installing signal handlers.");
          }
        
          signal(SIGTERM, SetSignal);

          signal(SIGINT, SetSignal);

          signal(SIGQUIT, SetSignal);

          signal(SIGHUP, SetSignal);

          signal(SIGUSR1, SetSignal);

          signal(SIGUSR2, SetSignal);

          signal(SIGPIPE, SIG_IGN);

          signal(SIGCHLD, SIG_IGN);

          signal(SIGALRM, SetSignal);

          signal(SIGSEGV, CrashSignalHandler);

          signal(SIGABRT, CrashSignalHandler);
     }
     catch (...)
     {
          /* Ignore registration failures during early process setup */
     }
}

/* Executes deferred signal handling operations from the primary server loop */

void hlquery::ProcessDeferredSignals()
{
     bool Expected = false;

     if (!ShutdownProcessingValue.compare_exchange_strong(Expected, true))
     {
          return;
     }
    
     if (PendingShutdownSignal != 0)
     {
          sig_atomic_t SignalNum = PendingShutdownSignal;

          PendingShutdownSignal = 0;
        
          static std::mutex AlarmMutex;

          unsigned int TimeoutSec = DAEMON_SHUTDOWN_TIMEOUT_SEC;
        
          try
          {
               {
                    std::lock_guard<std::mutex> Lock(AlarmMutex);

                    alarm(0);

                    if (Instance && Instance->Database)
                    {
                         TimeoutSec = DAEMON_SHUTDOWN_TIMEOUT_EXTENDED_SEC;
                    }

                    alarm(TimeoutSec);
               }
            
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("daemon", 
                        "Processing shutdown signal " + std::to_string(SignalNum) + 
                        " (timeout: " + std::to_string(TimeoutSec) + "s).");
               }
            
               if (Instance && Instance->Database)
               {
                    bool SyncAttempted = false;

                    bool SyncSucceeded = false;

                    bool SkipSync = true;
                
                    if (!SkipSync)
                    {
                         try
                         {
                              if (Instance->Logs)
                              {
                                   Instance->Logs->Normal("daemon", "Shutdown: Stopping background flush/compaction threads.");
                              }
                        
                              if (Instance->Logs)
                              {
                                   Instance->Logs->Normal("daemon", "Shutdown: Background threads stopped, flushing database to ensure data durability.");
                              }
                        
                              SyncAttempted = true;
                        
                              if (Instance->Logs)
                              {
                                   Instance->Logs->Normal("daemon", "Shutdown: Flushing and syncing database to disk.");
                              }
                        
                              SyncSucceeded = Instance->Database->FlushAndSync();
                    
                              if (SyncSucceeded)
                              {
                                   if (Instance->Logs)
                                   {
                                        Instance->Logs->Normal("daemon", "Shutdown: Database flush and WAL sync completed successfully.");
                                   }
                              }
                              else
                              {
                                   const char* Msg = "[CRITICAL] WAL sync failed during shutdown - DATA MAY BE LOST!\n";

                                   size_t MsgLen = 0;

                                   while (Msg[MsgLen] != '\0')
                                   {
                                        MsgLen++;
                                   }

                                   ssize_t WriteResult = write(STDERR_FILENO, Msg, MsgLen);

                                   (void)WriteResult;

                                   if (Instance->Logs)
                                   {
                                        Instance->Logs->Critical("daemon", 
                                            "WAL sync failed during shutdown - DATA MAY BE LOST.");
                                   }
                              }
                         }
                         catch (const std::exception& e)
                         {
                              const char* PrefixMsg = "[CRITICAL] Database flush/sync failed during shutdown: ";

                              const char* SuffixMsg = " - DATA MAY BE LOST!\n";

                              size_t PrefixLen = 0;

                              while (PrefixMsg[PrefixLen] != '\0')
                              {
                                   PrefixLen++;
                              }

                              size_t SuffixLen = 0;

                              while (SuffixMsg[SuffixLen] != '\0')
                              {
                                   SuffixLen++;
                              }

                              size_t WhatLen = 0;

                              const char* WhatStr = e.what();

                              while (WhatStr[WhatLen] != '\0')
                              {
                                   WhatLen++;
                              }

                              ssize_t WriteResult1 = write(STDERR_FILENO, PrefixMsg, PrefixLen);

                              (void)WriteResult1;

                              ssize_t WriteResult2 = write(STDERR_FILENO, e.what(), WhatLen);

                              (void)WriteResult2;

                              ssize_t WriteResult3 = write(STDERR_FILENO, SuffixMsg, SuffixLen);

                              (void)WriteResult3;

                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("daemon", 
                                       "Database flush/sync failed during shutdown: " + 
                                       std::string(e.what()) + " - DATA MAY BE LOST.");
                              }
                         }
                         catch (...)
                         {
                              const char* ErrorMsgText = "[CRITICAL] Database flush/sync failed during shutdown (unknown error) - DATA MAY BE LOST!\n";

                              size_t MsgLen = 0;

                              while (ErrorMsgText[MsgLen] != '\0')
                              {
                                   MsgLen++;
                              }

                              ssize_t WriteResult = write(STDERR_FILENO, ErrorMsgText, MsgLen);

                              (void)WriteResult;

                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Critical("daemon", 
                                       "Database flush/sync failed during shutdown (unknown error) - DATA MAY BE LOST.");
                              }
                         }
                    
                         if (SyncAttempted && !SyncSucceeded && Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("daemon", 
                                  "FINAL WARNING: Data may not be fully persisted to disk. Check logs above for details.");
                         }
                    }
                    else
                    {
                         if (Instance->Logs)
                         {
                              Instance->Logs->Normal("daemon", "Shutdown: Skipping flush/sync (test mode - fast exit).");
                         }
                    }
               }
            
               if (SearchAPI::GetInstance().IsInitialized())
               {
                    try
                    {
                         hlquery_storage::HybridStorageManager::GetInstance().SaveDataToDisk();
                    }
                    catch (const std::exception& e)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("daemon", 
                                  "ProcessDeferredSignals: SaveDataToDisk failed: " + std::string(e.what()) + ".");
                         }
                    }
                    catch (...)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("daemon", 
                                  "ProcessDeferredSignals: SaveDataToDisk failed with unknown exception.");
                         }
                    }
               }

               {
                    std::lock_guard<std::mutex> Lock(AlarmMutex);

                    alarm(0);
               }
            
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("daemon", "Shutdown complete, exiting.");
               }
          }
          catch (...)
          {
               {
                    std::lock_guard<std::mutex> Lock(AlarmMutex);

                    alarm(0);
               }

               ShutdownProcessingValue.store(false);
          }
        
          ExitManager::Exit(0);
     }
}

/* Returns true if a shutdown request has been initiated */

bool hlquery::ShouldShutdown()
{
     return ShuttingDown != 0;
}

/* Returns true if a force exit request has been initiated */

bool hlquery::ShouldForceExit()
{
     return ForceExit != 0;
}

/* Reset all signals and shutdown counters to their default state */

void hlquery::ResetSignalCounters()
{
     SigintCount = 0;

     ShuttingDown = 0;

     ForceExit = 0;

     PendingShutdownSignal = 0;

     InSignalHandler = 0;

     ShutdownProcessingValue.store(false);
}

/* Set the internal shutdown flag to begin graceful termination */

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

/* Checks if another instance of the server is currently active */

bool hlquery::CheckExistingProcess()
{
     try
     {
          std::string PIDFileName = "hlquery.pid";

          if (Instance && Instance->Config)
          {
               std::string ServerID = Instance->Config->GetServerId();

               if (ServerID.empty() || ServerID == "001")
               {
                    const std::string& ConfigFile = Instance->Config->GetConfigFile();

                    if (ConfigFile.find("server2") != std::string::npos)
                    {
                         ServerID = "002";
                    }
               }

               if (!ServerID.empty() && ServerID != "001")
               {
                    PIDFileName = "hlquery-" + ServerID + ".pid";
               }
          }

          std::string PIDFilePath = std::string(HLQUERY_PID_DIR) + "/" + PIDFileName;
        
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
        
          if (fcntl(PIDFdGuard.get(), F_GETLK, &PIDFileLock) == 0 && PIDFileLock.l_type != F_UNLCK)
          {
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
            
               if (ExistingPIDValue <= 1 || ExistingPIDValue > 4194304)
               {
                    unlink(PIDFilePath.c_str());

                    return false;
               }
            
               int KillSignalResult = kill(ExistingPIDValue, 0);

               if (KillSignalResult == 0)
               {
                    if (ExistingPIDValue != getpid())
                    {
                         PIDFdGuard.release();

                         return true;
                    }

                    return false;
               }
               else if (errno == EPERM)
               {
                    if (ExistingPIDValue != getpid())
                    {
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
          catch (const std::exception&)
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

/* Write the process ID to the PID file for instance management */

void hlquery::WritePID()
{
     if (PIDFileFD >= 0)
     {
          close(PIDFileFD);

          PIDFileFD = -1;
     }
    
     try
     {
          try
          {
               std::filesystem::create_directories(HLQUERY_PID_DIR);
          }
          catch (const std::filesystem::filesystem_error& e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Failed to create PID directory: " + std::string(e.what()) + ".");
               }

               return;
          }

          std::string PIDFileName = "hlquery.pid";

          if (Instance && Instance->Config)
          {
               std::string ServerID = Instance->Config->GetServerId();

               if (!ServerID.empty() && ServerID != "001")
               {
                    PIDFileName = "hlquery-" + ServerID + ".pid";
               }
          }

          std::string PIDFilePath = std::string(HLQUERY_PID_DIR) + "/" + PIDFileName;
        
          int PIDFileHandle = open(PIDFilePath.c_str(), O_CREAT | O_WRONLY, 0644);

          if (PIDFileHandle < 0)
          {
               return;
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
            
               if (fcntl(PIDFdGuard.get(), F_SETLK, &lock) < 0)
               {
                    return;
               }

               PIDFdGuard.set_lock_acquired(true);
            
               if (ftruncate(PIDFdGuard.get(), 0) < 0)
               {
                    return;
               }
            
               std::string CurrentPIDStr = std::to_string(getpid());

               ssize_t BytesWrittenCount = write(PIDFdGuard.get(), CurrentPIDStr.c_str(), CurrentPIDStr.length());
            
               if (BytesWrittenCount < 0 || static_cast<size_t>(BytesWrittenCount) != CurrentPIDStr.length())
               {
                    return;
               }
            
               if (fsync(PIDFdGuard.get()) < 0)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("daemon", 
                             "Warning: fsync() failed on PID file: " + std::string(strerror(errno)) + 
                             " - PID may not be persisted to disk.");
                    }
               }
            
               PIDFileFD = PIDFdGuard.get();

               PIDFdGuard.release();
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("daemon", "Exception during PID file creation - continuing anyway.");
               }
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("daemon", "Outer exception during PID file creation - continuing anyway.");
          }
     }
}

/* Force stop a running daemon instance and remove its associated PID file */

void hlquery::ForceStop()
{
     std::string PIDFileName = "hlquery.pid";

     if (Instance && Instance->Config)
     {
          std::string ServerID = Instance->Config->GetServerId();

          if (ServerID.empty() || ServerID == "001")
          {
               const std::string& ConfigFile = Instance->Config->GetConfigFile();

               if (ConfigFile.find("server2") != std::string::npos)
               {
                    ServerID = "002";
               }
          }

          if (!ServerID.empty() && ServerID != "001")
          {
               PIDFileName = "hlquery-" + ServerID + ".pid";
          }
     }

     std::string PIDFilePath = std::string(HLQUERY_PID_DIR) + "/" + PIDFileName;
    
     std::cout << "hlquery forcestop: Attempting to stop daemon..." << std::endl;
    
     /* Check if the process ID file exists */

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
               std::cerr << "hlquery forcestop: Error opening PID file: " << strerror(errno) << "." << std::endl;
               ExitManager::Exit(1);
          }
     }
    
     if (lseek(PIDFileHandle, 0, SEEK_SET) < 0)
     {
          close(PIDFileHandle);

          std::cerr << "hlquery forcestop: Error seeking PID file: " << strerror(errno) << "." << std::endl;
          ExitManager::Exit(1);
     }
    
     char PIDBuffer[32] = {0};

     ssize_t BytesReadCount = read(PIDFileHandle, PIDBuffer, sizeof(PIDBuffer) - 1);

     close(PIDFileHandle);
    
     PIDBuffer[sizeof(PIDBuffer) - 1] = '\0';
    
     if (BytesReadCount <= 0)
     {
          std::cerr << "hlquery forcestop: PID file is empty or unreadable." << std::endl;

          unlink(PIDFilePath.c_str());

          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }
    
     pid_t TargetPIDValue = 0;

     try
     {
          TargetPIDValue = std::stoi(PIDBuffer);
     }
     catch (const std::exception& e)
     {
          std::cerr << "hlquery forcestop: Invalid PID in file: " << PIDBuffer << "." << std::endl;

          unlink(PIDFilePath.c_str());

          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }
    
     if (TargetPIDValue <= 1 || TargetPIDValue > 4194304)
     {
          std::cerr << "hlquery forcestop: Invalid PID value: " << TargetPIDValue << "." << std::endl;

          unlink(PIDFilePath.c_str());

          std::cout << "hlquery forcestop: Removed corrupted PID file." << std::endl;
          ExitManager::Exit(0);
     }
    
     std::cout << "hlquery forcestop: Found PID " << TargetPIDValue << " in PID file." << std::endl;
    
     std::string ProcCmdLinePath = "/proc/" + std::to_string(TargetPIDValue) + "/cmdline";

     std::ifstream CmdLineFileStream(ProcCmdLinePath);

     bool IsHLQueryProcess = false;

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

               int LockFileHandle = open(PIDFilePath.c_str(), O_RDWR);

               if (LockFileHandle >= 0)
               {
                    struct flock lock;
                    lock.l_type = F_WRLCK;
                    lock.l_whence = SEEK_SET;
                    lock.l_start = 0;
                    lock.l_len = 0;

                    if (fcntl(LockFileHandle, F_SETLK, &lock) == 0)
                    {
                         unlink(PIDFilePath.c_str());
                    }

                    close(LockFileHandle);
               }
               else
               {
                    unlink(PIDFilePath.c_str());
               }

               std::cout << "hlquery forcestop: Removed stale PID file." << std::endl;
               ExitManager::Exit(0);
          }
          else if (errno == EPERM)
          {
               std::cerr << "hlquery forcestop: Permission denied to signal process " << TargetPIDValue << "." << std::endl;
               std::cerr << "hlquery forcestop: Try running with sudo." << std::endl;
               ExitManager::Exit(1);
          }
          else
          {
               std::cerr << "hlquery forcestop: Error checking process " << TargetPIDValue << ": " << strerror(errno) << "." << std::endl;
               ExitManager::Exit(1);
          }
     }
    
     if (!IsHLQueryProcess && kill(TargetPIDValue, 0) == 0)
     {
          std::cerr << "hlquery forcestop: WARNING: Could not verify process " << TargetPIDValue << " is hlquery." << std::endl;
          std::cerr << "hlquery forcestop: Process exists but cmdline check failed - proceeding anyway." << std::endl;
     }
    
     if (kill(TargetPIDValue, 0) != 0)
     {
          if (errno == ESRCH)
          {
               std::cout << "hlquery forcestop: Process " << TargetPIDValue << " exited between check and signal." << std::endl;

               int LockFileHandle = open(PIDFilePath.c_str(), O_RDWR);

               if (LockFileHandle >= 0)
               {
                    struct flock lock;
                    lock.l_type = F_WRLCK;
                    lock.l_whence = SEEK_SET;
                    lock.l_start = 0;
                    lock.l_len = 0;

                    if (fcntl(LockFileHandle, F_SETLK, &lock) == 0)
                    {
                         unlink(PIDFilePath.c_str());
                    }

                    close(LockFileHandle);
               }
               else
               {
                    unlink(PIDFilePath.c_str());
               }

               ExitManager::Exit(0);
          }
     }
    
     std::cout << "hlquery forcestop: Sending SIGTERM to process " << TargetPIDValue << "..." << std::endl;

     if (kill(TargetPIDValue, SIGTERM) != 0)
     {
          std::cerr << "hlquery forcestop: Failed to send SIGTERM: " << strerror(errno) << "." << std::endl;
          ExitManager::Exit(1);
     }
    
     std::cout << "hlquery forcestop: Waiting for process to exit..." << std::endl;

     const int MaxWaitSeconds = 5;

     const int PollIntervalMS = 100;

     int WaitedMSCount = 0;

     bool ProcessHasExited = false;
    
     while (WaitedMSCount < MaxWaitSeconds * 1000)
     {
          int KillSignalCheckResult = kill(TargetPIDValue, 0);

          if (KillSignalCheckResult != 0)
          {
               if (errno == ESRCH)
               {
                    std::cout << "hlquery forcestop: Process exited gracefully." << std::endl;

                    int LockFileHandle = open(PIDFilePath.c_str(), O_RDWR);

                    if (LockFileHandle >= 0)
                    {
                         struct flock lock;
                         lock.l_type = F_WRLCK;
                         lock.l_whence = SEEK_SET;
                         lock.l_start = 0;
                         lock.l_len = 0;
                    
                         if (fcntl(LockFileHandle, F_SETLK, &lock) == 0)
                         {
                              unlink(PIDFilePath.c_str());
                         }
                    
                         close(LockFileHandle);
                    }
                    else
                    {
                         unlink(PIDFilePath.c_str());
                    }
                
                    std::cout << "hlquery forcestop: Removed PID file." << std::endl;
                    std::cout << "hlquery forcestop: SUCCESS - Daemon stopped." << std::endl;
                    ProcessHasExited = true;
                    break;
               }
               else if (errno == EPERM)
               {
                    /* Handle permission issues if they arise during polling */
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
          std::cerr << "hlquery forcestop: Failed to send SIGKILL: " << strerror(errno) << "." << std::endl;
          ExitManager::Exit(1);
     }
    
     if (kill(TargetPIDValue, 0) == 0)
     {
          std::cerr << "hlquery forcestop: Process " << TargetPIDValue << " still running after SIGKILL!" << std::endl;
          std::cerr << "hlquery forcestop: This should never happen - process may be in uninterruptible state." << std::endl;
          ExitManager::Exit(1);
     }
    
     std::cout << "hlquery forcestop: Process killed." << std::endl;

     int LockFileHandleFinal = open(PIDFilePath.c_str(), O_RDWR);

     if (LockFileHandleFinal >= 0)
     {
          struct flock lock;
          lock.l_type = F_WRLCK;
          lock.l_whence = SEEK_SET;
          lock.l_start = 0;
          lock.l_len = 0;

          if (fcntl(LockFileHandleFinal, F_SETLK, &lock) == 0)
          {
               unlink(PIDFilePath.c_str());
          }

          close(LockFileHandleFinal);
     }
     else
     {
          unlink(PIDFilePath.c_str());
     }

     std::cout << "hlquery forcestop: Removed PID file." << std::endl;
     std::cout << "hlquery forcestop: SUCCESS - Daemon force-stopped." << std::endl;
     ExitManager::Exit(0);
}

/* Configures file descriptors for various operational modes */

void hlquery::SetupFileDescriptors()
{
     try
     {
          if (Config && !Config->GetNoForkMode())
          {
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
               fflush(stderr);

               fflush(stdout);
          }
     }
     catch (...)
     {
          /* Swallow exceptions during FD setup to avoid early process crash */
     }
}

/* Completes the final daemon configuration steps after the fork operation */

void hlquery::CompleteDaemonSetup()
{
     if (Config && !Config->GetNoForkMode() && !Config->GetTestMode())
     {
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

/* Transitions the process into a daemon by forking into the background */

bool hlquery::Daemonize()
{
     DaemonSyncPipe[0] = -1;

     DaemonSyncPipe[1] = -1;
    
     try
     {
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

               std::cout << std::endl;

               fflush(stdout);
            
               ExitManager::EmergencyExit(0);
          }
        
          close(DaemonSyncPipe[0]);
        
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

          (void)ChdirResultValueVal;
        
          return true;
     }
     catch (const std::exception&)
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

/* Configures the core dump size limit to the maximum allowable value */

void hlquery::IncreaseCoreDumpSize()
{
     try
     {
          errno = 0;

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
          /* Ignore failures during resource limit configuration */
     }
}

/* Performs comprehensive cleanup of server resources and persistent state */

void hlquery::Cleanup()
{
     static std::atomic<bool> CleanupCalledFlagValue{false};

     bool ExpectedFlag = false;

     if (!CleanupCalledFlagValue.compare_exchange_strong(ExpectedFlag, true))
     {
          return;
     }
    
     /* Shut down critical subsystems to join all background threads early */

     hlquery_storage::HybridStorageManagerInstance().Shutdown();

     hlquery_threadpool::ThreadPoolManager::GetInstance().Shutdown();

     if (Instance)
     {
          for (auto* server : Instance->HTTPServers)
          {
               hlquery_server::ShutdownHttpServer(server);
          }
          Instance->HTTPServers.clear();
     }

     extern std::vector<std::thread> BackgroundThreads;

     extern std::mutex BackgroundThreadsMutex;

     {
          std::lock_guard<std::mutex> Lock(BackgroundThreadsMutex);

          for (auto& ThreadPtr : BackgroundThreads)
          {
               if (ThreadPtr.joinable())
               {
                    ThreadPtr.join();
               }
          }

          BackgroundThreads.clear();
     }
    
     try
     {
          const char* TerminationMsg = "hlquery shutting down...\n";

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
          /* Swallow exceptions during final message output */
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
               catch (const std::exception& e)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", 
                             "Cleanup: Database flush failed: " + 
                             std::string(e.what()) + ".");
                    }
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("shutdown", 
                             "Cleanup: Database flush failed with unknown exception.");
                    }
               }
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
        
          std::string PIDFileNameCleanup = "hlquery.pid";

          if (Instance && Instance->Config)
          {
               std::string ServerIDCleanup = Instance->Config->GetServerId();

               if (!ServerIDCleanup.empty() && ServerIDCleanup != "001")
               {
                    PIDFileNameCleanup = "hlquery-" + ServerIDCleanup + ".pid";
               }
          }

          std::string PIDFilePathCleanup = std::string(HLQUERY_PID_DIR) + "/" + PIDFileNameCleanup;

          if (std::remove(PIDFilePathCleanup.c_str()) == 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("shutdown", "PID file removed: " + PIDFilePathCleanup + ".");
               }
          }
        
          Listeners.clear();
        
          try
          {
               ActionList::ClearActions();
          }
          catch (...)
          {
               /* Ignore failures when clearing the action queue */
          }
        
          if (DaemonSyncPipe[0] >= 0)
          {
               close(DaemonSyncPipe[0]);

               DaemonSyncPipe[0] = -1;
          }

          if (DaemonSyncPipe[1] >= 0)
          {
               close(DaemonSyncPipe[1]);

               DaemonSyncPipe[1] = -1;
          }
     }
     catch (const std::exception& e)
     {
          try
          {
               std::cerr << "Exception during cleanup: " << e.what() << "." << std::endl;
          }
          catch (...)
          {
               /* Ignore double-faults during error reporting */
          }
     }
     catch (...)
     {
          try
          {
               std::cerr << "Unknown exception during cleanup." << std::endl;
          }
          catch (...)
          {
               /* Ignore double-faults during error reporting */
          }
     }
}

/* Terminates the server process with the specified exit code */

void hlquery::Exit(int ExitCodeVal)
{
     ExitManager::Exit(ExitCodeVal);
}
