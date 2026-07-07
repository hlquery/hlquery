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

/*
 * hlquery talk - interactive command shell for lightweight server inspection.
 */

#include <chrono>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <string>
#include <unistd.h>

#include "cli/app.h"
#include "talk/linenoise.h"
#include "talk/session.h"

int main(int argc, char **argv)
{
     /* Default connection settings for the talk shell. */

     std::string host = "localhost";
     std::string port = "9200";

     std::string command_to_run;

     const std::string history_file = ResolveHistoryFilePath();
     const char *program_name = (argc > 0 && argv[0] != nullptr) ? argv[0] : "talk";

     /* Supported short and long command-line options. */

     static struct option long_options[] =
     {
          {"host", required_argument, nullptr, 'H'},
          {"port", required_argument, nullptr, 'p'},
          {"command", required_argument, nullptr, 'c'},
          {"help", no_argument, nullptr, 'h'},
          {nullptr, 0, nullptr, 0}
     };

     /* Silence getopt diagnostics because this entry point prints its own messages. */

     opterr = 0;
     optind = 1;

     int option = 0;

     /* Parse connection and one-shot execution flags before starting the shell. */

     while ((option = getopt_long(argc, argv, "H:p:c:h", long_options, nullptr)) != -1)
     {
          switch (option)
          {
          case 'H':
               host = optarg;
               break;

          case 'p':
               port = optarg;
               break;

          case 'c':
               command_to_run = optarg;
               break;

          case 'h':
               std::cout << "Usage: " << program_name << " [--host HOST] [--port PORT] [-c COMMAND]\n";
               std::cout << "Default endpoint: http://localhost:9200\n\n";
               PrintHelp();
               return 0;

          case '?':
               if (optopt == 'H' || optopt == 'p' || optopt == 'c')
               {
                    std::cerr << "Option requires a value: -" << static_cast<char>(optopt) << "\n";
               }
               else if (optind > 0 && optind <= argc && argv[optind - 1] != nullptr)
               {
                    std::cerr << "Unknown argument: " << argv[optind - 1] << "\n";
               }
               else
               {
                    std::cerr << "Unknown argument.\n";
               }

               std::cerr << "Use --help to see available options.\n";
               return 1;

          default:
               std::cerr << "Use --help to see available options.\n";
               return 1;
          }
     }

     /* Reject leftover positional arguments because talk only accepts named options. */

     if (optind < argc)
     {
          for (int index = optind; index < argc; ++index)
          {
               std::cerr << "Unknown argument: " << argv[index] << "\n";
          }

          std::cerr << "Use --help to see available options.\n";
          return 1;
     }

     /* Normalize the host so validation and prompt rendering use one canonical value. */

     host = NormalizeHostValue(host);

     if (!IsValidHostValue(host))
     {
          std::cerr << "Invalid host value: " << host << "\n";
          return 1;
     }

     int parsed_port = 0;

     /* Parse and validate the port separately so the final URL always uses a normalized number. */

     try
     {
          size_t consumed = 0;
          parsed_port = std::stoi(port, &consumed);

          if (consumed != port.size())
          {
               std::cerr << "Invalid port value: " << port << "\n";
               return 1;
          }
     }
     catch (...)
     {
          std::cerr << "Invalid port value: " << port << "\n";
          return 1;
     }

     if (!IsValidPortValue(parsed_port))
     {
          std::cerr << "Invalid port value: " << port << "\n";
          return 1;
     }

     port = std::to_string(parsed_port);

     /* Initialize the CLI client and restore persisted talk aliases. */

     const std::string base_url = "http://" + host + ":" + port;
     HLQueryCLI cli(base_url, false, "", "talk");

     TalkState state;
     state.SessionID = "talk-" + std::to_string(static_cast<long long>(getpid())) + "-" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());

     LoadTalkAliases(state);

     /* Execute a single command when -c or --command is supplied. */

     if (!command_to_run.empty())
     {
          ExecuteTalkCommand(TrimWhitespace(command_to_run), cli, state, host, port);
          SaveTalkAliases(state);
          return cli.GetExitCode();
     }

     /* Prepare history and completion before entering the interactive loop. */

     linenoiseHistoryLoad(history_file.c_str());
     linenoiseSetCompletionCallback(CompleteTalkCommandLine);

     /* Run the interactive shell until the user exits or an interrupt closes the session. */

     while (true)
     {
          const std::string prompt = BuildPrompt(host, port, state);

          char *raw_line = linenoise(prompt.c_str());

          /* Handle terminal control flow before attempting to parse a command line. */

          if (raw_line == nullptr)
          {
               if (linenoiseWasControlD() != 0)
               {
                    std::cout << "\x1b[2J\x1b[H";
                    std::cout.flush();
                    linenoiseResetState();
                    continue;
               }

               if (linenoiseWasInterrupted() != 0)
               {
                    break;
               }

               break;
          }

          /* Trim the accepted input and skip empty commands. */

          const std::string line = TrimWhitespace(raw_line);

          std::free(raw_line);

          if (line.empty())
          {
               continue;
          }

          linenoiseHistoryAdd(line.c_str());

          const bool keep_running = ExecuteTalkCommand(line, cli, state, host, port);

          linenoiseResetState();

          if (!keep_running)
          {
               break;
          }
     }

     /* Persist shell state on exit. */

     linenoiseHistorySave(history_file.c_str());
     linenoiseHistoryFree();
     SaveTalkAliases(state);

     return cli.GetExitCode();
}
