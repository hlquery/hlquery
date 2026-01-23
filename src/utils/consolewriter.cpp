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

#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/hlquery.h"
#include "utils/consolewriter.h"

/* ANSI color codes */

const std::string ConsoleWriter::ColorGreen = "\033[32m";
const std::string ConsoleWriter::ColorRed = "\033[31m";
const std::string ConsoleWriter::ColorYellow = "\033[33m";
const std::string ConsoleWriter::ColorBlue = "\033[34m";
const std::string ConsoleWriter::ColorCyan = "\033[36m";
const std::string ConsoleWriter::ColorBlack = "\033[30m";
const std::string ConsoleWriter::ColorWhite = "\033[37m";
const std::string ConsoleWriter::ColorBold = "\033[1m";
const std::string ConsoleWriter::ColorReset = "\033[0m";

/* Bright variants for enhanced visibility */

const std::string ConsoleWriter::ColorBrightGreen = "\033[92m";
const std::string ConsoleWriter::ColorBrightRed = "\033[91m";
const std::string ConsoleWriter::ColorBrightYellow = "\033[93m";
const std::string ConsoleWriter::ColorBrightBlue = "\033[94m";
const std::string ConsoleWriter::ColorBrightCyan = "\033[96m";
const std::string ConsoleWriter::ColorBrightMagenta = "\033[95m";

/* Comment goes here */

bool ConsoleWriter::SupportsColor()
{
    /* Always support colors for consistent output */

    return true;
}

/* Comment goes here */

std::string ConsoleWriter::FormatMessage(const std::string& Message, const std::string& Status)
{
    std::ostringstream Oss;

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        /* Create colored status with black brackets and colored text */

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

/* Comment goes here */

void ConsoleWriter::WriteStartup(const std::string& Message, bool Success)
{
    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        if (Success)
        {
            std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightGreen << "OK" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final
                      << std::endl;
        }
        else
        {
            std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "FAIL" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[ OK ] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteConfig(const std::string& Message, bool Success)
{
    /* Print CONFIG messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        if (Success)
        {
            std::cout << ColorBrightBlue << "[LOAD]" << ColorReset << " " << Final
                      << std::endl;
        }
        else
        {
            std::cout << ColorBrightRed << "[FAIL]" << ColorReset << " " << Final
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[" << (Success ? "LOAD" : "FAIL") << "] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteInfo(const std::string& Message)
{
    /* Print INFO messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cout << ColorBrightBlue << "[INFO]" << ColorReset << " " << Final << std::endl;
    }
    else
    {
        std::cout << "[INFO] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteError(const std::string& Message)
{
    /* Print ERROR messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cerr << ColorBrightRed << "[ERROR]" << ColorReset << " " << ColorRed << Final << ColorReset
                  << std::endl;
    }
    else
    {
        std::cerr << "[ERROR] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteWarning(const std::string& Message)
{
    /* Print WARNING messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightRed << "WARNING" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << ColorYellow << Final << ColorReset
                  << std::endl;
    }
    else
    {
        std::cout << "[ WARNING ] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteDebug(const std::string& Message)
{
    /* Daemon mode removed - always write to stderr in nofork mode */
    /* Debug messages can also go through LogManager if available */

    std::string Final = EnsurePeriod(Message);

    try 
    {
        if (false) 
        {  
            /* Never true - daemon mode removed */
            /* Daemon mode removed - this code path is never executed */

            if (Instance && Instance->Logs) 
            {
                Instance->Logs->Debug("console", Final);
            }

            return;
        }
    } 
    catch (...) 
    {
        /* If check fails, continue with normal output (foreground mode) */
    }
    
    if (SupportsColor())
    {
        std::cerr << ColorBlack << "[" << ColorReset << " " << ColorBrightMagenta << "DEBUG" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
    }
    else
    {
        std::cerr << "[ DEBUG ] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteExit(const std::string& Message)
{
    /* Print EXIT messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cout << ColorBrightCyan << "[EXIT]" << ColorReset << " " << Final << std::endl;
    }
    else
    {
        std::cout << "[EXIT] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteHeader(const std::string& Title)
{
    /* Print HEADER messages in BOTH nofork and background modes */

    if (SupportsColor())
    {
        std::cout << std::endl
                  << ColorBold << ColorBrightCyan << Title
                  << ColorReset << std::endl;
    }
    else
    {
        std::cout << std::endl << Title << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteServerInfo(const std::string& Key, const std::string& Value,
                                    const std::string& Status)
{
    /* Print SERVER INFO messages in BOTH nofork and background modes */

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

        std::cout << StatusColor << "[" << Status << "]" << ColorReset << " " << ColorBold
                  << Key << ColorReset << ": " << Value << std::endl;
    }
    else
    {
        std::cout << "[" << Status << "] " << Key << ": " << Value << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteBlankLine()
{
    /* Print blank lines in BOTH nofork and background modes */

    std::cout << std::endl;
}

/* Comment goes here */

void ConsoleWriter::WriteInit(const std::string& Message)
{
    /* Print INIT messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cout << ColorBlack << "[" << ColorReset << " " << ColorBrightBlue << "INIT" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " << Final << std::endl;
    }
    else
    {
        std::cout << "[ INIT ] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteInitCritical(const std::string& Message)
{
    /* Print CRITICAL messages in BOTH nofork and background modes */

    std::string Final = EnsurePeriod(Message);

    if (SupportsColor())
    {
        std::cerr << ColorBlack << "[" << ColorReset << " " << ColorBrightBlue << "INIT" << ColorReset << " " << ColorBlack << "]" << ColorReset << " " 
                  << ColorBold << ColorRed << "" << ColorReset 
                  << ColorRed << Final << ColorReset << std::endl;
    }
    else
    {
        std::cerr << "[ INIT ] " << Final << std::endl;
    }
}

/* Comment goes here */

void ConsoleWriter::WriteInitSafe(int Fd, const char* Message)
{
    /* Async-signal-safe version using write() syscall */

    const char* BracketOpen = "[";
    const char* Space = " ";
    const char* InitText = "INIT";
    const char* BracketClose = "]";
    const char* ColorBlack = "\033[30m";
    const char* ColorBlue = "\033[94m";
    const char* ColorReset = "\033[0m";
    const char* Newline = "\n";
    const char* Period = ".";
    
    /* Write [ INIT ] with black brackets and blue text */

    size_t Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    ssize_t WriteResult __attribute__((unused)) = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketOpen[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketOpen, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorBlue[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBlue, Len);
    
    Len = 0;

    while (InitText[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, InitText, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketClose[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketClose, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    /* Write Message (calculate length manually for async-signal-safety) */

    Len = 0;

    if (Message) 
    {
        while (Message[Len]) 
        {
            Len++;
        }

        WriteResult = write(Fd, Message, Len);

        if (Len > 0 && Message[Len - 1] != '.')
        {
            WriteResult = write(Fd, Period, 1);
        }
    }
    
    /* Write Newline */

    WriteResult = write(Fd, Newline, 1);
}

/* Comment goes here */

void ConsoleWriter::WriteStartupSafe(int Fd, const char* Message)
{
    /* Async-signal-safe version using write() syscall */

    const char* BracketOpen = "[";
    const char* Space = " ";
    const char* OkText = "OK";
    const char* BracketClose = "]";
    const char* ColorBlack = "\033[30m";
    const char* ColorGreen = "\033[92m";
    const char* ColorReset = "\033[0m";
    const char* Newline = "\n";
    const char* Period = ".";
    
    /* Write [ OK ] with black brackets and green text */

    size_t Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    ssize_t WriteResult __attribute__((unused)) = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketOpen[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketOpen, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorGreen[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorGreen, Len);
    
    Len = 0;

    while (OkText[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, OkText, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketClose[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketClose, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    /* Write Message (calculate length manually for async-signal-safety) */

    Len = 0;

    if (Message) 
    {
        while (Message[Len]) 
        {
            Len++;
        }

        WriteResult = write(Fd, Message, Len);

        if (Len > 0 && Message[Len - 1] != '.')
        {
            WriteResult = write(Fd, Period, 1);
        }
    }
    
    /* Write Newline */

    WriteResult = write(Fd, Newline, 1);
}

/* Comment goes here */

void ConsoleWriter::WriteInitCriticalSafe(int Fd, const char* Message)
{
    /* Async-signal-safe version using write() syscall */

    const char* BracketOpen = "[";
    const char* Space = " ";
    const char* InitText = "INIT";
    const char* BracketClose = "]";
    const char* CriticalTag = "";
    const char* ColorBlack = "\033[30m";
    const char* ColorBlue = "\033[94m";
    const char* ColorRed = "\033[31m";
    const char* ColorReset = "\033[0m";
    const char* ColorBold = "\033[1m";
    const char* Newline = "\n";
    const char* Period = ".";
    
    /* Write [ INIT ] with black brackets and blue text */

    size_t Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    ssize_t WriteResult __attribute__((unused)) = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketOpen[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketOpen, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorBlue[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBlue, Len);
    
    Len = 0;

    while (InitText[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, InitText, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    Len = 0;

    while (ColorBlack[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBlack, Len);
    
    Len = 0;

    while (BracketClose[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, BracketClose, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    Len = 0;

    while (Space[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, Space, Len);
    
    /* Write CRITICAL tag in bold red */

    Len = 0;

    while (ColorBold[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorBold, Len);
    
    Len = 0;

    while (ColorRed[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorRed, Len);
    
    Len = 0;

    while (CriticalTag[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, CriticalTag, Len);
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    /* Write Message in red */

    Len = 0;

    while (ColorRed[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorRed, Len);
    
    Len = 0;

    if (Message) 
    {
        while (Message[Len]) 
        {
            Len++;
        }

        WriteResult = write(Fd, Message, Len);

        if (Len > 0 && Message[Len - 1] != '.')
        {
            WriteResult = write(Fd, Period, 1);
        }
    }
    
    Len = 0;

    while (ColorReset[Len]) 
    {
        Len++;
    }

    WriteResult = write(Fd, ColorReset, Len);
    
    /* Write Newline */

    WriteResult = write(Fd, Newline, 1);
}

/* Helper to ensure periods in messages */

std::string ConsoleWriter::EnsurePeriod(const std::string& Message)
{
    std::string Final = Message;

    if (!Final.empty() && Final.back() != '.')
    {
        Final += ".";
    }

    return Final;
}
