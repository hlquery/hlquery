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

#include <algorithm>
#include <cctype>
#include <sstream>
#include <set>
#include <iterator>
#include <filesystem>
#include <cmath>
#include <numeric>

#include "core/hlquery.h"
#include "rocksdb/inverted_index.h"
#include "rocksdb/hybrid_storage.h"
#include "rocksdb/mmap_index.h"
#include "core/config.h"
#include "core/serverconfig.h"

namespace hlquery_storage
{
     /*
      * NormalizeTerm - Normalizes a term by converting to lowercase and removing punctuation.
      */

     std::string InvertedIndex::NormalizeTerm(const std::string& Term)
     {
          std::string Normalized = Term;

          std::transform(Normalized.begin(), Normalized.end(), Normalized.begin(), ::tolower);

          /* Remove punctuation at start and end, but preserve wildcards and underscores. */

          while (!Normalized.empty() && !std::isalnum(static_cast<unsigned char>(Normalized.front())) && Normalized.front() != '*' && Normalized.front() != '?' && Normalized.front() != '_')
          {

               Normalized.erase(0, 1);
          }

          while (!Normalized.empty() && !std::isalnum(static_cast<unsigned char>(Normalized.back())) && Normalized.back() != '*' && Normalized.back() != '?' && Normalized.back() != '_')
          {

               Normalized.pop_back();
          }

          return Normalized;
     }

     /*
      * ExtractTerms - Extracts and normalizes terms from a given text.
      */

     std::vector<std::string> InvertedIndex::ExtractTerms(const std::string& Text)
     {
          std::vector<std::string> Terms;

          if (Text.empty())
          {
               return Terms;
          }

          /* Limit text size to prevent processing extremely large documents. */

          const size_t MaxTextSize = 1000000;

          const std::string& TextToProcess = (Text.length() > MaxTextSize) ? Text.substr(0, MaxTextSize) : Text;

          std::set<std::string> UniqueTerms;

          size_t Pos = 0;

          size_t TextLen = TextToProcess.length();

          const size_t MaxTerms = 100000;

          size_t TermCount = 0;

          while (Pos < TextLen && TermCount < MaxTerms)
          {
               /* Skip whitespace and punctuation that should be treated as separators. */

               while (Pos < TextLen && (std::isspace(static_cast<unsigned char>(TextToProcess[Pos])) || TextToProcess[Pos] == '-' || TextToProcess[Pos] == '.' || TextToProcess[Pos] == ',' || TextToProcess[Pos] == ':' || TextToProcess[Pos] == '/' || TextToProcess[Pos] == '\\' || TextToProcess[Pos] == '(' || TextToProcess[Pos] == ')' || TextToProcess[Pos] == '[' || TextToProcess[Pos] == ']' || TextToProcess[Pos] == '{' || TextToProcess[Pos] == '}' || TextToProcess[Pos] == '@' || TextToProcess[Pos] == '#' || TextToProcess[Pos] == '$' || TextToProcess[Pos] == '%' || TextToProcess[Pos] == '&' || TextToProcess[Pos] == '+' || TextToProcess[Pos] == '=' || TextToProcess[Pos] == ';' || TextToProcess[Pos] == '|' || TextToProcess[Pos] == '!' || TextToProcess[Pos] == '?' || TextToProcess[Pos] == '~' || TextToProcess[Pos] == '^' || TextToProcess[Pos] == '`'))
               {
                    Pos++;
               }

               if (Pos >= TextLen)
               {
                    break;
               }

               /* Find end of word. */

               size_t WordStart = Pos;

               while (Pos < TextLen && !std::isspace(static_cast<unsigned char>(TextToProcess[Pos])) && TextToProcess[Pos] != '-' && TextToProcess[Pos] != '.' && TextToProcess[Pos] != ',' && TextToProcess[Pos] != ':' && TextToProcess[Pos] != '/' && TextToProcess[Pos] != '\\' && TextToProcess[Pos] != '(' && TextToProcess[Pos] != ')' && TextToProcess[Pos] != '[' && TextToProcess[Pos] != ']' && TextToProcess[Pos] != '{' && TextToProcess[Pos] != '}' && TextToProcess[Pos] != '@' && TextToProcess[Pos] != '#' && TextToProcess[Pos] != '$' && TextToProcess[Pos] != '%' && TextToProcess[Pos] != '&' && TextToProcess[Pos] != '+' && TextToProcess[Pos] != '=' && TextToProcess[Pos] != ';' && TextToProcess[Pos] != '|' && TextToProcess[Pos] != '!' && TextToProcess[Pos] != '?' && TextToProcess[Pos] != '~' && TextToProcess[Pos] != '^' && TextToProcess[Pos] != '`')
               {
                    Pos++;
               }

               if (Pos > WordStart)
               {

                    std::string Word = TextToProcess.substr(WordStart, Pos - WordStart);

                    std::string Normalized = NormalizeTerm(Word);

                    if (!Normalized.empty() && Normalized.length() >= 1)
                    {

                         UniqueTerms.insert(Normalized);

                         TermCount++;
                    }
               }
          }

          Terms.assign(UniqueTerms.begin(), UniqueTerms.end());

          return Terms;
     }

     /*
      * RemoveDocumentFromIndex - Removes a document from the inverted index.
      */

     void InvertedIndex::RemoveDocumentFromIndex(const std::string& Collection, const std::string& DocID)
     {

          auto IndexIt = Index.find(Collection);

          if (IndexIt == Index.end())
          {
               return;
          }

          auto DocTermsIt = DocumentTerms.find(Collection);

          if (DocTermsIt == DocumentTerms.end())
          {
               return;
          }

          auto TermsIt = DocTermsIt->second.find(DocID);

          if (TermsIt == DocTermsIt->second.end())
          {
               return;
          }

          auto& CollectionIndex = IndexIt->second;

          for (const auto& Term : TermsIt->second)
          {

               auto TermIt = CollectionIndex.find(Term);

               if (TermIt != CollectionIndex.end())
               {
                    auto& Postings = TermIt->second;

                    Postings.erase(std::remove_if(Postings.begin(), Postings.end(), [&DocID](const Posting& p)
                    {
                         return p.DocumentID == DocID;

                    }), Postings.end());

                    if (Postings.empty())
                    {

                         CollectionIndex.erase(TermIt);
                    }
               }
          }

          DocTermsIt->second.erase(TermsIt);

          if (DocTermsIt->second.empty())
          {

               DocumentTerms.erase(DocTermsIt);
          }

          auto DocLengthsIt = DocumentLengths.find(Collection);

          if (DocLengthsIt != DocumentLengths.end())
          {

               DocLengthsIt->second.erase(DocID);

               if (DocLengthsIt->second.empty())
               {

                    DocumentLengths.erase(DocLengthsIt);
               }
          }

          auto DocLengthsItAfter = DocumentLengths.find(Collection);

          if (DocLengthsItAfter != DocumentLengths.end() && !DocLengthsItAfter->second.empty())
          {
               size_t TotalLength = 0;

               size_t DocCountValue = 0;

               for (const auto& [DocIDIter, Length] : DocLengthsItAfter->second)
               {
                    TotalLength += Length;

                    DocCountValue++;
               }

               DocCounts[Collection] = DocCountValue;

               AvgDocLengths[Collection] = (DocCountValue > 0) ? static_cast<double>(TotalLength) / static_cast<double>(DocCountValue) : 1.0;
          }
          else
          {
               AvgDocLengths[Collection] = 1.0;

               DocCounts[Collection] = 0;
          }
     }

     /*
      * AddDocument - Adds a document to the inverted index.
      */

     bool InvertedIndex::AddDocument(const std::string& Collection, const Document& Doc)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Indexing document '" + Doc.ID + "' in collection '" + Collection + "'.");
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Checking document count.");
          }

          auto DocTermsIt = DocumentTerms.find(Collection);

          size_t CollectionDocCount = (DocTermsIt != DocumentTerms.end()) ? DocTermsIt->second.size() : 0;

          if (CollectionDocCount >= INVERTED_INDEX_MAX_DOCUMENTS_PER_COLLECTION)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {

                    Instance->Logs->Debug("inverted_index", "AddDocument: Collection '" + Collection + "' has reached maximum document limit (" + std::to_string(INVERTED_INDEX_MAX_DOCUMENTS_PER_COLLECTION) + "), skipping index update to prevent memory exhaustion.");
               }

               return false;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Checking term count.");
          }

          auto IndexIt = Index.find(Collection);

          size_t CollectionTermCount = (IndexIt != Index.end()) ? IndexIt->second.size() : 0;

          if (CollectionTermCount >= INVERTED_INDEX_MAX_TERMS_PER_COLLECTION)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {

                    Instance->Logs->Debug("inverted_index", "AddDocument: Collection '" + Collection + "' has reached maximum term limit (" + std::to_string(INVERTED_INDEX_MAX_TERMS_PER_COLLECTION) + "), skipping index update to prevent memory exhaustion.");
               }

               return false;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Removing old document if exists.");
          }

          RemoveDocumentFromIndex(Collection, Doc.ID);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Extracting terms from title (length=" + std::to_string(Doc.Title.length()) + ") and content (length=" + std::to_string(Doc.Content.length()) + ").");
          }

          std::vector<std::string> TitleTerms = ExtractTerms(Doc.Title);

          std::vector<std::string> ContentTerms = ExtractTerms(Doc.Content);

          std::vector<std::string> IDTerms = ExtractTerms(Doc.ID);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Extracted " + std::to_string(TitleTerms.size()) + " title terms, " + std::to_string(ContentTerms.size()) + " content terms and " + std::to_string(IDTerms.size()) + " id terms.");
          }

          std::vector<std::string> AllTermsList;

          std::set<std::string> AllTermsSet;

          for (const auto& Term : TitleTerms)
          {

               AllTermsList.push_back(Term);

               AllTermsSet.insert(Term);
          }

          for (const auto& Term : ContentTerms)
          {

               AllTermsList.push_back(Term);

               AllTermsSet.insert(Term);
          }

          for (const auto& Term : IDTerms)
          {

               AllTermsList.push_back(Term);

               AllTermsSet.insert(Term);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Extracting terms from " + std::to_string(Doc.Fields.size()) + " custom fields.");
          }

          size_t FieldCount = 0;

          for (const auto& Field : Doc.Fields)
          {
               FieldCount++;

               if (FieldCount % 100 == 0)
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {

                         Instance->Logs->Debug("inverted_index", "AddDocument: Processing field " + std::to_string(FieldCount) + "/" + std::to_string(Doc.Fields.size()) + ".");
                    }
               }

               std::vector<std::string> FieldTerms = ExtractTerms(Field.second);

               for (const auto& Term : FieldTerms)
               {

                    AllTermsList.push_back(Term);

                    AllTermsSet.insert(Term);
               }
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Total terms: " + std::to_string(AllTermsList.size()) + " (unique: " + std::to_string(AllTermsSet.size()) + ").");
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Adding terms to index.");
          }

          auto& CollectionIndex = Index[Collection];

          auto& DocTerms = DocumentTerms[Collection][Doc.ID];

          DocTerms.clear();

          size_t DocLength = AllTermsList.size();

          std::unordered_map<std::string, std::vector<size_t>> TermPositions;

          size_t Pos = 0;

          for (const auto& T : AllTermsList)
          {

               TermPositions[T].push_back(Pos);

               Pos++;
          }

          size_t TermIndexCount = 0;

          for (const auto& Term : AllTermsSet)
          {
               TermIndexCount++;

               if (TermIndexCount % 1000 == 0)
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {

                         Instance->Logs->Debug("inverted_index", "AddDocument: Indexing term " + std::to_string(TermIndexCount) + "/" + std::to_string(AllTermsSet.size()) + ".");
                    }
               }

               Posting Post;

               Post.DocumentID = Doc.ID;

               Post.Collection = Collection;

               Post.Score = 1.0;

               auto PosIt = TermPositions.find(Term);

               if (PosIt != TermPositions.end())
               {
                    Post.Positions = PosIt->second;
               }

               bool InTitle = std::find(TitleTerms.begin(), TitleTerms.end(), Term) != TitleTerms.end();

               if (InTitle)
               {
                    Post.Score = 2.0;
               }

               CollectionIndex[Term].push_back(Post);

               DocTerms.insert(Term);
          }

          DocumentLengths[Collection][Doc.ID] = DocLength;

          auto DocLengthsIt = DocumentLengths.find(Collection);

          if (DocLengthsIt != DocumentLengths.end() && !DocLengthsIt->second.empty())
          {
               size_t TotalLength = 0;

               size_t DocCountValue = 0;

               for (const auto& [DocID, Length] : DocLengthsIt->second)
               {
                    TotalLength += Length;

                    DocCountValue++;
               }

               DocCounts[Collection] = DocCountValue;

               AvgDocLengths[Collection] = (DocCountValue > 0) ? static_cast<double>(TotalLength) / static_cast<double>(DocCountValue) : 1.0;
          }
          else
          {
               AvgDocLengths[Collection] = 1.0;

               DocCounts[Collection] = 0;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "AddDocument: Indexed " + std::to_string(AllTermsSet.size()) + " unique terms for document '" + Doc.ID + "' (length: " + std::to_string(DocLength) + ").");
          }

          return true;
     }

     /*
      * DeleteDocument - Deletes a document from the inverted index.
      */

     bool InvertedIndex::DeleteDocument(const std::string& Collection, const std::string& DocID)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "DeleteDocument: Removing document '" + DocID + "' from collection '" + Collection + "'.");
          }

          RemoveDocumentFromIndex(Collection, DocID);

          return true;
     }

     /*
      * UpdateDocument - Updates a document in the inverted index.
      */

     bool InvertedIndex::UpdateDocument(const std::string& Collection, const Document& OldDoc, const Document& NewDoc)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "UpdateDocument: Updating document '" + OldDoc.ID + "' in collection '" + Collection + "'.");
          }

          std::set<std::string> OldTerms;

          if (!OldDoc.ID.empty())
          {

               auto DocTermsIt = DocumentTerms.find(Collection);

               if (DocTermsIt != DocumentTerms.end())
               {

                    auto TermsIt = DocTermsIt->second.find(OldDoc.ID);

                    if (TermsIt != DocTermsIt->second.end())
                    {

                         OldTerms.insert(TermsIt->second.begin(), TermsIt->second.end());
                    }
               }
          }

          RemoveDocumentFromIndex(Collection, OldDoc.ID);

          std::vector<std::string> TitleTerms = ExtractTerms(NewDoc.Title);

          std::vector<std::string> ContentTerms = ExtractTerms(NewDoc.Content);

          std::vector<std::string> IDTerms = ExtractTerms(NewDoc.ID);

          std::set<std::string> AllTerms;

          for (const auto& Term : TitleTerms)
          {

               AllTerms.insert(Term);
          }

          for (const auto& Term : ContentTerms)
          {

               AllTerms.insert(Term);
          }

          for (const auto& Term : IDTerms)
          {

               AllTerms.insert(Term);
          }

          for (const auto& Field : NewDoc.Fields)
          {

               std::vector<std::string> FieldTerms = ExtractTerms(Field.second);

               for (const auto& Term : FieldTerms)
               {

                    AllTerms.insert(Term);
               }
          }

          auto& CollectionIndex = Index[Collection];

          auto& DocTerms = DocumentTerms[Collection][NewDoc.ID];

          DocTerms.clear();

          size_t DocLength = 0;

          size_t TermCount = 0;

          for (const auto& Term : AllTerms)
          {
               TermCount++;

               DocLength++;

               Posting Post;

               Post.DocumentID = NewDoc.ID;

               Post.Collection = Collection;

               Post.Score = 1.0;

               Post.Positions.push_back(TermCount);

               bool InTitle = std::find(TitleTerms.begin(), TitleTerms.end(), Term) != TitleTerms.end();

               if (InTitle)
               {
                    Post.Score = 2.0;
               }

               CollectionIndex[Term].push_back(Post);

               DocTerms.insert(Term);
          }

          DocumentLengths[Collection][NewDoc.ID] = DocLength;

          auto DocLengthsIt = DocumentLengths.find(Collection);

          if (DocLengthsIt != DocumentLengths.end() && !DocLengthsIt->second.empty())
          {
               size_t TotalLength = 0;

               size_t DocCountValue = 0;

               for (const auto& [DocID, Length] : DocLengthsIt->second)
               {
                    TotalLength += Length;

                    DocCountValue++;
               }

               DocCounts[Collection] = DocCountValue;

               AvgDocLengths[Collection] = (DocCountValue > 0) ? static_cast<double>(TotalLength) / static_cast<double>(DocCountValue) : 1.0;
          }
          else
          {
               AvgDocLengths[Collection] = 1.0;

               DocCounts[Collection] = 0;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "UpdateDocument: Updated document '" + NewDoc.ID + "' with " + std::to_string(AllTerms.size()) + " terms.");
          }

          return true;
     }

     /*
      * SearchTerm - Searches for a single term in the index.
      */

     std::vector<Posting> InvertedIndex::SearchTerm(const std::string& Collection, const std::string& Term)
     {

          std::string NormalizedTerm = NormalizeTerm(Term);

          {

               std::lock_guard<std::mutex> Lock(IndexMutex);

               auto CollectionIt = Index.find(Collection);

               bool HasMemoryIndex = (CollectionIt != Index.end() && !CollectionIt->second.empty());

               if (HasMemoryIndex)
               {

                    auto TermIt = CollectionIt->second.find(NormalizedTerm);

                    if (TermIt != CollectionIt->second.end())
                    {
                         return TermIt->second;
                    }
               }
          }

          MMapIndex* MMapIdx = nullptr;

          {

               auto MMapIt = MMapIndexes.find(Collection);

               if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
               {

                    MMapIdx = MMapIt->second.get();
               }
          }

          if (MMapIdx)
          {

               auto Results = MMapIdx->SearchTerm(NormalizedTerm);

               for (auto& Post : Results)
               {
                    Post.Collection = Collection;
               }

               return Results;
          }

          return {};
     }

     /*
      * Search - Performs a multi-term search in the index.
      */

     std::vector<Posting> InvertedIndex::Search(const std::string& Collection, const std::string& Query, int Limit)
     {
          if (Limit < 0)
          {
               Limit = 0;
          }

          if (Limit > 10000)
          {
               Limit = 10000;
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "Search: Searching for '" + Query + "' in collection '" + Collection + "' with limit " + std::to_string(Limit) + ".");
          }

          std::vector<std::string> QueryTerms = ExtractTerms(Query);

          if (QueryTerms.empty())
          {
               return {};
          }

          bool UseMMap = false;

          MMapIndex* MMapIdx = nullptr;

          {

               std::lock_guard<std::mutex> Lock(IndexMutex);

               auto MMapIt = MMapIndexes.find(Collection);

               if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
               {
                    UseMMap = true;

                    MMapIdx = MMapIt->second.get();
               }
          }

          std::vector<std::unordered_map<std::string, Posting>> TermResults;

          if (UseMMap && MMapIdx)
          {
               for (const auto& TermValue : QueryTerms)
               {

                    std::string Normalized = NormalizeTerm(TermValue);

                    if (Normalized.empty())
                    {
                         continue;
                    }

                    auto Postings = MMapIdx->SearchTerm(Normalized);

                    std::unordered_map<std::string, Posting> TermDocs;

                    for (const auto& Post : Postings)
                    {
                         TermDocs[Post.DocumentID] = Post;

                         TermDocs[Post.DocumentID].Collection = Collection;
                    }

                    if (!TermDocs.empty())
                    {

                         TermResults.push_back(TermDocs);
                    }
                    else
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {

                              Instance->Logs->Debug("inverted_index", "Search: Term '" + Normalized + "' has no matches, returning empty results (AND logic).");
                         }

                         return {};
                    }
               }
          }
          else
          {

               std::lock_guard<std::mutex> Lock(IndexMutex);

               for (const auto& TermValue : QueryTerms)
               {

                    std::string Normalized = NormalizeTerm(TermValue);

                    if (Normalized.empty())
                    {
                         continue;
                    }

                    auto CollectionIt = Index.find(Collection);

                    if (CollectionIt == Index.end())
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {

                              Instance->Logs->Debug("inverted_index", "Search: Collection not found in index, returning empty results (AND logic).");
                         }

                         return {};
                    }

                    auto TermIt = CollectionIt->second.find(Normalized);

                    if (TermIt == CollectionIt->second.end())
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {

                              Instance->Logs->Debug("inverted_index", "Search: Term '" + Normalized + "' not found in index, returning empty results (AND logic).");
                         }

                         return {};
                    }

                    std::vector<Posting> Postings = TermIt->second;

                    std::unordered_map<std::string, Posting> TermDocs;

                    for (const auto& Post : Postings)
                    {
                         TermDocs[Post.DocumentID] = Post;
                    }

                    if (!TermDocs.empty())
                    {

                         TermResults.push_back(TermDocs);
                    }
                    else
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {

                              Instance->Logs->Debug("inverted_index", "Search: Term '" + Normalized + "' has no matches, returning empty results (AND logic).");
                         }

                         return {};
                    }
               }
          }

          if (TermResults.empty())
          {
               return {};
          }

          std::unordered_map<std::string, Posting> DocScores;

          double K1 = 1.2;

          double B = 0.75;

          double Delta = 1.0;

          bool PivotEnabled = false;

          double PivotValue = 0.25;

          K1 = Instance->Config->GetRankingK1();

          B = Instance->Config->GetRankingB();

          Delta = Instance->Config->GetRankingDelta();

          PivotEnabled = Instance->Config->GetPivotNormEnabled();

          PivotValue = Instance->Config->GetPivotNormPivot();

          double AvgDocLengthValue = 1.0;

          size_t CollectionSizeValue = 0;

          {

               std::lock_guard<std::mutex> Lock(IndexMutex);

               auto AvgIt = AvgDocLengths.find(Collection);

               if (AvgIt != AvgDocLengths.end())
               {
                    AvgDocLengthValue = AvgIt->second;
               }

               auto CountIt = DocCounts.find(Collection);

               if (CountIt != DocCounts.end())
               {
                    CollectionSizeValue = CountIt->second;
               }
          }

          std::vector<size_t> TermOrder(TermResults.size());

          std::iota(TermOrder.begin(), TermOrder.end(), 0);

          std::sort(TermOrder.begin(), TermOrder.end(), [&TermResults](size_t IndexA, size_t IndexB)
          {

               return TermResults[IndexA].size() < TermResults[IndexB].size();
          });

          for (const auto& Pair : TermResults[TermOrder[0]])
          {
               DocScores[Pair.first] = Pair.second;
          }

          for (size_t Idx = 1; Idx < TermOrder.size(); ++Idx)
          {
               size_t I = TermOrder[Idx];

               const auto& CurrentTermDocs = TermResults[I];

               if (CurrentTermDocs.size() > DocScores.size() * 10 && DocScores.size() > 100)
               {

                    std::vector<std::pair<std::string, Posting>> SortedDocs(DocScores.begin(), DocScores.end());

                    std::sort(SortedDocs.begin(), SortedDocs.end(), [](const auto& DocA, const auto& DocB)
                    {
                         return DocA.first < DocB.first;
                    });

                    std::vector<std::pair<std::string, Posting>> SortedCurrent(CurrentTermDocs.begin(), CurrentTermDocs.end());

                    std::sort(SortedCurrent.begin(), SortedCurrent.end(), [](const auto& DocA, const auto& DocB)
                    {
                         return DocA.first < DocB.first;
                    });

                    std::unordered_map<std::string, Posting> Intersection;

                    size_t Pos1 = 0;

                    size_t Pos2 = 0;

                    while (Pos1 < SortedDocs.size() && Pos2 < SortedCurrent.size())
                    {
                         if (SortedDocs[Pos1].first < SortedCurrent[Pos2].first)
                         {
                              Pos1++;
                         }
                         else if (SortedCurrent[Pos2].first < SortedDocs[Pos1].first)
                         {
                              Pos2++;
                         }
                         else
                         {
                              Intersection[SortedDocs[Pos1].first] = SortedDocs[Pos1].second;

                              for (const auto& PosVal : SortedCurrent[Pos2].second.Positions)
                              {

                                   Intersection[SortedDocs[Pos1].first].Positions.push_back(PosVal);
                              }

                              Pos1++;

                              Pos2++;
                         }
                    }

                    DocScores = std::move(Intersection);
               }
               else
               {
                    std::unordered_map<std::string, Posting> Intersection;

                    for (const auto& Pair : CurrentTermDocs)
                    {

                         auto It = DocScores.find(Pair.first);

                         if (It != DocScores.end())
                         {
                              Intersection[Pair.first] = It->second;

                              for (const auto& PosVal : Pair.second.Positions)
                              {

                                   Intersection[Pair.first].Positions.push_back(PosVal);
                              }
                         }
                    }

                    DocScores = std::move(Intersection);
               }
          }

          std::vector<Posting> Results;

          Results.reserve(DocScores.size());

          for (auto& [DocID, Post] : DocScores)
          {
               double DocLengthValue = 1.0;

               {

                    std::lock_guard<std::mutex> Lock(IndexMutex);

                    auto DocLenIt = DocumentLengths[Collection].find(DocID);

                    if (DocLenIt != DocumentLengths[Collection].end())
                    {

                         DocLengthValue = static_cast<double>(DocLenIt->second);
                    }
               }

               double TotalScore = 0.0;

               for (const auto& TermValue : QueryTerms)
               {

                    std::string Normalized = NormalizeTerm(TermValue);

                    if (Normalized.empty())
                    {
                         continue;
                    }

                    double TermFreq = static_cast<double>(CalculateTermFrequency(Collection, DocID, Normalized));

                    double DocFreq = static_cast<double>(CalculateDocumentFrequency(Collection, Normalized));

                    double TermScoreValue = 0.0;

                    if (PivotEnabled)
                    {

                         TermScoreValue = CalculatePivotNormScore(TermFreq, DocFreq, DocLengthValue, AvgDocLengthValue, static_cast<double>(CollectionSizeValue), PivotValue);
                    }
                    else
                    {

                         TermScoreValue = CalculateBM25PlusScore(TermFreq, DocFreq, DocLengthValue, AvgDocLengthValue, static_cast<double>(CollectionSizeValue), K1, B, Delta);
                    }

                    TotalScore += TermScoreValue;
               }

               if (QueryTerms.size() >= 2)
               {

                    double ProximityBoostValue = CalculateProximityBoost(QueryTerms, {Post}, DocID);

                    TotalScore *= ProximityBoostValue;
               }

               Post.Score = TotalScore;

               Results.push_back(Post);
          }

          std::sort(Results.begin(), Results.end(), [](const Posting& PostingA, const Posting& PostingB)
          {
               return PostingA.Score > PostingB.Score;
          });

          if (Limit > 0 && static_cast<int>(Results.size()) > Limit)
          {

               Results.resize(Limit);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "Search: Found " + std::to_string(Results.size()) + " results for query '" + Query + "' (using " + (UseMMap ? "mmap" : "memory") + " index).");
          }

          return Results;
     }

     /*
      * SearchPrefix - Searches for terms with a given prefix in the index.
      */

     std::vector<Posting> InvertedIndex::SearchPrefix(const std::string& Collection, const std::string& Prefix, int Limit)
     {
          if (Limit < 0)
          {
               Limit = 0;
          }

          if (Limit > 10000)
          {
               Limit = 10000;
          }

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "SearchPrefix: Searching for prefix '" + Prefix + "' in collection '" + Collection + "' with limit " + std::to_string(Limit) + ".");
          }

          std::string NormalizedPrefix = NormalizeTerm(Prefix);

          if (NormalizedPrefix.empty())
          {
               return {};
          }

          auto CollectionIt = Index.find(Collection);

          if (CollectionIt == Index.end())
          {
               return {};
          }

          std::unordered_map<std::string, Posting> DocScores;

          for (const auto& [TermValue, Postings] : CollectionIt->second)
          {
               if (TermValue.length() >= NormalizedPrefix.length() && TermValue.substr(0, NormalizedPrefix.length()) == NormalizedPrefix)
               {
                    for (const auto& Post : Postings)
                    {

                         auto It = DocScores.find(Post.DocumentID);

                         if (It == DocScores.end())
                         {
                              DocScores[Post.DocumentID] = Post;
                         }
                         else
                         {
                              It->second.Score += Post.Score * 0.9;
                         }
                    }
               }
          }

          std::vector<Posting> Results;

          Results.reserve(DocScores.size());

          for (auto& Pair : DocScores)
          {

               Results.push_back(Pair.second);
          }

          std::sort(Results.begin(), Results.end(), [](const Posting& PostingA, const Posting& PostingB)
          {
               return PostingA.Score > PostingB.Score;
          });

          if (Limit > 0 && static_cast<int>(Results.size()) > Limit)
          {

               Results.resize(Limit);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "SearchPrefix: Found " + std::to_string(Results.size()) + " results for prefix '" + Prefix + "'.");
          }

          return Results;
     }

     /*
      * DeleteCollection - Deletes an entire collection from the index.
      */

     void InvertedIndex::DeleteCollection(const std::string& Collection)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          Index.erase(Collection);

          DocumentTerms.erase(Collection);

          DocumentLengths.erase(Collection);

          DocCounts.erase(Collection);

          AvgDocLengths.erase(Collection);

          MMapIndexes.erase(Collection);

          if (Instance && Instance->Database)
          {

               Instance->Database->Del("flush_pending:" + Collection);
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "DeleteCollection: Removed collection '" + Collection + "' from index.");
          }
     }

     /*
      * Clear - Clears all in-memory and mmap index data.
      */

     void InvertedIndex::Clear()
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          Index.clear();

          DocumentTerms.clear();

          MMapIndexes.clear();
     }

     /*
      * InvalidateDocumentCache - Invalidates the cache for a specific document.
      */

     void InvertedIndex::InvalidateDocumentCache(const std::string& Collection, const std::string& DocID)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto DocTermsIt = DocumentTerms.find(Collection);

          if (DocTermsIt != DocumentTerms.end())
          {

               auto TermsIt = DocTermsIt->second.find(DocID);

               if (TermsIt != DocTermsIt->second.end())
               {
                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {

                         Instance->Logs->Debug("inverted_index", "InvalidateDocumentCache: Invalidated cache for document '" + DocID + "' in collection '" + Collection + "'.");
                    }
               }
          }
     }

     /*
      * InvalidateCollectionCache - Invalidates the cache for an entire collection.
      */

     void InvertedIndex::InvalidateCollectionCache(const std::string& Collection)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {

               Instance->Logs->Debug("inverted_index", "InvalidateCollectionCache: Invalidated cache for collection '" + Collection + "'.");
          }
     }

     /*
      * GetTermCount - Returns the number of terms in a collection.
      */

     size_t InvertedIndex::GetTermCount(const std::string& Collection) const
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto It = Index.find(Collection);

          if (It == Index.end())
          {
               return 0;
          }

          return It->second.size();
     }

     /*
      * GetDocumentCount - Returns the number of documents in a collection.
      */

     size_t InvertedIndex::GetDocumentCount(const std::string& Collection) const
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto It = DocumentTerms.find(Collection);

          if (It != DocumentTerms.end() && !It->second.empty())
          {

               return It->second.size();
          }

          auto MMapIt = MMapIndexes.find(Collection);

          if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
          {
               return 1;
          }

          return 0;
     }

     /*
      * GetIndexDir - Returns the directory where indexes are stored.
      */

     std::string InvertedIndex::GetIndexDir() const
     {

          std::string BaseDir = std::string(HLQUERY_DATA_DIR);

          if (Instance && Instance->Config && Instance->Config->IsValid())
          {

               const auto& RocksDBOpts = Instance->Config->GetRocksDBOptions();

               if (!RocksDBOpts.DataDir.empty())
               {
                    BaseDir = RocksDBOpts.DataDir;
               }
          }

          return BaseDir + "/rocksdb/indices";
     }

     /*
      * FlushToDisk - Flushes in-memory indexes to disk.
      */

     void InvertedIndex::FlushToDisk(const std::string& IndexDir)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs)
          {

               Instance->Logs->Normal("inverted_index", "FlushToDisk: Flushing indexes to disk.");
          }

          for (const auto& [Collection, TermMap] : Index)
          {
               if (TermMap.empty())
               {
                    continue;
               }

               std::string FlushMarker = "flush_pending:" + Collection;

               if (Instance && Instance->Database)
               {

                    Instance->Database->Set(FlushMarker, "1");

                    Instance->Database->FlushAndSync();
               }

               IndexWriter CollectionWriter(IndexDir, Collection);

               std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>> SingleCollection;

               SingleCollection[Collection] = TermMap;

               if (CollectionWriter.WriteIndex(SingleCollection))
               {
                    if (Instance && Instance->Database)
                    {

                         Instance->Database->Del(FlushMarker);

                         Instance->Database->FlushAndSync();
                    }

                    if (Instance && Instance->Logs)
                    {

                         Instance->Logs->Normal("inverted_index", "FlushToDisk: Flushed collection '" + Collection + "' to disk.");
                    }
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {

                         Instance->Logs->Normal("inverted_index", "FlushToDisk: Failed to flush collection '" + Collection + "'.");
                    }
               }
          }
     }

     /*
      * LoadFromDisk - Loads indexes from disk into memory.
      */

     void InvertedIndex::LoadFromDisk(const std::string& IndexDir)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          if (Instance && Instance->Logs)
          {

               Instance->Logs->Normal("inverted_index", "LoadFromDisk: Loading indexes from disk.");
          }

          if (!std::filesystem::exists(IndexDir))
          {
               if (Instance && Instance->Logs)
               {

                    Instance->Logs->Normal("inverted_index", "LoadFromDisk: Index directory does not exist: " + IndexDir + ".");
               }

               return;
          }

          for (const auto& Entry : std::filesystem::directory_iterator(IndexDir))
          {
               if (!Entry.is_directory())
               {
                    continue;
               }

               std::string Collection = Entry.path().filename().string();

               std::string CollectionDir = Entry.path().string();

               std::string FlushMarker = "flush_pending:" + Collection;

               if (Instance && Instance->Database)
               {

                    std::string Val = Instance->Database->Get(FlushMarker);

                    if (!Val.empty())
                    {
                         if (Instance && Instance->Logs)
                         {

                              Instance->Logs->Normal("inverted_index", "LoadFromDisk: Detected interrupted flush for collection '" + Collection + "'. The mmap index may be corrupted. Triggering repair.");
                         }

                         try
                         {

                              std::filesystem::remove_all(CollectionDir);
                         }
                         catch (...)
                         {
                         }

                         continue;
                    }
               }

               auto MMapIdx = MMapIndex::Open(IndexDir, Collection);

               if (MMapIdx && MMapIdx->IsValid())
               {

                    MMapIndexes[Collection] = std::move(MMapIdx);

                    if (Instance && Instance->Logs)
                    {

                         Instance->Logs->Normal("inverted_index", "LoadFromDisk: Loaded mmap index for collection '" + Collection + "' (" + std::to_string(MMapIndexes[Collection]->GetTermCount()) + " terms).");
                    }
               }
          }
     }

     /*
      * HasMMapIndex - Checks if a collection has an associated mmap index.
      */

     bool InvertedIndex::HasMMapIndex(const std::string& Collection) const
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto It = MMapIndexes.find(Collection);

          return It != MMapIndexes.end() && It->second && It->second->IsValid();
     }

     /*
      * CalculateBM25PlusScore - Calculates the BM25+ score for a term in a document.
      */

     double InvertedIndex::CalculateBM25PlusScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1, double B, double Delta) const
     {
          if (CollectionSize <= 0 || DocFreq <= 0 || DocLength <= 0)
          {
               return 0.0;
          }

          double Idf = std::log((CollectionSize - DocFreq + 0.5) / (DocFreq + 0.5));

          if (Idf < 0.0)
          {
               Idf = 0.0;
          }

          double NormalizedLengthValue = DocLength / std::max(AvgDocLength, 1.0);

          double NumeratorValue = (TermFreq + Delta) * (K1 + 1.0);

          double DenominatorValue = TermFreq + Delta + K1 * (1.0 - B + B * NormalizedLengthValue);

          if (DenominatorValue <= 0.0)
          {
               DenominatorValue = 1.0;
          }

          double ScoreValueResult = Idf * (NumeratorValue / DenominatorValue);

          return ScoreValueResult;
     }

     /*
      * CalculateBM25LScore - Calculates the BM25L score for a term in a document.
      */

     double InvertedIndex::CalculateBM25LScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1, double B, double Delta) const
     {
          if (CollectionSize <= 0 || DocFreq <= 0 || DocLength <= 0)
          {
               return 0.0;
          }

          double Idf = std::log((CollectionSize - DocFreq + 0.5) / (DocFreq + 0.5));

          if (Idf < 0.0)
          {
               Idf = 0.0;
          }

          double NormalizedLengthValue = DocLength / std::max(AvgDocLength, 1.0);

          double NormFactorValue = 1.0 - B + B * NormalizedLengthValue;

          NormFactorValue = std::max(NormFactorValue, 1.0);

          double NumeratorValue = (TermFreq + Delta) * (K1 + 1.0);

          double DenominatorValue = TermFreq + Delta + K1 * NormFactorValue;

          if (DenominatorValue <= 0.0)
          {
               DenominatorValue = 1.0;
          }

          double ScoreValueResult = Idf * (NumeratorValue / DenominatorValue);

          return ScoreValueResult;
     }

     /*
      * CalculatePivotNormScore - Calculates the score using pivot-based normalization.
      */

     double InvertedIndex::CalculatePivotNormScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double Pivot) const
     {
          if (CollectionSize <= 0 || DocFreq <= 0 || DocLength <= 0 || TermFreq <= 0)
          {
               return 0.0;
          }

          /* IDF calculation */

          double Idf = std::log((CollectionSize + 1.0) / DocFreq);

          /* Pivot-based normalization formula: norm = (1 - pivot) + pivot * (doc_length / avg_length) */

          double Norm = (1.0 - Pivot) + Pivot * (DocLength / std::max(AvgDocLength, 1.0));

          if (Norm <= 0.0)
          {
               Norm = 1.0;
          }

          /* TF component with pivot normalization */

          double Tf = std::log(1.0 + TermFreq) / Norm;

          return Idf * Tf;
     }

     /*
      * CalculateTermFrequency - Calculates the frequency of a term in a specific document.
      */

     size_t InvertedIndex::CalculateTermFrequency(const std::string& Collection, const std::string& DocID, const std::string& Term) const
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto CollectionIt = Index.find(Collection);

          if (CollectionIt == Index.end())
          {
               return 0;
          }

          auto TermIt = CollectionIt->second.find(Term);

          if (TermIt == CollectionIt->second.end())
          {
               return 0;
          }

          size_t TfValue = 0;

          for (const auto& Post : TermIt->second)
          {
               if (Post.DocumentID == DocID)
               {

                    TfValue = Post.Positions.size();

                    if (TfValue == 0)
                    {
                         TfValue = 1;

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {

                              Instance->Logs->Debug("inverted_index", "CalculateTermFrequency: WARNING - positions vector is empty for term '" + Term + "' in document '" + DocID + "' - using fallback tf=1.");
                         }
                    }

                    break;
               }
          }

          return TfValue;
     }

     /*
      * CalculateDocumentFrequency - Calculates how many documents contain a specific term.
      */

     size_t InvertedIndex::CalculateDocumentFrequency(const std::string& Collection, const std::string& Term) const
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto CollectionIt = Index.find(Collection);

          if (CollectionIt == Index.end())
          {
               return 0;
          }

          auto TermIt = CollectionIt->second.find(Term);

          if (TermIt == CollectionIt->second.end())
          {
               return 0;
          }

          std::unordered_set<std::string> UniqueDocs;

          for (const auto& Post : TermIt->second)
          {

               UniqueDocs.insert(Post.DocumentID);
          }

          return UniqueDocs.size();
     }

     /*
      * UpdateCollectionStatistics - Updates average document length and count for a collection.
      */

     void InvertedIndex::UpdateCollectionStatistics(const std::string& Collection)
     {

          std::lock_guard<std::mutex> Lock(IndexMutex);

          auto DocLengthsIt = DocumentLengths.find(Collection);

          if (DocLengthsIt == DocumentLengths.end() || DocLengthsIt->second.empty())
          {
               AvgDocLengths[Collection] = 1.0;

               DocCounts[Collection] = 0;

               return;
          }

          size_t TotalLengthValue = 0;

          size_t DocCountValue = 0;

          for (const auto& [DocID, Length] : DocLengthsIt->second)
          {
               TotalLengthValue += Length;

               DocCountValue++;
          }

          DocCounts[Collection] = DocCountValue;

          AvgDocLengths[Collection] = (DocCountValue > 0) ? static_cast<double>(TotalLengthValue) / static_cast<double>(DocCountValue) : 1.0;
     }

     /*
      * CalculateProximityBoost - Calculates a score boost based on how close query terms are in a document.
      */

     double InvertedIndex::CalculateProximityBoost(const std::vector<std::string>& QueryTerms, const std::vector<Posting>& Postings, const std::string& DocID) const
     {
          if (QueryTerms.size() < 2)
          {
               return 1.0;
          }

          std::vector<Posting> DocPostings;

          for (const auto& Post : Postings)
          {
               if (Post.DocumentID == DocID && !Post.Positions.empty())
               {

                    DocPostings.push_back(Post);
               }
          }

          if (DocPostings.size() < 2)
          {
               return 1.0;
          }

          size_t MinDistanceValue = SIZE_MAX;

          for (size_t I = 0; I < DocPostings.size(); ++I)
          {
               for (size_t J = I + 1; J < DocPostings.size(); ++J)
               {
                    for (size_t Pos1 : DocPostings[I].Positions)
                    {
                         for (size_t Pos2 : DocPostings[J].Positions)
                         {

                              size_t DistanceValue = (Pos1 > Pos2) ? (Pos1 - Pos2) : (Pos2 - Pos1);

                              if (DistanceValue < MinDistanceValue)
                              {
                                   MinDistanceValue = DistanceValue;
                              }
                         }
                    }
               }
          }

          if (MinDistanceValue == SIZE_MAX)
          {
               return 1.0;
          }

          double BoostValueResult = 1.0 + (1.0 / (1.0 + static_cast<double>(MinDistanceValue)));

          return std::min(BoostValueResult, 2.0);
     }

}
