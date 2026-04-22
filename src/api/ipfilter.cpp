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
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>

#include "api/ipfilter.h"
#include "core/hlquery.h"
#include "core/logmanager.h"

IPFilter::IPFilter() : AllowAll(true),
                       DenyAll(false), HasHostnames(false), HasWildcardHostnames(false), HasDenyEntries(false), HasDenyHostnames(false), HasDenyWildcardHostnames(false), DNSCacheMaxSize(DNS_CACHE_MAX_SIZE), LastCacheFlush(Instance->Now())
{

}

/* IPFilter destructor. */

IPFilter::~IPFilter()
{

}

/* Initialize IP filter from allow configuration. */

bool IPFilter::Initialize(const std::string &AllowedIPsConfig)
{
     return Initialize(AllowedIPsConfig, "");
}

/* Initialize IP filter from allow and deny configuration. */

bool IPFilter::Initialize(const std::string &AllowedIPsConfig, const std::string &DeniedIPsConfig)
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     /* Configure DNS cache size from config when present. */

     if (Instance && Instance->Config)
     {
          size_t ConfigCacheSize = Instance->Config->GetDNSCacheMaxSize();
          DNSCacheMaxSize = (ConfigCacheSize > 0) ? ConfigCacheSize : DNS_CACHE_MAX_SIZE;
     }

     /* Preserve original configuration. */

     OriginalConfig = AllowedIPsConfig;

     /* Reset state to a clean slate. */

     AllowAll = false;
     DenyAll = false;
     HasHostnames = false;
     HasWildcardHostnames = false;
     HasDenyEntries = false;
     HasDenyHostnames = false;
     HasDenyWildcardHostnames = false;
     AllowedIPs.clear();
     DirectIPs.clear();
     CIDRRanges.clear();
     WildcardHostnames.clear();
     RegularHostnames.clear();
     OriginalEntries.clear();
     DeniedIPs.clear();
     DeniedDirectIPs.clear();
     DeniedCIDRRanges.clear();
     DeniedWildcardHostnames.clear();
     DeniedRegularHostnames.clear();
     DeniedOriginalEntries.clear();

     /* Reset DNS caches. */

     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          DNSCache.clear();

          ReverseDNSCache.clear();

          DNSCacheOrder.clear();

          ReverseDNSCacheOrder.clear();
     }

     /* Normalize allow list input. */

     std::string AllowTrimmed = AllowedIPsConfig;

     AllowTrimmed.erase(0, AllowTrimmed.find_first_not_of(" \t\n\r"));

     AllowTrimmed.erase(AllowTrimmed.find_last_not_of(" \t\n\r") + 1);

     if (AllowTrimmed.empty())
     {
          AllowAll = false;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("ip_allow", "IP allow filter: deny all (no entries configured).");
          }
     }
     else if (AllowTrimmed == "*")
     {
          AllowAll = true;

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("ip_allow", "IP allow filter set to allow all (wildcard * - no DNS resolution needed).");
          }
     }
     else
     {
          std::vector<std::string> IPList = ParseIPList(AllowedIPsConfig);

          for (const auto &Entry : IPList)
          {
               OriginalEntries.push_back(Entry);

               if (IsCIDR(Entry))
               {
                    CIDRRanges.push_back(Entry);

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("ip_allow", "Added CIDR range: " + Entry + ".");
                    }

                    continue;
               }

               if (IsWildcardHostname(Entry))
               {
                    WildcardHostnames.push_back(Entry);

                    HasWildcardHostnames = true;

                    HasHostnames = true;

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("ip_allow", "Added wildcard hostname pattern: " + Entry + ".");
                    }

                    continue;
               }

               if (IsHostname(Entry))
               {
                    RegularHostnames.push_back(Entry);

                    HasHostnames = true;

                    std::vector<std::string> ResolvedIPs;

                    if (ResolveHostname(Entry, ResolvedIPs, true))
                    {
                         for (const auto &IP : ResolvedIPs)
                         {
                              AllowedIPs.insert(IP);
                         }

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              std::string ResolvedStr;

                              for (size_t Index = 0; Index < ResolvedIPs.size(); ++Index)
                              {
                                   if (Index > 0)
                                   {
                                        ResolvedStr += ", ";
                                   }

                                   ResolvedStr += ResolvedIPs[Index];
                              }

                              Instance->Logs->Debug("ip_allow", "Resolved hostname " + Entry + " -> " + ResolvedStr + ".");
                         }
                    }
                    else
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("ip_allow", "Failed to resolve hostname: " + Entry + ".");
                         }
                    }

                    continue;
               }

               if (IsValidIP(Entry))
               {
                    AllowedIPs.insert(Entry);

                    DirectIPs.insert(Entry);

                    if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                    {
                         Instance->Logs->Debug("ip_allow", "Added IP address: " + Entry + ".");
                    }
               }
               else
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("ip_allow", "Invalid IP address or hostname: " + Entry + ".");
                    }
               }
          }
     }

     /* Normalize deny list input. */

     std::string DenyTrimmed = DeniedIPsConfig;

     DenyTrimmed.erase(0, DenyTrimmed.find_first_not_of(" \t\n\r"));

     DenyTrimmed.erase(DenyTrimmed.find_last_not_of(" \t\n\r") + 1);

     if (!DenyTrimmed.empty())
     {
          if (DenyTrimmed == "*")
          {
               DenyAll = true;

               HasDenyEntries = true;

               DeniedOriginalEntries.push_back("*");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("ip_deny", "IP deny filter: deny all (wildcard *).");
               }
          }
          else
          {
               std::vector<std::string> DenyList = ParseIPList(DeniedIPsConfig);

               for (const auto &Entry : DenyList)
               {
                    DeniedOriginalEntries.push_back(Entry);

                    if (IsCIDR(Entry))
                    {
                         DeniedCIDRRanges.push_back(Entry);

                         HasDenyEntries = true;

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("ip_deny", "Added CIDR range: " + Entry + ".");
                         }

                         continue;
                    }

                    if (IsWildcardHostname(Entry))
                    {
                         DeniedWildcardHostnames.push_back(Entry);

                         HasDenyEntries = true;

                         HasDenyWildcardHostnames = true;

                         HasDenyHostnames = true;

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("ip_deny", "Added wildcard hostname pattern: " + Entry + ".");
                         }

                         continue;
                    }

                    if (IsHostname(Entry))
                    {
                         DeniedRegularHostnames.push_back(Entry);

                         HasDenyEntries = true;

                         HasDenyHostnames = true;

                         std::vector<std::string> ResolvedIPs;

                         if (ResolveHostname(Entry, ResolvedIPs, true))
                         {
                              for (const auto &IP : ResolvedIPs)
                              {
                                   DeniedIPs.insert(IP);
                              }

                              if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                              {
                                   std::string ResolvedStr;

                                   for (size_t Index = 0; Index < ResolvedIPs.size(); ++Index)
                                   {
                                        if (Index > 0)
                                        {
                                             ResolvedStr += ", ";
                                        }

                                        ResolvedStr += ResolvedIPs[Index];
                                   }

                                   Instance->Logs->Debug("ip_deny", "Resolved hostname " + Entry + " -> " + ResolvedStr + ".");
                              }
                         }
                         else
                         {
                              if (Instance && Instance->Logs)
                              {
                                   Instance->Logs->Normal("ip_deny", "Failed to resolve hostname: " + Entry + ".");
                              }
                         }

                         continue;
                    }

                    if (IsValidIP(Entry))
                    {
                         DeniedIPs.insert(Entry);

                         DeniedDirectIPs.insert(Entry);

                         HasDenyEntries = true;

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("ip_deny", "Added IP address: " + Entry + ".");
                         }
                    }
                    else
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("ip_deny", "Invalid IP address or hostname: " + Entry + ".");
                         }
                    }
               }
          }
     }

     /* Ensure deny hostnames also enable DNS support. */

     HasHostnames = HasHostnames || HasDenyHostnames;

     HasWildcardHostnames = HasWildcardHostnames || HasDenyWildcardHostnames;

     if (!AllowAll && !OriginalEntries.empty())
     {
          if (Instance && Instance->Logs)
          {
               std::string DNSInfo = HasHostnames ? " (DNS resolution enabled)" : " (no DNS resolution needed)";

               Instance->Logs->Normal("ip_allow", "IP allow filter initialized with " + std::to_string(AllowedIPs.size()) + " IP(s), " + std::to_string(CIDRRanges.size()) + " CIDR range(s), " + std::to_string(RegularHostnames.size()) + " hostname(s), " + std::to_string(WildcardHostnames.size()) + " wildcard hostname(s)" + DNSInfo + ".");
          }
     }

     if (HasDenyEntries && !DenyAll)
     {
          if (Instance && Instance->Logs)
          {
               std::string DNSInfo = HasDenyHostnames ? " (DNS resolution enabled)" : " (no DNS resolution needed)";

               Instance->Logs->Normal("ip_deny", "IP deny filter initialized with " + std::to_string(DeniedIPs.size()) + " IP(s), " + std::to_string(DeniedCIDRRanges.size()) + " CIDR range(s), " + std::to_string(DeniedRegularHostnames.size()) + " hostname(s), " + std::to_string(DeniedWildcardHostnames.size()) + " wildcard hostname(s)" + DNSInfo + ".");
          }
     }

     return true;
}

/* Check if an IP address or hostname is allowed. */

bool IPFilter::IsAllowed(const std::string &IPOrHostname) const
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     if (DenyAll)
     {
          return false;
     }

     if (HasDenyEntries)
     {
          if (IsHostname(IPOrHostname))
          {
               std::vector<std::string> ResolvedIPs;

               if (ResolveHostname(IPOrHostname, ResolvedIPs, true))
               {
                    for (const auto &ResolvedIP : ResolvedIPs)
                    {
                         if (IsDeniedInternal(ResolvedIP))
                         {
                              return false;
                         }
                    }
               }
          }
          else
          {
               if (IsDeniedInternal(IPOrHostname))
               {
                    return false;
               }
          }
     }

     if (AllowAll)
     {
          return true;
     }

     std::string CheckIP = IPOrHostname;

     if (IsHostname(IPOrHostname) && HasHostnames)
     {
          std::vector<std::string> ResolvedIPs;

          if (ResolveHostname(IPOrHostname, ResolvedIPs, true))
          {
               for (const auto &ResolvedIP : ResolvedIPs)
               {
                    if (IsAllowedInternal(ResolvedIP))
                    {
                         return true;
                    }
               }

               return false;
          }
          else
          {
               return false;
          }
     }

     return IsAllowedInternal(CheckIP);
}

/* Internal method to check IP address. */

bool IPFilter::IsAllowedInternal(const std::string &IPAddress) const
{
     if (AllowedIPs.find(IPAddress) != AllowedIPs.end())
     {
          return true;
     }

     for (const auto &CIDR : CIDRRanges)
     {
          if (IsIPInCIDR(IPAddress, CIDR))
          {
               return true;
          }
     }

     if (HasWildcardHostnames && !WildcardHostnames.empty())
     {
          std::string Hostname = ReverseDNS(IPAddress);

          if (!Hostname.empty())
          {
               for (const auto &Pattern : WildcardHostnames)
               {
                    if (MatchWildcardHostname(Hostname, Pattern))
                    {
                         return true;
                    }
               }
          }
     }

     return false;
}

/* Internal method to check IP address against deny list. */

bool IPFilter::IsDeniedInternal(const std::string &IPAddress) const
{
     if (DeniedIPs.find(IPAddress) != DeniedIPs.end())
     {
          return true;
     }

     for (const auto &CIDR : DeniedCIDRRanges)
     {
          if (IsIPInCIDR(IPAddress, CIDR))
          {
               return true;
          }
     }

     if (HasDenyWildcardHostnames && !DeniedWildcardHostnames.empty())
     {
          std::string Hostname = ReverseDNS(IPAddress);

          if (!Hostname.empty())
          {
               for (const auto &Pattern : DeniedWildcardHostnames)
               {
                    if (MatchWildcardHostname(Hostname, Pattern))
                    {
                         return true;
                    }
               }
          }
     }

     return false;
}

/* Get list of allowed IP addresses. */

std::vector<std::string> IPFilter::GetAllowedIPs() const
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     std::vector<std::string> Result;

     Result.reserve(AllowedIPs.size() + CIDRRanges.size() + WildcardHostnames.size());

     for (const auto &IP : AllowedIPs)
     {
          Result.push_back(IP);
     }

     for (const auto &CIDR : CIDRRanges)
     {
          Result.push_back(CIDR);
     }

     for (const auto &Pattern : WildcardHostnames)
     {
          Result.push_back(Pattern);
     }

     return Result;
}

/* Get list of original allowed entries. */

std::vector<std::string> IPFilter::GetOriginalEntries() const
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     return OriginalEntries;
}

/* Get list of denied IP addresses. */

std::vector<std::string> IPFilter::GetDeniedIPs() const
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     std::vector<std::string> Result;

     Result.reserve(DeniedIPs.size() + DeniedCIDRRanges.size() + DeniedWildcardHostnames.size());

     for (const auto &IP : DeniedIPs)
     {
          Result.push_back(IP);
     }

     for (const auto &CIDR : DeniedCIDRRanges)
     {
          Result.push_back(CIDR);
     }

     for (const auto &Pattern : DeniedWildcardHostnames)
     {
          Result.push_back(Pattern);
     }

     return Result;
}

/* Get list of original denied entries. */

std::vector<std::string> IPFilter::GetDeniedEntries() const
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     return DeniedOriginalEntries;
}

/* Add an IP address or hostname to the allowed list. */

bool IPFilter::AddAllowed(const std::string &IPOrHostname)
{
     if (IsValidIP(IPOrHostname))
     {
          std::lock_guard<std::mutex> Lock(MutexValue);

          AllowedIPs.insert(IPOrHostname);

          DirectIPs.insert(IPOrHostname);

          return true;
     }

     return false;
}

/* Clear all allowed IPs. */

void IPFilter::Clear()
{
     std::lock_guard<std::mutex> Lock(MutexValue);

     AllowedIPs.clear();
     DirectIPs.clear();
     CIDRRanges.clear();
     WildcardHostnames.clear();
     RegularHostnames.clear();
     OriginalEntries.clear();
     DeniedIPs.clear();
     DeniedDirectIPs.clear();
     DeniedCIDRRanges.clear();
     DeniedWildcardHostnames.clear();
     DeniedRegularHostnames.clear();
     DeniedOriginalEntries.clear();
     AllowAll = true;
     DenyAll = false;
     HasDenyEntries = false;
     HasDenyHostnames = false;
     HasDenyWildcardHostnames = false;
     HasHostnames = false;
     HasWildcardHostnames = false;
     OriginalConfig.clear();
}

/* Reload allow configuration. */

bool IPFilter::Reload(const std::string &AllowedIPsConfig)
{
     return Initialize(AllowedIPsConfig, "");
}

/* Reload allow and deny configuration. */

bool IPFilter::Reload(const std::string &AllowedIPsConfig, const std::string &DeniedIPsConfig)
{
     return Initialize(AllowedIPsConfig, DeniedIPsConfig);
}

/* Flush DNS cache. */

void IPFilter::FlushDNSCache()
{
     size_t DNSCount = 0;

     size_t ReverseCount = 0;

     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          DNSCount = DNSCache.size();

          ReverseCount = ReverseDNSCache.size();

          DNSCache.clear();

          ReverseDNSCache.clear();

          DNSCacheOrder.clear();

          ReverseDNSCacheOrder.clear();
     }

     if (HasHostnames && !RegularHostnames.empty())
     {
          std::lock_guard<std::mutex> Lock(MutexValue);

          AllowedIPs = DirectIPs;

          for (const auto &Hostname : RegularHostnames)
          {
               std::vector<std::string> ResolvedIPs;

               if (!ResolveHostname(Hostname, ResolvedIPs, false))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("ip_allow", "Failed to re-resolve hostname after cache flush: " + Hostname + ".");
                    }

                    continue;
               }

               for (const auto &IP : ResolvedIPs)
               {
                    AllowedIPs.insert(IP);
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    std::string ResolvedStr;

                    for (size_t Index = 0; Index < ResolvedIPs.size(); ++Index)
                    {
                         if (Index > 0)
                         {
                              ResolvedStr += ", ";
                         }

                         ResolvedStr += ResolvedIPs[Index];
                    }

                    Instance->Logs->Debug("ip_allow", "Re-resolved hostname " + Hostname + " -> " + ResolvedStr + " (after cache flush).");
               }
          }
     }

     if (HasDenyHostnames && !DeniedRegularHostnames.empty())
     {
          std::lock_guard<std::mutex> Lock(MutexValue);

          DeniedIPs = DeniedDirectIPs;

          for (const auto &Hostname : DeniedRegularHostnames)
          {
               std::vector<std::string> ResolvedIPs;

               if (!ResolveHostname(Hostname, ResolvedIPs, false))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("ip_deny", "Failed to re-resolve hostname after cache flush: " + Hostname + ".");
                    }

                    continue;
               }

               for (const auto &IP : ResolvedIPs)
               {
                    DeniedIPs.insert(IP);
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    std::string ResolvedStr;

                    for (size_t Index = 0; Index < ResolvedIPs.size(); ++Index)
                    {
                         if (Index > 0)
                         {
                              ResolvedStr += ", ";
                         }

                         ResolvedStr += ResolvedIPs[Index];
                    }

                    Instance->Logs->Debug("ip_deny", "Re-resolved hostname " + Hostname + " -> " + ResolvedStr + " (after cache flush).");
               }
          }
     }

     LastCacheFlush = Instance->Now();

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          bool ReResolved = (HasHostnames && !RegularHostnames.empty()) || (HasDenyHostnames && !DeniedRegularHostnames.empty());

          Instance->Logs->Debug("ip_allow", "DNS cache flushed: " + std::to_string(DNSCount) + " forward, " + std::to_string(ReverseCount) + " reverse entries cleared" + (ReResolved ? " (hostnames re-resolved)" : "") + ".");
     }
}

/* Configure maximum DNS cache entries. */

void IPFilter::SetDNSCacheMaxSize(size_t MaxSize)
{
     if (MaxSize == 0)
     {
          MaxSize = DNS_CACHE_MAX_SIZE;
     }

     std::lock_guard<std::mutex> CacheLock(CacheMutex);

     DNSCacheMaxSize = MaxSize;

     if (DNSCache.size() > DNSCacheMaxSize)
     {
          DNSCache.clear();

          DNSCacheOrder.clear();
     }

     if (ReverseDNSCache.size() > DNSCacheMaxSize)
     {
          ReverseDNSCache.clear();

          ReverseDNSCacheOrder.clear();
     }
}

/* Resolve hostname to IP addresses. */

bool IPFilter::ResolveHostname(const std::string &Hostname, std::vector<std::string> &ResolvedIPs, bool UseCache) const
{
     if (UseCache)
     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          auto It = DNSCache.find(Hostname);

          if (It != DNSCache.end())
          {
               ResolvedIPs = It->second;

               return !ResolvedIPs.empty();
          }
     }

     struct addrinfo Hints;

     struct addrinfo *Result = nullptr;

     std::memset(&Hints, 0, sizeof(Hints));

     Hints.ai_family = AF_INET;

     Hints.ai_socktype = SOCK_STREAM;

     int StatusVal = getaddrinfo(Hostname.c_str(), nullptr, &Hints, &Result);

     if (StatusVal != 0)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("ip_allow", "DNS resolution failed for " + Hostname + ": " + gai_strerror(StatusVal) + ".");
          }

          return false;
     }

     for (struct addrinfo *RP = Result; RP != nullptr; RP = RP->ai_next)
     {
          if (RP->ai_family == AF_INET)
          {
               struct sockaddr_in *IPv4 = reinterpret_cast<struct sockaddr_in *>(RP->ai_addr);

               char IPStr[INET_ADDRSTRLEN];

               if (inet_ntop(AF_INET, &(IPv4->sin_addr), IPStr, INET_ADDRSTRLEN) != nullptr)
               {
                    ResolvedIPs.push_back(std::string(IPStr));
               }
          }
     }

     freeaddrinfo(Result);

     if (UseCache && !ResolvedIPs.empty())
     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          auto It = DNSCache.find(Hostname);

          if (It != DNSCache.end())
          {
               ResolvedIPs = It->second;

               return !ResolvedIPs.empty();
          }

          if (DNSCache.size() >= DNSCacheMaxSize)
          {
               DNSCache.clear();

               DNSCacheOrder.clear();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("ip_allow", "DNS cache reached " + std::to_string(DNSCacheMaxSize) + " items, flushing everything.");
               }
          }

          DNSCache[Hostname] = ResolvedIPs;

          DNSCacheOrder.push_back(Hostname);
     }

     return !ResolvedIPs.empty();
}

/* Check if string is a hostname. */

bool IPFilter::IsHostname(const std::string &StrVal) const
{
     if (IsValidIP(StrVal))
     {
          return false;
     }

     if (IsCIDR(StrVal))
     {
          return false;
     }

     for (char CharValue : StrVal)
     {
          if ((CharValue >= 'a' && CharValue <= 'z') || (CharValue >= 'A' && CharValue <= 'Z'))
          {
               return true;
          }
     }

     return false;
}

/* Validate IP address format. */

bool IPFilter::IsValidIP(const std::string &IP) const
{
     struct sockaddr_in SA;

     return inet_pton(AF_INET, IP.c_str(), &(SA.sin_addr)) == 1;
}

/* Check if string is CIDR notation. */

bool IPFilter::IsCIDR(const std::string &CIDR) const
{
     size_t SlashPos = CIDR.find('/');

     if (SlashPos == std::string::npos || SlashPos == 0 || SlashPos == CIDR.length() - 1)
     {
          return false;
     }

     std::string IPPart = CIDR.substr(0, SlashPos);

     std::string MaskPart = CIDR.substr(SlashPos + 1);

     if (!IsValidIP(IPPart))
     {
          return false;
     }

     try
     {
          int MaskVal = std::stoi(MaskPart);

          return MaskVal >= 0 && MaskVal <= 32;
     }
     catch (...)
     {
          return false;
     }
}

/* Check if string is wildcard hostname. */

bool IPFilter::IsWildcardHostname(const std::string &Hostname) const
{
     return Hostname.find('*') != std::string::npos;
}

/* Check if IP is in CIDR range. */

bool IPFilter::IsIPInCIDR(const std::string &IP, const std::string &CIDR) const
{
     size_t SlashPos = CIDR.find('/');

     if (SlashPos == std::string::npos)
     {
          return false;
     }

     std::string NetworkStr = CIDR.substr(0, SlashPos);

     int MaskBits = std::stoi(CIDR.substr(SlashPos + 1));

     struct sockaddr_in NetworkAddr;

     struct sockaddr_in IPAddr;

     if (inet_pton(AF_INET, NetworkStr.c_str(), &(NetworkAddr.sin_addr)) != 1)
     {
          return false;
     }

     if (inet_pton(AF_INET, IP.c_str(), &(IPAddr.sin_addr)) != 1)
     {
          return false;
     }

     uint32_t Network = ntohl(NetworkAddr.sin_addr.s_addr);

     uint32_t Address = ntohl(IPAddr.sin_addr.s_addr);

     uint32_t Mask = (0xFFFFFFFF << (32 - MaskBits)) & 0xFFFFFFFF;

     return (Address & Mask) == (Network & Mask);
}

/* Match hostname against wildcard pattern. */

bool IPFilter::MatchWildcardHostname(const std::string &Hostname, const std::string &Pattern) const
{
     if (Pattern == "*")
     {
          return true;
     }

     if (Pattern.front() == '*')
     {
          std::string Suffix = Pattern.substr(1);

          if (!Suffix.empty() && Suffix.front() == '.')
          {
               Suffix = Suffix.substr(1);
          }

          if (!Suffix.empty() && Hostname.length() > Suffix.length())
          {
               std::string HostnameSuffix = Hostname.substr(Hostname.length() - Suffix.length());

               return HostnameSuffix == Suffix && Hostname[Hostname.length() - Suffix.length() - 1] == '.';
          }
     }

     return Hostname == Pattern;
}

/* Perform reverse DNS lookup. */

std::string IPFilter::ReverseDNS(const std::string &IP) const
{
     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          auto It = ReverseDNSCache.find(IP);

          if (It != ReverseDNSCache.end())
          {
               return It->second;
          }
     }

     struct sockaddr_in SA;

     if (inet_pton(AF_INET, IP.c_str(), &(SA.sin_addr)) != 1)
     {
          return "";
     }

     char Hostname[NI_MAXHOST];

     int ResultVal = getnameinfo(reinterpret_cast<struct sockaddr *>(&SA), sizeof(SA), Hostname, NI_MAXHOST, nullptr, 0, 0);

     if (ResultVal != 0)
     {
          return "";
     }

     std::string HostnameStr(Hostname);

     {
          std::lock_guard<std::mutex> CacheLock(CacheMutex);

          auto It = ReverseDNSCache.find(IP);

          if (It != ReverseDNSCache.end())
          {
               return It->second;
          }

          if (ReverseDNSCache.size() >= DNSCacheMaxSize)
          {
               ReverseDNSCache.clear();

               ReverseDNSCacheOrder.clear();

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("ip_allow", "Reverse DNS cache reached " + std::to_string(DNSCacheMaxSize) + " items, flushing everything.");
               }
          }

          ReverseDNSCache[IP] = HostnameStr;

          ReverseDNSCacheOrder.push_back(IP);
     }

     return HostnameStr;
}

/* Get hostname for an IP address. */

std::string IPFilter::GetHostnameForIP(const std::string &IP) const
{
     return ReverseDNS(IP);
}

/* Resolve hostname to a single IP address. */

bool IPFilter::ResolveHostnameToIP(const std::string &Hostname, std::string &ResolvedIP, bool UseCache) const
{
     std::vector<std::string> ResolvedIPs;

     if (!ResolveHostname(Hostname, ResolvedIPs, UseCache))
     {
          return false;
     }

     if (ResolvedIPs.empty())
     {
          return false;
     }

     ResolvedIP = ResolvedIPs[0];

     return true;
}

/* Parse comma-separated list of IPs/hostnames. */

std::vector<std::string> IPFilter::ParseIPList(const std::string &AllowedIPsConfig)
{
     std::vector<std::string> Result;
     std::stringstream SS(AllowedIPsConfig);
     std::string Item;

     while (std::getline(SS, Item, ','))
     {
          Item.erase(0, Item.find_first_not_of(" \t\n\r"));

          Item.erase(Item.find_last_not_of(" \t\n\r") + 1);

          if (!Item.empty())
          {
               Result.push_back(Item);
          }
     }

     return Result;
}
