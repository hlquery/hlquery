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
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>

#include "api/ip_filter.h"
#include "core/hlquery.h"
#include "core/hlquery.h"
#include "core/logmanager.h"


IPFilter::IPFilter() : allow_all(true), has_hostnames(false), has_wildcard_hostnames(false),
    last_cache_flush(Instance->Now())
{

}

IPFilter::~IPFilter() 
{

}

bool IPFilter::Initialize(const std::string& allowed_ips_config)
{
    std::lock_guard<std::mutex> lock(mutex);
    
    original_config = allowed_ips_config;
    allow_all = false;
    allowed_ips.clear();
    direct_ips.clear();
    cidr_ranges.clear();
    wildcard_hostnames.clear();
    regular_hostnames.clear();
    original_entries.clear();
    has_hostnames = false;
    has_wildcard_hostnames = false;
    
    /* Clear DNS cache (need to access mutable members) */
    
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);
        dns_cache.clear();
        reverse_dns_cache.clear();
        dns_cache_order.clear();
        reverse_dns_cache_order.clear();
    }
    
    /* Check for wildcard */
    
    std::string trimmed = allowed_ips_config;

    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    /* Empty config means deny all (no entries = deny all) */
    
    if (trimmed.empty())
    {
        allow_all = false;
        has_hostnames = false;
        has_wildcard_hostnames = false;

        if (Instance && Instance->Logs)
        {
            Instance->Logs->Normal("ip_allow", "IP allow filter: deny all (no entries configured)");
        }
        return true;
    }
    
    /* Explicit wildcard "*" means allow all */

    if (trimmed == "*")
    {
        allow_all = true;
        has_hostnames = false;
        has_wildcard_hostnames = false;

        if (Instance && Instance->Logs)
        {
            Instance->Logs->Normal("ip_allow", "IP allow filtering disabled (wildcard * - allowing all IPs, no DNS resolution needed)");
        }
        return true;
    }
    
    /* Parse IP list */

    std::vector<std::string> ip_list = ParseIPList(allowed_ips_config);
    
    for (const auto& entry : ip_list)
    {
        original_entries.push_back(entry);
        
        /* Check for CIDR notation */

        if (IsCIDR(entry))
        {
            cidr_ranges.push_back(entry);

            if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
            {
                Instance->Logs->Debug("ip_allow", "Added CIDR range: " + entry);
            }
            continue;
        }
        
        /* Check for wildcard hostname */

        if (IsWildcardHostname(entry))
        {
            wildcard_hostnames.push_back(entry);
            has_wildcard_hostnames = true;
            has_hostnames = true;

            if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
            {
                Instance->Logs->Debug("ip_allow", "Added wildcard hostname pattern: " + entry);
            }
            continue;
        }
        
        /* Check if it's a hostname (not an IP) */

        if (IsHostname(entry))
        {
            regular_hostnames.push_back(entry);
            has_hostnames = true;

            /* Resolve hostname at startup (only if we have hostnames configured) */

            std::vector<std::string> resolved_ips;

            if (ResolveHostname(entry, resolved_ips, true))
            {
                /* Already holding mutex from Initialize() - no need to lock again */

                for (const auto& ip : resolved_ips)
                {
                    allowed_ips.insert(ip);
                }

                if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                {
                    std::string resolved_str;

                    for (size_t i = 0; i < resolved_ips.size(); ++i)
                    {
                        if (i > 0)
                        {
                            resolved_str += ", ";
                        }
                        resolved_str += resolved_ips[i];
                    }
                    Instance->Logs->Debug("ip_allow", "Resolved hostname " + entry + " -> " + resolved_str);
                }
            }
            else
            {
                if (Instance && Instance->Logs)
                {
                    Instance->Logs->Normal("ip_allow", "Failed to resolve hostname: " + entry);
                }
            }
            continue;
        }
        
        /* It's an IP address */

        if (IsValidIP(entry))
        {
            /* Already holding mutex from Initialize() - no need to lock again */

            allowed_ips.insert(entry);

            /* Track as direct IP (not from hostname) */

            direct_ips.insert(entry);

            if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
            {
                Instance->Logs->Debug("ip_allow", "Added IP address: " + entry);
            }
        }
        else
        {
            if (Instance && Instance->Logs)
            {
                Instance->Logs->Normal("ip_allow", "Invalid IP address or hostname: " + entry);
            }
        }
    }
    
    if (Instance && Instance->Logs)
    {
        std::string dns_info = "";

        if (has_hostnames)
        {
            dns_info = " (DNS resolution enabled)";
        }
        else
        {
            dns_info = " (no DNS resolution needed)";
        }
        Instance->Logs->Normal("ip_allow", "IP allow filter initialized with " + 
            std::to_string(allowed_ips.size()) + " IP(s), " +
            std::to_string(cidr_ranges.size()) + " CIDR range(s), " +
            std::to_string(regular_hostnames.size()) + " hostname(s), " +
            std::to_string(wildcard_hostnames.size()) + " wildcard hostname(s)" + dns_info);
    }
    
    return true;
}

bool IPFilter::IsAllowed(const std::string& ip_or_hostname) const
{
    std::lock_guard<std::mutex> lock(mutex);
    
    /* If wildcard mode, allow all (no DNS needed) */

    if (allow_all)
    {
        return true;
    }
    
    /* If input is a hostname, resolve it first (using cache) */

    std::string check_ip = ip_or_hostname;

    if (IsHostname(ip_or_hostname) && has_hostnames)
    {
        std::vector<std::string> resolved_ips;

        if (ResolveHostname(ip_or_hostname, resolved_ips, true))
        {
            /* Check all resolved IPs - if any match, allow */

            for (const auto& resolved_ip : resolved_ips)
            {
                if (IsAllowedInternal(resolved_ip))
                {
                    return true;
                }
            }

            /* None of the resolved IPs matched */

            return false;
        }
        else
        {
            /* DNS resolution failed - deny */

            return false;
        }
    }
    
    /* Input is an IP address, check directly */

    return IsAllowedInternal(check_ip);
}

bool IPFilter::IsAllowedInternal(const std::string& ip_address) const
{
    /* Check if IP is in allowed set */

    if (allowed_ips.find(ip_address) != allowed_ips.end())
    {
        return true;
    }
    
    /* Check CIDR ranges */

    for (const auto& cidr : cidr_ranges)
    {
        if (IsIPInCIDR(ip_address, cidr))
        {
            return true;
        }
    }
    
    /* Check wildcard hostnames (only if we have wildcard patterns configured) */

    if (has_wildcard_hostnames && !wildcard_hostnames.empty())
    {
        std::string hostname = ReverseDNS(ip_address);

        if (!hostname.empty())
        {
            for (const auto& pattern : wildcard_hostnames)
            {
                if (MatchWildcardHostname(hostname, pattern))
                {
                    return true;
                }
            }
        }
    }
    
    return false;
}

std::vector<std::string> IPFilter::GetAllowedIPs() const
{
    std::lock_guard<std::mutex> lock(mutex);
    
    std::vector<std::string> result;

    result.reserve(allowed_ips.size() + cidr_ranges.size() + wildcard_hostnames.size());
    
    /* Add resolved IPs */

    for (const auto& ip : allowed_ips)
    {
        result.push_back(ip);
    }
    
    /* Add CIDR ranges */

    for (const auto& cidr : cidr_ranges)
    {
        result.push_back(cidr);
    }
    
    /* Add wildcard hostnames */

    for (const auto& pattern : wildcard_hostnames)
    {
        result.push_back(pattern);
    }
    
    return result;
}

std::vector<std::string> IPFilter::GetOriginalEntries() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return original_entries;
}

bool IPFilter::AddAllowed(const std::string& ip_or_hostname)
{
    /* This method is now only used internally - hostname resolution is done in Initialize() */

    if (IsValidIP(ip_or_hostname))
    {
        std::lock_guard<std::mutex> lock(mutex);
        allowed_ips.insert(ip_or_hostname);

        /* Track as direct IP */

        direct_ips.insert(ip_or_hostname);
        return true;
    }
    return false;
}

void IPFilter::Clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    allowed_ips.clear();
    direct_ips.clear();
    allow_all = true;
    original_config.clear();
}

bool IPFilter::Reload(const std::string& allowed_ips_config)
{
    return Initialize(allowed_ips_config);
}

void IPFilter::FlushDNSCache()
{
    size_t dns_count = 0;
    size_t reverse_count = 0;
    
    /* Clear caches */

    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);
        dns_count = dns_cache.size();
        reverse_count = reverse_dns_cache.size();
        
        dns_cache.clear();
        reverse_dns_cache.clear();
        dns_cache_order.clear();
        reverse_dns_cache_order.clear();
    }
    
    /* 
     * Re-resolve configured hostnames and update allowed_ips
     * Preserve direct IPs (not from hostname resolution)
     */
     
    if (has_hostnames && !regular_hostnames.empty()) 
    {
        std::lock_guard<std::mutex> lock(mutex);
        
        /*
         * Remove only hostname-resolved IPs (keep direct IPs)
         * Rebuild allowed_ips from direct_ips first
         */

        allowed_ips = direct_ips;
        
        /* Re-resolve all regular hostnames and add their IPs */

        for (const auto& hostname : regular_hostnames)
        {
            std::vector<std::string> resolved_ips;

            /* Force fresh lookup (use_cache=false) to get latest IPs */

            if (ResolveHostname(hostname, resolved_ips, false))
            {
                for (const auto& ip : resolved_ips)
                {
                    allowed_ips.insert(ip);
                }

                if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                {
                    std::string resolved_str;

                    for (size_t i = 0; i < resolved_ips.size(); ++i)
                    {
                        if (i > 0)
                        {
                            resolved_str += ", ";
                        }
                        resolved_str += resolved_ips[i];
                    }
                    Instance->Logs->Debug("ip_allow", "Re-resolved hostname " + hostname + " -> " + resolved_str + " (after cache flush)");
                }
            }
            else
            {
                if (Instance && Instance->Logs)
                {
                    Instance->Logs->Normal("ip_allow", "Failed to re-resolve hostname after cache flush: " + hostname);
                }
            }
        }
    }
    
    last_cache_flush = Instance->Now();
    
    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
    {
        Instance->Logs->Debug("ip_allow", "DNS cache flushed: " + 
            std::to_string(dns_count) + " forward, " + 
            std::to_string(reverse_count) + " reverse entries cleared" +
            (has_hostnames && !regular_hostnames.empty() ? " (hostnames re-resolved)" : ""));
    }
}

bool IPFilter::ResolveHostname(const std::string& hostname, std::vector<std::string>& resolved_ips, bool use_cache) const
{
    /* Check cache first (with lock held) */

    if (use_cache)
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);
        auto it = dns_cache.find(hostname);

        if (it != dns_cache.end())
        {
            resolved_ips = it->second;
            return !resolved_ips.empty();
        }
    }
    
    /* Perform DNS lookup (without lock to avoid blocking other threads) */

    struct addrinfo hints;
    struct addrinfo* result = nullptr;
    
    std::memset(&hints, 0, sizeof(hints));

    /* Only IPv4 */

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);

    if (status != 0)
    {
        if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
        {
            Instance->Logs->Debug("ip_allow", "DNS resolution failed for " + hostname + ": " + gai_strerror(status));
        }
        return false;
    }
    
    /* Extract IP addresses */

    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next)
    {
        if (rp->ai_family == AF_INET)
        {
            struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
            char ip_str[INET_ADDRSTRLEN];

            if (inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN) != nullptr)
            {
                resolved_ips.push_back(std::string(ip_str));
            }
        }
    }
    
    freeaddrinfo(result);
    
    /*
     * Cache the result (limit cache size to MAX_CACHE_SIZE)
     * Fix Bug 2: Check cache again after acquiring lock (another thread might have added it)
     */

    if (use_cache && !resolved_ips.empty())
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);

        /* Double-check: another thread might have resolved this while we were doing DNS lookup */

        auto it = dns_cache.find(hostname);

        if (it != dns_cache.end())
        {
            /* Another thread beat us to it, use their result */

            resolved_ips = it->second;
            return !resolved_ips.empty();
        }
        
        /*
         * Flush entire cache when it reaches MAX_CACHE_SIZE
         */

        if (dns_cache.size() >= MAX_CACHE_SIZE)
        {
            /* Flush entire cache when limit reached */
            dns_cache.clear();
            dns_cache_order.clear();

            if (Instance && Instance->Logs)
            {
                Instance->Logs->Debug("ip_allow", "DNS cache reached " + std::to_string(MAX_CACHE_SIZE) + " items, flushing everything.");
            }
        }
        
        /* Add new entry */

        dns_cache[hostname] = resolved_ips;
        dns_cache_order.push_back(hostname);
    }
    
    return !resolved_ips.empty();
}

bool IPFilter::IsHostname(const std::string& str) const 
{
    /* If it's a valid IP, it's not a hostname */
    
    if (IsValidIP(str)) 
    {
        return false;
    }
    
    /* If it's CIDR, it's not a hostname */
    
    if (IsCIDR(str)) 
    {
        return false;
    }
    
    /*
     * If it contains letters or dots (and not just numbers/dots), likely a hostname
     * Simple heuristic: contains at least one letter
     */

    for (char c : str)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        {
            return true;
        }
    }
    
    return false;
}

bool IPFilter::IsValidIP(const std::string& ip) const
{
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) == 1;
}

bool IPFilter::IsCIDR(const std::string& cidr) const
{
    size_t slash_pos = cidr.find('/');

    if (slash_pos == std::string::npos || slash_pos == 0 || slash_pos == cidr.length() - 1)
    {
        return false;
    }
    
    std::string ip_part = cidr.substr(0, slash_pos);
    std::string mask_part = cidr.substr(slash_pos + 1);
    
    /* Validate IP part */

    if (!IsValidIP(ip_part))
    {
        return false;
    }
    
    /* Validate mask (0-32) */

    try
    {
        int mask = std::stoi(mask_part);
        return mask >= 0 && mask <= 32;
    }
    catch (...)
    {
        return false;
    }
}

bool IPFilter::IsWildcardHostname(const std::string& hostname) const
{
    return hostname.find('*') != std::string::npos;
}

bool IPFilter::IsIPInCIDR(const std::string& ip, const std::string& cidr) const
{
    size_t slash_pos = cidr.find('/');

    if (slash_pos == std::string::npos)
    {
        return false;
    }
    
    std::string network_str = cidr.substr(0, slash_pos);
    int mask_bits = std::stoi(cidr.substr(slash_pos + 1));
    
    struct sockaddr_in network_addr, ip_addr;

    if (inet_pton(AF_INET, network_str.c_str(), &(network_addr.sin_addr)) != 1)
    {
        return false;
    }

    if (inet_pton(AF_INET, ip.c_str(), &(ip_addr.sin_addr)) != 1)
    {
        return false;
    }
    
    uint32_t network = ntohl(network_addr.sin_addr.s_addr);
    uint32_t address = ntohl(ip_addr.sin_addr.s_addr);
    uint32_t mask = (0xFFFFFFFF << (32 - mask_bits)) & 0xFFFFFFFF;
    
    return (address & mask) == (network & mask);
}

bool IPFilter::MatchWildcardHostname(const std::string& hostname, const std::string& pattern) const
{
    /* Simple wildcard matching: *.example.com matches api.example.com, www.example.com, etc. */

    if (pattern == "*")
    {
        return true;
    }
    
    if (pattern.front() == '*')
    {
        /* Pattern starts with * (e.g., *.example.com) */

        std::string suffix = pattern.substr(1);

        if (suffix.front() == '.')
        {
            /* Remove leading dot */

            suffix = suffix.substr(1);
        }
        
        /* Check if hostname ends with suffix */

        if (hostname.length() >= suffix.length())
        {
            std::string hostname_suffix = hostname.substr(hostname.length() - suffix.length());
            return hostname_suffix == suffix;
        }
    }
    
    /* Exact match */

    return hostname == pattern;
}

std::string IPFilter::ReverseDNS(const std::string& ip) const
{
    /* Check cache first (with lock held) */

    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);
        auto it = reverse_dns_cache.find(ip);

        if (it != reverse_dns_cache.end())
        {
            return it->second;
        }
    }
    
    /* Perform reverse DNS lookup (without lock to avoid blocking other threads) */

    struct sockaddr_in sa;

    if (inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) != 1)
    {
        return "";
    }
    
    char hostname[NI_MAXHOST];
    int result = getnameinfo(reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa),
                            hostname, NI_MAXHOST, nullptr, 0, 0);
    
    if (result != 0)
    {
        return "";
    }
    
    std::string hostname_str(hostname);
    
    /*
     * Cache the result (limit cache size to MAX_CACHE_SIZE)
     * Fix Bug 2: Check cache again after acquiring lock (another thread might have added it)
     */

    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex);

        /* Double-check: another thread might have resolved this while we were doing DNS lookup */

        auto it = reverse_dns_cache.find(ip);

        if (it != reverse_dns_cache.end())
        {
            /* Another thread beat us to it, use their result */

            return it->second;
        }
        
        /*
         * Flush entire cache when it reaches MAX_CACHE_SIZE
         */

        if (reverse_dns_cache.size() >= MAX_CACHE_SIZE)
        {
            /* Flush entire cache when limit reached */
            reverse_dns_cache.clear();
            reverse_dns_cache_order.clear();

            if (Instance && Instance->Logs)
            {
                Instance->Logs->Debug("ip_allow", "Reverse DNS cache reached " + std::to_string(MAX_CACHE_SIZE) + " items, flushing everything.");
            }
        }
        
        /* Add new entry */

        reverse_dns_cache[ip] = hostname_str;
        reverse_dns_cache_order.push_back(ip);
    }
    
    return hostname_str;
}

std::string IPFilter::GetHostnameForIP(const std::string& ip) const
{
    /* Only perform reverse DNS if DNS is enabled */

    if (!IsDNSEnabled())
    {
        return "";
    }
    return ReverseDNS(ip);
}

bool IPFilter::ResolveHostnameToIP(const std::string& hostname, std::string& resolved_ip, bool use_cache) const
{
    std::vector<std::string> resolved_ips;

    if (!ResolveHostname(hostname, resolved_ips, use_cache))
    {
        return false;
    }
    
    if (resolved_ips.empty())
    {
        return false;
    }
    
    /* Return the first resolved IP address */

    resolved_ip = resolved_ips[0];
    return true;
}

std::vector<std::string> IPFilter::ParseIPList(const std::string& allowed_ips_config)
{
    std::vector<std::string> result;
    std::stringstream ss(allowed_ips_config);
    std::string item;
    
    while (std::getline(ss, item, ','))
    {
        /* Trim whitespace */

        item.erase(0, item.find_first_not_of(" \t\n\r"));
        item.erase(item.find_last_not_of(" \t\n\r") + 1);
        
        if (!item.empty())
        {
            result.push_back(item);
        }
    }
    
    return result;
}
