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
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/config.h"
#include "core/hlquery.h"
#include "runtime/serverconfig.h"
#include "search/bm25_scoring.h"
#include "search/document_collection_store.h"
#include "search/lexical_inverted_index.h"
#include "search/mapped_posting_index.h"
#include "search/rocksdb_storage_engine.h"
#include "utils/wildcard.h"

/* ToLowerAsciiSafe - Lowercases one byte using unsigned-char promotion to avoid undefined behavior. */

static char ToLowerAsciiSafe(char ch)
{
     return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

/* IsNormalizedCashtag - Returns true for normalized ticker-style cashtags such as $spy. */

static bool IsNormalizedCashtag(const std::string &term)
{
     return term.size() > 1 &&
            term.front() == '$' &&
            std::isalnum(static_cast<unsigned char>(term[1]));
}

/* InvertedIndex::NormalizeTerm - Normalizes a term for indexing. */

std::string InvertedIndex::NormalizeTerm(const std::string &Term)
{
     std::string Normalized = Term;

     std::transform(Normalized.begin(), Normalized.end(), Normalized.begin(), ToLowerAsciiSafe);

     /* Remove punctuation at start and end, but preserve wildcards and underscores. */

     while (!Normalized.empty() &&
            !std::isalnum(static_cast<unsigned char>(Normalized.front())) &&
            Normalized.front() != '*' &&
            Normalized.front() != '?' &&
            Normalized.front() != '_' &&
            !IsNormalizedCashtag(Normalized))
     {
          Normalized.erase(0, 1);
     }

     while (!Normalized.empty() && !std::isalnum(static_cast<unsigned char>(Normalized.back())) && Normalized.back() != '*' && Normalized.back() != '?' && Normalized.back() != '_')
     {
          Normalized.pop_back();
     }

     return Normalized;
}

/* NormalizeUrlToken - Normalizes a URL token for indexing. */

static std::string NormalizeUrlToken(const std::string &token)
{
     std::string Normalized = token;
     std::transform(Normalized.begin(), Normalized.end(), Normalized.begin(), ToLowerAsciiSafe);

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

/* ToLowerCopy - Returns a lowercase copy of a string. */

static std::string ToLowerCopy(const std::string &input)
{
     std::string out = input;
     std::transform(out.begin(), out.end(), out.begin(), ToLowerAsciiSafe);
     return out;
}

/* BuildFieldScopedTerm - Builds the synthetic term used for field-restricted search clauses. */

static std::string BuildFieldScopedTerm(const std::string &field_name, const std::string &term)
{
     return "__field__" + field_name + ":" + term;
}

/* FieldNameHasToken - Checks if field name contains any token. */

static bool FieldNameHasToken(const std::string &field_name, const std::initializer_list<const char *> &tokens)
{
     std::string lower = ToLowerCopy(field_name);

     for (const auto *token : tokens)
     {
          if (lower.find(token) != std::string::npos)
          {
               return true;
          }
     }

     return false;
}

/* InvertedIndex::MarkCollectionDirtyLocked - Marks a collection dirty while the caller holds the lock. */

void InvertedIndex::MarkCollectionDirtyLocked(const std::string &Collection)
{
     DirtyCollections.insert(Collection);
     CollectionLastMutation[Collection] = std::chrono::steady_clock::now();
}

size_t InvertedIndex::EnsureCollectionTotalLengthLocked(const std::string &Collection)
{
     /* Reuse the cached total when the collection has not invalidated its length accounting. */

     auto TotalIt = CollectionTotalLengths.find(Collection);

     if (TotalIt != CollectionTotalLengths.end())
     {
          return TotalIt->second;
     }

     /* Fall back to rebuilding the total from per-document lengths after cache misses or repairs. */

     size_t TotalLength = 0;
     auto LengthsIt = DocumentLengths.find(Collection);

     if (LengthsIt != DocumentLengths.end())
     {
          for (const auto &[DocID, Length] : LengthsIt->second)
          {
               (void)DocID;
               TotalLength += Length;
          }
     }

     CollectionTotalLengths[Collection] = TotalLength;
     return TotalLength;
}

void InvertedIndex::RefreshCollectionStatsFromTotalLocked(const std::string &Collection)
{
     /* Document count is derived from the authoritative length map for the collection. */

     auto LengthsIt = DocumentLengths.find(Collection);
     const size_t DocCountValue = (LengthsIt != DocumentLengths.end()) ? LengthsIt->second.size() : 0;

     DocCounts[Collection] = DocCountValue;

     if (DocCountValue == 0)
     {
          /* Empty collections keep a nonzero average length so scoring code avoids divide-by-zero paths. */

          CollectionTotalLengths[Collection] = 0;
          AvgDocLengths[Collection] = 1.0;
          return;
     }

     const size_t TotalLength = EnsureCollectionTotalLengthLocked(Collection);
     AvgDocLengths[Collection] = static_cast<double>(TotalLength) / static_cast<double>(DocCountValue);
}

void InvertedIndex::TouchCollectionLocked(const std::string &Collection)
{
     if (!Collection.empty())
     {
          CollectionLastAccess[Collection] = std::chrono::steady_clock::now();
     }
}

size_t InvertedIndex::EstimateCollectionMemoryLocked(const std::string &Collection) const
{
     /* This is an approximate residency estimate used for eviction, not a precise allocator report. */

     size_t Bytes = 0;

     auto IndexIt = Index.find(Collection);
     if (IndexIt != Index.end())
     {
          /* Account for term keys, posting vectors, and position buffers in the in-memory inverted index. */

          Bytes += sizeof(IndexIt->second);

          for (const auto &[Term, Postings] : IndexIt->second)
          {
               Bytes += sizeof(Term) + Term.capacity();
               Bytes += sizeof(Postings) + (Postings.capacity() * sizeof(Posting));

               for (const auto &Post : Postings)
               {
                    Bytes += Post.DocumentID.capacity();
                    Bytes += Post.Collection.capacity();
                    Bytes += Post.Positions.capacity() * sizeof(size_t);
               }
          }
     }

     auto TermsIt = DocumentTerms.find(Collection);
     if (TermsIt != DocumentTerms.end())
     {
          /* DocumentTerms lets deletion find every term contributed by a document, so it is part of residency. */

          Bytes += sizeof(TermsIt->second);

          for (const auto &[DocID, Terms] : TermsIt->second)
          {
               Bytes += sizeof(DocID) + DocID.capacity();
               Bytes += sizeof(Terms);

               for (const auto &Term : Terms)
               {
                    Bytes += sizeof(Term) + Term.capacity();
               }
          }
     }

     auto LengthsIt = DocumentLengths.find(Collection);
     if (LengthsIt != DocumentLengths.end())
     {
          Bytes += sizeof(LengthsIt->second) + (LengthsIt->second.size() * (sizeof(std::string) + sizeof(size_t) + 32));

          for (const auto &[DocID, Length] : LengthsIt->second)
          {
               (void)Length;
               Bytes += DocID.capacity();
          }
     }

     auto MMapIt = MMapIndexes.find(Collection);
     if (MMapIt != MMapIndexes.end() && MMapIt->second)
     {
          /* Mapped indexes contribute their mapped byte range even when postings stay outside heap memory. */

          Bytes += sizeof(MMapIndex) + MMapIt->second->GetMappedSize();
     }

     return Bytes;
}

size_t InvertedIndex::EstimateLoadedMemoryLocked() const
{
     /* Build a collection set first so memory shared across maps is counted once per collection. */

     std::unordered_set<std::string> Collections;

     for (const auto &[Collection, Terms] : Index)
     {
          (void)Terms;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, Terms] : DocumentTerms)
     {
          (void)Terms;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, Lengths] : DocumentLengths)
     {
          (void)Lengths;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, MMapIdx] : MMapIndexes)
     {
          (void)MMapIdx;
          Collections.insert(Collection);
     }

     size_t Bytes = 0;

     for (const auto &Collection : Collections)
     {
          Bytes += EstimateCollectionMemoryLocked(Collection);
     }

     return Bytes;
}

size_t InvertedIndex::EvictLoadedCollectionsLocked(size_t TargetBytes)
{
     if (TargetBytes == 0)
     {
          return 0;
     }

     /* Eviction only runs when estimated loaded data exceeds the configured target. */

     size_t LoadedBytes = EstimateLoadedMemoryLocked();
     if (LoadedBytes <= TargetBytes)
     {
          return 0;
     }

     struct EvictionCandidate
     {
          std::string Collection;
          std::chrono::steady_clock::time_point LastAccess;
          size_t Bytes = 0;
     };

     /* Dirty collections are excluded so pending writes are not discarded before a flush. */

     std::unordered_set<std::string> Collections;

     for (const auto &[Collection, Terms] : Index)
     {
          (void)Terms;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, MMapIdx] : MMapIndexes)
     {
          (void)MMapIdx;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, Terms] : DocumentTerms)
     {
          (void)Terms;
          Collections.insert(Collection);
     }

     for (const auto &[Collection, Lengths] : DocumentLengths)
     {
          (void)Lengths;
          Collections.insert(Collection);
     }

     std::vector<EvictionCandidate> Candidates;
     Candidates.reserve(Collections.size());

     for (const auto &Collection : Collections)
     {
          if (DirtyCollections.find(Collection) != DirtyCollections.end())
          {
               continue;
          }

          const size_t Bytes = EstimateCollectionMemoryLocked(Collection);
          if (Bytes == 0)
          {
               continue;
          }

          auto AccessIt = CollectionLastAccess.find(Collection);
          const auto LastAccess = (AccessIt != CollectionLastAccess.end())
                                       ? AccessIt->second
                                       : std::chrono::steady_clock::time_point{};

          Candidates.push_back({Collection, LastAccess, Bytes});
     }

     std::sort(Candidates.begin(), Candidates.end(), [](const EvictionCandidate &A, const EvictionCandidate &B)
               {
                    /* Older access times are preferred, with collection name as a deterministic tie-breaker. */

                    if (A.LastAccess != B.LastAccess)
                    {
                         return A.LastAccess < B.LastAccess;
                    }

                    return A.Collection < B.Collection;
               });

     size_t Evicted = 0;

     for (const auto &Candidate : Candidates)
     {
          if (LoadedBytes <= TargetBytes)
          {
               break;
          }

          Index.erase(Candidate.Collection);
          DocumentTerms.erase(Candidate.Collection);
          DocumentLengths.erase(Candidate.Collection);
          CollectionTotalLengths.erase(Candidate.Collection);
          DocCounts.erase(Candidate.Collection);
          AvgDocLengths.erase(Candidate.Collection);
          MMapIndexes.erase(Candidate.Collection);
          CollectionLastMutation.erase(Candidate.Collection);
          CollectionLastFlush.erase(Candidate.Collection);
          CollectionLastAccess.erase(Candidate.Collection);

          /* Keep the running estimate monotonic even when the approximation is larger than loaded bytes. */

          LoadedBytes = (LoadedBytes > Candidate.Bytes) ? (LoadedBytes - Candidate.Bytes) : 0;
          Evicted++;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "Evicted clean loaded index for collection '" + Candidate.Collection + "' to reduce index memory.");
          }
     }

     return Evicted;
}

size_t InvertedIndex::EvictLoadedCollectionsIfNeeded(size_t MaxBytes)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     return EvictLoadedCollectionsLocked(MaxBytes);
}

/* InvertedIndex::SelectFlushCollectionsLocked - Selects dirty collections to flush while the caller holds the lock. */

std::vector<std::string> InvertedIndex::SelectFlushCollectionsLocked(uint64_t MinDirtyAgeSeconds, size_t MaxCollections) const
{
     std::vector<std::string> CollectionsToFlush;
     const auto Now = std::chrono::steady_clock::now();

     /* Only collections that are both dirty and backed by in-memory postings can be flushed. */

     for (const auto &Collection : DirtyCollections)
     {
          auto IndexIt = Index.find(Collection);

          if (IndexIt == Index.end() || IndexIt->second.empty())
          {
               continue;
          }

          if (MinDirtyAgeSeconds > 0)
          {
               /* Age gating lets background flushers batch rapid mutation bursts. */

               auto MutationIt = CollectionLastMutation.find(Collection);

               if (MutationIt != CollectionLastMutation.end())
               {
                    const auto DirtyAge = std::chrono::duration_cast<std::chrono::seconds>(Now - MutationIt->second).count();

                    if (DirtyAge < static_cast<long long>(MinDirtyAgeSeconds))
                    {
                         continue;
                    }
               }
          }

          CollectionsToFlush.push_back(Collection);

          if (MaxCollections > 0 && CollectionsToFlush.size() >= MaxCollections)
          {
               break;
          }
     }

     return CollectionsToFlush;
}

/* InvertedIndex::FlushCollectionToDiskLocked - Flushes a collection while the caller holds the lock. */

bool InvertedIndex::FlushCollectionToDiskLocked(const std::string &IndexDir, const std::string &Collection)
{
     auto IndexIt = Index.find(Collection);

     if (IndexIt == Index.end() || IndexIt->second.empty())
     {
          /* A dirty marker without postings is stale and can be cleared immediately. */

          DirtyCollections.erase(Collection);
          return false;
     }

     auto ExistingMMapIt = MMapIndexes.find(Collection);

     /* Avoid overwriting an existing mapped snapshot when the current memory state is only a partial delta. */

     if (ExistingMMapIt != MMapIndexes.end() &&
         ExistingMMapIt->second &&
         ExistingMMapIt->second->IsValid() &&
         CollectionLastFlush.find(Collection) == CollectionLastFlush.end())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "FlushToDisk: Deferred collection '" + Collection + "' because it has a loaded mmap snapshot and only partial in-memory mutations; segment merge or rebuild is required before overwriting the snapshot.");
          }

          return false;
     }

     const std::string FlushMarker = "flush_pending:" + Collection;

     if (Instance && Instance->Database)
     {
          /* The marker lets startup detect and repair interrupted flushes. */

          Instance->Database->Set(FlushMarker, "1");
     }

     /* IndexWriter expects the same collection map shape used by full index serialization. */

     IndexWriter CollectionWriter(IndexDir, Collection);
     std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>> SingleCollection;
     SingleCollection[Collection] = IndexIt->second;

     if (!CollectionWriter.WriteIndex(SingleCollection))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "FlushToDisk: Failed to flush collection '" + Collection + "'.");
          }

          return false;
     }

     auto MMapIdx = MMapIndex::Open(IndexDir, Collection);

     if (MMapIdx && MMapIdx->IsValid())
     {
          MMapIndexes[Collection] = std::move(MMapIdx);
     }

     DirtyCollections.erase(Collection);
     CollectionLastFlush[Collection] = std::chrono::steady_clock::now();

     if (Instance && Instance->Database)
     {
          Instance->Database->Del(FlushMarker);
     }

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("inverted_index", "FlushToDisk: Flushed collection '" + Collection + "' to disk.");
     }

     return true;
}

/* InvertedIndex::ExtractTerms - Extracts normalized terms from text. */

std::vector<std::string> InvertedIndex::ExtractTerms(const std::string &Text)
{
     std::vector<std::string> Terms;

     if (Text.empty())
     {
          return Terms;
     }

     /* Limit text size to prevent processing extremely large documents. */

     const size_t MaxTextSize = 1000000;
     const std::string &TextToProcess = (Text.length() > MaxTextSize) ? Text.substr(0, MaxTextSize) : Text;
     size_t Pos = 0;
     size_t TextLen = TextToProcess.length();
     const size_t MaxTerms = 100000;

     size_t TermCount = 0;

     /* Cashtags are kept as their own searchable form and later mirrored without the dollar prefix. */

     auto IsCashtagStart = [&](size_t Offset) -> bool
     {
          return TextToProcess[Offset] == '$' &&
                 Offset + 1 < TextLen &&
                 std::isalnum(static_cast<unsigned char>(TextToProcess[Offset + 1]));
     };

     auto IsTermSeparator = [&](size_t Offset) -> bool
     {
          const char Ch = TextToProcess[Offset];

          if (Ch == '$')
          {
               /* A dollar sign starts a term only when it is followed by an alphanumeric symbol. */

               return !IsCashtagStart(Offset);
          }

          return std::isspace(static_cast<unsigned char>(Ch)) || Ch == '-' || Ch == '.' || Ch == ',' || Ch == ':' || Ch == '/' || Ch == '\\' || Ch == '(' || Ch == ')' || Ch == '[' || Ch == ']' || Ch == '{' || Ch == '}' || Ch == '@' || Ch == '#' || Ch == '%' || Ch == '&' || Ch == '+' || Ch == '=' || Ch == ';' || Ch == '|' || Ch == '!' || Ch == '?' || Ch == '~' || Ch == '^' || Ch == '`';
     };

     while (Pos < TextLen && TermCount < MaxTerms)
     {
          /* Skip whitespace and punctuation that should be treated as separators. */

          while (Pos < TextLen && IsTermSeparator(Pos))
          {
               Pos++;
          }

          if (Pos >= TextLen)
          {
               break;
          }

          /* Find end of word. */

          size_t WordStart = Pos;

          while (Pos < TextLen && !IsTermSeparator(Pos))
          {
               if (TextToProcess[Pos] == '$' && Pos != WordStart)
               {
                    break;
               }

               Pos++;
          }

          if (Pos > WordStart)
          {
               std::string Word = TextToProcess.substr(WordStart, Pos - WordStart);

               std::string Normalized = NormalizeTerm(Word);

               if (!Normalized.empty() && Normalized.length() >= 1)
               {
                    Terms.push_back(Normalized);

                    if (IsNormalizedCashtag(Normalized))
                    {
                         /* Index the bare ticker too so $spy can match both cashtag and plain ticker queries. */

                         Terms.push_back(Normalized.substr(1));
                    }

                    TermCount++;
               }
          }
     }

     return Terms;
}

/* IsLikelyTld - Returns true if the tail looks like a TLD. */

static bool IsLikelyTld(const std::string &tail)
{
     if (tail.empty())
     {
          return false;
     }

     /* TLD checks are deliberately conservative to avoid treating arbitrary dotted text as URLs. */

     std::string lower = tail;
     std::transform(lower.begin(), lower.end(), lower.begin(), ToLowerAsciiSafe);

     if (lower.size() < 2 || lower.size() > 63)
     {
          return false;
     }

     if (lower.rfind("xn--", 0) == 0)
     {
          /* Punycode labels may contain digits and hyphens after the xn-- prefix. */

          for (char ch : lower)
          {
               if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-'))
               {
                    return false;
               }
          }
          return true;
     }

     for (char ch : lower)
     {
          if (!std::isalpha(static_cast<unsigned char>(ch)))
          {
               return false;
          }
     }

     return true;
}

/* IsValidHostLabel - Validates one hostname label before URL-specific token extraction. */

static bool IsValidHostLabel(const std::string &label)
{
     if (label.empty() || label.size() > 63)
     {
          return false;
     }

     if (label.front() == '-' || label.back() == '-')
     {
          return false;
     }

     for (char ch : label)
     {
          if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-'))
          {
               return false;
          }
     }

     return true;
}

/*
 * SplitAndValidateHostname - Splits a host into validated labels and returns the detected top-level domain.
 */

static bool SplitAndValidateHostname(const std::string &host, std::vector<std::string> &parts, std::string &tld_out)
{
     if (host.empty() || host.find("..") != std::string::npos)
     {
          return false;
     }

     /* Host labels are validated one by one before URL terms are admitted into the index. */

     std::stringstream host_stream(host);
     std::string part;

     while (std::getline(host_stream, part, '.'))
     {
          if (part.empty() || !IsValidHostLabel(part))
          {
               return false;
          }
          parts.push_back(part);
     }

     if (parts.size() < 2)
     {
          return false;
     }

     /* A hostname must include at least one label and one plausible top-level domain. */

     const std::string &tld = parts.back();

     if (!IsLikelyTld(tld))
     {
          return false;
     }

     tld_out = tld;
     return true;
}

/* ExtractHostFromToken - Pulls the host portion from a URL-like token before path parsing starts. */

static std::string ExtractHostFromToken(const std::string &token)
{
     std::string lower = token;
     std::transform(lower.begin(), lower.end(), lower.begin(), ToLowerAsciiSafe);

     /* Strip the scheme and common web prefix before host validation. */

     const std::string scheme_sep = "://";
     size_t scheme_pos = lower.find(scheme_sep);
     if (scheme_pos != std::string::npos)
     {
          lower = lower.substr(scheme_pos + scheme_sep.size());
     }

     if (lower.rfind("www.", 0) == 0)
     {
          lower = lower.substr(4);
     }

     size_t end_host = lower.find_first_of("/?#");
     std::string host = (end_host == std::string::npos) ? lower : lower.substr(0, end_host);

     if (host.empty())
     {
          return std::string();
     }

     size_t port_pos = host.find(':');
     if (port_pos != std::string::npos)
     {
          /* Ports are not indexed as hostname terms. */

          host = host.substr(0, port_pos);
     }

     return host;
}

/* TrimUrlToken - Trims punctuation around URL tokens. */

static std::string TrimUrlToken(const std::string &token)
{
     size_t start = 0;
     size_t end = token.size();

     /* Preserve slash and colon while trimming sentence punctuation around URL-like tokens. */

     while (start < end && std::ispunct(static_cast<unsigned char>(token[start])) && token[start] != '/' && token[start] != ':')
     {
          start++;
     }

     while (end > start && std::ispunct(static_cast<unsigned char>(token[end - 1])) && token[end - 1] != '/' && token[end - 1] != ':')
     {
          end--;
     }

     return token.substr(start, end - start);
}

/* AddUrlParts - Extracts URL terms and TLDs from a token. */

static void AddUrlParts(const std::string &url_token, std::unordered_set<std::string> &url_terms, std::unordered_set<std::string> &url_tlds)
{
     std::string token = TrimUrlToken(url_token);

     if (token.empty() || token.find('@') != std::string::npos)
     {
          /* Email-like tokens are skipped so domains from addresses do not get boosted as URLs. */

          return;
     }

     std::string lower = token;
     std::transform(lower.begin(), lower.end(), lower.begin(), ToLowerAsciiSafe);

     const std::string scheme_sep = "://";
     size_t scheme_pos = lower.find(scheme_sep);

     if (scheme_pos != std::string::npos)
     {
          lower = lower.substr(scheme_pos + scheme_sep.size());
     }

     if (lower.rfind("www.", 0) == 0)
     {
          lower = lower.substr(4);
     }

     size_t end_host = lower.find_first_of("/?#");
     std::string host = (end_host == std::string::npos) ? lower : lower.substr(0, end_host);

     if (host.empty())
     {
          return;
     }

     size_t port_pos = host.find(':');
     if (port_pos != std::string::npos)
     {
          host = host.substr(0, port_pos);
     }

     std::vector<std::string> host_parts;
     std::string tld;
     if (!SplitAndValidateHostname(host, host_parts, tld))
     {
          return;
     }

     if (!host_parts.empty())
     {
          /* TLDs are tracked separately because their ranking weight is intentionally lower. */

          url_tlds.insert(tld);
          host_parts.pop_back();
     }

     for (const auto &hp : host_parts)
     {
          /* Host labels become normal searchable terms after URL-specific normalization. */

          std::string normalized = NormalizeUrlToken(hp);
          if (!normalized.empty())
          {
               url_terms.insert(normalized);
          }
     }

     if (end_host != std::string::npos && end_host + 1 < lower.size())
     {
          /* Path segments are broken into alphanumeric tokens so slugs remain searchable. */

          std::string path = lower.substr(end_host + 1);
          std::string token_accum;

          for (char ch : path)
          {
               if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')
               {
                    token_accum.push_back(ch);
               }
               else
               {
                    if (!token_accum.empty())
                    {
                         std::string normalized = NormalizeUrlToken(token_accum);
                         if (normalized.length() >= 2)
                         {
                              url_terms.insert(normalized);
                         }
                         token_accum.clear();
                    }
               }
          }

          if (!token_accum.empty())
          {
               std::string normalized = NormalizeUrlToken(token_accum);
               if (normalized.length() >= 2)
               {
                    url_terms.insert(normalized);
               }
          }
     }
}

/* ExtractUrlTerms - Extracts URL-related terms from text. */

static void ExtractUrlTerms(const std::string &text, std::unordered_set<std::string> &url_terms, std::unordered_set<std::string> &url_tlds)
{
     if (text.empty())
     {
          return;
     }

     /* URL detection starts from whitespace-delimited tokens and then validates host structure. */

     std::istringstream iss(text);
     std::string token;

     while (iss >> token)
     {
          std::string trimmed = TrimUrlToken(token);
          if (trimmed.empty())
          {
               continue;
          }

          bool has_scheme = trimmed.find("://") != std::string::npos;
          bool has_www = trimmed.rfind("www.", 0) == 0;
          bool has_tld = false;

          if (!has_scheme && !has_www)
          {
               /* Bare domains are accepted only after hostname and TLD validation. */

               std::string host = ExtractHostFromToken(trimmed);
               if (!host.empty())
               {
                    std::vector<std::string> host_parts;
                    std::string tld;
                    if (SplitAndValidateHostname(host, host_parts, tld))
                    {
                         has_tld = true;
                    }
               }
          }

          if (has_scheme || has_www || has_tld)
          {
               AddUrlParts(trimmed, url_terms, url_tlds);
          }
     }
}

/*
 * InvertedIndex::RemoveDocumentFromIndex - Removes every posting previously contributed by one document.
 */

void InvertedIndex::RemoveDocumentFromIndex(const std::string &Collection, const std::string &DocID)
{
     auto IndexIt = Index.find(Collection);

     if (IndexIt == Index.end())
     {
          return;
     }

     /* DocumentTerms is the reverse map that makes removal bounded by a document's own terms. */

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

     auto &CollectionIndex = IndexIt->second;

     for (const auto &Term : TermsIt->second)
     {
          /* Remove only postings owned by this document and delete empty term buckets afterwards. */

          auto TermIt = CollectionIndex.find(Term);

          if (TermIt != CollectionIndex.end())
          {
               auto &Postings = TermIt->second;

               Postings.erase(std::remove_if(Postings.begin(), Postings.end(), [&DocID](const Posting &p)
                                             {
                                                  return p.DocumentID == DocID;
                                             }),
                              Postings.end());

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
          const auto RemovedLengthIt = DocLengthsIt->second.find(DocID);
          if (RemovedLengthIt != DocLengthsIt->second.end())
          {
               /* Subtract the removed document from the cached total before erasing its length row. */

               const size_t CurrentTotal = EnsureCollectionTotalLengthLocked(Collection);
               CollectionTotalLengths[Collection] = (CurrentTotal > RemovedLengthIt->second) ? (CurrentTotal - RemovedLengthIt->second) : 0;
          }

          DocLengthsIt->second.erase(DocID);

          if (DocLengthsIt->second.empty())
          {
               DocumentLengths.erase(DocLengthsIt);
          }
     }

     RefreshCollectionStatsFromTotalLocked(Collection);
}

/*
 * InvertedIndex::AddDocument - Tokenizes document fields, updates postings, and refreshes collection statistics.
 */

bool InvertedIndex::AddDocument(const std::string &Collection, const Document &Doc)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

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
     const bool DocumentAlreadyIndexed = DocTermsIt != DocumentTerms.end() &&
                                         DocTermsIt->second.find(Doc.ID) != DocTermsIt->second.end();

     /* Existing documents can be replaced even when the collection is at the configured document limit. */

     if (!DocumentAlreadyIndexed && CollectionDocCount >= INVERTED_INDEX_MAX_DOCUMENTS_PER_COLLECTION)
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

     /* The term limit caps collection growth before extracting and allocating a large replacement payload. */

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

     std::unordered_map<std::string, std::vector<std::string>> FieldTermsByName;
     FieldTermsByName["title"] = TitleTerms;
     FieldTermsByName["content"] = ContentTerms;
     FieldTermsByName["id"] = IDTerms;

     /* URL-derived terms are tracked separately so their weights can differ from ordinary text tokens. */

     std::unordered_set<std::string> UrlTerms;
     std::unordered_set<std::string> UrlTldTerms;
     ExtractUrlTerms(Doc.Title, UrlTerms, UrlTldTerms);
     ExtractUrlTerms(Doc.Content, UrlTerms, UrlTldTerms);

     std::unordered_set<std::string> TitleLikeTerms;
     std::unordered_set<std::string> TagLikeTerms;
     std::unordered_set<std::string> UrlFieldTerms;

     double url_token_boost = 1.3;
     double url_tld_weight = 0.3;
     double title_like_boost = 1.6;
     double tag_like_boost = 1.2;

     if (Instance && Instance->Config)
     {
          /* Runtime ranking configuration can tune field and URL boosts without rebuilding the index. */

          url_token_boost = Instance->Config->GetUrlTokenBoost();
          url_tld_weight = Instance->Config->GetUrlTldWeight();
          title_like_boost = Instance->Config->GetTitleLikeBoost();
          tag_like_boost = Instance->Config->GetTagLikeBoost();
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Extracted " + std::to_string(TitleTerms.size()) + " title terms, " + std::to_string(ContentTerms.size()) + " content terms and " + std::to_string(IDTerms.size()) + " id terms.");
     }

     std::vector<std::string> AllTermsList;

     std::set<std::string> AllTermsSet;

     /* The list preserves term positions, while the set keeps one posting bucket per unique term. */

     for (const auto &Term : TitleTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     for (const auto &Term : ContentTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     for (const auto &Term : IDTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Extracting terms from " + std::to_string(Doc.Fields.size()) + " custom fields.");
     }

     size_t FieldCount = 0;

     for (const auto &Field : Doc.Fields)
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
          FieldTermsByName[Field.first] = FieldTerms;

          /* Field names are interpreted heuristically to apply boosts without requiring a fixed schema. */

          bool is_title_like = FieldNameHasToken(Field.first, {"title", "name", "subject", "headline"});
          bool is_tag_like = FieldNameHasToken(Field.first, {"tag", "tags", "keyword", "keywords", "category", "topic"});
          bool is_url_like = FieldNameHasToken(Field.first, {"url", "uri", "link", "website", "site", "source"});

          for (const auto &Term : FieldTerms)
          {
               AllTermsList.push_back(Term);

               AllTermsSet.insert(Term);

               if (is_title_like)
               {
                    TitleLikeTerms.insert(Term);
               }

               if (is_tag_like)
               {
                    TagLikeTerms.insert(Term);
               }

               if (is_url_like)
               {
                    UrlFieldTerms.insert(Term);
               }
          }

          ExtractUrlTerms(Field.second, UrlTerms, UrlTldTerms);
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Total terms: " + std::to_string(AllTermsList.size()) + " (unique: " + std::to_string(AllTermsSet.size()) + ").");
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Adding terms to index.");
     }

     auto &CollectionIndex = Index[Collection];

     auto &DocTerms = DocumentTerms[Collection][Doc.ID];

     DocTerms.clear();

     size_t DocLength = AllTermsList.size();

     std::unordered_map<std::string, std::vector<size_t>> TermPositions;
     std::unordered_map<std::string, std::unordered_map<std::string, std::vector<size_t>>> FieldTermPositions;

     /* Global positions support proximity scoring across the document body. */

     size_t Pos = 0;

     for (const auto &T : AllTermsList)
     {
          TermPositions[T].push_back(Pos);

          Pos++;
     }

     for (const auto &[FieldName, FieldTerms] : FieldTermsByName)
     {
          /* Field positions support scoped field queries without mixing offsets across fields. */

          std::size_t FieldPos = 0;

          for (const auto &FieldTerm : FieldTerms)
          {
               FieldTermPositions[FieldName][FieldTerm].push_back(FieldPos);
               FieldPos++;
          }
     }

     auto BuildPostingForTerm = [&](const std::string &Term,
                                    const std::vector<size_t> &Positions,
                                    bool InTitle) -> Posting
     {
          /* Posting score starts as a boost carrier; final ranking is computed at query time. */

          Posting Post;

          Post.DocumentID = Doc.ID;
          Post.Collection = Collection;
          Post.Score = 1.0;
          Post.Positions = Positions;

          if (InTitle)
          {
               Post.Score = std::max(Post.Score, 2.0);
          }

          if (TitleLikeTerms.find(Term) != TitleLikeTerms.end())
          {
               Post.Score = std::max(Post.Score, title_like_boost);
          }

          if (TagLikeTerms.find(Term) != TagLikeTerms.end())
          {
               Post.Score *= tag_like_boost;
          }

          if (UrlTldTerms.find(Term) != UrlTldTerms.end())
          {
               /* Top-level domains are useful signals but should not dominate content terms. */

               Post.Score *= url_tld_weight;
          }
          else if (UrlTerms.find(Term) != UrlTerms.end() || UrlFieldTerms.find(Term) != UrlFieldTerms.end())
          {
               Post.Score *= url_token_boost;
          }

          return Post;
     };

     size_t TermIndexCount = 0;

     for (const auto &Term : AllTermsSet)
     {
          TermIndexCount++;

          if (TermIndexCount % 1000 == 0)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("inverted_index", "AddDocument: Indexing term " + std::to_string(TermIndexCount) + "/" + std::to_string(AllTermsSet.size()) + ".");
               }
          }

          auto PosIt = TermPositions.find(Term);
          const std::vector<size_t> Positions = (PosIt != TermPositions.end()) ? PosIt->second : std::vector<size_t>{};
          const bool InTitle = std::find(TitleTerms.begin(), TitleTerms.end(), Term) != TitleTerms.end();
          Posting Post = BuildPostingForTerm(Term, Positions, InTitle);

          /* Unscoped postings serve normal keyword searches. */

          CollectionIndex[Term].push_back(Post);

          DocTerms.insert(Term);
     }

     for (const auto &[FieldName, FieldPositions] : FieldTermPositions)
     {
          const bool IsTitleField = (FieldName == "title");

          for (const auto &[Term, Positions] : FieldPositions)
          {
               /* Scoped synthetic terms keep field filters in the same inverted index structure. */

               const std::string ScopedTerm = BuildFieldScopedTerm(FieldName, Term);
               Posting ScopedPost = BuildPostingForTerm(Term, Positions, IsTitleField);
               CollectionIndex[ScopedTerm].push_back(ScopedPost);
               DocTerms.insert(ScopedTerm);
          }
     }

     const size_t CurrentTotalLength = EnsureCollectionTotalLengthLocked(Collection);
     DocumentLengths[Collection][Doc.ID] = DocLength;
     CollectionTotalLengths[Collection] = CurrentTotalLength + DocLength;
     RefreshCollectionStatsFromTotalLocked(Collection);

     /* Mark after all maps are consistent so background flushers see a complete mutation. */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Indexed " + std::to_string(AllTermsSet.size()) + " unique terms for document '" + Doc.ID + "' (length: " + std::to_string(DocLength) + ").");
     }

     MarkCollectionDirtyLocked(Collection);

     if ((DocCounts[Collection] % INVERTED_INDEX_MEMORY_CHECK_INTERVAL) == 0)
     {
          /* Periodic eviction keeps loaded clean snapshots bounded during bulk indexing. */

          EvictLoadedCollectionsLocked(INVERTED_INDEX_MAX_MEMORY_BYTES);
     }

     return true;
}

/*
 * InvertedIndex::DeleteDocument - Removes a document by ID while keeping collection statistics consistent.
 */

bool InvertedIndex::DeleteDocument(const std::string &Collection, const std::string &DocID)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "DeleteDocument: Removing document '" + DocID + "' from collection '" + Collection + "'.");
     }

     RemoveDocumentFromIndex(Collection, DocID);

     MarkCollectionDirtyLocked(Collection);
     EvictLoadedCollectionsLocked(INVERTED_INDEX_MAX_MEMORY_BYTES);

     return true;
}

/*
 * InvertedIndex::UpdateDocument - Replaces an indexed document by removing the old postings and adding the new ones.
 */

bool InvertedIndex::UpdateDocument(const std::string &Collection, const Document &OldDoc, const Document &NewDoc)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "UpdateDocument: Updating document '" + OldDoc.ID + "' in collection '" + Collection + "'.");
     }

     RemoveDocumentFromIndex(Collection, OldDoc.ID);

     /* Update rebuilds postings from the replacement document after removing the previous contribution. */

     std::vector<std::string> TitleTerms = ExtractTerms(NewDoc.Title);

     std::vector<std::string> ContentTerms = ExtractTerms(NewDoc.Content);

     std::vector<std::string> IDTerms = ExtractTerms(NewDoc.ID);

     std::unordered_map<std::string, std::vector<std::string>> FieldTermsByName;
     FieldTermsByName["title"] = TitleTerms;
     FieldTermsByName["content"] = ContentTerms;
     FieldTermsByName["id"] = IDTerms;

     /* URL and field-class sets mirror AddDocument so replacements score the same way as inserts. */

     std::unordered_set<std::string> UrlTerms;
     std::unordered_set<std::string> UrlTldTerms;
     ExtractUrlTerms(NewDoc.Title, UrlTerms, UrlTldTerms);
     ExtractUrlTerms(NewDoc.Content, UrlTerms, UrlTldTerms);

     std::unordered_set<std::string> TitleLikeTerms;
     std::unordered_set<std::string> TagLikeTerms;
     std::unordered_set<std::string> UrlFieldTerms;

     double url_token_boost = 1.3;
     double url_tld_weight = 0.3;
     double title_like_boost = 1.6;
     double tag_like_boost = 1.2;

     if (Instance && Instance->Config)
     {
          url_token_boost = Instance->Config->GetUrlTokenBoost();
          url_tld_weight = Instance->Config->GetUrlTldWeight();
          title_like_boost = Instance->Config->GetTitleLikeBoost();
          tag_like_boost = Instance->Config->GetTagLikeBoost();
     }

     std::vector<std::string> AllTermsList;

     std::set<std::string> AllTermsSet;

     /* Keep both ordered terms for positions and unique terms for posting creation. */

     for (const auto &Term : TitleTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     for (const auto &Term : ContentTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     for (const auto &Term : IDTerms)
     {
          AllTermsList.push_back(Term);
          AllTermsSet.insert(Term);
     }

     for (const auto &Field : NewDoc.Fields)
     {
          std::vector<std::string> FieldTerms = ExtractTerms(Field.second);
          FieldTermsByName[Field.first] = FieldTerms;

          /* Custom fields use naming heuristics for boosts while still being indexed generically. */

          bool is_title_like = FieldNameHasToken(Field.first, {"title", "name", "subject", "headline"});
          bool is_tag_like = FieldNameHasToken(Field.first, {"tag", "tags", "keyword", "keywords", "category", "topic"});
          bool is_url_like = FieldNameHasToken(Field.first, {"url", "uri", "link", "website", "site", "source"});

          for (const auto &Term : FieldTerms)
          {
               AllTermsList.push_back(Term);
               AllTermsSet.insert(Term);

               if (is_title_like)
               {
                    TitleLikeTerms.insert(Term);
               }

               if (is_tag_like)
               {
                    TagLikeTerms.insert(Term);
               }

               if (is_url_like)
               {
                    UrlFieldTerms.insert(Term);
               }
          }

          ExtractUrlTerms(Field.second, UrlTerms, UrlTldTerms);
     }

     auto &CollectionIndex = Index[Collection];

     auto &DocTerms = DocumentTerms[Collection][NewDoc.ID];

     DocTerms.clear();

     size_t DocLength = AllTermsList.size();

     std::unordered_map<std::string, std::vector<size_t>> TermPositions;
     std::unordered_map<std::string, std::unordered_map<std::string, std::vector<size_t>>> FieldTermPositions;

     /* Term positions are recomputed from the replacement payload rather than inherited from the old document. */

     size_t Pos = 0;

     for (const auto &Term : AllTermsList)
     {
          TermPositions[Term].push_back(Pos);
          Pos++;
     }

     for (const auto &[FieldName, FieldTerms] : FieldTermsByName)
     {
          std::size_t FieldPos = 0;

          for (const auto &FieldTerm : FieldTerms)
          {
               FieldTermPositions[FieldName][FieldTerm].push_back(FieldPos);
               FieldPos++;
          }
     }

     auto BuildPostingForTerm = [&](const std::string &Term,
                                    const std::vector<size_t> &Positions,
                                    bool InTitle) -> Posting
     {
          /* The posting boost is stored with the indexed term and consumed later by ranking. */

          Posting Post;

          Post.DocumentID = NewDoc.ID;
          Post.Collection = Collection;
          Post.Score = 1.0;
          Post.Positions = Positions;

          if (InTitle)
          {
               Post.Score = std::max(Post.Score, 2.0);
          }

          if (TitleLikeTerms.find(Term) != TitleLikeTerms.end())
          {
               Post.Score = std::max(Post.Score, title_like_boost);
          }

          if (TagLikeTerms.find(Term) != TagLikeTerms.end())
          {
               Post.Score *= tag_like_boost;
          }

          if (UrlTldTerms.find(Term) != UrlTldTerms.end())
          {
               Post.Score *= url_tld_weight;
          }
          else if (UrlTerms.find(Term) != UrlTerms.end() || UrlFieldTerms.find(Term) != UrlFieldTerms.end())
          {
               Post.Score *= url_token_boost;
          }

          return Post;
     };

     for (const auto &Term : AllTermsSet)
     {
          auto PosIt = TermPositions.find(Term);
          const std::vector<size_t> Positions = (PosIt != TermPositions.end()) ? PosIt->second : std::vector<size_t>{};
          const bool InTitle = std::find(TitleTerms.begin(), TitleTerms.end(), Term) != TitleTerms.end();
          Posting Post = BuildPostingForTerm(Term, Positions, InTitle);

          CollectionIndex[Term].push_back(Post);

          DocTerms.insert(Term);
     }

     for (const auto &[FieldName, FieldPositions] : FieldTermPositions)
     {
          const bool IsTitleField = (FieldName == "title");

          for (const auto &[Term, Positions] : FieldPositions)
          {
               /* Field-scoped replacements overwrite the old scoped terms because removal ran first. */

               const std::string ScopedTerm = BuildFieldScopedTerm(FieldName, Term);
               Posting ScopedPost = BuildPostingForTerm(Term, Positions, IsTitleField);
               CollectionIndex[ScopedTerm].push_back(ScopedPost);
               DocTerms.insert(ScopedTerm);
          }
     }

     const size_t CurrentTotalLength = EnsureCollectionTotalLengthLocked(Collection);
     DocumentLengths[Collection][NewDoc.ID] = DocLength;
     CollectionTotalLengths[Collection] = CurrentTotalLength + DocLength;
     RefreshCollectionStatsFromTotalLocked(Collection);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "UpdateDocument: Updated document '" + NewDoc.ID + "' with " + std::to_string(AllTermsSet.size()) + " unique terms (length: " + std::to_string(DocLength) + ").");
     }

     MarkCollectionDirtyLocked(Collection);
     EvictLoadedCollectionsLocked(INVERTED_INDEX_MAX_MEMORY_BYTES);

     return true;
}

/*
 * DeleteCollection - Deletes an entire collection from the index.
 */

/* InvertedIndex::DeleteCollection - Deletes all data for a collection. */

void InvertedIndex::DeleteCollection(const std::string &Collection)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Remove every in-memory structure that can hold collection-specific state. */

     Index.erase(Collection);

     DocumentTerms.erase(Collection);

     DocumentLengths.erase(Collection);

     CollectionTotalLengths.erase(Collection);

     DocCounts.erase(Collection);

     AvgDocLengths.erase(Collection);

     MMapIndexes.erase(Collection);

     DirtyCollections.erase(Collection);

     CollectionLastMutation.erase(Collection);

     CollectionLastFlush.erase(Collection);

     CollectionLastAccess.erase(Collection);

     if (Instance && Instance->Database)
     {
          /* Clear any stale interrupted-flush marker for the deleted collection. */

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

/* InvertedIndex::Clear - Clears all collections and caches. */

void InvertedIndex::Clear()
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Clear resets loaded index state but leaves persisted index directories untouched. */

     Index.clear();

     DocumentTerms.clear();

     DocumentLengths.clear();

     CollectionTotalLengths.clear();

     DocCounts.clear();

     AvgDocLengths.clear();

     MMapIndexes.clear();

     DirtyCollections.clear();

     CollectionLastMutation.clear();

     CollectionLastFlush.clear();

     CollectionLastAccess.clear();
}

/*
 * InvalidateDocumentCache - Invalidates the cache for a specific document.
 */

/* InvertedIndex::InvalidateDocumentCache - Invalidates document cache entry. */

void InvertedIndex::InvalidateDocumentCache(const std::string &Collection, const std::string &DocID)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* This hook currently reports cache invalidation without mutating index postings. */

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

/* InvertedIndex::InvalidateCollectionCache - Invalidates collection cache. */

void InvertedIndex::InvalidateCollectionCache(const std::string &Collection)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Collection cache invalidation is logged for callers that coordinate external caches. */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "InvalidateCollectionCache: Invalidated cache for collection '" + Collection + "'.");
     }
}

/*
 * GetTermCount - Returns the number of terms in a collection.
 */

/* InvertedIndex::GetTermCount - Returns term count for collection. */

size_t InvertedIndex::GetTermCount(const std::string &Collection) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Term count reflects the resident in-memory term map. */

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

/* InvertedIndex::GetDocumentCount - Returns document count for collection. */

size_t InvertedIndex::GetDocumentCount(const std::string &Collection) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Prefer in-memory document tracking, then fall back to the mapped index metadata. */

     auto It = DocumentTerms.find(Collection);

     if (It != DocumentTerms.end() && !It->second.empty())
     {
          return It->second.size();
     }

     auto MMapIt = MMapIndexes.find(Collection);

     if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
     {
          return MMapIt->second->GetDocumentCount();
     }

     return 0;
}

/*
 * HasInMemoryIndex - Returns true if the collection has documents indexed in memory.
 */

bool InvertedIndex::HasInMemoryIndex(const std::string &Collection) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* DocumentTerms is the fastest resident signal that a collection has mutable in-memory postings. */

     auto It = DocumentTerms.find(Collection);

     return It != DocumentTerms.end() && !It->second.empty();
}

/*
 * GetIndexDir - Returns the directory where indexes are stored.
 */

/* InvertedIndex::GetIndexDir - Returns index directory path. */

std::string InvertedIndex::GetIndexDir() const
{
     std::string BaseDir = std::string(HLQUERY_DATA_DIR);

     if (Instance && Instance->Config && Instance->Config->IsValid())
     {
          /* Index storage follows the configured RocksDB data directory when one is provided. */

          const auto &RocksDBOpts = Instance->Config->GetRocksDBOptions();

          if (!RocksDBOpts.DataDir.empty())
          {
               BaseDir = RocksDBOpts.DataDir;
          }
     }

     return BaseDir + "/storage/indices";
}

/*
 * FlushToDisk - Flushes in-memory indexes to disk.
 */

/* InvertedIndex::FlushToDisk - Writes index data to disk. */

size_t InvertedIndex::FlushToDisk(const std::string &IndexDir, uint64_t MinDirtyAgeSeconds, size_t MaxCollections)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("inverted_index", "FlushToDisk: Flushing indexes to disk.");
     }

     /* Flush work is snapshotted under lock and written outside the lock. */

     struct FlushWorkItem
     {
          std::string Collection;
          std::chrono::steady_clock::time_point MutationTime;
          std::unordered_map<std::string, std::vector<Posting>> IndexSnapshot;
     };

     std::vector<FlushWorkItem> WorkItems;

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          const std::vector<std::string> CollectionsToFlush = SelectFlushCollectionsLocked(MinDirtyAgeSeconds, MaxCollections);

          WorkItems.reserve(CollectionsToFlush.size());

          for (const auto &Collection : CollectionsToFlush)
          {
               /* Snapshot only collections that still have resident postings when the flush begins. */

               auto IndexIt = Index.find(Collection);

               if (IndexIt == Index.end() || IndexIt->second.empty())
               {
                    DirtyCollections.erase(Collection);
                    continue;
               }

               auto ExistingMMapIt = MMapIndexes.find(Collection);

               /* Keep partial in-memory mutations from replacing a previously loaded mmap snapshot. */

               if (ExistingMMapIt != MMapIndexes.end() &&
                   ExistingMMapIt->second &&
                   ExistingMMapIt->second->IsValid() &&
                   CollectionLastFlush.find(Collection) == CollectionLastFlush.end())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("inverted_index", "FlushToDisk: Deferred collection '" + Collection + "' because it has a loaded mmap snapshot and only partial in-memory mutations; segment merge or rebuild is required before overwriting the snapshot.");
                    }

                    continue;
               }

               auto MutationIt = CollectionLastMutation.find(Collection);
               const auto MutationTime = (MutationIt != CollectionLastMutation.end())
                                              ? MutationIt->second
                                              : std::chrono::steady_clock::time_point{};

               WorkItems.push_back({Collection, MutationTime, IndexIt->second});
          }
     }

     size_t FlushedCollections = 0;

     for (const auto &WorkItem : WorkItems)
     {
          const std::string FlushMarker = "flush_pending:" + WorkItem.Collection;

          if (Instance && Instance->Database)
          {
               /* Persist the marker before writing files so startup can detect interrupted writes. */

               Instance->Database->Set(FlushMarker, "1");
          }

          /* Each collection is serialized independently to keep flush failure isolated. */

          IndexWriter CollectionWriter(IndexDir, WorkItem.Collection);
          std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Posting>>> SingleCollection;
          SingleCollection[WorkItem.Collection] = WorkItem.IndexSnapshot;

          if (!CollectionWriter.WriteIndex(SingleCollection))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("inverted_index", "FlushToDisk: Failed to flush collection '" + WorkItem.Collection + "'.");
               }

               continue;
          }

          auto MMapIdx = MMapIndex::Open(IndexDir, WorkItem.Collection);

          {
               std::lock_guard<std::mutex> Lock(IndexMutex);

               /* Install the new mmap view only after the writer has produced a valid index. */

               if (MMapIdx && MMapIdx->IsValid())
               {
                    MMapIndexes[WorkItem.Collection] = std::move(MMapIdx);
               }

               auto MutationIt = CollectionLastMutation.find(WorkItem.Collection);
               const bool HasNewMutation = MutationIt != CollectionLastMutation.end() &&
                                           MutationIt->second > WorkItem.MutationTime;

               /* A collection remains dirty when new writes arrived after the snapshot was taken. */

               if (!HasNewMutation)
               {
                    DirtyCollections.erase(WorkItem.Collection);
               }

               CollectionLastFlush[WorkItem.Collection] = std::chrono::steady_clock::now();
          }

          if (Instance && Instance->Database)
          {
               Instance->Database->Del(FlushMarker);
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "FlushToDisk: Flushed collection '" + WorkItem.Collection + "' to disk.");
          }

          FlushedCollections++;
     }

     if (Instance && Instance->Database && FlushedCollections > 0)
     {
          /* Sync after successful collection writes so flush markers and removals are durable. */

          Instance->Database->FlushAndSync();
     }

     if (FlushedCollections > 0)
     {
          EvictLoadedCollectionsIfNeeded(INVERTED_INDEX_MAX_MEMORY_BYTES);
     }

     return FlushedCollections;
}

/*
 * LoadFromDisk - Loads indexes from disk into memory.
 */

/* InvertedIndex::LoadFromDisk - Loads index data from disk. */

void InvertedIndex::LoadFromDisk(const std::string &IndexDir)
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("inverted_index", "LoadFromDisk: Loading indexes from disk.");
     }

     /* Resolve every persisted interrupted-flush marker before scanning index
      * directories. This also handles the case where a prior recovery already
      * removed the directory but crashed before deleting the marker. RocksDB
      * remains the source of truth and the lexical index is rebuilt lazily. */

     if (Instance && Instance->Database)
     {
          bool RepairedAny = false;
          const std::string MarkerPrefix = "flush_pending:";
          const std::vector<std::string> PendingMarkers = Instance->Database->PrefixKeys(MarkerPrefix, 0, 0);

          for (const auto &Marker : PendingMarkers)
          {
               if (Marker.rfind(MarkerPrefix, 0) != 0)
               {
                    continue;
               }

               const std::string Collection = Marker.substr(MarkerPrefix.size());
               const bool SafeCollection = !Collection.empty() &&
                                           std::all_of(Collection.begin(), Collection.end(), [](unsigned char Ch)
                                                       {
                                                            return std::isalnum(Ch) || Ch == '_' || Ch == '-' || Ch == '.';
                                                       });

               if (!SafeCollection)
               {
                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("inverted_index", "LoadFromDisk: Refusing unsafe interrupted-flush marker key '" + Marker + "'.");
                    }
                    continue;
               }

               const std::filesystem::path CollectionDir = std::filesystem::path(IndexDir) / Collection;
               std::error_code RemoveError;
               std::filesystem::remove_all(CollectionDir, RemoveError);

               if (RemoveError)
               {
                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("inverted_index", "LoadFromDisk: Could not remove interrupted index for collection '" + Collection + "': " + RemoveError.message() + ".");
                    }
                    continue;
               }

               if (Instance->Database->Del(Marker) >= 0)
               {
                    RepairedAny = true;

                    if (Instance->Logs)
                    {
                         Instance->Logs->Normal("inverted_index", "LoadFromDisk: Repaired interrupted flush for collection '" + Collection + "'; the lexical index will rebuild from RocksDB on demand.");
                    }
               }
          }

          if (RepairedAny)
          {
               Instance->Database->FlushAndSync();
          }
     }

     if (!std::filesystem::exists(IndexDir))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "LoadFromDisk: Index directory does not exist: " + IndexDir + ".");
          }

          return;
     }

     std::vector<std::pair<std::string, std::unique_ptr<MMapIndex>>> LoadedIndexes;

     /* Validate all directories before taking the index mutex. */

     for (const auto &Entry : std::filesystem::directory_iterator(IndexDir))
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
                    /* Interrupted flushes are repaired by removing the possibly incomplete collection index. */

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
               /* Loaded indexes are installed after the scan so mmap open work does not hold the mutex. */

               LoadedIndexes.emplace_back(Collection, std::move(MMapIdx));
          }
     }

     std::lock_guard<std::mutex> Lock(IndexMutex);

     for (auto &Entry : LoadedIndexes)
     {
          /* Startup residency is not user access; actual reads update recency before eviction. */

          const std::string &Collection = Entry.first;
          const size_t TermCount = Entry.second->GetTermCount();

          MMapIndexes[Collection] = std::move(Entry.second);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "LoadFromDisk: Loaded mmap index for collection '" + Collection + "' (" + std::to_string(TermCount) + " terms).");
          }
     }

     EvictLoadedCollectionsLocked(INVERTED_INDEX_MAX_MEMORY_BYTES);
}

/*
 * HasMMapIndex - Checks if a collection has an associated mmap index.
 */

/* InvertedIndex::HasMMapIndex - Returns whether memory-mapped index exists. */

bool InvertedIndex::HasMMapIndex(const std::string &Collection) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* A mapped index must be present and valid before callers use it for disk-backed search. */

     auto It = MMapIndexes.find(Collection);

     return It != MMapIndexes.end() && It->second && It->second->IsValid();
}

/*
 * CalculateBM25PlusScore - Calculates the BM25+ score for a term in a document.
 */

/* InvertedIndex::CalculateBM25PlusScore - Computes BM25+ score. */

double InvertedIndex::CalculateBM25PlusScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1, double B, double Delta) const
{
     if (!std::isfinite(TermFreq) || !std::isfinite(DocFreq) ||
         !std::isfinite(DocLength) || !std::isfinite(AvgDocLength) ||
         !std::isfinite(CollectionSize) || !std::isfinite(K1) ||
         !std::isfinite(B) || !std::isfinite(Delta) ||
         TermFreq <= 0.0 || DocFreq <= 0.0 || CollectionSize <= 0.0 ||
         DocFreq > CollectionSize || DocLength <= 0.0 || AvgDocLength <= 0.0 ||
         K1 < 0.0 || B < 0.0 || B > 1.0 || Delta < 0.0)
     {
          /* Invalid scoring inputs return a neutral score instead of propagating NaN or infinity. */

          return 0.0;
     }

     std::string IdfMode = "legacy";
     bool ClampNegative = true;
     double IdfSmoothValue = 1.0;
     double IdfFloorFactor = 0.05;

     if (Instance && Instance->Config)
     {
          /* IDF behavior is shared with the main Search path through runtime configuration. */

          IdfMode = Instance->Config->GetRankingIdfMode();
          ClampNegative = Instance->Config->GetRankingIdfClampNegative();
          IdfSmoothValue = std::max(0.0, Instance->Config->GetRankingIdfSmooth());
          IdfFloorFactor = std::max(0.0, Instance->Config->GetRankingIdfFloorFactor());
     }

     if (!std::isfinite(IdfSmoothValue))
     {
          IdfSmoothValue = 1.0;
     }

     if (!std::isfinite(IdfFloorFactor))
     {
          IdfFloorFactor = 0.05;
     }

     IdfFloorFactor = std::clamp(IdfFloorFactor, 0.0, 1.0);

     const double Idf = BM25Scoring::CalculateIdf(DocFreq, CollectionSize, IdfMode,
                                                   IdfSmoothValue, ClampNegative,
                                                   IdfFloorFactor);

     const double NormalizedLengthValue = std::max(1e-9, DocLength / AvgDocLength);
     const double NumeratorValue = TermFreq * (K1 + 1.0);
     const double DenominatorValue = TermFreq + K1 * (1.0 - B + B * NormalizedLengthValue);

     /* BM25+ adds Delta after frequency saturation to reduce long-document under-scoring. */

     if (DenominatorValue <= 0.0)
     {
          return 0.0;
     }

     const double SaturatedFrequency = NumeratorValue / DenominatorValue;
     const double ScoreValueResult = Idf * (SaturatedFrequency + Delta);
     return std::isfinite(ScoreValueResult) ? ScoreValueResult : 0.0;
}

/* InvertedIndex::CalculateTFIDFScore - Computes sublinear TF-IDF. */

double InvertedIndex::CalculateTFIDFScore(double TermFreq, double DocFreq, double DocLength, double CollectionSize, double IdfSmooth, bool Normalize) const
{
     if (!std::isfinite(TermFreq) || !std::isfinite(DocFreq) ||
         !std::isfinite(DocLength) || !std::isfinite(CollectionSize) ||
         !std::isfinite(IdfSmooth) || TermFreq <= 0.0 || DocFreq <= 0.0 ||
         DocLength <= 0.0 || CollectionSize <= 0.0 || DocFreq > CollectionSize ||
         IdfSmooth < 0.0)
     {
          /* Reject invalid TF-IDF inputs before logarithms or square roots are evaluated. */

          return 0.0;
     }

     const double IdfDenominator = DocFreq + IdfSmooth;
     const double IdfNumerator = CollectionSize + IdfSmooth;

     /* Smoothed IDF keeps rare terms strong while avoiding division by zero. */

     if (IdfDenominator <= 0.0 || IdfNumerator <= 0.0)
     {
          return 0.0;
     }

     double TermWeight = 1.0 + std::log(TermFreq);
     if (Normalize)
     {
          /* Optional normalization reduces the advantage of long documents with repeated terms. */

          TermWeight /= std::sqrt(DocLength);
     }

     const double Idf = 1.0 + std::log(IdfNumerator / IdfDenominator);
     const double Score = TermWeight * Idf;
     return std::isfinite(Score) && Score > 0.0 ? Score : 0.0;
}

/*
 * CalculateBM25LScore - Calculates the BM25L score for a term in a document.
 */

/* InvertedIndex::CalculateBM25LScore - Computes BM25L score. */

double InvertedIndex::CalculateBM25LScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double K1, double B, double Delta) const
{
     if (CollectionSize <= 0 || DocFreq <= 0 || DocLength <= 0)
     {
          /* BM25L has no meaningful score without collection, frequency, and length data. */

          return 0.0;
     }

     std::string IdfMode = "legacy";
     bool ClampNegative = true;
     double IdfSmoothValue = 1.0;
     double IdfFloorFactor = 0.05;

     if (Instance && Instance->Config)
     {
          /* Keep standalone BM25L scoring aligned with configured IDF behavior. */

          IdfMode = Instance->Config->GetRankingIdfMode();
          ClampNegative = Instance->Config->GetRankingIdfClampNegative();
          IdfSmoothValue = std::max(0.0, Instance->Config->GetRankingIdfSmooth());
          IdfFloorFactor = std::max(0.0, Instance->Config->GetRankingIdfFloorFactor());
     }

     double Idf = 0.0;

     if (IdfMode == "smooth")
     {
          /* Smooth IDF avoids negative values for very frequent terms. */

          double Denominator = DocFreq + 0.5 + IdfSmoothValue;
          if (Denominator <= 0.0)
          {
               return 0.0;
          }

          double Ratio = (CollectionSize - DocFreq + 0.5) / Denominator;
          Idf = std::log1p(std::max(0.0, Ratio));
     }
     else
     {
          /* Legacy IDF can be floored when clamping is enabled. */

          double Denominator = DocFreq + 0.5;
          if (Denominator <= 0.0)
          {
               return 0.0;
          }

          Idf = std::log((CollectionSize - DocFreq + 0.5) / Denominator);

          if (ClampNegative && Idf < 0.0)
          {
               Idf = IdfFloorFactor * std::log1p(CollectionSize / DocFreq);
          }
     }

     double NormalizedLengthValue = DocLength / std::max(AvgDocLength, 1.0);

     double NormFactorValue = 1.0 - B + B * NormalizedLengthValue;

     NormFactorValue = std::max(NormFactorValue, 1.0);

     /* BM25L shifts term frequency by Delta before saturation. */

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

/* InvertedIndex::CalculatePivotNormScore - Computes pivot normalization score. */

double InvertedIndex::CalculatePivotNormScore(double TermFreq, double DocFreq, double DocLength, double AvgDocLength, double CollectionSize, double Pivot) const
{
     if (CollectionSize <= 0 || DocFreq <= 0 || DocLength <= 0 || TermFreq <= 0)
     {
          /* Pivot normalization needs positive statistics to produce a useful score. */

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

/* InvertedIndex::CalculateTermFrequency - Calculates term frequency in doc. */

size_t InvertedIndex::CalculateTermFrequency(const std::string &Collection, const std::string &DocID, const std::string &Term) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Term frequency is read from resident postings and does not consult mapped indexes. */

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

     for (const auto &Post : TermIt->second)
     {
          if (Post.DocumentID == DocID)
          {
               /* Position count is the term frequency; missing positions fall back to one observed hit. */

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

/* InvertedIndex::CalculateDocumentFrequency - Calculates document frequency for term. */

size_t InvertedIndex::CalculateDocumentFrequency(const std::string &Collection, const std::string &Term) const
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Count unique document IDs because duplicate postings can occur during merged in-memory updates. */

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

     for (const auto &Post : TermIt->second)
     {
          UniqueDocs.insert(Post.DocumentID);
     }

     return UniqueDocs.size();
}

/*
 * UpdateCollectionStatistics - Updates average document length and count for a collection.
 */

/* InvertedIndex::UpdateCollectionStatistics - Refreshes collection stats. */

void InvertedIndex::UpdateCollectionStatistics(const std::string &Collection)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

     /* Statistics are rebuilt from document lengths so cached totals cannot drift indefinitely. */

     auto DocLengthsIt = DocumentLengths.find(Collection);

     if (DocLengthsIt == DocumentLengths.end() || DocLengthsIt->second.empty())
     {
          /* Empty collections keep a neutral average length for scoring callers. */

          AvgDocLengths[Collection] = 1.0;
          DocCounts[Collection] = 0;
          CollectionTotalLengths[Collection] = 0;

          return;
     }

     CollectionTotalLengths.erase(Collection);
     EnsureCollectionTotalLengthLocked(Collection);
     RefreshCollectionStatsFromTotalLocked(Collection);
}

/*
 * CalculateProximityBoost - Calculates a score boost based on how close query terms are in a document.
 */

/* InvertedIndex::CalculateProximityBoost - Computes proximity boost for doc. */

double InvertedIndex::CalculateProximityBoost(const std::vector<std::string> &QueryTerms, const std::vector<Posting> &Postings, const std::string &DocID) const
{
     if (QueryTerms.size() < 2)
     {
          /* Single-term queries cannot benefit from proximity. */

          return 1.0;
     }

     double boost_scale = 1.0;
     double boost_max = 2.0;

     if (Instance && Instance->Config)
     {
          /* Proximity strength and maximum boost are runtime-tunable. */

          boost_scale = Instance->Config->GetProximityBoostScale();
          boost_max = Instance->Config->GetProximityBoostMax();
     }

     if (!std::isfinite(boost_scale) || boost_scale < 0.0)
     {
          boost_scale = 1.0;
     }

     if (!std::isfinite(boost_max) || boost_max < 1.0)
     {
          boost_max = 1.0;
     }

     std::vector<Posting> DocPostings;

     /* Keep only postings for the candidate document that have recorded positions. */

     for (const auto &Post : Postings)
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

     /* Compare every pair of matched term positions and keep the closest observed distance. */

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
     BoostValueResult = 1.0 + ((BoostValueResult - 1.0) * boost_scale);

     /* Clamp the boost so proximity cannot overwhelm the base rank. */

     return std::min(BoostValueResult, boost_max);
}
