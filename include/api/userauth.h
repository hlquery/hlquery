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

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/* User permission flags. */

enum class UserFlag
{
     USER, /* Standard user permissions. */
     ADMIN /* Administrative permissions. */
};

/* User information structure. */

struct User
{
     std::string Name;
     std::string Token;
     std::string Password;
     std::set<UserFlag> Flags;
     std::string Description;

     User() = default;

     User(const std::string &N, const std::string &T, const std::set<UserFlag> &F, const std::string &D = "")
         : Name(N), Token(T), Flags(F), Description(D)
     {
     }

     User(const std::string &N, const std::string &T, const std::string &P, const std::set<UserFlag> &F, const std::string &D = "")
         : Name(N), Token(T), Password(P), Flags(F), Description(D)
     {
     }

     bool HasFlag(UserFlag FlagVal) const
     {
          return Flags.find(FlagVal) != Flags.end();
     }

     bool IsAdmin() const
     {
          return HasFlag(UserFlag::ADMIN);
     }

     bool IsUser() const
     {
          return HasFlag(UserFlag::USER);
     }
};

/* Authentication result structure. */

struct AuthResult
{
     bool Valid;
     User UserObj;
     std::string ErrorMessage;

     AuthResult() : Valid(false)
     {
     }

     AuthResult(bool V, const User &U, const std::string &Msg = "")
         : Valid(V), UserObj(U), ErrorMessage(Msg)
     {
     }
};

/* User authentication manager. */

class UserAuthManager
{
   public:
     UserAuthManager() = default;

     /* Initialize authentication system. */

     bool Initialize();

     /* Load users from configuration file. */

     bool LoadUsersFromConfig(const std::string &ConfigFile);

     /* Load users from ConfigReader (used when config is included in main config). */

     bool LoadUsersFromConfigReader(const class ConfigReader &Reader);

     /* Authentication methods. */

     AuthResult AuthenticateToken(const std::string &Token);

     AuthResult AuthenticateRequest(const std::string &AuthHeader);

     /* User management. */

     bool AddUser(const User &UserObj);

     bool RemoveUser(const std::string &Name);

     bool UpdateUser(const User &UserObj);

     /* 
      * Return optional<User> instead of pointer to prevent Use-After-Free. 
      * Returning a pointer to internal map element is unsafe if RemoveUser is called concurrently. 
      */

     std::optional<User> GetUser(const std::string &Name);

     std::vector<User> GetAllUsers();

     /* Permission checking. */

     bool HasPermission(const std::string &Token, UserFlag RequiredFlag);

     bool IsAdmin(const std::string &Token);

     bool IsUser(const std::string &Token);

     /* Configuration. */

     bool IsAuthEnabled() const
     {
          return AuthEnabled;
     }

     void SetAuthEnabled(bool Enabled)
     {
          AuthEnabled = Enabled;
     }

     /* Token validation. */

     bool IsValidToken(const std::string &Token);

     /* Encrypted ACL methods. */

     bool CreateRootUser();

     bool LoadUsersFromEncryptedFile(const std::string &FilePath);

     bool SaveUsersToEncryptedFile(const std::string &FilePath);

   private:
     /* Internal methods. */

     /* Extract the bearer token value from one authorization header. */

     std::string ExtractTokenFromHeader(const std::string &AuthHeader);

     /* Parse one user flag token into the corresponding enum value. */

     UserFlag ParseUserFlag(const std::string &FlagStr);

     /* Generate one random authentication token for a user. */

     std::string GenerateRandomToken();

     /* Parse a flag list string into the set of user flags. */

     std::set<UserFlag> ParseUserFlags(const std::string &FlagsStr);

     /* Data members. */

     /* Users indexed by authentication token */

     std::unordered_map<std::string, User> UsersByToken;

     /* Users indexed by user name */

     std::unordered_map<std::string, User> UsersByName;

     /* Protects user storage updates and lookups */

     std::mutex UsersMutex;

     /* Indicates whether authentication enforcement is enabled */

     bool AuthEnabled;

     /* Configuration file parsing. */

     /* Parse the full authentication configuration file. */

     bool ParseConfigFile(const std::string &ConfigFile);

     /* Parse one user entry line from the configuration file. */

     bool ParseUserEntry(const std::string &Line);

     /* Parse the authentication enable or disable setting line. */

     bool ParseAuthSetting(const std::string &Line);
};
