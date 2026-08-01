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

#include <string>

/* Configure and load file-backed --fake benchmark fixtures. */

void SetBenchmarkFixtureExecutable(const std::string &path);

enum class BenchmarkFixtureLoadResult
{
     NotFound,
     Loaded,
     Failed
};

BenchmarkFixtureLoadResult LoadBenchmarkFixtures(const std::string &base_url,
                                                  const std::string &auth_token,
                                                  bool reuse_collections,
                                                  bool verbose);
