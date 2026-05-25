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

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hlquery.h"

/* API Key permissions. */

enum class APIKeyAction
{
     SEARCH,           /* documents:search. */
     CREATE,           /* documents:create. */
     UPDATE,           /* documents:update. */
     DELETE,           /* documents:delete. */
     COLLECTIONS_LIST, /* collections:*. */
     COLLECTIONS_CREATE,
     COLLECTIONS_DELETE,
     IMPORT, /* documents:import. */
     ALL     /* Full access. */
};

/* Collection scope structure. */

struct CollectionScope
{
     std::unordered_set<APIKeyAction> Actions;
     std::string EmbeddedFilters;
};

/* Scoped API Key structure. */

struct APIKey
{
     std::string ID;
     std::string KeyHash; /* SHA256 hash of actual key. */
     std::string Description;

     /* Collection-specific scopes. */

     std::unordered_map<std::string, CollectionScope> Scopes;

     /* Expiration. */

     std::chrono::system_clock::time_point ExpiresAt;
     bool HasExpiration;

     /* Rate limiting. */

     int RateLimitPerMinute;

     /* Access to hanalyzer. */

     bool AllowHanalyzer;

     /* Metadata. */

     std::chrono::system_clock::time_point CreatedAt;
     std::chrono::system_clock::time_point LastUsedAt;
     int UseCount;

     APIKey() : HasExpiration(false), RateLimitPerMinute(1000), AllowHanalyzer(false), UseCount(0)
     {
          if (::Instance)
          {
               auto NowMS = ::Instance->NowMs();

               CreatedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
          }
          else
          {
               CreatedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()));
          }

          LastUsedAt = CreatedAt;
     }

     /* Check if key is expired. */

     bool IsExpired() const
     {
          if (!HasExpiration)
          {
               return false;
          }

          std::chrono::system_clock::time_point NowVal;

          if (::Instance)
          {
               auto NowMS = ::Instance->NowMs();

               NowVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
          }
          else
          {
               NowVal = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()));
          }

          return NowVal > ExpiresAt;
     }

     /* Check if action is allowed for a specific collection. */

     bool HasAction(const std::string& CollectionName, APIKeyAction ActionVal) const
     {
          auto Scope = GetScopeForCollection(CollectionName);

          if (!Scope)
          {
               return false;
          }

          if (Scope->Actions.find(APIKeyAction::ALL) != Scope->Actions.end())
          {
               return true;
          }

          return Scope->Actions.find(ActionVal) != Scope->Actions.end();
     }

     /* Check if collection is allowed. */

     bool CanAccessCollection(const std::string& CollectionName) const
     {
          return GetScopeForCollection(CollectionName) != nullptr;
     }

     /* Get scope for a collection, considering wildcards. */

     const CollectionScope* GetScopeForCollection(const std::string& CollectionName) const
     {
          auto It = Scopes.find(CollectionName);

          if (It != Scopes.end())
          {
               return &It->second;
          }

          /* Check for wildcard scope. */

          auto ItWildcard = Scopes.find("*");

          if (ItWildcard != Scopes.end())
          {
               return &ItWildcard->second;
          }

          return nullptr;
     }

     /* Get embedded filters for a collection. */

     std::string GetEmbeddedFilters(const std::string& CollectionName) const
     {
          auto Scope = GetScopeForCollection(CollectionName);

          if (Scope)
          {
               return Scope->EmbeddedFilters;
          }

          return "";
     }
};

/* API Key Manager (Singleton). */

class APIKeyManager
{
   public:

     static APIKeyManager& Instance()
     {
          static APIKeyManager SInstance;

          return SInstance;
     }

     /* Create new API key. */

     std::string CreateKey(APIKey& KeySpec);

     /* Get key by ID. */

     APIKey* GetKey(const std::string& KeyID);

     /* Validate key (returns nullptr if invalid/expired). */

     APIKey* ValidateKey(const std::string& KeyString);

     /* List all keys. */

     std::vector<APIKey> ListKeys();

     /* Delete key. */

     bool DeleteKey(const std::string& KeyID);

     /* Update key. */

     bool UpdateKey(const std::string& KeyID, const APIKey& KeySpec);

     /* Update last used time. */

     void UpdateLastUsed(const std::string& KeyID);

     /* Check rate limit. */

     bool CheckRateLimit(const std::string& KeyID);

     /* Persistence. */

     bool Initialize();
     bool SaveKeysToEncryptedFile(const std::string& FilePath);
     bool LoadKeysFromEncryptedFile(const std::string& FilePath);

   private:

     APIKeyManager()
     {
     
     }

     ~APIKeyManager()
     {
     
     }

     APIKeyManager(const APIKeyManager&) = delete;
     APIKeyManager& operator=(const APIKeyManager&) = delete;

     std::unordered_map<std::string, APIKey> Keys;
     std::unordered_map<std::string, std::string> HashToID; /* Hash -> ID lookup. */
     std::shared_mutex MutexValue;

     /* Rate limiting tracking. */

     struct RateLimitTracker
     {
          std::chrono::system_clock::time_point WindowStart;
          int RequestCount;
     };

     std::unordered_map<std::string, RateLimitTracker> RateLimits;
     std::mutex RateLimitMutex;

     /* Generate secure random key. */

     std::string GenerateSecureKey();

     /* Hash key for storage. */

     std::string HashKey(const std::string& Key);
};
