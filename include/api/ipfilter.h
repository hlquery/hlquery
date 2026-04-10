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

#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <list>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"

/*
 * IP Filter Manager.
 *
 * Manages IP address filtering with DNS resolution support.
 * Supports:
 * - Multiple IP addresses (IPv4).
 * - CIDR subnet notation (e.g., 192.168.1.0/24).
 * - Hostname resolution (DNS).
 * - Wildcard hostnames (e.g., *.example.com).
 * - Wildcard "*" to allow all IPs.
 * - Deny lists for explicit blocks.
 * - Thread-safe operations.
 */

class CoreExport IPFilter
{
   private:

     /* Resolve hostname to IP addresses. */

     bool ResolveHostname(const std::string& Hostname, std::vector<std::string>& ResolvedIPs, bool UseCache = true) const;

     /* Check if string is a hostname. */

     bool IsHostname(const std::string& StrVal) const;

     /* Validate IP address format. */

     bool IsValidIP(const std::string& IP) const;

     /* Check if string is CIDR notation. */

     bool IsCIDR(const std::string& CIDR) const;

     /* Check if string is wildcard hostname. */

     bool IsWildcardHostname(const std::string& Hostname) const;

     /* Check if IP is in CIDR range. */

     bool IsIPInCIDR(const std::string& IP, const std::string& CIDR) const;

     /* Match hostname against wildcard pattern. */

     bool MatchWildcardHostname(const std::string& Hostname, const std::string& Pattern) const;

     /* Internal method to check IP address. */

     bool IsAllowedInternal(const std::string& IPAddress) const;

     /* Internal method to check IP address against deny list. */

     bool IsDeniedInternal(const std::string& IPAddress) const;

     /* Perform reverse DNS lookup. */

     std::string ReverseDNS(const std::string& IP) const;

     /* Parse comma-separated list of IPs/hostnames. */

     static std::vector<std::string> ParseIPList(const std::string& AllowedIPsConfig);

     /* Synchronizes filter updates. */

     mutable std::mutex MutexValue;

     /* Cached allowed IPs after resolution. */

     std::unordered_set<std::string> AllowedIPs;

     /* Direct allowed IPs without DNS lookup. */

     std::unordered_set<std::string> DirectIPs;

     /* Allowed CIDR ranges. */

     std::vector<std::string> CIDRRanges;

     /* Allowed wildcard hostnames. */

     std::vector<std::string> WildcardHostnames;

     /* Allowed regular hostnames. */

     std::vector<std::string> RegularHostnames;

     /* Original allowed entries from config. */

     std::vector<std::string> OriginalEntries;

     /* Cached denied IPs after resolution. */

     std::unordered_set<std::string> DeniedIPs;

     /* Direct denied IPs without DNS lookup. */

     std::unordered_set<std::string> DeniedDirectIPs;

     /* Denied CIDR ranges. */

     std::vector<std::string> DeniedCIDRRanges;

     /* Denied wildcard hostnames. */

     std::vector<std::string> DeniedWildcardHostnames;

     /* Denied regular hostnames. */

     std::vector<std::string> DeniedRegularHostnames;

     /* Original denied entries from config. */

     std::vector<std::string> DeniedOriginalEntries;

     /* Allow all IPs when true. */

     bool AllowAll;

     /* Deny all IPs when true. */

     bool DenyAll;

     /* True when any hostname entries are present. */

     bool HasHostnames;

     /* True when wildcard hostname entries are present. */

     bool HasWildcardHostnames;

     /* True when any deny entries are present. */

     bool HasDenyEntries;

     /* True when denied hostnames are present. */

     bool HasDenyHostnames;

     /* True when denied wildcard hostnames are present. */

     bool HasDenyWildcardHostnames;

     /* Synchronizes DNS cache access. */

     mutable std::mutex CacheMutex;

     /* Resolved hostname cache. */

     mutable std::unordered_map<std::string, std::vector<std::string>> DNSCache;

     /* Reverse DNS cache entries. */

     mutable std::unordered_map<std::string, std::string> ReverseDNSCache;

     /* DNS cache eviction order. */

     mutable std::list<std::string> DNSCacheOrder;

     /* Reverse DNS cache eviction order. */

     mutable std::list<std::string> ReverseDNSCacheOrder;

     /* Maximum size of DNS cache. */

     size_t DNSCacheMaxSize;

     /* Last DNS cache flush time. */

     std::chrono::steady_clock::time_point LastCacheFlush;

     /* Periodic DNS cache flush interval. */

     static constexpr std::chrono::hours CACHE_FLUSH_INTERVAL{1};

     /* Original configuration string. */

     std::string OriginalConfig;

   public:

     /* Constructor. */

     IPFilter();

     /* Destructor. */

     ~IPFilter();

     /* Initialize IP filter from allow configuration. */

     bool Initialize(const std::string& AllowedIPsConfig);

     /* Initialize IP filter from allow and deny configuration. */

     bool Initialize(const std::string& AllowedIPsConfig, const std::string& DeniedIPsConfig);

     /* Check if an IP address or hostname is allowed. */

     bool IsAllowed(const std::string& IPOrHostname) const;

     /* Check if IP filtering is enabled. */

     bool IsEnabled() const
     {
          return !AllowAll || HasDenyEntries || DenyAll;
     }

     /* Check if DNS resolution is enabled. */

     bool IsDNSEnabled() const
     {
          return HasHostnames || HasWildcardHostnames;
     }

     /* Get list of allowed IP addresses (resolved). */

     std::vector<std::string> GetAllowedIPs() const;

     /* Get list of original allowed entries (before resolution). */

     std::vector<std::string> GetOriginalEntries() const;

     /* Get list of denied IP addresses (resolved). */

     std::vector<std::string> GetDeniedIPs() const;

     /* Get list of original denied entries (before resolution). */

     std::vector<std::string> GetDeniedEntries() const;

     /* Check if deny-all is enabled. */

     bool IsDenyAll() const
     {
          return DenyAll;
     }

     /* Check if deny entries are configured. */

     bool HasDenyList() const
     {
          return HasDenyEntries;
     }

     /* Add an IP address or hostname to the allowed list. */

     bool AddAllowed(const std::string& IPOrHostname);

     /* Clear all allowed IPs and reset to allow all. */

     void Clear();

     /* Reload allow configuration. */

     bool Reload(const std::string& AllowedIPsConfig);

     /* Reload allow and deny configuration. */

     bool Reload(const std::string& AllowedIPsConfig, const std::string& DeniedIPsConfig);

     /* Flush DNS cache. */

     void FlushDNSCache();

     /* Configure maximum DNS cache entries. */

     void SetDNSCacheMaxSize(size_t max_size);

     /* Get hostname for an IP address. */

     std::string GetHostnameForIP(const std::string& IP) const;

     /* Resolve hostname to a single IP address. */

     bool ResolveHostnameToIP(const std::string& Hostname, std::string& ResolvedIP, bool UseCache = true) const;
};
