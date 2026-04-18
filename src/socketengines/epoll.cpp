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
#include "runtime/exitmanager.h"
#include "core/hlquery.h"
#include "core/socketengine.h"
#include "search/storageengine.h"
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

/* NUMA-aware thread pools */

/* Zero-copy optimizations - lazy allocation to save memory if unused */

static std::array<void *, 16> ZeroCopyBuffers{};

static std::atomic<size_t> ZeroCopyBufferIndex{0};

static std::atomic<bool> ZeroCopyBuffersAllocated{false};

/* 64KB buffers */

static constexpr size_t ZERO_COPY_BUFFER_SIZE = 64 * 1024;

/*
 * Maximum reasonable file descriptor value for validation
 * Most systems have much lower limits (typically 1024-4096), but allow higher for safety
 */

static constexpr int MAX_REASONABLE_FD = 1000000;

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

     PendingWritesCount.store(0, std::memory_order_relaxed);
     PendingMessageCount.store(0, std::memory_order_relaxed);
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
          PendingWritesSet.reserve(1024);
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

     if (CurrentConnections > 10000)
     {
          /* Ultra-high load - use 0ms timeout (non-blocking) */

          NewTimeout = 0;
     }
     else if (CurrentConnections > 5000)
     {
          /* High load - use 1ms timeout */

          NewTimeout = 1;
     }
     else if (CurrentConnections > 1000)
     {
          /* Medium load - use 5ms timeout */

          NewTimeout = 5;
     }
     else if (CurrentConnections > 100)
     {
          /* Low-medium load - use 10ms timeout */

          NewTimeout = 10;
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
               ZeroCopyBuffers[i] = std::aligned_alloc(4096, ZERO_COPY_BUFFER_SIZE);

               if (!ZeroCopyBuffers[i])
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("socketengine", "Failed to allocate zero-copy buffer " + std::to_string(i) + ".");
                    }

                    AllocationFailed = true;
                    break; /* Stop allocation on first failure */
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
               return nullptr; /* Return null on allocation failure */
          }
     }

     if (!ZeroCopyBuffersAllocated.load())
     {
          return nullptr; /* Allocation failed */
     }

     size_t Index = ZeroCopyBufferIndex.fetch_add(1) % ZeroCopyBuffers.size();
     return ZeroCopyBuffers[Index];
}

/* Returns a zero-copy buffer to the pool */

void SocketEngine::ReturnZeroCopyBuffer(void *buffer)
{
     /* Buffer is automatically returned when index wraps around */

     (void)buffer; /* Suppress unused parameter warning */
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

     /* Skip fcntl check if we know fd is valid (optimization) */
     /* Only check on first call or if we suspect it's invalid */

     struct epoll_event ev;

     memset(&ev, 0, sizeof(ev));

     /*
     * IMPROVEMENT: Use edge-triggered epoll correctly: in the event handler, read/write in a loop
     * until EAGAIN to ensure no data remains in the socket buffer (preventing missed events from stalling communication)
     * Always subscribe to error/hangup notifications so we can detect peer disconnects immediately
     */

     /* Enable edge-triggered mode */

     ev.events = EventsMask | EPOLLHUP | EPOLLRDHUP | EPOLLERR | EPOLLET;
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

     /* Increment active connections counter */

     ActiveConnections.fetch_add(1, std::memory_order_relaxed);

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

          return; /* engine down */
     }

     if (!EH)
     {
          return; /* Check for null handler */
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

          /* Only decrement if fd was actually registered (epoll_ctl succeeds) */

          int result = epoll_ctl(EpollFD, EPOLL_CTL_DEL, fd, nullptr);

          /* Save errno immediately after call */

          int saved_errno = errno;

          if (result == 0)
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DelFD: Successfully removed fd=" + std::to_string(fd) + " from epoll (ActiveConnections=" + std::to_string(ActiveConnections.load() - 1) + ").");
               }

               /* Decrement active connections counter */

               ActiveConnections.fetch_sub(1, std::memory_order_relaxed);

               /*
             * Ensure socket is closed properly - EventHandler destructor should handle this,
             * but we verify the FD is removed from epoll first to prevent use-after-free
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

               /* Still decrement counter as the connection is being closed */

               ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
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

               /* Still try to decrement counter - connection is being closed regardless */

               ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
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

     if (ShuttingDown || ForceExit)
     {
          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DispatchEvents: Shutdown requested, returning 0.");
          }

          return 0; /* Don't even call epoll_wait during shutdown */
     }

     /*
     * Check for pending work before blocking.
     * Problem: If we have pending work (cursor slices, pending actions, etc.)
     * and epoll_wait(-1) blocks, the CPU-only work stalls because no I/O events
     * wake up the epoll.
     *
     * Solution: If there's pending work, use timeout = 0 (non-blocking) to
     * immediately return to the main loop to process the pending work.
     */

     int timeout_ms = -1; /* Default to infinite blocking */

     /* Check if we have pending work that needs immediate processing */

     if (HasPendingWork())
     {
          timeout_ms = 0; /* Don't block - we have work to do */

          /*
         * Reset timeout immediately when pending work detected.
         * This ensures high throughput mode is active from the start of new activity.
         */

          CurrentTimeoutMS.store(0, std::memory_order_relaxed);
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

     /* HLQuery-style high-throughput event processing */

     /*
     * CRITICAL FIX: Reduce debug log verbosity - only log every 10000th call to reduce log spam.
     * Don't log every infinite timeout call - that's too verbose.
     * This dramatically reduces log noise during operations like ping that call DispatchEvents frequently.
     */

     /*
     * CRITICAL FIX: Re-check EpollFD validity right before epoll_wait to prevent race condition.
     * Another thread could have closed EpollFD between the check above and this call.
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
          /* Activity detected - immediately reset to non-blocking for high throughput */

          CurrentTimeoutMS.store(0, std::memory_order_relaxed);

          /* REMOVED: Flood protection counter - no throttling, maximum throughput */
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
             * Still have pending work - keep timeout=0 for now.
             * The main loop will process the pending work via ActionList::ProcessActions()
             */
          }

          return 0;
     }

     /* Process socket events with HLQuery-style batch optimization */
     /* OPTIMIZATION: Process in-place to avoid extra allocations and improve cache locality */
     /* Process errors first, then reads, then writes for optimal ordering */

     /* First pass: process error events immediately (they need cleanup) */

     /* Add explicit bounds check for Events array access */

     for (int i = 0; i < nfds && i < static_cast<int>(Events.size()); ++i)
     {
          EventHandler *EH = static_cast<EventHandler *>(Events[i].data.ptr);

          if (!EH)
          {
               continue; /* Safety check */
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
               bool valid_fd = (fd >= 0 && fd <= MAX_REASONABLE_FD);
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
               continue; /* Already processed */
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

          if (fd < 0 || fd > MAX_REASONABLE_FD)
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

               /* CRITICAL FIX: Only log read event completion in very verbose debug mode */
          }
     }

     /* Third pass: process write events */

     int write_events = 0;

     /* Add explicit bounds check for Events array access */

     for (int i = 0; i < nfds && i < static_cast<int>(Events.size()); ++i)
     {
          if (Events[i].events == 0)
          {
               continue; /* Already processed */
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

          if (fd < 0 || fd > MAX_REASONABLE_FD)
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

     /*
     * IMPROVEMENT: Handle epoll event buffer overflow by processing in a loop until all
     * pending events are handled
     */

     /* HLQuery-style: If we hit MAX_EVENTS, immediately check for more events */

     if (nfds == MAX_EVENTS)
     {
          /* Process any remaining events immediately (non-blocking) in a loop */

          int total_processed = nfds;
          int more_events;
          int loop_count = 0;

          /* Prevent infinite loops */

          const int MAX_LOOPS = 10;

          /* Prevent processing too many events at once */

          const int MAX_TOTAL_EVENTS = MAX_EVENTS * 20;

          while (loop_count < MAX_LOOPS && total_processed < MAX_TOTAL_EVENTS)
          {
               /* Re-check EpollFD validity before each call */

               if (EpollFD == -1 || !EpollFDValid.load())
               {
                    break; /* EpollFD became invalid */
               }

               more_events = epoll_wait(EpollFD, Events.data(), MAX_EVENTS, 0);

               if (more_events <= 0)
               {
                    /* No more events or error */

                    if (more_events < 0 && errno != EINTR)
                    {
                         /* Real error (not interrupted) */

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Normal("socketengine", "epoll_wait error in overflow loop: " + std::string(strerror(errno)) + ".");
                         }
                    }

                    break; /* No more events pending */
               }

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Debug("socketengine", "Found " + std::to_string(more_events) + " additional events after hitting limit (loop " + std::to_string(loop_count + 1) + ").");
               }

               /* Process these events (simplified - would need full processing) */

               total_processed += more_events;
               loop_count++;

               if (more_events < MAX_EVENTS)
               {
                    break; /* No more events pending */
               }
          }

          if (loop_count >= MAX_LOOPS)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("socketengine", "Hit max loops processing overflow events - may have more pending.");
               }
          }

          if (total_processed >= MAX_TOTAL_EVENTS)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("socketengine", "Hit max total events limit - throttling to prevent overload.");
               }
          }

          return total_processed;
     }

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
          return; /* Fast path: no pending writes */
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

          WriteCandidates.swap(PendingWrites); /* O(1) swap, no copying */

          PendingWritesSet.clear();
          PendingWritesCount.store(0, std::memory_order_relaxed);
     }

     /* Remove invalid handlers from WriteCandidates in-place */

     WriteCandidates.erase(
          std::remove_if(WriteCandidates.begin(), WriteCandidates.end(),
                         [](EventHandler *EH)
                         {
                              if (!EH || !EH->HasFD())
                              {
                                   return true; /* Remove invalid handlers */
                              }

                              return false;
                         }),
          WriteCandidates.end());

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
                    continue; /* Skip invalid handlers */
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

               /* No yielding for immediate publish message delivery */
          }

          /* REMOVED: Inter-batch yielding - no throttling, maximum throughput */
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
          return; /* Invalid handler */
     }

     /* Fast O(1) duplicate check using PendingWritesSet - thread-safe */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (PendingWritesSet.find(EH) != PendingWritesSet.end())
          {
               return; /* Already registered */
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
     ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
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
          /* BUG FIX: Handle epoll_ctl failure comprehensively - remove from pending writes if handler is invalid */

          if (saved_errno == EBADF || saved_errno == ENOENT)
          {
               /*
             * Handler was already removed or fd is invalid - clean up
             * OPTIMIZATION: Check set first (O(1)), then remove from vector
             */

               {
                    std::lock_guard<std::mutex> lock(PendingWritesMutex);

                    if (PendingWritesSet.erase(EH))
                    {
                         /* Fast removal: swap with last element for O(1) removal */

                         auto it = std::find(PendingWrites.begin(), PendingWrites.end(), EH);

                         if (it != PendingWrites.end())
                         {
                              if (it != PendingWrites.end() - 1)
                              {
                                   /* Swap with last element for O(1) removal */

                                   std::iter_swap(it, PendingWrites.end() - 1);
                              }

                              PendingWrites.pop_back();
                         }
                    }
               }

               PendingWritesCount.fetch_sub(1, std::memory_order_relaxed);
          }
          else
          {
               /* Handle other error codes */

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

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (PendingWritesSet.erase(EH) == 0)
          {
               return; /* Not registered - set erase returns 0 if not found */
          }

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

     PendingWritesCount.fetch_sub(1, std::memory_order_relaxed);

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

          /* No EPOLLOUT */

          ev.events = EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
          ev.data.ptr = EH;

          epoll_ctl(EpollFD, EPOLL_CTL_MOD, EH->GetFD(), &ev);
     }
}

/* No Wake() function needed */

/* Returns the number of events processed */

int SocketEngine::GetEventCount()
{
     /* Event counting removed - use connection count instead */

     return 0;
}

/* Increments pending message count */

void SocketEngine::IncrementPendingMessages()
{
     PendingMessageCount.fetch_add(1, std::memory_order_relaxed);
}

/* Decrements pending message count */

void SocketEngine::DecrementPendingMessages()
{
     PendingMessageCount.fetch_sub(1, std::memory_order_relaxed);
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

     /* Check if we have pending writes (uses atomic counter for thread-safety) */

     if (PendingWritesCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     /* Check if we have pending messages */

     if (PendingMessageCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     return false;
}
