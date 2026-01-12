
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/hlquery.h"
#include "utils/consolewriter.h"






/* ANSI color codes*/
const std::string ConsoleWriter::COLOR_GREEN = "\033[32m";
const std::string ConsoleWriter::COLOR_RED = "\033[31m";
const std::string ConsoleWriter::COLOR_YELLOW = "\033[33m";
const std::string ConsoleWriter::COLOR_BLUE = "\033[34m";
const std::string ConsoleWriter::COLOR_CYAN = "\033[36m";
const std::string ConsoleWriter::COLOR_BLACK = "\033[30m";
const std::string ConsoleWriter::COLOR_WHITE = "\033[37m";
const std::string ConsoleWriter::COLOR_BOLD = "\033[1m";
const std::string ConsoleWriter::COLOR_RESET = "\033[0m";

/* Bright variants for enhanced visibility*/
const std::string ConsoleWriter::COLOR_BRIGHT_GREEN = "\033[92m";
const std::string ConsoleWriter::COLOR_BRIGHT_RED = "\033[91m";
const std::string ConsoleWriter::COLOR_BRIGHT_YELLOW = "\033[93m";
const std::string ConsoleWriter::COLOR_BRIGHT_BLUE = "\033[94m";
const std::string ConsoleWriter::COLOR_BRIGHT_CYAN = "\033[96m";
const std::string ConsoleWriter::COLOR_BRIGHT_MAGENTA = "\033[95m";

bool ConsoleWriter::SupportsColor()
{
        /* Always support colors for consistent output*/
        return true;
}

std::string ConsoleWriter::FormatMessage(const std::string& message, const std::string& status)
{
        std::ostringstream oss;

        if (SupportsColor())
        {
                /* Create colored status with black brackets and colored text*/
                if (status == "OK") {
                        oss << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_GREEN << "OK" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message;
                } else if (status == "INFO") {
                        oss << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_BLUE << "INFO" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message;
                } else if (status == "WARN") {
                        oss << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_RED << "WARNING" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message;
                } else {
                        oss << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_RED << "ERROR" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message;
                }
        }
        else
        {
                oss << "[" << status << "] " << message;
        }

        return oss.str();
}

void ConsoleWriter::WriteStartup(const std::string& message, bool success)
{
        // Print [ OK ] messages in BOTH nofork and background modes
        // These messages should appear identically in both modes
        if (SupportsColor())
        {
                if (success)
                {
                        std::cout << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_GREEN << "OK" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message
                                  << std::endl;
                }
                else
                {
                        std::cout << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_RED << "FAIL" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message
                                  << std::endl;
                }
        }
        else
        {
                std::cout << "[ OK ] " << message << std::endl;
        }
}

void ConsoleWriter::WriteConfig(const std::string& message, bool success)
{
        // Print CONFIG messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                if (success)
                {
                        std::cout << COLOR_BRIGHT_BLUE << "[LOAD]" << COLOR_RESET << " " << message
                                  << std::endl;
                }
                else
                {
                        std::cout << COLOR_BRIGHT_RED << "[FAIL]" << COLOR_RESET << " " << message
                                  << std::endl;
                }
        }
        else
        {
                std::cout << "[" << (success ? "LOAD" : "FAIL") << "] " << message << std::endl;
        }
}

void ConsoleWriter::WriteInfo(const std::string& message)
{
        // Print INFO messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cout << COLOR_BRIGHT_BLUE << "[INFO]" << COLOR_RESET << " " << message << std::endl;
        }
        else
        {
                std::cout << "[INFO] " << message << std::endl;
        }
}

void ConsoleWriter::WriteError(const std::string& message)
{
        // Print ERROR messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cerr << COLOR_BRIGHT_RED << "[ERROR]" << COLOR_RESET << " " << COLOR_RED << message << COLOR_RESET
                          << std::endl;
        }
        else
        {
                std::cerr << "[ERROR] " << message << std::endl;
        }
}

void ConsoleWriter::WriteWarning(const std::string& message)
{
        // Print WARNING messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cout << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_RED << "WARNING" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << COLOR_YELLOW << message << COLOR_RESET
                          << std::endl;
        }
        else
        {
                std::cout << "[ WARNING ] " << message << std::endl;
        }
}

void ConsoleWriter::WriteDebug(const std::string& message)
{
        // Daemon mode removed - always write to stderr in nofork mode
        // Debug messages can also go through LogManager if available
        try {
            if (false) {  // Never true - daemon mode removed
                // Daemon mode removed - this code path is never executed
                if (Instance && Instance->Logs) {
                    Instance->Logs->Debug("console", message);
                }
                return;
            }
        } catch (...) {
            // If check fails, continue with normal output (foreground mode)
        }
        
        if (SupportsColor())
        {
                std::cerr << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_MAGENTA << "DEBUG" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message << std::endl;
        }
        else
        {
                std::cerr << "[ DEBUG ] " << message << std::endl;
        }
}

void ConsoleWriter::WriteExit(const std::string& message)
{
        // Print EXIT messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cout << COLOR_BRIGHT_CYAN << "[EXIT]" << COLOR_RESET << " " << message << std::endl;
        }
        else
        {
                std::cout << "[EXIT] " << message << std::endl;
        }
}

void ConsoleWriter::WriteHeader(const std::string& title)
{
        // Print HEADER messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cout << std::endl
                          << COLOR_BOLD << COLOR_BRIGHT_CYAN << title
                          << COLOR_RESET << std::endl;
        }
        else
        {
                std::cout << std::endl << title << std::endl;
        }
}

void ConsoleWriter::WriteServerInfo(const std::string& key, const std::string& value,
                                    const std::string& status)
{
        // Print SERVER INFO messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::string status_color = COLOR_BRIGHT_BLUE;
                if (status == "OK")
                        status_color = COLOR_BRIGHT_GREEN;
                else if (status == "WARNING")
                        status_color = COLOR_BRIGHT_YELLOW;

                std::cout << status_color << "[" << status << "]" << COLOR_RESET << " " << COLOR_BOLD
                          << key << COLOR_RESET << ": " << value << std::endl;
        }
        else
        {
                std::cout << "[" << status << "] " << key << ": " << value << std::endl;
        }
}

void ConsoleWriter::WriteBlankLine()
{
        // Print blank lines in BOTH nofork and background modes
        std::cout << std::endl;
}

void ConsoleWriter::WriteInit(const std::string& message)
{
        // Print INIT messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cout << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_BLUE << "INIT" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " << message << std::endl;
        }
        else
        {
                std::cout << "[ INIT ] " << message << std::endl;
        }
}

void ConsoleWriter::WriteInitCritical(const std::string& message)
{
        // Print CRITICAL messages in BOTH nofork and background modes
        if (SupportsColor())
        {
                std::cerr << COLOR_BLACK << "[" << COLOR_RESET << " " << COLOR_BRIGHT_BLUE << "INIT" << COLOR_RESET << " " << COLOR_BLACK << "]" << COLOR_RESET << " " 
                          << COLOR_BOLD << COLOR_RED << "CRITICAL: " << COLOR_RESET 
                          << COLOR_RED << message << COLOR_RESET << std::endl;
        }
        else
        {
                std::cerr << "[ INIT ] CRITICAL: " << message << std::endl;
        }
}

void ConsoleWriter::WriteInitSafe(int fd, const char* message)
{
        /* Async-signal-safe version using write() syscall*/
        const char* bracket_open = "[";
        const char* space = " ";
        const char* init_text = "INIT";
        const char* bracket_close = "]";
        const char* color_black = "\033[30m";
        const char* color_blue = "\033[94m";
        const char* color_reset = "\033[0m";
        const char* newline = "\n";
        
        /* Write [ INIT ] with black brackets and blue text*/
        size_t len = 0;
        while (color_black[len]) len++;
        ssize_t _ __attribute__((unused)) = write(fd, color_black, len);
        
        len = 0;
        while (bracket_open[len]) len++;
        _ = write(fd, bracket_open, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_blue[len]) len++;
        _ = write(fd, color_blue, len);
        
        len = 0;
        while (init_text[len]) len++;
        _ = write(fd, init_text, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_black[len]) len++;
        _ = write(fd, color_black, len);
        
        len = 0;
        while (bracket_close[len]) len++;
        _ = write(fd, bracket_close, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        /* Write message (calculate length manually for async-signal-safety)*/
        len = 0;
        if (message) {
                while (message[len]) len++;
                _ = write(fd, message, len);
        }
        
        /* Write newline*/
        _ = write(fd, newline, 1);
}

void ConsoleWriter::WriteStartupSafe(int fd, const char* message)
{
        /* Async-signal-safe version using write() syscall*/
        const char* bracket_open = "[";
        const char* space = " ";
        const char* ok_text = "OK";
        const char* bracket_close = "]";
        const char* color_black = "\033[30m";
        const char* color_green = "\033[92m";
        const char* color_reset = "\033[0m";
        const char* newline = "\n";
        
        /* Write [ OK ] with black brackets and green text*/
        size_t len = 0;
        while (color_black[len]) len++;
        ssize_t _ __attribute__((unused)) = write(fd, color_black, len);
        
        len = 0;
        while (bracket_open[len]) len++;
        _ = write(fd, bracket_open, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_green[len]) len++;
        _ = write(fd, color_green, len);
        
        len = 0;
        while (ok_text[len]) len++;
        _ = write(fd, ok_text, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_black[len]) len++;
        _ = write(fd, color_black, len);
        
        len = 0;
        while (bracket_close[len]) len++;
        _ = write(fd, bracket_close, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        /* Write message (calculate length manually for async-signal-safety)*/
        len = 0;
        if (message) {
                while (message[len]) len++;
                _ = write(fd, message, len);
        }
        
        /* Write newline*/
        _ = write(fd, newline, 1);
}

void ConsoleWriter::WriteInitCriticalSafe(int fd, const char* message)
{
        /* Async-signal-safe version using write() syscall*/
        const char* bracket_open = "[";
        const char* space = " ";
        const char* init_text = "INIT";
        const char* bracket_close = "]";
        const char* critical_tag = "CRITICAL: ";
        const char* color_black = "\033[30m";
        const char* color_blue = "\033[94m";
        const char* color_red = "\033[31m";
        const char* color_reset = "\033[0m";
        const char* color_bold = "\033[1m";
        const char* newline = "\n";
        
        /* Write [ INIT ] with black brackets and blue text*/
        size_t len = 0;
        while (color_black[len]) len++;
        ssize_t _ __attribute__((unused)) = write(fd, color_black, len);
        
        len = 0;
        while (bracket_open[len]) len++;
        _ = write(fd, bracket_open, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_blue[len]) len++;
        _ = write(fd, color_blue, len);
        
        len = 0;
        while (init_text[len]) len++;
        _ = write(fd, init_text, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        len = 0;
        while (color_black[len]) len++;
        _ = write(fd, color_black, len);
        
        len = 0;
        while (bracket_close[len]) len++;
        _ = write(fd, bracket_close, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        len = 0;
        while (space[len]) len++;
        _ = write(fd, space, len);
        
        /* Write CRITICAL tag in bold red*/
        len = 0;
        while (color_bold[len]) len++;
        _ = write(fd, color_bold, len);
        
        len = 0;
        while (color_red[len]) len++;
        _ = write(fd, color_red, len);
        
        len = 0;
        while (critical_tag[len]) len++;
        _ = write(fd, critical_tag, len);
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        /* Write message in red*/
        len = 0;
        while (color_red[len]) len++;
        _ = write(fd, color_red, len);
        
        len = 0;
        if (message) {
                while (message[len]) len++;
                _ = write(fd, message, len);
        }
        
        len = 0;
        while (color_reset[len]) len++;
        _ = write(fd, color_reset, len);
        
        /* Write newline*/
        _ = write(fd, newline, 1);
}
