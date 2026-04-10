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

/* CODE STYLE AND FORMATTING RULES
 *
 * Scope:
 * These rules must be applied to every file in the project without exception.
 *
 * Comment Style:
 * Use only block comments.
 *
 * Single line:
 * [Block comment example] Commentary like this.
 *
 * Multi-line:
 * [Block comment example]
 * Commentary line one.
 * Commentary line two.
 *
 * Never use // comments.
 *
 * Braces and Blocks:
 * Never use compact braces. Do not put the opening brace on the same line as the control statement.
 *
 * Guard If Chaining:
 * For guard checks, chained if statements may stay on one line when the second if is the only statement.
 *
 * Example:
 * if (cond) if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
 * {
 *      Instance->Logs->Debug("inverted_index", "AddDocument: Checking document count.");
 * }
 *
 * All blocks must follow this structure:
 *
 * if (condition)
 * {
 *      Statement();
 * }
 *
 * while (condition)
 * {
 *      Statement();
 * }
 *
 * There must be a newline between comments and the following code element.
 *
 * Example:
 * [Comment]
 *
 * FunctionHere();
 *
 * Indentation (Critical Rule):
 * Use spaces only. Never use tabs.
 *
 * Indentation increases with nesting:
 * - Top-level statements start at column 0.
 * - For each nested { ... } block level, indent by exactly 5 additional spaces.
 * - A statement inside one block starts with exactly 5 spaces.
 * - A statement inside a nested block (block inside a block) starts with exactly 10 spaces.
 * - And so on.
 *
 * Example:
 * if (condition)
 * {
 *      DoThing();
 *
 *      [Another if]
 *
 *      if (another)
 *      {
 *           DoOtherThing();
 *      }
 * }
 *
 * Functions:
 * Functions must be written as:
 *
 * [Comment goes here]
 *
 * void Function()
 * {
 *      Body();
 * }
 *
 * Not:
 * [Comment line 1]
 * [Comment line 2]
 * function() { }
 *
 * Variables:
 * Variables must have a newline above and below, unless there are exactly two grouped variables.
 *
 * Example:
 * [Variable description]
 *
 * var1 = 4;
 *
 * var2 = 3;
 *
 * Avoid names ending with underscore. Use vars_like_this, not vars_like_this_.
 *
 * Naming:
 * Use PascalCase for function names: LookLikeThis()
 *
 * Avoid names like: camBack()
 *
 * Logging:
 * Do not use LOG_DEBUG.
 * Always use: Instance->Logs->Debug or Instance->Logs->Normal
 *
 * Rules for log messages:
 * - Every log message must end with a period (.)
 * - Do not add extra punctuation after the period
 * - Logs must be written in a single line
 * - Keep `Instance->Logs->Debug(...)` and `Instance->Logs->Normal(...)` on one physical line (no wrapped arguments)
 *
 * Example:
 * Instance->Logs->Debug("test", "Dispatch completed.");
 *
 * Includes:
 * System headers first with angle brackets, then local headers with quotes.
 *
 * Example:
 * #include <vector>
 * #include "hlquery.h"
 *
 * Namespaces:
 * Do not add comments at the end of namespace closing braces.
 *
 * Correct:
 * namespace something
 * {
 *      StartsHere();
 *
 *      [Comment]
 *
 *      var = 1;
 * }
 *
 * Classes:
 * Private members and methods come first.
 * Public members and methods come second.
 *
 * The keywords private and public must be indented with 3 spaces.
 *
 * Spacing:
 * There must always be a newline between comments and functions or declarations.
 *
 * Loops and Conditionals:
 * All statements inside loops and conditionals must follow the indentation rule, including nested blocks.
 *
 * Example:
 * if (cond)
 * {
 *      Result.push_back(value);
 * }
 *
 * Exceptions:
 * Do not modify: hlquery::hlquery(int argc, char** argv)
 * Do not modify core signatures like: int main(int argc, char** argv)
 *
 * Data Types:
 * Prefer optimized types when appropriate: unsigned int, signed int, long, double, etc.
 *
 * Grammar and Comments:
 * Use clear, correct English grammar.
 * Add meaningful comments where useful without over-commenting.
 *
 * private or public  declarations in classes must have a new line beneath:
 *
 *    private:
 *
 *      [Internal initialization function]
 *
 * - Un constructor o function debe ser asi: 
 * TimerManager::TimerManager()
 * {
 *
 * }
 *
 * con los:
 * { 
 * 
 * } 
 *
 * separados por un espacio en blanco.
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#include "api/userauth.h"
#include "common/cryptoutils.h"
#include "core/config.h"
#include "core/configreader.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"

namespace
{
constexpr size_t kMaxUserNameLen = 256;
constexpr size_t kMaxUserTokenLen = 1024;
constexpr size_t kMaxUserDescLen = 4096;
constexpr uint32_t kMaxUserFlagsCount = 16;
constexpr size_t kMaxUsersDatSize = 64 * 1024 * 1024;

/* GetUsersEncryptionKey returns encryption key for users data. */

std::string GetUsersEncryptionKey()
{
     const char *EnvKey = std::getenv("HLQUERY_USERS_ENCRYPTION_KEY");

     if (EnvKey && *EnvKey)
     {
          return std::string(EnvKey);
     }

     static bool Warned = false;

     if (!Warned)
     {
          Warned = true;

          print_info("WARNING: Using default users.dat encryption key - set HLQUERY_USERS_ENCRYPTION_KEY.");
     }

     return "hlquery-users-acl-encryption-key-v1";
}
}
/* Initialize initializes UserAuthManager. */

bool UserAuthManager::Initialize()
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     AuthEnabled = false;

     UsersByToken.clear();
     UsersByName.clear();

     if (Instance && Instance->Config)
     {
          const ConfigReader &Reader = Instance->Config->GetConfigReader();

          if (LoadUsersFromConfigReader(Reader))
          {
               return true;
          }
     }

     std::string UsersConf = std::string(HLQUERY_CONFIG_DIR) + "/users.conf";

     if (std::filesystem::exists(UsersConf))
     {
          if (LoadUsersFromConfig(UsersConf))
          {
               return true;
          }
     }

     std::string UsersDat = std::string(HLQUERY_ADMIN_DIR) + "/users.dat";

     if (std::filesystem::exists(UsersDat))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("auth", "Loading users from users.dat (users.conf not found).");
          }

          print_info("Loading users from users.dat (users.conf not found).");

          return LoadUsersFromEncryptedFile(UsersDat);
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("auth", "No users.conf or users.dat found - authentication DISABLED.");
     }

     print_ok("Authentication disabled (no users.conf or users.dat).");

     print_info("To enable authentication: create users.conf or use ./run/bin/hlquery --reset.");

     AuthEnabled = false;

     return true;
}

/* LoadUsersFromConfig loads users from configuration file. */

bool UserAuthManager::LoadUsersFromConfig(const std::string &ConfigFile)
{
     return ParseConfigFile(ConfigFile);
}

/* LoadUsersFromConfigReader loads users from ConfigReader. */

bool UserAuthManager::LoadUsersFromConfigReader(const ConfigReader &Reader)
{
     auto AuthTag = Reader.GetTag("auth");

     if (AuthTag)
     {
          AuthEnabled = AuthTag->GetBool("enabled", false);
     }
     else
     {
          AuthEnabled = false;
     }

     auto UserTags = Reader.GetTags("user");

     if (!AuthEnabled)
     {
          UsersByToken.clear();
          UsersByName.clear();

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("auth", "Authentication disabled in config.");
          }

          return true;
     }

     for (const auto &UserTag : UserTags)
     {
          std::string Name = UserTag->GetString("name", "");
          std::string Token = UserTag->GetString("token", "");
          std::string FlagsStr = UserTag->GetString("flags", "user");
          std::string Description = UserTag->GetString("description", "");

          if (Token.empty())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "Skipping user with empty token.");
               }

               continue;
          }

          if (Name.empty())
          {
               Name = Token.substr(0, std::min(size_t(8), Token.size()));

               if (Token.size() > 8)
               {
                    Name += "...";
               }
          }

          User UserObj;

          UserObj.Name = Name;
          UserObj.Token = Token;
          UserObj.Description = Description;
          UserObj.Flags = ParseUserFlags(FlagsStr);

          UsersByToken[Token] = UserObj;
          UsersByName[Name] = UserObj;
     }

     return true;
}

/* ParseConfigFile parses users configuration file. */

bool UserAuthManager::ParseConfigFile(const std::string &ConfigFile)
{
     std::ifstream File(ConfigFile);

     if (!File.is_open())
     {
          ConsoleWriter::WriteError("Could not open users config file: " + ConfigFile + ".", true);

          return false;
     }

     std::string Line;
     std::string CurrentUserEntry = "";
     bool InUserEntry = false;

     while (std::getline(File, Line))
     {
          if (Line.empty() || Line[0] == '#' || Line.find("---") == 0)
          {
               continue;
          }

          if (Line.find("<auth") != std::string::npos)
          {
               ParseAuthSetting(Line);
          }
          else if (AuthEnabled && Line.find("<user") != std::string::npos)
          {
               CurrentUserEntry = Line;
               InUserEntry = true;
          }
          else if (AuthEnabled && InUserEntry)
          {
               CurrentUserEntry += " " + Line;

               if (Line.find(">") != std::string::npos)
               {
                    ParseUserEntry(CurrentUserEntry);

                    CurrentUserEntry = "";
                    InUserEntry = false;
               }
          }
     }

     File.close();

     if (AuthEnabled)
     {
          print_info("Loaded {} users from config.", UsersByToken.size());
     }

     print_ok("Authentication {}.", AuthEnabled ? "enabled" : "disabled");

     return true;
}

/* ParseAuthSetting parses authentication settings. */

bool UserAuthManager::ParseAuthSetting(const std::string &Line)
{
     std::regex AuthRegex("enabled=\"([^\"]+)\"");
     std::smatch Match;

     if (std::regex_search(Line, Match, AuthRegex))
     {
          std::string EnabledStr = Match[1].str();

          std::transform(EnabledStr.begin(), EnabledStr.end(), EnabledStr.begin(), ::tolower);

          AuthEnabled = (EnabledStr == "true");

          return true;
     }

     return false;
}

/* ParseUserEntry parses a user entry from configuration. */

bool UserAuthManager::ParseUserEntry(const std::string &Line)
{
     std::string Name;
     std::string Token;
     std::string FlagsStr;
     std::string Description;

     std::regex TokenRegex("token=\"([^\"]+)\"");
     std::smatch Match;

     if (std::regex_search(Line, Match, TokenRegex))
     {
          Token = Match[1].str();
     }
     else
     {
          return false;
     }

     std::regex NameRegex("name=\"([^\"]+)\"");

     if (std::regex_search(Line, Match, NameRegex))
     {
          Name = Match[1].str();
     }
     else
     {
          Name = Token.substr(0, std::min(size_t(8), Token.size()));

          if (Token.size() > 8)
          {
               Name += "...";
          }
     }

     if (Token.size() > kMaxUserTokenLen)
     {
          print_info("ERROR: User token too long in config.");

          return false;
     }

     if (!Name.empty() && Name.size() > kMaxUserNameLen)
     {
          print_info("ERROR: User name too long in config.");

          return false;
     }

     std::regex FlagsRegex("flags=\"([^\"]+)\"");

     if (std::regex_search(Line, Match, FlagsRegex))
     {
          FlagsStr = Match[1].str();
     }
     else
     {
          return false;
     }

     std::regex DescRegex("description=\"([^\"]+)\"");

     if (std::regex_search(Line, Match, DescRegex))
     {
          Description = Match[1].str();
     }

     if (!Description.empty() && Description.size() > kMaxUserDescLen)
     {
          print_info("ERROR: User description too long in config.");

          return false;
     }

     std::set<UserFlag> Flags = ParseUserFlags(FlagsStr);

     User UserObj(Name, Token, Flags, Description);

     UsersByToken[Token] = UserObj;
     UsersByName[Name] = UserObj;

     return true;
}

/* ParseUserFlags parses comma-separated user flags. */

std::set<UserFlag> UserAuthManager::ParseUserFlags(const std::string &FlagsStr)
{
     std::set<UserFlag> Flags;
     std::istringstream ISS(FlagsStr);
     std::string Flag;

     while (std::getline(ISS, Flag, ','))
     {
          Flag.erase(0, Flag.find_first_not_of(" \t"));
          Flag.erase(Flag.find_last_not_of(" \t") + 1);

          UserFlag UserFlagVal = ParseUserFlag(Flag);

          if (UserFlagVal != UserFlag::USER || Flag == "user")
          {
               Flags.insert(UserFlagVal);
          }
     }

     return Flags;
}

/* ParseUserFlag parses a single user flag. */

UserFlag UserAuthManager::ParseUserFlag(const std::string &FlagStr)
{
     std::string Flag = FlagStr;

     std::transform(Flag.begin(), Flag.end(), Flag.begin(), ::tolower);

     if (Flag == "user")
     {
          return UserFlag::USER;
     }
     else if (Flag == "admin")
     {
          return UserFlag::ADMIN;
     }

     return UserFlag::USER;
}

/* AuthenticateToken authenticates a user token. */

AuthResult UserAuthManager::AuthenticateToken(const std::string &Token)
{
     if (Token.empty())
     {
          return AuthResult(false, User(), "Empty token provided.");
     }

     std::lock_guard<std::mutex> Lock(UsersMutex);

     bool FoundPlain = false;
     bool FoundHashed = false;
     User FoundUser;

     auto It = UsersByToken.find(Token);

     if (It != UsersByToken.end())
     {
          FoundPlain = true;
          FoundUser = It->second;
     }

     if (!FoundPlain)
     {
          std::string HashedToken = Hex(SHA256(Token.data(), Token.size()));

          It = UsersByToken.find(HashedToken);

          if (It != UsersByToken.end())
          {
               FoundHashed = true;
               FoundUser = It->second;
          }
     }

     if (FoundPlain || FoundHashed)
     {
          return AuthResult(true, FoundUser);
     }

     return AuthResult(false, User(), "Invalid token.");
}

/* AuthenticateRequest authenticates an HTTP request based on headers. */

AuthResult UserAuthManager::AuthenticateRequest(const std::string &AuthHeader)
{
     std::string Token = ExtractTokenFromHeader(AuthHeader);

     if (Token.empty())
     {
          return AuthResult(false, User(), "No authentication token provided.");
     }

     return AuthenticateToken(Token);
}

/* ExtractTokenFromHeader extracts token from HTTP authentication header. */

std::string UserAuthManager::ExtractTokenFromHeader(const std::string &AuthHeader)
{
     if (AuthHeader.empty())
     {
          return "";
     }

     if (AuthHeader.find("Bearer ") == 0)
     {
          return AuthHeader.substr(7);
     }

     if (AuthHeader.find("X-API-Key: ") == 0)
     {
          return AuthHeader.substr(11);
     }

     return AuthHeader;
}

/* AddUser adds a new user. */

bool UserAuthManager::AddUser(const User &UserObj)
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     UsersByToken[UserObj.Token] = UserObj;
     UsersByName[UserObj.Name] = UserObj;

     return true;
}

/* RemoveUser removes a user by name. */

bool UserAuthManager::RemoveUser(const std::string &Name)
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     auto NameIt = UsersByName.find(Name);

     if (NameIt != UsersByName.end())
     {
          std::string Token = NameIt->second.Token;

          UsersByToken.erase(Token);
          UsersByName.erase(NameIt);

          return true;
     }

     return false;
}

/* UpdateUser updates user information. */

bool UserAuthManager::UpdateUser(const User &UserObj)
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     auto NameIt = UsersByName.find(UserObj.Name);

     if (NameIt != UsersByName.end())
     {
          UsersByToken.erase(NameIt->second.Token);
     }

     UsersByToken[UserObj.Token] = UserObj;
     UsersByName[UserObj.Name] = UserObj;

     return true;
}

/* GetUser gets user information by name. */

std::optional<User> UserAuthManager::GetUser(const std::string &Name)
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     auto It = UsersByName.find(Name);

     if (It != UsersByName.end())
     {
          return It->second;
     }

     return std::nullopt;
}

/* GetAllUsers gets all registered users. */

std::vector<User> UserAuthManager::GetAllUsers()
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     std::vector<User> Users;

     for (const auto &Pair : UsersByName)
     {
          Users.push_back(Pair.second);
     }

     return Users;
}

/* HasPermission checks if a token has a required flag. */

bool UserAuthManager::HasPermission(const std::string &Token, UserFlag RequiredFlag)
{
     AuthResult Auth = AuthenticateToken(Token);

     if (!Auth.Valid)
     {
          return false;
     }

     return Auth.UserObj.HasFlag(RequiredFlag);
}

/* IsAdmin checks if a token belongs to an admin. */

bool UserAuthManager::IsAdmin(const std::string &Token)
{
     return HasPermission(Token, UserFlag::ADMIN);
}

/* IsUser checks if a token belongs to a regular user. */

bool UserAuthManager::IsUser(const std::string &Token)
{
     return HasPermission(Token, UserFlag::USER);
}

/* IsValidToken checks if a token is valid. */

bool UserAuthManager::IsValidToken(const std::string &Token)
{
     AuthResult Auth = AuthenticateToken(Token);

     return Auth.Valid;
}

/* GenerateRandomToken generates a random secure token. */

std::string UserAuthManager::GenerateRandomToken()
{
     std::vector<uint8_t> RandomBytesVal = RandomBytes(32);

     return Hex(RandomBytesVal);
}

/* CreateRootUser creates an initial root user. */

bool UserAuthManager::CreateRootUser()
{
     std::lock_guard<std::mutex> Lock(UsersMutex);

     std::string Token = GenerateRandomToken();

     User RootUser;

     RootUser.Name = "root";
     RootUser.Token = Token;
     RootUser.Flags = {UserFlag::ADMIN, UserFlag::USER};
     RootUser.Description = "System administrator (auto-generated).";

     UsersByToken[Token] = RootUser;
     UsersByName[RootUser.Name] = RootUser;

     AuthEnabled = true;

     std::string UsersDat = std::string(HLQUERY_ADMIN_DIR) + "/users.dat";

     std::filesystem::create_directories(HLQUERY_ADMIN_DIR);

     if (!SaveUsersToEncryptedFile(UsersDat))
     {
          print_info("WARNING: Failed to save users.dat.");
     }

     std::cerr << "\n";
     std::cerr << "ROOT USER CREATED - SAVE THIS TOKEN!\n";
     std::cerr << "\n";
     std::cerr << "Username: root\n";
     std::cerr << "Token:    " << Token << "\n";
     std::cerr << "\n";
     std::cerr << "This token will NEVER be shown again!\n";
     std::cerr << std::flush;

     print_ok("Store it in a secure location.");
     print_ok("");

     return true;
}

/* SaveUsersToEncryptedFile saves users to an encrypted file. */

bool UserAuthManager::SaveUsersToEncryptedFile(const std::string &FilePath)
{
     std::ostringstream OSS(std::ios::binary);

     uint32_t UserCount = UsersByToken.size();

     OSS.write(reinterpret_cast<const char *>(&UserCount), sizeof(UserCount));

     for (const auto &Pair : UsersByToken)
     {
          const User &UserObj = Pair.second;

          if (UserObj.Name.size() > kMaxUserNameLen)
          {
               print_info("ERROR: User name too long to persist.");

               return false;
          }

          if (UserObj.Token.size() > kMaxUserTokenLen)
          {
               print_info("ERROR: User token too long to persist.");

               return false;
          }

          if (UserObj.Description.size() > kMaxUserDescLen)
          {
               print_info("ERROR: User description too long to persist.");

               return false;
          }

          if (UserObj.Flags.size() > kMaxUserFlagsCount)
          {
               print_info("ERROR: User flags count too large to persist.");

               return false;
          }

          uint32_t NameLen = static_cast<uint32_t>(UserObj.Name.size());

          OSS.write(reinterpret_cast<const char *>(&NameLen), sizeof(NameLen));
          OSS.write(UserObj.Name.data(), NameLen);

          uint32_t TokenLen = static_cast<uint32_t>(UserObj.Token.size());

          OSS.write(reinterpret_cast<const char *>(&TokenLen), sizeof(TokenLen));
          OSS.write(UserObj.Token.data(), TokenLen);

          uint32_t FlagsCount = static_cast<uint32_t>(UserObj.Flags.size());

          OSS.write(reinterpret_cast<const char *>(&FlagsCount), sizeof(FlagsCount));

          for (UserFlag FlagVal : UserObj.Flags)
          {
               uint32_t FlagValue = static_cast<uint32_t>(FlagVal);

               OSS.write(reinterpret_cast<const char *>(&FlagValue), sizeof(FlagValue));
          }

          uint32_t DescLen = static_cast<uint32_t>(UserObj.Description.size());

          OSS.write(reinterpret_cast<const char *>(&DescLen), sizeof(DescLen));
          OSS.write(UserObj.Description.data(), DescLen);
     }

     std::string Plaintext = OSS.str();
     std::string Encrypted = AES256Encrypt(Plaintext, GetUsersEncryptionKey());

     if (Encrypted.empty())
     {
          return false;
     }

     std::ofstream File(FilePath, std::ios::binary);

     if (!File.is_open())
     {
          return false;
     }

     File.write(Encrypted.data(), Encrypted.size());
     File.flush();

     if (File.rdbuf())
     {
          File.rdbuf()->pubsync();
     }

     File.close();

     return File.good();
}

/* LoadUsersFromEncryptedFile loads users from an encrypted file. */

bool UserAuthManager::LoadUsersFromEncryptedFile(const std::string &FilePath)
{
     std::ifstream File(FilePath, std::ios::binary);

     if (!File.is_open())
     {
          return false;
     }

     File.seekg(0, std::ios::end);

     std::streampos FileSizePos = File.tellg();

     File.seekg(0, std::ios::beg);

     if (FileSizePos <= 0)
     {
          return false;
     }

     if (FileSizePos > static_cast<std::streampos>(kMaxUsersDatSize))
     {
          print_info("ERROR: users.dat too large.");

          return false;
     }

     size_t FileSize = static_cast<size_t>(FileSizePos);
     std::string Encrypted(FileSize, '\0');

     if (!File.read(Encrypted.data(), FileSize))
     {
          File.close();

          return false;
     }

     File.close();

     std::string Plaintext = AES256Decrypt(Encrypted, GetUsersEncryptionKey());

     if (Plaintext.empty())
     {
          print_info("ERROR: Failed to decrypt users.dat - corrupted or wrong key!.");

          return false;
     }

     std::istringstream ISS(Plaintext, std::ios::binary);

     auto ReadValueFunc = [&](auto &Val) -> bool
     {
          return static_cast<bool>(ISS.read(reinterpret_cast<char *>(&Val), sizeof(Val)));
     };

     auto ReadStringFunc = [&](std::string &StrVal, uint32_t Len, size_t MaxLen) -> bool
     {
          if (Len > MaxLen)
          {
               return false;
          }

          if (Len == 0)
          {
               StrVal.clear();

               return true;
          }

          StrVal.resize(Len);

          return static_cast<bool>(ISS.read(&StrVal[0], Len));
     };

     uint32_t UserCount = 0;

     if (!ReadValueFunc(UserCount))
     {
          return false;
     }

     if (UserCount > 10000)
     {
          print_info("ERROR: users.dat corrupted - invalid user count: {}.", UserCount);

          return false;
     }

     for (uint32_t I = 0; I < UserCount; ++I)
     {
          User UserObj;
          uint32_t NameLen = 0;

          if (!ReadValueFunc(NameLen))
          {
               return false;
          }

          if (!ReadStringFunc(UserObj.Name, NameLen, kMaxUserNameLen))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "User name too long or truncated in users.dat.");
               }

               return false;
          }

          uint32_t TokenLen = 0;

          if (!ReadValueFunc(TokenLen))
          {
               return false;
          }

          if (!ReadStringFunc(UserObj.Token, TokenLen, kMaxUserTokenLen))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "User token too long or truncated in users.dat.");
               }

               return false;
          }

          uint32_t FlagsCount = 0;

          if (!ReadValueFunc(FlagsCount))
          {
               return false;
          }

          if (FlagsCount > kMaxUserFlagsCount)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "User flags count too large in users.dat.");
               }

               return false;
          }

          for (uint32_t J = 0; J < FlagsCount; ++J)
          {
               uint32_t FlagValue = 0;

               if (!ReadValueFunc(FlagValue))
               {
                    return false;
               }

               if (FlagValue > static_cast<uint32_t>(UserFlag::ADMIN))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("auth", "Invalid user flag in users.dat.");
                    }

                    return false;
               }

               UserObj.Flags.insert(static_cast<UserFlag>(FlagValue));
          }

          uint32_t DescLen = 0;

          if (!ReadValueFunc(DescLen))
          {
               return false;
          }

          if (!ReadStringFunc(UserObj.Description, DescLen, kMaxUserDescLen))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "User description too long or truncated in users.dat.");
               }

               return false;
          }

          UsersByToken[UserObj.Token] = UserObj;
          UsersByName[UserObj.Name] = UserObj;
     }

     AuthEnabled = true;

     print_ok("Loaded {} users from encrypted ACL.", UsersByToken.size());
     print_ok("Authentication enabled.");

     return true;
}
