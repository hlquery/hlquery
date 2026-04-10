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
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/actionlist.h"
#include "core/hlquery.h"
#include "core/socketengine.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#define HLQUERY_HAS_KQUEUE 1
#else
#define HLQUERY_HAS_KQUEUE 0
#endif

#if HLQUERY_HAS_KQUEUE

/* High-performance kqueue backend for BSD/macOS */

struct KqueueState
{
     bool read_enabled = false;
     bool write_enabled = false;
     bool base_write = false;
};

static std::vector<struct kevent> KqueueEvents;
static std::unordered_map<int, EventHandler *> FDToHandler;
static std::vector<EventHandler *> HandlerByFD;
static std::unordered_map<EventHandler *, int> HandlerToFD;
static std::unordered_map<int, KqueueState> FDStates;

/* Define static members declared in socketengine.h */

int SocketEngine::EpollFD = -1; /* Used as kqueue fd */

std::atomic<bool> SocketEngine::EpollFDValid{false};
std::vector<epoll_event> SocketEngine::Events; /* Unused, ABI compatibility */

std::vector<EventHandler *> SocketEngine::PendingWrites;
std::atomic<size_t> SocketEngine::PendingWritesCount{0};
std::atomic<int> SocketEngine::PendingMessageCount{0};

/* OPTIMIZATION: Fast duplicate checking for pending writes */

static std::unordered_set<EventHandler *> PendingWritesSet;

/* Mutex to protect PendingWrites and PendingWritesSet for thread safety */

static std::mutex PendingWritesMutex;

/* Adaptive timeout support */

static std::atomic<bool> AdaptiveTimeoutEnabled{true};
static std::atomic<int> CurrentTimeoutMS{-1};
static std::atomic<uint64_t> TotalBytesProcessed{0};
static std::atomic<uint64_t> ActiveConnections{0};
static std::atomic<bool> EngineInitialized{false};

static int GetTimedWorkWakeupMs()
{
     if (Instance && Instance->Timers)
     {
          const int timer_ms = Instance->Timers->GetTimeUntilNextMs();

          if (timer_ms == 0)
          {
               return 0;
          }

          if (timer_ms > 0)
          {
               return timer_ms;
          }
     }

     return 1000;
}

static bool ApplyKevent(int kq, int fd, int16_t filter, uint16_t flags)
{
     struct kevent change;
     EV_SET(&change, fd, filter, flags, 0, 0, nullptr);

     if (kevent(kq, &change, 1, nullptr, 0, nullptr) == -1)
     {
          return false;
     }

     return true;
}

/* Initializes the socket engine */

void SocketEngine::Init()
{
     if (EngineInitialized.load(std::memory_order_acquire))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "SocketEngine::Init() called multiple times! FDToHandler.size()=" +
                                             std::to_string(FDToHandler.size()) + " - this will clear registered FDs.");
          }
          return;
     }

     EpollFD = kqueue();
     if (EpollFD == -1)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "kqueue() failed: " + std::string(strerror(errno)) + ".");
          }
          return;
     }

     EpollFDValid.store(true, std::memory_order_relaxed);

     KqueueEvents.clear();
     KqueueEvents.resize(MAX_EVENTS);
     FDToHandler.clear();
     HandlerByFD.clear();
     HandlerToFD.clear();
     FDStates.clear();
     SocketEngine::PendingWrites.clear();

     InitializeAdvancedIO();
     InitializeAdaptiveTimeout();

     EngineInitialized.store(true, std::memory_order_release);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("socketengine", "High-performance kqueue engine initialized.");
     }
}

/* Deinitializes the socket engine */

void SocketEngine::Deinit()
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "Deinit: Clearing " + std::to_string(FDToHandler.size()) + " registered file descriptors.");
     }

     if (EpollFD != -1)
     {
          close(EpollFD);
          EpollFD = -1;
     }
     EpollFDValid.store(false, std::memory_order_relaxed);

     KqueueEvents.clear();
     FDToHandler.clear();
     HandlerByFD.clear();
     HandlerToFD.clear();
     FDStates.clear();

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          SocketEngine::PendingWrites.clear();
          PendingWritesSet.clear();
     }

     SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
     SocketEngine::PendingMessageCount.store(0, std::memory_order_relaxed);
     EngineInitialized.store(false, std::memory_order_release);
}

/* Resets the engine after a fork */

void SocketEngine::ResetAfterFork()
{
     Deinit();
     Init();
}

/* Adds a file descriptor to the engine */

bool SocketEngine::AddFD(EventHandler *EH, int EventsMask)
{
     if (!EH || !EH->HasFD())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "AddFD: Invalid handler or no FD.");
          }
          return false;
     }

     int fd = EH->GetFD();

     if (FDToHandler.find(fd) != FDToHandler.end())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "AddFD: FD " + std::to_string(fd) + " already registered.");
          }
          return false;
     }

     KqueueState state;
     state.read_enabled = (EventsMask & EPOLLIN) != 0;
     state.write_enabled = (EventsMask & EPOLLOUT) != 0;
     state.base_write = state.write_enabled;

     if (state.read_enabled)
     {
          if (!ApplyKevent(EpollFD, fd, EVFILT_READ, EV_ADD | EV_ENABLE))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "kevent(ADD, READ) failed: " + std::string(strerror(errno)) + ".");
               }
               return false;
          }
     }

     if (state.write_enabled)
     {
          if (!ApplyKevent(EpollFD, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE))
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "kevent(ADD, WRITE) failed: " + std::string(strerror(errno)) + ".");
               }
               if (state.read_enabled)
               {
                    ApplyKevent(EpollFD, fd, EVFILT_READ, EV_DELETE);
               }
               return false;
          }
     }

     FDToHandler[fd] = EH;
     if (fd >= static_cast<int>(HandlerByFD.size()))
     {
          HandlerByFD.resize(static_cast<size_t>(fd) + 1U, nullptr);
     }

     HandlerByFD[fd] = EH;
     HandlerToFD[EH] = fd;
     FDStates[fd] = state;

     ActiveConnections.fetch_add(1, std::memory_order_relaxed);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "AddFD: Registered fd=" + std::to_string(fd) + " with EventsMask=0x" + std::to_string(EventsMask) + ".");
     }

     return true;
}

/* Deletes a file descriptor from the engine */

void SocketEngine::DelFD(EventHandler *EH)
{
     if (!EH || !EH->HasFD())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: Invalid handler or no FD.");
          }
          return;
     }

     int fd = EH->GetFD();
     auto handler_it = FDToHandler.find(fd);

     if (handler_it == FDToHandler.end())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: FD " + std::to_string(fd) + " not registered.");
          }
          return;
     }

     auto state_it = FDStates.find(fd);
     if (state_it != FDStates.end())
     {
          if (state_it->second.read_enabled)
          {
               ApplyKevent(EpollFD, fd, EVFILT_READ, EV_DELETE);
          }
          if (state_it->second.write_enabled)
          {
               ApplyKevent(EpollFD, fd, EVFILT_WRITE, EV_DELETE);
          }
     }

     FDStates.erase(fd);
     HandlerToFD.erase(EH);
     FDToHandler.erase(handler_it);
     if (fd >= 0 && fd < static_cast<int>(HandlerByFD.size()))
     {
          HandlerByFD[fd] = nullptr;
     }
     UnregisterPendingWrite(EH);

     ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
}

/* Dispatches pending events */

int SocketEngine::DispatchEvents()
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: ENTRY (FDToHandler.size()=" + std::to_string(FDToHandler.size()) + ", ShuttingDown=" + std::to_string(ShuttingDown) + ", ForceExit=" + std::to_string(ForceExit) + ").");
     }

     if (FDToHandler.empty())
     {
          if (ShuttingDown || ForceExit)
          {
               return 0;
          }
          return 0;
     }

     if (ShuttingDown || ForceExit)
     {
          return 0;
     }

     int timeout_ms = -1;
     if (HasPendingWork())
     {
          timeout_ms = 0;
          CurrentTimeoutMS.store(0, std::memory_order_relaxed);
     }
     else
     {
          timeout_ms = GetTimedWorkWakeupMs();
          CurrentTimeoutMS.store(timeout_ms, std::memory_order_relaxed);
     }

     struct timespec ts;
     struct timespec *ts_ptr = nullptr;
     if (timeout_ms >= 0)
     {
          ts.tv_sec = timeout_ms / 1000;
          ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
          ts_ptr = &ts;
     }

     int nevents = kevent(EpollFD, nullptr, 0, KqueueEvents.data(), MAX_EVENTS, ts_ptr);

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "kevent() returned " + std::to_string(nevents) + " events (timeout=" + (timeout_ms == -1 ? std::string("infinite") : std::to_string(timeout_ms) + "ms") + (nevents < 0 ? ", errno=" + std::string(strerror(errno)) : "") + ").");
     }

     if (nevents <= 0)
     {
          if (nevents < 0 && errno == EINTR)
          {
               return 0;
          }
          return 0;
     }

     int events_processed = 0;

     for (int i = 0; i < nevents; ++i)
     {
          struct kevent &ev = KqueueEvents[i];
          int fd = static_cast<int>(ev.ident);

          EventHandler *EH = nullptr;

          if (fd >= 0 && fd < static_cast<int>(HandlerByFD.size()))
          {
               EH = HandlerByFD[fd];
          }

          if (!EH)
          {
               continue;
          }

          if (ev.flags & EV_ERROR)
          {
               EH->OnEventHandlerError(static_cast<int>(ev.data));
               continue;
          }

          if (ev.filter == EVFILT_READ)
          {
               EH->OnEventHandlerRead();
               events_processed++;
          }
          else if (ev.filter == EVFILT_WRITE)
          {
               EH->OnEventHandlerWrite();
               events_processed++;
          }
     }

     return events_processed;
}

/* Dispatches trial writes */

void SocketEngine::DispatchTrialWrites()
{
     if (SocketEngine::PendingWritesCount.load(std::memory_order_relaxed) == 0)
     {
          return;
     }

     std::vector<EventHandler *> WriteCandidates;
     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (SocketEngine::PendingWrites.empty())
          {
               SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
               return;
          }

          WriteCandidates.swap(SocketEngine::PendingWrites);
          PendingWritesSet.clear();
          SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
     }

     const size_t BatchSize = 256;

     for (size_t batch_start = 0; batch_start < WriteCandidates.size(); batch_start += BatchSize)
     {
          size_t batch_end = std::min(batch_start + BatchSize, WriteCandidates.size());
          for (size_t i = batch_start; i < batch_end; ++i)
          {
               EventHandler *EH = WriteCandidates[i];
               if (!EH || !EH->HasFD())
               {
                    continue;
               }
               EH->OnEventHandlerWrite();
          }
     }
}

/* Registers a pending write */

void SocketEngine::RegisterPendingWrite(EventHandler *EH)
{
     if (!EH || !EH->HasFD())
     {
          return;
     }

     std::lock_guard<std::mutex> lock(PendingWritesMutex);

     if (PendingWritesSet.find(EH) != PendingWritesSet.end())
     {
          return;
     }

     PendingWritesSet.insert(EH);
     SocketEngine::PendingWrites.push_back(EH);
     SocketEngine::PendingWritesCount.fetch_add(1, std::memory_order_relaxed);

     int fd = EH->GetFD();
     auto state_it = FDStates.find(fd);
     if (state_it != FDStates.end() && !state_it->second.write_enabled)
     {
          if (ApplyKevent(EpollFD, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE))
          {
               state_it->second.write_enabled = true;
          }
     }
}

/* Unregisters a pending write */

void SocketEngine::UnregisterPendingWrite(EventHandler *EH)
{
     if (!EH)
     {
          return;
     }

     std::lock_guard<std::mutex> lock(PendingWritesMutex);

     if (PendingWritesSet.erase(EH) == 0)
     {
          return;
     }

     auto it = std::find(SocketEngine::PendingWrites.begin(), SocketEngine::PendingWrites.end(), EH);
     if (it != SocketEngine::PendingWrites.end())
     {
          if (it != SocketEngine::PendingWrites.end() - 1)
          {
               std::iter_swap(it, SocketEngine::PendingWrites.end() - 1);
          }
          SocketEngine::PendingWrites.pop_back();
          SocketEngine::PendingWritesCount.fetch_sub(1, std::memory_order_relaxed);
     }

     if (EH->HasFD())
     {
          int fd = EH->GetFD();
          auto state_it = FDStates.find(fd);
          if (state_it != FDStates.end() && state_it->second.write_enabled && !state_it->second.base_write)
          {
               if (ApplyKevent(EpollFD, fd, EVFILT_WRITE, EV_DELETE))
               {
                    state_it->second.write_enabled = false;
               }
          }
     }
}

/* Initializes adaptive timeout */

void SocketEngine::InitializeAdaptiveTimeout()
{
     CurrentTimeoutMS.store(-1);
}

/* Adapts the engine timeout based on load */

void SocketEngine::AdaptTimeout()
{
     if (!AdaptiveTimeoutEnabled.load())
     {
          return;
     }

     uint64_t CurrentConnectionsValue = ActiveConnections.load();
     int NewTimeoutValue = -1;

     if (CurrentConnectionsValue > 10000)
     {
          NewTimeoutValue = 0;
     }
     else if (CurrentConnectionsValue > 5000)
     {
          NewTimeoutValue = 1;
     }
     else if (CurrentConnectionsValue > 1000)
     {
          NewTimeoutValue = 5;
     }
     else if (CurrentConnectionsValue > 100)
     {
          NewTimeoutValue = 10;
     }

     CurrentTimeoutMS.store(NewTimeoutValue);
}

/* Enables or disables adaptive timeout */

void SocketEngine::EnableAdaptiveTimeout(bool Enable)
{
     AdaptiveTimeoutEnabled.store(Enable);
}

/* Initializes advanced I/O optimizations */

void SocketEngine::InitializeAdvancedIO()
{
     TotalBytesProcessed.store(0);
     ActiveConnections.store(0);
}

/* Initializes zero-copy buffers */

void SocketEngine::InitializeZeroCopyBuffers()
{
}

/* Cleans up zero-copy buffers */

void SocketEngine::CleanupZeroCopyBuffers()
{
}

/* Starts I/O worker threads */

void SocketEngine::StartIOWorkerThreads()
{
}

/* I/O worker thread loop */

void SocketEngine::IOWorkerThread(unsigned int WorkerID)
{
     (void)WorkerID;
}

/* Sets optimal socket options */

void SocketEngine::SetOptimalSocketOptions()
{
}

/* Returns a zero-copy buffer */

void *SocketEngine::GetZeroCopyBuffer()
{
     return nullptr;
}

/* Returns a zero-copy buffer to the pool */

void SocketEngine::ReturnZeroCopyBuffer(void *buffer)
{
     (void)buffer;
}

/* Resets I/O statistics */

void SocketEngine::ResetIOStats()
{
     TotalBytesProcessed.store(0);
     ActiveConnections.store(0);
}

/* Returns I/O statistics */

SocketEngine::IOStats SocketEngine::GetIOStats()
{
     IOStats StatsVal;
     StatsVal.TotalBytesProcessed = TotalBytesProcessed.load();
     StatsVal.ActiveConnections = ActiveConnections.load();
     return StatsVal;
}

/* Increments active connection count */

void SocketEngine::IncrementConnectionCount()
{
     ActiveConnections.fetch_add(1, std::memory_order_relaxed);
}

/* Decrements active connection count */

void SocketEngine::DecrementConnectionCount()
{
     ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
}

/* Increments total bytes processed */

void SocketEngine::IncrementBytesProcessed(uint64_t Bytes)
{
     TotalBytesProcessed.fetch_add(Bytes, std::memory_order_relaxed);
}

/* Returns the number of events processed */

int SocketEngine::GetEventCount()
{
     return 0;
}

/* Increments pending message count */

void SocketEngine::IncrementPendingMessages()
{
     SocketEngine::PendingMessageCount.fetch_add(1, std::memory_order_relaxed);
}

/* Decrements pending message count */

void SocketEngine::DecrementPendingMessages()
{
     SocketEngine::PendingMessageCount.fetch_sub(1, std::memory_order_relaxed);
}

/* Returns the number of pending messages */

int SocketEngine::GetPendingMessageCount()
{
     return SocketEngine::PendingMessageCount.load(std::memory_order_relaxed);
}

/* Returns true if there is pending work */

bool SocketEngine::HasPendingWork()
{
     if (ActionList::GetActionCount() > 0)
     {
          return true;
     }

     if (SocketEngine::PendingWritesCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     if (SocketEngine::PendingMessageCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     return false;
}

#else

/* Stub kqueue backend for platforms without sys/event.h */

void SocketEngine::Init()
{
     if (Instance && Instance->Logs)
     {
          Instance->Logs->Critical("socketengine", "kqueue backend is not supported on this platform.");
     }
}

void SocketEngine::Deinit()
{
}

void SocketEngine::ResetAfterFork()
{
}

bool SocketEngine::AddFD(EventHandler *, int)
{
     return false;
}

void SocketEngine::DelFD(EventHandler *)
{
}

int SocketEngine::DispatchEvents()
{
     return 0;
}

void SocketEngine::DispatchTrialWrites()
{
}

void SocketEngine::RegisterPendingWrite(EventHandler *)
{
}

void SocketEngine::UnregisterPendingWrite(EventHandler *)
{
}

void SocketEngine::InitializeAdaptiveTimeout()
{
}

void SocketEngine::AdaptTimeout()
{
}

void SocketEngine::EnableAdaptiveTimeout(bool)
{
}

void SocketEngine::InitializeAdvancedIO()
{
}

void SocketEngine::InitializeZeroCopyBuffers()
{
}

void SocketEngine::CleanupZeroCopyBuffers()
{
}

void SocketEngine::StartIOWorkerThreads()
{
}

void SocketEngine::IOWorkerThread(unsigned int)
{
}

void SocketEngine::SetOptimalSocketOptions()
{
}

void *SocketEngine::GetZeroCopyBuffer()
{
     return nullptr;
}

void SocketEngine::ReturnZeroCopyBuffer(void *)
{
}

void SocketEngine::ResetIOStats()
{
}

SocketEngine::IOStats SocketEngine::GetIOStats()
{
     IOStats StatsVal{};
     return StatsVal;
}

void SocketEngine::IncrementConnectionCount()
{
}

void SocketEngine::DecrementConnectionCount()
{
}

void SocketEngine::IncrementBytesProcessed(uint64_t)
{
}

int SocketEngine::GetEventCount()
{
     return 0;
}

void SocketEngine::IncrementPendingMessages()
{
}

void SocketEngine::DecrementPendingMessages()
{
}

int SocketEngine::GetPendingMessageCount()
{
     return 0;
}

bool SocketEngine::HasPendingWork()
{
     return ActionList::GetActionCount() > 0;
}

#endif
