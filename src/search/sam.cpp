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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <rocksdb/write_batch.h>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "core/hlquery.h"
#include "search/sam.h"
#include "search/storageengine.h"
#include "utils/tools.h"
#include "vendor/json/json.hpp"

namespace
{
std::string TrimCopy(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

std::string NormalizeTerm(const std::string& Value)
{
     std::string Normalized;
     Normalized.reserve(Value.size());
     bool LastWasSpace = false;

     for (unsigned char C : Value)
     {
          if (std::isalnum(C))
          {
               Normalized.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '_' || C == '/' || C == '.')
          {
               if (!Normalized.empty() && !LastWasSpace)
               {
                    Normalized.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     return TrimCopy(Normalized);
}

void AppendUniqueNormalized(std::vector<std::string>& Target,
                            std::unordered_set<std::string>& Seen,
                            const std::string& Value)
{
     const std::string Normalized = NormalizeTerm(Value);

     if (Normalized.empty() || Normalized.size() < 3 || Normalized.size() > 96)
     {
          return;
     }

     if (Seen.insert(Normalized).second)
     {
          Target.push_back(Normalized);
     }
}

std::vector<std::string> SplitWords(const std::string& Value)
{
     std::vector<std::string> Words;
     std::istringstream In(Value);
     std::string Word;

     while (In >> Word)
     {
          Word = NormalizeTerm(Word);

          if (!Word.empty())
          {
               Words.push_back(Word);
          }
     }

     return Words;
}

std::vector<std::string> ExtractArrayishValues(const std::string& Raw)
{
     std::vector<std::string> Values;
     const std::string Trimmed = TrimCopy(Raw);

     if (Trimmed.empty())
     {
          return Values;
     }

     try
     {
          if (!Trimmed.empty() && Trimmed.front() == '[')
          {
               nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

               if (Parsed.is_array())
               {
                    for (const auto& Entry : Parsed)
                    {
                         if (Entry.is_string())
                         {
                              const std::string Value = TrimCopy(Entry.get<std::string>());

                              if (!Value.empty())
                              {
                                   Values.push_back(Value);
                              }
                         }
                    }
               }

               return Values;
          }
     }
     catch (...)
     {
     }

     std::string Token;
     std::istringstream In(Trimmed);

     while (std::getline(In, Token, ','))
     {
          Token = TrimCopy(Token);

          if (!Token.empty())
          {
               Values.push_back(Token);
          }
     }

     if (Values.empty() && !Trimmed.empty())
     {
          Values.push_back(Trimmed);
     }

     return Values;
}

std::string ResolveSamDataDir()
{
     std::string BaseDataDir = std::string(HLQUERY_DATA_DIR);
     const char* EnvDataDir = std::getenv("HLQUERY_DATA_DIR");

     if (EnvDataDir && *EnvDataDir)
     {
          BaseDataDir = EnvDataDir;
     }

     try
     {
          if (Instance && Instance->Config && Instance->Config->IsValid())
          {
               const auto& RocksDBOptions = Instance->Config->GetRocksDBOptions();

               if (!RocksDBOptions.DataDir.empty())
               {
                    BaseDataDir = RocksDBOptions.DataDir;
               }
          }
     }
     catch (...)
     {
     }

     return BaseDataDir + "/sam";
}

std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID)
{
     return "sam:doc:" + Collection + ":" + DocumentID;
}

std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID)
{
     return "sam:term:" + Term + ":" + Collection + ":" + DocumentID;
}
}

SAM::SAM()
{
     OptionsValue.create_if_missing = true;
     OptionsValue.error_if_exists = false;
     OptionsValue.max_open_files = 128;
}

SAM::~SAM()
{
     Shutdown();
}

bool SAM::Initialize()
{
     bool ShouldRecreate = false;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               return true;
          }

          DBPath = ResolveDBPath();

          try
          {
               std::filesystem::create_directories(DBPath);
          }
          catch (const std::exception& E)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to create SAM directory '" + DBPath + "': " + E.what() + ".");
               }

               return false;
          }

          std::unique_ptr<rocksdb::DB> RawDB;
          const rocksdb::Status Status = rocksdb::DB::Open(OptionsValue, DBPath, &RawDB);

          if (!Status.ok())
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("sam", "Failed to open SAM database at '" + DBPath + "': " + Status.ToString() + ".");
               }

               return false;
          }

          Database = std::move(RawDB);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "Secondary Assistant Manager opened at " + DBPath + ".");
          }

          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
          Iterator->SeekToFirst();
          ShouldRecreate = !Iterator->Valid();
     }

     if (ShouldRecreate)
     {
          std::string ErrorMessage;

          if (!Recreate(&ErrorMessage) && Instance && Instance->Logs)
          {
               Instance->Logs->Normal("sam", "Initial SAM rebuild failed: " + ErrorMessage + ".");
          }
     }

     return true;
}

void SAM::Shutdown()
{
     std::lock_guard<std::mutex> Lock(DBMutex);
     Database.reset();
}

bool SAM::IsOpen() const
{
     std::lock_guard<std::mutex> Lock(DBMutex);
     return static_cast<bool>(Database);
}

std::string SAM::ResolveDBPath() const
{
     return ResolveSamDataDir();
}

bool SAM::ClearAll(std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     rocksdb::WriteBatch Batch;
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->SeekToFirst(); Iterator->Valid(); Iterator->Next())
     {
          Batch.Delete(Iterator->key());
     }

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::Recreate(std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!ClearAll(ErrorMessage))
     {
          return false;
     }

     size_t IndexedDocuments = 0;
     size_t FailedDocuments = 0;

     for (const std::string& Collection : HybridStorageManager::GetInstance().ListCollections())
     {
          if (!Instance || !Instance->Database)
          {
               break;
          }

          const std::vector<std::string> DocKeys = Instance->Database->Keys("doc:" + Collection + ":*");

          for (const auto& DocKey : DocKeys)
          {
               const size_t LastColon = DocKey.find_last_of(':');

               if (LastColon == std::string::npos || LastColon + 1 >= DocKey.size())
               {
                    continue;
               }

               const std::string DocumentID = DocKey.substr(LastColon + 1);
               const Document Doc = HybridStorageManager::GetInstance().GetDocument(Collection, DocumentID);

               if (Doc.ID.empty())
               {
                    continue;
               }

               std::string IndexError;

               if (IndexDocumentLocked(Collection, Doc, &IndexError))
               {
                    IndexedDocuments++;
               }
               else
               {
                    FailedDocuments++;

                    if (Instance && Instance->Logs && !IndexError.empty())
                    {
                         Instance->Logs->Normal("sam", "Failed to index '" + Collection + "/" + DocumentID + "' during recreate: " + IndexError + ".");
                    }
               }
          }
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("sam",
                                 "SAM recreate complete: indexed " + std::to_string(IndexedDocuments) +
                                      " documents, failed " + std::to_string(FailedDocuments) + ".");
     }

     return true;
}

bool SAM::RemoveExistingDocumentTermsLocked(const std::string& Collection,
                                            const std::string& DocumentID,
                                            std::string* ErrorMessage)
{
     const std::string ManifestKey = BuildDocManifestKey(Collection, DocumentID);
     std::string ExistingValue;
     const rocksdb::Status GetStatus = Database->Get(rocksdb::ReadOptions(), ManifestKey, &ExistingValue);

     if (GetStatus.IsNotFound())
     {
          return true;
     }

     if (!GetStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = GetStatus.ToString();
          }

          return false;
     }

     rocksdb::WriteBatch Batch;

     try
     {
          nlohmann::json Root = nlohmann::json::parse(ExistingValue);

          if (Root.contains("terms") && Root["terms"].is_array())
          {
               for (const auto& Entry : Root["terms"])
               {
                    if (!Entry.is_string())
                    {
                         continue;
                    }

                    Batch.Delete(BuildTermKey(Entry.get<std::string>(), Collection, DocumentID));
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
     }

     Batch.Delete(ManifestKey);

     const rocksdb::Status WriteStatus = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::IndexDocumentLocked(const std::string& Collection, const Document& Doc, std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection or document ID is empty.";
          }

          return false;
     }

     if (!RemoveExistingDocumentTermsLocked(Collection, Doc.ID, ErrorMessage))
     {
          return false;
     }

     const std::vector<std::string> Terms = ExpandDocumentTerms(Collection, Doc);
     rocksdb::WriteBatch Batch;
     nlohmann::json Manifest;
     Manifest["collection"] = Collection;
     Manifest["id"] = Doc.ID;
     Manifest["title"] = Doc.Title;
     Manifest["terms"] = Terms;

     for (const auto& Term : Terms)
     {
          nlohmann::json Payload;
          Payload["collection"] = Collection;
          Payload["id"] = Doc.ID;
          Payload["title"] = Doc.Title;
          Payload["term"] = Term;
          Batch.Put(BuildTermKey(Term, Collection, Doc.ID), Payload.dump());
     }

     Batch.Put(BuildDocManifestKey(Collection, Doc.ID), Manifest.dump());

     const rocksdb::Status Status = Database->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

bool SAM::IndexDocument(const std::string& Collection, const Document& Doc, std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);
     return IndexDocumentLocked(Collection, Doc, ErrorMessage);
}

bool SAM::DeleteDocument(const std::string& Collection, const std::string& DocumentID, std::string* ErrorMessage)
{
     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     return RemoveExistingDocumentTermsLocked(Collection, DocumentID, ErrorMessage);
}

std::vector<std::string> SAM::GenerateLLMTerms(const std::string& Collection, const Document& Doc) const
{
     std::vector<std::string> Terms;

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          return Terms;
     }

     const std::string& Command = Instance->LLM->GetInferenceCommand();

     if (Command.empty())
     {
          return Terms;
     }

     nlohmann::json Payload;
     Payload["collection"] = Collection;
     Payload["id"] = Doc.ID;
     Payload["title"] = Doc.Title;
     Payload["content"] = Doc.Content;
     Payload["fields"] = Doc.Fields;

     std::lock_guard<std::mutex> Lock(InferenceMutex);

     setenv("HLQUERY_LLM_MODEL", Instance->LLM->GetModelPath().c_str(), 1);
     setenv("HLQUERY_SAM_DOC_JSON", Payload.dump().c_str(), 1);

     FILE* Pipe = popen(Command.c_str(), "r");

     if (!Pipe)
     {
          unsetenv("HLQUERY_LLM_MODEL");
          unsetenv("HLQUERY_SAM_DOC_JSON");
          return Terms;
     }

     std::unordered_set<std::string> Seen;
     std::array<char, 512> Buffer{};

     while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe))
     {
          AppendUniqueNormalized(Terms, Seen, Buffer.data());

          if (Terms.size() >= 16)
          {
               break;
          }
     }

     pclose(Pipe);
     unsetenv("HLQUERY_LLM_MODEL");
     unsetenv("HLQUERY_SAM_DOC_JSON");

     return Terms;
}

std::vector<std::string> SAM::GenerateHeuristicTerms(const std::string& Collection, const Document& Doc) const
{
     std::vector<std::string> Terms;
     std::unordered_set<std::string> Seen;

     const std::string Title = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);
     const std::string LowerTitle = ToLowerCopy(Title);

     std::string Category;
     std::vector<std::string> Labels;
     std::vector<std::string> Genres;

     for (const auto& Pair : Doc.Fields)
     {
          const std::string LowerKey = ToLowerCopy(Pair.first);

          if (LowerKey == "category" && Category.empty())
          {
               Category = Pair.second;
          }
          else if (LowerKey == "labels" || LowerKey == "tags" || LowerKey == "keywords")
          {
               auto Values = ExtractArrayishValues(Pair.second);
               Labels.insert(Labels.end(), Values.begin(), Values.end());
          }
          else if (LowerKey == "genre" || LowerKey == "genres" || LowerKey == "style")
          {
               auto Values = ExtractArrayishValues(Pair.second);
               Genres.insert(Genres.end(), Values.begin(), Values.end());
          }
     }

     AppendUniqueNormalized(Terms, Seen, Title);

     if (!Collection.empty())
     {
          AppendUniqueNormalized(Terms, Seen, Title + " " + Collection);
     }

     if (!Category.empty())
     {
          AppendUniqueNormalized(Terms, Seen, Title + " " + Category);
          AppendUniqueNormalized(Terms, Seen, Category + " " + Title);
     }

     for (const auto& Label : Labels)
     {
          AppendUniqueNormalized(Terms, Seen, Title + " " + Label);
          AppendUniqueNormalized(Terms, Seen, Label + " " + Title);
     }

     for (const auto& Genre : Genres)
     {
          AppendUniqueNormalized(Terms, Seen, Genre + " " + Title);
     }

     const std::string CombinedContext = ToLowerCopy(Collection + " " + Category + " " + Doc.Content);

     if (CombinedContext.find("music") != std::string::npos ||
         CombinedContext.find("song") != std::string::npos ||
         CombinedContext.find("singer") != std::string::npos ||
         CombinedContext.find("album") != std::string::npos ||
         std::find_if(Genres.begin(), Genres.end(), [](const std::string& Value)
                      {
                           const std::string Lower = ToLowerCopy(Value);
                           return Lower.find("pop") != std::string::npos ||
                                  Lower.find("rock") != std::string::npos ||
                                  Lower.find("hip hop") != std::string::npos;
                      }) != Genres.end())
     {
          AppendUniqueNormalized(Terms, Seen, Title + " songs");
          AppendUniqueNormalized(Terms, Seen, Title + " music");
          AppendUniqueNormalized(Terms, Seen, Title + " albums");
          AppendUniqueNormalized(Terms, Seen, "music by " + Title);
     }

     if (CombinedContext.find("art") != std::string::npos ||
         CombinedContext.find("painting") != std::string::npos ||
         CombinedContext.find("artist") != std::string::npos ||
         CombinedContext.find("gallery") != std::string::npos)
     {
          AppendUniqueNormalized(Terms, Seen, Title + " art");
          AppendUniqueNormalized(Terms, Seen, "art by " + Title);
          AppendUniqueNormalized(Terms, Seen, Title + " artwork");
     }

     static const std::unordered_map<std::string, std::vector<std::string>> EntityAliases = {
          {"madonna", {"queen of pop", "pop queen", "madonna songs", "madonna music"}},
          {"beyonce", {"queen bey", "beyonce songs", "beyonce music"}},
          {"taylor swift", {"taylor swift songs", "swift music", "pop star taylor swift"}},
          {"picasso", {"pablo picasso", "picasso art", "cubist artist"}},
          {"vincent van gogh", {"van gogh", "van gogh art", "post impressionist artist"}}};

     const auto AliasIt = EntityAliases.find(LowerTitle);

     if (AliasIt != EntityAliases.end())
     {
          for (const auto& Alias : AliasIt->second)
          {
               AppendUniqueNormalized(Terms, Seen, Alias);
          }
     }

     const std::vector<std::string> ContentWords = SplitWords(Doc.Content);

     for (size_t I = 0; I < ContentWords.size() && Terms.size() < 24; ++I)
     {
          const std::string& Word = ContentWords[I];

          if (Word.size() < 4)
          {
               continue;
          }

          if (Word == "artist" || Word == "music" || Word == "album" || Word == "songs")
          {
               continue;
          }

          AppendUniqueNormalized(Terms, Seen, Title + " " + Word);
     }

     return Terms;
}

std::vector<std::string> SAM::ExpandDocumentTerms(const std::string& Collection, const Document& Doc) const
{
     std::vector<std::string> Terms = GenerateLLMTerms(Collection, Doc);
     std::unordered_set<std::string> Seen(Terms.begin(), Terms.end());
     std::vector<std::string> HeuristicTerms = GenerateHeuristicTerms(Collection, Doc);

     for (const auto& Term : HeuristicTerms)
     {
          if (Seen.insert(Term).second)
          {
               Terms.push_back(Term);
          }
     }

     if (Terms.size() > 24)
     {
          Terms.resize(24);
     }

     return Terms;
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const std::string Normalized = NormalizeTerm(Query);

     if (Normalized.empty())
     {
          return Hits;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          return Hits;
     }

     const std::string Prefix = "sam:term:" + Normalized + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          try
          {
               nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
               LookupHit Hit;
               Hit.Collection = Payload.value("collection", "");
               Hit.DocumentID = Payload.value("id", "");
               Hit.Title = Payload.value("title", "");
               Hit.MatchedTerm = Payload.value("term", "");

               if (!Hit.Collection.empty() && !Hit.DocumentID.empty())
               {
                    Hits.push_back(std::move(Hit));
               }
          }
          catch (...)
          {
          }

          if (Hits.size() >= Limit)
          {
               break;
          }
     }

     return Hits;
}
