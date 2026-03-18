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
#include <sstream>

#include "cli_utils.h"
#include "utils/consolewriter.h"

namespace hlquery_cli
{
/* Prints a success message. */

void PrintSuccess(const std::string &message)
{
     ConsoleWriter::WriteInfo(message);
}

/* Prints an error message. */

void PrintError(const std::string &message)
{
     ConsoleWriter::WriteError("Error: " + message, true);
}

/* Prints an info message. */

void PrintInfo(const std::string &message)
{
     std::cout << Colors::CYAN << message << Colors::RESET << std::endl;
}

/* Prints a warning message. */

void PrintWarning(const std::string &message)
{
     std::cout << Colors::YELLOW << "Warning: " << message << Colors::RESET << std::endl;
}

/* Escapes a JSON string. */

std::string EscapeJSONString(const std::string &str)
{
     std::ostringstream oss;

     /* Loop through each character to escape it. */

     for (char c : str)
     {
          switch (c)
          {
               case '\"':

                    oss << "\\\"";
                    break;

               case '\\':

                    oss << "\\\\";
                    break;

               case '\b':

                    oss << "\\b";
                    break;

               case '\f':

                    oss << "\\f";
                    break;

               case '\n':

                    oss << "\\n";
                    break;

               case '\r':

                    oss << "\\r";
                    break;

               case '\t':

                    oss << "\\t";
                    break;

               default:

                    if (c < 0x20)
                    {
                         oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    }
                    else
                    {
                         oss << c;
                    }
          }
     }

     return oss.str();
}

/* Encodes a URL string. */

std::string UrlEncode(const std::string &str)
{
     std::ostringstream oss;

     /* Loop through each character to encode it. */

     for (unsigned char c : str)
     {
          if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
          {
               oss << c;
          }
          else
          {
               oss << '%' << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(c);
          }
     }

     return oss.str();
}
}
