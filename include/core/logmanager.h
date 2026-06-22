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

#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"

/*
 * Defines logging severity levels in ascending order of detail.
 * Stream thresholds use these values to filter emitted messages.
 */

enum class LogLevel
{
     /* Disables log output. */

     LOG_NONE       =  0,

     /* Reports failures that require immediate attention. */

     LOG_CRITICAL   =  1,

     /* Reports infrequent high-level events. */

     LOG_SPARSE     =  2,

     /* Reports normal operational events. */

     LOG_NORMAL     =  3,

     /* Reports detailed operational events. */

     LOG_VERBOSE    =  4,

     /* Reports diagnostic events. */

     LOG_DEBUG      =  5
};

/* Identifies the supported log output destinations. */

enum class LogTarget
{
     /* Writes messages to a persistent file. */

     LOG_FILE,

     /* Writes messages to the process console. */

     LOG_CONSOLE
};

/* Stores routing, severity, rotation, and retention settings for one stream. */

struct LogConfig
{
     /* Selects the output method. */

     std::string method;

     /* Filters messages by type, or accepts all types when set to an asterisk. */
     
     std::string type;

     /* Sets the highest detail level accepted by the stream. */

     LogLevel level = LogLevel::LOG_NORMAL;

     /* Names the output file or console target. */
     
     std::string target;

     /* Sets the size threshold that triggers file rotation. */

     size_t max_size = 0;

     /* Sets the time threshold that triggers file rotation. */

     int rotation_interval = 0;

     /* Limits the number of rotated files retained on disk. */

     size_t max_rotated_files = 10;

     /* Limits retention according to file age in days. */

     size_t max_age_days = 0;

     /* Initializes log configuration with defaults. */

     LogConfig() = default;
};

/*
 * Manages a single logging destination, including formatting,
 * file rotation, and synchronized writes.
 */

class CoreExport LogStream
{
   private:

     /* Allow LogManager to access internal state. */

     friend class LogManager;

     /* Stores the effective stream configuration. */

     LogConfig ConfigValue;

     /* Owns the file output stream when file logging is configured. */

     std::unique_ptr<std::ofstream> FileStream;

     /* Records whether the configured destination is available. */

     bool IsOpenValue;

     /* Serializes writes, flushes, and file rotation. */

     std::mutex WriteMutex;

     /* Tracks the current file size used by size-based rotation. */

     size_t CurrentFileSizeValue;

     /* Records when the most recent rotation completed. */

     std::time_t LastRotationTime;

     /* Rotation sequence counter. */

     size_t RotationCount;

     /* Formats a log line without terminal color sequences. */

     std::string FormatLogLine(LogLevel level, const std::string& type, const std::string& message);

     /* Formats a log line with optional terminal color sequences. */

     std::string FormatLogLine(LogLevel level, const std::string& type, const std::string& message, bool use_colors);

     /* Retrieves a formatted timestamp string. */

     std::string GetTimestamp();

     /* Converts a severity value to its fixed-width display label. */

     std::string LogLevelToString(LogLevel level);

     /* Determines whether a rotation event is required. */

     bool ShouldRotate();

     /* Performs the rotation sequence for the log file. */

     bool RotateLogFile();

     /* Purges old rotated files based on retention policy. */

     void CleanupOldRotatedFiles();

     /* Builds a unique timestamped filename for a rotated log file. */

     std::string GenerateRotatedFilename(size_t sequence);

     /* Gets the current size of the log file from disk. */

     size_t GetCurrentFileSize();

   public:

     /* Opens and prepares the destination described by the configuration. */

     LogStream(const LogConfig& config);

     /* Flushes and closes the destination. */

     ~LogStream();

     /* Returns true if the stream is open. */

     bool IsOpen() const;

     /* Formats and writes a log message to the configured destination. */

     void WriteLog(LogLevel level, const std::string& type, const std::string& message);

     /* Flushes buffered file output. */

     void Flush();
};

/*
 * Coordinates all configured log streams and routes messages
 * according to severity, type, and runtime mode flags.
 */

class CoreExport LogManager
{
   private:

     /* Defines the value used to detect invalid manager instances. */

     static constexpr uint32_t SENTINEL_VALUE = 0xDEADBEEF;

     /* Stores the per-instance integrity marker. */

     uint32_t Sentinel = SENTINEL_VALUE;

     /* Owns every configured log stream. */

     std::vector<std::unique_ptr<LogStream>> LogStreams;

     /* Protects manager state and the stream collection. */

     mutable std::mutex ManagerMutex;

     /* Lifecycle state flags. */

     bool Initialized;

     /* Enables diagnostic messages independently of stream thresholds. */

     bool DebugMode;

     /* Records whether the process remains attached to its console. */

     bool NoForkMode;

     /* Enables verbose output and diagnostic behavior. */

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

     /* Creates and initializes a manager from server configuration. */

     static std::unique_ptr<LogManager> CreateAndInitialize(class ServerConfig* config);

     /* Initializes the logging system from stream configurations and mode flags. */

     bool Initialize(const std::vector<LogConfig>& log_configs, bool debug = false, bool nofork = false, bool verbose = false);

     /* Routes a message according to its severity and type. */

     void Log(LogLevel level, const std::string& type, const std::string& message);

     /* Convenience logging methods. */

     void Critical(const std::string& type, const std::string& message);

     void Sparse(const std::string& type, const std::string& message);

     void Normal(const std::string& type, const std::string& message);

     void Verbose(const std::string& type, const std::string& message);

     void Debug(const std::string& type, const std::string& message);

     /* Safely logs through a manager pointer with fallback console output. */

     static void SafeLog(LogManager* self, LogLevel level, const std::string& type, const std::string& message);

     /* Creates the default log directory when it does not exist. */

     static bool CreateLogsDirectory(const std::string& path = HLQUERY_LOG_DIR);

     /* Converts a textual severity name to a logging level. */

     static LogLevel StringToLogLevel(const std::string& level_str);

     /* Flushes every active log stream. */

     void FlushAll();

     /* Returns the number of active log streams. */

     size_t GetLogCount() const;

     /* Returns true if debug mode is enabled. */

     bool GetDebugMode() const;

     /* Restores logging state after the process forks. */

     void ResetAfterFork();
};
