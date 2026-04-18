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
#include <limits>
#include <sstream>
#include <string>

#include "../core/config.h"

enum class WALEntryValidationError
{
     None = 0,
     SizeOverflow,
     EntryTooLarge
};

struct WALEntryValidationResult
{
     bool Valid = true;
     WALEntryValidationError Error = WALEntryValidationError::None;
     size_t KeyLength = 0;
     size_t ValueLength = 0;
     size_t TotalLength = 0;
     size_t MaxAllowedLength = 0;
     bool IsRecoveryMode = false;
};

inline const char* WALEntryValidationErrorCode(WALEntryValidationError error)
{
     switch (error)
     {
          case WALEntryValidationError::None:
               return "none";
          case WALEntryValidationError::SizeOverflow:
               return "wal_entry_size_overflow";
          case WALEntryValidationError::EntryTooLarge:
               return "wal_entry_too_large";
     }

     return "unknown_wal_validation_error";
}

inline std::string WALEntryValidationMessage(const WALEntryValidationResult& result)
{
     if (result.Valid)
     {
          return "wal entry validation passed";
     }

     std::ostringstream message;
     message << "WAL entry rejected"
             << " code=" << WALEntryValidationErrorCode(result.Error)
             << " key_len=" << result.KeyLength
             << " value_len=" << result.ValueLength
             << " entry_len=" << result.TotalLength
             << " max_len=" << result.MaxAllowedLength
             << " mode=" << (result.IsRecoveryMode ? "recovery" : "normal");
     return message.str();
}

inline WALEntryValidationResult ValidateWALEntrySize(size_t key_len, size_t value_len, bool is_recovery)
{
     WALEntryValidationResult result;

     result.KeyLength = key_len;
     result.ValueLength = value_len;
     result.MaxAllowedLength = MAX_WAL_ENTRY_SIZE_FOR_MODE(is_recovery);
     result.IsRecoveryMode = is_recovery;

     if (key_len > (std::numeric_limits<size_t>::max() - value_len))
     {
          result.Valid = false;
          result.Error = WALEntryValidationError::SizeOverflow;
          result.TotalLength = std::numeric_limits<size_t>::max();
          return result;
     }

     result.TotalLength = key_len + value_len;

     if (result.TotalLength > result.MaxAllowedLength)
     {
          result.Valid = false;
          result.Error = WALEntryValidationError::EntryTooLarge;
     }

     return result;
}
