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
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unistd.h>

#include "core/config.h"
#include "core/logmanager.h"
#include "runtime/clock.h"
#include "runtime/serverconfig.h"
#include "utils/consolewriter.h"

namespace fs = std::filesystem;

/* Ensures log messages end with a period. */

static std::string EnsureLogPeriod(const std::string &Message)
{
     if (Message.empty())
     {
          return ".";
     }

     if (Message.back() == '.')
     {
          return Message;
     }

     return Message + ".";
}

/* Converts a filesystem timestamp to the system clock representation. */

static std::time_t FileTimeToTimeT(const fs::file_time_type &WriteTimeVal)
{
     const auto NowMS = NowMs();
     const auto SystemNow = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
     auto SctpTimePoint = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          WriteTimeVal - fs::file_time_type::clock::now() + SystemNow);

     return std::chrono::system_clock::to_time_t(SctpTimePoint);
}

/* Returns true only for filenames produced by GenerateRotatedFilename(). */

static bool IsGeneratedRotatedLogName(const std::string &Filename,
                                      const std::string &LogBasename,
                                      const std::string &LogExtension)
{
     const std::string RotatedExtension = LogExtension.empty() ? ".log" : LogExtension;
     const std::string TimestampPrefix = LogBasename + "_";

     if (Filename.rfind(TimestampPrefix, 0) == 0 &&
         Filename.size() > TimestampPrefix.size() + 16 + RotatedExtension.size() &&
         Filename.compare(Filename.size() - RotatedExtension.size(), RotatedExtension.size(), RotatedExtension) == 0)
     {
          const size_t DateStart = TimestampPrefix.size();
          const size_t DateEnd = DateStart + 8;

          for (size_t I = DateStart; I < DateEnd; ++I)
          {
               if (!std::isdigit(static_cast<unsigned char>(Filename[I])))
               {
                    return false;
               }
          }

          if (Filename[DateEnd] != '_')
          {
               return false;
          }

          const size_t TimeStart = DateEnd + 1;
          const size_t TimeEnd = TimeStart + 6;

          for (size_t I = TimeStart; I < TimeEnd; ++I)
          {
               if (!std::isdigit(static_cast<unsigned char>(Filename[I])))
               {
                    return false;
               }
          }

          if (Filename[TimeEnd] != '_')
          {
               return false;
          }

          const size_t SequenceStart = TimeEnd + 1;
          const size_t SequenceEnd = Filename.size() - RotatedExtension.size();

          if (SequenceStart >= SequenceEnd)
          {
               return false;
          }

          for (size_t I = SequenceStart; I < SequenceEnd; ++I)
          {
               if (!std::isdigit(static_cast<unsigned char>(Filename[I])))
               {
                    return false;
               }
          }

          return true;
     }

     const std::string LegacyPrefix = LogExtension.empty()
          ? LogBasename + ".log."
          : LogBasename + LogExtension + ".";

     if (Filename.rfind(LegacyPrefix, 0) != 0 || Filename.size() == LegacyPrefix.size())
     {
          return false;
     }

     for (size_t I = LegacyPrefix.size(); I < Filename.size(); ++I)
     {
          if (!std::isdigit(static_cast<unsigned char>(Filename[I])))
          {
               return false;
          }
     }

     return true;
}

/* LogStream implementation for handling individual logging targets. */

LogStream::LogStream(const LogConfig &Config)
    : ConfigValue(Config),
      IsOpenValue(false),
      CurrentFileSizeValue(0),
      LastRotationTime(std::time(nullptr)),
      RotationCount(0)
{
     if (ConfigValue.method == "file")
     {
          fs::path FilePath(ConfigValue.target);

          if (FilePath.has_parent_path())
          {
               try
               {
                    fs::create_directories(FilePath.parent_path());
               }
               catch (const std::exception &e)
               {
                    ConsoleWriter::WriteError("ERROR: Failed to create log directory: " + std::string(e.what()) + ".", true);
               }
          }

          FileStream = std::make_unique<std::ofstream>(ConfigValue.target, std::ios::app);

          IsOpenValue = FileStream->is_open();

          if (IsOpenValue)
          {
               CurrentFileSizeValue = GetCurrentFileSize();

               if (ConfigValue.max_size == 0 && ConfigValue.rotation_interval != 0)
               {
                    /* Use a default size limit when rotation is time-based. */

                    ConfigValue.max_size = 100 * 1024 * 1024;
               }

               WriteLog(LogLevel::LOG_NORMAL, "startup", "Log file opened: " + ConfigValue.target + ".");
          }
          else
          {
               ConsoleWriter::WriteError("ERROR: Failed to open log file: " + ConfigValue.target + ".", true);
               ConsoleWriter::WriteError("ERROR: Current working directory: " + fs::current_path().string() + ".", true);
          }
     }
     else if (ConfigValue.method == "console")
     {
          IsOpenValue = true;
     }
}

/* Destructor for LogStream ensuring file handles are closed properly. */

LogStream::~LogStream()
{
     if (FileStream && FileStream->is_open())
     {
          WriteLog(LogLevel::LOG_NORMAL, "shutdown", "Log file closing: " + ConfigValue.target + ".");

          FileStream->close();
     }
}

/* Returns true if the stream is open. */

bool LogStream::IsOpen() const
{
     return IsOpenValue;
}

/* Writes a formatted log entry to the configured target. */

void LogStream::WriteLog(LogLevel Level, const std::string &Type, const std::string &Message)
{
     std::lock_guard<std::mutex> Lock(WriteMutex);

     if (!IsOpenValue)
     {
          return;
     }

     /* Verify if log rotation is required before committing the new entry. */

     if (ConfigValue.method == "file" && ShouldRotate())
     {
          if (!RotateLogFile())
          {
               /* Report rotation failure but attempt to continue with the current file. */

               ConsoleWriter::WriteError("ERROR: Failed to rotate log file: " + ConfigValue.target + ".", true);
          }
     }

     /* Prepare the normalized message and reusable output metadata. */

     std::string LogLine;
     size_t LineSize;

     std::string FinalMessage = EnsureLogPeriod(Message);

     /* Write through the destination selected by the stream configuration. */

     if (ConfigValue.method == "file" && FileStream)
     {
          LogLine = FormatLogLine(Level, Type, FinalMessage, false);
          LineSize = LogLine.size() + 1;
          *FileStream << LogLine << std::endl;
          FileStream->flush();

          CurrentFileSizeValue += LineSize;

          /* Ensure critical log levels are immediately persisted to storage. */

          if (Level == LogLevel::LOG_CRITICAL || Level == LogLevel::LOG_NORMAL)
          {
               if (FileStream->rdbuf())
               {
                    FileStream->rdbuf()->pubsync();
               }
          }
     }
     else if (ConfigValue.method == "console")
     {
          const bool UseColors = (isatty(STDOUT_FILENO) == 1);

          LogLine = FormatLogLine(Level, Type, FinalMessage, UseColors);
          LineSize = LogLine.size() + 1;
          std::cout << LogLine << std::endl;

          std::cout.flush();
     }
}

/* Flushes any buffered data to the configured target. */

void LogStream::Flush()
{
     std::lock_guard<std::mutex> Lock(WriteMutex);

     if (FileStream)
     {
          FileStream->flush();
     }
}

/* Formats a log line without color codes. */

std::string LogStream::FormatLogLine(LogLevel Level, const std::string &Type, const std::string &Message)
{
     return FormatLogLine(Level, Type, Message, false);
}

/* Performs comprehensive formatting of log lines including timestamps and levels. */

std::string LogStream::FormatLogLine(LogLevel Level, const std::string &Type, const std::string &Message, bool UseColors)
{
     /* Build the result string manually to ensure thread safety and performance. */

     std::string Result;

     Result.reserve(256);
     Result += "[";
     Result += GetTimestamp();
     Result += "] ";

     /* Apply terminal colors only to diagnostic console output. */

     if (UseColors && Level == LogLevel::LOG_DEBUG)
     {
          Result += "\033[30m[\033[0m \033[96mDEBUG\033[0m \033[30m]\033[0m ";
     }
     else
     {
          Result += "[";
          Result += LogLevelToString(Level);
          Result += "] ";
     }

     if (!Type.empty())
     {
          Result += "[";
          Result += Type;
          Result += "] ";
     }

     /* Append the caller-provided message after all metadata fields. */

     Result += Message;
     return Result;
}

/* Retrieves a thread-safe, formatted timestamp string. */

std::string LogStream::GetTimestamp()
{
     auto NowTime = std::time(nullptr);

     struct tm TmBuf;
     struct tm *TmPtr = localtime_r(&NowTime, &TmBuf);

     if (!TmPtr)
     {
          /* Fallback to raw epoch timestamp if detailed formatting fails. */

          return std::to_string(NowTime);
     }

     char Buffer[32];
     int ResultValue = std::snprintf(Buffer, sizeof(Buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                                     TmPtr->tm_year + 1900, TmPtr->tm_mon + 1, TmPtr->tm_mday,
                                     TmPtr->tm_hour, TmPtr->tm_min, TmPtr->tm_sec);

     if (ResultValue < 0 || static_cast<size_t>(ResultValue) >= sizeof(Buffer))
     {
          return std::to_string(NowTime);
     }

     return std::string(Buffer);
}

/* Converts a LogLevel enum value to its corresponding string representation. */

std::string LogStream::LogLevelToString(LogLevel Level)
{
     switch (Level)
     {
          case LogLevel::LOG_CRITICAL:
          {
               return "CRIT";
          }

          case LogLevel::LOG_SPARSE:
          {
               return "SPAR";
          }

          case LogLevel::LOG_NORMAL:
          {
               return " OK ";
          }

          case LogLevel::LOG_VERBOSE:
          {
               return "VERB";
          }

          case LogLevel::LOG_DEBUG:
          {
               return "DEBG";
          }
          default:
          {
               return "UNKN";
          }
     }
}

/* Evaluates whether the current log file should be rotated based on size or time. */

bool LogStream::ShouldRotate()
{
     /* Rotation applies only to an active file destination. */

     if (ConfigValue.method != "file" || !FileStream || !IsOpenValue)
     {
          return false;
     }

     if (ConfigValue.max_size > 0 && CurrentFileSizeValue >= ConfigValue.max_size)
     {
          return true;
     }

     if (ConfigValue.rotation_interval != 0)
     {
          /* Resolve symbolic daily and weekly intervals to seconds. */

          std::time_t NowTime = std::time(nullptr);

          std::time_t IntervalSeconds = 0;

          if (ConfigValue.rotation_interval == -1)
          {
               /* Daily rotation period. */

               IntervalSeconds = 86400;
          }
          else if (ConfigValue.rotation_interval == -2)
          {
               /* Weekly rotation period. */

               IntervalSeconds = 604800;
          }
          else if (ConfigValue.rotation_interval > 0)
          {
               IntervalSeconds = static_cast<std::time_t>(ConfigValue.rotation_interval);
          }

          if (IntervalSeconds > 0 && (NowTime - LastRotationTime) >= IntervalSeconds)
          {
               return true;
          }
     }

     return false;
}

/* Executes the log rotation sequence by renaming the current file and opening a new one. */

bool LogStream::RotateLogFile()
{
     if (!FileStream || !IsOpenValue)
     {
          return false;
     }

     try
     {
          /* Close and rename the active file before creating its replacement. */

          FileStream->close();
          RotationCount++;
          std::string RotatedFilename = GenerateRotatedFilename(RotationCount);

          fs::path CurrentFilePath(ConfigValue.target);
          fs::path RotatedFilePath(RotatedFilename);

          if (fs::exists(CurrentFilePath))
          {
               fs::rename(CurrentFilePath, RotatedFilePath);
          }

          FileStream = std::make_unique<std::ofstream>(ConfigValue.target, std::ios::app);

          IsOpenValue = FileStream->is_open();

          if (!IsOpenValue)
          {
               return false;
          }

          /* Reset rotation bookkeeping for the new active file. */

          CurrentFileSizeValue = GetCurrentFileSize();
          LastRotationTime = std::time(nullptr);
          CleanupOldRotatedFiles();

          return true;
     }
     catch (const std::exception &e)
     {
          ConsoleWriter::WriteError("ERROR: Exception during log rotation: " + std::string(e.what()) + ".", true);
          FileStream = std::make_unique<std::ofstream>(ConfigValue.target, std::ios::app);
          IsOpenValue = FileStream->is_open();

          return false;
     }
}

/* Removes old rotated log files to satisfy the configured retention policy. */

void LogStream::CleanupOldRotatedFiles()
{
     if (ConfigValue.max_rotated_files == 0 && ConfigValue.max_age_days == 0)
     {
          return;
     }

     try
     {
          /* Discover rotated files that share the active log basename. */

          fs::path LogPath(ConfigValue.target);
          fs::path LogDir = LogPath.parent_path();

          if (LogDir.empty())
          {
               LogDir = ".";
          }

          std::string LogBasename = LogPath.stem().string();
          std::string LogExtension = LogPath.extension().string();
          std::vector<std::pair<std::time_t, fs::path>> RotatedFilesList;

          if (fs::exists(LogDir) && fs::is_directory(LogDir))
          {
               for (const auto &EntryItem : fs::directory_iterator(LogDir))
               {
                    if (EntryItem.is_regular_file())
                    {
                         std::string FilenameStr = EntryItem.path().filename().string();

                         if (FilenameStr != LogPath.filename().string() &&
                             IsGeneratedRotatedLogName(FilenameStr, LogBasename, LogExtension))
                         {
                              auto WriteTimeVal = fs::last_write_time(EntryItem);
                              std::time_t CfTimeVal = FileTimeToTimeT(WriteTimeVal);
                              RotatedFilesList.push_back({CfTimeVal, EntryItem.path()});
                         }
                    }
               }
          }

          std::sort(RotatedFilesList.begin(), RotatedFilesList.end(), [](const auto &a, const auto &b)
          {
                   return a.first < b.first;
          });

          /* Remove files that exceed the configured age limit. */

          const std::time_t NowTime = std::time(nullptr);

          if (ConfigValue.max_age_days > 0)
          {
               const std::time_t MaxAgeSeconds = static_cast<std::time_t>(ConfigValue.max_age_days) * 86400;

               for (const auto &RotatedFileItem : RotatedFilesList)
               {
                    if (NowTime >= RotatedFileItem.first && (NowTime - RotatedFileItem.first) >= MaxAgeSeconds)
                    {
                         try
                         {
                              fs::remove(RotatedFileItem.second);
                         }
                         catch (...)
                         {
                              /* Ignore failures during background file cleanup. */
                         }
                    }
               }

               RotatedFilesList.erase(std::remove_if(RotatedFilesList.begin(), RotatedFilesList.end(),
               [&](const auto &RotatedFileItem)
               {
                                        return !fs::exists(RotatedFileItem.second);
               }),
               
               RotatedFilesList.end());
          }

          if (ConfigValue.max_rotated_files > 0 && RotatedFilesList.size() > ConfigValue.max_rotated_files)
          {
               /* Remove the oldest surplus files first. */

               size_t FilesToDeleteCount = RotatedFilesList.size() - ConfigValue.max_rotated_files;

               for (size_t i = 0; i < FilesToDeleteCount; ++i)
               {
                    try
                    {
                         fs::remove(RotatedFilesList[i].second);
                    }
                    catch (...)
                    {
                         /* Ignore failures during background file cleanup. */
                    }
               }
          }
     }
     catch (const std::exception &e)
     {
          /* Log rotation cleanup is non-critical; suppress errors. */
     }
}

/* Generates a unique filename for a rotated log file using timestamps or sequences. */

std::string LogStream::GenerateRotatedFilename(size_t SequenceNum)
{
     /* Preserve the original directory, basename, and extension. */

     fs::path LogPath(ConfigValue.target);
     fs::path LogDir = LogPath.parent_path();
     std::string LogBasename = LogPath.stem().string();
     std::string LogExtension = LogPath.extension().string();
     std::time_t NowTime = std::time(nullptr);

     struct tm TmBuf;
     struct tm *TmPtr = localtime_r(&NowTime, &TmBuf);

     if (TmPtr)
     {
          /* Combine local time and a sequence number to avoid collisions. */

          char TimestampStr[32];

          std::snprintf(TimestampStr, sizeof(TimestampStr), "%04d%02d%02d_%02d%02d%02d",
                        TmPtr->tm_year + 1900, TmPtr->tm_mon + 1, TmPtr->tm_mday,
                        TmPtr->tm_hour, TmPtr->tm_min, TmPtr->tm_sec);

          const std::string RotatedExtension = LogExtension.empty() ? ".log" : LogExtension;
          fs::path CandidatePath;

          do
          {
               CandidatePath = LogDir / (LogBasename + "_" + std::string(TimestampStr) + "_" +
                                         std::to_string(SequenceNum++) + RotatedExtension);
          }
          while (fs::exists(CandidatePath));

          return CandidatePath.string();
     }
     else
     {
          /* Fall back to a sequence-only filename if local time conversion fails. */

          if (LogExtension.empty())
          {
               return (LogDir / (LogBasename + ".log." + std::to_string(SequenceNum))).string();
          }
          else
          {
               std::string ExtWithoutDot = LogExtension.substr(1);

               return (LogDir / (LogBasename + "." + ExtWithoutDot + "." + std::to_string(SequenceNum))).string();
          }
     }
}

/* Retrieves the current size of the log file from the file system. */

size_t LogStream::GetCurrentFileSize()
{
     if (ConfigValue.method != "file")
     {
          return 0;
     }

     try
     {
          if (fs::exists(ConfigValue.target))
          {
               return fs::file_size(ConfigValue.target);
          }
     }
     catch (const std::exception &)
     {
          /* Swallow file system inspection errors. */
     }

     return 0;
}

/* LogManager implementation for coordinating multiple log streams. */

LogManager::LogManager()
    : Sentinel(SENTINEL_VALUE),
      Initialized(false),
      DebugMode(false),
      NoForkMode(false),
      VerboseMode(false)
{

}

/* Destructor for LogManager ensuring all active streams are flushed. */

LogManager::~LogManager()
{
     FlushAll();
}

/* Configures and initializes the logging manager with multiple streams. */

bool LogManager::Initialize(const std::vector<LogConfig> &LogConfigs, bool DebugFlag, bool NoForkFlag, bool VerboseFlag)
{
     std::lock_guard<std::mutex> Lock(ManagerMutex);

     /* Capture runtime modes before constructing output streams. */

     this->DebugMode 	   =   DebugFlag;
     this->NoForkMode      =   NoForkFlag;
     this->VerboseMode     =   VerboseFlag;

     if (this->VerboseMode && !this->DebugMode)
     {
          this->DebugMode  =   true;
     }

     LogStreams.clear();

     CreateLogsDirectory(HLQUERY_LOG_DIR);

     /* Provide console logging when no stream configuration is available. */

     if (LogConfigs.empty())
     {
          LogConfig DefaultConfig;

          DefaultConfig.method = "console";
          DefaultConfig.type = "*";
          DefaultConfig.level = LogLevel::LOG_NORMAL;
          DefaultConfig.target = "console";

          auto StreamInstance = std::make_unique<LogStream>(DefaultConfig);

          if (StreamInstance->IsOpen())
          {
               LogStreams.push_back(std::move(StreamInstance));
          }
     }
     else
     {
          for (size_t i = 0; i < LogConfigs.size(); ++i)
          {
               const auto &ConfigItem = LogConfigs[i];
               auto StreamInstance = std::make_unique<LogStream>(ConfigItem);

               if (StreamInstance->IsOpen())
               {
                    LogStreams.push_back(std::move(StreamInstance));
               }
               else
               {
                    ConsoleWriter::WriteError("ERROR: Failed to open log stream: " + ConfigItem.target + ".", true);
               }
          }

          if (this->NoForkMode && this->DebugMode)
          {
               /* Ensure foreground diagnostic sessions have a console destination. */

               bool HasConsoleFlag = false;

               for (const auto &StreamItem : LogStreams)
               {
                    if (StreamItem->ConfigValue.method == "console")
                    {
                         HasConsoleFlag = true;
                         break;
                    }
               }

               if (!HasConsoleFlag)
               {
                    LogConfig ConsoleConfig;

                    ConsoleConfig.method = "console";
                    ConsoleConfig.type = "*";
                    ConsoleConfig.level = LogLevel::LOG_NORMAL;
                    ConsoleConfig.target = "console";

                    auto ConsoleStreamInstance = std::make_unique<LogStream>(ConsoleConfig);

                    if (ConsoleStreamInstance->IsOpen())
                    {
                         LogStreams.push_back(std::move(ConsoleStreamInstance));
                    }
               }
          }
     }

     Initialized = !LogStreams.empty();
     return Initialized;
}

/* Factory method to create and initialize a LogManager instance based on server config. */

std::unique_ptr<LogManager> LogManager::CreateAndInitialize(class ServerConfig *ConfigPointer)
{
     auto LogsInstance = std::make_unique<LogManager>();

     if (!ConfigPointer)
     {
          ConsoleWriter::WriteError("[FATAL] Config is null in LogManager::CreateAndInitialize().", true);
          return nullptr;
     }

     const std::string &ConfigFileLocation = ConfigPointer->GetConfigFile();

     if (ConfigPointer->GetDebugMode())
     {
          ConsoleWriter::WriteDebug("LogManager::CreateAndInitialize: Loading config file: " + ConfigFileLocation + ".");
     }

     if (!ConfigPointer->LoadConfig(ConfigFileLocation))
     {
          /* Fall back to console output when configuration loading fails. */

          LogConfig DefaultConfig;

          DefaultConfig.method = "console";
          DefaultConfig.type = "*";
          DefaultConfig.level = LogLevel::LOG_NORMAL;
          DefaultConfig.target = "console";

          if (!LogsInstance || !LogsInstance->Initialize({DefaultConfig}, false, true, ConfigPointer ? ConfigPointer->GetVerboseMode() : false))
          {
               ConsoleWriter::WriteError("[FATAL] Failed to initialize LogManager with default config.", true);

               return nullptr;
          }
     }
     else
     {
          auto LogConfigsList = ConfigPointer->GetLogConfigs();

          if (!LogsInstance || !LogsInstance->Initialize(LogConfigsList, ConfigPointer->GetDebugMode(), ConfigPointer->GetNoForkMode(), ConfigPointer->GetVerboseMode()))
          {
               ConsoleWriter::WriteError("[FATAL] Failed to initialize LogManager with config file: " + ConfigFileLocation + ".", true);

               return nullptr;
          }
     }

     return LogsInstance;
}

/* Primary logging entry point that dispatches messages to relevant streams. */

void LogManager::Log(LogLevel LevelValue, const std::string &Type, const std::string &Message)
{
     /* Reject calls made through a corrupted or stale manager instance. */

     uint32_t SentinelCheckVal = Sentinel;

     if (SentinelCheckVal != SENTINEL_VALUE)
     {
          ConsoleWriter::WriteError("[LOG_ERROR] LogManager::Log called with invalid object.", true);

          return;
     }

     std::string FinalMessage = EnsureLogPeriod(Message);

     /* Keep manager state protected while dispatching so reinitialization cannot
      * invalidate stream objects selected for this message. */

     std::lock_guard<std::mutex> Lock(ManagerMutex);

     if (!Initialized)
     {
          /* Preserve message visibility through standard error before initialization. */

          const char *LevelStrValue = "UNKNOWN";

          switch (LevelValue)
          {
               case LogLevel::LOG_NONE:
               {
                    LevelStrValue = "NONE";
                    break;
               }

               case LogLevel::LOG_CRITICAL:
               {
                    LevelStrValue = "CRITICAL";
                    break;
               }

               case LogLevel::LOG_SPARSE:
               {
                    LevelStrValue = "SPARSE";
                    break;
               }

               case LogLevel::LOG_NORMAL:
               {
                    LevelStrValue = " OK ";
                    break;
               }

               case LogLevel::LOG_VERBOSE:
               {
                    LevelStrValue = "VERBOSE";
                    break;
               }

               case LogLevel::LOG_DEBUG:
               {
                    LevelStrValue = "DEBUG";
                    break;
               }
          }

          std::cerr << "[" << LevelStrValue << "] [" << Type << "] " << FinalMessage << std::endl;

          return;
     }

     bool HasConsoleStreamFlag = false;

     /* Dispatch the message to each matching configured stream. */

     for (const auto &StreamPtr : LogStreams)
     {
          if (StreamPtr && ShouldLog(*StreamPtr, LevelValue, Type))
          {
               StreamPtr->WriteLog(LevelValue, Type, Message);

               if (StreamPtr->ConfigValue.method == "console")
               {
                    HasConsoleStreamFlag = true;
               }
          }
     }

     if (VerboseMode && !HasConsoleStreamFlag && (LevelValue == LogLevel::LOG_DEBUG || LevelValue == LogLevel::LOG_VERBOSE))
     {
          /* Mirror detailed foreground output when no console stream handled it. */

          const char *LevelStrFinal = "UNKNOWN";

          switch (LevelValue)
          {
               case LogLevel::LOG_DEBUG:
               {
                    LevelStrFinal = "DEBUG";
                    break;
               }
               case LogLevel::LOG_VERBOSE:
               {
                    LevelStrFinal = "VERBOSE";
                    break;
               }
               default:
               {
                    break;
               }
          }

          std::cerr << "[" << LevelStrFinal << "] [" << Type << "] " << FinalMessage << std::endl;
     }
}

/* Logging method for critical failure scenarios. */

void LogManager::Critical(const std::string &Type, const std::string &Message)
{
     SafeLog(this, LogLevel::LOG_CRITICAL, Type, Message);
}

/* Logging method for sparse, low-frequency event reports. */

void LogManager::Sparse(const std::string &Type, const std::string &Message)
{
     Log(LogLevel::LOG_SPARSE, Type, Message);
}

/* Logging method for normal operational status reports. */

void LogManager::Normal(const std::string &Type, const std::string &Message)
{
     SafeLog(this, LogLevel::LOG_NORMAL, Type, Message);
}

/* Logging method for detailed operational reports. */

void LogManager::Verbose(const std::string &Type, const std::string &Message)
{
     Log(LogLevel::LOG_VERBOSE, Type, Message);
}

/* Logging method for fine-grained diagnostic reports. */

void LogManager::Debug(const std::string &Type, const std::string &Message)
{
     SafeLog(this, LogLevel::LOG_DEBUG, Type, Message);
}

/* A robust logging utility that performs validation checks before attempting to log. */

void LogManager::SafeLog(LogManager *SelfPointer, LogLevel LevelVal, const std::string &Type, const std::string &Message)
{
     /* Use standard error when no manager instance is available. */

     if (!SelfPointer)
     {
          std::string FinalMessage = EnsureLogPeriod(Message);

          const char *LevelStrFallback = "UNKNOWN";

          switch (LevelVal)
          {
               case LogLevel::LOG_NONE:
               {
                    LevelStrFallback = "NONE";
                    break;
               }

               case LogLevel::LOG_CRITICAL:
               {
                    LevelStrFallback = "CRITICAL";
                    break;
               }

               case LogLevel::LOG_SPARSE:
               {
                    LevelStrFallback = "SPARSE";
                    break;
               }

               case LogLevel::LOG_NORMAL:
               {
                    LevelStrFallback = " OK ";
                    break;
               }

               case LogLevel::LOG_VERBOSE:
               {
                    LevelStrFallback = "VERBOSE";
                    break;
               }

               case LogLevel::LOG_DEBUG:
               {
                    LevelStrFallback = "DEBUG";
                    break;
               }
          }

          std::cerr << "[" << LevelStrFallback << "] [" << Type << "] " << FinalMessage << std::endl;

          return;
     }

     if (SelfPointer->Sentinel != SENTINEL_VALUE)
     {
          /* Report invalid state and retain the original message through fallback output. */

          ConsoleWriter::WriteError("[LOG_ERROR] LogManager object has invalid sentinel.", true);

          std::string FinalMessage = EnsureLogPeriod(Message);

          const char *LevelStrSentinel = "UNKNOWN";

          switch (LevelVal)
          {
               case LogLevel::LOG_NONE:
               {
                    LevelStrSentinel = "NONE";
                    break;
               }

               case LogLevel::LOG_CRITICAL:
               {
                    LevelStrSentinel = "CRITICAL";
                    break;
               }

               case LogLevel::LOG_SPARSE:
               {
                    LevelStrSentinel = "SPARSE";
                    break;
               }

               case LogLevel::LOG_NORMAL:
               {
                    LevelStrSentinel = " OK ";
                    break;
               }

               case LogLevel::LOG_VERBOSE:
               {
                    LevelStrSentinel = "VERBOSE";
                    break;
               }

               case LogLevel::LOG_DEBUG:
               {
                    LevelStrSentinel = "DEBUG";
                    break;
               }
          }

          std::cerr << "[" << LevelStrSentinel << "] [" << Type << "] " << FinalMessage << std::endl;

          return;
     }

     SelfPointer->Log(LevelVal, Type, Message);
}

/* Ensures the existence of the directory where log files will be persisted. */

bool LogManager::CreateLogsDirectory(const std::string &PathStr)
{
     try
     {
          fs::create_directories(PathStr);
          return true;
     }
     catch (const fs::filesystem_error &e)
     {
          ConsoleWriter::WriteError("Failed to create logs directory: " + std::string(e.what()) + ".", true);
          return false;
     }
}

/* Maps a string representation of a log level to its corresponding enum value. */

LogLevel LogManager::StringToLogLevel(const std::string &LevelStr)
{
     std::string LowerLevelValue = LevelStr;

     std::transform(LowerLevelValue.begin(), LowerLevelValue.end(), LowerLevelValue.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });

     if (LowerLevelValue == "critical" || LowerLevelValue == "crit")
     {
          return LogLevel::LOG_CRITICAL;
     }

     if (LowerLevelValue == "sparse" || LowerLevelValue == "spar")
     {
          return LogLevel::LOG_SPARSE;
     }

     if (LowerLevelValue == "normal" || LowerLevelValue == "norm")
     {
          return LogLevel::LOG_NORMAL;
     }

     if (LowerLevelValue == "verbose" || LowerLevelValue == "verb")
     {
          return LogLevel::LOG_VERBOSE;
     }

     if (LowerLevelValue == "debug" || LowerLevelValue == "debg")
     {
          return LogLevel::LOG_DEBUG;
     }

     return LogLevel::LOG_NORMAL;
}

/* Flushes all active log streams to ensure data persistence. */

void LogManager::FlushAll()
{
     std::lock_guard<std::mutex> Lock(ManagerMutex);

     for (const auto &Stream : LogStreams)
     {
          Stream->Flush();
     }
}

/* Determines if a message should be emitted by a specific stream based on configured filters. */

bool LogManager::ShouldLog(const LogStream &StreamInstance, LogLevel LevelVal, const std::string &Type)
{
     /* Query-only streams reject every unrelated message type. */

     if (StreamInstance.ConfigValue.type == "query")
     {
          if (Type != "query")
          {
               return false;
          }

          if (this->DebugMode && LevelVal == LogLevel::LOG_DEBUG)
          {
               return true;
          }

          if (LevelVal > StreamInstance.ConfigValue.level)
          {
               return false;
          }

          return true;
     }

     if (this->DebugMode && LevelVal == LogLevel::LOG_DEBUG)
     {
          return true;
     }

     if (LevelVal > StreamInstance.ConfigValue.level)
     {
          return false;
     }

     if (StreamInstance.ConfigValue.type == "*")
     {
          return true;
     }

     /* Exact type matching applies to every non-wildcard stream. */

     return (Type == StreamInstance.ConfigValue.type);
}

/* Returns the total count of active log streams managed by this instance. */

size_t LogManager::GetLogCount() const
{
     std::lock_guard<std::mutex> Lock(ManagerMutex);
     return LogStreams.size();
}

/* Returns true if debug mode is enabled. */

bool LogManager::GetDebugMode() const
{
     std::lock_guard<std::mutex> Lock(ManagerMutex);
     return DebugMode;
}

/* Resets internal state after a process fork to ensure operational integrity. */

void LogManager::ResetAfterFork()
{
     /* Handle post-fork synchronization state resets here if needed. */
}
