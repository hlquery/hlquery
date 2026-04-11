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

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/memorypool.h"

/* RAII wrapper for automatic memory management */

template <typename T>
class PoolAllocator
{
   public:

     using value_type = T;

     PoolAllocator() = default;

     template <typename U>
     PoolAllocator(const PoolAllocator<U> &)
     {
     }

     T *allocate(size_t n)
     {
          return static_cast<T *>(std::malloc(n * sizeof(T)));
     }

     void deallocate(T *ptr, size_t)
     {
          if (ptr)
          {
               std::free(ptr);
          }
     }

     bool operator==(const PoolAllocator &) const
     {
          return true;
     }

     bool operator!=(const PoolAllocator &) const
     {
          return false;
     }
};

/* High-performance string with pool allocation */

using FastString = std::basic_string<char, std::char_traits<char>, PoolAllocator<char>>;

/* High-performance vector with pool allocation */

template <typename T>
using FastVector = std::vector<T, PoolAllocator<T>>;

/* High-performance unordered_map with pool allocation */

template <typename K, typename V>
using FastHashMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, PoolAllocator<std::pair<const K, V>>>;

