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

#pragma once

#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"

/* Forward declaration for server configuration. */

class ServerConfig;

/* Log severity levels. */

enum class LogLevel
{
     LOG_NONE = 0,
     LOG_CRITICAL = 1,
     LOG_SPARSE = 2,
     LOG_NORMAL = 3,
     LOG_VERBOSE = 4,
     LOG_DEBUG = 5
};

/* Log target types. */

enum class LogTarget
{
     LOG_FILE,
     LOG_CONSOLE
};

/* Individual log configuration. */

struct LogConfig
{
     /* Output routing configuration. */

     std::string method;

     std::string type;

     /* Output severity and target. */

     LogLevel level;

     std::string target;

     /* Log rotation configuration. */

     size_t max_size;

     int rotation_interval;

     /* Retention policy for rotated files. */

     size_t max_rotated_files;

     /* Initializes log configuration with defaults. */

     LogConfig();
};

/* Log stream handler. */

class LogStream
{
  
  private:

     /* Allow LogManager to access internal state. */

     friend class LogManager;

     /* Primary log configuration and output stream. */

     LogConfig ConfigValue;

     std::unique_ptr<std::ofstream> FileStream;

     /* Stream state and serialization guard. */

     bool IsOpenValue;

     std::mutex WriteMutex;

     /* Rotation size tracking. */

     size_t CurrentFileSizeValue;

     std::time_t LastRotationTime;

     /* Rotation sequence counter. */

     size_t RotationCount;

     /* Formats a log line without color output. */

     std::string FormatLogLine(LogLevel level, const std::string& type, const std::string& message);

     /* Formats a log line with optional color output. */

     std::string FormatLogLine(LogLevel level, const std::string& type, const std::string& message, bool use_colors);

     /* Retrieves a formatted timestamp string. */

     std::string GetTimestamp();

     /* Converts LogLevel enum values to display strings. */

     std::string LogLevelToString(LogLevel level);

     /* Determines whether a rotation event is required. */

     bool ShouldRotate();

     /* Performs the rotation sequence for the log file. */

     bool RotateLogFile();

     /* Purges old rotated files based on retention policy. */

     void CleanupOldRotatedFiles();

     /* Builds the rotated filename for a new log file. */

     std::string GenerateRotatedFilename(size_t sequence);

     /* Gets the current size of the log file from disk. */

     size_t GetCurrentFileSize();

  public:

     /* Constructor. */

     LogStream(const LogConfig& config);

     /* Destructor. */

     ~LogStream();

     /* Returns true if the stream is open. */

     bool IsOpen() const;

     /* Writes a log message to the stream. */

     void WriteLog(LogLevel level, const std::string& type, const std::string& message);

     /* Flushes the stream. */

     void Flush();
};

/* Coordinator for multiple logging streams. */

class LogManager
{
  
  private:

     /* Sentinel values for integrity checks. */

     static constexpr uint32_t SENTINEL_VALUE = 0xDEADBEEF;

     uint32_t Sentinel = SENTINEL_VALUE;

     /* Owned log streams and synchronization guard. */

     std::vector<std::unique_ptr<LogStream>> LogStreams;

     mutable std::mutex ManagerMutex;

     /* Lifecycle state flags. */

     bool Initialized;

     bool DebugMode;

     /* Fork and verbosity state flags. */

     bool NoForkMode;

     bool VerboseMode;

     /* Determines if a stream accepts a specific message. */

     bool ShouldLog(const LogStream& stream, LogLevel level, const std::string& type);

     /* Collects matching streams for a message. */

     std::vector<LogStream*> GetStreamsForLogging(LogLevel level, const std::string& type);

  public:

     /* Constructor. */

     LogManager();

     /* Destructor. */

     ~LogManager();

     /* Static factory method to create and initialize LogManager from configuration. */

     static std::unique_ptr<LogManager> CreateAndInitialize(class ServerConfig* config);

     /* Initialize logging system from configuration. */

     bool Initialize(const std::vector<LogConfig>& log_configs, bool debug = false, bool nofork = false, bool verbose = false);

     /* Log a message with specified level and type. */

     void Log(LogLevel level, const std::string& type, const std::string& message);

     /* Convenience logging methods. */

     void Critical(const std::string& type, const std::string& message);

     void Sparse(const std::string& type, const std::string& message);

     void Normal(const std::string& type, const std::string& message);

     void Verbose(const std::string& type, const std::string& message);

     void Debug(const std::string& type, const std::string& message);

     /* Static thread-safe logging method. */

     static void SafeLog(LogManager* self, LogLevel level, const std::string& type, const std::string& message);

     /* Create default logs directory. */

     static bool CreateLogsDirectory(const std::string& path = HLQUERY_LOG_DIR);

     /* Convert string to LogLevel. */

     static LogLevel StringToLogLevel(const std::string& level_str);

     /* Flush all log streams. */

     void FlushAll();

     /* Get the number of active log streams. */

     size_t GetLogCount() const;

     /* Returns true if debug mode is enabled. */

     bool GetDebugMode() const;

     /* Reset after fork - reopen all file streams. */

     void ResetAfterFork();
};

/* Inline helpers. */

/* Returns true if the stream is open. */

inline bool LogStream::IsOpen() const
{
     return IsOpenValue;
}

/* Initializes log configuration with defaults. */

inline LogConfig::LogConfig() : level(LogLevel::LOG_NORMAL), max_size(0), rotation_interval(0),
max_rotated_files(10)
{

}

/* Returns true if debug mode is enabled. */

inline bool LogManager::GetDebugMode() const
{
     return DebugMode;
}
