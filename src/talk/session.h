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

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

class HLQueryCLI;

/* Stores interactive shell state between commands.
 * This tracks the active collection and the last visible list results.
 */

struct TalkState
{
     std::string CurrentCollection;
     std::vector<std::string> CollectionHistory;
     std::vector<std::string> LastListedCollections;
     std::vector<std::string> LastListedDocumentIds;
     std::vector<std::string> LastListedSAMDocumentIds;
     std::unordered_map<std::string, std::string> CommandAliases;
};

/* Resolve the on-disk history path used by the talk shell. */

std::string ResolveHistoryFilePath();

/* Resolve the on-disk aliases path used by the talk shell. */

std::string ResolveAliasesFilePath();

/* Load persisted command aliases into TalkState (best-effort). */

void LoadTalkAliases(TalkState &state);

/* Persist command aliases from TalkState to disk (best-effort). */

void SaveTalkAliases(const TalkState &state);

/* Trim leading and trailing ASCII whitespace. */

std::string TrimWhitespace(const std::string &value);

/* Normalize a host value before validation and connection setup. */

std::string NormalizeHostValue(const std::string &value);

/* Returns whether the supplied port number is valid for TCP use. */

bool IsValidPortValue(int port);

/* Returns whether the supplied host value is a valid IP address or hostname. */

bool IsValidHostValue(const std::string &value);

/* Print the available interactive shell commands. */

void PrintHelp();

/* Return the top-level talk commands used for help and prompt completion. */

std::vector<std::string> GetTalkCommands();

/* Complete the current talk prompt line when the user presses Tab. */

int CompleteTalkCommandLine(const char *line, char *buffer, size_t buffer_size);

/* Build the prompt shown for the current talk shell state. */

std::string BuildPrompt(const std::string &host, const std::string &port, const TalkState &state);

/* Execute one talk shell command line.
 * Returns false when the caller should stop the interactive loop.
 */

bool ExecuteTalkCommand(const std::string &line,
                        HLQueryCLI &cli,
                        TalkState &state,
                        std::string &host,
                        std::string &port);
