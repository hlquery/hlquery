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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "api/apikeys.h"
#include "common/cryptoutils.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "utils/infos.h"

namespace
{
/* Encryption key for keys.dat. */

std::string GetKeysEncryptionKey()
{
     const char *EnvKey = std::getenv("HLQUERY_KEYS_ENCRYPTION_KEY");

     if (EnvKey && *EnvKey)
     {
          return std::string(EnvKey);
     }

     return "hlquery-api-keys-encryption-key-v1";
}
}
/* Initialize APIKeyManager. */

bool APIKeyManager::Initialize()
{
     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     Keys.clear();
     HashToID.clear();

     std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

     if (std::filesystem::exists(KeysDat))
     {
          Lock.unlock();

          return LoadKeysFromEncryptedFile(KeysDat);
     }

     return true;
}

/* Create a new API key. */

std::string APIKeyManager::CreateKey(APIKey &KeySpec)
{
     std::string RawKey = GenerateSecureKey();
     std::string Hashed = HashKey(RawKey);

     if (KeySpec.ID.empty())
     {
          KeySpec.ID = Hex(RandomBytes(8));
     }

     APIKey NewKey = KeySpec;
     NewKey.KeyHash = Hashed;
     NewKey.HasExpiration = false;
     NewKey.ExpiresAt = std::chrono::system_clock::time_point();

     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     Keys[NewKey.ID] = NewKey;
     HashToID[Hashed] = NewKey.ID;

     std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

     Lock.unlock();

     SaveKeysToEncryptedFile(KeysDat);

     return RawKey;
}

/* Get key by ID. */

APIKey *APIKeyManager::GetKey(const std::string &KeyID)
{
     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It != Keys.end())
     {
          return &It->second;
     }

     return nullptr;
}

/* Validate a key string. */

APIKey *APIKeyManager::ValidateKey(const std::string &KeyString)
{
     std::string Hashed = HashKey(KeyString);

     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     if (::Instance && ::Instance->Logs)
     {
     }

     auto ItHash = HashToID.find(Hashed);

     if (ItHash == HashToID.end())
     {
          return nullptr;
     }

     auto ItKey = Keys.find(ItHash->second);

     if (ItKey == Keys.end())
     {
          return nullptr;
     }

     if (ItKey->second.IsExpired())
     {
          return nullptr;
     }

     return &ItKey->second;
}

/* List all keys. */

std::vector<APIKey> APIKeyManager::ListKeys()
{
     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     std::vector<APIKey> Result;

     for (const auto &Pair : Keys)
     {
          Result.push_back(Pair.second);
     }

     return Result;
}

/* Delete a key. */

bool APIKeyManager::DeleteKey(const std::string &KeyID)
{
     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It != Keys.end())
     {
          HashToID.erase(It->second.KeyHash);
          Keys.erase(It);

          std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

          Lock.unlock();

          SaveKeysToEncryptedFile(KeysDat);

          return true;
     }

     return false;
}

/* Update an existing key. */

bool APIKeyManager::UpdateKey(const std::string &KeyID, const APIKey &KeySpec)
{
     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It == Keys.end())
     {
          return false;
     }

     /* Update allowed fields. */

     It->second.Description = KeySpec.Description;
     It->second.Scopes = KeySpec.Scopes;
     It->second.ExpiresAt = std::chrono::system_clock::time_point();
     It->second.HasExpiration = false;
     It->second.RateLimitPerMinute = KeySpec.RateLimitPerMinute;
     It->second.AllowHanalyzer = KeySpec.AllowHanalyzer;

     std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

     Lock.unlock();

     SaveKeysToEncryptedFile(KeysDat);

     return true;
}

/* Update last used time. */

void APIKeyManager::UpdateLastUsed(const std::string &KeyID)
{
     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It != Keys.end())
     {
          if (::Instance)
          {
               auto NowMS = ::Instance->NowMs();

               It->second.LastUsedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
          }
          else
          {
               It->second.LastUsedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()));
          }

          It->second.UseCount++;
     }
}

/* Check and update rate limit. */

bool APIKeyManager::CheckRateLimit(const std::string &KeyID)
{
     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     auto ItKey = Keys.find(KeyID);

     if (ItKey == Keys.end())
     {
          return false;
     }

     int Limit = ItKey->second.RateLimitPerMinute;

     Lock.unlock();

     std::lock_guard<std::mutex> RateLock(RateLimitMutex);

     auto Now = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMs()));
     auto &Tracker = RateLimits[KeyID];

     if (std::chrono::duration_cast<std::chrono::minutes>(Now - Tracker.WindowStart).count() >= 1)
     {
          Tracker.WindowStart = Now;
          Tracker.RequestCount = 0;
     }

     if (Tracker.RequestCount >= Limit)
     {
          return false;
     }

     Tracker.RequestCount++;

     return true;
}

/* Generate a secure random key. */

std::string APIKeyManager::GenerateSecureKey()
{
     std::vector<uint8_t> Bytes = RandomBytes(32);

     return Hex(Bytes);
}

/* Hash a key for storage. */

std::string APIKeyManager::HashKey(const std::string &Key)
{
     std::vector<uint8_t> Hashed = SHA256(Key.data(), Key.size());
     std::string Result = Hex(Hashed);
     return Result;
}

/* Persistence implementation. */

bool APIKeyManager::SaveKeysToEncryptedFile(const std::string &FilePath)
{
     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     std::ostringstream OSS(std::ios::binary);

     uint32_t KeyCount = static_cast<uint32_t>(Keys.size());

     OSS.write(reinterpret_cast<const char *>(&KeyCount), sizeof(KeyCount));

     for (const auto &Pair : Keys)
     {
          const APIKey &KeyObj = Pair.second;

          auto WriteString = [&](const std::string &S)
          {
               uint32_t Len = static_cast<uint32_t>(S.size());

               OSS.write(reinterpret_cast<const char *>(&Len), sizeof(Len));
               OSS.write(S.data(), Len);
          };

          WriteString(KeyObj.ID);
          WriteString(KeyObj.KeyHash);
          WriteString(KeyObj.Description);

          uint32_t ScopeCount = static_cast<uint32_t>(KeyObj.Scopes.size());

          OSS.write(reinterpret_cast<const char *>(&ScopeCount), sizeof(ScopeCount));

          for (const auto &ScopePair : KeyObj.Scopes)
          {
               WriteString(ScopePair.first); /* Collection name. */

               const CollectionScope &Scope = ScopePair.second;

               uint32_t ActionCount = static_cast<uint32_t>(Scope.Actions.size());

               OSS.write(reinterpret_cast<const char *>(&ActionCount), sizeof(ActionCount));

               for (APIKeyAction Action : Scope.Actions)
               {
                    uint32_t ActionVal = static_cast<uint32_t>(Action);

                    OSS.write(reinterpret_cast<const char *>(&ActionVal), sizeof(ActionVal));
               }

               WriteString(Scope.EmbeddedFilters);
          }

          OSS.write(reinterpret_cast<const char *>(&KeyObj.HasExpiration), sizeof(KeyObj.HasExpiration));

          auto ExpiresAtTime = KeyObj.ExpiresAt.time_since_epoch().count();

          OSS.write(reinterpret_cast<const char *>(&ExpiresAtTime), sizeof(ExpiresAtTime));
          OSS.write(reinterpret_cast<const char *>(&KeyObj.RateLimitPerMinute), sizeof(KeyObj.RateLimitPerMinute));
          OSS.write(reinterpret_cast<const char *>(&KeyObj.AllowHanalyzer), sizeof(KeyObj.AllowHanalyzer));
          OSS.write(reinterpret_cast<const char *>(&KeyObj.UseCount), sizeof(KeyObj.UseCount));
     }

     std::string Plaintext = OSS.str();
     std::string Encrypted = AES256Encrypt(Plaintext, GetKeysEncryptionKey());

     if (Encrypted.empty())
     {
          return false;
     }

     std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path());

     std::ofstream File(FilePath, std::ios::binary);

     if (!File.is_open())
     {
          return false;
     }

     File.write(Encrypted.data(), Encrypted.size());
     File.close();

     return File.good();
}

bool APIKeyManager::LoadKeysFromEncryptedFile(const std::string &FilePath)
{
     std::ifstream File(FilePath, std::ios::binary);

     if (!File.is_open())
     {
          return false;
     }

     std::string Encrypted((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

     File.close();

     std::string Plaintext = AES256Decrypt(Encrypted, GetKeysEncryptionKey());

     if (Plaintext.empty() && !Encrypted.empty())
     {
          return false;
     }

     if (Plaintext.empty())
     {
          return true;
     }

     std::istringstream ISS(Plaintext, std::ios::binary);

     auto ReadValue = [&](auto &Val)
     {
          ISS.read(reinterpret_cast<char *>(&Val), sizeof(Val));
     };

     auto ReadString = [&]() -> std::string
     {
          uint32_t Len = 0;

          ReadValue(Len);

          std::string S(Len, '\0');

          ISS.read(&S[0], Len);

          return S;
     };

     uint32_t KeyCount = 0;

     ReadValue(KeyCount);

     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     Keys.clear();
     HashToID.clear();

     for (uint32_t I = 0; I < KeyCount; ++I)
     {
          APIKey KeyObj;

          KeyObj.ID = ReadString();
          KeyObj.KeyHash = ReadString();
          KeyObj.Description = ReadString();

          uint32_t ScopeCount = 0;

          ReadValue(ScopeCount);

          for (uint32_t J = 0; J < ScopeCount; ++J)
          {
               std::string ColName = ReadString();

               CollectionScope Scope;

               uint32_t ActionCount = 0;

               ReadValue(ActionCount);

               for (uint32_t K = 0; K < ActionCount; ++K)
               {
                    uint32_t ActionVal = 0;

                    ReadValue(ActionVal);
                    Scope.Actions.insert(static_cast<APIKeyAction>(ActionVal));
               }

               Scope.EmbeddedFilters = ReadString();

               KeyObj.Scopes[ColName] = Scope;
          }

          ReadValue(KeyObj.HasExpiration);

          long long ExpiresAtTime = 0;

          ReadValue(ExpiresAtTime);

          KeyObj.ExpiresAt = std::chrono::system_clock::time_point(std::chrono::system_clock::duration(ExpiresAtTime));

          ReadValue(KeyObj.RateLimitPerMinute);
          ReadValue(KeyObj.AllowHanalyzer);
          ReadValue(KeyObj.UseCount);

          Keys[KeyObj.ID] = KeyObj;
          HashToID[KeyObj.KeyHash] = KeyObj.ID;
     }

     return true;
}
