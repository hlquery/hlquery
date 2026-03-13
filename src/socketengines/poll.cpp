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

#include "common/action_list.h"
#include "core/hlquery.h"
#include "core/socketengine.h"

/* HLQuery ae.c inspired - clean and simple poll backend */

static std::vector<struct pollfd> PollFDs;

static std::unordered_map<int, EventHandler *> FDToHandler;

/* OPTIMIZATION: Reverse map from EventHandler* to pollfd index for O(1) DelFD */

static std::unordered_map<EventHandler *, size_t> HandlerToIndex;

/* Define static members declared in socketengine.h */

int SocketEngine::EpollFD = -1; /* Not used in poll, but must be defined */

std::atomic<bool> SocketEngine::EpollFDValid{false}; /* Not used in poll, but must be defined */

std::vector<epoll_event> SocketEngine::Events; /* Not used in poll, but must be defined */

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

static int GetTimedWorkWakeupMs()
{
     int wake_ms = 1000;

     if (Instance && Instance->Timers)
     {
          const int timer_ms = Instance->Timers->GetTimeUntilNextMs();

          if (timer_ms == 0)
          {
               return 0;
          }

          if (timer_ms > 0)
          {
               wake_ms = std::min(wake_ms, timer_ms);
          }
     }

     return wake_ms;
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
     HandlerToIndex.clear();

     /* Thread-safe cleanup of pending writes */

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);
          SocketEngine::PendingWrites.clear();
          PendingWritesSet.clear();
     }

     SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
     SocketEngine::PendingMessageCount.store(0, std::memory_order_relaxed);
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

     if (FDToHandler.find(fd) != FDToHandler.end())
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("socketengine", "AddFD: FD " + std::to_string(fd) + " already registered.");
          }

          return false; /* Already registered */
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
     HandlerToIndex[EH] = index; /* OPTIMIZATION: Store index for O(1) DelFD */

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

     /* DEBUG: Log when FD is being removed */

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

          return; /* Not registered */
     }

     /* Validate that the handler matches what's in the map */

     if (handler_it->second != EH)
     {
          /* Handler mismatch - this shouldn't happen but handle gracefully */

          return;
     }

     /* OPTIMIZATION: O(1) removal using reverse index map */

     auto index_it = HandlerToIndex.find(EH);

     if (index_it != HandlerToIndex.end())
     {
          size_t index = index_it->second;

          /* Validate index is within bounds */

          if (index >= PollFDs.size())
          {
               /* Index is out of bounds - clear stale entry and continue */

               HandlerToIndex.erase(index_it);
          }
          else
          {
               /* Swap with last element for O(1) removal */

               if (index < PollFDs.size() - 1)
               {
                    /* Update the handler that's moving to our position */

                    int swapped_fd = PollFDs.back().fd;
                    auto swapped_handler_it = FDToHandler.find(swapped_fd);

                    /* Only update index if swapped handler exists in map */

                    if (swapped_handler_it != FDToHandler.end() && swapped_handler_it->second)
                    {
                         EventHandler *swapped_handler = swapped_handler_it->second;
                         HandlerToIndex[swapped_handler] = index;
                    }

                    std::swap(PollFDs[index], PollFDs.back());
               }

               PollFDs.pop_back();
               HandlerToIndex.erase(index_it);
          }
     }

     FDToHandler.erase(handler_it);
     UnregisterPendingWrite(EH);

     /* Decrement active connections counter */

     ActiveConnections.fetch_sub(1, std::memory_order_relaxed);

     /* OPTIMIZATION: Only recalculate MaxFD if we removed the max */

     if (fd == MaxFD)
     {
          MaxFD = -1;

          /* Only scan if we have fds left */

          if (!FDToHandler.empty())
          {
               for (const auto &[fd_key, handler] : FDToHandler)
               {
                    if (fd_key > MaxFD)
                    {
                         MaxFD = fd_key;
                    }
               }
          }
     }
}

/* Dispatches pending events */

int SocketEngine::DispatchEvents()
{
     /* Always log entry to DispatchEvents for troubleshooting */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: ENTRY (PollFDs.size()=" + std::to_string(PollFDs.size()) + ", ShuttingDown=" + std::to_string(ShuttingDown) + ", ForceExit=" + std::to_string(ForceExit) + ").");
     }

     if (PollFDs.empty())
     {
          /* No file descriptors to monitor - check shutdown first */

          if (ShuttingDown || ForceExit)
          {
               return 0; /* Don't sleep during shutdown */
          }

          /* No file descriptors yet - return immediately to allow main loop to continue */
          /* The main loop will handle timing and check for new connections */

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

     if (ShuttingDown || ForceExit)
     {
          return 0; /* Don't call poll during shutdown */
     }

     /*
     * Check for pending work before blocking
     *
     * Problem: If we have pending work (cursor slices, pending actions, etc.)
     * and poll(-1) blocks, the CPU-only work stalls because no I/O events
     * wake up the poll.
     *
     * Solution: If there's pending work, use timeout = 0 (non-blocking) to
     * immediately return to the main loop to process the pending work.
     */

     /* Match epoll behavior for performance - use 0ms (non-blocking) when busy */
     /* When idle, use -1 (block indefinitely) like epoll to avoid CPU waste */

     int timeout_ms = -1; /* Default to infinite blocking when idle (like epoll) */

     /* Check if we have pending work that needs immediate processing */

     if (HasPendingWork())
     {
          /* CRITICAL FIX: Use 0ms (non-blocking) when we have pending work - same as epoll */
          /* This is essential for benchmark performance - don't block at all when busy */

          timeout_ms = 0; /* Non-blocking - process pending work immediately */

          /*
         * Reset timeout immediately when pending work detected
         * This ensures high throughput mode is active from the start of new activity
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
     /* poll() returns number of file descriptors with events, or 0 on timeout, or -1 on error */
     /* timeout_ms can be -1 (infinite), 0 (non-blocking), or positive (milliseconds) */

     if (timeout_ms < -1)
     {
          timeout_ms = -1; /* Sanity check: invalid timeout, use infinite */
     }

     /* poll() must be called with valid parameters and non-empty array */
     /* poll() will block for at most timeout_ms milliseconds, then return */
     /* Ensure we have file descriptors to monitor */

     if (PollFDs.empty())
     {
          return 0; /* No file descriptors to monitor */
     }

     /* poll() expects nfds_t (unsigned int), not size_t - cast explicitly */
     /* Validate array size is within nfds_t limits */

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

     /* Re-check shutdown flags right before blocking call */
     /* Another thread could have set shutdown between the check above and this call */

     if (ShuttingDown || ForceExit)
     {
          return 0; /* Don't block during shutdown */
     }

     /* DEBUG: Log before calling poll() */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
     {
          Instance->Logs->Debug("socketengine", "DispatchEvents: About to call poll() with " + std::to_string(PollFDs.size()) + " fds, timeout=" + (timeout_ms == -1 ? std::string("infinite") : std::to_string(timeout_ms) + "ms") + ".");
     }

     int nfds = poll(PollFDs.data(), static_cast<nfds_t>(PollFDs.size()), timeout_ms);

     /* DEBUG: ALWAYS log poll() results - no troubleshooting */

     if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
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

               if (ShuttingDown || ForceExit)
               {
                    return 0; /* Shutdown requested, don't retry */
               }

               /* Retry poll with same timeout */

               nfds = poll(PollFDs.data(), static_cast<nfds_t>(PollFDs.size()), timeout_ms);

               if (nfds < 0)
               {
                    err = errno;

                    /* Still error after retry - check if it's still EINTR or real error */

                    if (err == EINTR)
                    {
                         return 0; /* Multiple interrupts, return to main loop */
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

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "poll() timeout: nfds=0, timeout_ms=" + (timeout_ms == -1 ? std::string("infinite") : std::to_string(timeout_ms) + "ms") + ", HasPendingWork()=" + std::string(HasPendingWork() ? "true" : "false") + ".");
          }

          /* If we got timeout but have pending work, switch to non-blocking mode */

          if (HasPendingWork())
          {
               CurrentTimeoutMS.store(0, std::memory_order_relaxed);
          }

          return 0; /* Timeout - no events to process, return to main loop */
     }

     /*
     * CRITICAL FIX: Reset timeout immediately when events are detected
     * This prevents second benchmark from starting slow due to accumulated timeout
     * from previous idle period. Reset to 0ms for maximum throughput.
     */

     if (nfds > 0)
     {
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
                    /* Ignore flush errors, continue processing */
               }
          }
     }

     /* Process events - HLQuery ae.c pattern - similar to epoll implementation */
     /* Process events when nfds > 0 (poll() detected events) */
     /* poll() sets revents on file descriptors that have events - we must check all fds */

     int events_processed = 0; /* Track number of events processed */

     /* Only process events if poll() returned events (nfds > 0) */

     if (nfds > 0)
     {
          /* DEBUG: Log when we have events to process */

          if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("socketengine", "Processing " + std::to_string(nfds) + " events from poll() - iterating through " + std::to_string(PollFDs.size()) + " file descriptors.");
          }

          /* Collect handlers to delete AFTER processing to avoid modifying vector during iteration */

          std::vector<EventHandler *> handlers_to_delete;

          /* First pass: process error/hangup events immediately (they need cleanup) */

          for (size_t i = 0; i < PollFDs.size(); ++i)
          {
               struct pollfd &pfd = PollFDs[i];

               /* Skip file descriptors with no events */

               if (pfd.revents == 0)
               {
                    continue;
               }

               /* DEBUG: Log which fd has events - ALWAYS log */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    std::string event_types;

                    if (pfd.revents & POLLIN)
                         event_types += "POLLIN ";
                    if (pfd.revents & POLLOUT)
                         event_types += "POLLOUT ";
                    if (pfd.revents & POLLERR)
                         event_types += "POLLERR ";
                    if (pfd.revents & POLLHUP)
                         event_types += "POLLHUP ";
                    if (pfd.revents & POLLNVAL)
                         event_types += "POLLNVAL ";
                    if (pfd.revents & POLLPRI)
                         event_types += "POLLPRI ";

                    Instance->Logs->Debug("socketengine", "poll() detected event on fd=" + std::to_string(pfd.fd) + ", revents=0x" + std::to_string(pfd.revents) + " (" + (event_types.empty() ? "unknown" : event_types) + ").");
               }

               /* Validate fd is still valid */

               if (pfd.fd < 0)
               {
                    pfd.revents = 0;
                    continue;
               }

               /* Find handler for this file descriptor */

               auto handler_it = FDToHandler.find(pfd.fd);

               if (handler_it == FDToHandler.end() || !handler_it->second)
               {
                    pfd.revents = 0;
                    continue;
               }

               EventHandler *eh = handler_it->second;

               /* Handle errors/hangups first - these need immediate cleanup */
               /* POLLERR, POLLHUP, POLLNVAL are always returned in revents when true */
               /* Also handle POLLRDHUP if available (Linux 2.6.17+) */

               if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL
#ifdef POLLRDHUP
                                  | POLLRDHUP
#endif
                                  ))
               {
                    handlers_to_delete.push_back(eh);

                    try
                    {
                         /* Validate handler is still valid before calling */

                         if (eh->HasFD() && eh->GetFD() == pfd.fd)
                         {
                              eh->OnEventHandlerRead(); /* Let handler cleanup */
                         }
                    }
                    catch (...)
                    {
                         /* Ignore exceptions during cleanup */
                    }

                    pfd.revents = 0; /* Clear before deletion */
               }
          }

          /* Second pass: process read events (most common) */

          for (size_t i = 0; i < PollFDs.size(); ++i)
          {
               struct pollfd &pfd = PollFDs[i];

               /* Skip if no events or already processed */

               if (pfd.revents == 0)
               {
                    continue;
               }

               if (pfd.fd < 0)
               {
                    pfd.revents = 0;
                    continue;
               }

               auto handler_it = FDToHandler.find(pfd.fd);

               if (handler_it == FDToHandler.end())
               {
                    /* Handler not found - clear revents and continue */

                    pfd.revents = 0;
                    continue;
               }

               EventHandler *eh = handler_it->second;

               if (!eh)
               {
                    /* Handler is null - clear revents and continue */

                    pfd.revents = 0;
                    continue;
               }

               /* Process read events */

               if (pfd.revents & POLLIN)
               {
                    /* Validate handler is still valid before calling */

                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         /* Handler fd changed or invalid - skip */

                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("socketengine", "Calling OnEventHandlerRead() for fd=" + std::to_string(pfd.fd) + ".");
                         }

                         eh->OnEventHandlerRead();
                         events_processed++;

                         if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
                         {
                              Instance->Logs->Debug("socketengine", "OnEventHandlerRead() completed for fd=" + std::to_string(pfd.fd) + ", events_processed=" + std::to_string(events_processed) + ".");
                         }
                    }
                    catch (...)
                    {
                         /* On exception, mark handler for deletion */

                         if (Instance && Instance->Logs)
                         {
                              Instance->Logs->Critical("socketengine",
                                                       "Exception in OnEventHandlerRead() for fd=" + std::to_string(pfd.fd) + ".");
                         }

                         handlers_to_delete.push_back(eh);

                         /* Ignore exceptions, continue processing */
                    }

                    /* Clear POLLIN bit after processing, but keep other bits for third pass */

                    pfd.revents &= ~POLLIN;
               }

               /* Process POLLPRI (out-of-band data) - treat as read event */

               if (pfd.revents & POLLPRI)
               {
                    /* Validate handler is still valid before calling */

                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         /* Handler fd changed or invalid - skip */

                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         /* POLLPRI indicates out-of-band data - treat as read */

                         eh->OnEventHandlerRead();
                         events_processed++;
                    }
                    catch (...)
                    {
                         /* On exception, mark handler for deletion */

                         handlers_to_delete.push_back(eh);
                    }

                    /* Clear POLLPRI bit after processing */

                    pfd.revents &= ~POLLPRI;
               }
          }

          /* Third pass: process write events */

          for (size_t i = 0; i < PollFDs.size(); ++i)
          {
               struct pollfd &pfd = PollFDs[i];

               /* Skip if no events */

               if (pfd.revents == 0)
               {
                    continue;
               }

               if (pfd.fd < 0)
               {
                    pfd.revents = 0;
                    continue;
               }

               auto handler_it = FDToHandler.find(pfd.fd);

               if (handler_it == FDToHandler.end() || !handler_it->second)
               {
                    pfd.revents = 0;
                    continue;
               }

               EventHandler *eh = handler_it->second;

               /* Process write events */

               if (pfd.revents & POLLOUT)
               {
                    /* Validate handler is still valid before calling */

                    if (!eh->HasFD() || eh->GetFD() != pfd.fd)
                    {
                         /* Handler fd changed or invalid - skip */

                         pfd.revents = 0;
                         continue;
                    }

                    try
                    {
                         eh->OnEventHandlerWrite();
                         UnregisterPendingWrite(eh); /* Auto-cleanup when writable */

                         events_processed++;
                    }
                    catch (...)
                    {
                         /* On exception, mark handler for deletion */

                         handlers_to_delete.push_back(eh);

                         /* Ignore exceptions, continue processing */
                    }

                    /* Clear POLLOUT bit after processing */

                    pfd.revents &= ~POLLOUT;
               }

               /* Clear any remaining revents for next poll() call - must clear after processing */
               /* Only clear if we've processed all expected events */
               /* Remaining bits should only be error flags that were already handled in first pass */

               pfd.revents = 0;
          }

          /* Now delete handlers that had errors/hangups (after iteration to avoid vector modification issues) */
          /* Remove duplicates before deletion to avoid double-deletion */

          std::unordered_set<EventHandler *> unique_handlers_to_delete(handlers_to_delete.begin(), handlers_to_delete.end());

          for (EventHandler *eh : unique_handlers_to_delete)
          {
               if (eh)
               {
                    DelFD(eh);
                    UnregisterPendingWrite(eh);
               }
          }

          /* HLQuery-style: If we processed many events, check for more immediately */
          /* Use non-blocking mode (timeout=0) for recursive call to match epoll behavior */

          if (events_processed >= MAX_EVENTS / 2)
          {
               /* Temporarily set timeout to 0 for immediate processing */

               int saved_timeout = CurrentTimeoutMS.load();

               CurrentTimeoutMS.store(0, std::memory_order_relaxed);

               int more_events = DispatchEvents();

               /* Restore timeout */

               CurrentTimeoutMS.store(saved_timeout, std::memory_order_relaxed);

               return events_processed + more_events;
          }

          /* DEBUG: Log if poll() returned events but we didn't process any */

          if (nfds > 0 && events_processed == 0 && Instance && Instance->Logs && Instance->Logs->GetDebugMode())
          {
               int fds_with_events = 0;

               for (size_t i = 0; i < PollFDs.size(); ++i)
               {
                    if (PollFDs[i].revents != 0)
                    {
                         fds_with_events++;
                    }
               }

               Instance->Logs->Debug("socketengine", "poll() returned " + std::to_string(nfds) + " events but processed 0 (fds_with_events=" + std::to_string(fds_with_events) + ", PollFDs.size()=" + std::to_string(PollFDs.size()) + ").");
          }
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
          return; /* Fast path: no pending writes */
     }

     /* OPTIMIZATION: Swap instead of copy for zero-copy */
     /* Release lock BEFORE processing writes to avoid deadlock */
     /* Writes may call RegisterPendingWrite/UnregisterPendingWrite which need the lock */

     std::vector<EventHandler *> WriteCandidates;

     {
          std::lock_guard<std::mutex> lock(PendingWritesMutex);

          if (SocketEngine::PendingWrites.empty())
          {
               SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
               return;
          }

          WriteCandidates.swap(SocketEngine::PendingWrites); /* O(1) swap, no copying */

          PendingWritesSet.clear();
          SocketEngine::PendingWritesCount.store(0, std::memory_order_relaxed);
     }

     /* Process writes immediately for publish messages */
     /* Lock is released - writes can safely call RegisterPendingWrite/UnregisterPendingWrite */

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
                    continue; /* Skip invalid handlers */
               }

               /* Simple approach: just try the write */

               EH->OnEventHandlerWrite();
               ProcessedCount++;

               /* No yielding for immediate publish message delivery */
          }

          /* REMOVED: Inter-batch yielding - no throttling, maximum throughput */
     }
}

/* Registers a pending write */

void SocketEngine::RegisterPendingWrite(EventHandler *EH)
{
     if (!EH || !EH->HasFD())
     {
          return; /* Invalid handler */
     }

     /* Thread-safe access to PendingWrites */

     std::lock_guard<std::mutex> lock(PendingWritesMutex);

     /* OPTIMIZATION: O(1) duplicate check using unordered_set */

     if (PendingWritesSet.find(EH) != PendingWritesSet.end())
     {
          return; /* Already registered */
     }

     /* Add to both set and vector */

     PendingWritesSet.insert(EH);
     SocketEngine::PendingWrites.push_back(EH);
     SocketEngine::PendingWritesCount.fetch_add(1, std::memory_order_relaxed);

     /* Enable POLLOUT for this fd in PollFDs array */
     /* Use HandlerToIndex for O(1) lookup instead of linear search */

     int fd = EH->GetFD();
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
          return; /* Not registered */
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

     /* Disable POLLOUT for this fd in PollFDs array */
     /* Use HandlerToIndex for O(1) lookup instead of linear search */

     if (EH->HasFD())
     {
          int fd = EH->GetFD();
          auto index_it = HandlerToIndex.find(EH);

          if (index_it != HandlerToIndex.end())
          {
               size_t index = index_it->second;

               if (index < PollFDs.size() && PollFDs[index].fd == fd)
               {
                    PollFDs[index].events &= ~POLLOUT;
                    return;
               }
          }

          /* Fallback: linear search if index map is stale */

          for (auto &pfd : PollFDs)
          {
               if (pfd.fd == fd)
               {
                    /* Remove POLLOUT flag */

                    pfd.events &= ~POLLOUT;
                    break;
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
     /* Event counting removed - use connection count instead */

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
