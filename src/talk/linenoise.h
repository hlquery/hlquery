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

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

     typedef int (*linenoiseCompletionCallback)(const char *line, char *buffer, size_t buffer_size);

     /* Read one interactive line from the terminal.
 * Interactive editing is ASCII-only; non-interactive stdin is passed through unchanged.
 */

     char *linenoise(const char *prompt);

     /* Register an optional completion callback used when the user presses Tab. */

     void linenoiseSetCompletionCallback(linenoiseCompletionCallback callback);

     /* Report whether the last readline ended because the user pressed Ctrl-D. */

     int linenoiseWasControlD(void);

     /* Report whether the last readline ended because the user pressed Ctrl-C. */

     int linenoiseWasInterrupted(void);

     /* Reset prompt redraw bookkeeping after external command output. */

     void linenoiseResetState(void);

     /* Append one line to the in-memory history. */

     int linenoiseHistoryAdd(const char *line);

     /* Load history lines from disk. */

     int linenoiseHistoryLoad(const char *filename);

     /* Persist history lines to disk. */

     int linenoiseHistorySave(const char *filename);

     /* Release in-memory history storage. */

     void linenoiseHistoryFree(void);

#ifdef __cplusplus
}
#endif
