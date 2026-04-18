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
#include <limits>

#include "common/actionlist.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"

/* RAII guard for processing flag to ensure it's always reset */

class ProcessingGuard
{
   private:

     /* Shared processing flag protected by this guard. */

     std::atomic<bool> &Flag;

     /* Tracks whether this instance acquired the processing flag. */

     bool ShouldReset;

   public:

     /* Attempt to acquire the processing flag for the current scope. */

     explicit ProcessingGuard(std::atomic<bool> &flag)
          : Flag(flag)
     {
          bool expected = false;

          if (!Flag.compare_exchange_strong(expected, true))
          {
               /* Already processing, don't set flag */

               ShouldReset = false;
          }
          else
          {
               ShouldReset = true;
          }
     }

     /* Release the processing flag when this guard owns it. */

     ~ProcessingGuard()
     {
          if (ShouldReset)
          {
               Flag.store(false);
          }
     }

     /* Non-copyable, non-movable */

     ProcessingGuard(const ProcessingGuard &) = delete;
     ProcessingGuard &operator=(const ProcessingGuard &) = delete;
     ProcessingGuard(ProcessingGuard &&) = delete;
     ProcessingGuard &operator=(ProcessingGuard &&) = delete;

     /* Return whether this guard successfully entered processing mode. */

     bool IsActive() const
     {
          return ShouldReset;
     }
};

/* Static member definitions - wrap in struct to control destruction order */

struct ActionListImpl
{
     /* Actions currently detached for processing. */

     std::vector<ActionList::Action> Actions;

     /* Actions queued for the next processing pass. */

     std::vector<ActionList::Action> PendingActions;

     /* Synchronizes access to the action queues. */

     std::mutex ActionsMutex;

     /* Prevents concurrent action processing. */

     std::atomic<bool> Processing{false};

     /* Total number of accepted actions. */

     std::atomic<size_t> TotalQueued{0};

     /* Total number of successfully executed actions. */

     std::atomic<size_t> TotalProcessed{0};

     /* Total number of failed actions. */

     std::atomic<size_t> TotalFailed{0};

     /* Total number of dropped actions. */

     std::atomic<size_t> TotalDropped{0};

     /* Number of actions in the current processing batch. */

     std::atomic<size_t> CurrentProcessingCount{0};

     /* Initialize queue storage to reduce early reallocations. */

     ActionListImpl()
     {
          /* Reserve initial capacity to reduce reallocations */

          Actions.reserve(INITIAL_CAPACITY);
          PendingActions.reserve(INITIAL_CAPACITY);
     }

     /* Clear stored actions during shutdown without propagating exceptions. */

     ~ActionListImpl()
     {
          try
          {
               /* Clear any remaining actions during destruction */

               try
               {
                    std::lock_guard<std::mutex> lock(ActionsMutex);

                    Actions.clear();
                    PendingActions.clear();
               }
               catch (...)
               {
               }
          }
          catch (...)
          {
          }
     }
};

static ActionListImpl action_impl;

/* Access static members through the implementation */

std::vector<ActionList::Action> &ActionList::Actions = action_impl.Actions;

std::vector<ActionList::Action> &ActionList::PendingActions = action_impl.PendingActions;

std::mutex &ActionList::ActionsMutex = action_impl.ActionsMutex;

std::atomic<bool> &ActionList::Processing = action_impl.Processing;

/* Statistics accessors */

std::atomic<size_t> &ActionList::TotalQueued = action_impl.TotalQueued;

std::atomic<size_t> &ActionList::TotalProcessed = action_impl.TotalProcessed;

std::atomic<size_t> &ActionList::TotalFailed = action_impl.TotalFailed;

std::atomic<size_t> &ActionList::TotalDropped = action_impl.TotalDropped;

std::atomic<size_t> &ActionList::CurrentProcessingCount = action_impl.CurrentProcessingCount;

/* Queue a new action for execution during the next processing cycle. */

void ActionList::QueueAction(Action action)
{
     if (!action)
     {
          return;
     }

     std::lock_guard<std::mutex> lock(ActionsMutex);

     /* Check capacity limit to prevent unbounded growth */

     if (PendingActions.size() >= MAX_QUEUE_SIZE)
     {
          TotalDropped.fetch_add(1, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("action_list", "Action queue full (" + std::to_string(MAX_QUEUE_SIZE) + "), dropping action.");
          }

          return;
     }

     /* Reserve capacity if needed to reduce reallocations */

     if (PendingActions.size() >= PendingActions.capacity())
     {
          PendingActions.reserve(std::min(PendingActions.capacity() * 2, static_cast<size_t>(MAX_QUEUE_SIZE)));
     }

     PendingActions.push_back(std::move(action));

     TotalQueued.fetch_add(1, std::memory_order_relaxed);

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Debug("action_list", "Action queued, total pending: " + std::to_string(PendingActions.size()) + ".");
     }
}

/* Execute the currently queued actions while preventing concurrent processing. */

void ActionList::ProcessActions()
{
     ProcessingGuard guard(Processing);

     if (!guard.IsActive())
     {
          /* Already processing in another thread, skip */

          return;
     }

     try
     {
          /* Swap pending actions with current actions atomically */

          size_t ActionCount = 0;
          std::vector<ActionList::Action> Batch;

          {
               std::lock_guard<std::mutex> lock(ActionsMutex);

               ActionCount = PendingActions.size();

               if (ActionCount == 0)
               {
                    return; /* Nothing to process */
               }

               /* Move the batch out under the lock so execution never races queue inspection. */

               Batch.swap(PendingActions);
               Actions.clear();

               /* Reserve capacity for next batch if needed */

               if (PendingActions.capacity() < INITIAL_CAPACITY)
               {
                    PendingActions.reserve(INITIAL_CAPACITY);
               }
          }

          CurrentProcessingCount.store(ActionCount, std::memory_order_relaxed);

          /* Process all actions outside the lock to minimize contention */

          size_t Processed = 0;

          size_t Failed = 0;

          for (auto &action : Batch)
          {
               try
               {
                    if (action)
                    {
                         action();
                         Processed++;
                    }
               }
               catch (const std::exception &e)
               {
                    Failed++;

                    TotalFailed.fetch_add(1, std::memory_order_relaxed);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("action_list", "Exception in queued action: " + std::string(e.what()) + ".");
                    }
               }
               catch (...)
               {
                    Failed++;

                    TotalFailed.fetch_add(1, std::memory_order_relaxed);

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("action_list", "Unknown exception in queued action.");
                    }
               }
          }

          /* Update statistics */

          TotalProcessed.fetch_add(Processed, std::memory_order_relaxed);
          CurrentProcessingCount.store(0, std::memory_order_relaxed);

          /* Clear processed actions efficiently */

          Batch.clear();

          /* Shrink vector if it grew too large (keep some capacity for efficiency) */

          if (Batch.capacity() > INITIAL_CAPACITY * 4)
          {
               std::vector<ActionList::Action>().swap(Batch);
          }

          if (Instance && Instance->Logs && Processed > 0)
          {
               Instance->Logs->Debug("action_list", "Processed " + std::to_string(Processed) + " actions" + (Failed > 0 ? " (" + std::to_string(Failed) + " failed)" : "") + ".");
          }
     }
     catch (const std::exception &e)
     {
          CurrentProcessingCount.store(0, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("action_list", "Exception in ProcessActions: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          CurrentProcessingCount.store(0, std::memory_order_relaxed);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("action_list", "Unknown exception in ProcessActions.");
          }
     }
}

/* Return the number of pending actions plus the actions currently being executed. */

size_t ActionList::GetActionCount()
{
     std::lock_guard<std::mutex> lock(ActionsMutex);

     return PendingActions.size() + CurrentProcessingCount.load(std::memory_order_relaxed);
}

/* Remove all queued actions and restore the default queue capacity. */

void ActionList::ClearActions()
{
     try
     {
          std::lock_guard<std::mutex> lock(ActionsMutex);

          size_t Cleared = Actions.size() + PendingActions.size();

          Actions.clear();
          PendingActions.clear();

          /* Reset capacity to initial size */

          Actions.shrink_to_fit();
          PendingActions.shrink_to_fit();

          Actions.reserve(INITIAL_CAPACITY);
          PendingActions.reserve(INITIAL_CAPACITY);

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Debug("action_list", "All actions cleared (" + std::to_string(Cleared) + " actions).");
          }
     }
     catch (...)
     {
          ConsoleWriter::WriteError("Exception caught in ActionList::ClearActions - ignoring to prevent std::terminate()", true);
     }
}

/* Return a snapshot of the current action queue statistics. */

ActionList::Statistics ActionList::GetStatistics()
{
     Statistics Stats;

     Stats.TotalQueued = TotalQueued.load(std::memory_order_relaxed);
     Stats.TotalProcessed = TotalProcessed.load(std::memory_order_relaxed);
     Stats.TotalFailed = TotalFailed.load(std::memory_order_relaxed);
     Stats.TotalDropped = TotalDropped.load(std::memory_order_relaxed);

     {
          std::lock_guard<std::mutex> lock(ActionsMutex);

          Stats.CurrentPending = PendingActions.size();
          Stats.CurrentProcessing = CurrentProcessingCount.load(std::memory_order_relaxed);
     }

     return Stats;
}

/* Reset all accumulated action processing counters. */

void ActionList::ResetStatistics()
{
     TotalQueued.store(0, std::memory_order_relaxed);
     TotalProcessed.store(0, std::memory_order_relaxed);
     TotalFailed.store(0, std::memory_order_relaxed);
     TotalDropped.store(0, std::memory_order_relaxed);
}
