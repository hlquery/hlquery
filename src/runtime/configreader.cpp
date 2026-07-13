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

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "runtime/configreader.h"
#include "core/hlquery.h"
#include "utils/consolewriter.h"
#include "utils/infos.h"

/* ConfigReader Constructor */

ConfigReader::ConfigReader() : Valid(false),
                               IncludeDepth(0)
{
}

/* ConfigReader Destructor */

ConfigReader::~ConfigReader()
{
}

/* Helper function to format file size in human-readable format */

static std::string FormatFileSize(size_t Bytes)
{
     const char *Units[] = {"B", "KB", "MB", "GB", "TB"};

     size_t UnitIndex = 0;

     double Size = static_cast<double>(Bytes);

     while (Size >= 1024.0 && UnitIndex < 4)
     {
          Size /= 1024.0;
          UnitIndex++;
     }

     char Buffer[64];

     if (UnitIndex == 0 || Size >= 100.0)
     {
          snprintf(Buffer, sizeof(Buffer), "%.0f %s", Size, Units[UnitIndex]);
     }
     else
     {
          snprintf(Buffer, sizeof(Buffer), "%.2f %s", Size, Units[UnitIndex]);
     }

     return std::string(Buffer);
}

/* Helper function to format file permissions in readable format */

static std::string FormatPermissions(std::filesystem::perms Perms)
{
     std::string Result;

     auto AddPerm = [&](const char *Name, std::filesystem::perms Mask)
     {
          if ((Perms & Mask) != std::filesystem::perms::none)
          {
               if (!Result.empty())
               {
                    Result += ", ";
               }

               Result += Name;
          }
     };

     AddPerm("owner_read", std::filesystem::perms::owner_read);
     AddPerm("owner_write", std::filesystem::perms::owner_write);
     AddPerm("owner_exec", std::filesystem::perms::owner_exec);
     AddPerm("group_read", std::filesystem::perms::group_read);
     AddPerm("group_write", std::filesystem::perms::group_write);
     AddPerm("group_exec", std::filesystem::perms::group_exec);
     AddPerm("others_read", std::filesystem::perms::others_read);
     AddPerm("others_write", std::filesystem::perms::others_write);
     AddPerm("others_exec", std::filesystem::perms::others_exec);

     return Result.empty() ? "no permissions" : Result;
}

/* Helper function to suggest similar file paths (typo detection) */

static std::string SuggestSimilarPath(const std::string &Filename)
{
     std::filesystem::path Path(Filename);

     std::filesystem::path Parent = Path.parent_path();

     std::string Basename = Path.filename().string();

     if (!std::filesystem::exists(Parent) || !std::filesystem::is_directory(Parent))
     {
          return "";
     }

     std::vector<std::string> Suggestions;

     try
     {
          for (const auto &Entry : std::filesystem::directory_iterator(Parent))
          {
               if (Entry.is_regular_file())
               {
                    std::string Candidate = Entry.path().filename().string();

                    /* Simple similarity check: same length or one char difference */

                    if (Candidate.length() == Basename.length() ||
                        std::abs(static_cast<int>(Candidate.length()) - static_cast<int>(Basename.length())) <= 1)
                    {
                         /* Count character differences */

                         size_t Diff = 0;

                         size_t MinLen = std::min(Candidate.length(), Basename.length());

                         for (size_t i = 0; i < MinLen; i++)
                         {
                              if (Candidate[i] != Basename[i])
                              {
                                   Diff++;
                              }
                         }

                         Diff += std::max(Candidate.length(), Basename.length()) - MinLen;

                         if (Diff <= 2 && Diff > 0)
                         {
                              Suggestions.push_back(Entry.path().string());
                         }
                    }
               }
          }
     }
     catch (...)
     {
          /* Ignore errors when scanning directory */
     }

     if (Suggestions.empty())
     {
          return "";
     }

     std::string Result = "\n  Did you mean one of these?";

     for (size_t i = 0; i < Suggestions.size() && i < 3; i++)
     {
          Result += "\n    - " + Suggestions[i];
     }

     return Result;
}

/* Load and parse configuration file */

bool ConfigReader::LoadFile(const std::string &Filename)
{
     FileName = Filename;

     Valid = false;

     ErrorMsg.clear();

     Tags.clear();

     ActiveIncludes.clear();

     IncludeDepth = 0;

     /* Verify that the configuration file is present before continuing */

     if (!std::filesystem::exists(Filename))
     {
          std::filesystem::path Path(Filename);

          std::filesystem::path Parent = Path.parent_path();

          ErrorMsg = "Configuration file not found";
          ErrorMsg += "\n  File: " + Filename;

          ConsoleWriter::WriteError("Configuration file not found: " + Filename + ".");

          /* Provide helpful context about why the file might not exist */

          if (!Parent.empty() && !std::filesystem::exists(Parent))
          {
               ErrorMsg += "\n  Directory does not exist: " + Parent.string();
               ErrorMsg += "\n  Solution: Create the directory with: mkdir -p \"" + Parent.string() + "\"";
               ConsoleWriter::WriteError("Directory does not exist: " + Parent.string() + ".");
               ConsoleWriter::WriteInfo("Solution: Create the directory with: mkdir -p \"" + Parent.string() + "\".");
          }
          else if (!Parent.empty())
          {
               ErrorMsg += "\n  Directory exists: " + Parent.string();
               ErrorMsg += "\n  File is missing";
               ErrorMsg += "\n  Check:";
               ErrorMsg += "\n     - Is the filename spelled correctly?";
               ErrorMsg += "\n     - Does the file need to be created?";
               ConsoleWriter::WriteInfo("Directory exists: " + Parent.string() + ".");
               ConsoleWriter::WriteError("File is missing.");
               ConsoleWriter::WriteInfo("Check: Is the filename spelled correctly? Does the file need to be created?");

               /* Suggest similar files if directory exists */

               std::string Suggestions = SuggestSimilarPath(Filename);

               if (!Suggestions.empty())
               {
                    ErrorMsg += Suggestions;
                    ConsoleWriter::WriteInfo(Suggestions + ".");
               }
          }
          else
          {
               ErrorMsg += "\n  Check:";
               ErrorMsg += "\n     - Is the path correct?";
               ErrorMsg += "\n     - Use absolute path if relative path fails";
               ConsoleWriter::WriteInfo("Check: Is the path correct? Use absolute path if relative path fails.");
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Check if path is a directory instead of a file */

     if (std::filesystem::is_directory(Filename))
     {
          ErrorMsg = "Configuration path is a directory, not a file";
          ErrorMsg += "\n  Path: " + Filename;
          ErrorMsg += "\n  Solution: Specify the full path to a configuration file";
          ErrorMsg += "\n     Example: " + Filename + "/hlquery.conf";
          ConsoleWriter::WriteError("Configuration path is a directory, not a file: " + Filename + ".");
          ConsoleWriter::WriteInfo("Solution: Specify the full path to a configuration file.");
          ConsoleWriter::WriteInfo("Example: " + Filename + "/hlquery.conf.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Enforce a reasonable size limit to guard against corrupted or hostile configs */

     const size_t MAX_CONFIG_SIZE = CONFIG_READER_MAX_CONFIG_SIZE;

     size_t FileSize;

     try
     {
          FileSize = std::filesystem::file_size(Filename);
     }
     catch (const std::filesystem::filesystem_error &e)
     {
          ErrorMsg = "Cannot read configuration file size";
          ErrorMsg += "\n  File: " + Filename;
          ErrorMsg += "\n  Error: " + std::string(e.what());
          ErrorMsg += "\n  Possible causes:";
          ErrorMsg += "\n     - File permissions prevent reading";
          ErrorMsg += "\n     - File is a special file (device, pipe, socket)";
          ErrorMsg += "\n     - Filesystem error or disk issue";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Check permissions: ls -l \"" + Filename + "\"";
          ErrorMsg += "\n     - Fix permissions: chmod 644 \"" + Filename + "\"";
          ErrorMsg += "\n     - Verify file is a regular file, not a directory or special file";
          ConsoleWriter::WriteError("Cannot read configuration file size: " + Filename + ".");
          ConsoleWriter::WriteError("Error: " + std::string(e.what()) + ".");
          ConsoleWriter::WriteWarning("Possible causes: File permissions prevent reading, file is a special file, or filesystem error.");
          ConsoleWriter::WriteInfo("Solutions: Check permissions with 'ls -l', fix with 'chmod 644', verify file is regular.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     if (FileSize > MAX_CONFIG_SIZE)
     {
          ErrorMsg = "Configuration file too large";
          ErrorMsg += "\n  File: " + Filename;
          ErrorMsg += "\n  Size: " + FormatFileSize(FileSize) + " (max: " + FormatFileSize(MAX_CONFIG_SIZE) + ")";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Split configuration into multiple files using <include file=\"...\">";
          ErrorMsg += "\n     - Remove unnecessary comments or whitespace";
          ErrorMsg += "\n     - Move large data sections to external files";
          ConsoleWriter::WriteError("Configuration file too large: " + FormatFileSize(FileSize) + " (max: " + FormatFileSize(MAX_CONFIG_SIZE) + ").");
          ConsoleWriter::WriteInfo("Solutions: Split into multiple files using <include>, remove unnecessary content, move large data to external files.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Check if file is empty */

     if (FileSize == 0)
     {
          ErrorMsg = "Configuration file is empty";
          ErrorMsg += "\n  File: " + Filename;
          ErrorMsg += "\n  Solution: Add configuration content to the file";
          ErrorMsg += "\n     Minimum required: at least one <bind ...> listener in hlquery.conf";
          ConsoleWriter::WriteError("Configuration file is empty: " + Filename + ".");
          ConsoleWriter::WriteInfo("Solution: Add configuration content. Minimum required: at least one <bind ...> listener in hlquery.conf.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     std::ifstream File(Filename);

     if (!File.is_open())
     {
          ErrorMsg = "Cannot open configuration file";
          ErrorMsg += "\n  File: " + Filename;

          ConsoleWriter::WriteError("Cannot open configuration file: " + Filename + ".");

          /* Provide more context about why opening failed */

          std::error_code Ec;

          auto Status = std::filesystem::status(Filename, Ec);

          if (!Ec)
          {
               auto Perms = Status.permissions();

               std::string PermStr = FormatPermissions(Perms);

               ErrorMsg += "\n  Permissions: " + PermStr;

               bool Readable = (Perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none ||
                               (Perms & std::filesystem::perms::group_read) != std::filesystem::perms::none ||
                               (Perms & std::filesystem::perms::others_read) != std::filesystem::perms::none;

               if (!Readable)
               {
                    ErrorMsg += "\n  File is not readable";
                    ErrorMsg += "\n  Solution: Fix permissions with: chmod 644 \"" + Filename + "\"";
                    ConsoleWriter::WriteError("File is not readable.");
                    ConsoleWriter::WriteInfo("Solution: Fix permissions with: chmod 644 \"" + Filename + "\".");
               }
               else
               {
                    ErrorMsg += "\n  File appears readable but still cannot be opened";
                    ErrorMsg += "\n  Possible causes:";
                    ErrorMsg += "\n     - File is locked by another process";
                    ErrorMsg += "\n     - Disk is full or filesystem error";
                    ErrorMsg += "\n     - SELinux/AppArmor restrictions";
                    ConsoleWriter::WriteWarning("File appears readable but still cannot be opened.");
                    ConsoleWriter::WriteInfo("Possible causes: File locked, disk full, or SELinux/AppArmor restrictions.");
               }
          }
          else
          {
               ErrorMsg += "\n  Cannot determine file status: " + Ec.message();
               ConsoleWriter::WriteError("Cannot determine file status: " + Ec.message() + ".");
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     std::string Content;

     std::string Line;

     /* Read entire file into a string for processing */

     while (std::getline(File, Line))
     {
          Content += Line + "\n";
     }

     /* Check if read failed or encountered errors */

     if (File.bad())
     {
          ErrorMsg = "Error reading configuration file";
          ErrorMsg += "\n  File: " + Filename;
          ErrorMsg += "\n  Size: " + FormatFileSize(FileSize);
          ErrorMsg += "\n  File read operation failed";
          ErrorMsg += "\n  Possible causes:";
          ErrorMsg += "\n     - File is corrupted or incomplete";
          ErrorMsg += "\n     - Disk I/O error occurred";
          ErrorMsg += "\n     - File was removed during read";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Check disk health: df -h and dmesg";
          ErrorMsg += "\n     - Verify file integrity";
          ErrorMsg += "\n     - Restore from backup if available";
          ConsoleWriter::WriteError("Error reading configuration file: " + Filename + ".");
          ConsoleWriter::WriteError("File read operation failed.");
          ConsoleWriter::WriteWarning("Possible causes: File corrupted, disk I/O error, or file removed during read.");
          ConsoleWriter::WriteInfo("Solutions: Check disk health, verify file integrity, restore from backup.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Verify we actually read some content */

     if (Content.empty() && FileSize > 0)
     {
          ErrorMsg = "Configuration file appears empty after reading";
          ErrorMsg += "\n  File: " + Filename;
          ErrorMsg += "\n  Expected size: " + FormatFileSize(FileSize);
          ErrorMsg += "\n  Bytes read: 0";
          ErrorMsg += "\n  Possible causes:";
          ErrorMsg += "\n     - File contains only whitespace or control characters";
          ErrorMsg += "\n     - File encoding issue (expected UTF-8)";
          ErrorMsg += "\n     - File is a special file (device, pipe, etc.)";
          ErrorMsg += "\n  Solution: Ensure file contains readable text content";
          ConsoleWriter::WriteError("Configuration file appears empty after reading: " + Filename + ".");
          ConsoleWriter::WriteWarning("Possible causes: Only whitespace/control characters, encoding issue, or special file.");
          ConsoleWriter::WriteInfo("Solution: Ensure file contains readable text content.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Parse the content (handles comments, includes, and tag parsing) */

     std::string ProcessedContent;
     std::filesystem::path ConfigPath(Filename);
     std::string ConfigCanonicalPath;

     try
     {
          ConfigPath = std::filesystem::canonical(ConfigPath);
          ConfigCanonicalPath = ConfigPath.string();
     }
     catch (const std::exception &)
     {
          std::error_code Ec;
          ConfigPath = std::filesystem::absolute(ConfigPath, Ec);
          ConfigCanonicalPath = ConfigPath.string();
     }

     ActiveIncludes.insert(ConfigCanonicalPath);

     if (!ProcessCommentsAndIncludes(Content, ProcessedContent, ConfigPath.parent_path().string(), ConfigCanonicalPath))
     {
          ActiveIncludes.erase(ConfigCanonicalPath);
          ConsoleWriter::WriteError("Failed to process configuration content: " + Filename + ".");
          return false;
     }

     ActiveIncludes.erase(ConfigCanonicalPath);

     if (!ParseContent(ProcessedContent))
     {
          /* ParseContent sets ErrorMsg with specific details */

          if (ErrorMsg.empty())
          {
               ErrorMsg = "Failed to parse configuration file";
               ErrorMsg += "\n  File: " + Filename;
               ErrorMsg += "\n  Check that the file contains valid XML-like tags and attributes";
               ErrorMsg += "\n     Example format: <tagname key=\"value\" key2=\"value2\"></tagname>";
               ConsoleWriter::WriteError("Failed to parse configuration file: " + Filename + ".");
               ConsoleWriter::WriteInfo("Check that the file contains valid XML-like tags and attributes.");
          }
          else
          {
               /* Add file context to existing error */

               ErrorMsg = "Parse error in configuration file\n  File: " + Filename + "\n" + ErrorMsg;
               ConsoleWriter::WriteError("Parse error in configuration file: " + Filename + ".");
          }

          return false;
     }

     /* Confirm that all required configuration tags are available */

     if (!ValidateRequiredTags())
     {
          return false;
     }

     /* Debug: Log how many tags were parsed */

     if (Instance && Instance->Config && Instance->Config->GetDebugMode())
     {
          ConsoleWriter::WriteDebug("ConfigReader::LoadFile: Parsed " + std::to_string(Tags.size()) + " unique tag types from " + Filename + ".");
     }

     Valid = true;

     if (Instance && Instance->Logs)
     {
          Instance->Logs->Normal("configreader", "Successfully loaded configuration.");
     }

     return true;
}

/* Parse configuration content */

bool ConfigReader::ParseContent(const std::string &Content)
{
     const std::string &ProcessedContent = Content;

     /* Protect against excessive tag count (memory exhaustion) */

     const size_t MAX_TAGS = CONFIG_READER_MAX_TAGS;

     size_t TotalTags = 0;

     /* Use regex to find all tags in the format <tagname attr="value" attr2="value2"> */

     std::regex TagRegex(R"(<\s*(\w+)([^>]*)>)");

     std::sregex_iterator Begin(ProcessedContent.begin(), ProcessedContent.end(), TagRegex);

     std::sregex_iterator End;

     for (auto it = Begin; it != End; ++it)
     {
          const std::smatch &Match = *it;

          std::string TagName = Match[1].str();

          std::string AttributesStr = Match[2].str();

          /* Validate tag name */

          if (!ValidateTagName(TagName))
          {
               ErrorMsg = "Invalid tag name: '" + TagName + "'";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Validation failed:";

               if (TagName.empty())
               {
                    ErrorMsg += "\n     - Tag name is empty";
               }
               else if (TagName.length() > 64)
               {
                    ErrorMsg += "\n     - Tag name too long: " + std::to_string(TagName.length()) + " characters (max: 64)";
               }
               else
               {
                    ErrorMsg += "\n     - Contains invalid characters";
               }

               ErrorMsg += "\n  Rules:";
               ErrorMsg += "\n     - Tag names must contain only alphanumeric characters (a-z, A-Z, 0-9) and underscores (_)";
               ErrorMsg += "\n     - Tag names must be 64 characters or less";
               ErrorMsg += "\n     - Examples: <server>, <database>, <log_config>";
               ConsoleWriter::WriteError("Invalid tag name: '" + TagName + "' in file: " + FileName + ".");
               ConsoleWriter::WriteInfo("Rules: Tag names must contain only alphanumeric characters and underscores, max 64 characters.");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }

          /* Create new tag */

          auto Tag = std::make_shared<ConfigTag>(TagName);

          /* Parse attributes */

          if (!ParseAttributes(AttributesStr, Tag))
          {
               if (ErrorMsg.empty())
               {
                    ErrorMsg = "Failed to parse attributes for tag: '" + TagName + "'";
                    ErrorMsg += "\n  File: " + FileName;
                    ErrorMsg += "\n  Check attribute format:";
                    ErrorMsg += "\n     - Correct: <tag key=\"value\" key2=\"value2\"></tag>";
                    ErrorMsg += "\n     - Wrong:   <tag key=value></tag> (missing quotes)";
                    ErrorMsg += "\n     - Wrong:   <tag key=\"value key2=\"value2\"></tag> (unclosed quote)";
                    ErrorMsg += "\n  Ensure all attribute values are properly quoted with double quotes";
                    ConsoleWriter::WriteError("Failed to parse attributes for tag: '" + TagName + "' in file: " + FileName + ".");
                    ConsoleWriter::WriteInfo("Check attribute format: All attribute values must be properly quoted with double quotes.");
               }
               else
               {
                    /* Add tag and file context to existing error */

                    ErrorMsg = "Error in tag '" + TagName + "'\n  File: " + FileName + "\n" + ErrorMsg;
                    ConsoleWriter::WriteError("Error in tag '" + TagName + "' in file: " + FileName + ".");
               }

               return false;
          }

          /* Check for excessive tag count */

          TotalTags++;

          if (TotalTags > MAX_TAGS)
          {
               ErrorMsg = "Too many tags in configuration file";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Found: " + std::to_string(TotalTags) + " tags (max: " + std::to_string(MAX_TAGS) + ")";
               ErrorMsg += "\n  Solutions:";
               ErrorMsg += "\n     - Split configuration into multiple files:";
               ErrorMsg += "\n       <include file=\"database.conf\"></include>";
               ErrorMsg += "\n       <include file=\"logging.conf\"></include>";
               ErrorMsg += "\n     - Remove duplicate or unnecessary tags";
               ErrorMsg += "\n     - Consolidate similar tags";
               ConsoleWriter::WriteError("Too many tags in configuration file: " + std::to_string(TotalTags) + " (max: " + std::to_string(MAX_TAGS) + ").");
               ConsoleWriter::WriteInfo("Solutions: Split into multiple files using <include>, remove duplicates, consolidate similar tags.");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }

          /* Store tag */

          Tags[TagName].push_back(Tag);
     }

     return true;
}

/* Parse attributes from a tag's attribute string */

bool ConfigReader::ParseAttributes(const std::string &AttrStr, std::shared_ptr<ConfigTag> Tag)
{
     std::regex AttrRegex(R"((\w+)\s*=\s*\"([^\"]*)\")");

     std::sregex_iterator Begin(AttrStr.begin(), AttrStr.end(), AttrRegex);

     std::sregex_iterator End;

     /* Security limits to prevent memory exhaustion */

     const size_t MAX_ATTRIBUTES = CONFIG_READER_MAX_ATTRIBUTES;

     const size_t MAX_ATTR_LENGTH = CONFIG_READER_MAX_ATTR_LENGTH;

     size_t AttrCount = 0;

     size_t LastPos = 0;

     for (auto it = Begin; it != End; ++it)
     {
          const std::smatch &Match = *it;

          size_t MatchPos = static_cast<size_t>(Match.position());
          size_t MatchLen = static_cast<size_t>(Match.length());
          size_t GapLen = MatchPos > LastPos ? MatchPos - LastPos : 0;
          std::string Gap = TrimWhitespace(AttrStr.substr(LastPos, GapLen));

          if (!Gap.empty())
          {
               ErrorMsg = "Malformed attribute syntax in tag '" + Tag->GetName() + "'";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Attribute chunk: '" + Gap + "'";
               ErrorMsg += "\n  Check attribute format: <tag key=\"value\">";
               ConsoleWriter::WriteError("Malformed attribute syntax in tag '" + Tag->GetName() + "'.");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }

          LastPos = MatchPos + MatchLen;

          std::string Key = Match[1].str();

          std::string Value = Match[2].str();

          /* Validate attribute count */

          AttrCount++;

          if (AttrCount > MAX_ATTRIBUTES)
          {
               ErrorMsg = "Too many attributes in tag '" + Tag->GetName() + "'";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Found: " + std::to_string(AttrCount) + " attributes (max: " + std::to_string(MAX_ATTRIBUTES) + ")";
               ErrorMsg += "\n  Solutions:";
               ErrorMsg += "\n     - Split into multiple tags with fewer attributes";
               ErrorMsg += "\n     - Move some attributes to a separate include file";
               ErrorMsg += "\n     - Use nested configuration structure if supported";
               ConsoleWriter::WriteError("Too many attributes in tag '" + Tag->GetName() + "': " + std::to_string(AttrCount) + " (max: " + std::to_string(MAX_ATTRIBUTES) + ").");
               ConsoleWriter::WriteInfo("Solutions: Split into multiple tags, move to include file, or use nested structure.");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }

          /* Validate attribute value length */

          if (Value.length() > MAX_ATTR_LENGTH)
          {
               ErrorMsg = "Attribute value too long";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Tag: '" + Tag->GetName() + "'";
               ErrorMsg += "\n  Attribute: '" + Key + "'";
               ErrorMsg += "\n  Length: " + FormatFileSize(Value.length()) + " (max: " + FormatFileSize(MAX_ATTR_LENGTH) + ")";
               ErrorMsg += "\n  Solutions:";
               ErrorMsg += "\n     - Store large values in external files and reference them";
               ErrorMsg += "\n     - Split the value across multiple attributes";
               ErrorMsg += "\n     - Use <include> directive for large configuration blocks";
               ConsoleWriter::WriteError("Attribute value too long in tag '" + Tag->GetName() + "' attribute '" + Key + "': " + FormatFileSize(Value.length()) + " (max: " + FormatFileSize(MAX_ATTR_LENGTH) + ").");
               ConsoleWriter::WriteInfo("Solutions: Store in external files, split across attributes, or use <include> directive.");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }

          /* Validate attribute value */

          if (!ValidateAttributeValue(Key, Value, Tag->GetName()))
          {
               return false;
          }

          Tag->SetAttribute(Key, Value);
     }

     std::string Remaining = TrimWhitespace(AttrStr.substr(LastPos));

     if (!Remaining.empty())
     {
          ErrorMsg = "Malformed attribute syntax in tag '" + Tag->GetName() + "'";
          ErrorMsg += "\n  File: " + FileName;
          ErrorMsg += "\n  Trailing content: '" + Remaining + "'";
          ErrorMsg += "\n  Check attribute format: <tag key=\"value\">";
          ConsoleWriter::WriteError("Malformed attribute syntax in tag '" + Tag->GetName() + "'.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     return true;
}

/* Helper function to trim whitespace from a string */

std::string ConfigReader::TrimWhitespace(const std::string &Str)
{
     size_t Start = Str.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     size_t End = Str.find_last_not_of(" \t\r\n");

     return Str.substr(Start, End - Start + 1);
}

/* Get all tags with specified name */

std::vector<std::shared_ptr<ConfigTag>> ConfigReader::GetTags(const std::string &TagName) const
{
     auto it = Tags.find(TagName);

     if (it != Tags.end())
     {
          return it->second;
     }

     return {};
}

/* Get first tag with specified name */

std::shared_ptr<ConfigTag> ConfigReader::GetTag(const std::string &TagName) const
{
     auto tags = GetTags(TagName);

     if (!tags.empty())
     {
          return tags[0];
     }

     return nullptr;
}

/* ConfigTag implementation */

std::string ConfigTag::GetString(const std::string &Key, const std::string &DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          return it->second;
     }

     return DefaultValue;
}

int ConfigTag::GetInt(const std::string &Key, int DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          try
          {
               size_t Parsed = 0;
               long long Value = std::stoll(it->second, &Parsed);

               if (Parsed != it->second.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Integer attribute '" + Key + "' for tag '" + Name + "' contains extra characters: '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Guard against integer overflow before casting */

               if (Value > INT_MAX || Value < INT_MIN)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Integer value out of range for tag '" + Name + "' attribute '" + Key + "': " + it->second + " (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               return static_cast<int>(Value);
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("configreader", "WARNING: Invalid integer value for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
               }

               return DefaultValue;
          }
     }

     return DefaultValue;
}

bool ConfigTag::GetBool(const std::string &Key, bool DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          std::string Value = it->second;

          std::transform(Value.begin(), Value.end(), Value.begin(),
                         [](unsigned char C)
                         {
                              return static_cast<char>(std::tolower(C));
                         });

          if (Value == "true" || Value == "yes" || Value == "1" || Value == "on" || Value == "enabled")
          {
               return true;
          }

          if (Value == "false" || Value == "no" || Value == "0" || Value == "off" || Value == "disabled")
          {
               return false;
          }

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("configreader", "WARNING: Invalid boolean value for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::string(DefaultValue ? "true" : "false") + ").");
          }

          return DefaultValue;
     }

     return DefaultValue;
}

double ConfigTag::GetDouble(const std::string &Key, double DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          try
          {
               size_t Parsed = 0;
               double Value = std::stod(it->second, &Parsed);

               if (Parsed != it->second.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Double attribute '" + Key + "' for tag '" + Name + "' contains trailing characters: '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Reject NaN or infinity to keep math sane */

               if (std::isnan(Value) || std::isinf(Value))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Invalid double value (NaN/Infinity) for tag '" + Name + "' attribute '" + Key + "' (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               return Value;
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("configreader", "WARNING: Invalid double value for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
               }

               return DefaultValue;
          }
     }

     return DefaultValue;
}

bool ConfigTag::HasAttribute(const std::string &Key) const
{
     return Attributes.find(Key) != Attributes.end();
}

void ConfigTag::SetAttribute(const std::string &Key, const std::string &Value)
{
     Attributes[Key] = Value;
}

bool ConfigReader::ProcessCommentsAndIncludes(const std::string &Content,
                                              std::string &out_content,
                                              const std::string &BaseDir,
                                              const std::string &SourceFile)
{
     std::istringstream Iss(Content);

     std::ostringstream Result;

     std::string Line;

     static const std::regex IncludeRegex(R"(<\s*include\s+file\s*=\s*[\"']([^\"']+)[\"']\s*>)");

     while (std::getline(Iss, Line))
     {
          /*
           * Remove all # comments (everything after #)
           * Logic updated to ignore # inside quoted strings (Fix for Bug 1)
           */

          bool InQuotes = false;

          size_t CommentPos = std::string::npos;

          for (size_t i = 0; i < Line.length(); ++i)
          {
               if (Line[i] == '\"')
               {
                    /* Check if quote is escaped */

                    if (i == 0 || Line[i - 1] != '\\')
                    {
                         InQuotes = !InQuotes;
                    }
               }
               else if (Line[i] == '#' && !InQuotes)
               {
                    CommentPos = i;
                    break;
               }
          }

          if (CommentPos != std::string::npos)
          {
               Line = Line.substr(0, CommentPos);
          }

          /* Trim whitespace after comment removal */

          Line = TrimWhitespace(Line);

          /* Skip empty lines */

          if (Line.empty())
          {
               continue;
          }

          std::smatch IncludeMatch;

          if (std::regex_search(Line, IncludeMatch, IncludeRegex))
          {
               std::string IncludeFileStr = IncludeMatch[1].str();

               std::string IncludeContent;
               std::string IncludeCanonicalPath;

               if (!LoadIncludeFile(IncludeFileStr, IncludeContent, BaseDir, IncludeCanonicalPath))
               {
                    return false;
               }

               if (ActiveIncludes.find(IncludeCanonicalPath) != ActiveIncludes.end())
               {
                    ErrorMsg = "Circular include detected";
                    ErrorMsg += "\n  File: " + IncludeCanonicalPath;
                    ErrorMsg += "\n  Requested from: " + SourceFile;
                    ErrorMsg += "\n  This file is already being processed in the include chain";
                    ErrorMsg += "\n  Solution: Remove the circular dependency";
                    ConsoleWriter::WriteError("Circular include detected: " + IncludeCanonicalPath + ".");
                    ConsoleWriter::WriteInfo("Solution: Remove the circular dependency or restructure the configuration.");

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("configreader", ErrorMsg + ".");
                    }

                    return false;
               }

               if (IncludeDepth + 1 > CONFIG_READER_MAX_INCLUDE_DEPTH)
               {
                    ErrorMsg = "Maximum include depth exceeded";
                    ErrorMsg += "\n  Current depth: " + std::to_string(IncludeDepth + 1) + " (max: " + std::to_string(CONFIG_READER_MAX_INCLUDE_DEPTH) + ")";
                    ErrorMsg += "\n  Include file: " + IncludeCanonicalPath;
                    ErrorMsg += "\n  Requested from: " + SourceFile;
                    ErrorMsg += "\n  This usually indicates circular includes in your configuration";
                    ErrorMsg += "\n  Solutions:";
                    ErrorMsg += "\n     - Check for files that include each other (A includes B, B includes A)";
                    ErrorMsg += "\n     - Remove circular include dependencies";
                    ErrorMsg += "\n     - Restructure configuration to avoid deep nesting";
                    ConsoleWriter::WriteError("Maximum include depth exceeded. This usually indicates circular includes.");
                    ConsoleWriter::WriteInfo("Solutions: Check for circular includes, remove dependencies, restructure configuration.");

                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("configreader", ErrorMsg + ".");
                    }

                    return false;
               }

               struct ActiveIncludeScope
               {
                    ConfigReader *Reader;
                    std::string Path;

                    ActiveIncludeScope(ConfigReader *ReaderPtr, const std::string &PathValue)
                        : Reader(ReaderPtr), Path(PathValue)
                    {
                         Reader->ActiveIncludes.insert(Path);
                         Reader->IncludeDepth++;
                    }

                    ~ActiveIncludeScope()
                    {
                         Reader->ActiveIncludes.erase(Path);
                         Reader->IncludeDepth--;
                    }
               };

               ActiveIncludeScope Scope(this, IncludeCanonicalPath);
               std::string ProcessedInclude;
               std::filesystem::path IncludePath(IncludeCanonicalPath);

               if (!ProcessCommentsAndIncludes(IncludeContent,
                                               ProcessedInclude,
                                               IncludePath.parent_path().string(),
                                               IncludeCanonicalPath))
               {
                    return false;
               }

               Result << ProcessedInclude << "\n";
          }
          else
          {
               /* Add non-include lines to result */

               Result << Line << "\n";
          }
     }

     out_content = Result.str();

     return true;
}

bool ConfigReader::LoadIncludeFile(const std::string &Filename,
                                   std::string &out_content,
                                   const std::string &BaseDir,
                                   std::string &CanonicalPath)
{
     /* Make include file path relative to the file that requested it. */

     std::filesystem::path IncludePath(Filename);

     if (!IncludePath.is_absolute())
     {
          IncludePath = std::filesystem::path(BaseDir) / IncludePath;
     }

     /* Normalize the path so circular includes can be detected reliably */

     try
     {
          CanonicalPath = std::filesystem::canonical(IncludePath).string();
     }
     catch (const std::filesystem::filesystem_error &e)
     {
          ErrorMsg = "Cannot resolve include file path";
          ErrorMsg += "\n  Include file: " + IncludePath.string();
          ErrorMsg += "\n  Requested from: " + BaseDir;
          ErrorMsg += "\n  Error: " + std::string(e.what());
          ErrorMsg += "\n  Check:";
          ErrorMsg += "\n     - Include file exists: ls -l \"" + IncludePath.string() + "\"";
          ErrorMsg += "\n     - Path is relative to config file directory: " + BaseDir;
          ErrorMsg += "\n     - Use absolute path if relative path fails";
          ErrorMsg += "\n     - Check for typos in the include filename";
          ConsoleWriter::WriteError("Cannot resolve include file path: " + IncludePath.string() + ".");
          ConsoleWriter::WriteInfo("Check: Include file exists, path is relative to config directory, check for typos.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }
     catch (const std::exception &e)
     {
          ErrorMsg = "Cannot resolve include file path";
          ErrorMsg += "\n  Include file: " + IncludePath.string();
          ErrorMsg += "\n  Error: " + std::string(e.what());
          ErrorMsg += "\n  Check file path and permissions";
          ConsoleWriter::WriteError("Cannot resolve include file path.");
          ConsoleWriter::WriteInfo("Check file path and permissions.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     /* Apply a size check to each include as well */

     const size_t MAX_INCLUDE_SIZE = CONFIG_READER_MAX_INCLUDE_SIZE;

     size_t FileSize;

     try
     {
          FileSize = std::filesystem::file_size(CanonicalPath);
     }
     catch (const std::filesystem::filesystem_error &e)
     {
          ErrorMsg = "Cannot read include file size";
          ErrorMsg += "\n  Include file: " + CanonicalPath;
          ErrorMsg += "\n  Requested from: " + FileName;
          ErrorMsg += "\n  Error: " + std::string(e.what());
          ErrorMsg += "\n  Possible causes:";
          ErrorMsg += "\n     - File permissions prevent reading";
          ErrorMsg += "\n     - File is a special file (device, pipe, socket)";
          ErrorMsg += "\n     - Filesystem error";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Check permissions: ls -l \"" + CanonicalPath + "\"";
          ErrorMsg += "\n     - Fix permissions: chmod 644 \"" + CanonicalPath + "\"";
          ErrorMsg += "\n     - Verify file is a regular file";
          ConsoleWriter::WriteError("Cannot read include file size: " + CanonicalPath + ".");
          ConsoleWriter::WriteWarning("Possible causes: File permissions, special file, or filesystem error.");
          ConsoleWriter::WriteInfo("Solutions: Check permissions, fix with chmod 644, verify file is regular.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     if (FileSize > MAX_INCLUDE_SIZE)
     {
          ErrorMsg = "Include file too large";
          ErrorMsg += "\n  Include file: " + CanonicalPath;
          ErrorMsg += "\n  Requested from: " + FileName;
          ErrorMsg += "\n  Size: " + FormatFileSize(FileSize) + " (max: " + FormatFileSize(MAX_INCLUDE_SIZE) + ")";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Split the include file into smaller files";
          ErrorMsg += "\n     - Remove unnecessary content";
          ErrorMsg += "\n     - Use multiple <include> directives for different sections";
          ConsoleWriter::WriteError("Include file too large: " + FormatFileSize(FileSize) + " (max: " + FormatFileSize(MAX_INCLUDE_SIZE) + ").");
          ConsoleWriter::WriteInfo("Solutions: Split into smaller files, remove unnecessary content, use multiple <include> directives.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     std::ifstream File(CanonicalPath);

     if (!File.is_open())
     {
          ErrorMsg = "Cannot open include file";
          ErrorMsg += "\n  Include file: " + CanonicalPath;
          ErrorMsg += "\n  Requested from: " + FileName;
          ErrorMsg += "\n  Configuration cannot continue without required include file";
          ErrorMsg += "\n  Check:";
          ErrorMsg += "\n     - File exists: ls -l \"" + CanonicalPath + "\"";
          ErrorMsg += "\n     - File is readable: cat \"" + CanonicalPath + "\"";
          ErrorMsg += "\n     - Permissions: chmod 644 \"" + CanonicalPath + "\"";
          ErrorMsg += "\n     - Path is correct (relative to config file directory)";
          ConsoleWriter::WriteError("Cannot open include file: " + CanonicalPath + ".");
          ConsoleWriter::WriteError("Configuration cannot continue without required include file.");
          ConsoleWriter::WriteInfo("Check: File exists, is readable, permissions correct, path is relative to config directory.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     std::string Content;

     std::string Line;

     while (std::getline(File, Line))
     {
          Content += Line + "\n";
     }

     /* Check for read errors */

     if (File.bad())
     {
          ErrorMsg = "Error reading include file";
          ErrorMsg += "\n  Include file: " + CanonicalPath;
          ErrorMsg += "\n  Requested from: " + FileName;
          ErrorMsg += "\n  File read operation failed";
          ErrorMsg += "\n  Possible causes:";
          ErrorMsg += "\n     - File is corrupted or incomplete";
          ErrorMsg += "\n     - Disk I/O error";
          ErrorMsg += "\n     - File was removed during read";
          ErrorMsg += "\n  Solutions:";
          ErrorMsg += "\n     - Check file integrity";
          ErrorMsg += "\n     - Verify disk health";
          ErrorMsg += "\n     - Restore from backup";
          ConsoleWriter::WriteError("Error reading include file: " + CanonicalPath + ".");
          ConsoleWriter::WriteError("File read operation failed.");
          ConsoleWriter::WriteWarning("Possible causes: File corrupted, disk I/O error, or file removed during read.");
          ConsoleWriter::WriteInfo("Solutions: Check file integrity, verify disk health, restore from backup.");

          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", ErrorMsg + ".");
          }

          return false;
     }

     if (Instance && Instance->Logs)
     {
          if (Instance && Instance->Logs->GetDebugMode())
          {
               Instance->Logs->Debug("configreader", "Loaded include file: " + CanonicalPath + " (depth: " + std::to_string(IncludeDepth) + ").");
          }
     }

     out_content = Content;

     return true;
}

bool ConfigReader::ValidateRequiredTags()
{
     /*
      * CRITICAL FIX: The <server> tag is only required for main server configuration files.
      * Search-specific config files (search.conf) don't need a <server> tag.
      * Check filename to determine if server tag is required.
      */

     std::filesystem::path FilePath(FileName);

     std::string FilenameLower = FilePath.filename().string();

     std::transform(FilenameLower.begin(), FilenameLower.end(), FilenameLower.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });

     /* Search config files don't require <server> tag - they only need <search> tags */

     bool IsSearchConfig = (FilenameLower == "search.conf" ||
                            FilenameLower.find("search") != std::string::npos);

     if (IsSearchConfig)
     {
          /* For search config files, skip server tag validation */
          /* Search config files can be empty and use defaults */

          auto SearchTag = GetTag("search");

          auto ParamsTag = GetTag("params");

          /* Support both old format (for backward compatibility) and new unified format */

          auto RankingBM25PlusTag = GetTag("ranking_bm25_plus");

          auto RankingBM25Tag = GetTag("ranking_bm25");

          auto RankingTfidfTag = GetTag("ranking_tfidf");

          auto RankingHybridTag = GetTag("ranking_hybrid");

          auto Bm25Tag = GetTag("bm25");

          if (!SearchTag && !ParamsTag && !RankingBM25PlusTag && !RankingBM25Tag &&
              !RankingTfidfTag && !RankingHybridTag && !Bm25Tag)
          {
               /* Not an error - search.conf can be empty and use defaults */
               /* Just log a debug message */

               if (Instance && Instance->Logs && Instance->Logs->GetDebugMode())
               {
                    Instance->Logs->Debug("configreader", "search.conf has no search tags - using default search configuration.");
               }
          }

          /* Skip server tag validation for search config files - return success */

          return true;
     }

     /* <server> tag is optional. Keep validating it only when present. */

     auto ServerTag = GetTag("server");

     if (!ServerTag)
     {
          return true;
     }

     /* Ensure the configured port is within the TCP valid range */

     if (ServerTag->HasAttribute("port"))
     {
          int Port = ServerTag->GetInt("port", 9200);

          if (Port < 1 || Port > 65535)
          {
               ErrorMsg = "Invalid server port";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Port: " + std::to_string(Port) + " (valid range: 1-65535)";
               ErrorMsg += "\n  Solution: Use a valid TCP port number";
               ErrorMsg += "\n     Common ports: 9200 (default), 8080, 3000";
               ErrorMsg += "\n     Privileged ports (1-1023) may require root/admin privileges";
               ConsoleWriter::WriteError("Invalid server port: " + std::to_string(Port) + " (valid range: 1-65535).");
               ConsoleWriter::WriteInfo("Solution: Use a valid TCP port number (common: 9200, 8080, 3000).");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }
     }

     /* Empty bind addresses indicate a configuration error */

     if (ServerTag->HasAttribute("bind"))
     {
          std::string Bind = ServerTag->GetString("bind");

          if (Bind.empty())
          {
               ErrorMsg = "Empty bind address in <server> tag";
               ErrorMsg += "\n  File: " + FileName;
               ErrorMsg += "\n  Solution: Specify a valid bind address";
               ErrorMsg += "\n     Examples:";
               ErrorMsg += "\n       - bind=\"0.0.0.0\" (all interfaces)";
               ErrorMsg += "\n       - bind=\"127.0.0.1\" (localhost only)";
               ErrorMsg += "\n       - bind=\"::\" (IPv6 all interfaces)";
               ConsoleWriter::WriteError("Empty bind address in <server> tag in file: " + FileName + ".");
               ConsoleWriter::WriteInfo("Solution: Specify a valid bind address (0.0.0.0 for all interfaces, 127.0.0.1 for localhost, :: for IPv6).");

               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader", ErrorMsg + ".");
               }

               return false;
          }
     }

     /* Emit a summary after successful parsing for observability */

     if (Instance && Instance->Logs)
     {
          int TotalTagsSummary = 0;

          for (const auto &TagList : Tags)
          {
               TotalTagsSummary += TagList.second.size();
          }
     }

     return true;
}

int ConfigTag::GetIntRange(const std::string &Key, int DefaultValue, int MinValue, int MaxValue) const
{
     /* Skip validation when the attribute is absent */

     if (!HasAttribute(Key))
     {
          return DefaultValue;
     }

     int Value = GetInt(Key, DefaultValue);

     /* Emit actionable warnings when the supplied value falls outside the allowed range */

     if (Value < MinValue || Value > MaxValue)
     {
          if (Instance && Instance->Logs)
          {
               std::string WarningMessage = "WARNING: Value out of range for tag '" + Name + "' attribute '" + Key +
                                            "': " + std::to_string(Value) + " (allowed range: " + std::to_string(MinValue) +
                                            "-" + std::to_string(MaxValue) + ") - using default: " + std::to_string(DefaultValue) + ".";
               Instance->Logs->Normal("configreader", WarningMessage);
          }

          return DefaultValue;
     }

     return Value;
}

double ConfigTag::GetDoubleRange(const std::string &Key, double DefaultValue, double MinValue, double MaxValue) const
{
     /* Skip computation unless the attribute exists */

     if (!HasAttribute(Key))
     {
          return DefaultValue;
     }

     double Value = GetDouble(Key, DefaultValue);

     /* Reject NaN and infinity so downstream math remains valid */

     if (std::isnan(Value) || std::isinf(Value))
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("configreader", "WARNING: Invalid value (NaN/Inf) for tag '" + Name + "' attribute '" + Key + "' - using default: " + std::to_string(DefaultValue) + ".");
          }

          return DefaultValue;
     }

     /* Warn when callers configure a value outside the requested bounds */

     if (Value < MinValue || Value > MaxValue)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("configreader", "WARNING: Value out of range for tag '" + Name + "' attribute '" + Key + "': " + std::to_string(Value) + " (min: " + std::to_string(MinValue) + ", max: " + std::to_string(MaxValue) + ") - using default: " + std::to_string(DefaultValue) + ".");
          }

          return DefaultValue;
     }

     return Value;
}

bool ConfigReader::ValidateTagName(const std::string &TagName)
{
     /* Tag names must not be empty */

     if (TagName.empty())
     {
          return false;
     }

     /* Tag names must not be too long */

     if (TagName.length() > 64)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader", "Tag name too long: " + TagName + ".");
          }

          return false;
     }

     /* Tag names must contain only alphanumeric characters and underscores */

     for (char c : TagName)
     {
          if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader",
                                             "Invalid character in tag name '" + TagName + "': '" + std::string(1, c) + "'.");
               }

               return false;
          }
     }

     return true;
}

bool ConfigReader::ValidateAttributeValue(const std::string &Key, const std::string &Value, const std::string &TagName)
{
     /* Check for path traversal attempts in absolute paths only. */

     if (Key == "file" || Key == "path" || Key == "directory" || Key == "datadir")
     {
          const bool IsAbsolutePath = !Value.empty() && (Value[0] == '/' || Value[0] == '\\');

          if (IsAbsolutePath && Value.find("..") != std::string::npos)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader",
                                             "Path traversal detected in tag '" + TagName + "' attribute '" + Key + "': " + Value + ".");
               }

               return false;
          }

          /* Relative paths such as ../tests/file.txt are valid and resolved against the config directory. */

          /* Check for absolute paths to sensitive locations */

          if (Value.find("/etc/") == 0 || Value.find("/sys/") == 0 || Value.find("/proc/") == 0)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader",
                                             "Suspicious path in tag '" + TagName + "' attribute '" + Key + "': " + Value + ".");
               }

               return false;
          }
     }

     /* Validate port numbers */

     if (Key == "port")
     {
          try
          {
               size_t Parsed = 0;
               int PortValueVal = std::stoi(Value, &Parsed);

               if (Parsed != Value.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("configreader",
                                                  "Invalid port number in tag '" + TagName + "': " + Value + " (contains extra characters).");
                    }

                    return false;
               }

               if (PortValueVal < 1 || PortValueVal > 65535)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Critical("configreader",
                                                  "Invalid port number in tag '" + TagName + "': " + Value + " (must be 1-65535).");
                    }

                    return false;
               }
          }
          catch (...)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader",
                                             "Non-numeric port in tag '" + TagName + "': " + Value + ".");
               }

               return false;
          }
     }

     /* Validate IP addresses */

     if (Key == "bind" || Key == "address" || Key == "host")
     {
          if (!Value.empty() && Value != "0.0.0.0" && Value != "::" && Value != "localhost")
          {
               if (!ValidateIPAddress(Value))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Possibly invalid IP/hostname in tag '" + TagName + "' attribute '" + Key + "': " + Value + ".");
                    }

                    /* Hostnames are acceptable, so only warn instead of failing */
               }
          }
     }

     /* Check for null bytes (security) */

     if (Value.find('\0') != std::string::npos)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader",
                                        "Null byte detected in tag '" + TagName + "' attribute '" + Key + "'.");
          }

          return false;
     }

     /* Check for control characters (except tab and newline which are already stripped) */

     for (char c : Value)
     {
          if (std::iscntrl(static_cast<unsigned char>(c)) && c != '\t' && c != '\n' && c != '\r')
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Critical("configreader",
                                             "Control character detected in tag '" + TagName + "' attribute '" + Key + "'.");
               }

               return false;
          }
     }

     /* Warn about whitespace-only values */

     if (!Value.empty())
     {
          bool AllWhitespaceFlag = true;

          for (char c : Value)
          {
               if (!std::isspace(static_cast<unsigned char>(c)))
               {
                    AllWhitespaceFlag = false;
                    break;
               }
          }

          if (AllWhitespaceFlag)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("configreader", "WARNING: Whitespace-only value in tag '" + TagName + "' attribute '" + Key + "'.");
               }
          }
     }

     return true;
}

bool ConfigReader::ValidateIPAddress(const std::string &IP)
{
     /* Simple IPv4 validation */

     std::regex Ipv4Pattern(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");

     /* Simple IPv6 validation (basic check) */

     std::regex Ipv6Pattern(R"(^([0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}$)");

     return std::regex_match(IP, Ipv4Pattern) || std::regex_match(IP, Ipv6Pattern);
}

std::string ConfigTag::GetStringNonEmpty(const std::string &Key, const std::string &DefaultValue) const
{
     std::string Value = GetString(Key, DefaultValue);

     /* Return default if value is empty or whitespace-only */

     if (Value.empty())
     {
          return DefaultValue;
     }

     bool AllWhitespaceFlagVal = true;

     for (char c : Value)
     {
          if (!std::isspace(static_cast<unsigned char>(c)))
          {
               AllWhitespaceFlagVal = false;
               break;
          }
     }

     if (AllWhitespaceFlagVal)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Normal("configreader", "WARNING: Whitespace-only value for tag '" + Name + "' attribute '" + Key + "' (using default: '" + DefaultValue + "').");
          }

          return DefaultValue;
     }

     return Value;
}

std::string ConfigTag::GetPath(const std::string &Key, const std::string &DefaultValue) const
{
     std::string Value = GetString(Key, DefaultValue);

     /* Validate path */

     if (Value.find("..") != std::string::npos)
     {
          if (Instance && Instance->Logs)
          {
               Instance->Logs->Critical("configreader",
                                        "Path traversal in tag '" + Name + "' attribute '" + Key + "' (using default).");
          }

          return DefaultValue;
     }

     return Value;
}

unsigned int ConfigTag::GetUnsignedInt(const std::string &Key, unsigned int DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          try
          {
               size_t Parsed = 0;
               long long ValueVal = std::stoll(it->second, &Parsed);

               if (Parsed != it->second.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Unsigned attribute '" + Key + "' for tag '" + Name + "' contains extra characters: '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Check for negative values */

               if (ValueVal < 0)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Negative value for unsigned attribute in tag '" + Name + "' attribute '" + Key + "': " + it->second + " (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Check for overflow */

               if (ValueVal > UINT_MAX)
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Value too large for unsigned int in tag '" + Name + "' attribute '" + Key + "': " + it->second + " (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               return static_cast<unsigned int>(ValueVal);
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("configreader", "WARNING: Invalid unsigned integer for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
               }

               return DefaultValue;
          }
     }

     return DefaultValue;
}

/*
 * Parse size string with unit support (KB, MB, GB, TB).
 * Supports formats like: "16MB", "256KB", "1GB", "512" (bytes if no unit).
 * Case-insensitive unit matching is performed.
 */

size_t ConfigTag::GetSize(const std::string &Key, size_t DefaultValue) const
{
     auto it = Attributes.find(Key);

     if (it != Attributes.end())
     {
          try
          {
               std::string ValueStrValue = it->second;

               /* Trim whitespace */

               ValueStrValue.erase(0, ValueStrValue.find_first_not_of(" \t\r\n"));
               ValueStrValue.erase(ValueStrValue.find_last_not_of(" \t\r\n") + 1);

               if (ValueStrValue.empty())
               {
                    return DefaultValue;
               }

               /* Convert to lowercase for case-insensitive unit matching */

               std::string LowerStrVal = ValueStrValue;

               std::transform(LowerStrVal.begin(), LowerStrVal.end(), LowerStrVal.begin(),
                              [](unsigned char C)
                              {
                                   return static_cast<char>(std::tolower(C));
                              });

               /* Extract numeric part and unit */

               size_t MultiplierVal = 1;

               std::string NumStrVal = LowerStrVal;

               /* Check for unit suffixes (KB, MB, GB, TB) */

               if (LowerStrVal.length() >= 2)
               {
                    std::string LastTwoChars = LowerStrVal.substr(LowerStrVal.length() - 2);

                    if (LastTwoChars == "kb")
                    {
                         MultiplierVal = 1024;
                         NumStrVal = LowerStrVal.substr(0, LowerStrVal.length() - 2);
                    }
                    else if (LastTwoChars == "mb")
                    {
                         MultiplierVal = 1024 * 1024;
                         NumStrVal = LowerStrVal.substr(0, LowerStrVal.length() - 2);
                    }
                    else if (LastTwoChars == "gb")
                    {
                         MultiplierVal = 1024ULL * 1024 * 1024;
                         NumStrVal = LowerStrVal.substr(0, LowerStrVal.length() - 2);
                    }
                    else if (LastTwoChars == "tb")
                    {
                         MultiplierVal = 1024ULL * 1024 * 1024 * 1024;
                         NumStrVal = LowerStrVal.substr(0, LowerStrVal.length() - 2);
                    }
               }

               /* Parse numeric value */

               size_t ParsedLength = 0;
               double NumericValueResult = std::stod(NumStrVal, &ParsedLength);

               if (ParsedLength != NumStrVal.length())
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Invalid size value (contains trailing characters) for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Validate before converting to size_t. */

               if (NumericValueResult < 0 || std::isnan(NumericValueResult) || std::isinf(NumericValueResult))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Invalid size value for tag '" + Name + "' attribute '" + Key + "': " + it->second + " (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               long double BytesValue = static_cast<long double>(NumericValueResult) *
                                        static_cast<long double>(MultiplierVal);

               if (BytesValue > static_cast<long double>(std::numeric_limits<size_t>::max()))
               {
                    if (Instance && Instance->Logs)
                    {
                         Instance->Logs->Normal("configreader", "WARNING: Size value too large for tag '" + Name + "' attribute '" + Key + "': " + it->second + " (using default: " + std::to_string(DefaultValue) + ").");
                    }

                    return DefaultValue;
               }

               /* Calculate final size in bytes */

               size_t FinalResultValue = static_cast<size_t>(BytesValue);

               return FinalResultValue;
          }
          catch (const std::exception &e)
          {
               if (Instance && Instance->Logs)
               {
                    Instance->Logs->Normal("configreader", "WARNING: Invalid size value for tag '" + Name + "' attribute '" + Key + "': '" + it->second + "' (using default: " + std::to_string(DefaultValue) + ").");
               }

               return DefaultValue;
          }
     }

     return DefaultValue;
}
