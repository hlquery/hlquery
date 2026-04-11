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

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* ConsistencyChecker validates and repairs database consistency. */

class ConsistencyChecker
{
   private:

     /* CheckKeyConsistency validates key-level consistency. */

     bool CheckKeyConsistency(int DbID, const std::string& Key);

     /* CheckMemoryConsistency validates in-memory structures. */

     bool CheckMemoryConsistency(int DbID);

     /* Mutex guards consistency operations. */

     mutable std::mutex Mutex;

   public:

     /* Constructor. */

     ConsistencyChecker();

     /* Destructor. */

     ~ConsistencyChecker();

     /* CheckConsistency validates the database. */

     bool CheckConsistency(int DbID);

     /* RepairInconsistencies repairs detected issues. */

     int RepairInconsistencies(int DbID);

     /* GetConsistencyReport returns a detailed report. */

     std::unordered_map<std::string, std::vector<std::string>> GetConsistencyReport(int DbID);
};
