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

#include <chrono>

class hlquery;
class UserAuthManager;
class HttpServer;
class IPFilter;
class LogManager;
class ServerConfig;
class SearchAPI;
class DBManager;
class InvertedIndex;
class SAM;
struct SearchHit;
