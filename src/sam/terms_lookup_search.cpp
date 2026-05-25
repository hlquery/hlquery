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


#ifdef HLQUERY_SAM_SPLIT_INCLUDE

/* Search idea recording, pending queue flushing, and lookup helpers. */

bool SAM::RecordSearchIdea(const std::string& Collection,
                           const std::string& Query,
                           const std::vector<SearchIdeaDocumentRef>& Documents,
                           std::string* ErrorMessage)
{
     if (!EnqueuePendingSearchIdea(Collection, Query, Documents, ErrorMessage))
     {
          return false;
     }

     /* Wake background SAM workers so the queued idea can be flushed soon. */
     QueueCV.notify_one();
     return true;
}

bool SAM::RecordSearchInteraction(const std::string& Collection,
                                  const std::string& Query,
                                  const SearchIdeaDocumentRef& Document,
                                  std::string* ErrorMessage)
{
     return EnqueuePendingSearchInteraction(Collection, Query, Document, ErrorMessage);
}

bool SAM::EnqueuePendingSearchIdea(const std::string& Collection,
                                   const std::string& Query,
                                   const std::vector<SearchIdeaDocumentRef>& Documents,
                                   std::string* ErrorMessage)
{
     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (Collection.empty() || NormalizedQuery.empty() || Documents.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A non-empty collection, query, and result set are required.";
          }

          return false;
     }

     {
          std::vector<PendingSearchIdeaJob> DroppedJobs;

          {
               std::lock_guard<std::mutex> Lock(SearchIdeaQueueMutex);

               for (auto& Existing : PendingSearchIdeaJobs)
               {
                    if (Existing.Collection == Collection && NormalizeTerm(Existing.Query) == NormalizedQuery)
                    {
                         Existing.Query = Query;
                         Existing.Documents = Documents;
                         Existing.Attempts = std::min<size_t>(Existing.Attempts + 1, 16);
                         return true;
                    }
               }

               PendingSearchIdeaJob Job;
               Job.Collection = Collection;
               Job.Query = Query;
               Job.Documents = Documents;
               PendingSearchIdeaJobs.push_back(std::move(Job));

               constexpr size_t MaxPendingSearchIdeaJobs = 256;
               while (PendingSearchIdeaJobs.size() > MaxPendingSearchIdeaJobs)
               {
                    DroppedJobs.push_back(PendingSearchIdeaJobs.front());
                    PendingSearchIdeaJobs.pop_front();
                    ++DroppedPendingSearchIdeaJobs;
               }
          }

          for (const auto& DroppedJob : DroppedJobs)
          {
               RecordDebugEvent(DroppedJob.Collection,
                                "dropped pending search idea for '" + NormalizeTerm(DroppedJob.Query) + "' because the queue limit was reached.");
          }
     }

     return true;
}

/* Flush queued search idea updates into the SAM database. */

size_t SAM::FlushPendingSearchIdeas(size_t MaxJobs)
{
     if (MaxJobs == 0)
     {
          return 0;
     }

     size_t Flushed = 0;

     while (Flushed < MaxJobs)
     {
          PendingSearchIdeaJob Job;

          {
               std::lock_guard<std::mutex> Lock(SearchIdeaQueueMutex);
               if (PendingSearchIdeaJobs.empty())
               {
                    break;
               }

               Job = PendingSearchIdeaJobs.front();
               PendingSearchIdeaJobs.pop_front();
          }

          bool Recorded = false;
          bool DBBusy = false;

          {
               std::unique_lock<std::mutex> Lock(DBMutex, std::try_to_lock);
               if (!Lock.owns_lock())
               {
                    DBBusy = true;
               }
               else if (Database)
               {
                    Recorded = RecordSearchIdeaLocked(Job.Collection, Job.Query, Job.Documents, nullptr);
               }
          }

          if (Recorded)
          {
               ++Flushed;
               continue;
          }

          if (DBBusy)
          {
               std::lock_guard<std::mutex> Lock(SearchIdeaQueueMutex);
               PendingSearchIdeaJobs.push_front(std::move(Job));
               break;
          }

          ++Job.Attempts;

          std::lock_guard<std::mutex> Lock(SearchIdeaQueueMutex);
          if (Job.Attempts < 16)
          {
               PendingSearchIdeaJobs.push_back(std::move(Job));
          }
          break;
     }

     return Flushed;
}

bool SAM::EnqueuePendingSearchInteraction(const std::string& Collection,
                                          const std::string& Query,
                                          const SearchIdeaDocumentRef& Document,
                                          std::string* ErrorMessage)
{
     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (Collection.empty() || NormalizedQuery.empty() || Document.DocumentID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A non-empty collection, query, and document ID are required.";
          }

          return false;
     }

     {
          std::vector<PendingSearchInteractionJob> DroppedJobs;

          {
               std::lock_guard<std::mutex> Lock(SearchInteractionQueueMutex);

               for (auto& Existing : PendingSearchInteractionJobs)
               {
                    if (Existing.Collection == Collection &&
                        NormalizeTerm(Existing.Query) == NormalizedQuery &&
                        Existing.Document.DocumentID == Document.DocumentID)
                    {
                         Existing.Query = Query;
                         Existing.Document.Title = Document.Title.empty() ? Existing.Document.Title : Document.Title;
                         Existing.Document.Score = std::max(Existing.Document.Score, Document.Score);
                         Existing.Document.InteractionUses += std::max<uint64_t>(1, Document.InteractionUses);
                         Existing.Document.LastInteractionMS = std::max(Existing.Document.LastInteractionMS,
                                                                        Document.LastInteractionMS);
                         Existing.Attempts = std::min<size_t>(Existing.Attempts + 1, 16);
                         return true;
                    }
               }

               PendingSearchInteractionJob Job;
               Job.Collection = Collection;
               Job.Query = Query;
               Job.Document = Document;
               Job.Document.InteractionUses = std::max<uint64_t>(1, Job.Document.InteractionUses);
               PendingSearchInteractionJobs.push_back(std::move(Job));

               constexpr size_t MaxPendingSearchInteractionJobs = 512;
               while (PendingSearchInteractionJobs.size() > MaxPendingSearchInteractionJobs)
               {
                    DroppedJobs.push_back(PendingSearchInteractionJobs.front());
                    PendingSearchInteractionJobs.pop_front();
                    ++DroppedPendingSearchInteractionJobs;
               }
          }

          for (const auto& DroppedJob : DroppedJobs)
          {
               RecordDebugEvent(DroppedJob.Collection,
                                "dropped pending search interaction for '" + NormalizeTerm(DroppedJob.Query) + "' because the queue limit was reached.");
          }
     }

     return true;
}

size_t SAM::FlushPendingSearchInteractions(size_t MaxJobs)
{
     if (MaxJobs == 0)
     {
          return 0;
     }

     size_t Flushed = 0;

     while (Flushed < MaxJobs)
     {
          PendingSearchInteractionJob Job;

          {
               std::lock_guard<std::mutex> Lock(SearchInteractionQueueMutex);
               if (PendingSearchInteractionJobs.empty())
               {
                    break;
               }

               Job = PendingSearchInteractionJobs.front();
               PendingSearchInteractionJobs.pop_front();
          }

          bool Recorded = false;
          bool DBBusy = false;

          {
               std::unique_lock<std::mutex> Lock(DBMutex, std::try_to_lock);
               if (!Lock.owns_lock())
               {
                    DBBusy = true;
               }
               else if (Database)
               {
                    Recorded = RecordSearchInteractionLocked(Job.Collection, Job.Query, Job.Document, nullptr);
               }
          }

          if (Recorded)
          {
               ++Flushed;
               continue;
          }

          if (DBBusy)
          {
               std::lock_guard<std::mutex> Lock(SearchInteractionQueueMutex);
               PendingSearchInteractionJobs.push_front(std::move(Job));
               break;
          }

          ++Job.Attempts;

          std::lock_guard<std::mutex> Lock(SearchInteractionQueueMutex);
          if (Job.Attempts < 16)
          {
               PendingSearchInteractionJobs.push_back(std::move(Job));
          }
          break;
     }

     return Flushed;
}

/* Persist one search interaction against an existing search idea. */

bool SAM::RecordSearchInteractionLocked(const std::string& Collection,
                                        const std::string& Query,
                                        const SearchIdeaDocumentRef& Document,
                                        std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!ShouldTrackSAMSearchIdeas(Collection))
     {
          return true;
     }

     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (Collection.empty() || NormalizedQuery.empty() || Document.DocumentID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A non-empty collection, query, and document ID are required.";
          }

          return false;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     SearchIdeaEntry Entry;
     std::string RawValue;
     const std::string IdeaKey = BuildSearchIdeaKey(Collection, NormalizedQuery);
     const rocksdb::Status ReadStatus =
          Database->Get(rocksdb::ReadOptions(), IdeaKey, &RawValue);

     if (ReadStatus.ok())
     {
          ParseSearchIdeaEntry(RawValue, Entry);
     }
     else if (!ReadStatus.IsNotFound() && ErrorMessage)
     {
          *ErrorMessage = ReadStatus.ToString();
          return false;
     }

     Entry.Collection = Collection;
     Entry.Query = TrimCopy(Query);
     Entry.NormalizedQuery = NormalizedQuery;
     Entry.FirstSeenMS = Entry.FirstSeenMS == 0 ? NowMS : Entry.FirstSeenMS;
     Entry.LastSeenMS = std::max(Entry.LastSeenMS, NowMS);
     Entry.Uses = std::max<uint64_t>(1, Entry.Uses);
     Entry.Vector = BuildHashedSemanticVector({Entry.NormalizedQuery});
     Entry.InteractionUses += std::max<uint64_t>(1, Document.InteractionUses);
     Entry.LastInteractionMS = NowMS;

     std::unordered_map<std::string, SearchIdeaDocumentRef> RankedDocuments;

     for (const auto& Existing : Entry.Documents)
     {
          if (!Existing.DocumentID.empty())
          {
               RankedDocuments[Existing.DocumentID] = Existing;
          }
     }

     SearchIdeaDocumentRef Candidate = Document;
     Candidate.InteractionUses = std::max<uint64_t>(1, Candidate.InteractionUses);
     Candidate.LastInteractionMS = Candidate.LastInteractionMS == 0 ? NowMS : Candidate.LastInteractionMS;
     Candidate.Score = std::max(Candidate.Score,
                                kSAMSearchIdeaInteractionBaseBoost +
                                     (static_cast<double>(std::min<uint64_t>(Candidate.InteractionUses, 4) - 1) *
                                      kSAMSearchIdeaInteractionStepBoost));

     auto ExistingIt = RankedDocuments.find(Candidate.DocumentID);

     if (ExistingIt == RankedDocuments.end())
     {
          RankedDocuments[Candidate.DocumentID] = Candidate;
     }
     else
     {
          ExistingIt->second.Title = Candidate.Title.empty() ? ExistingIt->second.Title : Candidate.Title;
          ExistingIt->second.Score = std::max(ExistingIt->second.Score, Candidate.Score);
          ExistingIt->second.InteractionUses += Candidate.InteractionUses;
          ExistingIt->second.LastInteractionMS = std::max(ExistingIt->second.LastInteractionMS,
                                                          Candidate.LastInteractionMS);
     }

     Entry.Documents.clear();
     Entry.Documents.reserve(RankedDocuments.size());

     for (const auto& Pair : RankedDocuments)
     {
          Entry.Documents.push_back(Pair.second);
     }

     std::sort(Entry.Documents.begin(), Entry.Documents.end(),
               [](const SearchIdeaDocumentRef& A, const SearchIdeaDocumentRef& B)
               {
                    if (A.InteractionUses != B.InteractionUses)
                    {
                         return A.InteractionUses > B.InteractionUses;
                    }

                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Entry.Documents.size() > kSAMSearchIdeaMaxDocs)
     {
          Entry.Documents.resize(kSAMSearchIdeaMaxDocs);
     }

     const rocksdb::Status WriteStatus =
          Database->Put(rocksdb::WriteOptions(), IdeaKey, SerializeSearchIdeaEntry(Entry).dump());

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     if (!TrimSearchIdeasLocked(Collection, ErrorMessage))
     {
          return false;
     }

     return true;
}

bool SAM::RecordSearchIdeaLocked(const std::string& Collection,
                                 const std::string& Query,
                                 const std::vector<SearchIdeaDocumentRef>& Documents,
                                 std::string* ErrorMessage)
{
     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     if (!ShouldTrackSAMSearchIdeas(Collection))
     {
          return true;
     }

     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (Collection.empty() || NormalizedQuery.empty() || NormalizedQuery == "*")
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A non-empty collection and query are required.";
          }

          return false;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     SearchIdeaEntry Entry;
     std::string RawValue;
     const std::string IdeaKey = BuildSearchIdeaKey(Collection, NormalizedQuery);
     const rocksdb::Status ReadStatus =
          Database->Get(rocksdb::ReadOptions(), IdeaKey, &RawValue);

     if (ReadStatus.ok())
     {
          ParseSearchIdeaEntry(RawValue, Entry);
     }
     else if (!ReadStatus.IsNotFound() && ErrorMessage)
     {
          *ErrorMessage = ReadStatus.ToString();
          return false;
     }

     Entry.Collection = Collection;
     Entry.Query = TrimCopy(Query);
     Entry.NormalizedQuery = NormalizedQuery;
     Entry.FirstSeenMS = Entry.FirstSeenMS == 0 ? NowMS : Entry.FirstSeenMS;
     Entry.LastSeenMS = NowMS;
     Entry.Uses = Entry.Uses == 0 ? 1 : (Entry.Uses + 1);
     Entry.Vector = BuildHashedSemanticVector({Entry.NormalizedQuery});

     std::unordered_map<std::string, SearchIdeaDocumentRef> RankedDocuments;

     for (const auto& Existing : Entry.Documents)
     {
          if (Existing.DocumentID.empty())
          {
               continue;
          }

          RankedDocuments[Existing.DocumentID] = Existing;
     }

     for (size_t Index = 0; Index < Documents.size(); ++Index)
     {
          const SearchIdeaDocumentRef& Document = Documents[Index];

          if (Document.DocumentID.empty())
          {
               continue;
          }

          SearchIdeaDocumentRef Candidate = Document;

          if (Candidate.Score <= 0.0)
          {
               Candidate.Score = std::max(0.05, 1.0 - (static_cast<double>(Index) * 0.12));
          }

          auto ExistingIt = RankedDocuments.find(Candidate.DocumentID);

          if (ExistingIt == RankedDocuments.end())
          {
               RankedDocuments[Candidate.DocumentID] = Candidate;
               continue;
          }

          ExistingIt->second.Score = std::max(ExistingIt->second.Score, Candidate.Score);

          if (ExistingIt->second.Title.empty() && !Candidate.Title.empty())
          {
               ExistingIt->second.Title = Candidate.Title;
          }
     }

     Entry.Documents.clear();
     Entry.Documents.reserve(RankedDocuments.size());

     for (const auto& Pair : RankedDocuments)
     {
          Entry.Documents.push_back(Pair.second);
     }

     std::sort(Entry.Documents.begin(), Entry.Documents.end(),
               [](const SearchIdeaDocumentRef& A, const SearchIdeaDocumentRef& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Title.size() != B.Title.size())
                    {
                         return A.Title.size() > B.Title.size();
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Entry.Documents.size() > kSAMSearchIdeaMaxDocs)
     {
          Entry.Documents.resize(kSAMSearchIdeaMaxDocs);
     }

     const rocksdb::Status WriteStatus =
          Database->Put(rocksdb::WriteOptions(), IdeaKey, SerializeSearchIdeaEntry(Entry).dump());

     if (!WriteStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return false;
     }

     if (!TrimSearchIdeasLocked(Collection, ErrorMessage))
     {
          return false;
     }

     return true;
}

bool SAM::TrimSearchIdeasLocked(const std::string& Collection, std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Search idea trimming requires an open database and collection.";
          }

          return false;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<std::pair<std::string, SearchIdeaEntry>> Entries;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          Entries.push_back({Iterator->key().ToString(), std::move(Entry)});
     }

     if (Entries.size() <= kSAMSearchIdeasMaxEntries)
     {
          return true;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     std::sort(Entries.begin(), Entries.end(),
               [NowMS](const auto& Left, const auto& Right)
               {
                    const double LeftFreshness = GetSAMIdeaFreshness(Left.second.LastSeenMS, NowMS);
                    const double RightFreshness = GetSAMIdeaFreshness(Right.second.LastSeenMS, NowMS);

                    if (Left.second.Uses != Right.second.Uses)
                    {
                         return Left.second.Uses < Right.second.Uses;
                    }

                    if (LeftFreshness != RightFreshness)
                    {
                         return LeftFreshness < RightFreshness;
                    }

                    if (Left.second.LastSeenMS != Right.second.LastSeenMS)
                    {
                         return Left.second.LastSeenMS < Right.second.LastSeenMS;
                    }

                    return Left.second.FirstSeenMS < Right.second.FirstSeenMS;
               });

     rocksdb::WriteBatch Batch;

     for (size_t Index = 0; Index + kSAMSearchIdeasMaxEntries < Entries.size(); ++Index)
     {
          Batch.Delete(Entries[Index].first);
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

double ComputeSearchIntentTokenOverlap(const std::vector<std::string>& LeftTokens,
                                       const std::vector<std::string>& RightTokens)
{
     if (LeftTokens.empty() || RightTokens.empty())
     {
          return 0.0;
     }

     std::unordered_set<std::string> RightSet(RightTokens.begin(), RightTokens.end());
     size_t Matched = 0;

     for (const auto& Token : LeftTokens)
     {
          if (RightSet.find(Token) != RightSet.end())
          {
               ++Matched;
          }
     }

     return ClampSAMScore(static_cast<double>(Matched) / static_cast<double>(LeftTokens.size()));
}

std::vector<std::string> BuildSearchIntentDocumentPhrases(const SAM::DocumentEntry& Entry)
{
     std::vector<std::string> Phrases;

     if (!Entry.Title.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Title));
     }

     if (!Entry.Subject.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Subject));
     }

     if (!Entry.Summary.empty())
     {
          Phrases.push_back(NormalizeTerm(Entry.Summary));
     }

     for (const auto& Alias : Entry.Aliases)
     {
          Phrases.push_back(NormalizeTerm(Alias));
     }

     for (const auto& Descriptor : Entry.Descriptors)
     {
          Phrases.push_back(NormalizeTerm(Descriptor));
     }

     for (const auto& Query : Entry.Queries)
     {
          Phrases.push_back(NormalizeTerm(Query));
     }

     for (const auto& Term : Entry.Terms)
     {
          Phrases.push_back(NormalizeTerm(Term.Text));

          if (Phrases.size() >= 32)
          {
               break;
          }
     }

     Phrases.erase(std::remove_if(Phrases.begin(), Phrases.end(),
                                  [](const std::string& Value)
                                  {
                                       return Value.empty();
                                  }),
                   Phrases.end());
     return Phrases;
}

double ScoreSearchIntentDocumentMatch(const SAM::DocumentEntry& Entry,
                                      const std::vector<llm::SearchIntentCandidate>& Candidates,
                                      const std::vector<llm::SearchIntentCandidate>& RankedTerms)
{
     const std::vector<std::string> Phrases = BuildSearchIntentDocumentPhrases(Entry);

     if (Phrases.empty())
     {
          return 0.0;
     }

     const std::string Title = NormalizeTerm(Entry.Title);
     const std::string Subject = NormalizeTerm(Entry.Subject);
     double BestScore = 0.0;

     const auto ScoreOne = [&](const llm::SearchIntentCandidate& Candidate,
                               double BaseWeight) -> double
     {
          const std::string Text = NormalizeTerm(Candidate.Text);

          if (Text.empty())
          {
               return 0.0;
          }

          const std::vector<std::string> CandidateTokens = TokenizeNormalized(Text);
          double Score = 0.0;

          if (!Title.empty() && Title == Text)
          {
               Score = std::max(Score, BaseWeight * 1.45);
          }

          if (!Subject.empty() && Subject == Text)
          {
               Score = std::max(Score, BaseWeight * 1.55);
          }

          for (const auto& Phrase : Phrases)
          {
               if (Phrase == Text)
               {
                    Score = std::max(Score, BaseWeight * 1.30);
                    continue;
               }

               if (Phrase.find(Text) != std::string::npos || Text.find(Phrase) != std::string::npos)
               {
                    Score = std::max(Score, BaseWeight * 1.08);
               }

               const double Overlap = ComputeSearchIntentTokenOverlap(CandidateTokens, TokenizeNormalized(Phrase));

               if (Overlap > 0.0)
               {
                    Score = std::max(Score, BaseWeight * (0.70 + (Overlap * 0.45)));
               }
          }

          return Score;
     };

     for (const auto& Candidate : Candidates)
     {
          BestScore = std::max(BestScore, ScoreOne(Candidate, 0.85 + ClampSAMScore(Candidate.Weight)));
     }

     for (const auto& RankedTerm : RankedTerms)
     {
          BestScore = std::max(BestScore, ScoreOne(RankedTerm, 0.60 + ClampSAMScore(RankedTerm.Weight)));
     }

     return BestScore;
}

Document BuildSearchIntentCandidateDocument(const SAM::DocumentEntry& Entry)
{
     Document Candidate;
     Candidate.ID = Entry.DocumentID;
     Candidate.Title = Entry.Title;
     Candidate.Content = Entry.Summary;

     if (!Entry.Subject.empty())
     {
          Candidate.Fields["subject"] = Entry.Subject;
     }

     if (!Entry.Aliases.empty())
     {
          Candidate.Fields["aliases"] = JoinValues(Entry.Aliases, ", ");
     }

     if (!Entry.Descriptors.empty())
     {
          Candidate.Fields["descriptors"] = JoinValues(Entry.Descriptors, ", ");
     }

     if (!Entry.Queries.empty())
     {
          Candidate.Fields["queries"] = JoinValues(Entry.Queries, ", ");
     }

     std::vector<std::string> RankedTerms;

     for (const auto& Term : Entry.Terms)
     {
          RankedTerms.push_back(Term.Text);

          if (RankedTerms.size() >= 10)
          {
               break;
          }
     }

     if (!RankedTerms.empty())
     {
          Candidate.Fields["terms"] = JoinValues(RankedTerms, ", ");
     }

     return Candidate;
}

/* Optimize stored search ideas into intent-level support data. */

bool SAM::OptimizeSearchIdeaIntentLocked(const std::string& Collection,
                                         const std::string& NormalizedQuery,
                                         bool* Updated,
                                         std::string* ErrorMessage)
{
     if (Updated)
     {
          *Updated = false;
     }

     if (Collection.empty() || NormalizedQuery.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection and normalized query are required.";
          }

          return false;
     }

     if (!Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          return true;
     }

     SearchIdeaEntry Entry;
     std::vector<Document> CandidateDocuments;

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

          std::string RawValue;
          const rocksdb::Status ReadStatus =
               Database->Get(rocksdb::ReadOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             &RawValue);

          if (!ReadStatus.ok())
          {
               if (!ReadStatus.IsNotFound() && ErrorMessage)
               {
                    *ErrorMessage = ReadStatus.ToString();
               }

               return ReadStatus.IsNotFound();
          }

          if (!ParseSearchIdeaEntry(RawValue, Entry))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Failed to parse stored search idea.";
               }

               return false;
          }

          for (const auto& DocumentRef : Entry.Documents)
          {
               if (DocumentRef.DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status ManifestStatus =
                    Database->Get(rocksdb::ReadOptions(),
                                  BuildDocManifestKey(Collection, DocumentRef.DocumentID),
                                  &ManifestValue);

               if (!ManifestStatus.ok())
               {
                    continue;
               }

               SAM::DocumentEntry ManifestEntry;

               if (!ParseManifestValue(ManifestValue, ManifestEntry) || !IsSAMDocumentEntryCurrent(ManifestEntry))
               {
                    continue;
               }

               CandidateDocuments.push_back(BuildSearchIntentCandidateDocument(ManifestEntry));
          }
     }

     if (CandidateDocuments.empty())
     {
          return true;
     }

     const size_t IntentLimit = Instance && Instance->Config
          ? static_cast<size_t>(std::max(1, Instance->Config->GetSamLLMMaxIdeas()))
          : 6;
     const llm::SearchIntentResolution Resolution =
          Instance->LLM->ResolveSearchIntent(Collection, Entry.Query, CandidateDocuments, IntentLimit);

     if (Resolution.Candidates.empty() && Resolution.RankedTerms.empty() &&
         Resolution.Interpretation.empty() && Resolution.Conclusion.empty())
     {
          return true;
     }

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

          std::string RawValue;
          const rocksdb::Status ReadStatus =
               Database->Get(rocksdb::ReadOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             &RawValue);

          if (!ReadStatus.ok())
          {
               if (!ReadStatus.IsNotFound() && ErrorMessage)
               {
                    *ErrorMessage = ReadStatus.ToString();
               }

               return ReadStatus.IsNotFound();
          }

          if (!ParseSearchIdeaEntry(RawValue, Entry))
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = "Failed to parse stored search idea.";
               }

               return false;
          }

          Entry.ResolvedInterpretation = Resolution.Interpretation;
          Entry.ResolvedConclusion = Resolution.Conclusion;
          Entry.ResolvedAtMS = GetSAMCurrentTimeMS();
          Entry.ResolvedUses = Entry.Uses;
          Entry.ResolvedCandidates = Resolution.Candidates;
          Entry.ResolvedRankedTerms = Resolution.RankedTerms;

          std::unordered_map<std::string, SearchIdeaDocumentRef> RankedDocuments;

          for (const auto& Existing : Entry.Documents)
          {
               if (!Existing.DocumentID.empty())
               {
                    RankedDocuments[Existing.DocumentID] = Existing;
               }
          }

          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
          const double MinIntentDocMatchScore = Instance && Instance->Config
               ? Instance->Config->GetSam25IntentDocMatchMinScore()
               : 0.65;

          for (Iterator->Seek(ManifestPrefix);
               Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix);
               Iterator->Next())
          {
               SAM::DocumentEntry ManifestEntry;

               if (!ParseManifestValue(Iterator->value().ToString(), ManifestEntry) ||
                   !IsSAMDocumentEntryCurrent(ManifestEntry))
               {
                    continue;
               }

               const double MatchScore =
                    ScoreSearchIntentDocumentMatch(ManifestEntry,
                                                  Entry.ResolvedCandidates,
                                                  Entry.ResolvedRankedTerms);

               if (MatchScore < MinIntentDocMatchScore)
               {
                    continue;
               }

               SearchIdeaDocumentRef& RankedDocument = RankedDocuments[ManifestEntry.DocumentID];
               RankedDocument.DocumentID = ManifestEntry.DocumentID;
               RankedDocument.Title = ManifestEntry.Title;
               RankedDocument.Score = std::max(RankedDocument.Score, MatchScore);
          }

          Entry.Documents.clear();

          for (const auto& Pair : RankedDocuments)
          {
               Entry.Documents.push_back(Pair.second);
          }

          std::sort(Entry.Documents.begin(), Entry.Documents.end(),
                    [](const SearchIdeaDocumentRef& Left, const SearchIdeaDocumentRef& Right)
                    {
                         if (Left.Score != Right.Score)
                         {
                              return Left.Score > Right.Score;
                         }

                         return Left.Title < Right.Title;
                    });

          if (Entry.Documents.size() > kSAMSearchIdeaMaxDocs)
          {
               Entry.Documents.resize(kSAMSearchIdeaMaxDocs);
          }

          const rocksdb::Status WriteStatus =
               Database->Put(rocksdb::WriteOptions(),
                             BuildSearchIdeaKey(Collection, NormalizedQuery),
                             SerializeSearchIdeaEntry(Entry).dump());

          if (!WriteStatus.ok())
          {
               if (ErrorMessage)
               {
                    *ErrorMessage = WriteStatus.ToString();
               }

               return false;
          }
     }

     if (Updated)
     {
          *Updated = true;
     }

     return true;
}

/* Rebuild one collection profile from accumulated search idea evidence. */

bool SAM::RefreshCollectionProfileFromSearchIdeas(const std::string& Collection,
                                                  bool* Updated,
                                                  std::string* ErrorMessage)
{
     if (Updated)
     {
          *Updated = false;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     std::lock_guard<std::mutex> Lock(DBMutex);

     if (!Database)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     size_t RecentIdeaCount = 0;
     const uint64_t LatestRecentIdeaSeenMS =
          GetLatestRecentSearchIdeaTimestampLocked(Database.get(), Collection, &RecentIdeaCount);

     if (LatestRecentIdeaSeenMS == 0 || RecentIdeaCount == 0)
     {
          return true;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (Status.ok() && !RawProfile.empty())
     {
          try
          {
               const nlohmann::json Root = nlohmann::json::parse(RawProfile);
               const uint64_t LastIdeaSyncMarker = Root.value("idea_sync_marker_ms", static_cast<uint64_t>(0));
               const uint64_t LastIdeaProfileSyncMS = Root.value("idea_profile_synced_at_ms", static_cast<uint64_t>(0));
               const size_t LastIdeaRecentCount =
                    static_cast<size_t>(Root.value("idea_recent_count", static_cast<uint64_t>(0)));

               if (LastIdeaSyncMarker >= LatestRecentIdeaSeenMS)
               {
                    return true;
               }

               const uint64_t NowMS = GetSAMCurrentTimeMS();

               if (LastIdeaProfileSyncMS > 0 &&
                   NowMS > LastIdeaProfileSyncMS &&
                   (NowMS - LastIdeaProfileSyncMS) < kSAMSearchIdeaProfileSyncCooldownMs)
               {
                    const size_t IdeaCountDelta = RecentIdeaCount > LastIdeaRecentCount
                         ? (RecentIdeaCount - LastIdeaRecentCount)
                         : 0;

                    if (IdeaCountDelta < kSAMSearchIdeaProfileForceSyncMinDelta)
                    {
                         return true;
                    }
               }
          }
          catch (...)
          {
          }
     }

     if (!RebuildCollectionProfileLocked(Database.get(), Collection, ErrorMessage))
     {
          return false;
     }

     if (Updated)
     {
          *Updated = true;
     }

     return true;
}

size_t SAM::ProcessPendingSearchIntentOptimizations(size_t MaxCollections)
{
     FlushPendingSearchInteractions(8);
     FlushPendingSearchIdeas(2);

     if (FlushInProgress.load(std::memory_order_acquire))
     {
          return 0;
     }

     if (MaxCollections == 0 || !Instance || !Instance->LLM || !Instance->LLM->Configured())
     {
          return 0;
     }

     std::vector<std::pair<std::string, std::string>> PendingIdeas;
     std::vector<SearchIdeaEntry> RankedIdeas;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (!Database)
          {
               return 0;
          }

          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
          std::unordered_set<std::string> SeenCollections;

          for (Iterator->Seek("sam:idea:");
               Iterator->Valid() && Iterator->key().starts_with("sam:idea:");
               Iterator->Next())
          {
               SearchIdeaEntry Entry;

               if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
               {
                    continue;
               }

               if (Entry.Collection.empty() || Entry.NormalizedQuery.empty() || Entry.Documents.empty())
               {
                    continue;
               }

               if (Entry.ResolvedUses >= Entry.Uses && Entry.ResolvedAtMS >= Entry.LastSeenMS &&
                   !Entry.ResolvedCandidates.empty())
               {
                    continue;
               }

               if (!SeenCollections.insert(Entry.Collection).second)
               {
                    continue;
               }

               RankedIdeas.push_back(std::move(Entry));
          }
     }

     std::sort(RankedIdeas.begin(), RankedIdeas.end(),
               [](const SearchIdeaEntry& Left, const SearchIdeaEntry& Right)
               {
                    if (Left.LastSeenMS != Right.LastSeenMS)
                    {
                         return Left.LastSeenMS > Right.LastSeenMS;
                    }

                    if (Left.Uses != Right.Uses)
                    {
                         return Left.Uses > Right.Uses;
                    }

                    return Left.Query < Right.Query;
               });

     for (const auto& Entry : RankedIdeas)
     {
          PendingIdeas.push_back({Entry.Collection, Entry.NormalizedQuery});

          if (PendingIdeas.size() >= MaxCollections)
          {
               break;
          }
     }

     size_t Processed = 0;

     for (const auto& PendingIdea : PendingIdeas)
     {
          {
               std::lock_guard<std::mutex> QueueLock(QueueMutex);
               const bool HasQueuedIndexWork =
                    std::any_of(PendingIndexJobs.begin(),
                                PendingIndexJobs.end(),
                                [&](const PendingIndexJob& Job)
                                {
                                     return Job.Collection == PendingIdea.first;
                                });

               if (HasQueuedIndexWork)
               {
                    continue;
               }
          }

          {
               std::lock_guard<std::mutex> JobLock(JobMutex);
               const auto ActiveIt = ActiveCollectionTasks.find(PendingIdea.first);
               const auto StatusIt = CollectionJobs.find(PendingIdea.first);

               if (IsCollectionCancelledLocked(PendingIdea.first) ||
                   ActiveIt != ActiveCollectionTasks.end() ||
                   (StatusIt != CollectionJobs.end() && StatusIt->second.Running))
               {
                    continue;
               }

               ++ActiveCollectionTasks[PendingIdea.first];
          }

          bool Updated = false;
          std::string ErrorMessage;

          if (!OptimizeSearchIdeaIntentLocked(PendingIdea.first, PendingIdea.second, &Updated, &ErrorMessage))
          {
               FinishBackgroundImprovement(PendingIdea.first);

               if (Instance->Logs && !ErrorMessage.empty())
               {
                    Instance->Logs->Normal("sam", "Failed to optimize SAM search intent for collection '" + PendingIdea.first + "' and query '" + PendingIdea.second + "': " + ErrorMessage + ".");
               }

               continue;
          }

          if (!Updated)
          {
               FinishBackgroundImprovement(PendingIdea.first);
               continue;
          }

          bool ProfileUpdated = false;
          ErrorMessage.clear();
          RefreshCollectionProfileFromSearchIdeas(PendingIdea.first, &ProfileUpdated, &ErrorMessage);

          if (Instance->Logs)
          {
               if (!ErrorMessage.empty())
               {
                    Instance->Logs->Normal("sam", "Optimized SAM search intent for collection '" + PendingIdea.first + "' but failed to refresh its profile: " + ErrorMessage + ".");
               }
               else
               {
                    Instance->Logs->Debug("sam", "Optimized SAM search intent for collection '" + PendingIdea.first + "' and refreshed learned terms.");
               }
          }

          ++Processed;
          FinishBackgroundImprovement(PendingIdea.first);
     }

     return Processed;
}

size_t SAM::FlushPendingInteractionSignals(size_t MaxJobs)
{
     return FlushPendingSearchInteractions(MaxJobs);
}

/* Cancel queued and running SAM work for one collection. */

bool SAM::CancelCollectionWork(const std::string& Collection, std::string* ErrorMessage)
{
     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name is required.";
          }

          return false;
     }

     {
          std::lock_guard<std::mutex> QueueLock(QueueMutex);
          PendingIndexJobs.erase(
               std::remove_if(PendingIndexJobs.begin(), PendingIndexJobs.end(),
                              [&](const PendingIndexJob& Job)
                              {
                                   return Job.Collection == Collection;
                              }),
               PendingIndexJobs.end());

          for (auto It = PendingIndexKeys.begin(); It != PendingIndexKeys.end(); )
          {
               const std::string Prefix = Collection + std::string(1, '\0');
               if (It->rfind(Prefix, 0) == 0)
               {
                    It = PendingIndexKeys.erase(It);
               }
               else
               {
                    ++It;
               }
          }
     }

     {
          std::lock_guard<std::mutex> DBLock(DBMutex);
          if (Database)
          {
               const std::string QueuePrefix =
                    std::string("sam:queue:index:") + Collection + std::string(1, '\0');
               std::vector<std::string> QueueKeysToDelete;
               std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

               for (Iterator->Seek(QueuePrefix);
                    Iterator->Valid() && Iterator->key().starts_with(QueuePrefix);
                    Iterator->Next())
               {
                    QueueKeysToDelete.push_back(Iterator->key().ToString());
               }

               if (!QueueKeysToDelete.empty())
               {
                    rocksdb::WriteBatch Batch;

                    for (const auto& Key : QueueKeysToDelete)
                    {
                         Batch.Delete(Key);
                    }

                    (void)Database->Write(rocksdb::WriteOptions(), &Batch);
               }
          }
     }

     QueueCV.notify_all();

     std::unique_lock<std::mutex> Lock(JobMutex);
     CancelledCollections.insert(Collection);
     CollectionJobStatus& Status = CollectionJobs[Collection];
     Status.Running = false;
     Status.Completed = false;
     Status.PendingDocuments = 0;
     if (Status.ErrorMessage.empty())
     {
          Status.ErrorMessage = "Cancelled.";
     }

     JobStateCV.wait(Lock, [&]()
     {
          return ActiveCollectionTasks.find(Collection) == ActiveCollectionTasks.end();
     });

     CancelledCollections.erase(Collection);
     return true;
}

bool SAM::CancelAllWork(std::string* ErrorMessage)
{
     (void)ErrorMessage;

     (void)ClearQueuedAutoIndexJobs();
     QueueCV.notify_all();

     std::unique_lock<std::mutex> Lock(JobMutex);
     CancelAllRequested = true;

     for (auto& Entry : CollectionJobs)
     {
          Entry.second.Running = false;
          Entry.second.Completed = false;
          Entry.second.PendingDocuments = 0;
          if (Entry.second.ErrorMessage.empty())
          {
               Entry.second.ErrorMessage = "Cancelled.";
          }
     }

     JobStateCV.wait(Lock, [&]()
     {
          return ActiveCollectionTasks.empty();
     });

     CancelledCollections.clear();
     CancelAllRequested = false;
     return true;
}

/* Start tracking one visible lookup activity. */

uint64_t SAM::BeginLookupActivity(const std::string& Collection,
                                  const std::string& Query) const
{
     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (NormalizedQuery.empty())
     {
          return 0;
     }

     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     SearchActivityEntry Entry;
     Entry.Sequence = NextSearchActivitySequence++;
     Entry.Collection = Collection;
     Entry.Query = TrimCopy(Query);
     Entry.NormalizedQuery = NormalizedQuery;
     Entry.StartedMS = GetSAMCurrentTimeMS();
     Entry.Running = true;
     ActiveSearchActivities[Entry.Sequence] = Entry;
     return Entry.Sequence;
}

/* Finish a visible lookup activity and store the latest snapshot. */

void SAM::FinishLookupActivity(uint64_t Sequence,
                               size_t ResultCount) const
{
     if (Sequence == 0)
     {
          return;
     }

     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     const auto It = ActiveSearchActivities.find(Sequence);

     if (It == ActiveSearchActivities.end())
     {
          return;
     }

     SearchActivityEntry Entry = It->second;
     Entry.CompletedMS = GetSAMCurrentTimeMS();
     Entry.ResultCount = ResultCount;
     Entry.Running = false;
     LatestSearchActivityByCollection["*"] = Entry;

     if (!Entry.Collection.empty())
     {
          LatestSearchActivityByCollection[Entry.Collection] = Entry;
     }

     ActiveSearchActivities.erase(It);
}

static std::vector<SAM::LookupHit> LookupDirectTermOnly(std::shared_ptr<rocksdb::DB> DatabaseHandle,
                                                        const std::string& Collection,
                                                        const std::string& Query,
                                                        const SAMQueryTokenViews& QueryViews,
                                                        size_t Limit)
{
     std::vector<SAM::LookupHit> Hits;
     if (!DatabaseHandle || Collection.empty() || Query.empty() || Limit == 0)
     {
          return Hits;
     }

     std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;
     std::unordered_map<std::string, bool> FreshnessCache;

     const std::string Normalized = NormalizeTerm(Query);
     if (Normalized.empty())
     {
          return Hits;
     }

     const std::string Prefix = "sam:term:" + Normalized + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          try
          {
               nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
               SAM::LookupHit Hit;
               Hit.Collection = Payload.value("collection", "");
               if (Hit.Collection != Collection)
               {
                    continue;
               }
               Hit.DocumentID = Payload.value("id", "");
               Hit.Title = Payload.value("title", "");
               Hit.MatchedTerm = Payload.value("term", "");
               Hit.MatchedKind = Payload.value("kind", "");
               Hit.MatchedSource = Payload.value("source", "");
               Hit.TermOrigin = Hit.MatchedSource;
               Hit.MatchedPath = "sam_term_degraded";
               Hit.MatchedScore = Payload.value("score", 0.0);
               Hit.MatchedSignal = Payload.value("signal", 0.0);
               Hit.Breakdown.TermScore = Hit.MatchedScore;
               Hit.Breakdown.FinalScore = Hit.MatchedScore;

               if (!Hit.DocumentID.empty())
               {
                    std::string ManifestValue;
                    const rocksdb::Status ManifestStatus =
                         DatabaseHandle->Get(rocksdb::ReadOptions(),
                                             BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                                             &ManifestValue);

                    if (!ManifestStatus.ok())
                    {
                         continue;
                    }

                    SAM::DocumentEntry Entry;

                    if (!ParseManifestValue(ManifestValue, Entry) ||
                        !IsSAMDocumentEntryCurrent(Entry, &FreshnessCache))
                    {
                         continue;
                    }

                    Hit.Title = Entry.Title.empty() ? Hit.Title : Entry.Title;
                    AccumulateSAMHit(AggregatedHits, Hit);
               }
          }
          catch (...)
          {
          }
     }

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, DatabaseHandle.get(), Limit);
     return Hits;
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;
     const uint64_t ActivitySequence = BeginLookupActivity("", Query);
     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(DatabaseHandle.get(), "", Query);

     std::vector<std::string> Variants = BuildQueryVariants(DatabaseHandle.get(), "", Query);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(DatabaseHandle.get(), "", Query, QueryViews);

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

    std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;
    std::unordered_map<std::string, bool> FreshnessCache;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

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
                    Hit.MatchedKind = Payload.value("kind", "");
                    Hit.MatchedSource = Payload.value("source", "");
                    Hit.TermOrigin = Hit.MatchedSource;
                    Hit.MatchedPath = "sam_term";
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    Hit.Breakdown.TermScore = Hit.MatchedScore;
                    Hit.Breakdown.FinalScore = Hit.MatchedScore;
                    if (IsSAM25DebugExplainEnabled())
                    {
                         std::ostringstream Stream;
                         Stream << "sam25+ direct score=" << Hit.MatchedScore
                                << " signal=" << Hit.MatchedSignal
                                << " source=" << Hit.MatchedSource;
                         Hit.Explain = Stream.str();
                    }

                    if (!Hit.Collection.empty() && !Hit.DocumentID.empty())
                    {
                         std::string ManifestValue;
                         const rocksdb::Status ManifestStatus =
                              DatabaseHandle->Get(rocksdb::ReadOptions(),
                                                  BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                                                  &ManifestValue);

                         if (!ManifestStatus.ok())
                         {
                              continue;
                         }

                         DocumentEntry Entry;

                         if (!ParseManifestValue(ManifestValue, Entry) ||
                             !IsSAMDocumentEntryCurrent(Entry, &FreshnessCache))
                         {
                              continue;
                         }

                         Hit.Title = Entry.Title.empty() ? Hit.Title : Entry.Title;
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     AppendFuzzyFallbackHits(AggregatedHits, DatabaseHandle.get(), "", Query, Limit);
     AppendSAMLikePatternHits(AggregatedHits, DatabaseHandle.get(), "", Query);
     AppendSemanticProfileHits(AggregatedHits, DatabaseHandle.get(), "", SemanticPlan,
                               std::max<size_t>(256, Limit * 24));

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, DatabaseHandle.get(), Limit);
     CaptureLookupEvaluation(DatabaseHandle.get(), "__global__", Query, Hits, nullptr);

     EmitSAM25DebugLog(Query, Hits);
     FinishLookupActivity(ActivitySequence, Hits.size());
     return Hits;
}

static bool TryParseSAMRankDouble(const std::string& Value, double* Out)
{
     if (!Out)
     {
          return false;
     }

     const std::string Trimmed = TrimCopy(Value);

     if (Trimmed.empty())
     {
          return false;
     }

     char* End = nullptr;
     const double Parsed = std::strtod(Trimmed.c_str(), &End);

     if (End == Trimmed.c_str() || *End != '\0' || !std::isfinite(Parsed))
     {
          return false;
     }

     *Out = Parsed;
     return true;
}

static double GetSAMHitRankingScore(const SAM::LookupHit& Hit)
{
     if (Hit.Breakdown.FinalScore > 0.0)
     {
          return Hit.Breakdown.FinalScore;
     }

     return Hit.MatchedScore;
}

static bool ApplySAMCollectionRankPrior(const std::string& Collection,
                                        std::vector<SAM::LookupHit>& Hits)
{
     if (Collection.empty() || Hits.empty())
     {
          return false;
     }

     CollectionConfig Config;
     if (!HybridStorageManagerInstance().GetCollectionConfig(Collection, Config))
     {
          return false;
     }

     auto MetadataValue = [&](const std::string& Key) -> std::string
     {
          auto It = Config.Metadata.find(Key);
          return It == Config.Metadata.end() ? "" : TrimCopy(It->second);
     };

     const std::string RankField = MetadataValue("_rank_field");
     if (RankField.empty())
     {
          return false;
     }

     double RankWeight = 0.25;
     const std::string RankWeightRaw = MetadataValue("_rank_weight");
     if (!RankWeightRaw.empty() &&
         (!TryParseSAMRankDouble(RankWeightRaw, &RankWeight) || RankWeight <= 0.0 || !std::isfinite(RankWeight)))
     {
          return false;
     }

     RankWeight = std::min(RankWeight, 10.0);

     const std::string RankOrder = ToLowerCopy(MetadataValue("_rank_order"));
     const bool Descending = (RankOrder == "desc" || RankOrder == "descending" ||
                              RankOrder == "higher" || RankOrder == "higher_is_better");
     const std::string RankAlgorithm = ToLowerCopy(MetadataValue("_rank_algorithm"));

     struct ParsedRank
     {
          size_t Index = 0;
          double Value = 0.0;
          double Signal = 1.0;
     };

     std::vector<ParsedRank> ParsedRanks;
     ParsedRanks.reserve(Hits.size());

     double MinRank = std::numeric_limits<double>::max();
     double MaxRank = std::numeric_limits<double>::lowest();

     for (size_t Index = 0; Index < Hits.size(); ++Index)
     {
          const SAM::LookupHit& Hit = Hits[Index];

          if (Hit.Collection != Collection || Hit.DocumentID.empty())
          {
               continue;
          }

          const Document Doc = HybridStorageManagerInstance().GetDocument(Collection, Hit.DocumentID);
          auto FieldIt = Doc.Fields.find(RankField);

          if (FieldIt == Doc.Fields.end())
          {
               continue;
          }

          double RankValue = 0.0;
          if (!TryParseSAMRankDouble(FieldIt->second, &RankValue))
          {
               continue;
          }

          ParsedRanks.push_back({Index, RankValue, 1.0});
          MinRank = std::min(MinRank, RankValue);
          MaxRank = std::max(MaxRank, RankValue);
     }

     if (ParsedRanks.empty())
     {
          return false;
     }

     const double Range = MaxRank - MinRank;
     for (ParsedRank& Parsed : ParsedRanks)
     {
          double Signal = 1.0;

          if (Range > 0.0)
          {
               Signal = Descending
                    ? ((Parsed.Value - MinRank) / Range)
                    : ((MaxRank - Parsed.Value) / Range);
          }

          Parsed.Signal = std::clamp(Signal, 0.0, 1.0);
     }

     std::vector<double> Multipliers(ParsedRanks.size(), 1.0);

     if (RankAlgorithm == "spectral" && ParsedRanks.size() >= 2)
     {
          double Alpha = 0.85;
          const std::string AlphaRaw = MetadataValue("_rank_alpha");
          if (!AlphaRaw.empty())
          {
               double ParsedAlpha = 0.0;
               if (TryParseSAMRankDouble(AlphaRaw, &ParsedAlpha) && ParsedAlpha > 0.0 && ParsedAlpha < 1.0)
               {
                    Alpha = ParsedAlpha;
               }
          }

          double Beta = 4.0;
          const std::string BetaRaw = MetadataValue("_rank_beta");
          if (!BetaRaw.empty())
          {
               double ParsedBeta = 0.0;
               if (TryParseSAMRankDouble(BetaRaw, &ParsedBeta) && ParsedBeta > 0.0 && std::isfinite(ParsedBeta))
               {
                    Beta = std::min(ParsedBeta, 20.0);
               }
          }

          const size_t Count = ParsedRanks.size();
          std::vector<double> BaseSignals(Count, 0.0);
          double BaseMin = std::numeric_limits<double>::max();
          double BaseMax = std::numeric_limits<double>::lowest();

          for (size_t I = 0; I < Count; ++I)
          {
               const double BaseScore = GetSAMHitRankingScore(Hits[ParsedRanks[I].Index]);
               BaseSignals[I] = std::isfinite(BaseScore) && BaseScore > 0.0 ? BaseScore : 0.0;
               BaseMin = std::min(BaseMin, BaseSignals[I]);
               BaseMax = std::max(BaseMax, BaseSignals[I]);
          }

          const double BaseRange = BaseMax - BaseMin;
          std::vector<double> Personalization(Count, 0.0);
          double PersonalizationSum = 0.0;

          for (size_t I = 0; I < Count; ++I)
          {
               const double RelevanceSignal = BaseRange > 0.0 ? ((BaseSignals[I] - BaseMin) / BaseRange) : 1.0;
               Personalization[I] = std::max(1e-9, (0.70 * RelevanceSignal) + (0.30 * ParsedRanks[I].Signal));
               PersonalizationSum += Personalization[I];
          }

          for (double& Value : Personalization)
          {
               Value /= PersonalizationSum;
          }

          std::vector<double> State = Personalization;
          std::vector<double> Next(Count, 0.0);

          for (int Iteration = 0; Iteration < 32; ++Iteration)
          {
               std::fill(Next.begin(), Next.end(), 0.0);

               for (size_t From = 0; From < Count; ++From)
               {
                    double Denominator = 0.0;

                    for (size_t To = 0; To < Count; ++To)
                    {
                         Denominator += std::exp(Beta * (ParsedRanks[To].Signal - ParsedRanks[From].Signal));
                    }

                    if (Denominator <= 0.0 || !std::isfinite(Denominator))
                    {
                         continue;
                    }

                    for (size_t To = 0; To < Count; ++To)
                    {
                         const double Transition =
                              std::exp(Beta * (ParsedRanks[To].Signal - ParsedRanks[From].Signal)) / Denominator;
                         Next[To] += Alpha * Transition * State[From];
                    }
               }

               double Delta = 0.0;
               for (size_t I = 0; I < Count; ++I)
               {
                    Next[I] += (1.0 - Alpha) * Personalization[I];
                    Delta += std::abs(Next[I] - State[I]);
               }

               State.swap(Next);
               if (Delta < 1e-8)
               {
                    break;
               }
          }

          const double MeanState = 1.0 / static_cast<double>(Count);
          for (size_t I = 0; I < Count; ++I)
          {
               const double RelativeInfluence = State[I] / MeanState;
               Multipliers[I] = std::clamp(1.0 + (RankWeight * (RelativeInfluence - 1.0)),
                                           0.05,
                                           1.0 + (RankWeight * 4.0));
          }
     }
     else
     {
          for (size_t I = 0; I < ParsedRanks.size(); ++I)
          {
               Multipliers[I] = 1.0 + (RankWeight * ParsedRanks[I].Signal);
          }
     }

     for (size_t I = 0; I < ParsedRanks.size(); ++I)
     {
          const double Multiplier = Multipliers[I];

          if (!std::isfinite(Multiplier) || Multiplier <= 0.0)
          {
               continue;
          }

          SAM::LookupHit& Hit = Hits[ParsedRanks[I].Index];
          const double Before = GetSAMHitRankingScore(Hit);
          const double After = Before * Multiplier;

          Hit.MatchedScore = std::max(0.0, Hit.MatchedScore * Multiplier);
          Hit.Breakdown.RankPriorMultiplier = Multiplier;
          Hit.Breakdown.RankPriorScore = After - Before;
          Hit.Breakdown.FinalScore = std::max(0.0, After);

          if (IsSAM25DebugExplainEnabled())
          {
               std::ostringstream Stream;
               Stream << " rank_prior_field=" << RankField
                      << " rank_prior_algorithm=" << (RankAlgorithm.empty() ? "linear" : RankAlgorithm)
                      << " rank_prior_multiplier=" << Multiplier;

               if (!Hit.Explain.empty())
               {
                    Hit.Explain += " ";
               }

               Hit.Explain += Stream.str();
          }
     }

     std::sort(Hits.begin(), Hits.end(),
               [](const SAM::LookupHit& A, const SAM::LookupHit& B)
               {
                    const double AScore = GetSAMHitRankingScore(A);
                    const double BScore = GetSAMHitRankingScore(B);

                    if (AScore != BScore)
                    {
                         return AScore > BScore;
                    }

                    if (A.MatchedSignal != B.MatchedSignal)
                    {
                         return A.MatchedSignal > B.MatchedSignal;
                    }

                    if (A.Collection != B.Collection)
                    {
                         return A.Collection < B.Collection;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     return true;
}

std::vector<SAM::LookupHit> SAM::Lookup(const std::string& Collection, const std::string& Query, size_t Limit) const
{
     std::vector<LookupHit> Hits;

     if (Collection.empty())
     {
          return Lookup(Query, Limit);
     }

     const uint64_t ActivitySequence = BeginLookupActivity(Collection, Query);
     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(DatabaseHandle.get(), Collection, Query);

     SAM::CollectionJobStatus Status;
     if (GetCollectionJobStatus(Collection, Status) && Status.Running)
     {
          Hits = LookupDirectTermOnly(DatabaseHandle, Collection, Query, QueryViews, Limit);
          FinishLookupActivity(ActivitySequence, Hits.size());
          return Hits;
     }

     SAMEvaluationCalibration Calibration;
     LoadLookupEvaluationCalibration(DatabaseHandle.get(), Collection, Calibration, nullptr);

     const SAMCollectionProfileHints ProfileHints = LoadCollectionProfileHints(DatabaseHandle.get(), Collection);
     std::vector<std::string> Variants = BuildQueryVariants(DatabaseHandle.get(), Collection, Query, &ProfileHints.StrongTokens);
     const SAMSemanticQuery SemanticPlan = BuildSemanticQueryPlan(DatabaseHandle.get(), Collection, Query, QueryViews);
     const std::vector<std::string> PersistedProfileVariants =
          BuildPersistedCollectionProfileVariants(DatabaseHandle.get(), Collection, QueryViews,
                                                 &ProfileHints.StrongTokens,
                                                 std::max<size_t>(8, std::min<size_t>(Limit * 3 + Calibration.AdaptiveVariantBudget, 24)));
     const std::vector<std::string> LearnedVariants =
          BuildCollectionLearnedVariants(DatabaseHandle.get(), Collection, Query, QueryViews,
                                         &ProfileHints.StrongTokens,
                                         std::max<size_t>(8, std::min<size_t>(Limit * 3 + Calibration.AdaptiveVariantBudget, 24)));
     const std::vector<SAMMatchedSearchIdea> SearchIdeas =
          BuildMatchedSearchIdeas(DatabaseHandle.get(), Collection, Query, QueryViews,
                                  std::max<size_t>(6, std::min<size_t>(Limit * 2, 10)));
     const std::vector<std::string> SearchIdeaVariants =
          BuildSearchIdeaVariants(SearchIdeas, QueryViews, &ProfileHints.StrongTokens,
                                  std::max<size_t>(6, std::min<size_t>(Limit * 2 + Calibration.AdaptiveVariantBudget, 18)));
     const std::vector<std::string> GraphVariants =
          BuildIntentGraphVariants(DatabaseHandle.get(), Collection, Query,
                                   std::max<size_t>(4, std::min<size_t>(Limit * 2 + Calibration.AdaptiveGraphBudget, 16)));

     for (const auto& Candidate : PersistedProfileVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : LearnedVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : SearchIdeaVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : GraphVariants)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     for (const auto& Candidate : SemanticPlan.Rewrites)
     {
          if (std::find(Variants.begin(), Variants.end(), Candidate) == Variants.end())
          {
               Variants.push_back(Candidate);
          }
     }

     if (Variants.empty())
     {
          FinishLookupActivity(ActivitySequence, 0);
          return Hits;
     }

     std::unordered_map<std::string, SAMAggregatedHit> AggregatedHits;
     std::unordered_map<std::string, bool> FreshnessCache;

     for (const auto& Variant : Variants)
     {
         const std::string Prefix = "sam:term:" + Variant + ":" + Collection + ":";
         std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
          {
               try
               {
                    const nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
                    LookupHit Hit;
                    Hit.Collection = Payload.value("collection", "");
                    Hit.DocumentID = Payload.value("id", "");
                    Hit.Title = Payload.value("title", "");
                    Hit.MatchedTerm = Payload.value("term", "");
                    Hit.MatchedKind = Payload.value("kind", "");
                    Hit.MatchedSource = Payload.value("source", "");
                    Hit.TermOrigin = Hit.MatchedSource;
                    Hit.MatchedPath = "sam_term";
                    Hit.MatchedScore = Payload.value("score", 0.0);
                    Hit.MatchedSignal = Payload.value("signal", 0.0);
                    Hit.Breakdown.TermScore = Hit.MatchedScore;
                    Hit.Breakdown.FinalScore = Hit.MatchedScore;
                    if (IsSAM25DebugExplainEnabled())
                    {
                         std::ostringstream Stream;
                         Stream << "sam25+ direct score=" << Hit.MatchedScore
                                << " signal=" << Hit.MatchedSignal
                                << " source=" << Hit.MatchedSource;
                         Hit.Explain = Stream.str();
                    }

                    if (!Hit.DocumentID.empty())
                    {
                         std::string ManifestValue;
                         const rocksdb::Status ManifestStatus =
                              DatabaseHandle->Get(rocksdb::ReadOptions(),
                                                  BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                                                  &ManifestValue);

                         if (!ManifestStatus.ok())
                         {
                              continue;
                         }

                         DocumentEntry Entry;

                         if (!ParseManifestValue(ManifestValue, Entry) ||
                             !IsSAMDocumentEntryCurrent(Entry, &FreshnessCache))
                         {
                              continue;
                         }

                         Hit.Title = Entry.Title.empty() ? Hit.Title : Entry.Title;
                         AccumulateSAMHit(AggregatedHits, Hit);
                    }
               }
               catch (...)
               {
               }
          }
     }

     AppendFuzzyFallbackHits(AggregatedHits, DatabaseHandle.get(), Collection, Query, Limit);
     AppendSAMLikePatternHits(AggregatedHits, DatabaseHandle.get(), Collection, Query);
     AppendSemanticProfileHits(AggregatedHits, DatabaseHandle.get(), Collection, SemanticPlan,
                               std::max<size_t>(256, (Limit * 24) + (Calibration.AdaptiveSemanticBudget * 8)));
     AppendSearchIdeaHits(AggregatedHits, DatabaseHandle.get(), Collection, SearchIdeas);
     const std::vector<SAM::LookupHit> GraphHits =
          BuildIntentGraphHits(DatabaseHandle.get(), Collection, Query,
                               std::max<size_t>(4, std::min<size_t>(Limit + Calibration.AdaptiveGraphBudget, 20)));
     for (const auto& GraphHit : GraphHits)
     {
          AccumulateSAMHit(AggregatedHits, GraphHit);
     }
     const std::vector<SAMLearnedVariant> SeededVariants =
          BuildSeededCollectionVariants(DatabaseHandle.get(), Collection, QueryViews, AggregatedHits,
                                        &ProfileHints.StrongTokens,
                                        std::max<size_t>(6, std::min<size_t>(Limit * 3, 12)));
     AppendCollectionLearnedHits(AggregatedHits, DatabaseHandle.get(), Collection, SeededVariants);

     const size_t RankingWindowLimit = Limit > 0
          ? std::max<size_t>(Limit, std::min<size_t>(AggregatedHits.size(), std::max<size_t>(Limit * 4, 64)))
          : 0;

     FinalizeSAMAggregatedHits(Hits, AggregatedHits, QueryViews, DatabaseHandle.get(), RankingWindowLimit);
     ApplySAMCollectionRankPrior(Collection, Hits);

     if (Limit > 0 && Hits.size() > Limit)
     {
          Hits.resize(Limit);
     }

     CaptureLookupEvaluation(DatabaseHandle.get(), Collection, Query, Hits, nullptr);

     EmitSAM25DebugLog(Query, Hits);
     FinishLookupActivity(ActivitySequence, Hits.size());
     return Hits;
}

/* Return stored search idea history for diagnostics and UI views. */

std::vector<SAM::SearchIdeaEntry> SAM::GetSearchIdeaHistory(const std::string& Collection,
                                                            size_t Limit) const
{
     std::vector<SearchIdeaEntry> Entries;

     if (Limit == 0)
     {
          return Entries;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return Entries;
     }

     const std::string Prefix = Collection.empty() ? "sam:idea:" : BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          if (!Collection.empty() && Entry.Collection != Collection)
          {
               continue;
          }

          Entries.push_back(std::move(Entry));
     }

     std::sort(Entries.begin(), Entries.end(),
               [](const SearchIdeaEntry& Left, const SearchIdeaEntry& Right)
               {
                    if (Left.LastSeenMS != Right.LastSeenMS)
                    {
                         return Left.LastSeenMS > Right.LastSeenMS;
                    }

                    if (Left.Uses != Right.Uses)
                    {
                         return Left.Uses > Right.Uses;
                    }

                    return Left.Query < Right.Query;
               });

     if (Entries.size() > Limit)
     {
          Entries.resize(Limit);
     }

     return Entries;
}

std::vector<SAM::SearchActivityEntry> SAM::GetActiveSearchActivities(const std::string& Collection) const
{
     std::vector<SearchActivityEntry> Entries;
     std::lock_guard<std::mutex> Lock(SearchActivityMutex);

     for (const auto& Pair : ActiveSearchActivities)
     {
          if (!Collection.empty() && Pair.second.Collection != Collection)
          {
               continue;
          }

          Entries.push_back(Pair.second);
     }

     std::sort(Entries.begin(), Entries.end(),
               [](const SearchActivityEntry& Left, const SearchActivityEntry& Right)
               {
                    if (Left.StartedMS != Right.StartedMS)
                    {
                         return Left.StartedMS > Right.StartedMS;
                    }

                    return Left.Sequence > Right.Sequence;
               });

     return Entries;
}

bool SAM::GetLatestSearchActivity(const std::string& Collection,
                                  SearchActivityEntry& Entry) const
{
     Entry = SearchActivityEntry{};
     const std::string ActivityKey = Collection.empty() ? "*" : Collection;
     std::lock_guard<std::mutex> Lock(SearchActivityMutex);
     const auto It = LatestSearchActivityByCollection.find(ActivityKey);

     if (It == LatestSearchActivityByCollection.end())
     {
          return false;
     }

     Entry = It->second;
     return true;
}

std::vector<SAM::DocumentEntry> SAM::ListDocuments(const std::string& Collection, size_t Limit, size_t Offset) const
{
     std::vector<DocumentEntry> Entries;

     if (Collection.empty() || Limit == 0)
     {
          return Entries;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return Entries;
     }

     const std::string Prefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(DatabaseHandle->NewIterator(rocksdb::ReadOptions()));
     size_t Seen = 0;

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          if (Seen++ < Offset)
          {
               continue;
          }

          Entries.push_back(std::move(Entry));

          if (Entries.size() >= Limit)
          {
               break;
          }
     }

     return Entries;
}

bool SAM::GetCollectionJobStatus(const std::string& Collection, CollectionJobStatus& Status) const
{
     Status = CollectionJobStatus{};

     if (Collection.empty())
     {
          return false;
     }

     std::lock_guard<std::mutex> Lock(JobMutex);

     const auto It = CollectionJobs.find(Collection);

     if (It == CollectionJobs.end())
     {
          return false;
     }

     Status = It->second;
     return true;
}

std::map<std::string, SAM::CollectionJobStatus> SAM::GetAllCollectionJobStatuses() const
{
     std::lock_guard<std::mutex> Lock(JobMutex);

     return CollectionJobs;
}

size_t SAM::GetBackgroundWorkerCount() const
{
     return std::max<size_t>(1, BackgroundWorkerCount);
}

void SAM::SetAutoIndexPauseUntilMS(uint64_t UntilMS)
{
     ManualAutoIndexPauseUntilMS.store(UntilMS, std::memory_order_relaxed);
}

bool SAM::BeginFlushPause(uint64_t UntilMS, std::string* ErrorMessage)
{
     bool Expected = false;

     if (!FlushInProgress.compare_exchange_strong(Expected, true, std::memory_order_acq_rel))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "A flush operation is already coordinating SAM work.";
          }

          return false;
     }

     FlushAutoIndexPauseUntilMS.store(UntilMS, std::memory_order_release);
     QueueCV.notify_all();
     return true;
}

void SAM::EndFlushPause()
{
     FlushAutoIndexPauseUntilMS.store(0, std::memory_order_release);
     FlushInProgress.store(false, std::memory_order_release);
     QueueCV.notify_all();
}

uint64_t SAM::GetAutoIndexPauseUntilMS() const
{
     return std::max(ManualAutoIndexPauseUntilMS.load(std::memory_order_acquire),
                     FlushAutoIndexPauseUntilMS.load(std::memory_order_acquire));
}

std::string SAM::GetAutoIndexPauseReason(uint64_t NowMS) const
{
     const uint64_t ManualPause = ManualAutoIndexPauseUntilMS.load(std::memory_order_acquire);
     const uint64_t FlushPause = FlushAutoIndexPauseUntilMS.load(std::memory_order_acquire);

     if (FlushInProgress.load(std::memory_order_acquire) &&
         (NowMS == 0 || FlushPause == 0 || NowMS < FlushPause))
     {
          return "flush";
     }

     if (ManualPause > 0 && (NowMS == 0 || NowMS < ManualPause))
     {
          return "manual";
     }

     return "";
}

bool SAM::IsFlushInProgress() const
{
     return FlushInProgress.load(std::memory_order_acquire);
}

size_t SAM::GetRunningCollectionJobCount() const
{
     std::lock_guard<std::mutex> Lock(JobMutex);
     size_t Running = 0;

     for (const auto& Entry : CollectionJobs)
     {
          if (Entry.second.Running)
          {
               ++Running;
          }
     }

     return Running;
}

bool SAM::GetCollectionIndexedMutationVersion(const std::string& Collection, uint64_t& Version) const
{
     Version = 0;

     if (Collection.empty())
     {
          return false;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          return false;
     }

     return ReadCollectionIndexedMutationVersionLocked(DatabaseHandle.get(), Collection, Version);
}

bool SAM::HasPendingCollectionRebuild(const std::string& Collection,
                                      uint64_t* RequestedVersion) const
{
     if (RequestedVersion)
     {
          *RequestedVersion = 0;
     }

     if (Collection.empty())
     {
          return false;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

          if (!DatabaseHandle)
          {
               return false;
          }

          SAMCollectionState State;
          std::string StateError;
          if (!ReadCollectionStateLocked(DatabaseHandle.get(), Collection, State, nullptr, &StateError))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("sam", "Failed to read collection state for '" + Collection + "' in HasPendingCollectionRebuild(): " + (StateError.empty() ? std::string("unknown error") : StateError) + ".");
               }
               return false;
          }

     if (RequestedVersion)
     {
          *RequestedVersion = State.RequestedMutationVersion;
     }

     return State.RebuildRequested;
}

bool SAM::SyncLexicalResources(const std::string& Collection,
                               bool* Updated,
                               std::string* ErrorMessage)
{
     if (Updated)
     {
          *Updated = false;
     }

     if (Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection is required for SAM lexical sync.";
          }

          return false;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     const std::string SynonymsSourceKey = "synonyms:" + Collection;
     const std::string StopwordsSourceKey = "stopwords:" + Collection;
     const std::string SynonymsRaw = HybridStorageManagerInstance().Get(SynonymsSourceKey);
     const std::string StopwordsRaw = HybridStorageManagerInstance().Get(StopwordsSourceKey);
     const uint64_t NowMS = Instance ? Instance->NowMs() : 0;

     auto BuildWrappedJSON = [&](const char* Kind,
                                 const std::string& SourceKey,
                                 const std::string& RawValue) -> std::string
     {
          nlohmann::json Wrapped;
          Wrapped["collection"] = Collection;
          Wrapped["kind"] = Kind;
          Wrapped["source_key"] = SourceKey;
          Wrapped["synced_at_ms"] = NowMS;

          if (RawValue.empty())
          {
               if (std::string(Kind) == "synonyms")
               {
                    Wrapped["synonyms"] = nlohmann::json::array();
               }
               else
               {
                    Wrapped["stopwords"] = nlohmann::json::array();
               }
          }
          else
          {
               try
               {
                    const nlohmann::json Parsed = nlohmann::json::parse(RawValue);

                    if (std::string(Kind) == "synonyms")
                    {
                         if (Parsed.is_object() && Parsed.contains("synonyms"))
                         {
                              Wrapped["synonyms"] = Parsed["synonyms"];
                         }
                         else if (Parsed.is_array())
                         {
                              Wrapped["synonyms"] = Parsed;
                         }
                         else
                         {
                              Wrapped["synonyms"] = nlohmann::json::array();
                         }
                    }
                    else
                    {
                         if (Parsed.is_object() && Parsed.contains("stopwords"))
                         {
                              Wrapped["stopwords"] = Parsed["stopwords"];
                         }
                         else if (Parsed.is_array())
                         {
                              Wrapped["stopwords"] = Parsed;
                         }
                         else
                         {
                              Wrapped["stopwords"] = nlohmann::json::array();
                         }
                    }
               }
               catch (const std::exception& E)
               {
                    if (ErrorMessage)
                    {
                         *ErrorMessage = E.what();
                    }

                    return "";
               }
          }

          return Wrapped.dump();
     };

     const std::string WrappedSynonyms = BuildWrappedJSON("synonyms", SynonymsSourceKey, SynonymsRaw);
     const std::string WrappedStopwords = BuildWrappedJSON("stopwords", StopwordsSourceKey, StopwordsRaw);

     if (WrappedSynonyms.empty() || WrappedStopwords.empty())
     {
          return false;
     }

     std::string ExistingSynonyms;
     std::string ExistingStopwords;
     (void)DatabaseHandle->Get(rocksdb::ReadOptions(), BuildLexicalMirrorKey("synonyms", Collection), &ExistingSynonyms);
     (void)DatabaseHandle->Get(rocksdb::ReadOptions(), BuildLexicalMirrorKey("stopwords", Collection), &ExistingStopwords);

     const bool Changed = (ExistingSynonyms != WrappedSynonyms || ExistingStopwords != WrappedStopwords);
     rocksdb::WriteBatch Batch;
     Batch.Put(BuildLexicalMirrorKey("synonyms", Collection), WrappedSynonyms);
     Batch.Put(BuildLexicalMirrorKey("stopwords", Collection), WrappedStopwords);

     const rocksdb::Status Status = DatabaseHandle->Write(rocksdb::WriteOptions(), &Batch);

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     if (Updated)
     {
          *Updated = Changed;
     }

     return true;
}

bool SAM::GetLexicalSyncInfo(const std::string& Collection,
                             LexicalSyncInfo& Info,
                             std::string* ErrorMessage) const
{
     Info = LexicalSyncInfo{};

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     auto ReadOne = [&](const std::string& Kind,
                        const std::string& Scope,
                        bool* Synced,
                        size_t* Count,
                        uint64_t* SyncedAt)
     {
          nlohmann::json Root;
          std::string LocalError;

          if (!LoadMirroredLexicalJSON(DatabaseHandle.get(), Kind, Scope, Root, &LocalError))
          {
               return;
          }

          *Synced = true;
          *SyncedAt = Root.value("synced_at_ms", static_cast<uint64_t>(0));

          if (Kind == "synonyms")
          {
               *Count = CountMirroredSynonymGroups(Root);
          }
          else
          {
               *Count = CountMirroredStopwords(Root);
          }
     };

     if (!Collection.empty())
     {
          ReadOne("synonyms", Collection, &Info.CollectionSynonymsSynced, &Info.CollectionSynonymGroups, &Info.CollectionSynonymsSyncedAtMS);
          ReadOne("stopwords", Collection, &Info.CollectionStopwordsSynced, &Info.CollectionStopwords, &Info.CollectionStopwordsSyncedAtMS);
     }

     ReadOne("synonyms", kSAMGlobalLexicalCollection, &Info.GlobalSynonymsSynced, &Info.GlobalSynonymGroups, &Info.GlobalSynonymsSyncedAtMS);
     ReadOne("stopwords", kSAMGlobalLexicalCollection, &Info.GlobalStopwordsSynced, &Info.GlobalStopwords, &Info.GlobalStopwordsSyncedAtMS);

     return true;
}

bool SAM::GetDocumentEntry(const std::string& Collection,
                           const std::string& DocumentID,
                           DocumentEntry& Entry,
                           std::string* ErrorMessage) const
{
     Entry = DocumentEntry{};

     if (Collection.empty() || DocumentID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection name and document ID are required.";
          }

          return false;
     }

     std::shared_ptr<rocksdb::DB> DatabaseHandle;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);
          DatabaseHandle = Database;
     }

     if (!DatabaseHandle)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM database is not open.";
          }

          return false;
     }

     std::string Value;
     const rocksdb::Status Status = DatabaseHandle->Get(rocksdb::ReadOptions(),
                                                        BuildDocManifestKey(Collection, DocumentID),
                                                        &Value);

     if (Status.IsNotFound())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "SAM document not found.";
          }

          return false;
     }

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     if (!ParseManifestValue(Value, Entry))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Failed to parse SAM document entry.";
          }

          return false;
     }

     if (!IsSAMDocumentEntryCurrent(Entry))
     {
          Entry = DocumentEntry{};

          if (ErrorMessage)
          {
               *ErrorMessage = "SAM document is stale relative to the source table.";
          }

          return false;
     }

     return true;
}

std::vector<SAM::DebugEvent> SAM::GetDebugEvents(const std::string& Collection,
                                                 uint64_t SinceSequence,
                                                 size_t Limit) const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);
     std::vector<DebugEvent> Events;

     if (Limit == 0)
     {
          return Events;
     }

     for (const auto& Event : DebugEvents)
     {
          if (Event.Sequence <= SinceSequence)
          {
               continue;
          }

          if (!Collection.empty() && Event.Collection != Collection)
          {
               continue;
          }

          Events.push_back(Event);

          if (Events.size() >= Limit)
          {
               break;
          }
     }

     return Events;
}

uint64_t SAM::GetLatestDebugSequence() const
{
     std::lock_guard<std::mutex> Lock(DebugMutex);

     if (DebugEvents.empty())
     {
          return 0;
     }

     return DebugEvents.back().Sequence;
}

size_t SAM::GetDroppedPendingSearchIdeaJobs() const
{
     std::lock_guard<std::mutex> Lock(SearchIdeaQueueMutex);

     return DroppedPendingSearchIdeaJobs;
}

size_t SAM::GetDroppedPendingSearchInteractionJobs() const
{
     std::lock_guard<std::mutex> Lock(SearchInteractionQueueMutex);

     return DroppedPendingSearchInteractionJobs;
}

#endif /* HLQUERY_SAM_SPLIT_INCLUDE */
