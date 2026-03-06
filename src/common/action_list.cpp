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
#include <limits>

#include "common/action_list.h"
#include "core/config.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"

/* RAII guard for processing flag to ensure it's always reset */

class ProcessingGuard
{
   public:

     explicit ProcessingGuard(std::atomic<bool>& flag) : Flag(flag)
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

     ~ProcessingGuard()
     {
          if (ShouldReset)
          {
               Flag.store(false);
          }
     }

     /* Non-copyable, non-movable */

     ProcessingGuard(const ProcessingGuard&) = delete;
     ProcessingGuard& operator=(const ProcessingGuard&) = delete;
     ProcessingGuard(ProcessingGuard&&) = delete;
     ProcessingGuard& operator=(ProcessingGuard&&) = delete;

     bool IsActive() const
     {
          return ShouldReset;
     }

   private:

     std::atomic<bool>& Flag;

     bool ShouldReset;
};

/* Static member definitions - wrap in struct to control destruction order */

struct ActionListImpl
{
     std::vector<ActionList::Action> Actions;

     std::vector<ActionList::Action> PendingActions;

     std::mutex ActionsMutex;

     std::atomic<bool> Processing{false};

     std::atomic<size_t> TotalQueued{0};

     std::atomic<size_t> TotalProcessed{0};

     std::atomic<size_t> TotalFailed{0};

     std::atomic<size_t> TotalDropped{0};

     ActionListImpl()
     {
          /* Reserve initial capacity to reduce reallocations */

          Actions.reserve(INITIAL_CAPACITY);
          PendingActions.reserve(INITIAL_CAPACITY);
     }

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

std::vector<ActionList::Action>& ActionList::Actions = action_impl.Actions;

std::vector<ActionList::Action>& ActionList::PendingActions = action_impl.PendingActions;

std::mutex& ActionList::ActionsMutex = action_impl.ActionsMutex;

std::atomic<bool>& ActionList::Processing = action_impl.Processing;

/* Statistics accessors */

std::atomic<size_t>& ActionList::TotalQueued = action_impl.TotalQueued;

std::atomic<size_t>& ActionList::TotalProcessed = action_impl.TotalProcessed;

std::atomic<size_t>& ActionList::TotalFailed = action_impl.TotalFailed;

std::atomic<size_t>& ActionList::TotalDropped = action_impl.TotalDropped;

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

          {
               std::lock_guard<std::mutex> lock(ActionsMutex);

               ActionCount = PendingActions.size();

               if (ActionCount == 0)
               {
                    return; /* Nothing to process */
               }

               /* Swap efficiently */

               Actions.swap(PendingActions);
               PendingActions.clear();

               /* Reserve capacity for next batch if needed */

               if (PendingActions.capacity() < INITIAL_CAPACITY)
               {
                    PendingActions.reserve(INITIAL_CAPACITY);
               }
          }

          /* Process all actions outside the lock to minimize contention */

          size_t Processed = 0;

          size_t Failed = 0;

          for (auto& action : Actions)
          {
               try
               {
                    if (action)
                    {
                         action();
                         Processed++;
                    }
               }
               catch (const std::exception& e)
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

          /* Clear processed actions efficiently */

          Actions.clear();

          /* Shrink vector if it grew too large (keep some capacity for efficiency) */

          if (Actions.capacity() > INITIAL_CAPACITY * 4)
          {
               Actions.shrink_to_fit();
               Actions.reserve(INITIAL_CAPACITY);
          }

          if (Instance && Instance->Logs && Processed > 0)
          {
               Instance->Logs->Debug("action_list", "Processed " + std::to_string(Processed) + " actions" + (Failed > 0 ? " (" + std::to_string(Failed) + " failed)" : "") + ".");
          }
     }
     catch (const std::exception& e)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("action_list", "Exception in ProcessActions: " + std::string(e.what()) + ".");
          }
     }
     catch (...)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("action_list", "Unknown exception in ProcessActions.");
          }
     }
}

size_t ActionList::GetActionCount()
{
     std::lock_guard<std::mutex> lock(ActionsMutex);

     return PendingActions.size();
}

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
          Stats.CurrentProcessing = Actions.size();
     }

     return Stats;
}

void ActionList::ResetStatistics()
{
     TotalQueued.store(0, std::memory_order_relaxed);
     TotalProcessed.store(0, std::memory_order_relaxed);
     TotalFailed.store(0, std::memory_order_relaxed);
     TotalDropped.store(0, std::memory_order_relaxed);
}
