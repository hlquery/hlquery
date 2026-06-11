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
 * Minimal local linenoise-compatible API used by the talk REPL.
 * Interactive editing deliberately treats input as single-byte ASCII.
 */

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "talk/linenoise.h"

namespace
{
     /* Shared command history for the local REPL session. */

     std::vector<std::string> History;
     linenoiseCompletionCallback CompletionCallback = nullptr;
     bool LastReadWasControlD = false;
     bool LastReadWasInterrupted = false;
     size_t LastRefreshRows = 1;
     size_t LastCursorRow = 0;
     constexpr size_t MaxHistoryEntries = 500;

     enum class EscapeKey
     {
          None,
          ArrowUp,
          ArrowDown,
          ArrowRight,
          ArrowLeft,
          Home,
          End,
          Delete
     };

     enum class EditorReadAction
     {
          None,
          Interrupted,
          EndOfInput
     };

     struct EditorState
     {
          std::string Line;
          std::string PendingLine;
          size_t HistoryIndex = 0;
          size_t CursorPosition = 0;
     };

     class RawModeGuard
     {
        public:
          RawModeGuard() = default;

          explicit RawModeGuard(const termios &original)
              : Original(original),
                Active(true)
          {
 
          }

          RawModeGuard(const RawModeGuard &) = delete;
          RawModeGuard &operator=(const RawModeGuard &) = delete;

          ~RawModeGuard()
          {
               Restore();
          }

          void Restore()
          {
               if (Active)
               {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &Original);
                    Active = false;
               }
          }

        private:
          termios Original {};
          bool Active = false;
     };

     size_t GetTerminalColumns()
     {
          struct winsize window_size;

          if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0 && window_size.ws_col > 0)
          {
               return static_cast<size_t>(window_size.ws_col);
          }

          return 80;
     }

     void TrimHistoryToLimit()
     {
          if (History.size() <= MaxHistoryEntries)
          {
               return;
          }

          History.erase(History.begin(),
                        History.begin() + static_cast<std::ptrdiff_t>(History.size() - MaxHistoryEntries));
     }

     bool IsAsciiPrintable(char character)
     {
          const unsigned char value = static_cast<unsigned char>(character);

          return value >= 32 && value <= 126;
     }

     size_t CountCursorRows(size_t character_count, size_t terminal_columns)
     {
          if (terminal_columns == 0)
          {
               return 0;
          }

          return character_count / terminal_columns;
     }

     size_t CountOccupiedRows(size_t character_count, size_t terminal_columns)
     {
          if (terminal_columns == 0)
          {
               return 1;
          }

          if (character_count == 0)
          {
               return 1;
          }

          /*
           * When the rendered text lands exactly on a terminal column boundary,
           * the cursor advances to column zero on the following physical row.
           * RefreshLine() needs to clear that extra row as well before redrawing,
           * otherwise history navigation can walk back into already printed output.
           */

          return CountCursorRows(character_count, terminal_columns) + 1;
     }

     /* Redraw the current prompt and editable line buffer. */

     void RefreshLine(const char *prompt, const std::string &line, size_t cursor_position)
     {
          const size_t terminal_columns = GetTerminalColumns();
          const size_t prompt_length = prompt != nullptr ? std::strlen(prompt) : 0;
          const size_t line_length = line.size();
          const size_t rendered_length = prompt_length + line_length;
          const size_t cursor_length = prompt_length + cursor_position;

          std::cout << '\r';

          if (LastCursorRow > 0)
          {
               std::cout << "\x1b[" << LastCursorRow << 'A';
          }

          std::cout << '\r';
          /*
           * Clearing row-by-row with EL can leave wrapped prompt/input state behind
           * on some terminals. Jump to the start of the previously rendered block
           * and clear everything below before painting the new buffer.
           */
          std::cout << "\x1b[J";

          if (prompt != nullptr)
          {
               std::cout << prompt;
          }

          std::cout << line << "\x1b[K";

          const size_t rendered_cursor_row = CountCursorRows(rendered_length, terminal_columns);
          const size_t target_cursor_row = CountCursorRows(cursor_length, terminal_columns);
          const size_t target_cursor_col = terminal_columns == 0 ? 0 : (cursor_length % terminal_columns);

          if (rendered_cursor_row > target_cursor_row)
          {
               std::cout << "\x1b[" << (rendered_cursor_row - target_cursor_row) << 'A';
          }

          std::cout << '\r';

          if (target_cursor_col > 0)
          {
               std::cout << "\x1b[" << target_cursor_col << 'C';
          }

          LastRefreshRows = CountOccupiedRows(rendered_length, terminal_columns);
          LastCursorRow = target_cursor_row;

          std::cout.flush();
     }

     /* Enable raw terminal mode for interactive editing. */

     bool EnableRawMode(termios &original)
     {
          if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) == -1)
          {
               return false;
          }

          termios raw = original;

          raw.c_iflag &= static_cast<unsigned long>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
          raw.c_oflag &= static_cast<unsigned long>(~(OPOST));
          raw.c_cflag |= CS8;
          raw.c_lflag &= static_cast<unsigned long>(~(ECHO | ICANON | IEXTEN | ISIG));
          raw.c_cc[VMIN] = 1;
          raw.c_cc[VTIME] = 0;

          return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != -1;
     }

     EscapeKey ReadEscapeKey()
     {
          char prefix = '\0';

          if (read(STDIN_FILENO, &prefix, 1) != 1)
          {
               return EscapeKey::None;
          }

          if (prefix == 'O')
          {
               char final = '\0';

               if (read(STDIN_FILENO, &final, 1) != 1)
               {
                    return EscapeKey::None;
               }

               switch (final)
               {
                    case 'A':
                         return EscapeKey::ArrowUp;
                    case 'B':
                         return EscapeKey::ArrowDown;
                    case 'C':
                         return EscapeKey::ArrowRight;
                    case 'D':
                         return EscapeKey::ArrowLeft;
                    case 'H':
                         return EscapeKey::Home;
                    case 'F':
                         return EscapeKey::End;
                    default:
                         return EscapeKey::None;
               }
          }

          if (prefix != '[')
          {
               return EscapeKey::None;
          }

          std::string sequence;

          while (sequence.size() < 8)
          {
               char next = '\0';

               if (read(STDIN_FILENO, &next, 1) != 1)
               {
                    return EscapeKey::None;
               }

               sequence.push_back(next);

               if (next >= '@' && next <= '~')
               {
                    break;
               }
          }

          if (sequence.empty())
          {
               return EscapeKey::None;
          }

          if (sequence == "A")
          {
               return EscapeKey::ArrowUp;
          }

          if (sequence == "B")
          {
               return EscapeKey::ArrowDown;
          }

          if (sequence == "C")
          {
               return EscapeKey::ArrowRight;
          }

          if (sequence == "D")
          {
               return EscapeKey::ArrowLeft;
          }

          if (sequence == "H" || sequence == "1~" || sequence == "7~")
          {
               return EscapeKey::Home;
          }

          if (sequence == "F" || sequence == "4~" || sequence == "8~")
          {
               return EscapeKey::End;
          }

          if (sequence == "3~")
          {
               return EscapeKey::Delete;
          }

          return EscapeKey::None;
     }

     bool ApplyBackspace(EditorState &state)
     {
          if (state.CursorPosition == 0 || state.Line.empty())
          {
               return false;
          }

          state.Line.erase(state.CursorPosition - 1, 1);
          --state.CursorPosition;
          state.HistoryIndex = History.size();

          return true;
     }

     bool ApplyCursorControl(EditorState &state, char character)
     {
          if (character != 1 && character != 2 && character != 5 && character != 6)
          {
               return false;
          }

          if (character == 1)
          {
               state.CursorPosition = 0;
          }
          else if (character == 5)
          {
               state.CursorPosition = state.Line.size();
          }
          else if (character == 2)
          {
               if (state.CursorPosition > 0)
               {
                    --state.CursorPosition;
               }
          }
          else if (character == 6)
          {
               if (state.CursorPosition < state.Line.size())
               {
                    ++state.CursorPosition;
               }
          }

          return true;
     }

     EditorReadAction ApplyControlCharacter(EditorState &state, char character)
     {
          if (character == 3)
          {
               return EditorReadAction::Interrupted;
          }

          if (character != 4)
          {
               return EditorReadAction::None;
          }

          if (state.Line.empty())
          {
               return EditorReadAction::EndOfInput;
          }

          if (state.CursorPosition < state.Line.size())
          {
               state.Line.erase(state.CursorPosition, 1);
               state.HistoryIndex = History.size();
          }

          return EditorReadAction::None;
     }

     bool ApplyEscapeKey(EditorState &state, EscapeKey key)
     {
          switch (key)
          {
               case EscapeKey::ArrowUp:
                    if (!History.empty() && state.HistoryIndex > 0)
                    {
                         if (state.HistoryIndex == History.size())
                         {
                              state.PendingLine = state.Line;
                         }

                         --state.HistoryIndex;
                         state.Line = History[state.HistoryIndex];
                         state.CursorPosition = state.Line.size();
                         return true;
                    }

                    return false;
               case EscapeKey::ArrowDown:
                    if (state.HistoryIndex < History.size())
                    {
                         ++state.HistoryIndex;

                         if (state.HistoryIndex == History.size())
                         {
                              state.Line = state.PendingLine;
                         }
                         else
                         {
                              state.Line = History[state.HistoryIndex];
                         }

                         state.CursorPosition = state.Line.size();
                         return true;
                    }

                    return false;
               case EscapeKey::ArrowRight:
                    if (state.CursorPosition < state.Line.size())
                    {
                         ++state.CursorPosition;
                         return true;
                    }

                    return false;
               case EscapeKey::ArrowLeft:
                    if (state.CursorPosition > 0)
                    {
                         --state.CursorPosition;
                         return true;
                    }

                    return false;
               case EscapeKey::Home:
                    state.CursorPosition = 0;
                    return true;
               case EscapeKey::End:
                    state.CursorPosition = state.Line.size();
                    return true;
               case EscapeKey::Delete:
                    if (state.CursorPosition < state.Line.size())
                    {
                         state.Line.erase(state.CursorPosition, 1);
                         state.HistoryIndex = History.size();
                         return true;
                    }

                    return false;
               case EscapeKey::None:
                    return false;
          }

          return false;
     }

     bool ApplyPrintableCharacter(EditorState &state, char character)
     {
          if (!IsAsciiPrintable(character))
          {
               return false;
          }

          state.Line.insert(state.CursorPosition, 1, character);
          ++state.CursorPosition;
          state.HistoryIndex = History.size();

          return true;
     }

     bool ApplyCompletion(EditorState &state)
     {
          if (CompletionCallback == nullptr)
          {
               return false;
          }

          std::vector<char> completion_buffer(state.Line.size() + 256, '\0');

          if (completion_buffer.empty())
          {
               return false;
          }

          if (CompletionCallback(state.Line.c_str(), completion_buffer.data(), completion_buffer.size()) == 0)
          {
               return false;
          }

          state.Line.assign(completion_buffer.data());
          state.CursorPosition = state.Line.size();
          state.HistoryIndex = History.size();
          return true;
     }
}

/* Read one line, using raw mode when attached to a terminal. */

void linenoiseSetCompletionCallback(linenoiseCompletionCallback callback)
{
     CompletionCallback = callback;
}

char *linenoise(const char *prompt)
{
     LastReadWasControlD = false;
     LastReadWasInterrupted = false;

     if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
     {
          if (prompt != nullptr)
          {
               std::cout << prompt;
               std::cout.flush();
          }

          std::string line;

          if (!std::getline(std::cin, line))
          {
               return nullptr;
          }

          char *buffer = static_cast<char *>(std::malloc(line.size() + 1));

          if (buffer == nullptr)
          {
               return nullptr;
          }

          std::memcpy(buffer, line.c_str(), line.size() + 1);

          return buffer;
     }

     termios original;

     if (!EnableRawMode(original))
     {
          return nullptr;
     }

     RawModeGuard raw_mode_guard(original);

     if (prompt != nullptr)
     {
          std::cout << prompt;
          std::cout.flush();
     }

     EditorState state;
     state.HistoryIndex = History.size();
     LastRefreshRows = CountOccupiedRows(prompt != nullptr ? std::strlen(prompt) : 0, GetTerminalColumns());
     LastCursorRow = CountCursorRows(prompt != nullptr ? std::strlen(prompt) : 0, GetTerminalColumns());

     while (true)
     {
          char character = '\0';

          if (read(STDIN_FILENO, &character, 1) != 1)
          {
               raw_mode_guard.Restore();
               linenoiseResetState();
               return nullptr;
          }

          if (character == '\r' || character == '\n')
          {
               std::cout << "\r\n";
               break;
          }

          const EditorReadAction control_action = ApplyControlCharacter(state, character);

          if (control_action == EditorReadAction::Interrupted)
          {
               std::cout << "^C\r\n";
               raw_mode_guard.Restore();
               linenoiseResetState();
               LastReadWasInterrupted = true;
               return nullptr;
          }

          if (control_action == EditorReadAction::EndOfInput)
          {
               std::cout << "\r\n";
               raw_mode_guard.Restore();
               linenoiseResetState();
               LastReadWasControlD = true;
               return nullptr;
          }

          if (character == 4)
          {
               RefreshLine(prompt, state.Line, state.CursorPosition);
               continue;
          }

          if (character == '\t')
          {
               if (!ApplyCompletion(state))
               {
                    std::cout << '\a';
                    std::cout.flush();
               }

               RefreshLine(prompt, state.Line, state.CursorPosition);
               continue;
          }

          if ((character == 127 || character == '\b') && ApplyBackspace(state))
          {
               RefreshLine(prompt, state.Line, state.CursorPosition);
               continue;
          }

          if (character == 127 || character == '\b')
          {
               continue;
          }

          if (ApplyCursorControl(state, character))
          {
               RefreshLine(prompt, state.Line, state.CursorPosition);
               continue;
          }

          if (character == 27)
          {
               if (ApplyEscapeKey(state, ReadEscapeKey()))
               {
                    RefreshLine(prompt, state.Line, state.CursorPosition);
               }

               continue;
          }

          if (ApplyPrintableCharacter(state, character))
          {
               RefreshLine(prompt, state.Line, state.CursorPosition);
          }
     }

     raw_mode_guard.Restore();
     linenoiseResetState();

     char *buffer = static_cast<char *>(std::malloc(state.Line.size() + 1));

     if (buffer == nullptr)
     {
          return nullptr;
     }

     std::memcpy(buffer, state.Line.c_str(), state.Line.size() + 1);

     return buffer;
}

/* Return whether the previous read ended due to Ctrl-D. */

int linenoiseWasControlD(void)
{
     return LastReadWasControlD ? 1 : 0;
}

/* Return whether the previous read ended due to Ctrl-C. */

int linenoiseWasInterrupted(void)
{
     return LastReadWasInterrupted ? 1 : 0;
}

/* Reset redraw bookkeeping after the REPL prints command output. */

void linenoiseResetState(void)
{
     LastRefreshRows = 1;
     LastCursorRow = 0;
     LastReadWasControlD = false;
     LastReadWasInterrupted = false;
}

/* Add a non-empty command to history while suppressing duplicates. */

int linenoiseHistoryAdd(const char *line)
{
     if (line == nullptr || *line == '\0')
     {
          return 0;
     }

     if (!History.empty() && History.back() == line)
     {
          return 1;
     }

     History.emplace_back(line);
     TrimHistoryToLimit();

     return 1;
}

/* Load history lines from a newline-delimited file. */

int linenoiseHistoryLoad(const char *filename)
{
     if (filename == nullptr || *filename == '\0')
     {
          return -1;
     }

     std::ifstream input(filename);

     if (!input.is_open())
     {
          return -1;
     }

     std::string line;

     while (std::getline(input, line))
     {
          if (!line.empty())
          {
               History.push_back(line);
          }
     }

     TrimHistoryToLimit();

     return 0;
}

/* Save history lines to a newline-delimited file. */

int linenoiseHistorySave(const char *filename)
{
     if (filename == nullptr || *filename == '\0')
     {
          return -1;
     }

     std::ofstream output(filename);

     if (!output.is_open())
     {
          return -1;
     }

     for (const auto &line : History)
     {
          output << line << '\n';
     }

     return 0;
}

/* Release all cached history lines. */

void linenoiseHistoryFree(void)
{
     History.clear();
}
