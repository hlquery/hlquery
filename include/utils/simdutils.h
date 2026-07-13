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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

/* SIMD-optimized string operations for ultra-fast text processing */

/* Fast string comparison using AVX2 */

inline bool fast_string_equal(const char *a, const char *b, size_t len)
{
     if (len == 0)
     {
          return true;
     }

     if (a == b)
     {
          return true;
     }

     size_t i = 0;

#if defined(__AVX2__)
     /* Process 32 bytes at a time using AVX2 */

     while (i + 32 <= len)
     {
          __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i));
          __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
          __m256i cmp = _mm256_cmpeq_epi8(va, vb);
          int mask = _mm256_movemask_epi8(cmp);

          if (mask != static_cast<int>(0xFFFFFFFF))
          {
               return false;
          }
          i += 32;
     }
#endif

     /* Handle remaining bytes */

     while (i < len)
     {
          if (a[i] != b[i])
          {
               return false;
          }
          i++;
     }

     return true;
}

/* Fast string search using AVX2 */

inline const char *fast_string_search(const char *haystack, size_t haystack_len,
                                      const char *needle, size_t needle_len)
{
     if (needle_len == 0)
     {
          return haystack;
     }

     if (needle_len > haystack_len)
     {
          return nullptr;
     }

#if defined(__AVX2__)
     /* Load first character of needle into all positions of AVX2 register */

     __m256i needle_first = _mm256_set1_epi8(needle[0]);

     for (size_t i = 0; i <= haystack_len - needle_len; i += 32)
     {
          /* Load 32 bytes of haystack */

          __m256i haystack_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(haystack + i));

          /* Compare with first character of needle */

          __m256i cmp = _mm256_cmpeq_epi8(haystack_chunk, needle_first);
          int mask = _mm256_movemask_epi8(cmp);

          /* Check each potential match */

          while (mask != 0)
          {
               int pos = __builtin_ctz(mask);

               if (i + pos + needle_len <= haystack_len)
               {
                    if (fast_string_equal(haystack + i + pos, needle, needle_len))
                    {
                         return haystack + i + pos;
                    }
               }

               /* Clear lowest set bit */

               mask &= mask - 1;
          }
     }
#else
     for (size_t i = 0; i <= haystack_len - needle_len; ++i)
     {
          if (haystack[i] == needle[0] &&
              fast_string_equal(haystack + i, needle, needle_len))
          {
               return haystack + i;
          }
     }
#endif

     return nullptr;
}

/* Fast string to lowercase conversion using AVX2 */

inline void fast_to_lowercase(char *str, size_t len)
{
     size_t i = 0;

#if defined(__AVX2__)
     /* Process 32 bytes at a time */

     while (i + 32 <= len)
     {
          __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(str + i));

          /* Check if characters are uppercase (A-Z) */

          __m256i is_upper = _mm256_and_si256(
               _mm256_cmpgt_epi8(chunk, _mm256_set1_epi8('A' - 1)),
               _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), chunk));

          /* Convert to lowercase by adding 32 to uppercase characters */

          __m256i lower_chunk = _mm256_add_epi8(chunk,
                                                _mm256_and_si256(is_upper, _mm256_set1_epi8(32)));

          _mm256_storeu_si256(reinterpret_cast<__m256i *>(str + i), lower_chunk);
          i += 32;
     }
#endif

     /* Handle remaining bytes */

     while (i < len)
     {
          if (str[i] >= 'A' && str[i] <= 'Z')
          {
               str[i] += 32;
          }
          i++;
     }
}

/* Fast string hash using FNV-1a with SIMD optimization */

inline uint64_t fast_hash(const char *str, size_t len)
{
     const uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
     const uint64_t FNV_PRIME = 1099511628211ULL;

     uint64_t hash = FNV_OFFSET_BASIS;
     size_t i = 0;

     /* Process 8 bytes at a time using 64-bit operations */

     while (i + 8 <= len)
     {
          uint64_t chunk = *reinterpret_cast<const uint64_t *>(str + i);
          hash ^= chunk;
          hash *= FNV_PRIME;
          i += 8;
     }

     /* Handle remaining bytes */

     while (i < len)
     {
          hash ^= static_cast<uint8_t>(str[i]);
          hash *= FNV_PRIME;
          i++;
     }

     return hash;
}

/* Fast string comparison for sorting */

inline int fast_string_compare(const char *a, const char *b, size_t len_a, size_t len_b)
{
     size_t min_len = std::min(len_a, len_b);
     size_t i = 0;

#if defined(__AVX2__)
     /* Process 32 bytes at a time */

     while (i + 32 <= min_len)
     {
          __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i));
          __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
          __m256i cmp = _mm256_cmpeq_epi8(va, vb);
          int mask = _mm256_movemask_epi8(cmp);

          if (mask != static_cast<int>(0xFFFFFFFF))
          {
               /* Find first difference */

               int diff_pos = __builtin_ctz(~mask);
               return static_cast<int>(static_cast<unsigned char>(a[i + diff_pos])) -
                      static_cast<int>(static_cast<unsigned char>(b[i + diff_pos]));
          }
          i += 32;
     }
#endif

     /* Handle remaining bytes */

     while (i < min_len)
     {
          int diff = static_cast<int>(static_cast<unsigned char>(a[i])) -
                     static_cast<int>(static_cast<unsigned char>(b[i]));

          if (diff != 0)
          {
               return diff;
          }
          i++;
     }

     /* Strings are equal up to min_len, compare lengths */

     return static_cast<int>(len_a) - static_cast<int>(len_b);
}

/* Fast memory copy with prefetching */

inline void fast_memcpy(void *dest, const void *src, size_t len)
{
     if (len == 0)
     {
          return;
     }

#if defined(__AVX2__)
     /* Use AVX2 for large copies */

     if (len >= 64)
     {
          size_t i = 0;

          /* Prefetch source data */

          __builtin_prefetch(src, 0, 3);
          __builtin_prefetch(static_cast<const char *>(src) + 64, 0, 3);

          /* Copy 32 bytes at a time */

          while (i + 32 <= len)
          {
               __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(static_cast<const char *>(src) + i));
               _mm256_storeu_si256(reinterpret_cast<__m256i *>(static_cast<char *>(dest) + i), chunk);
               i += 32;
          }

          /* Handle remaining bytes */

          std::memcpy(static_cast<char *>(dest) + i, static_cast<const char *>(src) + i, len - i);
     }
     else
     {
          std::memcpy(dest, src, len);
     }
#else
     std::memcpy(dest, src, len);
#endif
}

/* Fast string length calculation */

inline size_t fast_strlen(const char *str)
{
     size_t len = 0;

#if defined(__AVX2__)
     /* Process 32 bytes at a time looking for null terminator */

     while (true)
     {
          __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(str + len));
          __m256i zero_cmp = _mm256_cmpeq_epi8(chunk, _mm256_setzero_si256());
          int mask = _mm256_movemask_epi8(zero_cmp);

          if (mask != 0)
          {
               len += __builtin_ctz(mask);
               break;
          }
          len += 32;
     }
#else
     while (str[len] != '\0')
     {
          ++len;
     }
#endif

     return len;
}
