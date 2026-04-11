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

#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

#include "utils/consolewriter.h"

/* ANSI color codes. */

const std::string ConsoleWriter::ColorGreen = "\033[32m";

const std::string ConsoleWriter::ColorRed = "\033[31m";

const std::string ConsoleWriter::ColorYellow = "\033[33m";

const std::string ConsoleWriter::ColorBlue = "\033[34m";

const std::string ConsoleWriter::ColorCyan = "\033[36m";

const std::string ConsoleWriter::ColorBlack = "\033[30m";

const std::string ConsoleWriter::ColorWhite = "\033[37m";

const std::string ConsoleWriter::ColorBold = "\033[1m";

const std::string ConsoleWriter::ColorReset = "\033[0m";

/* Bright variants for improved visibility. */

const std::string ConsoleWriter::ColorBrightGreen = "\033[92m";

const std::string ConsoleWriter::ColorBrightRed = "\033[91m";

const std::string ConsoleWriter::ColorBrightYellow = "\033[93m";

const std::string ConsoleWriter::ColorBrightBlue = "\033[94m";

const std::string ConsoleWriter::ColorBrightCyan = "\033[96m";

const std::string ConsoleWriter::ColorBrightMagenta = "\033[95m";

/*
 * Report whether colored output is supported.
 */

bool ConsoleWriter::SupportsColor()
{
     /* Always enable colors for consistent operator output. */

     return true;
}

/*
 * Format a status message with optional color decoration.
 */

std::string ConsoleWriter::FormatMessage(const std::string &Message, const std::string &Status, bool Dot)
{
     std::ostringstream Oss;
     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          /* Build colored status with black brackets and colored text. */

          if (Status == "OK")
          {
               Oss << ColorBlack << "[" << ColorReset << " " << ColorBrightGreen << "OK" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final;
          }
          else if (Status == "INFO")
          {
               Oss << ColorBlack << "[" << ColorReset << " " << ColorBrightBlue << "INFO" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final;
          }
          else if (Status == "WARN")
          {
               Oss << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "WARNING" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final;
          }
          else
          {
               Oss << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "ERROR" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final;
          }
     }
     else
     {
          Oss << "[" << Status << "] " << Final;
     }

     return Oss.str();
}

/*
 * Write startup status with OK or FAIL markers.
 */

void ConsoleWriter::WriteStartup(const std::string &Message, bool Success, bool Dot)
{
     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          if (Success)
          {
               std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightGreen << "OK" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
          }
          else
          {
               std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "FAIL" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
          }
     }
     else
     {
          std::cout << "[ OK ] " << Final << std::endl;
     }
}

/*
 * Write a plain startup message with no status tag.
 */

void ConsoleWriter::WriteStartupPlain(const std::string &Message, bool Dot)
{
     std::string Final = EnsurePeriod(Message, Dot);

     std::cout << Final << std::endl;
}

/*
 * Write configuration load status.
 */

void ConsoleWriter::WriteConfig(const std::string &Message, bool Success, bool Dot)
{
     /* Print CONFIG messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          if (Success)
          {
               std::cout << ColorBrightBlue << "[LOAD]" << ColorReset << " " << Final << std::endl;
          }
          else
          {
               std::cout << ColorBrightRed << "[FAIL]" << ColorReset << " " << Final << std::endl;
          }
     }
     else
     {
          std::cout << "[" << (Success ? "LOAD" : "FAIL") << "] " << Final << std::endl;
     }
}

/*
 * Write informational output.
 */

void ConsoleWriter::WriteInfo(const std::string &Message, bool Dot)
{
     /* Print INFO messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);
     std::cout << Final << std::endl;
}

/*
 * Write error output to stderr.
 */

void ConsoleWriter::WriteError(const std::string &Message, bool Dot)
{
     /* Print ERROR messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cerr << ColorBrightRed << "[ERROR]" << ColorReset << " " << ColorRed << Final << ColorReset << std::endl;
     }
     else
     {
          std::cerr << "[ERROR] " << Final << std::endl;
     }
}

/*
 * Write warning output.
 */

void ConsoleWriter::WriteWarning(const std::string &Message, bool Dot)
{
     /* Print WARNING messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "WARNING" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << ColorYellow << Final << ColorReset << std::endl;
     }
     else
     {
          std::cout << "[ WARNING ] " << Final << std::endl;
     }
}

/*
 * Write debug output when debug or verbose mode is enabled.
 */

void ConsoleWriter::WriteDebug(const std::string &Message, bool Dot)
{
     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cerr << ColorBlack << "[" << ColorReset << " " << ColorBrightMagenta << "DEBUG" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
     }
     else
     {
          std::cerr << "[ DEBUG ] " << Final << std::endl;
     }
}

/*
 * Write exit status output.
 */

void ConsoleWriter::WriteExit(const std::string &Message, bool Dot)
{
     /* Print EXIT messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cout << ColorBrightCyan << "[EXIT]" << ColorReset << " " << Final << std::endl;
     }
     else
     {
          std::cout << "[EXIT] " << Final << std::endl;
     }
}

/*
 * Write a visual header to separate console sections.
 */

void ConsoleWriter::WriteHeader(const std::string &Title)
{
     /* Print HEADER messages in both nofork and background modes. */

     if (SupportsColor())
     {
          std::cout << std::endl
                    << ColorBold << ColorBrightCyan << Title << ColorReset << std::endl;
     }
     else
     {
          std::cout << std::endl
                    << Title << std::endl;
     }
}

/*
 * Write a status line for server information.
 */

void ConsoleWriter::WriteServerInfo(const std::string &Key, const std::string &Value, const std::string &Status)
{
     /* Print SERVER INFO messages in both nofork and background modes. */

     if (SupportsColor())
     {
          std::string StatusColor = ColorBrightBlue;

          if (Status == "OK")
          {
               StatusColor = ColorBrightGreen;
          }
          else if (Status == "WARNING")
          {
               StatusColor = ColorBrightYellow;
          }

          std::cout << StatusColor << "[" << Status << "]" << ColorReset << " " << ColorBold << Key << ColorReset << ": " << Value << std::endl;
     }
     else
     {
          std::cout << "[" << Status << "] " << Key << ": " << Value << std::endl;
     }
}

/*
 * Write a blank line to the console.
 */

void ConsoleWriter::WriteBlankLine()
{
     /* Print blank lines in both nofork and background modes. */

     std::cout << std::endl;
}

/*
 * Write an initialization status line.
 */

void ConsoleWriter::WriteInit(const std::string &Message, bool Dot)
{
     /* Print INIT messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightBlue << "INIT" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
     }
     else
     {
          std::cout << "[ INIT ] " << Final << std::endl;
     }
}

/*
 * Write a critical initialization status to stderr.
 */

void ConsoleWriter::WriteInitCritical(const std::string &Message, bool Dot)
{
     /* Print CRITICAL messages in both nofork and background modes. */

     std::string Final = EnsurePeriod(Message, Dot);

     if (SupportsColor())
     {
          std::cerr << ColorBlack << "[" << ColorReset << " " << ColorBrightBlue << "INIT" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << ColorBold << ColorRed << "" << ColorReset << ColorRed << Final << ColorReset << std::endl;
     }
     else
     {
          std::cerr << "[ INIT ] " << Final << std::endl;
     }
}

/*
 * Write an async-signal-safe INIT message using write().
 */

void ConsoleWriter::WriteInitSafe(int Fd, const char *Message, bool Dot)
{
     /* Async-signal-safe version using write() syscall. */

     const char *BracketOpen = "[";

     const char *Space = " ";

     const char *InitText = "INIT";

     const char *BracketClose = "]";

     const char *AnsiBlack = "\033[30m";

     const char *AnsiBrightBlue = "\033[94m";

     const char *AnsiReset = "\033[0m";

     const char *Newline = "\n";

     const char *Period = ".";

     /* Write [ INIT ] with black brackets and blue text. */

     size_t Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     ssize_t WriteResult __attribute__((unused)) = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketOpen[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketOpen, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBrightBlue[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBrightBlue, Len);

     Len = 0;

     while (InitText[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, InitText, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketClose[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketClose, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     /* Write message text with manual length calculation. */

     Len = 0;

     if (Message)
     {
          while (Message[Len])
          {
               Len++;
          }

          WriteResult = write(Fd, Message, Len);

          if (Dot && Len > 0 && Message[Len - 1] != '.')
          {
               WriteResult = write(Fd, Period, 1);
          }
     }

     /* Write trailing newline. */

     WriteResult = write(Fd, Newline, 1);
}

/*
 * Write an async-signal-safe startup OK message.
 */

void ConsoleWriter::WriteStartupSafe(int Fd, const char *Message, bool Dot)
{
     /* Async-signal-safe version using write() syscall. */

     const char *BracketOpen = "[";

     const char *Space = " ";

     const char *OkText = "OK";

     const char *BracketClose = "]";

     const char *AnsiBlack = "\033[30m";

     const char *AnsiBrightGreen = "\033[92m";

     const char *AnsiReset = "\033[0m";

     const char *Newline = "\n";

     const char *Period = ".";

     /* Write [ OK ] with black brackets and green text. */

     size_t Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     ssize_t WriteResult __attribute__((unused)) = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketOpen[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketOpen, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBrightGreen[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBrightGreen, Len);

     Len = 0;

     while (OkText[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, OkText, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketClose[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketClose, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     /* Write message text with manual length calculation. */

     Len = 0;

     if (Message)
     {
          while (Message[Len])
          {
               Len++;
          }

          WriteResult = write(Fd, Message, Len);

          if (Dot && Len > 0 && Message[Len - 1] != '.')
          {
               WriteResult = write(Fd, Period, 1);
          }
     }

     /* Write trailing newline. */

     WriteResult = write(Fd, Newline, 1);
}

/*
 * Write an async-signal-safe startup message without status.
 */

void ConsoleWriter::WriteStartupPlainSafe(int Fd, const char *Message, bool Dot)
{
     /* Async-signal-safe plain output without status prefix. */

     const char *Newline = "\n";

     const char *Period = ".";

     size_t Len = 0;

     ssize_t WriteResult __attribute__((unused)) = 0;

     if (Message)
     {
          while (Message[Len])
          {
               Len++;
          }

          WriteResult = write(Fd, Message, Len);

          if (Dot && Len > 0 && Message[Len - 1] != '.')
          {
               WriteResult = write(Fd, Period, 1);
          }
     }

     WriteResult = write(Fd, Newline, 1);
}

/*
 * Write an async-signal-safe critical INIT message.
 */

void ConsoleWriter::WriteInitCriticalSafe(int Fd, const char *Message, bool Dot)
{
     /* Async-signal-safe version using write() syscall. */

     const char *BracketOpen = "[";

     const char *Space = " ";

     const char *InitText = "INIT";

     const char *BracketClose = "]";

     const char *CriticalTag = "";

     const char *AnsiBlack = "\033[30m";

     const char *AnsiBrightBlue = "\033[94m";

     const char *AnsiRed = "\033[31m";

     const char *AnsiReset = "\033[0m";

     const char *AnsiBold = "\033[1m";

     const char *Newline = "\n";

     const char *Period = ".";

     /* Write [ INIT ] with black brackets and blue text. */

     size_t Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     ssize_t WriteResult __attribute__((unused)) = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketOpen[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketOpen, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBrightBlue[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBrightBlue, Len);

     Len = 0;

     while (InitText[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, InitText, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     Len = 0;

     while (AnsiBlack[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBlack, Len);

     Len = 0;

     while (BracketClose[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, BracketClose, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     Len = 0;

     while (Space[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, Space, Len);

     /* Write critical tag in bold red. */

     Len = 0;

     while (AnsiBold[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiBold, Len);

     Len = 0;

     while (AnsiRed[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiRed, Len);

     Len = 0;

     while (CriticalTag[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, CriticalTag, Len);

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     /* Write message text in red. */

     Len = 0;

     while (AnsiRed[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiRed, Len);

     Len = 0;

     if (Message)
     {
          while (Message[Len])
          {
               Len++;
          }

          WriteResult = write(Fd, Message, Len);

          if (Dot && Len > 0 && Message[Len - 1] != '.')
          {
               WriteResult = write(Fd, Period, 1);
          }
     }

     Len = 0;

     while (AnsiReset[Len])
     {
          Len++;
     }

     WriteResult = write(Fd, AnsiReset, Len);

     /* Write trailing newline. */

     WriteResult = write(Fd, Newline, 1);
}

/*
 * Ensure messages end with a period when requested.
 */

std::string ConsoleWriter::EnsurePeriod(const std::string &Message, bool Dot)
{
     std::string Final = Message;

     if (Dot && !Final.empty() && Final.back() != '.')
     {
          Final += ".";
     }

     return Final;
}
