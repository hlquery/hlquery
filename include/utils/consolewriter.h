/*
 * hlquery - Search beyond keywords.
 * Advanced Console Writer Header
 *
 * Provides sophisticated startup output with color coding, status indicators,
 * and professional formatting for production deployments.
 */

#pragma once

#include <string>

#include "core/config.h"

/* Comment goes here */

class CoreExport ConsoleWriter
{
   public:
     /* ANSI color codes */

     static const std::string ColorGreen;
     static const std::string ColorRed;
     static const std::string ColorYellow;
     static const std::string ColorBlue;
     static const std::string ColorCyan;
     static const std::string ColorBlack;
     static const std::string ColorWhite;
     static const std::string ColorBold;
     static const std::string ColorReset;

     /* Bright variants for enhanced visibility */

     static const std::string ColorBrightGreen;
     static const std::string ColorBrightRed;
     static const std::string ColorBrightYellow;
     static const std::string ColorBrightBlue;
     static const std::string ColorBrightCyan;
     static const std::string ColorBrightMagenta;

     /* Terminal support check */

     static bool SupportsColor();

     /* Message formatting */

     static std::string FormatMessage(const std::string &Message, const std::string &Status, bool Dot = true);

     /* Startup and status messages */

     static void WriteStartup(const std::string &Message, bool Success, bool Dot = true);
     static void WriteStartupPlain(const std::string &Message, bool Dot = true);

     static void WriteConfig(const std::string &Message, bool Success, bool Dot = true);

     static void WriteInfo(const std::string &Message, bool Dot = true);

     static void WriteWarning(const std::string &Message, bool Dot = true);

     static void WriteError(const std::string &Message, bool Dot = true);

     static void WriteDebug(const std::string &Message, bool Dot = true);

     static void WriteExit(const std::string &Message, bool Dot = true);

     /* Initialization messages (for daemon startup) */

     static void WriteInit(const std::string &Message, bool Dot = true);

     static void WriteInitCritical(const std::string &Message, bool Dot = true);

     /* Async-signal-safe version for post-fork logging (takes const char* for safety) */

     static void WriteInitSafe(int Fd, const char *Message, bool Dot = true);

     static void WriteInitCriticalSafe(int Fd, const char *Message, bool Dot = true);

     static void WriteStartupSafe(int Fd, const char *Message, bool Dot = true);
     static void WriteStartupPlainSafe(int Fd, const char *Message, bool Dot = true);

     /* Header and section display */

     static void WriteBlankLine();

     static void WriteHeader(const std::string &Title);

     static void WriteServerInfo(const std::string &Key, const std::string &Value, const std::string &Status);

     /* Helper to ensure periods in messages */

     static std::string EnsurePeriod(const std::string &Message, bool Dot = true);
};
