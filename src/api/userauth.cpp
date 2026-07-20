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
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <unistd.h>

#include "api/userauth.h"
#include "common/cryptoutils.h"
#include "core/config.h"
#include "runtime/configreader.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"

/* Implements user authentication configuration and token validation. */

constexpr size_t kMaxUserNameLen = 256;
constexpr size_t kMaxUserTokenLen = 1024;
constexpr size_t kMaxUserPasswordLen = 1024;
constexpr size_t kMaxUserDescLen = 4096;
constexpr uint32_t kMaxUserFlagsCount = 16;
constexpr size_t kMaxUsersDatSize = 64 * 1024 * 1024;

static std::string ReadSecret(const char *ValueEnv, const char *FileEnv, size_t MaxLength)
{
     const char *Value = std::getenv(ValueEnv);

     if (Value && *Value)
     {
          return std::string(Value);
     }

     const char *FilePath = std::getenv(FileEnv);

     if (!FilePath || !*FilePath)
     {
          return {};
     }

     std::ifstream SecretFile(FilePath, std::ios::binary);

     if (!SecretFile.is_open())
     {
          return {};
     }

     std::string Secret;
     Secret.resize(MaxLength + 1);
     SecretFile.read(Secret.data(), static_cast<std::streamsize>(Secret.size()));
     Secret.resize(static_cast<size_t>(SecretFile.gcount()));

     while (!Secret.empty() && (Secret.back() == '\n' || Secret.back() == '\r'))
     {
          Secret.pop_back();
     }

     if (Secret.size() > MaxLength)
     {
          return {};
     }

     return Secret;
}

static int Base64Value(char Ch)
{
     if (Ch >= 'A' && Ch <= 'Z') return Ch - 'A';
     if (Ch >= 'a' && Ch <= 'z') return Ch - 'a' + 26;
     if (Ch >= '0' && Ch <= '9') return Ch - '0' + 52;
     if (Ch == '+') return 62;
     if (Ch == '/') return 63;
     return -1;
}

static bool DecodeBase64(const std::string &Input, std::string &Output)
{
     Output.clear();

     int Val = 0;
     int ValBits = -8;

     for (char Ch : Input)
     {
          if (Ch == '=')
          {
               break;
          }

          if (Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n')
          {
               continue;
          }

          int Decoded = Base64Value(Ch);

          if (Decoded < 0)
          {
               return false;
          }

          Val = (Val << 6) + Decoded;
          ValBits += 6;

          if (ValBits >= 0)
          {
               Output.push_back(static_cast<char>((Val >> ValBits) & 0xFF));
               ValBits -= 8;
          }
     }

     return true;
}

/* WriteFileAtomic writes data to a temp file, fsyncs, then renames into place. */

static bool WriteFileAtomic(const std::string &FilePath, const std::string &Contents)
{
     std::error_code EC;

     std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path(), EC);

     std::string DirPath = std::filesystem::path(FilePath).parent_path().string();

     std::string TmpPath = FilePath + ".tmp." + std::to_string(static_cast<long long>(getpid())) + "." + std::to_string(::NowMs());

     int FD = open(TmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

     if (FD < 0)
     {
          return false;
     }

     size_t Offset = 0;

     while (Offset < Contents.size())
     {
          ssize_t Written = write(FD, Contents.data() + Offset, Contents.size() - Offset);

          if (Written <= 0)
          {
               close(FD);
               unlink(TmpPath.c_str());
               return false;
          }

          Offset += static_cast<size_t>(Written);
     }

     if (fsync(FD) < 0)
     {
          close(FD);
          unlink(TmpPath.c_str());
          return false;
     }

     close(FD);

     if (::rename(TmpPath.c_str(), FilePath.c_str()) != 0)
     {
          unlink(TmpPath.c_str());
          return false;
     }

     int DirFD = open(DirPath.c_str(), O_RDONLY | O_DIRECTORY);

     if (DirFD >= 0)
     {
          (void)fsync(DirFD);
          close(DirFD);
     }

     return true;
}

/* GetUsersEncryptionKey returns encryption key for users data. */

std::string GetUsersEncryptionKey()
{
     const std::string EnvKey = ReadSecret("HLQUERY_USERS_ENCRYPTION_KEY",
                                           "HLQUERY_USERS_ENCRYPTION_KEY_FILE",
                                           4096);

     if (!EnvKey.empty())
     {
          return EnvKey;
     }

     static bool Warned = false;

     if (!Warned)
     {
          Warned = true;

          print_info("WARNING: Using default users.dat encryption key - set HLQUERY_USERS_ENCRYPTION_KEY.");
     }

     return "hlquery-users-acl-encryption-key-v1";
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

          /* An explicit auth tag is authoritative. In particular, a malformed
           * or empty enabled configuration must fail closed instead of falling
           * through to the legacy "no auth files means disabled" behavior. */

          if (Reader.GetTag("auth"))
          {
               return LoadUsersFromConfigReader(Reader);
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
          std::string Password = UserTag->GetString("password", "");
          std::string FlagsStr = UserTag->GetString("flags", "user");
          std::string Description = UserTag->GetString("description", "");

          if (Token.empty() && Password.empty())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("auth", "Skipping user with empty token and password.");
               }

               continue;
          }

          if (Name.empty())
          {
               if (!Token.empty())
               {
                    Name = Token.substr(0, std::min(size_t(8), Token.size()));

                    if (Token.size() > 8)
                    {
                         Name += "...";
                    }
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("auth", "Skipping password user with empty name.");
                    }
                    continue;
               }
          }

          User UserObj;

          UserObj.Name = Name;
          UserObj.Token = Token;
          UserObj.Password = Password;
          UserObj.Description = Description;
          UserObj.Flags = ParseUserFlags(FlagsStr);

          if (!Token.empty())
          {
               UsersByToken[Token] = UserObj;
          }

          UsersByName[Name] = UserObj;
     }

     /* Production deployments can inject the initial administrator token
      * without writing a secret into a world-readable configuration file. */

     if (UsersByToken.empty())
     {
          const std::string AdminToken = ReadSecret("HLQUERY_ADMIN_TOKEN",
                                                    "HLQUERY_ADMIN_TOKEN_FILE",
                                                    kMaxUserTokenLen);

          if (!AdminToken.empty())
          {
               User AdminUser;
               AdminUser.Name = "root";
               AdminUser.Token = AdminToken;
               AdminUser.Description = "Environment-provisioned administrator.";
               AdminUser.Flags = {UserFlag::ADMIN, UserFlag::USER};

               UsersByToken[AdminToken] = AdminUser;
               UsersByName[AdminUser.Name] = AdminUser;
          }
     }

     if (UsersByToken.empty())
     {
          ConsoleWriter::WriteError("Authentication is enabled but no usable users are configured. Add a <user> entry or set HLQUERY_ADMIN_TOKEN.", true);
          return false;
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
               if (Line.find(">") != std::string::npos)
               {
                    ParseUserEntry(CurrentUserEntry);
                    CurrentUserEntry = "";
                    InUserEntry = false;
               }
               else
               {
                    InUserEntry = true;
               }
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
          print_info("Loaded {} users from config.", UsersByName.size());
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
     std::string Password;
     std::string FlagsStr;
     std::string Description;

     std::regex TokenRegex("token=\"([^\"]+)\"");
     std::smatch Match;

     if (std::regex_search(Line, Match, TokenRegex))
     {
          Token = Match[1].str();
     }

     std::regex PasswordRegex("password=\"([^\"]+)\"");

     if (std::regex_search(Line, Match, PasswordRegex))
     {
          Password = Match[1].str();
     }

     if (Token.empty() && Password.empty())
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
          if (Token.empty())
          {
               return false;
          }

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

     if (Password.size() > kMaxUserPasswordLen)
     {
          print_info("ERROR: User password too long in config.");

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

     User UserObj(Name, Token, Password, Flags, Description);

     if (!Token.empty())
     {
          UsersByToken[Token] = UserObj;
     }

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
     if (AuthHeader.find("Basic ") == 0)
     {
          std::string Decoded;

          if (!DecodeBase64(AuthHeader.substr(6), Decoded))
          {
               return AuthResult(false, User(), "Invalid basic authentication encoding.");
          }

          size_t Separator = Decoded.find(':');

          if (Separator == std::string::npos)
          {
               return AuthResult(false, User(), "Invalid basic authentication payload.");
          }

          std::string Name = Decoded.substr(0, Separator);
          std::string Password = Decoded.substr(Separator + 1);

          if (Name.empty() || Password.empty())
          {
               return AuthResult(false, User(), "Empty basic authentication credentials.");
          }

          std::lock_guard<std::mutex> Lock(UsersMutex);

          auto It = UsersByName.find(Name);

          if (It == UsersByName.end() || It->second.Password.empty() || It->second.Password != Password)
          {
               return AuthResult(false, User(), "Invalid username or password.");
          }

          return AuthResult(true, It->second);
     }

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

     if (!UserObj.Token.empty())
     {
          UsersByToken[UserObj.Token] = UserObj;
     }

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

          if (!Token.empty())
          {
               UsersByToken.erase(Token);
          }

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
          if (!NameIt->second.Token.empty())
          {
               UsersByToken.erase(NameIt->second.Token);
          }
     }

     if (!UserObj.Token.empty())
     {
          UsersByToken[UserObj.Token] = UserObj;
     }

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

     uint32_t UserCount = UsersByName.size();

     OSS.write(reinterpret_cast<const char *>(&UserCount), sizeof(UserCount));

     for (const auto &Pair : UsersByName)
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

     return WriteFileAtomic(FilePath, Encrypted);
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

          if (!UserObj.Token.empty())
          {
               UsersByToken[UserObj.Token] = UserObj;
          }

          UsersByName[UserObj.Name] = UserObj;
     }

     AuthEnabled = true;

     print_ok("Loaded {} users from encrypted ACL.", UsersByToken.size());
     print_ok("Authentication enabled.");

     return true;
}
