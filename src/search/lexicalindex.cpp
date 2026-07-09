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
#include "search/cstore.h"
#include "search/lindex.h"
#include "search/mindex.h"
#include "search/storageengine.h"
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

/* AddQueryTermVariants - Adds conservative singular/plural variants for lexical lookup. */

static void AddQueryTermVariants(const std::string &term, std::vector<std::pair<std::string, double>> &variants)
{
     if (term.empty())
     {
          return;
     }

     auto AddVariant = [&variants](const std::string &value, double weight)
     {
          if (value.empty())
          {
               return;
          }

          for (const auto &Existing : variants)
          {
               if (Existing.first == value)
               {
                    return;
               }
          }

          variants.push_back({value, weight});
     };

     AddVariant(term, 1.0);

     if (term.size() <= 3 || term.find('*') != std::string::npos || term.find('?') != std::string::npos)
     {
          return;
     }

     const char Last = term.back();

     if (term.size() > 4 && term.substr(term.size() - 3) == "ies")
     {
          AddVariant(term.substr(0, term.size() - 3) + "y", 0.92);
     }
     else if (Last == 'y')
     {
          const char BeforeLast = term[term.size() - 2];
          if (std::string("aeiou").find(BeforeLast) == std::string::npos)
          {
               AddVariant(term.substr(0, term.size() - 1) + "ies", 0.88);
          }
          else
          {
               AddVariant(term + "s", 0.86);
          }
     }
     else if (term.size() > 4 && (term.substr(term.size() - 2) == "es"))
     {
          const std::string Stem = term.substr(0, term.size() - 2);
          if (!Stem.empty() && (Stem.back() == 's' || Stem.back() == 'x' || Stem.back() == 'z' ||
                                Stem.back() == 'h'))
          {
               AddVariant(Stem, 0.86);
          }
     }
     else if (Last == 's' && term.size() > 4 && term.substr(term.size() - 2) != "ss")
     {
          AddVariant(term.substr(0, term.size() - 1), 0.88);
     }
     else
     {
          AddVariant(term + "s", 0.84);
          AddVariant(term + "es", 0.80);
     }
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
     auto TotalIt = CollectionTotalLengths.find(Collection);

     if (TotalIt != CollectionTotalLengths.end())
     {
          return TotalIt->second;
     }

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
     auto LengthsIt = DocumentLengths.find(Collection);
     const size_t DocCountValue = (LengthsIt != DocumentLengths.end()) ? LengthsIt->second.size() : 0;

     DocCounts[Collection] = DocCountValue;

     if (DocCountValue == 0)
     {
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
     size_t Bytes = 0;

     auto IndexIt = Index.find(Collection);
     if (IndexIt != Index.end())
     {
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
          Bytes += sizeof(MMapIndex) + MMapIt->second->GetMappedSize();
     }

     return Bytes;
}

size_t InvertedIndex::EstimateLoadedMemoryLocked() const
{
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

     for (const auto &Collection : DirtyCollections)
     {
          auto IndexIt = Index.find(Collection);

          if (IndexIt == Index.end() || IndexIt->second.empty())
          {
               continue;
          }

          if (MinDirtyAgeSeconds > 0)
          {
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
          DirtyCollections.erase(Collection);
          return false;
     }

     auto ExistingMMapIt = MMapIndexes.find(Collection);

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
          Instance->Database->Set(FlushMarker, "1");
     }

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

     std::string lower = tail;
     std::transform(lower.begin(), lower.end(), lower.begin(), ToLowerAsciiSafe);

     if (lower.size() < 2 || lower.size() > 63)
     {
          return false;
     }

     if (lower.rfind("xn--", 0) == 0)
     {
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
          host = host.substr(0, port_pos);
     }

     return host;
}

/* TrimUrlToken - Trims punctuation around URL tokens. */

static std::string TrimUrlToken(const std::string &token)
{
     size_t start = 0;
     size_t end = token.size();

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
          url_tlds.insert(tld);
          host_parts.pop_back();
     }

     for (const auto &hp : host_parts)
     {
          std::string normalized = NormalizeUrlToken(hp);
          if (!normalized.empty())
          {
               url_terms.insert(normalized);
          }
     }

     if (end_host != std::string::npos && end_host + 1 < lower.size())
     {
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

     size_t Pos = 0;

     for (const auto &T : AllTermsList)
     {
          TermPositions[T].push_back(Pos);

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

          CollectionIndex[Term].push_back(Post);

          DocTerms.insert(Term);
     }

     for (const auto &[FieldName, FieldPositions] : FieldTermPositions)
     {
          const bool IsTitleField = (FieldName == "title");

          for (const auto &[Term, Positions] : FieldPositions)
          {
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

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "AddDocument: Indexed " + std::to_string(AllTermsSet.size()) + " unique terms for document '" + Doc.ID + "' (length: " + std::to_string(DocLength) + ").");
     }

     MarkCollectionDirtyLocked(Collection);

     if ((DocCounts[Collection] % INVERTED_INDEX_MEMORY_CHECK_INTERVAL) == 0)
     {
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

     std::vector<std::string> TitleTerms = ExtractTerms(NewDoc.Title);

     std::vector<std::string> ContentTerms = ExtractTerms(NewDoc.Content);

     std::vector<std::string> IDTerms = ExtractTerms(NewDoc.ID);

     std::unordered_map<std::string, std::vector<std::string>> FieldTermsByName;
     FieldTermsByName["title"] = TitleTerms;
     FieldTermsByName["content"] = ContentTerms;
     FieldTermsByName["id"] = IDTerms;

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
 * InvertedIndex::SearchTerm - Resolves a normalized term from mmap and in-memory indexes.
 */

std::vector<Posting> InvertedIndex::SearchTerm(const std::string &Collection, const std::string &Term)
{
     std::string NormalizedTerm = NormalizeTerm(Term);
     std::unordered_map<std::string, Posting> TermDocs;

     std::lock_guard<std::mutex> Lock(IndexMutex);
     TouchCollectionLocked(Collection);

     auto MMapIt = MMapIndexes.find(Collection);

     if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid() && MMapIt->second->GetTermCount() > 0)
     {
          auto Results = MMapIt->second->SearchTerm(NormalizedTerm);

          for (auto &Post : Results)
          {
               Post.Collection = Collection;
               TermDocs[Post.DocumentID] = Post;
          }
     }

     auto CollectionIt = Index.find(Collection);

     if (CollectionIt != Index.end() && !CollectionIt->second.empty())
     {
          auto TermIt = CollectionIt->second.find(NormalizedTerm);

          if (TermIt != CollectionIt->second.end())
          {
               for (const auto &Post : TermIt->second)
               {
                    auto ExistingIt = TermDocs.find(Post.DocumentID);

                    if (ExistingIt == TermDocs.end())
                    {
                         TermDocs[Post.DocumentID] = Post;
                    }
                    else
                    {
                         ExistingIt->second.Score += Post.Score;
                    }
               }
          }
     }

     std::vector<Posting> Results;
     Results.reserve(TermDocs.size());

     for (auto &Pair : TermDocs)
     {
          Results.push_back(Pair.second);
     }

     return Results;
}

/*
 * InvertedIndex::Search - Parses the query, scores matches, and merges in-memory and mmap-backed results.
 */

std::vector<Posting> InvertedIndex::Search(const std::string &Collection, const std::string &Query, int Limit, const std::vector<std::string> &QueryFields)
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
     std::vector<std::string> NegativeTerms;

     {
          std::istringstream QueryStream(Query);
          std::string TokenValue;
          bool NextTokenIsNegative = false;

          while (QueryStream >> TokenValue)
          {
               std::string LowerToken = TokenValue;
               std::transform(LowerToken.begin(), LowerToken.end(), LowerToken.begin(), ToLowerAsciiSafe);

               if (LowerToken == "not")
               {
                    NextTokenIsNegative = true;
                    continue;
               }

               bool IsNegativeToken = NextTokenIsNegative;
               NextTokenIsNegative = false;

               if (!TokenValue.empty() && TokenValue.front() == '!')
               {
                    IsNegativeToken = true;
                    TokenValue.erase(0, 1);
               }

               if (IsNegativeToken)
               {
                    std::string NormalizedNegative = NormalizeTerm(TokenValue);

                    if (!NormalizedNegative.empty())
                    {
                         NegativeTerms.push_back(NormalizedNegative);
                    }
               }
          }
     }

     if (!NegativeTerms.empty())
     {
          std::unordered_set<std::string> NegativeTermSet(NegativeTerms.begin(), NegativeTerms.end());

          QueryTerms.erase(std::remove_if(QueryTerms.begin(), QueryTerms.end(), [&NegativeTermSet](const std::string &TermValue)
                         {
                              return TermValue == "not" || NegativeTermSet.find(TermValue) != NegativeTermSet.end();
                         }),
                         QueryTerms.end());
     }

     {
          std::unordered_set<std::string> SeenQueryTerms;
          std::vector<std::string> UniqueQueryTerms;
          UniqueQueryTerms.reserve(QueryTerms.size());

          for (const auto &TermValue : QueryTerms)
          {
               if (TermValue.empty() || !SeenQueryTerms.insert(TermValue).second)
               {
                    continue;
               }

               UniqueQueryTerms.push_back(TermValue);
          }

          QueryTerms = std::move(UniqueQueryTerms);
     }

     if (!NegativeTerms.empty())
     {
          std::unordered_set<std::string> SeenNegativeTerms;
          std::vector<std::string> UniqueNegativeTerms;
          UniqueNegativeTerms.reserve(NegativeTerms.size());

          for (const auto &TermValue : NegativeTerms)
          {
               if (TermValue.empty() || !SeenNegativeTerms.insert(TermValue).second)
               {
                    continue;
               }

               UniqueNegativeTerms.push_back(TermValue);
          }

          NegativeTerms = std::move(UniqueNegativeTerms);
     }

     if (Instance && Instance->Config && Instance->Config->GetQuerySettingsMaxQueryTerms() > 0)
     {
          const size_t MaxQueryTerms = static_cast<size_t>(Instance->Config->GetQuerySettingsMaxQueryTerms());
          if (QueryTerms.size() > MaxQueryTerms)
          {
               QueryTerms.resize(MaxQueryTerms);
          }
     }

     if (QueryTerms.empty() && NegativeTerms.empty())
     {
          return {};
     }

     bool UseMMap = false;

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          TouchCollectionLocked(Collection);

          auto MMapIt = MMapIndexes.find(Collection);

          if (MMapIt != MMapIndexes.end() && MMapIt->second && MMapIt->second->IsValid())
          {
               if (MMapIt->second->GetTermCount() > 0)
               {
                    UseMMap = true;
               }
          }
     }

     std::vector<std::unordered_map<std::string, Posting>> TermResults;
     std::unordered_set<std::string> NegativeDocIDs;
     size_t ValidQueryTermCount = 0;

     auto IsWildcardTerm = [](const std::string &term) -> bool
     {
          return term.find('*') != std::string::npos || term.find('?') != std::string::npos;
     };

     auto IsPrefixWildcardTerm = [](const std::string &term) -> bool
     {
          size_t StarPos = term.find('*');

          if (StarPos == std::string::npos || StarPos != term.size() - 1)
          {
               return false;
          }

          return term.find('?') == std::string::npos && term.find('*', StarPos + 1) == std::string::npos;
     };

     auto BuildScopedKeys = [&QueryFields](const std::string &normalized_term) -> std::vector<std::string>
     {
          if (QueryFields.empty())
          {
               return {normalized_term};
          }

          std::vector<std::string> ScopedKeys;
          ScopedKeys.reserve(QueryFields.size());

          for (const auto &FieldName : QueryFields)
          {
               if (!FieldName.empty())
               {
                    ScopedKeys.push_back(BuildFieldScopedTerm(FieldName, normalized_term));
               }
          }

          return ScopedKeys;
     };

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          TouchCollectionLocked(Collection);
          auto CollectionIt = Index.find(Collection);
          auto MMapIt = MMapIndexes.find(Collection);
          const bool HasMMapIndex = MMapIt != MMapIndexes.end() &&
                                    MMapIt->second &&
                                    MMapIt->second->IsValid() &&
                                    MMapIt->second->GetTermCount() > 0;
          const bool HasMemoryIndex = CollectionIt != Index.end() && !CollectionIt->second.empty();

          auto LoadTermDocs = [&](const std::string &Normalized, std::unordered_map<std::string, Posting> &TermDocs)
          {
               std::vector<std::pair<std::string, double>> TermVariants;
               AddQueryTermVariants(Normalized, TermVariants);

               if (HasMMapIndex)
               {
                    for (const auto &[VariantTerm, VariantWeight] : TermVariants)
                    {
                         const std::vector<std::string> ScopedKeys = BuildScopedKeys(VariantTerm);

                         for (const auto &ScopedKey : ScopedKeys)
                         {
                              std::vector<Posting> ScopedPostings;

                              if (IsWildcardTerm(VariantTerm))
                              {
                                   if (IsPrefixWildcardTerm(VariantTerm) && ScopedKey.size() > 1)
                                   {
                                        std::string Prefix = ScopedKey.substr(0, ScopedKey.size() - 1);
                                        ScopedPostings = MMapIt->second->SearchPrefix(Prefix, 0);
                                   }
                                   else
                                   {
                                        ScopedPostings = MMapIt->second->SearchWildcard(ScopedKey, 0);
                                   }
                              }
                              else
                              {
                                   ScopedPostings = MMapIt->second->SearchTerm(ScopedKey);
                              }

                              for (auto &ScopedPost : ScopedPostings)
                              {
                                   ScopedPost.Score *= VariantWeight;

                                   auto It = TermDocs.find(ScopedPost.DocumentID);
                                   if (It == TermDocs.end())
                                   {
                                        TermDocs[ScopedPost.DocumentID] = ScopedPost;
                                        TermDocs[ScopedPost.DocumentID].Collection = Collection;
                                   }
                                   else
                                   {
                                        It->second.Score += ScopedPost.Score;
                                   }
                              }
                         }
                    }
               }

               if (HasMemoryIndex)
               {
                    for (const auto &[VariantTerm, VariantWeight] : TermVariants)
                    {
                         const std::vector<std::string> ScopedKeys = BuildScopedKeys(VariantTerm);

                         if (IsWildcardTerm(VariantTerm))
                         {
                              for (const auto &ScopedKey : ScopedKeys)
                              {
                                   for (const auto &[IndexedTerm, MemoryPostings] : CollectionIt->second)
                                   {
                                        if (!Wildcard::Match(IndexedTerm, ScopedKey))
                                        {
                                             continue;
                                        }

                                        for (const auto &Post : MemoryPostings)
                                        {
                                             Posting WeightedPost = Post;
                                             WeightedPost.Score *= VariantWeight;

                                             auto It = TermDocs.find(WeightedPost.DocumentID);
                                             if (It == TermDocs.end())
                                             {
                                                  TermDocs[WeightedPost.DocumentID] = WeightedPost;
                                             }
                                             else
                                             {
                                                  It->second.Score += WeightedPost.Score;
                                             }
                                        }
                                   }
                              }
                         }
                         else
                         {
                              for (const auto &ScopedKey : ScopedKeys)
                              {
                                   auto TermIt = CollectionIt->second.find(ScopedKey);

                                   if (TermIt == CollectionIt->second.end())
                                   {
                                        continue;
                                   }

                                   for (const auto &Post : TermIt->second)
                                   {
                                        Posting WeightedPost = Post;
                                        WeightedPost.Score *= VariantWeight;

                                        auto It = TermDocs.find(WeightedPost.DocumentID);
                                        if (It == TermDocs.end())
                                        {
                                             TermDocs[WeightedPost.DocumentID] = WeightedPost;
                                        }
                                        else
                                        {
                                             It->second.Score += WeightedPost.Score;
                                        }
                                   }
                              }
                         }
                    }
               }
          };

          for (const auto &TermValue : NegativeTerms)
          {
               std::string Normalized = NormalizeTerm(TermValue);

               if (Normalized.empty())
               {
                    continue;
               }

               std::unordered_map<std::string, Posting> TermDocs;
               LoadTermDocs(Normalized, TermDocs);

               for (const auto &Pair : TermDocs)
               {
                    NegativeDocIDs.insert(Pair.first);
               }
          }

          for (const auto &TermValue : QueryTerms)
          {
               std::string Normalized = NormalizeTerm(TermValue);

               if (Normalized.empty())
               {
                    continue;
               }

               ValidQueryTermCount++;

               std::unordered_map<std::string, Posting> TermDocs;

               LoadTermDocs(Normalized, TermDocs);

               if (TermDocs.empty())
               {
                    if (Instance && Instance->Config && Instance->Config->GetSearchMatchMode() == "and")
                    {
                         if (Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("inverted_index", "Search: Term '" + Normalized + "' not found in index, returning empty results (AND logic).");
                         }

                         return {};
                    }

                    continue;
               }

               TermResults.push_back(TermDocs);
          }

          if (TermResults.empty() && !NegativeTerms.empty())
          {
               std::unordered_map<std::string, Posting> AllDocs;

               auto DocsIt = DocumentTerms.find(Collection);

               if (DocsIt != DocumentTerms.end())
               {
                    for (const auto &DocPair : DocsIt->second)
                    {
                         Posting Post;
                         Post.DocumentID = DocPair.first;
                         Post.Collection = Collection;
                         Post.Score = 1.0;
                         AllDocs[Post.DocumentID] = std::move(Post);
                    }
               }

               if (HasMemoryIndex)
               {
                    for (const auto &[IndexedTerm, MemoryPostings] : CollectionIt->second)
                    {
                         (void)IndexedTerm;

                         for (const auto &PostValue : MemoryPostings)
                         {
                              if (PostValue.DocumentID.empty())
                              {
                                   continue;
                              }

                              Posting Post = PostValue;
                              Post.Collection = Collection;
                              Post.Score = std::max(Post.Score, 1.0);
                              AllDocs.emplace(Post.DocumentID, std::move(Post));
                         }
                    }
               }

               if (!AllDocs.empty())
               {
                    TermResults.push_back(std::move(AllDocs));
               }
          }
     }

     if (TermResults.empty())
     {
          return {};
     }

     std::unordered_map<std::string, Posting> DocScores;
     std::unordered_map<std::string, size_t> DocMatchCounts;

     double K1 = 1.2;

     double B = 0.75;

     double Delta = 1.0;

     std::string SearchAlgorithm = "bm25+";

     double IdfSmooth = 1.0;
     
     bool NormalizeTFIDF = true;

     double BM25Weight = 0.7;

     double TFIDFWeight = 0.3;

     bool PivotEnabled = false;

     double PivotValue = 0.25;

     std::string MatchMode = "and";

     int MinShouldMatch = 1;

     int CandidatePruneMultiplier = 25;

     if (Instance && Instance->Config)
     {
          K1 = Instance->Config->GetRankingK1();

          B = Instance->Config->GetRankingB();

          Delta = Instance->Config->GetRankingDelta();

          SearchAlgorithm = Instance->Config->GetSearchAlgorithm();

          IdfSmooth = Instance->Config->GetRankingIdfSmooth();
          
          NormalizeTFIDF = Instance->Config->GetRankingNormalize();

          BM25Weight = Instance->Config->GetRankingBm25Weight();

          TFIDFWeight = Instance->Config->GetRankingTfidfWeight();

          PivotEnabled = Instance->Config->GetPivotNormEnabled();

          PivotValue = Instance->Config->GetPivotNormPivot();

          MatchMode = Instance->Config->GetSearchMatchMode();

          MinShouldMatch = Instance->Config->GetSearchMinShouldMatch();

          CandidatePruneMultiplier = Instance->Config->GetSearchCandidatePruneMultiplier();
     }

     if (!std::isfinite(K1) || K1 < 0.0)
     {
          K1 = 1.2;
     }

     if (!std::isfinite(B))
     {
          B = 0.75;
     }
     B = std::clamp(B, 0.0, 1.0);

     if (!std::isfinite(Delta) || Delta < 0.0)
     {
          Delta = 1.0;
     }

     if (!std::isfinite(IdfSmooth) || IdfSmooth < 0.0)
     {
          IdfSmooth = 1.0;
     }

     if (!std::isfinite(BM25Weight) || BM25Weight < 0.0)
     {
          BM25Weight = 0.7;
     }

     if (!std::isfinite(TFIDFWeight) || TFIDFWeight < 0.0)
     {
          TFIDFWeight = 0.3;
     }

     if (!std::isfinite(PivotValue))
     {
          PivotValue = 0.25;
     }
     PivotValue = std::clamp(PivotValue, 0.0, 1.0);

     std::transform(SearchAlgorithm.begin(), SearchAlgorithm.end(), SearchAlgorithm.begin(), ToLowerAsciiSafe);
     if (SearchAlgorithm != "bm25" && SearchAlgorithm != "bm25+" && SearchAlgorithm != "tfidf" && SearchAlgorithm != "hybrid")
     {
          SearchAlgorithm = "bm25+";
     }

     std::transform(MatchMode.begin(), MatchMode.end(), MatchMode.begin(), ToLowerAsciiSafe);
     if (MatchMode != "and" && MatchMode != "or" && MatchMode != "min_should_match")
     {
          MatchMode = "and";
     }

     if (MinShouldMatch < 1)
     {
          MinShouldMatch = 1;
     }

     if (CandidatePruneMultiplier < 0)
     {
          CandidatePruneMultiplier = 0;
     }

     size_t RequiredMatches = TermResults.size();
     if (MatchMode == "or")
     {
          RequiredMatches = 1;
     }
     else if (MatchMode == "min_should_match")
     {
          RequiredMatches = static_cast<size_t>(std::max(1, MinShouldMatch));
          RequiredMatches = std::min(RequiredMatches, TermResults.size());
     }

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

     if (MatchMode == "and")
     {
          for (const auto &Pair : TermResults[TermOrder[0]])
          {
               DocScores[Pair.first] = Pair.second;
               DocMatchCounts[Pair.first] = 1;
          }

          for (size_t Idx = 1; Idx < TermOrder.size(); ++Idx)
          {
               size_t I = TermOrder[Idx];

               const auto &CurrentTermDocs = TermResults[I];

               if (CurrentTermDocs.size() > DocScores.size() * 10 && DocScores.size() > 100)
               {
                    std::vector<std::pair<std::string, Posting>> SortedDocs(DocScores.begin(), DocScores.end());

                    std::sort(SortedDocs.begin(), SortedDocs.end(), [](const auto &DocA, const auto &DocB)
                              {
                                   return DocA.first < DocB.first;
                              });

                    std::vector<std::pair<std::string, Posting>> SortedCurrent(CurrentTermDocs.begin(), CurrentTermDocs.end());

                    std::sort(SortedCurrent.begin(), SortedCurrent.end(), [](const auto &DocA, const auto &DocB)
                              {
                                   return DocA.first < DocB.first;
                              });

                    std::unordered_map<std::string, Posting> Intersection;
                    std::unordered_map<std::string, size_t> IntersectionCounts;

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

                              for (const auto &PosVal : SortedCurrent[Pos2].second.Positions)
                              {
                                   Intersection[SortedDocs[Pos1].first].Positions.push_back(PosVal);
                              }

                              Intersection[SortedDocs[Pos1].first].Score += SortedCurrent[Pos2].second.Score;
                              IntersectionCounts[SortedDocs[Pos1].first] = DocMatchCounts[SortedDocs[Pos1].first] + 1;

                              Pos1++;

                              Pos2++;
                         }
                    }

                    DocScores = std::move(Intersection);
                    DocMatchCounts = std::move(IntersectionCounts);
               }
               else
               {
                    std::unordered_map<std::string, Posting> Intersection;
                    std::unordered_map<std::string, size_t> IntersectionCounts;

                    for (const auto &Pair : CurrentTermDocs)
                    {
                         auto It = DocScores.find(Pair.first);

                         if (It != DocScores.end())
                         {
                              Intersection[Pair.first] = It->second;

                              for (const auto &PosVal : Pair.second.Positions)
                              {
                                   Intersection[Pair.first].Positions.push_back(PosVal);
                              }

                              Intersection[Pair.first].Score += Pair.second.Score;
                              IntersectionCounts[Pair.first] = DocMatchCounts[Pair.first] + 1;
                         }
                    }

                    DocScores = std::move(Intersection);
                    DocMatchCounts = std::move(IntersectionCounts);
               }
          }
     }
     else
     {
          for (const auto &TermDocs : TermResults)
          {
               for (const auto &Pair : TermDocs)
               {
                    auto It = DocScores.find(Pair.first);

                    if (It == DocScores.end())
                    {
                         DocScores[Pair.first] = Pair.second;
                    }
                    else
                    {
                         It->second.Score += Pair.second.Score;

                         for (const auto &PosVal : Pair.second.Positions)
                         {
                              It->second.Positions.push_back(PosVal);
                         }
                    }

                    DocMatchCounts[Pair.first]++;
               }
          }

          if (RequiredMatches > 1)
          {
               for (auto It = DocScores.begin(); It != DocScores.end();)
               {
                    if (DocMatchCounts[It->first] < RequiredMatches)
                    {
                         DocMatchCounts.erase(It->first);
                         It = DocScores.erase(It);
                    }
                    else
                    {
                         ++It;
                    }
               }
          }
     }

     if (!NegativeDocIDs.empty())
     {
          for (const auto &DocID : NegativeDocIDs)
          {
               DocScores.erase(DocID);
               DocMatchCounts.erase(DocID);
          }
     }

     if (DocScores.empty())
     {
          return {};
     }

     CollectionSizeValue = std::max(CollectionSizeValue, DocScores.size());

     std::vector<Posting> Results;

     Results.reserve(DocScores.size());

     struct CandidateDocument
     {
          std::string DocumentID;
          Posting *PostingValue;
          double DocumentLength;
     };

     std::vector<CandidateDocument> CandidateDocs;
     CandidateDocs.reserve(DocScores.size());

     {
          std::lock_guard<std::mutex> Lock(IndexMutex);
          const auto CollectionLengthsIt = DocumentLengths.find(Collection);

          if (CollectionLengthsIt != DocumentLengths.end() && !CollectionLengthsIt->second.empty())
          {
               CollectionSizeValue = std::max(CollectionSizeValue, CollectionLengthsIt->second.size());

               if (!std::isfinite(AvgDocLengthValue) || AvgDocLengthValue <= 0.0)
               {
                    size_t TotalLength = 0;
                    for (const auto &[LengthDocID, LengthValue] : CollectionLengthsIt->second)
                    {
                         (void)LengthDocID;
                         TotalLength += LengthValue;
                    }

                    if (TotalLength > 0)
                    {
                         AvgDocLengthValue = static_cast<double>(TotalLength) / static_cast<double>(CollectionLengthsIt->second.size());
                    }
               }
          }

          if (!std::isfinite(AvgDocLengthValue) || AvgDocLengthValue <= 0.0)
          {
               AvgDocLengthValue = 1.0;
          }

          for (auto &[DocID, Post] : DocScores)
          {
               double DocLengthValue = 1.0;
               if (CollectionLengthsIt != DocumentLengths.end())
               {
                    const auto DocLengthIt = CollectionLengthsIt->second.find(DocID);
                    if (DocLengthIt != CollectionLengthsIt->second.end())
                    {
                         DocLengthValue = static_cast<double>(DocLengthIt->second);
                    }
               }

               CandidateDocs.push_back({DocID, &Post, DocLengthValue});
          }
     }

     if (Limit > 0 && CandidatePruneMultiplier > 0)
     {
          const size_t SafeLimitValue = static_cast<size_t>(Limit);
          const size_t SafeMultiplierValue = static_cast<size_t>(CandidatePruneMultiplier);
          const size_t ProductLimitValue = SafeMultiplierValue > 0 && SafeLimitValue > (std::numeric_limits<size_t>::max() / SafeMultiplierValue)
                                               ? std::numeric_limits<size_t>::max()
                                               : SafeLimitValue * SafeMultiplierValue;
          const size_t CandidateLimitValue = std::max(SafeLimitValue, ProductLimitValue);

          if (CandidateDocs.size() > CandidateLimitValue)
          {
               std::nth_element(CandidateDocs.begin(),
                                CandidateDocs.begin() + static_cast<std::ptrdiff_t>(CandidateLimitValue),
                                CandidateDocs.end(),
                                [](const CandidateDocument &DocA, const CandidateDocument &DocB)
                                {
                                     if (DocA.PostingValue->Score != DocB.PostingValue->Score)
                                     {
                                          return DocA.PostingValue->Score > DocB.PostingValue->Score;
                                     }

                                     return DocA.DocumentID < DocB.DocumentID;
                                });

               CandidateDocs.resize(CandidateLimitValue);
          }
     }

     for (const auto &[DocID, PostPtr, DocLengthValue] : CandidateDocs)
     {
          Posting &Post = *PostPtr;
          double MatchedTermScore = Post.Score;

          double TotalScore = 0.0;
          std::vector<Posting> MatchedPostingsForProximity;

          for (size_t TermIdx = 0; TermIdx < TermResults.size(); ++TermIdx)
          {
               const auto &TermDocs = TermResults[TermIdx];
               auto TermDocIt = TermDocs.find(DocID);
               if (TermDocIt == TermDocs.end())
               {
                    continue;
               }

               MatchedPostingsForProximity.push_back(TermDocIt->second);
               double TermFreq = static_cast<double>(TermDocIt->second.Positions.empty() ? 1 : TermDocIt->second.Positions.size());
               double DocFreq = static_cast<double>(TermDocs.size());

               double TermScoreValue = 0.0;
               const double EffectiveB = PivotEnabled ? 0.0 : B;
               const bool EffectiveTFIDFNormalization = NormalizeTFIDF && !PivotEnabled;

               if (SearchAlgorithm == "bm25")
               {
                    TermScoreValue = CalculateBM25PlusScore(TermFreq, DocFreq, DocLengthValue, AvgDocLengthValue, static_cast<double>(CollectionSizeValue), K1, EffectiveB, 0.0);
               }
               else if (SearchAlgorithm == "tfidf")
               {
                    TermScoreValue = CalculateTFIDFScore(TermFreq, DocFreq, DocLengthValue, static_cast<double>(CollectionSizeValue), IdfSmooth, EffectiveTFIDFNormalization);
               }
               else if (SearchAlgorithm == "hybrid")
               {
                    const double WeightTotal = BM25Weight + TFIDFWeight;
                    if (WeightTotal > 0.0)
                    {
                         const double BM25Score = CalculateBM25PlusScore(TermFreq, DocFreq, DocLengthValue, AvgDocLengthValue, static_cast<double>(CollectionSizeValue), K1, EffectiveB, 0.0);
                         const double TFIDFScore = CalculateTFIDFScore(TermFreq, DocFreq, DocLengthValue, static_cast<double>(CollectionSizeValue), IdfSmooth, EffectiveTFIDFNormalization);
                         TermScoreValue = ((BM25Weight * BM25Score) + (TFIDFWeight * TFIDFScore)) / WeightTotal;
                    }
               }
               else
               {
                    TermScoreValue = CalculateBM25PlusScore(TermFreq, DocFreq, DocLengthValue, AvgDocLengthValue, static_cast<double>(CollectionSizeValue), K1, EffectiveB, Delta);
               }

               if (PivotEnabled && AvgDocLengthValue > 0.0)
               {
                    const double PivotNorm = (1.0 - PivotValue) + PivotValue * (DocLengthValue / AvgDocLengthValue);
                    if (std::isfinite(PivotNorm) && PivotNorm > 0.0)
                    {
                         TermScoreValue /= PivotNorm;
                    }
               }

               TotalScore += TermScoreValue * std::max(0.0, TermDocIt->second.Score);
          }

          if (QueryTerms.size() >= 2)
          {
               double ProximityBoostValue = CalculateProximityBoost(QueryTerms, MatchedPostingsForProximity, DocID);

               TotalScore *= ProximityBoostValue;
          }

          if (ValidQueryTermCount > 1)
          {
               const double MatchedTerms = static_cast<double>(DocMatchCounts[DocID]);
               const double CoverageRatio = std::min(1.0, MatchedTerms / static_cast<double>(ValidQueryTermCount));
               TotalScore *= 0.75 + (0.25 * CoverageRatio);
          }

          Post.Score = TotalScore;

          if (!std::isfinite(TotalScore) || TotalScore <= 0.0)
          {
               Post.Score = std::max(MatchedTermScore, 1e-9);
          }

          Results.push_back(Post);
     }

     if (Limit > 0 && Results.size() > static_cast<size_t>(Limit))
     {
          const std::size_t LimitSize = static_cast<std::size_t>(Limit);

          std::nth_element(Results.begin(),
                           Results.begin() + static_cast<std::ptrdiff_t>(LimitSize),
                           Results.end(),
                           [](const Posting &PostingA, const Posting &PostingB)
                           {
                                if (PostingA.Score != PostingB.Score)
                                {
                                     return PostingA.Score > PostingB.Score;
                                }

                                return PostingA.DocumentID < PostingB.DocumentID;
                           });

          Results.resize(LimitSize);
     }

     std::sort(Results.begin(), Results.end(), [](const Posting &PostingA, const Posting &PostingB)
               {
                    if (PostingA.Score != PostingB.Score)
                    {
                         return PostingA.Score > PostingB.Score;
                    }

                    return PostingA.DocumentID < PostingB.DocumentID;
               });

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "Search: Found " + std::to_string(Results.size()) + " results for query '" + Query + "' (algorithm=" + SearchAlgorithm + ", index=" + (UseMMap ? "mmap" : "memory") + ").");
     }

     return Results;
}

/*
 * SearchPrefix - Searches for terms with a given prefix in the index.
 */

/* InvertedIndex::SearchPrefix - Searches prefix terms. */

std::vector<Posting> InvertedIndex::SearchPrefix(const std::string &Collection, const std::string &Prefix, int Limit)
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
     TouchCollectionLocked(Collection);

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

     for (const auto &[TermValue, Postings] : CollectionIt->second)
     {
          if (TermValue.length() >= NormalizedPrefix.length() && TermValue.substr(0, NormalizedPrefix.length()) == NormalizedPrefix)
          {
               for (const auto &Post : Postings)
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

     for (auto &Pair : DocScores)
     {
          Results.push_back(Pair.second);
     }

     std::sort(Results.begin(), Results.end(), [](const Posting &PostingA, const Posting &PostingB)
               {
                    return PostingA.Score > PostingB.Score;
               });

     if (Limit > 0)
     {
          const std::size_t LimitSize = static_cast<std::size_t>(Limit);
          if (Results.size() > LimitSize)
          {
               Results.resize(LimitSize);
          }
     }

     if (Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("inverted_index", "SearchPrefix: Found " + std::to_string(Results.size()) + " results for prefix '" + Prefix + "'.");
     }

     return Results;
}

/*
      * DeleteCollection - Deletes an entire collection from the index.
      */

/* InvertedIndex::DeleteCollection - Deletes all data for a collection. */

void InvertedIndex::DeleteCollection(const std::string &Collection)
{
     std::lock_guard<std::mutex> Lock(IndexMutex);

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
               auto IndexIt = Index.find(Collection);

               if (IndexIt == Index.end() || IndexIt->second.empty())
               {
                    DirtyCollections.erase(Collection);
                    continue;
               }

               auto ExistingMMapIt = MMapIndexes.find(Collection);

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
               Instance->Database->Set(FlushMarker, "1");
          }

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

               if (MMapIdx && MMapIdx->IsValid())
               {
                    MMapIndexes[WorkItem.Collection] = std::move(MMapIdx);
               }

               auto MutationIt = CollectionLastMutation.find(WorkItem.Collection);
               const bool HasNewMutation = MutationIt != CollectionLastMutation.end() &&
                    MutationIt->second > WorkItem.MutationTime;

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

     if (!std::filesystem::exists(IndexDir))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("inverted_index", "LoadFromDisk: Index directory does not exist: " + IndexDir + ".");
          }

          return;
     }

     std::vector<std::pair<std::string, std::unique_ptr<MMapIndex>>> LoadedIndexes;

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
               LoadedIndexes.emplace_back(Collection, std::move(MMapIdx));
          }
     }

     std::lock_guard<std::mutex> Lock(IndexMutex);

     for (auto &Entry : LoadedIndexes)
     {
          const std::string &Collection = Entry.first;
          const size_t TermCount = Entry.second->GetTermCount();

          MMapIndexes[Collection] = std::move(Entry.second);
          TouchCollectionLocked(Collection);

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
          return 0.0;
     }

     std::string IdfMode = "legacy";
     bool ClampNegative = true;
     double IdfSmoothValue = 1.0;
     double IdfFloorFactor = 0.05;

     if (Instance && Instance->Config)
     {
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

     double Idf = 0.0;

     if (IdfMode == "smooth")
     {
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

     const double NormalizedLengthValue = std::max(1e-9, DocLength / AvgDocLength);
     const double NumeratorValue = TermFreq * (K1 + 1.0);
     const double DenominatorValue = TermFreq + K1 * (1.0 - B + B * NormalizedLengthValue);

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
          return 0.0;
     }

     const double IdfDenominator = DocFreq + IdfSmooth;
     const double IdfNumerator = CollectionSize + IdfSmooth;
     if (IdfDenominator <= 0.0 || IdfNumerator <= 0.0)
     {
          return 0.0;
     }

     double TermWeight = 1.0 + std::log(TermFreq);
     if (Normalize)
     {
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
          return 0.0;
     }

     std::string IdfMode = "legacy";
     bool ClampNegative = true;
     double IdfSmoothValue = 1.0;
     double IdfFloorFactor = 0.05;

     if (Instance && Instance->Config)
     {
          IdfMode = Instance->Config->GetRankingIdfMode();
          ClampNegative = Instance->Config->GetRankingIdfClampNegative();
          IdfSmoothValue = std::max(0.0, Instance->Config->GetRankingIdfSmooth());
          IdfFloorFactor = std::max(0.0, Instance->Config->GetRankingIdfFloorFactor());
     }

     double Idf = 0.0;

     if (IdfMode == "smooth")
     {
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

     auto DocLengthsIt = DocumentLengths.find(Collection);

     if (DocLengthsIt == DocumentLengths.end() || DocLengthsIt->second.empty())
     {
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
          return 1.0;
     }

     double boost_scale = 1.0;
     double boost_max = 2.0;

     if (Instance && Instance->Config)
     {
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

     return std::min(BoostValueResult, boost_max);
}
