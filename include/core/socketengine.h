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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __linux__

#include <sys/epoll.h>

#else

/* Compatibility epoll-style event definitions for non-Linux platforms. */

struct epoll_event
{
     uint32_t events;
     union
     {
          int fd;
          void *ptr;
          uint64_t u64;
     } data;
};

static constexpr int EPOLLIN = 0x001;
static constexpr int EPOLLPRI = 0x002;
static constexpr int EPOLLOUT = 0x004;
static constexpr int EPOLLERR = 0x008;
static constexpr int EPOLLHUP = 0x010;
static constexpr int EPOLLRDHUP = 0x2000;
static constexpr int EPOLLET = 0x80000000;
static constexpr int EPOLL_CLOEXEC = 0x80000;

#endif

#include <unistd.h>
#include <vector>

/* hlquery event handler base class */

class EventHandler
{
   public:
     /* Destructor */

     virtual ~EventHandler()
     {
     }

     /* Called when there is a read event */

     virtual void OnEventHandlerRead() = 0;

     /* Called when there is a write event */

     virtual void OnEventHandlerWrite()
     {
     }

     /* Called when there is an error event */

     virtual void OnEventHandlerError(int ErrorNum)
     {
          (void)ErrorNum;
     }

     /* Returns the file descriptor */

     int GetFD() const
     {
          return FD.load(std::memory_order_acquire);
     }

     /* Sets the file descriptor */

     void SetFD(int fd)
     {
          FD.store(fd, std::memory_order_release);
     }

     /* Returns true if the file descriptor is valid */

     bool HasFD() const
     {
          return GetFD() >= 0;
     }

     /* Event mask management */

     /* Returns the event mask */

     int GetEventMask() const
     {
          return EventMask.load(std::memory_order_acquire);
     }

     /* Sets the event mask */

     void SetEventMask(int mask)
     {
          EventMask.store(mask, std::memory_order_release);
     }

   private:
     /* File descriptor */

     std::atomic<int> FD{-1};

     /* Event mask */

     std::atomic<int> EventMask{0};
};

/* hlquery socket engine - epoll-based event dispatcher */

class SocketEngine
{
   public:
     /* Increased from 1024 to handle high-throughput scenarios */

     static const int MAX_EVENTS = 16384;

     /* Main interface */

     /* Initializes the socket engine */

     static void Init();

     /* Deinitializes the socket engine */

     static void Deinit();

     /* Resets the engine after a fork */

     static void ResetAfterFork();

     /* Adds a file descriptor to the engine */

     static bool AddFD(EventHandler *EH, int Events = EPOLLIN);

     /* Deletes a file descriptor from the engine */

     static void DelFD(EventHandler *EH);

     /* Dispatches pending events */

     static int DispatchEvents();

     /* Pending write management (no eventfd/wake needed) */

     /* Registers a pending write */

     static void RegisterPendingWrite(EventHandler *EH);

     /* Unregisters a pending write */

     static void UnregisterPendingWrite(EventHandler *EH);

     /* Dispatches trial writes */

     static void DispatchTrialWrites();

     /* Performance monitoring for adaptive optimization */

     /* Returns the number of events processed */

     static int GetEventCount();

     /* Advanced I/O functions */

     /* Initializes advanced I/O optimizations */

     static void InitializeAdvancedIO();

     /* Initializes zero-copy buffers */

     static void InitializeZeroCopyBuffers();

     /* Cleans up zero-copy buffers */

     static void CleanupZeroCopyBuffers();

     /* Starts optional I/O worker threads */

     static void StartIOWorkerThreads();

     /* I/O worker thread entry point */

     static void IOWorkerThread(unsigned int WorkerID);

     /* Initializes adaptive timeout */

     static void InitializeAdaptiveTimeout();

     /* Adapts the engine timeout based on load */

     static void AdaptTimeout();

     /* Sets optimal socket options */

     static void SetOptimalSocketOptions();

     /* Returns a zero-copy buffer */

     static void *GetZeroCopyBuffer();

     /* Returns a zero-copy buffer to the pool */

     static void ReturnZeroCopyBuffer(void *buffer);

     /* Enables or disables adaptive timeout */

     static void EnableAdaptiveTimeout(bool Enable);

     /* Resets I/O statistics */

     static void ResetIOStats();

     /* I/O statistics structure */

     struct IOStats
     {
          uint64_t TotalBytesProcessed;
          uint64_t ActiveConnections;
     };

     /* Returns I/O statistics */

     static IOStats GetIOStats();

     /* Increments total bytes processed */

     static void IncrementBytesProcessed(uint64_t Bytes);

     /* Increments active connection count */

     static void IncrementConnectionCount();

     /* Decrements active connection count */

     static void DecrementConnectionCount();

     /* Pending message tracking for immediate delivery */

     /* Increments pending message count */

     static void IncrementPendingMessages();

     /* Decrements pending message count */

     static void DecrementPendingMessages();

     /* Returns the number of pending messages */

     static int GetPendingMessageCount();

     /* Check if there's pending work (actions, writes, etc.) that requires immediate processing */

     /* Returns true if there is pending work */

     static bool HasPendingWork();

     /*
      * Get engine file descriptor for restart safety.
      */

     static int GetEpollFD()
     {
          return EpollFD;
     }

   private:
     /* Static members for engine state. */

     /* Engine file descriptor */

     static int EpollFD;

     /* Validity flag for EpollFD */

     static std::atomic<bool> EpollFDValid;

     /* Events */

     static std::vector<epoll_event> Events;

     /* Pending writes queue */

     static std::vector<EventHandler *> PendingWrites;

     /* Thread-safe count for HasPendingWork() */

     static std::atomic<size_t> PendingWritesCount;

     /* Pending message count */

     static std::atomic<int> PendingMessageCount;
};
