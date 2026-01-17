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


#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "core/exitmanager.h"

namespace 
{
    std::vector<void (*)()> cleanup_funcs;
    std::mutex cleanup_mutex;
    std::atomic<bool> shutting_down{false};
}

void ExitManager::RegisterCleanup(void (*func)()) 
{
    std::lock_guard<std::mutex> lock(cleanup_mutex);
    cleanup_funcs.push_back(func);
}

void ExitManager::RunCleanups() 
{
    std::lock_guard<std::mutex> lock(cleanup_mutex);

    for (auto &func : cleanup_funcs) 
    {
        func();
    }
    
    cleanup_funcs.clear();
}

bool ExitManager::IsShuttingDown() 
{
    return shutting_down.load();
}

void ExitManager::Exit(int status) 
{
    shutting_down.store(true);
    RunCleanups();
    std::exit(status);
}

void ExitManager::QuickExit(int status) 
{
    shutting_down.store(true);
    RunCleanups();
    std::quick_exit(status);
}

void ExitManager::EmergencyExit(int status) 
{
    std::_Exit(status);
}
