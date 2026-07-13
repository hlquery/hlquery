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
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/actionlist.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "core/logmanager.h"
#include "core/socketengine.h"
#include "runtime/timers.h"
#include "search/storageengine.h"

/* HLQuery ae.c inspired - clean and simple poll backend */

static std::vector<struct pollfd> PollFDs;

static std::unordered_map<int, EventHandler *> FDToHandler;

static std::vector<EventHandler *> HandlerByFD;

/* OPTIMIZATION: Reverse map from EventHandler* to pollfd index for O(1) DelFD */

static std::unordered_map<EventHandler *, size_t> HandlerToIndex;

static std::mutex RegistryMutex;

/* Define static members declared in socketengine.h */

/* Poll has no engine descriptor, but the shared interface requires one. */

int SocketEngine::EpollFD = -1;

/* Poll has no descriptor validity state, but the shared interface requires one. */

std::atomic<bool> SocketEngine::EpollFDValid{false};

/* Poll does not use epoll events, but the shared interface requires storage. */

std::vector<epoll_event> SocketEngine::Events;

std::vector<EventHandler *> SocketEngine::PendingWrites;

std::atomic<size_t> SocketEngine::PendingWritesCount{0};

std::atomic<int> SocketEngine::PendingMessageCount{0};

/* OPTIMIZATION: Fast duplicate checking for pending writes */

static std::unordered_set<EventHandler *> PendingWritesSet;

/* Mutex to protect PendingWrites and PendingWritesSet for thread safety */

static std::mutex PendingWritesMutex;

static int MaxFD = -1;

/* Adaptive timeout support (consistent with epoll) */

static std::atomic<bool> AdaptiveTimeoutEnabled{true};

static std::atomic<int> CurrentTimeoutMS{-1};

static std::atomic<uint64_t> TotalBytesProcessed{0};

static std::atomic<uint64_t> ActiveConnections{0};

static std::atomic<bool> EngineInitialized{false};

static std::atomic<int> EventCounter{0};

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
     if (EngineInitialized.load(std::memory_order_acquire))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "SocketEngine::Init() called multiple times! PollFDs.size()=" +
                                             std::to_string(PollFDs.size()) + " - this will clear registered FDs.");
          }

          /* Don't clear if already initialized - preserve existing registrations */

          return;
     }

     /* HLQuery ae_poll.c style - just initialize data structures */

     PollFDs.clear();
     PollFDs.reserve(MAX_EVENTS);
     FDToHandler.clear();
     HandlerByFD.clear();
     HandlerToIndex.clear();
     SocketEngine::PendingWrites.clear();
     MaxFD = -1;

     /* No file descriptors to create - poll() is simpler than epoll */

     /* Initialize advanced I/O optimizations */

     InitializeAdvancedIO();

     /* Initialize adaptive timeout */

     InitializeAdaptiveTimeout();

     EngineInitialized.store(true, std::memory_order_release);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("socketengine", "High-performance poll engine initialized.");
     }
}

/* Deinitializes the socket engine */

void SocketEngine::Deinit()
{
     /* Clean shutdown - no file descriptors to close */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "Deinit: Clearing " + std::to_string(PollFDs.size()) + " registered file descriptors.");
     }

     PollFDs.clear();
     FDToHandler.clear();
     HandlerByFD.clear();
     HandlerToIndex.clear();

     /* Thread-safe cleanup of pending writes */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          SocketEngine::PendingWrites.clear();
          PendingWritesSet.clear();
     }

     SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
     SocketEngine::PendingMessageCount.store(0, std::memory_order_relaxed);
     EventCounter.store(0, std::memory_order_relaxed);
     MaxFD = -1;
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

     std::lock_guard<std::mutex> registry_lock(RegistryMutex);

     if (FDToHandler.find(fd) != FDToHandler.end())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "AddFD: FD " + std::to_string(fd) + " already registered.");
          }

          /* Already registered. */

          return false;
     }

     /* Add to poll fd array */

     struct pollfd pfd;

     pfd.fd = fd;
     pfd.events = 0;
     pfd.revents = 0;

     /* Convert epoll events to poll events */

     if (EventsMask & EPOLLIN)
     {
          pfd.events |= POLLIN;
     }

     if (EventsMask & EPOLLOUT)
     {
          pfd.events |= POLLOUT;
     }

     /* Handle POLLPRI (out-of-band data) - equivalent to EPOLLPRI */

     if (EventsMask & EPOLLPRI)
     {
          pfd.events |= POLLPRI;
     }

     /* Note: POLLERR, POLLHUP, POLLNVAL are output-only flags (revents)
     * They should NOT be set in events - poll() will set them in revents automatically
     * when errors/hangups occur
     *
     * POLLRDHUP is also output-only on some systems (Linux 2.6.17+)
     */

     size_t index = PollFDs.size();

     PollFDs.push_back(pfd);
     FDToHandler[fd] = EH;
     if (fd >= static_cast<int>(HandlerByFD.size()))
     {
          HandlerByFD.resize(static_cast<size_t>(fd) + 1U, nullptr);
     }

     HandlerByFD[fd] = EH;

     /* Store the index for O(1) deletion. */

     HandlerToIndex[EH] = index;

     /*
      * Preserve the original readiness subscription so temporary POLLOUT
      * interest does not erase a caller-owned write registration.
      */

     EH->SetEventMask(EventsMask);

     if (fd > MaxFD)
     {
          MaxFD = fd;
     }

     /* Increment active connections counter */

     ActiveConnections.fetch_add(1, std::memory_order_relaxed);

     /* DEBUG: Log when FD is added */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "AddFD: Registered fd=" + std::to_string(fd) + " with EventsMask=0x" + std::to_string(EventsMask) + " (poll events=0x" + std::to_string(pfd.events) + "), PollFDs.size()=" + std::to_string(PollFDs.size()) + ".");
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

     {
          std::lock_guard<std::mutex> registry_lock(RegistryMutex);

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DelFD: Removing fd=" + std::to_string(fd) + " (PollFDs.size()=" + std::to_string(PollFDs.size()) + ").");
          }

          auto handler_it = FDToHandler.find(fd);

          if (handler_it == FDToHandler.end())
          {
               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("socketengine", "DelFD: FD " + std::to_string(fd) + " not registered.");
               }

               return;
          }

          if (handler_it->second != EH)
          {
               return;
          }

          auto index_it = HandlerToIndex.find(EH);

          if (index_it != HandlerToIndex.end())
          {
               size_t index = index_it->second;

               if (index >= PollFDs.size())
               {
                    HandlerToIndex.erase(index_it);
               }
               else
               {
                    if (index < PollFDs.size() - 1)
                    {
                         int swapped_fd = PollFDs.back().fd;
                         EventHandler *swapped_handler = nullptr;

                         if (swapped_fd >= 0 && swapped_fd < static_cast<int>(HandlerByFD.size()))
                         {
                              swapped_handler = HandlerByFD[swapped_fd];
                         }

                         if (swapped_handler)
                         {
                              HandlerToIndex[swapped_handler] = index;
                         }

                         std::swap(PollFDs[index], PollFDs.back());
                    }

                    PollFDs.pop_back();
                    HandlerToIndex.erase(index_it);
               }
          }

          FDToHandler.erase(handler_it);

          if (fd >= 0 && fd < static_cast<int>(HandlerByFD.size()))
          {
               HandlerByFD[fd] = nullptr;
          }

          if (fd == MaxFD)
          {
               MaxFD = -1;

               for (const auto &[fd_key, handler] : FDToHandler)
               {
                    (void)handler;
                    if (fd_key > MaxFD)
                    {
                         MaxFD = fd_key;
                    }
               }
          }
     }

     UnregisterPendingWrite(EH);
     ActiveConnections.fetch_sub(1, std::memory_order_relaxed);

     /*
      * The descriptor is no longer registered, so discard the cached
      * subscription used by pending-write bookkeeping.
      */

     EH->SetEventMask(0);
}

/* Dispatches pending events */

int SocketEngine::DispatchEvents()
{
     const bool ShutdownRequested = hlquery::ShouldShutdown() || hlquery::ShouldForceExit();

     /* Always log entry to DispatchEvents for troubleshooting */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: ENTRY (PollFDs.size()=" + std::to_string(PollFDs.size()) + ", ShuttingDown=" + std::to_string(hlquery::GetSignalShutdownState()) + ", ForceExit=" + std::to_string(hlquery::GetForceExitState()) + ").");
     }

     if (PollFDs.empty())
     {
          /* No file descriptors to monitor - check shutdown first */

          if (ShutdownRequested)
          {
               /* Do not sleep during shutdown. */

               return 0;
          }

          /*
           * No file descriptors are registered yet, so return immediately
           * and let the main loop handle timing and new registrations.
           */

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "DispatchEvents: PollFDs is empty, returning 0.");
          }

          return 0;
     }

     /*
     * Check shutdown flags before blocking
     *
     * Problem: If shutdown is requested (via Ctrl+C), we should not block
     * in poll() as this prevents immediate shutdown.
     *
     * Solution: If ShuttingDown or ForceExit is set, return immediately
     * to the main loop.
     */

     if (ShutdownRequested)
     {
          /* Do not call poll during shutdown. */

          return 0;
     }

     /*
     * Check for pending work before blocking
     *
     * Problem: If we have pending work (cursor slices, pending actions, etc.)
     * and poll(-1) blocks, the CPU-only work stalls because no I/O events
     * wake up the poll.
     *
     * Solution: If there's pending work, use a short timeout instead of a
     * permanent non-blocking poll. This keeps CPU-only work responsive without
     * letting stale pending-work state spin the server or starve socket events.
     */

     /*
      * Match epoll behavior by using a short wait while busy and an
      * indefinite block when idle to avoid unnecessary CPU use.
      */

     /* Default to infinite blocking when idle, like epoll. */

     int timeout_ms = -1;

     /* Check if we have pending work that needs immediate processing */

     if (HasPendingWork())
     {
          timeout_ms = SOCKET_ENGINE_PENDING_WORK_TIMEOUT_MS;

          /*
         * Reset timeout immediately when pending work detected
         * This ensures high throughput mode is active from the start of new activity
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
      * poll() returns ready descriptor count, zero on timeout, and -1 on
      * error while accepting infinite, non-blocking, or millisecond waits.
      */

     if (timeout_ms < -1)
     {
          /* Sanity check: invalid timeout, use infinite. */

          timeout_ms = -1;
     }

     /*
      * poll() requires a non-empty descriptor array and will block for at
      * most the configured timeout before returning to the event loop.
      */

     if (PollFDs.empty())
     {
          /* No file descriptors to monitor. */

          return 0;
     }

     /*
      * poll() expects nfds_t, so validate the vector size before casting.
      */

     if (PollFDs.size() > static_cast<size_t>(std::numeric_limits<nfds_t>::max()))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine",
                                        "PollFDs.size() (" + std::to_string(PollFDs.size()) +
                                             ") exceeds nfds_t maximum (" + std::to_string(std::numeric_limits<nfds_t>::max()) + ").");
          }

          return 0;
     }

     /*
      * Re-check shutdown flags before blocking because another thread may
      * have requested shutdown after the earlier guard.
      */

     if (hlquery::ShouldShutdown() || hlquery::ShouldForceExit())
     {
          /* Do not block during shutdown. */

          return 0;
     }

     /* DEBUG: Log before calling poll() */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode() && timeout_ms != -1)
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: About to call poll() with " + std::to_string(PollFDs.size()) + " fds, timeout=" + (timeout_ms == -1 ? std::string("infinite") : std::to_string(timeout_ms) + "ms") + ".");
     }

     int nfds = poll(PollFDs.data(), static_cast<nfds_t>(PollFDs.size()), timeout_ms);

     /* DEBUG: ALWAYS log poll() results - no troubleshooting */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode() && (nfds > 0 || timeout_ms == 0))
     {
          Instance->Logs->Debug("socketengine", "poll() returned " + std::to_string(nfds) + " events (timeout=" + (timeout_ms == -1 ? std::string("infinite") : std::to_string(timeout_ms) + "ms") + ", PollFDs.size()=" + std::to_string(PollFDs.size()) + (nfds < 0 ? ", errno=" + std::string(strerror(errno)) : "") + ".");
     }

     if (nfds < 0)
     {
          int err = errno;

          /* Check if poll was interrupted by a signal (EINTR) - should retry */

          if (err == EINTR)
          {
               /* Signal interrupted poll - check shutdown flags and retry if needed */

               if (hlquery::ShouldShutdown() || hlquery::ShouldForceExit())
               {
                    /* Shutdown requested, do not retry. */

                    return 0;
               }

               /* Retry poll with same timeout */

               nfds = poll(PollFDs.data(), static_cast<nfds_t>(PollFDs.size()), timeout_ms);

               if (nfds < 0)
               {
                    err = errno;

                    /* Still error after retry - check if it's still EINTR or real error */

                    if (err == EINTR)
                    {
                         /* Multiple interrupts, return to main loop. */

                         return 0;
                    }

                    /* Real error - log and return */

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("socketengine", "poll() failed after EINTR retry: " + std::string(strerror(err)) + ".");
                    }

                    return 0;
               }
          }
          else if (err == EINVAL)
          {
               /* Invalid arguments - nfds might be too large or invalid */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "poll() failed with EINVAL: nfds=" + std::to_string(PollFDs.size()) +
                                                  ", timeout=" + std::to_string(timeout_ms) + " - " + std::string(strerror(err)) + ".");
               }

               return 0;
          }
          else if (err == EFAULT)
          {
               /* Invalid pointer - should not happen but handle gracefully */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine", "poll() failed with EFAULT: invalid pointer.");
               }

               return 0;
          }
          else if (err == ENOMEM)
          {
               /* Out of memory - kernel couldn't allocate memory */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine", "poll() failed with ENOMEM: " + std::string(strerror(err)) + ".");
               }

               return 0;
          }
          else
          {
               /* Other error - log and return */

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("socketengine",
                                             "poll() failed: errno=" + std::to_string(err) + " (" + std::string(strerror(err)) + ").");
               }

               return 0;
          }
     }

     if (nfds == 0)
     {
          /*
         * Timeout occurred - no events to process
         * If timeout was -1 (infinite), this shouldn't happen unless interrupted
         * If timeout was 0 (non-blocking), this is normal - no events ready
         */

          /* If we got timeout but have pending work, switch to non-blocking mode */

          if (HasPendingWork())
          {
               CurrentTimeoutMS.store(0, std::memory_order_relaxed);
          }

          /* Timeout with no events to process, return to main loop. */

          return 0;
     }

     /*
     * CRITICAL FIX: Reset timeout immediately when events are detected
     * This prevents second benchmark from starting slow due to accumulated timeout
     * from previous idle period. Reset to 0ms for maximum throughput.
     */

     if (nfds > 0)
     {
          EventCounter.fetch_add(nfds, std::memory_order_relaxed);

          /* Activity detected - immediately reset to non-blocking for high throughput */

          CurrentTimeoutMS.store(0, std::memory_order_relaxed);
     }

     /* HLQuery-style flush when event queue is maxed out or under high load */

     if (nfds > 0)
     {
          bool should_flush = false;

          if (nfds >= MAX_EVENTS)
          {
               /* Force flush when we hit maximum events (like HLQuery) */

               should_flush = true;
          }
          else if (nfds >= MAX_EVENTS / 2)
          {
               /* Flush when we have many events */

               static int consecutive_high_events = 0;

               consecutive_high_events++;

               if (consecutive_high_events >= 3)
               {
                    should_flush = true;
                    consecutive_high_events = 0;
               }
          }

          if (should_flush && Instance && Instance->Database)
          {
               try
               {
                    Instance->Database->Flush();
               }
               catch (...)
               {
                    /* Ignore flush errors and continue processing. */

               }
          }
     }

     /*
      * Process the descriptors only after poll reports events because
      * readiness is delivered through each descriptor's revents field.
      */

     /* Track number of events processed. */

     int events_processed = 0;

     /* Only process events if poll() returned events (nfds > 0) */

     if (nfds > 0)
     {
          /* Process ready descriptors in a single pass to avoid rescanning the array three times. */

          std::vector<EventHandler *> handlers_to_delete;

          for (size_t i = 0; i < PollFDs.size(); ++i)
          {
               struct pollfd &pfd = PollFDs[i];

               if (pfd.revents == 0)
               {
                    continue;
               }

               if (pfd.fd < 0)
               {
                    pfd.revents = 0;
                    continue;
               }

               EventHandler *eh = nullptr;

               if (pfd.fd >= 0 && pfd.fd < static_cast<int>(HandlerByFD.size()))
               {
                    eh = HandlerByFD[pfd.fd];
               }

               if (!eh)
               {
                    pfd.revents = 0;
                    continue;
               }

               if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL
#ifdef POLLRDHUP
                                  | POLLRDHUP
#endif
                                  ))
               {
                    handlers_to_delete.push_back(eh);

                    try
                    {
                         if (eh->HasFD() && eh->GetFD() == pfd.fd)
                         {
                              eh->OnEventHandlerRead();
                         }
                    }
                    catch (...)
                    {
                    }

                    pfd.revents = 0;
                    continue;
               }

               if ((pfd.revents & POLLIN) != 0)
               {
                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         eh->OnEventHandlerRead();
                         events_processed++;
                    }
                    catch (...)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("socketengine", "Exception in OnEventHandlerRead() for fd=" + std::to_string(pfd.fd) + ".");
                         }

                         handlers_to_delete.push_back(eh);
                    }

                    pfd.revents &= ~POLLIN;
               }

               if ((pfd.revents & POLLPRI) != 0)
               {
                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         eh->OnEventHandlerRead();
                         events_processed++;
                    }
                    catch (...)
                    {
                         handlers_to_delete.push_back(eh);
                    }

                    pfd.revents &= ~POLLPRI;
               }

               if ((pfd.revents & POLLOUT) != 0)
               {
                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         eh->OnEventHandlerWrite();
                         UnregisterPendingWrite(eh);
                         events_processed++;
                    }
                    catch (...)
                    {
                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("socketengine", "Exception in OnEventHandlerWrite() for fd=" + std::to_string(pfd.fd) + ".");
                         }

                         handlers_to_delete.push_back(eh);
                    }

                    pfd.revents &= ~POLLOUT;
               }

               pfd.revents = 0;
          }

          /*
           * Delete handlers with errors after iteration so vector indexes
           * stay stable, then de-duplicate to avoid repeated cleanup.
           */

          std::unordered_set<EventHandler *> unique_handlers_to_delete(handlers_to_delete.begin(), handlers_to_delete.end());

          for (EventHandler *eh : unique_handlers_to_delete)
          {
               if (eh)
               {
                    DelFD(eh);
                    UnregisterPendingWrite(eh);
               }
          }

          /*
           * poll() already delivered the ready set for this cycle, so do
           * not recurse and rescan the descriptor array.
           */

     }

     /* DEBUG: Log final result */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: EXIT returning " + std::to_string(events_processed) + " events processed.");
     }

     return events_processed;
}

/* Dispatches trial writes */

void SocketEngine::DispatchTrialWrites()
{
     /*
     * HLQuery-inspired Trial Write System for poll()
     *
     * Since poll() doesn't have the same fine-grained control as epoll,
     * we use a simpler approach for handling blocked writes.
     */

     if (SocketEngine::PendingWritesCount.load(std::memory_order_relaxed) == 0)
     {
          /* Fast path: no pending writes. */

          return;
     }

     /*
      * Copy candidates while holding the lock, then release it before
      * writes because write handlers may update pending-write state.
      */

     std::vector<EventHandler *> WriteCandidates;

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (SocketEngine::PendingWrites.empty())
          {
               SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
               return;
          }

          SocketEngine::PendingWrites.erase(
               std::remove_if(SocketEngine::PendingWrites.begin(), SocketEngine::PendingWrites.end(),
                              [](EventHandler *EH)
                              {
                                   if (!EH || !EH->HasFD())
                                   {
                                        PendingWritesSet.erase(EH);
                                        return true;
                                   }

                                   return false;
                              }),
               SocketEngine::PendingWrites.end());

          SocketEngine::PendingWritesCount.store(SocketEngine::PendingWrites.size(), std::memory_order_relaxed);

          if (SocketEngine::PendingWrites.empty())
          {
               return;
          }

          WriteCandidates = SocketEngine::PendingWrites;
     }

     /*
      * Process writes immediately after releasing the lock so handlers can
      * safely register or unregister pending writes.
      */

     const size_t BatchSize = POLL_BATCH_SIZE;
     size_t ProcessedCount = 0;

     for (size_t batch_start = 0; batch_start < WriteCandidates.size(); batch_start += BatchSize)
     {
          size_t batch_end = std::min(batch_start + BatchSize, WriteCandidates.size());

          for (size_t i = batch_start; i < batch_end; ++i)
          {
               EventHandler *EH = WriteCandidates[i];

               if (!EH || !EH->HasFD())
               {
                    /* Skip invalid handlers. */

                    continue;
               }

               /* Simple approach: just try the write */

               EH->OnEventHandlerWrite();
               ProcessedCount++;

               /*
                * Do not yield between immediate publish writes; the batch
                * boundary already limits per-pass work.
                */

          }

          /*
           * Inter-batch yielding is intentionally omitted to keep publish
           * delivery latency low under load.
           */

     }
}

/* Registers a pending write */

void SocketEngine::RegisterPendingWrite(EventHandler *EH)
{
     if (!EH || !EH->HasFD())
     {
          /* Invalid handler. */

          return;
     }

     /* Thread-safe access to PendingWrites */

     std::lock_guard<std::mutex> lock(PendingWritesMutex);

     /* OPTIMIZATION: O(1) duplicate check using unordered_set */

     if (PendingWritesSet.find(EH) != PendingWritesSet.end())
     {
          /* Already registered. */

          return;
     }

     /* Add to both set and vector */

     PendingWritesSet.insert(EH);
     SocketEngine::PendingWrites.push_back(EH);
     SocketEngine::PendingWritesCount.fetch_add(1, std::memory_order_relaxed);

     /*
      * Enable POLLOUT through the cached index when possible, falling back
      * to a scan if the index map is stale.
      */

     int fd = EH->GetFD();
     {
          std::lock_guard<std::mutex> registry_lock(RegistryMutex);
          auto index_it = HandlerToIndex.find(EH);

          if (index_it != HandlerToIndex.end())
          {
               size_t index = index_it->second;

               if (index < PollFDs.size() && PollFDs[index].fd == fd)
               {
                    PollFDs[index].events |= POLLOUT;
                    return;
               }
          }

          /* Fallback: linear search if index map is stale */

          for (auto &pfd : PollFDs)
          {
               if (pfd.fd == fd)
               {
                    pfd.events |= POLLOUT;
                    break;
               }
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

     /* Thread-safe access to PendingWrites */

     std::lock_guard<std::mutex> lock(PendingWritesMutex);

     /* OPTIMIZATION: O(1) check with set, then O(1) removal from vector */

     if (PendingWritesSet.erase(EH) == 0)
     {
          /* Not registered. */

          return;
     }

     /* Remove from vector - swap with last for O(1) */

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

     /*
      * Disable transient POLLOUT through the cached index when possible,
      * falling back to a scan if the index map is stale.
      */

     if (EH->HasFD())
     {
          int fd = EH->GetFD();
          {
               std::lock_guard<std::mutex> registry_lock(RegistryMutex);
               auto index_it = HandlerToIndex.find(EH);

               if (index_it != HandlerToIndex.end())
               {
                    size_t index = index_it->second;

                   if (index < PollFDs.size() && PollFDs[index].fd == fd)
                   {
                         /*
                          * Remove transient write readiness only when the
                          * original registration did not request POLLOUT.
                          */

                        if ((EH->GetEventMask() & EPOLLOUT) == 0)
                        {
                             PollFDs[index].events &= ~POLLOUT;
                         }
                         return;
                    }
               }

               /* Fallback: linear search if index map is stale */

               for (auto &pfd : PollFDs)
               {
                   if (pfd.fd == fd)
                   {
                         /*
                          * The fallback path follows the same rule as the
                          * indexed path: preserve caller-owned POLLOUT.
                          */

                        if ((EH->GetEventMask() & EPOLLOUT) == 0)
                        {
                             pfd.events &= ~POLLOUT;
                         }
                         break;
                    }
               }
          }
     }
}

/*
 * HLQuery ae.c pattern - no Wake() function needed
 * The adaptive timeout in DispatchEvents() handles responsiveness
 * without requiring any wake mechanism
 */

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

     if (CurrentConnectionsValue > SOCKET_ENGINE_ULTRA_HIGH_LOAD_CONNECTIONS)
     {
          NewTimeoutValue = SOCKET_ENGINE_ULTRA_HIGH_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnectionsValue > SOCKET_ENGINE_HIGH_LOAD_CONNECTIONS)
     {
          NewTimeoutValue = SOCKET_ENGINE_HIGH_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnectionsValue > SOCKET_ENGINE_MEDIUM_LOAD_CONNECTIONS)
     {
          NewTimeoutValue = SOCKET_ENGINE_MEDIUM_LOAD_TIMEOUT_MS;
     }
     else if (CurrentConnectionsValue > SOCKET_ENGINE_LOW_MEDIUM_LOAD_CONNECTIONS)
     {
          NewTimeoutValue = SOCKET_ENGINE_LOW_MEDIUM_LOAD_TIMEOUT_MS;
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
     /* No-op: poll doesn't support zero-copy buffers */

}

/* Cleans up zero-copy buffers */

void SocketEngine::CleanupZeroCopyBuffers()
{
     /* No-op: poll doesn't support zero-copy buffers */

}

/* Starts I/O worker threads */

void SocketEngine::StartIOWorkerThreads()
{
     /* No-op: poll doesn't use worker threads */

}

/* I/O worker thread loop */

void SocketEngine::IOWorkerThread(unsigned int WorkerID)
{
     (void)WorkerID;
}

/* Sets optimal socket options */

void SocketEngine::SetOptimalSocketOptions()
{
     /* No-op: poll doesn't require special socket options */

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
     return EventCounter.load(std::memory_order_relaxed);
}

/* Increments pending message count */

void SocketEngine::IncrementPendingMessages()
{
     SocketEngine::PendingMessageCount.fetch_add(1, std::memory_order_relaxed);
}

/* Decrements pending message count */

void SocketEngine::DecrementPendingMessages()
{
     int Current = SocketEngine::PendingMessageCount.load(std::memory_order_relaxed);

     while (Current > 0 &&
            !SocketEngine::PendingMessageCount.compare_exchange_weak(Current, Current - 1, std::memory_order_relaxed))
     {
     }
}

/* Returns the number of pending messages */

int SocketEngine::GetPendingMessageCount()
{
     return SocketEngine::PendingMessageCount.load(std::memory_order_relaxed);
}

/* Returns true if there is pending work */

bool SocketEngine::HasPendingWork()
{
     /*
     * Check multiple sources of pending work:
     * 1. ActionList - queued actions waiting to be processed
     * 2. Pending writes - sockets with buffered data to send
     * 3. Pending messages - messages waiting to be delivered
     *
     * If any of these have work pending, we should NOT block in poll
     * because we need to continue processing CPU-bound work.
     */

     /* Check if ActionList has pending actions */

     if (ActionList::GetActionCount() > 0)
     {
          return true;
     }

     /* Check if we have pending writes (uses atomic counter for thread-safety) */

     if (SocketEngine::PendingWritesCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     /* Check if we have pending messages */

     if (SocketEngine::PendingMessageCount.load(std::memory_order_relaxed) > 0)
     {
          return true;
     }

     return false;
}
