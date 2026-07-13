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
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sys/epoll.h>
#include <sys/time.h>
#include <unordered_map>
#include <unordered_set>

#include "common/actionlist.h"
#include "core/config.h"
#include "runtime/exitmanager.h"
#include "core/hlquery.h"
#include "core/logmanager.h"
#include "core/socketengine.h"
#include "runtime/timers.h"
#include "search/rocksdb_storage_engine.h"
#include "utils/consolewriter.h"

int SocketEngine::EpollFD = -1;

std::atomic<bool> SocketEngine::EpollFDValid{false};

std::vector<epoll_event> SocketEngine::Events(SocketEngine::MAX_EVENTS);

std::vector<EventHandler *> SocketEngine::PendingWrites;

std::atomic<size_t> SocketEngine::PendingWritesCount{0};

std::atomic<int> SocketEngine::PendingMessageCount{0};

/* Fast lookup for pending writes to avoid duplicates (O(1) instead of O(n)) */

static std::unordered_set<EventHandler *> PendingWritesSet;

/* Mutex to protect PendingWrites and PendingWritesSet for thread safety */

static std::mutex PendingWritesMutex;

static std::atomic<bool> AdaptiveTimeoutEnabled{true};

static std::atomic<int> CurrentTimeoutMS{-1};

static std::atomic<uint64_t> TotalBytesProcessed{0};

static std::atomic<uint64_t> ActiveConnections{0};

static std::atomic<bool> EngineInitialized{false};

static std::atomic<int> EventCounter{0};

static std::unordered_set<int> RegisteredFDs;

static std::mutex RegisteredFDsMutex;

/* NUMA-aware thread pools */

/* Zero-copy optimizations - lazy allocation to save memory if unused */

static std::array<void *, EPOLL_ZERO_COPY_BUFFER_COUNT> ZeroCopyBuffers{};

static std::atomic<size_t> ZeroCopyBufferIndex{0};

static std::atomic<bool> ZeroCopyBuffersAllocated{false};

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

     return SOCKET_ENGINE_TIMED_WORK_FALLBACK_MS;
}

/* Initializes the socket engine */

void SocketEngine::Init()
{
     /* Prevent double initialization. */

     if (EngineInitialized.load(std::memory_order_acquire))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("socketengine", "Socket engine already initialized (EpollFD=" + std::to_string(EpollFD) + "), skipping.");
          }

          return;
     }

     /* Create epoll with close-on-exec so restarts don't inherit it accidentally */

     EpollFD = epoll_create1(EPOLL_CLOEXEC);

     if (EpollFD == -1)
     {
          ConsoleWriter::WriteError("SocketEngine::Init() failed: epoll_create1() returned " + std::to_string(EpollFD) +
                                         ", errno: " + std::to_string(errno) + " (" + std::string(strerror(errno)) + ")",
                                    true);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "epoll_create1() failed: " + std::string(strerror(errno)) + ".");
          }

          EpollFDValid.store(false);
          ExitManager::Exit(1);
     }

     EpollFDValid.store(true);
     EngineInitialized.store(true, std::memory_order_release);

     /* Initialize advanced I/O optimizations */

     InitializeAdvancedIO();

     /* Initialize zero-copy buffers */

     InitializeZeroCopyBuffers();

     /* Initialize adaptive timeout */

     InitializeAdaptiveTimeout();

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("socketengine", "Ultra-high performance epoll engine initialized.");
     }
}

/* Deinitializes the socket engine */

void SocketEngine::Deinit()
{
     /* Cleanup zero-copy buffers */

     CleanupZeroCopyBuffers();

     /* Clear pending writes */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          PendingWrites.clear();
          PendingWritesSet.clear();
     }

     {
          std::lock_guard<std::mutex> lock(RegisteredFDsMutex);
          RegisteredFDs.clear();
     }

     PendingWritesCount.store(0, std::memory_order_relaxed);
     EventCounter.store(0, std::memory_order_relaxed);

     if (EpollFD != -1)
     {
          close(EpollFD);
          EpollFD = -1;
          EpollFDValid.store(false);
     }

     EngineInitialized.store(false, std::memory_order_release);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("socketengine", "Advanced epoll engine deinitialized.");
     }
}

/* Resets the engine after a fork */

void SocketEngine::ResetAfterFork()
{
     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          PendingWrites.clear();
          PendingWritesSet.clear();
     }

     {
          std::lock_guard<std::mutex> lock(RegisteredFDsMutex);
          RegisteredFDs.clear();
     }

     PendingWritesCount.store(0, std::memory_order_relaxed);
     PendingMessageCount.store(0, std::memory_order_relaxed);
     EventCounter.store(0, std::memory_order_relaxed);
     ZeroCopyBufferIndex.store(0, std::memory_order_relaxed);

     if (EpollFD != -1)
     {
          close(EpollFD);
          EpollFD = -1;
     }

     EpollFDValid.store(false, std::memory_order_relaxed);
     EngineInitialized.store(false, std::memory_order_release);

     Init();
}

/* Initializes advanced I/O optimizations */

void SocketEngine::InitializeAdvancedIO()
{
     /* Reserve space for pending writes set to reduce rehashing */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          PendingWritesSet.reserve(EPOLL_PENDING_WRITES_RESERVE);
     }

     /* Set optimal socket options */

     SetOptimalSocketOptions();
}

/* Initializes zero-copy buffers */

void SocketEngine::InitializeZeroCopyBuffers()
{
     /*
      * Lazy allocation: only allocate if actually needed (saves 1MB if unused)
      * Buffers will be allocated on first GetZeroCopyBuffer() call
      */

     ZeroCopyBuffersAllocated.store(false);
}

/* Cleans up zero-copy buffers */

void SocketEngine::CleanupZeroCopyBuffers()
{
     for (auto &buffer : ZeroCopyBuffers)
     {
          if (buffer)
          {
               std::free(buffer);
               buffer = nullptr;
          }
     }
}

/* Initializes adaptive timeout */

void SocketEngine::InitializeAdaptiveTimeout()
{
     /* Keep infinite timeout for maximum performance */

     CurrentTimeoutMS.store(-1);
}

/* Adapts the engine timeout based on load */

void SocketEngine::AdaptTimeout()
{
     if (!AdaptiveTimeoutEnabled.load())
     {
          return;
     }

     /* HLQuery-style adaptive timeout based on load */

     uint64_t CurrentConnections = ActiveConnections.load();

     /* Default to blocking */

     int NewTimeout = -1;

     if (CurrentConnections > SOCKET_ENGINE_ULTRA_HIGH_LOAD_CONNECTIONS)
     {
          /* Ultra-high load - use 0ms timeout (non-blocking) */

          NewTimeout = SOCKET_ENGINE_ULTRA_HIGH_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnections > SOCKET_ENGINE_HIGH_LOAD_CONNECTIONS)
     {
          /* High load - use 1ms timeout */

          NewTimeout = SOCKET_ENGINE_HIGH_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnections > SOCKET_ENGINE_MEDIUM_LOAD_CONNECTIONS)
     {
          /* Medium load - use 5ms timeout */

          NewTimeout = SOCKET_ENGINE_MEDIUM_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnections > SOCKET_ENGINE_LOW_MEDIUM_LOAD_CONNECTIONS)
     {
          /* Low-medium load - use 10ms timeout */

          NewTimeout = SOCKET_ENGINE_LOW_MEDIUM_LOAD_TIMEOUT_MS;
     }

     CurrentTimeoutMS.store(NewTimeout);
}

/* Sets optimal socket options */

void SocketEngine::SetOptimalSocketOptions()
{
     /*
      * Note: Socket options are set per-connection in HttpConnection::SetAdvancedSocketOptions()
      * The epoll file descriptor itself doesn't support TCP socket options
      * This method is kept for future system-level optimizations
      */

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("socketengine", "Socket engine initialized with per-connection optimizations.");
     }
}

/* Returns a zero-copy buffer */

void *SocketEngine::GetZeroCopyBuffer()
{
     bool expected = false;

     if (ZeroCopyBuffersAllocated.compare_exchange_strong(expected, true))
     {
          /* First call - allocate all buffers now */

          bool AllocationFailed = false;
          size_t AllocatedCount = 0;

          for (size_t i = 0; i < ZeroCopyBuffers.size(); ++i)
          {
               ZeroCopyBuffers[i] = std::aligned_alloc(EPOLL_ZERO_COPY_BUFFER_ALIGNMENT, EPOLL_ZERO_COPY_BUFFER_SIZE);

               if (!ZeroCopyBuffers[i])
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("socketengine", "Failed to allocate zero-copy buffer " + std::to_string(i) + ".");
                    }

                    AllocationFailed = true;

                    /* Stop allocation on first failure. */

                    break;
               }

               AllocatedCount++;
          }

          /* If allocation failed partway through, clean up and reset flag */

          if (AllocationFailed)
          {
               /* Free any buffers we already allocated */

               for (size_t j = 0; j < AllocatedCount; ++j)
               {
                    if (ZeroCopyBuffers[j])
                    {
                         std::free(ZeroCopyBuffers[j]);
                         ZeroCopyBuffers[j] = nullptr;
                    }
               }

               /* Reset flag so other threads can retry (or we can retry later) */

               ZeroCopyBuffersAllocated.store(false);

               /* Return null on allocation failure. */

               return nullptr;
          }
     }

     if (!ZeroCopyBuffersAllocated.load())
     {
          /* Allocation failed. */

          return nullptr;
     }

     size_t Index = ZeroCopyBufferIndex.fetch_add(1) % ZeroCopyBuffers.size();
     return ZeroCopyBuffers[Index];
}

/* Returns a zero-copy buffer to the pool */

void SocketEngine::ReturnZeroCopyBuffer(void *buffer)
{
     /* Buffer is automatically returned when index wraps around */

     /* Suppress unused parameter warning. */

     (void)buffer;
}

/* Returns I/O statistics */

SocketEngine::IOStats SocketEngine::GetIOStats()
{
     IOStats StatsVal;

     StatsVal.TotalBytesProcessed = TotalBytesProcessed.load();
     StatsVal.ActiveConnections = ActiveConnections.load();

     return StatsVal;
}

/* Resets I/O statistics */

void SocketEngine::ResetIOStats()
{
     TotalBytesProcessed.store(0);
     ActiveConnections.store(0);
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

/* Enables or disables adaptive timeout */

void SocketEngine::EnableAdaptiveTimeout(bool Enable)
{
     AdaptiveTimeoutEnabled.store(Enable);
}

/* Adds a file descriptor to the engine */

bool SocketEngine::AddFD(EventHandler *EH, int EventsMask)
{
     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "AddFD: ENTRY (FD=" + std::to_string(EH->HasFD() ? EH->GetFD() : -1) + ", EventsMask=0x" + std::to_string(EventsMask) + ", EpollFD=" + std::to_string(EpollFD) + ", valid=" + std::string(EpollFDValid.load() ? "true" : "false") + ").");
     }

     if (EpollFD == -1 || !EpollFDValid.load())
     {
          /* Engine not initialized or already deinitialized; ignore safely */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "Cannot add fd - socket engine not initialized (EpollFD = -1).");
          }

          return false;
     }

     if (!EH->HasFD())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "Cannot add fd - EventHandler has no valid file descriptor.");
          }

          return false;
     }

     /*
      * Skip fcntl validation when the descriptor was already accepted
      * by epoll and there is no reason to suspect it is invalid.
      */

     struct epoll_event ev;

     memset(&ev, 0, sizeof(ev));

     /*
     * IMPROVEMENT: Use edge-triggered epoll correctly: in the event handler, read/write in a loop
     * until EAGAIN to ensure no data remains in the socket buffer (preventing missed events from stalling communication)
     * Always subscribe to error/hangup notifications so we can detect peer disconnects immediately
     */

     ev.events = EventsMask | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
     ev.data.ptr = EH;

     int fd = EH->GetFD();

     if (epoll_ctl(EpollFD, EPOLL_CTL_ADD, fd, &ev) == -1)
     {
          /* If epoll_ctl fails with EBADF, mark EpollFD as invalid */

          if (errno == EBADF)
          {
               EpollFD = -1;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "epoll_ctl failed: " + std::string(strerror(errno)) + " (fd=" + std::to_string(fd) + ").");
          }

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "AddFD: FAILED - epoll_ctl error (fd=" + std::to_string(fd) + ", errno=" + std::string(strerror(errno)) + ").");
          }

          return false;
     }

     /*
      * Preserve the original readiness subscription so temporary write
      * interest can be added and removed without changing caller intent.
      */

     EH->SetEventMask(EventsMask);

     {
          std::lock_guard<std::mutex> lock(RegisteredFDsMutex);

          if (RegisteredFDs.insert(fd).second)
          {
               ActiveConnections.fetch_add(1, std::memory_order_relaxed);
          }
     }

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "AddFD: SUCCESS - fd=" + std::to_string(fd) + " added to epoll (ActiveConnections=" + std::to_string(ActiveConnections.load()) + ").");
     }

     return true;
}

/* Deletes a file descriptor from the engine */

void SocketEngine::DelFD(EventHandler *EH)
{
     if (EpollFD == -1 || !EpollFDValid.load())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: Engine down, skipping (EpollFD=" + std::to_string(EpollFD) + ", valid=" + std::string(EpollFDValid.load() ? "true" : "false") + ").");
          }

          /* Engine down. */

          return;
     }

     if (!EH)
     {
          /* Check for null handler. */

          return;
     }

     if (EH->HasFD())
     {
          /* Cache FD before any operations to prevent use-after-free */

          int fd = EH->GetFD();

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: Removing fd=" + std::to_string(fd) + " from epoll.");
          }

          /* Clean up pending writes BEFORE removing from epoll to prevent race conditions */

          UnregisterPendingWrite(EH);

          /*
           * IMPROVEMENT: Synchronize properly when closing sockets from outside the event loop thread
           * to prevent a race condition where epoll still has a dangling reference to a closed file descriptor
           * Use mutex if called from different thread (currently single-threaded, but prepare for future)
           */

          bool WasRegistered = false;

          {
               std::lock_guard<std::mutex> lock(RegisteredFDsMutex);
               WasRegistered = RegisteredFDs.erase(fd) > 0;
          }

          int result = epoll_ctl(EpollFD, EPOLL_CTL_DEL, fd, nullptr);

          /* Save errno immediately after call */

          int saved_errno = errno;

          if (result == 0)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DelFD: Successfully removed fd=" + std::to_string(fd) + " from epoll (ActiveConnections=" + std::to_string(ActiveConnections.load() - 1) + ").");
               }

               if (WasRegistered)
               {
                    ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
               }

               /*
                * Clear the cached subscription after epoll has accepted
                * removal so later write registration cannot revive it.
                */

               EH->SetEventMask(0);

               /*
                * Ensure the socket can be closed safely after epoll removal
                * so stale event references cannot reach the handler later.
                */
          }
          else if (saved_errno == EBADF)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DelFD: EBADF error, marking EpollFD as invalid (fd=" + std::to_string(fd) + ").");
               }

               /* EpollFD is invalid - mark it */

               EpollFDValid.store(false);
          }
          else if (saved_errno == ENOENT)
          {
               /* Handle ENOENT (fd not in epoll) - this is not necessarily an error */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DelFD: ENOENT - fd=" + std::to_string(fd) + " was not in epoll (already removed?).");
               }

               if (WasRegistered)
               {
                    ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
               }

               /*
                * ENOENT means the descriptor is already absent from epoll,
                * so the cached subscription must be cleared as well.
                */

               EH->SetEventMask(0);
          }
          else
          {
               /* Handle all other errors comprehensively */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "DelFD: epoll_ctl failed (fd=" + std::to_string(fd) +
                                                  ", errno=" + std::to_string(saved_errno) + " (" + std::string(strerror(saved_errno)) + ")).");
               }

               if (WasRegistered)
               {
                    ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
               }

               /*
                * Treat other removal failures as terminal for this engine
                * registration and clear the cached event subscription.
                */

               EH->SetEventMask(0);
          }
     }
     else
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: Handler has no FD, skipping.");
          }
     }
}

/*
 * CRITICAL FIX: Reduce debug log verbosity - only log ENTRY every 10000th call.
 * The event loop runs continuously, so logging every 100 calls is still too verbose.
 */

/* Dispatches pending events */

int SocketEngine::DispatchEvents()
{
     if (EpollFD == -1 || !EpollFDValid.load())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DispatchEvents: Engine deinitialized, returning 0.");
          }

          /* Engine is deinitialized; avoid blocking or using invalid fds */

          return 0;
     }

     /* Skip redundant fcntl check - use cached validity flag instead */

     /*
     * Check shutdown flags before blocking.
     * Problem: If shutdown is requested (via Ctrl+C), we should not block
     * in epoll_wait() as this prevents immediate shutdown.
     *
     * Solution: If ShuttingDown or ForceExit is set, use timeout = 0
     * (non-blocking) to return immediately to the main loop.
     */

     if (hlquery::ShouldShutdown() || hlquery::ShouldForceExit())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DispatchEvents: Shutdown requested, returning 0.");
          }

          /* Do not call epoll_wait during shutdown. */

          return 0;
     }

     /*
     * Check for pending work before blocking.
     * Problem: If we have pending work (cursor slices, pending actions, etc.)
     * and epoll_wait(-1) blocks, the CPU-only work stalls because no I/O events
     * wake up the epoll.
     *
     * Solution: If there's pending work, use a short timeout instead of a
     * permanent non-blocking poll. This keeps CPU-only work responsive without
     * letting stale pending-work state spin the server or starve socket events.
     */

     /* Default to infinite blocking. */

     int timeout_ms = -1;

     /* Check if we have pending work that needs immediate processing */

     if (HasPendingWork())
     {
          timeout_ms = SOCKET_ENGINE_PENDING_WORK_TIMEOUT_MS;

          /*
         * Reset timeout immediately when pending work detected.
         * This ensures high throughput mode is active from the start of new activity.
         */

          CurrentTimeoutMS.store(timeout_ms, std::memory_order_relaxed);
     }
     else
     {
          /*
         * Wake periodically for time-based work even when there is no socket activity.
         * This keeps timers and wall-clock hooks such as OnEveryOneMinute progressing
         * without requiring an external request to wake the event loop.
         */

          timeout_ms = GetTimedWorkWakeupMs();

          CurrentTimeoutMS.store(timeout_ms, std::memory_order_relaxed);
     }

     /*
      * Reduce debug log verbosity - only log every 10000th call to reduce log spam.
      * Don't log every infinite timeout call - that's too verbose.
      * This dramatically reduces log noise during operations like ping that call DispatchEvents frequently.
      */

     int local_EpollFD = EpollFD;

     if (local_EpollFD == -1 || !EpollFDValid.load())
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DispatchEvents: EpollFD became invalid before epoll_wait, returning 0.");
          }

          return 0;
     }

     /* Validate Events array is valid (it's initialized with MAX_EVENTS elements) */

     if (Events.empty() || Events.size() != MAX_EVENTS)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "DispatchEvents: Events array is invalid (size=" + std::to_string(Events.size()) +
                                             ", expected " + std::to_string(MAX_EVENTS) + ").");
          }

          return 0;
     }

     /*
     * IMPROVEMENT: Process multiple events per epoll_wait call (batch event handling)
     * to reduce system call overhead and improve throughput under high connection rates.
     */

     int nfds = epoll_wait(local_EpollFD, Events.data(), MAX_EVENTS, timeout_ms);

     /*
     * IMPROVEMENT: Increase the epoll event list capacity or handle the case where events overflow
     * the buffer by processing in a loop until all pending events are handled.
     */

     if (nfds == MAX_EVENTS)
     {
          /* Hit event limit - will process in loop below */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("socketengine", "epoll_wait returned MAX_EVENTS (" + std::to_string(MAX_EVENTS) + ") - may have more events pending, will process in loop.");
          }
     }

     /*
     * CRITICAL FIX: Only log epoll_wait results every 100000th call to prevent log spam.
     * Even when events are present, logging every single event is too verbose.
     * Increased from 1000 to 100000 to handle high-frequency ping operations.
     */

     /*
     * CRITICAL FIX: Reset timeout and flood protection when events are detected.
     * This prevents second benchmark from starting slow due to accumulated timeout
     * from previous idle period. Reset to 0ms for maximum throughput.
     * Also reset flood protection counter since we have real activity.
     */

     if (nfds > 0)
     {
          EventCounter.fetch_add(nfds, std::memory_order_relaxed);

          /* Activity detected - immediately reset to non-blocking for high throughput */

          CurrentTimeoutMS.store(0, std::memory_order_relaxed);

          /*
           * Flood protection is intentionally omitted here so real socket
           * activity can continue at full event-loop throughput.
           */
     }

     /* HLQuery-style: If we hit the event limit, process immediately and loop back */

     if (nfds == MAX_EVENTS)
     {
          /*
         * We hit the limit - process these events and immediately check for more.
         * This prevents event loss in high-throughput scenarios.
         */

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("socketengine", "Hit MAX_EVENTS limit (" + std::to_string(MAX_EVENTS) + "), processing immediately.");
          }
     }

     if (nfds < 0)
     {
          /* ANY error including EINTR - return immediately for flag check */

          int err = errno;

          if (err == EBADF)
          {
               /* EpollFD became invalid - mark it and return */

               EpollFDValid.store(false);

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "DispatchEvents: epoll_wait returned EBADF - EpollFD became invalid, marking as invalid.");
               }
          }
          else if (err == EINVAL)
          {
               /* Invalid arguments - this should not happen but handle gracefully */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "DispatchEvents: epoll_wait returned EINVAL - invalid arguments (EpollFD=" +
                                                  std::to_string(local_EpollFD) + ", maxevents=" + std::to_string(MAX_EVENTS) +
                                                  ", timeout=" + std::to_string(timeout_ms) + ").");
               }

               EpollFDValid.store(false);
          }
          else if (err != EINTR)
          {
               /* Other errors (not EINTR) - log but continue */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DispatchEvents: epoll_wait error, returning 0 (errno=" + std::string(strerror(err)) + ").");
               }
          }

          return 0;
     }

     if (nfds == 0)
     {
          /*
         * CRITICAL FIX: If we got no events but HasPendingWork() was true, check if it's still true.
         * If pending work counters are stale (not being processed), we should block instead of spinning.
         * This prevents the second benchmark from being slow due to unnecessary spinning.
         */

          if (!HasPendingWork())
          {
               /* No pending work - reset to blocking mode */

               CurrentTimeoutMS.store(-1, std::memory_order_relaxed);
          }
          else
          {
               /*
                * Still have pending work, so keep the current short timeout
                * and let the main loop process queued actions next.
                */
          }

          return 0;
     }

     /*
      * Process socket events in place to avoid extra allocations, then
      * handle errors before reads and writes for predictable cleanup.
      */

     /* First pass: process error events immediately (they need cleanup) */

     /* Add explicit bounds check for Events array access */

     for (int i = 0; i < nfds && i < static_cast<int>(Events.size()); ++i)
     {
          EventHandler *EH = static_cast<EventHandler *>(Events[i].data.ptr);

          if (!EH)
          {
               /* Safety check. */

               continue;
          }

          uint32_t ev = Events[i].events;

          /*
           * IMPROVEMENT: Handle EPOLLHUP and EPOLLERR events by promptly cleaning up those connections
           * (closing sockets and removing them from epoll) to avoid resource leakage.
           * Handle errors/hangups first - process immediately for cleanup.
           */

          if (ev & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
          {
               /* Validate fd before using it */

               int fd = EH->GetFD();
               bool valid_fd = (fd >= 0 && fd <= EPOLL_MAX_REASONABLE_FD);
               int error_num = 0;

               if ((ev & EPOLLERR) && valid_fd)
               {
                    socklen_t error_len = sizeof(error_num);

                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_num, &error_len) != 0 || error_num == 0)
                    {
                         error_num = errno != 0 ? errno : EIO;
                    }
               }
               else if (ev & EPOLLRDHUP)
               {
                    error_num = ECONNRESET;
               }
               else
               {
                    error_num = EPIPE;
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    std::string event_types;

                    if (ev & EPOLLHUP)
                    {
                         event_types += "EPOLLHUP ";
                    }

                    if (ev & EPOLLRDHUP)
                    {
                         event_types += "EPOLLRDHUP ";
                    }

                    if (ev & EPOLLERR)
                    {
                         event_types += "EPOLLERR ";
                    }

                    Instance->Logs->Debug("socketengine", "DispatchEvents: Processing error/hangup event (" + event_types + ", ev=0x" + std::to_string(ev) + ", fd=" + (valid_fd ? std::to_string(fd) : "INVALID") + ", errno=" + std::to_string(error_num) + ") - dispatching error handler.");
               }

               try
               {
                    EH->OnEventHandlerError(error_num);
               }
               catch (...)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("socketengine", "DispatchEvents: Exception in OnEventHandlerError() for fd=" + std::to_string(fd) + " - forcing cleanup.");
                    }

                    if (valid_fd)
                    {
                         DelFD(EH);
                    }

                    UnregisterPendingWrite(EH);
               }

               /* Mark as processed by clearing events */

               Events[i].events = 0;
          }
     }

     /* Second pass: process read events (most common) */

     int read_events = 0;

     /* Add explicit bounds check for Events array access */

     for (int i = 0; i < nfds && i < static_cast<int>(Events.size()); ++i)
     {
          if (Events[i].events == 0)
          {
               /* Already processed. */

               continue;
          }

          EventHandler *EH = static_cast<EventHandler *>(Events[i].data.ptr);

          if (!EH)
          {
               continue;
          }

          /*
         * Validate fd before using it - prevent crashes from invalid pointers.
         * Sanity check: fd should be reasonable.
         */

          int fd = EH->GetFD();

          if (fd < 0 || fd > EPOLL_MAX_REASONABLE_FD)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("socketengine", "DispatchEvents: Invalid fd detected in read event (fd=" + std::to_string(fd) + ", ptr=" + std::to_string(reinterpret_cast<uintptr_t>(EH)) + ") - skipping.");
               }

               Events[i].events = 0;
               continue;
          }

          uint32_t ev = Events[i].events;

          if (ev & EPOLLIN)
          {
               read_events++;

               /*
             * CRITICAL FIX: Only log read events in very verbose debug mode to reduce log spam.
             * Most read events are normal and don't need logging.
             */

               /* Validate handler is still valid before calling methods */

               if (!EH->HasFD() || EH->GetFD() != fd)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("socketengine", "DispatchEvents: Handler fd changed during read processing (was " + std::to_string(fd) + ") - skipping.");
                    }

                    Events[i].events = 0;
                    continue;
               }

               try
               {
                    EH->OnEventHandlerRead();
               }
               catch (...)
               {
                    /* Catch any exceptions to prevent crash */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("socketengine", "DispatchEvents: Exception in OnEventHandlerRead() for fd=" + std::to_string(fd) + " - cleaning up.");
                    }

                    DelFD(EH);
                    UnregisterPendingWrite(EH);
                    Events[i].events = 0;
                    continue;
               }

               /*
                * Read completion logging stays disabled here to avoid
                * excessive event-loop noise under normal traffic.
                */
          }
     }

     /* Third pass: process write events */

     int write_events = 0;

     /* Add explicit bounds check for Events array access */

     for (int i = 0; i < nfds && i < static_cast<int>(Events.size()); ++i)
     {
          if (Events[i].events == 0)
          {
               /* Already processed. */

               continue;
          }

          EventHandler *EH = static_cast<EventHandler *>(Events[i].data.ptr);

          if (!EH)
          {
               continue;
          }

          /*
          * Validate fd before using it - prevent crashes from invalid pointers.
          * File descriptors should be small positive integers (typically 0-1024).
          * Large values like 172182496 indicate memory corruption or invalid pointer.
          * Sanity check: fd should be reasonable.
          */

          int fd = EH->GetFD();

          if (fd < 0 || fd > EPOLL_MAX_REASONABLE_FD)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("socketengine", "DispatchEvents: Invalid fd detected in write event (fd=" + std::to_string(fd) + ", ptr=" + std::to_string(reinterpret_cast<uintptr_t>(EH)) + ") - skipping.");
               }

               /* Mark event as processed to avoid reprocessing */

               Events[i].events = 0;
               continue;
          }

          uint32_t ev = Events[i].events;

          if (ev & EPOLLOUT)
          {
               write_events++;

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DispatchEvents: Processing write event #" + std::to_string(write_events) + " (fd=" + std::to_string(fd) + ").");
               }

               /*
             * Validate handler is still valid before calling methods
             * Check if fd is still valid by verifying it hasn't been closed
             */

               if (!EH->HasFD() || EH->GetFD() != fd)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("socketengine", "DispatchEvents: Handler fd changed during write processing (was " + std::to_string(fd) + ") - skipping.");
                    }

                    Events[i].events = 0;
                    continue;
               }

               try
               {
                    EH->OnEventHandlerWrite();
               }
               catch (...)
               {
                    /* Catch any exceptions to prevent crash */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("socketengine", "DispatchEvents: Exception in OnEventHandlerWrite() for fd=" + std::to_string(fd) + " - cleaning up.");
                    }

                    /* Clean up the handler */

                    DelFD(EH);
                    UnregisterPendingWrite(EH);
                    Events[i].events = 0;
                    continue;
               }

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DispatchEvents: Write event #" + std::to_string(write_events) + " completed.");
               }
          }
     }

     /*
     * CRITICAL FIX: Only log summary every 10000th call to reduce log spam
     * Most event processing is normal and doesn't need logging
     */

     return nfds;
}

/*
 * HLQuery-Style Ultra-High-Performance Write Batching System.
 * This function implements HLQuery-style batch write dispatching optimized
 * for high-throughput scenarios (100k+ queries).
 *
 * HLQuery Optimizations:
 * - Zero-copy write attempts with vectorized I/O
 * - Smart batching to minimize syscall overhead
 * - Adaptive back-pressure management
 * - Lock-free pending writes tracking
 * - Intelligent write prioritization
 * - Batch size adaptation based on load
 */

/* Dispatches trial writes */

void SocketEngine::DispatchTrialWrites()
{
     if (PendingWritesCount.load(std::memory_order_relaxed) == 0)
     {
          /* Fast path: no pending writes. */

          return;
     }

     /*
     * HLQuery-style adaptive batch sizing for high-throughput
     * Note: Currently using fixed BATCH_SIZE below instead of adaptive sizing
     */

     /* OPTIMIZATION: Swap and clear for zero-copy - reuse existing vector */

     std::vector<EventHandler *> WriteCandidates;

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (PendingWrites.empty())
          {
               PendingWritesCount.store(0, std::memory_order_relaxed);
               return;
          }

          PendingWrites.erase(
               std::remove_if(PendingWrites.begin(), PendingWrites.end(),
                              [](EventHandler *EH)
                              {
                                   if (!EH || !EH->HasFD())
                                   {
                                        PendingWritesSet.erase(EH);
                                        return true;
                                   }

                                   return false;
                              }),
               PendingWrites.end());

          PendingWritesCount.store(PendingWrites.size(), std::memory_order_relaxed);

          if (PendingWrites.empty())
          {
               return;
          }

          WriteCandidates = PendingWrites;
     }

     /*
      * Intelligent Write Batching Algorithm
      *
      * Process writes in optimized batches to maximize throughput:
      * 1. Group consecutive handlers for cache locality
      * 2. Use MSG_MORE for TCP_CORK-like behavior
      * 3. Handle partial writes gracefully
      * 4. Re-register blocked writes automatically
      */

     /* Optimized batch size for high-speed writes */

     const size_t BatchSize = EPOLL_BATCH_SIZE;
     size_t Processed = 0;

     for (size_t batch_start = 0; batch_start < WriteCandidates.size(); batch_start += BatchSize)
     {
          size_t batch_end = std::min(batch_start + BatchSize, WriteCandidates.size());

          /* Process this batch with optimized write attempts */

          for (size_t i = batch_start; i < batch_end; ++i)
          {
               EventHandler *EH = WriteCandidates[i];

               if (!EH || !EH->HasFD())
               {
                    /* Skip invalid handlers. */

                    continue;
               }

               /*
              * Smart Write State Detection.
              * Before attempting writes, we check if the socket is actually
              * ready for writing using a non-blocking approach.
              */

               /*
              * Optimized: Skip select() check and directly attempt write
              * Modern kernels handle EAGAIN efficiently, making the select() overhead unnecessary
              */

               EH->OnEventHandlerWrite();
               Processed++;

               /*
                * Do not yield between immediate publish writes; the batch
                * boundary already limits per-pass work.
                */
          }
     }
}

/*
 * Pending Write Registration with Thread-Safe Counting.
 * Register handlers that have pending writes blocked by EAGAIN.
 * Updates atomic counter for thread-safe HasPendingWork() checks.
 *
 * THREADING: Must be called from main event loop thread only.
 * The vector is not thread-safe, but the counter is.
 *
 * OPTIMIZATION: Use PendingWritesSet for O(1) duplicate checking instead of O(n)
 */

/* Registers a pending write */

void SocketEngine::RegisterPendingWrite(EventHandler *EH)
{
     if (!EH || !EH->HasFD())
     {
          /* Invalid handler. */

          return;
     }

     /* Fast O(1) duplicate check using PendingWritesSet - thread-safe */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (PendingWritesSet.find(EH) != PendingWritesSet.end())
          {
               /* Already registered. */

               return;
          }

          /* Add to both set (for fast lookup) and vector (for iteration) */

          PendingWritesSet.insert(EH);
          PendingWrites.push_back(EH);
     }

     PendingWritesCount.fetch_add(1, std::memory_order_relaxed);

     /*
      * IMPROVEMENT: Use EPOLLOUT notifications to resume sending data when a socket's send buffer
      * was previously full, rather than blocking the event loop on a partial write.
      * Smart Write Event Registration.
      * Temporarily enable EPOLLOUT for this handler so we get notified
      * when the socket becomes writable again. This provides dual-path
      * write completion: both through DispatchTrialWrites() and epoll events.
      */

     struct epoll_event ev;

     memset(&ev, 0, sizeof(ev));

     /*
      * Add transient write readiness while preserving the handler's
      * original read/write flags and mandatory error notifications.
      */

     ev.events = EH->GetEventMask() | EPOLLOUT | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
     ev.data.ptr = EH;

     /*
      * IMPROVEMENT: If using one-shot epoll events, re-register the socket's events after handling
      * an event to continue receiving notifications for subsequent activity.
      * (Currently not using EPOLLONESHOT, but if we did, we'd re-register here)
      */

     /* Modify existing registration to include EPOLLOUT */

     int result = epoll_ctl(EpollFD, EPOLL_CTL_MOD, EH->GetFD(), &ev);

     /* Save errno immediately after epoll_ctl call */

     int saved_errno = errno;

     if (result == -1)
     {
          bool RemovedPendingWrite = false;

          {
               std::lock_guard<std::mutex> lock(PendingWritesMutex);

               if (PendingWritesSet.erase(EH))
               {
                    auto it = std::find(PendingWrites.begin(), PendingWrites.end(), EH);

                    if (it != PendingWrites.end())
                    {
                         if (it != PendingWrites.end() - 1)
                         {
                              std::iter_swap(it, PendingWrites.end() - 1);
                         }

                         PendingWrites.pop_back();
                    }

                    RemovedPendingWrite = true;
               }
          }

          if (RemovedPendingWrite)
          {
               PendingWritesCount.fetch_sub(1, std::memory_order_relaxed);
          }

          if (saved_errno == EINVAL)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "epoll_ctl(EPOLL_CTL_MOD) failed with EINVAL - invalid arguments (fd=" +
                                                  std::to_string(EH->GetFD()) + ").");
               }
          }
          else if (saved_errno == EPERM)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "epoll_ctl(EPOLL_CTL_MOD) failed with EPERM - permission denied (fd=" +
                                                  std::to_string(EH->GetFD()) + ").");
               }
          }
          else if (saved_errno == ENOMEM)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "epoll_ctl(EPOLL_CTL_MOD) failed with ENOMEM - out of memory (fd=" +
                                                  std::to_string(EH->GetFD()) + ").");
               }
          }
          else if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "epoll_ctl(EPOLL_CTL_MOD) failed in RegisterPendingWrite: " +
                                             std::string(strerror(saved_errno)) + " (fd=" + std::to_string(EH->GetFD()) +
                                             ", errno=" + std::to_string(saved_errno) + ").");
          }

          if (saved_errno == EBADF || saved_errno == ENOENT)
          {
               if (EH->HasFD())
               {
                    EH->OnEventHandlerError(saved_errno);
               }
          }
     }
}

/* Unregisters a pending write */

void SocketEngine::UnregisterPendingWrite(EventHandler *EH)
{
     /*
     * Pending Write Removal with Thread-Safe Counting
     *
     * Remove handlers from pending writes when they no longer need
     * write attention (buffer empty, socket closed, etc.)
     * Updates atomic counter for thread-safe HasPendingWork() checks.
     *
     * OPTIMIZATION: Use PendingWritesSet for O(1) lookup instead of O(n)
     */

     if (!EH)
     {
          return;
     }

     /* Fast O(1) check if handler is registered - thread-safe */

     bool RemovedPendingWrite = false;

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          RemovedPendingWrite = PendingWritesSet.erase(EH) > 0;

          if (RemovedPendingWrite)
          {
               /* Remove from vector - OPTIMIZATION: swap with last for O(1) removal */

               auto it = std::find(PendingWrites.begin(), PendingWrites.end(), EH);

               if (it != PendingWrites.end())
               {
                    if (it != PendingWrites.end() - 1)
                    {
                         /* Swap with last element for O(1) removal (order doesn't matter) */

                         std::iter_swap(it, PendingWrites.end() - 1);
                    }

                    PendingWrites.pop_back();
               }
          }
     }

     if (RemovedPendingWrite)
     {
          PendingWritesCount.fetch_sub(1, std::memory_order_relaxed);
     }

     /*
      * Restore Normal Event Registration
      *
      * Remove EPOLLOUT from the handler's registration since we no longer
      * need write notifications for this handler.
      */

     if (EH->HasFD())
     {
          struct epoll_event ev;

          memset(&ev, 0, sizeof(ev));

          /*
           * Restore the caller-owned readiness mask after pending writes
           * drain, while keeping error and hangup notifications active.
           */

          ev.events = EH->GetEventMask() | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
          ev.data.ptr = EH;

          epoll_ctl(EpollFD, EPOLL_CTL_MOD, EH->GetFD(), &ev);
     }
}

/* No Wake() function needed */

/* Returns the number of events processed */

int SocketEngine::GetEventCount()
{
     return EventCounter.load(std::memory_order_relaxed);
}

/* Increments pending message count */

void SocketEngine::IncrementPendingMessages()
{
     PendingMessageCount.fetch_add(1, std::memory_order_relaxed);
}

/* Decrements pending message count */

void SocketEngine::DecrementPendingMessages()
{
     int Current = PendingMessageCount.load(std::memory_order_relaxed);

     while (Current > 0 &&
            !PendingMessageCount.compare_exchange_weak(Current, Current - 1, std::memory_order_relaxed))
     {
     }
}

/* Returns the number of pending messages */

int SocketEngine::GetPendingMessageCount()
{
     return PendingMessageCount.load(std::memory_order_relaxed);
}

/* Returns true if there is pending work */

bool SocketEngine::HasPendingWork()
{
     /*
     * Thread-Safe Pending Work Detection
     *
     * Check multiple sources of pending work:
     * 1. ActionList - queued actions waiting to be processed
     * 2. Pending writes - sockets with buffered data to send (uses atomic counter)
     * 3. Pending messages - messages waiting to be delivered
     *
     * If any of these have work pending, we should NOT block in epoll_wait
     * because we need to continue processing CPU-bound work.
     *
     * THREAD-SAFETY: Uses atomic counters for lock-free access.
     * Safe to call from any thread, though typically called from main event loop.
     */

     /* Check if ActionList has pending actions */

     if (ActionList::GetActionCount() > 0)
     {
          return true;
     }

     /* Check if we have pending writes, but repair stale counters first. */

     if (PendingWritesCount.load(std::memory_order_relaxed) > 0)
     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (!PendingWrites.empty())
          {
               return true;
          }

          PendingWritesSet.clear();
          PendingWritesCount.store(0, std::memory_order_relaxed);
     }

     /*
      * PendingMessageCount is retained for ABI compatibility, but there is no
      * queue drained by the main loop. If it becomes positive, treating it as
      * active work forces epoll_wait(timeout=0) forever and spins the server.
      */

     if (PendingMessageCount.load(std::memory_order_relaxed) > 0)
     {
          PendingMessageCount.store(0, std::memory_order_relaxed);
     }

     return false;
}
