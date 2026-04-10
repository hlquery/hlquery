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

#pragma once

#include <string>

#include "fmt/format.h"
#include "utils/consolewriter.h"

/* Constants */

#ifndef LOG_DEFAULT
#define LOG_DEFAULT 0
#endif

/* Simplified Output Macros - Clean and Readable (fmt-enabled) */

/* Short status output macros */

#define print_ok(fmtstr, ...)                                    \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteStartup(                           \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               true);                                            \
     } while (0)

#define print_ok_nd(fmtstr, ...)                                 \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteStartup(                           \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               true, false);                                     \
     } while (0)

#define print_failed(fmtstr, ...)                                \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteStartup(                           \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false);                                           \
     } while (0)

#define print_failed_nd(fmtstr, ...)                             \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteStartup(                           \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false, false);                                    \
     } while (0)

#define print_info(fmtstr, ...)                                   \
     do                                                           \
     {                                                            \
          ConsoleWriter::WriteInfo(                               \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__)); \
     } while (0)

#define print_info_nd(fmtstr, ...)                               \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteInfo(                              \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false);                                           \
     } while (0)

#define print_warning(fmtstr, ...)                                \
     do                                                           \
     {                                                            \
          ConsoleWriter::WriteWarning(                            \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__)); \
     } while (0)

#define print_warning_nd(fmtstr, ...)                            \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteWarning(                           \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false);                                           \
     } while (0)

#define print_error(fmtstr, ...)                                  \
     do                                                           \
     {                                                            \
          ConsoleWriter::WriteError(                              \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__)); \
     } while (0)

#define print_error_nd(fmtstr, ...)                              \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteError(                             \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false);                                           \
     } while (0)

#define print_exit(fmtstr, ...)                                   \
     do                                                           \
     {                                                            \
          ConsoleWriter::WriteExit(                               \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__)); \
     } while (0)

#define print_exit_nd(fmtstr, ...)                               \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteExit(                              \
               fmt::format(fmt::runtime(fmtstr), ##__VA_ARGS__), \
               false);                                           \
     } while (0)

/* Header and info display */

#define print_h(title)                                    \
     do                                                   \
     {                                                    \
          ConsoleWriter::WriteBlankLine();                \
          ConsoleWriter::WriteHeader(std::string(title)); \
     } while (0)

#define print_s(key, value)                                 \
     do                                                     \
     {                                                      \
          ConsoleWriter::WriteServerInfo(                   \
               std::string(key), std::string(value), "OK"); \
     } while (0)

#define print_nl()                         \
     do                                    \
     {                                     \
          ConsoleWriter::WriteBlankLine(); \
     } while (0)

/* Legacy macros for backward compatibility - DEPRECATED */

#define SERVER_INFO_HEADER(title) print_h(title)
#define SERVER_INFO_OK(key, value) print_s(key, value)
#define SERVER_INFO_INFO(key, value) print_s(key, value)
#define SERVER_INFO_WARNING(key, value)                          \
     do                                                          \
     {                                                           \
          ConsoleWriter::WriteServerInfo(                        \
               std::string(key), std::string(value), "WARNING"); \
     } while (0)
#define SERVER_INFO_BLANK() print_nl()
#define DISPLAY_STATUS_OK(message) print_ok(message)
#define DISPLAY_STATUS_FAILED(message) print_failed(message)
#define DISPLAY_INFO(message) print_info(message)
#define DISPLAY_WARNING(message) print_warning(message)
#define DISPLAY_ERROR(message) print_error(message)
