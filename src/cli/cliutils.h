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

#include <iostream>
#include <string>

namespace hlquery_cli
{
namespace Colors
{
/* Reset color */

const std::string RESET = "\033[0m";

/* Red color */

const std::string RED = "\033[31m";

/* Green color */

const std::string GREEN = "\033[32m";

/* Yellow color */

const std::string YELLOW = "\033[33m";

/* Blue color */

const std::string BLUE = "\033[34m";

/* Cyan color */

const std::string CYAN = "\033[36m";
}

/* Prints a success message. */

void PrintSuccess(const std::string &message);

/* Prints an error message. */

void PrintError(const std::string &message);

/* Prints an info message. */

void PrintInfo(const std::string &message);

/* Prints a warning message. */

void PrintWarning(const std::string &message);

/* Escapes a JSON string. */

std::string EscapeJSONString(const std::string &str);

/* Encodes a URL string. */

std::string UrlEncode(const std::string &str);
}
