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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <unistd.h>

#include "api/apikeys.h"
#include "common/cryptoutils.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "utils/infos.h"

/* Implements API key persistence, validation, and rate limit bookkeeping. */

constexpr size_t kMaxKeysDatSize = 64 * 1024 * 1024;
constexpr uint32_t kMaxKeyCount = 100000;
constexpr uint32_t kMaxScopeCount = 10000;
constexpr uint32_t kMaxActionCount = 1024;
constexpr uint32_t kMaxKeyIdLen = 256;
constexpr uint32_t kMaxKeyHashLen = 256;
constexpr uint32_t kMaxKeyDescriptionLen = 4096;
constexpr uint32_t kMaxCollectionNameLen = 256;
constexpr uint32_t kMaxEmbeddedFiltersLen = 64 * 1024;

/* WriteFileAtomic writes data to a temp file, fsyncs, then renames into place. */

static bool WriteFileAtomic(const std::string &FilePath, const std::string &Contents)
{
     std::error_code EC;

     std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path(), EC);

     std::string DirPath = std::filesystem::path(FilePath).parent_path().string();

     const auto TmpNowMS = ::Instance ? ::Instance->NowMs() : ::NowMs();
     std::string TmpPath = FilePath + ".tmp." + std::to_string(static_cast<long long>(getpid())) + "." + std::to_string(TmpNowMS);

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

     if (!SaveKeysToEncryptedFile(KeysDat))
     {
          std::unique_lock<std::shared_mutex> RollbackLock(MutexValue);
          auto It = Keys.find(NewKey.ID);

          if (It != Keys.end() && It->second.KeyHash == Hashed)
          {
               Keys.erase(It);
               HashToID.erase(Hashed);
          }

          return "";
     }

     return RawKey;
}

/* Get key by ID. */

bool APIKeyManager::GetKey(const std::string &KeyID, APIKey *OutKey)
{
     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It != Keys.end())
     {
          if (OutKey)
          {
               *OutKey = It->second;
          }

          return true;
     }

     return false;
}

/* Validate a key string. */

bool APIKeyManager::ValidateKey(const std::string &KeyString, APIKey *OutKey)
{
     std::string Hashed = HashKey(KeyString);

     std::shared_lock<std::shared_mutex> Lock(MutexValue);

     if (::Instance && ::Instance->Logs)
     {
     }

     auto ItHash = HashToID.find(Hashed);

     if (ItHash == HashToID.end())
     {
          return false;
     }

     auto ItKey = Keys.find(ItHash->second);

     if (ItKey == Keys.end())
     {
          return false;
     }

     if (ItKey->second.IsExpired())
     {
          return false;
     }

     if (OutKey)
     {
          *OutKey = ItKey->second;
     }

     return true;
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
          APIKey Previous = It->second;

          HashToID.erase(Previous.KeyHash);
          Keys.erase(It);

          std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

          Lock.unlock();

          if (!SaveKeysToEncryptedFile(KeysDat))
          {
               std::unique_lock<std::shared_mutex> RollbackLock(MutexValue);
               Keys[Previous.ID] = Previous;
               HashToID[Previous.KeyHash] = Previous.ID;

               return false;
          }

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

     APIKey Previous = It->second;

     /* Update allowed fields. */

     It->second.Description = KeySpec.Description;
     It->second.Scopes = KeySpec.Scopes;
     It->second.ExpiresAt = std::chrono::system_clock::time_point();
     It->second.HasExpiration = false;
     It->second.RateLimitPerMinute = KeySpec.RateLimitPerMinute;
     It->second.AllowHanalyzer = KeySpec.AllowHanalyzer;

     std::string KeysDat = std::string(HLQUERY_ADMIN_DIR) + "/keys.dat";

     Lock.unlock();

     if (!SaveKeysToEncryptedFile(KeysDat))
     {
          std::unique_lock<std::shared_mutex> RollbackLock(MutexValue);
          Keys[KeyID] = Previous;
          HashToID[Previous.KeyHash] = KeyID;

          return false;
     }

     return true;
}

/* Update last used time. */

void APIKeyManager::UpdateLastUsed(const std::string &KeyID)
{
     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     auto It = Keys.find(KeyID);

     if (It != Keys.end())
     {
          const auto NowMS = ::Instance ? ::Instance->NowMs() : NowMs();
          It->second.LastUsedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));

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

     if (Limit <= 0)
     {
          Limit = 1000;
     }

     Lock.unlock();

     std::lock_guard<std::mutex> RateLock(RateLimitMutex);

     const auto NowMS = ::Instance ? ::Instance->NowMs() : NowMs();
     auto Now = std::chrono::system_clock::time_point(std::chrono::milliseconds(NowMS));
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

     return WriteFileAtomic(FilePath, Encrypted);
}

bool APIKeyManager::LoadKeysFromEncryptedFile(const std::string &FilePath)
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

     if (FileSizePos > static_cast<std::streampos>(kMaxKeysDatSize))
     {
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

     auto ReadValue = [&](auto &Val) -> bool
     {
          return static_cast<bool>(ISS.read(reinterpret_cast<char *>(&Val), sizeof(Val)));
     };

     auto ReadString = [&](std::string &Out, uint32_t MaxLen) -> bool
     {
          uint32_t Len = 0;

          if (!ReadValue(Len))
          {
               return false;
          }

          if (Len > MaxLen)
          {
               return false;
          }

          if (Len == 0)
          {
               Out.clear();

               return true;
          }

          Out.resize(Len);

          return static_cast<bool>(ISS.read(&Out[0], Len));
     };

     uint32_t KeyCount = 0;

     if (!ReadValue(KeyCount))
     {
          return false;
     }

     if (KeyCount > kMaxKeyCount)
     {
          return false;
     }

     std::unique_lock<std::shared_mutex> Lock(MutexValue);

     Keys.clear();
     HashToID.clear();

     for (uint32_t I = 0; I < KeyCount; ++I)
     {
          APIKey KeyObj;

          if (!ReadString(KeyObj.ID, kMaxKeyIdLen))
          {
               return false;
          }

          if (!ReadString(KeyObj.KeyHash, kMaxKeyHashLen))
          {
               return false;
          }

          if (!ReadString(KeyObj.Description, kMaxKeyDescriptionLen))
          {
               return false;
          }

          uint32_t ScopeCount = 0;

          if (!ReadValue(ScopeCount))
          {
               return false;
          }

          if (ScopeCount > kMaxScopeCount)
          {
               return false;
          }

          for (uint32_t J = 0; J < ScopeCount; ++J)
          {
               std::string ColName;

               if (!ReadString(ColName, kMaxCollectionNameLen))
               {
                    return false;
               }

               CollectionScope Scope;

               uint32_t ActionCount = 0;

               if (!ReadValue(ActionCount))
               {
                    return false;
               }

               if (ActionCount > kMaxActionCount)
               {
                    return false;
               }

               for (uint32_t K = 0; K < ActionCount; ++K)
               {
                    uint32_t ActionVal = 0;

                    if (!ReadValue(ActionVal))
                    {
                         return false;
                    }
                    Scope.Actions.insert(static_cast<APIKeyAction>(ActionVal));
               }

               if (!ReadString(Scope.EmbeddedFilters, kMaxEmbeddedFiltersLen))
               {
                    return false;
               }

               KeyObj.Scopes[ColName] = Scope;
          }

          if (!ReadValue(KeyObj.HasExpiration))
          {
               return false;
          }

          long long ExpiresAtTime = 0;

          if (!ReadValue(ExpiresAtTime))
          {
               return false;
          }

          KeyObj.ExpiresAt = std::chrono::system_clock::time_point(std::chrono::system_clock::duration(ExpiresAtTime));

          if (!ReadValue(KeyObj.RateLimitPerMinute))
          {
               return false;
          }

          if (KeyObj.RateLimitPerMinute <= 0)
          {
               KeyObj.RateLimitPerMinute = 1000;
          }

          if (!ReadValue(KeyObj.AllowHanalyzer))
          {
               return false;
          }

          if (!ReadValue(KeyObj.UseCount))
          {
               return false;
          }

          Keys[KeyObj.ID] = KeyObj;
          HashToID[KeyObj.KeyHash] = KeyObj.ID;
     }

     return true;
}
