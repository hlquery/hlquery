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

    std::string NormalizePattern(const std::string& Pattern) 
    {
        if (Pattern.empty()) 
        {
            return Pattern;
        }
        
        /* Quick check: if no consecutive stars, return original */

        bool NeedsNormalization = false;

        for (size_t i = 0; i < Pattern.size() - 1; ++i) 
        {
            if (Pattern[i] == '*' && Pattern[i + 1] == '*') 
            {
                NeedsNormalization = true;
                break;
            }
        }
        
        if (!NeedsNormalization) 
        {
            return Pattern; /* Return original to avoid allocation */
        }
        
        std::string Normalized;

        Normalized.reserve(Pattern.size());

        bool LastWasStar = false;
        
        for (char c : Pattern) 
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

    bool MatchExact(const char* Str, const char* Pattern) 
    {
        /* Try fast memcmp first (case-sensitive) */

        if (std::strcmp(Str, Pattern) == 0) 
        {
            return true;
        }

        /* Fall back to case-insensitive */

        while (*Str && *Pattern) 
        {
            if (!CharEqualCaseInsensitive(*Str, *Pattern)) 
            {
                return false;
            }

            Str++;
            Pattern++;
        }

        return *Str == *Pattern; /* Both must be null */
    }
    
    /*
     * Fast-path: Check if string starts with prefix (pattern ends with *)
     * Uses memcmp for case-sensitive fast path
     */

    bool MatchPrefix(const char* Str, const char* Pattern, size_t PrefixLen) 
    {
        /* Fast path: try case-sensitive first (common when both are lowercase) */

        if (std::memcmp(Str, Pattern, PrefixLen) == 0) 
        {
            return true;
        }

        /* Fall back to case-insensitive */

        for (size_t i = 0; i < PrefixLen; ++i) 
        {
            if (!CharEqualCaseInsensitive(Str[i], Pattern[i])) 
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

    bool MatchSuffix(const char* Str, const char* Pattern, size_t StrLen, size_t SuffixLen) 
    {
        if (StrLen < SuffixLen) 
        {
            return false;
        }

        const char* StrSuffix = Str + (StrLen - SuffixLen);

        const char* PatternSuffix = Pattern + 1; /* Skip leading * */
        
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

    bool MatchContains(const char* Str, const char* Pattern, size_t StrLen, size_t SubstrLen) 
    {
        if (StrLen < SubstrLen) 
        {
            return false;
        }
        
        /* Skip leading * */

        Pattern++;
        
        /* For very short substrings, use simple search */

        if (SubstrLen <= 4) 
        {
            for (size_t i = 0; i <= StrLen - SubstrLen; ++i) 
            {
                bool Found = true;

                for (size_t j = 0; j < SubstrLen; ++j) 
                {
                    if (!CharEqualCaseInsensitive(Str[i + j], Pattern[j])) 
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

            if (std::memcmp(Str + i, Pattern, SubstrLen) == 0) 
            {
                return true;
            }

            /* Case-insensitive check */

            bool Found = true;

            for (size_t j = 0; j < SubstrLen; ++j) 
            {
                if (!CharEqualCaseInsensitive(Str[i + j], Pattern[j])) 
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
    
    /* Comment goes here */

    PatternInfo AnalyzePattern(const char* Pattern) 
    {
        PatternInfo Info = {};

        if (!Pattern || *Pattern == '\0') 
        {
            Info.IsExact = true;
            return Info;
        }
        
        const char* P = Pattern;

        size_t Len = 0;

        bool FoundStar = false;

        bool FoundQuestion = false;

        size_t FirstStarPos = static_cast<size_t>(-1); /* Use max value as sentinel */

        size_t LastStarPos = static_cast<size_t>(-1);
        
        /* First pass: analyze pattern */

        while (*P) 
        {
            if (*P == '*') 
            {
                FoundStar = true;

                if (FirstStarPos == static_cast<size_t>(-1)) 
                {
                    FirstStarPos = Len;
                }

                LastStarPos = Len;
            } 
            else if (*P == '?') 
            {
                FoundQuestion = true;
            }

            Len++;
            P++;
        }
        
        Info.HasWildcard = FoundStar || FoundQuestion;
        Info.IsExact = !Info.HasWildcard;
        
        if (!FoundStar || FoundQuestion) 
        {
            return Info; /* Complex pattern, use general algorithm */
        }
        
        /* Check for simple patterns */

        if (FirstStarPos == 0 && LastStarPos == Len - 1 && Len > 1) 
        {
            /* Pattern is *substring* */

            Info.IsContains = true;
            Info.ContainsLen = Len - 2;
        } 
        else if (FirstStarPos == 0 && LastStarPos == 0 && Len > 1) 
        {
            /* Pattern starts with * only */

            Info.IsSuffix = true;
            Info.SuffixLen = Len - 1;
        } 
        else if (FirstStarPos == Len - 1 && LastStarPos == Len - 1 && Len > 1) 
        {
            /* Pattern ends with * only */

            Info.IsPrefix = true;
            Info.PrefixLen = Len - 1;
        }
        
        return Info;
    }
}

/* Comment goes here */

bool Wildcard::Match(const std::string& Str, const std::string& Pattern) 
{
    if (Pattern.empty()) 
    {
        return Str.empty();
    }
    
    /* Normalize pattern (collapse consecutive *) */

    std::string Normalized = NormalizePattern(Pattern);

    return Match(Str.c_str(), Normalized.c_str());
}

/* Comment goes here */

bool Wildcard::Match(const char* Str, const char* Pattern) 
{
    /* Input validation */

    if (!Str || !Pattern) 
    {
        return false;
    }
    
    /* Empty pattern matches only empty string */

    if (*Pattern == '\0') 
    {
        return *Str == '\0';
    }
    
    /* Empty string only matches if pattern is all stars */

    if (*Str == '\0') 
    {
        while (*Pattern == '*') 
        {
            Pattern++;
        }

        return *Pattern == '\0';
    }
    
    return MatchInternal(Str, Pattern);
}

/* Comment goes here */

bool Wildcard::MatchInternal(const char* Str, const char* Pattern) 
{
    /* Analyze pattern once to determine fast-path */

    PatternInfo Info = AnalyzePattern(Pattern);
    
    if (Info.IsExact) 
    {
        return MatchExact(Str, Pattern);
    }
    
    /* Get string length once (avoid multiple strlen calls) */

    size_t StrLen = std::strlen(Str);
    
    /* Fast-path: Prefix match (pattern ends with *) */

    if (Info.IsPrefix) 
    {
        if (StrLen < Info.PrefixLen) 
        {
            return false;
        }

        return MatchPrefix(Str, Pattern, Info.PrefixLen);
    }
    
    /* Fast-path: Suffix match (pattern starts with *) */

    if (Info.IsSuffix) 
    {
        if (StrLen < Info.SuffixLen) 
        {
            return false;
        }

        return MatchSuffix(Str, Pattern, StrLen, Info.SuffixLen);
    }
    
    /* Fast-path: Contains match (pattern is *substring*) */

    if (Info.IsContains) 
    {
        return MatchContains(Str, Pattern, StrLen, Info.ContainsLen);
    }
    
    /* General case: Use optimized backtracking algorithm for complex patterns */

    const char* Cp = nullptr;

    const char* Mp = nullptr;

    const char* StrStart = Str;

    /* Match characters until first * */

    while (*Str && *Pattern != '*') 
    {
        if (!CharEqualCaseInsensitive(*Pattern, *Str) && *Pattern != '?') 
        {
            return false;
        }

        Pattern++;
        Str++;
    }

    /* Handle remaining pattern with backtracking */

    while (*Str) 
    {
        if (*Pattern == '*') 
        {
            /* Skip consecutive stars (shouldn't happen after normalization, but be safe) */

            while (*Pattern == '*') 
            {
                Pattern++;
            }

            if (*Pattern == '\0') 
            {
                return true; /* Pattern ends with *, matches rest of string */
            }

            Mp = Pattern;
            Cp = Str + 1;
        } 
        else if (CharEqualCaseInsensitive(*Pattern, *Str) || *Pattern == '?') 
        {
            Pattern++;
            Str++;
        } 
        else 
        {
            /* Backtrack */

            if (Mp == nullptr) 
            {
                return false; /* No backtrack point */
            }

            Pattern = Mp;

            Str = Cp++;
            
            /* Safety: prevent infinite loop */

            if (Cp > StrStart + StrLen) 
            {
                return false;
            }
        }
    }

    /* Skip trailing stars */

    while (*Pattern == '*') 
    {
        Pattern++;
    }

    return *Pattern == '\0';
}

/* Comment goes here */

std::vector<std::string> Wildcard::Filter(const std::vector<std::string>& Strings, const std::string& Pattern) 
{
    if (Strings.empty()) 
    {
        return {};
    }
    
    std::vector<std::string> Results;
    
    /* Reserve space for better performance (estimate 10-20% match rate) */
    /* But cap at reasonable size to avoid over-allocation */

    size_t ReserveSize = std::min(Strings.size() / 5, Strings.size());

    if (ReserveSize < 8) 
    {
        ReserveSize = 8; /* Minimum reasonable reserve */
    }

    Results.reserve(ReserveSize);
    
    /* Normalize pattern once (reuse across all matches) */

    std::string Normalized = NormalizePattern(Pattern);

    const char* NormalizedCStr = Normalized.c_str();
    
    /* Optimize: use direct C-string matching to avoid string copies */
    
    for (const auto& Str : Strings) 
    {
        if (Match(Str.c_str(), NormalizedCStr)) 
        {
            Results.push_back(Str);
        }
    }
    
    /* Shrink to fit if we over-allocated significantly */

    if (Results.size() < Results.capacity() / 2) 
    {
        Results.shrink_to_fit();
    }
    
    return Results;
}
