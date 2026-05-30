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
#include <array>
#include <arpa/inet.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unordered_map>

#if __has_include("core/hlcore.h")
     #include "core/hlcore.h"
#else
     #include "core/hlquery.h"
#endif
     #include "utils/tools.h"

/*
 * Return the shared rate limit map.
 */

std::unordered_map<std::string, Tools::RateLimitEntry> &Tools::GetRateLimits()
{
     static std::unordered_map<std::string, RateLimitEntry> RateLimits;
     return RateLimits;
}

/*
 * Compute an exponential backoff delay with jitter.
 */

std::chrono::seconds Tools::GetBackoffDelay(int RetryCount, int BaseDelaySeconds, int MaxDelaySeconds, int JitterPercent)
{
     /* Exponential backoff with jitter. */

     int Delay = std::min(BaseDelaySeconds * (1 << std::min(RetryCount, 10)), MaxDelaySeconds);

     /* Add jitter (±JitterPercent%). */

     int JitterAmount = (Delay * JitterPercent) / 100;
     std::uniform_int_distribution<> JitterDist(-JitterAmount, JitterAmount);
     Delay += JitterDist(GetRNG());

     return std::chrono::seconds(std::max(Delay, 30));
}

/*
 * Generate a random integer within bounds.
 */

int Tools::RandomInt(int Min, int Max)
{
     std::uniform_int_distribution<> Dist(Min, Max);
     return Dist(GetRNG());
}

/*
 * Generate a random double within bounds.
 */

double Tools::RandomDouble(double Min, double Max)
{
     std::uniform_real_distribution<> Dist(Min, Max);
     return Dist(GetRNG());
}

/*
 * Generate a random string with optional symbols.
 */

std::string Tools::RandomString(size_t Length, bool IncludeSymbols)
{
     const std::string Chars = IncludeSymbols ? "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:,.<>?" : "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

     std::string Result;
     Result.reserve(Length);
     std::uniform_int_distribution<> Dist(0, Chars.length() - 1);

     for (size_t i = 0; i < Length; ++i)
     {
          Result += Chars[Dist(GetRNG())];
     }

     return Result;
}

/*
 * Generate a random hexadecimal string.
 */

std::string Tools::RandomHex(size_t Length)
{
     const std::string HexChars = "0123456789ABCDEF";

     std::string Result;
     Result.reserve(Length);
     std::uniform_int_distribution<> Dist(0, 15);

     for (size_t i = 0; i < Length; ++i)
     {
          Result += HexChars[Dist(GetRNG())];
     }

     return Result;
}

/*
 * Generate a random byte vector.
 */

std::vector<uint8_t> Tools::RandomBytes(size_t Count)
{
     std::vector<uint8_t> Result(Count);
     std::uniform_int_distribution<uint8_t> Dist(0, 255);

     for (size_t i = 0; i < Count; ++i)
     {
          Result[i] = Dist(GetRNG());
     }

     return Result;
}

/*
 * Convert a string to lowercase.
 */

std::string Tools::ToLower(const std::string &Str)
{
     std::string Result = Str;
     std::transform(Result.begin(), Result.end(), Result.begin(), ::tolower);

     return Result;
}

/*
 * Convert a string to uppercase.
 */

std::string Tools::ToUpper(const std::string &Str)
{
     std::string Result = Str;
     std::transform(Result.begin(), Result.end(), Result.begin(), ::toupper);

     return Result;
}

/*
 * Trim leading and trailing whitespace.
 */

std::string Tools::Trim(const std::string &Str)
{
     size_t Start = Str.find_first_not_of(" \t\n\r\f\v");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Str.find_last_not_of(" \t\n\r\f\v");

     return Str.substr(Start, End - Start + 1);
}

/*
 * Split a string by a delimiter substring.
 */

std::vector<std::string> Tools::Split(const std::string &Str, const std::string &Delimiter)
{
     std::vector<std::string> Result;

     size_t Start = 0;
     size_t End = Str.find(Delimiter);

     while (End != std::string::npos)
     {
          Result.push_back(Str.substr(Start, End - Start));

          Start = End + Delimiter.length();
          End = Str.find(Delimiter, Start);
     }

     Result.push_back(Str.substr(Start));

     return Result;
}

/*
 * Join strings using a delimiter.
 */

std::string Tools::Join(const std::vector<std::string> &Strings, const std::string &Delimiter)
{
     if (Strings.empty())
     {
          return "";
     }

     std::string Result = Strings[0];

     for (size_t i = 1; i < Strings.size(); ++i)
     {
          Result += Delimiter + Strings[i];
     }

     return Result;
}

/*
 * Check whether a string starts with a prefix.
 */

bool Tools::StartsWith(const std::string &Str, const std::string &Prefix)
{
     return Str.size() >= Prefix.size() && Str.substr(0, Prefix.size()) == Prefix;
}

/*
 * Check whether a string ends with a suffix.
 */

bool Tools::EndsWith(const std::string &Str, const std::string &Suffix)
{
     return Str.size() >= Suffix.size() && Str.substr(Str.size() - Suffix.size()) == Suffix;
}

/*
 * Replace all occurrences of a substring.
 */

std::string Tools::ReplaceAll(const std::string &Str, const std::string &From, const std::string &To)
{
     std::string Result = Str;

     size_t Pos = 0;

     while ((Pos = Result.find(From, Pos)) != std::string::npos)
     {
          Result.replace(Pos, From.length(), To);

          Pos += To.length();
     }

     return Result;
}

/*
 * Validate identifier characters based on allowed options.
 */

bool Tools::IsValidIdentifier(const std::string &Str, bool AllowNumbers, bool AllowUnderscores)
{
     if (Str.empty())
     {
          return false;
     }

     for (char c : Str)
     {
          if (std::isalpha(c))
          {
               continue;
          }

          if (AllowNumbers && std::isdigit(c))
          {
               continue;
          }

          if (AllowUnderscores && c == '_')
          {
               continue;
          }

          return false;
     }

     return true;
}

/*
 * Produce a formatted timestamp string.
 */

std::string Tools::GetTimestamp(const std::string &Format)
{
     const auto NowMS = Instance ? Instance->NowMs() : NowMs();
     auto Now = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));

     auto TimeVal = std::chrono::system_clock::to_time_t(Now);

     /* Use thread-safe localtime_r instead of localtime. */

     struct tm TmBuf;

     struct tm *Tm = localtime_r(&TimeVal, &TmBuf);

     if (!Tm)
     {
          /* Fall back to a simple timestamp if localtime_r fails. */

          return std::to_string(TimeVal);
     }

     std::stringstream Ss;

     Ss << std::put_time(Tm, Format.c_str());

     return Ss.str();
}

/*
 * Return the current Unix timestamp.
 */

int64_t Tools::GetUnixTimestamp()
{
     if (Instance)
     {
          return Instance->Time();
     }

     return time(nullptr);
}

/*
 * Format a duration into a human-readable string.
 */

std::string Tools::FormatDuration(std::chrono::seconds Duration)
{
     auto TotalSeconds = Duration.count();

     int Days = TotalSeconds / 86400;

     TotalSeconds %= 86400;

     int Hours = TotalSeconds / 3600;

     TotalSeconds %= 3600;

     int Minutes = TotalSeconds / 60;

     int Seconds = TotalSeconds % 60;

     std::stringstream Ss;

     if (Days > 0)
     {
          Ss << Days << "d ";
     }

     if (Hours > 0)
     {
          Ss << Hours << "h ";
     }

     if (Minutes > 0)
     {
          Ss << Minutes << "m ";
     }

     if (Seconds > 0 || Duration.count() == 0)
     {
          Ss << Seconds << "s";
     }

     return Trim(Ss.str());
}

/*
 * Format the standard hlquery startup message with local time and loaded modules.
 */

std::string Tools::FormatStartupMessage(const std::vector<std::string> &LoadedModules)
{
     std::time_t startup_time_raw = std::time(nullptr);
     std::tm startup_time_local{};
     std::string message = "Starting hlquery:";

     auto append_loaded_modules = [&message, &LoadedModules]()
     {
          if (LoadedModules.empty())
          {
               return;
          }

          message += " modules=[";

          for (size_t i = 0; i < LoadedModules.size(); ++i)
          {
               if (i != 0)
               {
                    message += ", ";
               }

               message += LoadedModules[i];
          }

          message += "]";
     };

     if (localtime_r(&startup_time_raw, &startup_time_local) == nullptr)
     {
          append_loaded_modules();
          return message;
     }

     std::array<char, 64> startup_time_str{};

     if (std::strftime(startup_time_str.data(), startup_time_str.size(), "%b-%d - %H:%M:%S", &startup_time_local) == 0)
     {
          append_loaded_modules();
          return message;
     }

     message += " [" + std::string(startup_time_str.data()) + "]";
     append_loaded_modules();

     return message;
}

/*
 * Parse a duration like "1h30m" into seconds.
 */

std::chrono::seconds Tools::ParseDuration(const std::string &DurationStr)
{
     int64_t TotalSeconds = 0;

     int64_t CurrentValue = 0;

     auto AddUnit = [&](int64_t Multiplier) -> bool
     {
          if (CurrentValue > std::numeric_limits<int64_t>::max() / Multiplier)
          {
               return false;
          }

          int64_t Addend = CurrentValue * Multiplier;
          if (TotalSeconds > std::numeric_limits<int64_t>::max() - Addend)
          {
               return false;
          }

          TotalSeconds += Addend;
          CurrentValue = 0;
          return true;
     };

     for (size_t i = 0; i < DurationStr.length(); ++i)
     {
          char c = DurationStr[i];

          if (std::isdigit(static_cast<unsigned char>(c)))
          {
               if (CurrentValue > (std::numeric_limits<int64_t>::max() - (c - '0')) / 10)
               {
                    return std::chrono::seconds(0);
               }

               CurrentValue = CurrentValue * 10 + (c - '0');
          }
          else
          {
               /* Process accumulated value with unit. */

               switch (c)
               {
                    case 'd':
                         if (!AddUnit(86400))
                         {
                              return std::chrono::seconds(0);
                         }
                         break;
                    case 'h':
                         if (!AddUnit(3600))
                         {
                              return std::chrono::seconds(0);
                         }
                         break;
                    case 'm':
                         if (!AddUnit(60))
                         {
                              return std::chrono::seconds(0);
                         }
                         break;
                    case 's':
                         if (!AddUnit(1))
                         {
                              return std::chrono::seconds(0);
                         }
                         break;
                    default:
                         return std::chrono::seconds(0);
               }
          }
     }

     /* Handle trailing number without unit (defaults to seconds). */

     if (CurrentValue > 0)
     {
          if (TotalSeconds > std::numeric_limits<int64_t>::max() - CurrentValue)
          {
               return std::chrono::seconds(0);
          }

          TotalSeconds += CurrentValue;
     }

     return std::chrono::seconds(TotalSeconds);
}

/*
 * Validate IPv4 or IPv6 address strings.
 */

bool Tools::IsValidIP(const std::string &IP)
{
     sockaddr_in Sa4{};

     sockaddr_in6 Sa6{};

     if (inet_pton(AF_INET, IP.c_str(), &Sa4) == 1)
     {
          return true;
     }

     if (inet_pton(AF_INET6, IP.c_str(), &Sa6) == 1)
     {
          return true;
     }

     return false;
}

/*
 * Validate hostnames while allowing local aliases such as "db" or "dev-box".
 */

bool Tools::IsValidHostname(const std::string &Hostname)
{
     const std::string Normalized = NormalizeHostname(Hostname);

     if (Normalized.empty() || Normalized.size() > 253)
     {
          return false;
     }

     if (Normalized.front() == '.' || Normalized.back() == '.')
     {
          return false;
     }

     bool saw_letter = false;
     bool saw_non_dot = false;
     size_t label_length = 0;
     char previous = '\0';

     for (char character : Normalized)
     {
          const unsigned char uc = static_cast<unsigned char>(character);

          if (character == '.')
          {
               if (label_length == 0 || previous == '-')
               {
                    return false;
               }

               label_length = 0;
               previous = character;
               continue;
          }

          if (std::isalnum(uc) == 0 && character != '-')
          {
               return false;
          }

          if (label_length == 0 && character == '-')
          {
               return false;
          }

          ++label_length;

          if (label_length > 63)
          {
               return false;
          }

          if (std::isalpha(uc) != 0)
          {
               saw_letter = true;
          }

          saw_non_dot = true;
          previous = character;
     }

     if (!saw_non_dot || label_length == 0 || previous == '-')
     {
          return false;
     }

     /* Reject malformed dotted-decimal strings like "3.443.4.3434". */

     if (!saw_letter && Normalized.find('.') != std::string::npos)
     {
          return false;
     }

     return true;
}

/*
 * Validate host values used by CLI clients.
 */

bool Tools::IsValidHost(const std::string &Host)
{
     const std::string Normalized = NormalizeHostname(Host);

     if (Normalized.empty())
     {
          return false;
     }

     if (IsValidIP(Normalized))
     {
          return true;
     }

     return IsValidHostname(Normalized);
}

/*
 * Validate port ranges (1-65535).
 */

bool Tools::IsValidPort(int Port)
{
     return Port > 0 && Port <= 65535;
}

/*
 * Parse host:port pairs with a default port fallback.
 */

std::pair<std::string, int> Tools::ParseHostPort(const std::string &HostPort, int DefaultPort)
{
     size_t ColonPos = HostPort.find_last_of(':');

     if (ColonPos == std::string::npos)
     {
          return {HostPort, DefaultPort};
     }

     std::string Host = HostPort.substr(0, ColonPos);

     std::string PortStr = HostPort.substr(ColonPos + 1);

     try
     {
          int Port = std::stoi(PortStr);

          return {Host, Port};
     }
     catch (...)
     {
          return {HostPort, DefaultPort};
     }
}

/*
 * Normalize a hostname by trimming and lowercasing it.
 */

std::string Tools::NormalizeHostname(const std::string &Hostname)
{
     return ToLower(Trim(Hostname));
}

/*
 * Check whether a file path exists.
 */

bool Tools::FileExists(const std::string &Path)
{
     struct stat Buffer;

     return (stat(Path.c_str(), &Buffer) == 0);
}

/*
 * Return file size in bytes.
 */

size_t Tools::GetFileSize(const std::string &Path)
{
     struct stat Buffer;

     if (stat(Path.c_str(), &Buffer) != 0)
     {
          return 0;
     }

     return Buffer.st_size;
}

/*
 * Ensure a directory exists or create it if missing.
 */

bool Tools::EnsureDirectory(const std::string &Path)
{
     struct stat St;

     if (stat(Path.c_str(), &St) == 0)
     {
          return S_ISDIR(St.st_mode);
     }

     /* Create directory (simplified, does not handle nested paths). */

     return mkdir(Path.c_str(), 0755) == 0;
}

/*
 * Extract the file extension from a path.
 */

std::string Tools::GetFileExtension(const std::string &Path)
{
     size_t DotPos = Path.find_last_of('.');

     if (DotPos == std::string::npos)
     {
          return "";
     }

     return Path.substr(DotPos);
}

/*
 * Return a simplified SHA256 label.
 */

std::string Tools::SHA256(const std::string &Input)
{
     /* Simplified; in production use a proper crypto library. */

     return "sha256_" + std::to_string(SimpleHash(Input));
}

/*
 * Return a simplified MD5 label.
 */

std::string Tools::MD5(const std::string &Input)
{
     /* Simplified; in production use a proper crypto library. */

     return "md5_" + std::to_string(SimpleHash(Input));
}

/*
 * Compute a simple DJB2-style hash for quick use.
 */

uint32_t Tools::SimpleHash(const std::string &Input)
{
     uint32_t Hash = 5381;

     for (char c : Input)
     {
          Hash = ((Hash << 5) + Hash) + c;
     }

     return Hash;
}

/*
 * Format byte counts into human-readable units.
 */

std::string Tools::FormatBytes(size_t Bytes)
{
     const char *Units[] = {"B", "KB", "MB", "GB", "TB"};

     int UnitIndex = 0;

     double Size = static_cast<double>(Bytes);

     while (Size >= 1024 && UnitIndex < 4)
     {
          Size /= 1024;
          UnitIndex++;
     }

     std::stringstream Ss;

     Ss << std::fixed << std::setprecision(2) << Size << " " << Units[UnitIndex];

     return Ss.str();
}

/*
 * Parse a memory size string such as "512MB".
 */

size_t Tools::ParseMemorySize(const std::string &SizeStr)
{
     if (SizeStr.empty())
     {
          return 0;
     }

     size_t i = 0;

     /* Skip leading whitespace. */

     while (i < SizeStr.length() && std::isspace(static_cast<unsigned char>(SizeStr[i])))
     {
          ++i;
     }

     if (i >= SizeStr.length())
     {
          return 0;
     }

     /* Parse number (integer or decimal). */

     size_t NumStart = i;

     bool HasDot = false;

     while (i < SizeStr.length() && (std::isdigit(static_cast<unsigned char>(SizeStr[i])) || (SizeStr[i] == '.' && !HasDot)))
     {
          if (SizeStr[i] == '.')
          {
               HasDot = true;
          }

          ++i;
     }

     if (i == NumStart)
     {
          return 0;
     }

     double Value = 0.0;

     try
     {
          Value = std::stod(SizeStr.substr(NumStart, i - NumStart));
     }
     catch (...)
     {
          return 0;
     }

     /* Skip whitespace. */

     while (i < SizeStr.length() && std::isspace(static_cast<unsigned char>(SizeStr[i])))
     {
          ++i;
     }

     /* Parse unit. */

     if (i < SizeStr.length())
     {
          size_t UnitStart = i;

          while (i < SizeStr.length() && std::isalpha(static_cast<unsigned char>(SizeStr[i])))
          {
               ++i;
          }

          if (UnitStart < i)
          {
               std::string Unit = ToUpper(SizeStr.substr(UnitStart, i - UnitStart));

               if (Unit == "KB")
               {
                    Value *= 1024;
               }
               else if (Unit == "MB")
               {
                    Value *= 1024 * 1024;
               }
               else if (Unit == "GB")
               {
                    Value *= 1024 * 1024 * 1024;
               }
               else if (Unit == "TB")
               {
                    Value *= 1024LL * 1024 * 1024 * 1024;
               }
          }
     }

     return static_cast<size_t>(Value);
}

/*
 * Validate basic email formatting rules.
 */

bool Tools::IsValidEmail(const std::string &Email)
{
     if (Email.empty())
     {
          return false;
     }

     size_t AtPos = Email.find('@');

     if (AtPos == std::string::npos || AtPos == 0)
     {
          return false;
     }

     /* Validate local part (before @). */

     for (size_t i = 0; i < AtPos; ++i)
     {
          char c = Email[i];

          if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '%' && c != '+' && c != '-')
          {
               return false;
          }
     }

     /* Validate domain part (after @). */

     size_t DotPos = Email.find_last_of('.');

     if (DotPos == std::string::npos || DotPos <= AtPos || DotPos == Email.length() - 1)
     {
          return false;
     }

     /* Check TLD has at least 2 characters. */

     if (Email.length() - DotPos - 1 < 2)
     {
          return false;
     }

     /* Validate domain characters and TLD. */

     for (size_t i = AtPos + 1; i < Email.length(); ++i)
     {
          char c = Email[i];

          if (i == DotPos)
          {
               continue;
          }

          if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
          {
               return false;
          }
     }

     /* Validate TLD contains only letters. */

     for (size_t i = DotPos + 1; i < Email.length(); ++i)
     {
          if (!std::isalpha(static_cast<unsigned char>(Email[i])))
          {
               return false;
          }
     }

     return true;
}

/*
 * Validate a username with length limits.
 */

bool Tools::IsValidUsername(const std::string &Username, size_t MinLength, size_t MaxLength)
{
     if (Username.length() < MinLength || Username.length() > MaxLength)
     {
          return false;
     }

     for (char c : Username)
     {
          if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
          {
               return false;
          }
     }

     return true;
}

/*
 * Validate password length requirements.
 */

bool Tools::IsValidPassword(const std::string &Password, size_t MinLength)
{
     return Password.length() >= MinLength;
}

/*
 * Check whether a key is within its rate limit window.
 */

bool Tools::CheckRateLimit(const std::string &Key, int MaxRequests, std::chrono::seconds Window)
{
     auto *InstancePtr = Instance;

     if (!InstancePtr)
     {
          return false;
     }

     auto Now = InstancePtr->Now();

     auto &RateLimits = GetRateLimits();

     auto &Entry = RateLimits[Key];

     /* Reset window if expired. */

     if (Now - Entry.WindowStart > Window)
     {
          Entry.WindowStart = Now;
          Entry.RequestCount = 0;
     }

     /* Check if limit exceeded. */

     if (Entry.RequestCount >= MaxRequests)
     {
          return false;
     }

     /* Allow request. */

     Entry.RequestCount++;

     return true;
}

/*
 * Reset the rate limit tracking for a key.
 */

void Tools::ResetRateLimit(const std::string &Key)
{
     GetRateLimits().erase(Key);
}
