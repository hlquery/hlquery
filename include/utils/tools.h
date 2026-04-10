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

#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/serverconfig.h"

/*
 * @brief Comprehensive tools and utilities for HLManager
 *
 * This class provides various utility functions including:
 * - Random number generation and randomization utilities
 * - String manipulation and validation
 * - Time and duration utilities
 * - Networking helpers
 * - General purpose utilities
 *
 * All methods are static - no instance needed.
 */

class CoreExport Tools
{
   public:

     /* Randomization Utilities */

     /*
      * @brief Get exponential backoff delay with jitter for connection retries
      * @param RetryCount Number of retry attempts
      * @param BaseDelaySeconds Base delay in seconds (default: 60)
      * @param MaxDelaySeconds Maximum delay in seconds (default: 1800 = 30 minutes)
      * @param JitterPercent Jitter percentage (default: 20%)
      * @return Delay duration in seconds
      */

     static std::chrono::seconds GetBackoffDelay(int RetryCount,
                                                 int BaseDelaySeconds = 60,
                                                 int MaxDelaySeconds = 1800,
                                                 int JitterPercent = 20);

     /*
      * @brief Generate random integer in range [Min, Max]
      */

     static int RandomInt(int Min, int Max);

     /*
      * @brief Generate random double in range [Min, Max)
      */

     static double RandomDouble(double Min, double Max);

     /*
      * @brief Generate random alphanumeric string
      * @param Length Length of string to generate
      * @param IncludeSymbols Whether to include symbols
      */

     static std::string RandomString(size_t Length, bool IncludeSymbols = false);

     /*
      * @brief Generate random hex string (for UIDs, tokens, etc)
      */

     static std::string RandomHex(size_t Length);

     /*
      * @brief Generate cryptographically secure random bytes
      */

     static std::vector<uint8_t> RandomBytes(size_t Count);

     /*
      * @brief Get thread-safe random number generator (32-bit)
      * @return Reference to thread-local std::mt19937 generator
      * @note Uses thread_local to ensure each thread has its own RNG instance
      */

     static std::mt19937& GetRNG()
     {
          thread_local static std::random_device Rd;
          thread_local static std::mt19937 Rng(Rd());
          return Rng;
     }

     /*
      * @brief Get thread-safe random number generator (64-bit)
      * @return Reference to thread-local std::mt19937_64 generator
      * @note Uses thread_local to ensure each thread has its own RNG instance
      */

     static std::mt19937_64& GetRNG64()
     {
          thread_local static std::random_device Rd;
          thread_local static std::mt19937_64 Rng(Rd());
          return Rng;
     }

     /* String Utilities */

     /*
      * @brief Convert string to lowercase
      */

     static std::string ToLower(const std::string& Str);

     /*
      * @brief Convert string to uppercase
      */

     static std::string ToUpper(const std::string& Str);

     /*
      * @brief Trim whitespace from both ends
      */

     static std::string Trim(const std::string& Str);

     /*
      * @brief Split string by delimiter
      */

     static std::vector<std::string> Split(const std::string& Str, const std::string& Delimiter);

     /*
      * @brief Join strings with delimiter
      */

     static std::string Join(const std::vector<std::string>& Strings, const std::string& Delimiter);

     /*
      * @brief Check if string starts with prefix
      */

     static bool StartsWith(const std::string& Str, const std::string& Prefix);

     /*
      * @brief Check if string ends with suffix
      */

     static bool EndsWith(const std::string& Str, const std::string& Suffix);

     /*
      * @brief Replace all occurrences of from with to
      */

     static std::string ReplaceAll(const std::string& Str, const std::string& From, const std::string& To);

     /*
      * @brief Validate that string contains only allowed characters
      */

     static bool IsValidIdentifier(const std::string& Str, bool AllowNumbers = true, bool AllowUnderscores = true);

     /* Time Utilities */

     /*
      * @brief Get current timestamp as string
      */

     static std::string GetTimestamp(const std::string& Format = "%Y-%m-%d %H:%M:%S");

     /*
      * @brief Get Unix timestamp
      */

     static int64_t GetUnixTimestamp();

     /*
      * @brief Format duration as human readable string
      */

     static std::string FormatDuration(std::chrono::seconds Duration);

     /*
      * @brief Format the standard hlquery startup banner
      */

     static std::string FormatStartupMessage(const std::vector<std::string>& LoadedModules = {});

     /*
      * @brief Parse duration string (e.g., "5m", "1h30m", "2d")
      */

     static std::chrono::seconds ParseDuration(const std::string& DurationStr);

     /* Networking Utilities */

     /*
      * @brief Validate IP address (IPv4 or IPv6)
      */

     static bool IsValidIP(const std::string& IP);

     /*
      * @brief Validate a hostname using conservative DNS-style label rules
      */

     static bool IsValidHostname(const std::string& Hostname);

     /*
      * @brief Validate either an IP address literal or a hostname
      */

     static bool IsValidHost(const std::string& Host);

     /*
      * @brief Validate port number
      */

     static bool IsValidPort(int Port);

     /*
      * @brief Parse host:port string
      */

     static std::pair<std::string, int> ParseHostPort(const std::string& HostPort, int DefaultPort = 0);

     /*
      * @brief Normalize hostname (lowercase, trim)
      */

     static std::string NormalizeHostname(const std::string& Hostname);

     /* File/Path Utilities */

     /*
      * @brief Check if file exists
      */

     static bool FileExists(const std::string& Path);

     /*
      * @brief Get file size in bytes
      */

     static size_t GetFileSize(const std::string& Path);

     /*
      * @brief Ensure directory exists (create if needed)
      */

     static bool EnsureDirectory(const std::string& Path);

     /*
      * @brief Get file extension
      */

     static std::string GetFileExtension(const std::string& Path);

     /* Hashing Utilities */

     /*
      * @brief Generate SHA256 hash of string
      */

     static std::string SHA256(const std::string& Input);

     /**
     * @brief Generate MD5 hash of string (for non-security uses)
     */

     static std::string MD5(const std::string& Input);

     /*
      * @brief Generate simple hash for quick lookups
      */

     static uint32_t SimpleHash(const std::string& Input);

     /* Memory Utilities */

     /*
      * @brief Format bytes as human readable string (KB, MB, GB)
      */

     static std::string FormatBytes(size_t Bytes);

     /*
      * @brief Parse memory size string ("1GB", "512MB", etc)
      */

     static size_t ParseMemorySize(const std::string& SizeStr);

     /* Validation Utilities */

     /*
      * @brief Validate email format
      */

     static bool IsValidEmail(const std::string& Email);

     /*
      * @brief Validate username format
      */

     static bool IsValidUsername(const std::string& Username, size_t MinLength = 3, size_t MaxLength = 32);

     /*
      * @brief Validate password strength
      */

     static bool IsValidPassword(const std::string& Password, size_t MinLength = 8);

     /* Rate Limiting Utilities */

     /*
      * @brief Simple rate limiter for actions
      */

     static bool CheckRateLimit(const std::string& Key, int MaxRequests, std::chrono::seconds Window);

     /*
      * @brief Reset rate limit for key
      */

     static void ResetRateLimit(const std::string& Key);

   private:

     /* Static utility class - no instances allowed */

     Tools() = delete;

     ~Tools() = delete;

     Tools(const Tools&) = delete;

     Tools& operator=(const Tools&) = delete;

     /* Helper structs for static functions */

     struct RateLimitEntry
     {
          std::chrono::steady_clock::time_point WindowStart;

          int RequestCount;
     };

     /* Static helpers */

     static std::unordered_map<std::string, RateLimitEntry>& GetRateLimits();
};

namespace RuntimePaths
{

     /*
      * Resolve the effective runtime data directory, preferring an explicit
      * environment override and then the loaded server config before falling back to
      * the compiled default path.
      */

     inline std::string ResolveRuntimeDataDir(const ServerConfig *ConfigValue)
     {
          const char *EnvDataDir = std::getenv("HLQUERY_DATA_DIR");

          if (EnvDataDir && *EnvDataDir)
          {
               return EnvDataDir;
          }

          if (ConfigValue && ConfigValue->IsValid())
          {
               try
               {
                    const auto &RocksDBOpts = ConfigValue->GetRocksDBOptions();

                    if (!RocksDBOpts.DataDir.empty())
                    {
                         return RocksDBOpts.DataDir;
                    }
               }
               catch (...)
               {
               
               }
          }

          return std::string(HLQUERY_DATA_DIR);
     }
}
