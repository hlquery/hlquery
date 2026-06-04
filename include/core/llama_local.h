/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#pragma once

#include <string>

struct LocalLlamaInferenceResult
{
     bool Started = false;
     bool TimedOut = false;
     int ExitCode = -1;
     std::string Output;
     std::string Error;
};

LocalLlamaInferenceResult RunLocalLlamaInference(const std::string& ModelPath,
                                                 const std::string& Mode,
                                                 const std::string& Payload,
                                                 int TimeoutMS);
