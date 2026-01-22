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
#include <cstring>

#include "utils/wildcard.h"

namespace 
{
    /*
     * Normalize pattern by collapsing consecutive '*' wildcards into a single '*'
     * This improves performance by reducing backtracking
     * Returns normalized pattern and whether it was modified
     */

    std::string NormalizePattern(const std::string& pattern) 
    {
        if (pattern.empty()) 
        {
            return pattern;
        }
        
        /* Quick check: if no consecutive stars, return original */

        bool NeedsNormalization = false;

        for (size_t i = 0; i < pattern.size() - 1; ++i) 
        {
            if (pattern[i] == '*' && pattern[i + 1] == '*') 
            {
                NeedsNormalization = true;
                break;
            }
        }
        
        if (!NeedsNormalization) 
        {
            return pattern; /* Return original to avoid allocation */
        }
        
        std::string Normalized;
        Normalized.reserve(pattern.size());

        bool LastWasStar = false;
        
        for (char c : pattern) 
        {
            if (c == '*') 
            {
                if (!LastWasStar) 
                {
                    Normalized.push_back(c);
                    LastWasStar = true;
                }
            } 
            else 
            {
                Normalized.push_back(c);
                LastWasStar = false;
            }
        }
        
        return Normalized;
    }
    
    /*
     * Fast case-insensitive character comparison
     */

    inline bool CharEqualCaseInsensitive(char a, char b) 
    {
        /* Fast path for ASCII: most common case */

        if ((a | b) & 0x80) 
        {
            /* Non-ASCII, use tolower */

            return std::tolower(static_cast<unsigned char>(a)) == 
                   std::tolower(static_cast<unsigned char>(b));
        }

        /* ASCII fast path */

        if (a == b) 
        {
            return true;
        }

        /* Case-insensitive ASCII comparison */

        return (a ^ b) == 0x20 && ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z'));
    }
    
    /*
     * Fast-path: Check if pattern is exact match (no wildcards)
     * Optimized for common case where strings are already lowercase
     */

    bool MatchExact(const char* str, const char* pattern) 
    {
        /* Try fast memcmp first (case-sensitive) */

        if (std::strcmp(str, pattern) == 0) 
        {
            return true;
        }

        /* Fall back to case-insensitive */

        while (*str && *pattern) 
        {
            if (!CharEqualCaseInsensitive(*str, *pattern)) 
            {
                return false;
            }

            str++;
            pattern++;
        }

        return *str == *pattern; /* Both must be null */
    }
    
    /*
     * Fast-path: Check if string starts with prefix (pattern ends with *)
     * Uses memcmp for case-sensitive fast path
     */

    bool MatchPrefix(const char* str, const char* pattern, size_t PrefixLen) 
    {
        /* Fast path: try case-sensitive first (common when both are lowercase) */

        if (std::memcmp(str, pattern, PrefixLen) == 0) 
        {
            return true;
        }

        /* Fall back to case-insensitive */

        for (size_t i = 0; i < PrefixLen; ++i) 
        {
            if (!CharEqualCaseInsensitive(str[i], pattern[i])) 
            {
                return false;
            }
        }

        return true;
    }
    
    /*
     * Fast-path: Check if string ends with suffix (pattern starts with *)
     * Uses memcmp for case-sensitive fast path
     */

    bool MatchSuffix(const char* str, const char* pattern, size_t StrLen, size_t SuffixLen) 
    {
        if (StrLen < SuffixLen) 
        {
            return false;
        }

        const char* StrSuffix = str + (StrLen - SuffixLen);
        const char* PatternSuffix = pattern + 1; /* Skip leading * */
        
        /* Fast path: try case-sensitive first */

        if (std::memcmp(StrSuffix, PatternSuffix, SuffixLen) == 0) 
        {
            return true;
        }

        /* Fall back to case-insensitive */

        for (size_t i = 0; i < SuffixLen; ++i) 
        {
            if (!CharEqualCaseInsensitive(StrSuffix[i], PatternSuffix[i])) 
            {
                return false;
            }
        }

        return true;
    }
    
    /*
     * Fast-path: Check if string contains substring (pattern is *substring*)
     * Uses Boyer-Moore-like optimization for better performance
     */

    bool MatchContains(const char* str, const char* pattern, size_t StrLen, size_t SubstrLen) 
    {
        if (StrLen < SubstrLen) 
        {
            return false;
        }
        
        /* Skip leading * */

        pattern++;
        
        /* For very short substrings, use simple search */

        if (SubstrLen <= 4) 
        {
            for (size_t i = 0; i <= StrLen - SubstrLen; ++i) 
            {
                bool Found = true;

                for (size_t j = 0; j < SubstrLen; ++j) 
                {
                    if (!CharEqualCaseInsensitive(str[i + j], pattern[j])) 
                    {
                        Found = false;
                        break;
                    }
                }

                if (Found) 
                {
                    return true;
                }
            }

            return false;
        }
        
        /* For longer substrings, try case-sensitive first (common case) */
        /* Then fall back to case-insensitive */

        for (size_t i = 0; i <= StrLen - SubstrLen; ++i) 
        {
            /* Fast path: try case-sensitive match first */

            if (std::memcmp(str + i, pattern, SubstrLen) == 0) 
            {
                return true;
            }

            /* Case-insensitive check */

            bool Found = true;

            for (size_t j = 0; j < SubstrLen; ++j) 
            {
                if (!CharEqualCaseInsensitive(str[i + j], pattern[j])) 
                {
                    Found = false;
                    break;
                }
            }

            if (Found) 
            {
                return true;
            }
        }

        return false;
    }
    
    /*
     * Analyze pattern to determine its type and extract metadata
     * Returns: HasWildcard, IsPrefix, IsSuffix, IsContains, PrefixLen, SuffixLen
     */

    struct PatternInfo 
    {
        bool HasWildcard;
        bool IsExact;
        bool IsPrefix;      /* pattern ends with * */
        bool IsSuffix;      /* pattern starts with * */
        bool IsContains;    /* pattern is *substring* */
        size_t PrefixLen;
        size_t SuffixLen;
        size_t ContainsLen;
    };
    
    PatternInfo AnalyzePattern(const char* pattern) 
    {
        PatternInfo Info = {};

        if (!pattern || *pattern == '\0') 
        {
            Info.IsExact = true;
            return Info;
        }
        
        const char* p = pattern;

        size_t Len = 0;

        bool FoundStar = false;
        bool FoundQuestion = false;

        size_t FirstStarPos = static_cast<size_t>(-1); /* Use max value as sentinel */
        size_t LastStarPos = static_cast<size_t>(-1);
        
        /* First pass: analyze pattern */

        while (*p) 
        {
            if (*p == '*') 
            {
                FoundStar = true;

                if (FirstStarPos == static_cast<size_t>(-1)) 
                {
                    FirstStarPos = Len;
                }

                LastStarPos = Len;
            } 
            else if (*p == '?') 
            {
                FoundQuestion = true;
            }

            Len++;
            p++;
        }
        
        Info.HasWildcard = FoundStar || FoundQuestion;
        Info.IsExact = !Info.HasWildcard;
        
        if (!FoundStar || FoundQuestion) 
        {
            return Info; /* Complex pattern, use general algorithm */
        }
        
        /* Check for simple patterns */

        if (FirstStarPos == 0 && LastStarPos == Len - 1 && Len > 1) {
            /* Pattern is *substring* */

            Info.IsContains = true;
            Info.ContainsLen = Len - 2;
        } else if (FirstStarPos == 0 && LastStarPos == 0 && Len > 1) {
            /* Pattern starts with * only */

            Info.IsSuffix = true;
            Info.SuffixLen = Len - 1;
        } else if (FirstStarPos == Len - 1 && LastStarPos == Len - 1 && Len > 1) {
            /* Pattern ends with * only */

            Info.IsPrefix = true;
            Info.PrefixLen = Len - 1;
        }
        
        return Info;
    }
}

bool WildcardMatcher::Match(const std::string& str, const std::string& pattern) 
{
    if (pattern.empty()) 
    {
        return str.empty();
    }
    
    /* Normalize pattern (collapse consecutive *) */

    std::string Normalized = NormalizePattern(pattern);

    return Match(str.c_str(), Normalized.c_str());
}

bool WildcardMatcher::Match(const char* str, const char* pattern) 
{
    /* Input validation */

    if (!str || !pattern) 
    {
        return false;
    }
    
    /* Empty pattern matches only empty string */

    if (*pattern == '\0') 
    {
        return *str == '\0';
    }
    
    /* Empty string only matches if pattern is all stars */

    if (*str == '\0') 
    {
        while (*pattern == '*') 
        {
            pattern++;
        }

        return *pattern == '\0';
    }
    
    return MatchInternal(str, pattern);
}

bool WildcardMatcher::MatchInternal(const char* str, const char* pattern) 
{
    /* Analyze pattern once to determine fast-path */

    PatternInfo Info = AnalyzePattern(pattern);
    
    if (Info.IsExact) 
    {
        return MatchExact(str, pattern);
    }
    
    /* Get string length once (avoid multiple strlen calls) */

    size_t StrLen = std::strlen(str);
    
    /* Fast-path: Prefix match (pattern ends with *) */

    if (Info.IsPrefix) 
    {
        if (StrLen < Info.PrefixLen) 
        {
            return false;
        }

        return MatchPrefix(str, pattern, Info.PrefixLen);
    }
    
    /* Fast-path: Suffix match (pattern starts with *) */

    if (Info.IsSuffix) 
    {
        if (StrLen < Info.SuffixLen) 
        {
            return false;
        }

        return MatchSuffix(str, pattern, StrLen, Info.SuffixLen);
    }
    
    /* Fast-path: Contains match (pattern is *substring*) */

    if (Info.IsContains) 
    {
        return MatchContains(str, pattern, StrLen, Info.ContainsLen);
    }
    
    /* General case: Use optimized backtracking algorithm for complex patterns */

    const char* cp = nullptr;
    const char* mp = nullptr;

    const char* StrStart = str;

    /* Match characters until first * */

    while (*str && *pattern != '*') 
    {
        if (!CharEqualCaseInsensitive(*pattern, *str) && *pattern != '?') 
        {
            return false;
        }

        pattern++;
        str++;
    }

    /* Handle remaining pattern with backtracking */

    while (*str) 
    {
        if (*pattern == '*') 
        {
            /* Skip consecutive stars (shouldn't happen after normalization, but be safe) */

            while (*pattern == '*') 
            {
                pattern++;
            }

            if (*pattern == '\0') 
            {
                return true; /* Pattern ends with *, matches rest of string */
            }

            mp = pattern;
            cp = str + 1;
        } 
        else if (CharEqualCaseInsensitive(*pattern, *str) || *pattern == '?') 
        {
            pattern++;
            str++;
        } 
        else 
        {
            /* Backtrack */

            if (mp == nullptr) 
            {
                return false; /* No backtrack point */
            }

            pattern = mp;
            str = cp++;
            
            /* Safety: prevent infinite loop */

            if (cp > StrStart + StrLen) 
            {
                return false;
            }
        }
    }

    /* Skip trailing stars */

    while (*pattern == '*') 
    {
        pattern++;
    }

    return *pattern == '\0';
}

std::vector<std::string> WildcardMatcher::Filter(const std::vector<std::string>& strings, const std::string& pattern) 
{
    if (strings.empty()) 
    {
        return {};
    }
    
    std::vector<std::string> Results;
    
    /* Reserve space for better performance (estimate 10-20% match rate) */
    /* But cap at reasonable size to avoid over-allocation */

    size_t ReserveSize = std::min(strings.size() / 5, strings.size());

    if (ReserveSize < 8) 
    {
        ReserveSize = 8; /* Minimum reasonable reserve */
    }

    Results.reserve(ReserveSize);
    
    /* Normalize pattern once (reuse across all matches) */

    std::string Normalized = NormalizePattern(pattern);
    const char* NormalizedCStr = Normalized.c_str();
    
    /* Optimize: use direct C-string matching to avoid string copies */
    
    for (const auto& str : strings) 
    {
        if (Match(str.c_str(), NormalizedCStr)) 
        {
            Results.push_back(str);
        }
    }
    
    /* Shrink to fit if we over-allocated significantly */

    if (Results.size() < Results.capacity() / 2) 
    {
        Results.shrink_to_fit();
    }
    
    return Results;
}
