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

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * Configuration parser for HLManager
 * Supports XML-like tags: <tagname key="value" key2="value2">
 * Does NOT support self-closing tags (no />)
 */

class ConfigTag;

class ConfigReader
{
   public:

     /* Constructor */

     ConfigReader();

     /* Destructor */

     ~ConfigReader();

     /* Load and parse configuration file */

     bool LoadFile(const std::string& filename);

     /* Get all tags with specified name */

     std::vector<std::shared_ptr<ConfigTag>> GetTags(const std::string& tagname) const;

     /* Get first tag with specified name */

     std::shared_ptr<ConfigTag> GetTag(const std::string& tagname) const;

     /* Check if configuration was loaded successfully */

     bool IsValid() const
     {
          return Valid;
     }

     /* Get last error message */

     const std::string& GetError() const
     {
          return ErrorMsg;
     }

   private:

     bool Valid;

     std::string ErrorMsg;

     std::string FileName;

     /* Storage for all parsed tags */

     std::unordered_map<std::string, std::vector<std::shared_ptr<ConfigTag>>> Tags;

     /* Circular include protection */

     std::unordered_set<std::string> ActiveIncludes;

     int IncludeDepth;

     /* Parsing methods */

     bool ParseContent(const std::string& content);

     bool ParseAttributes(const std::string& attr_str, std::shared_ptr<ConfigTag> tag);

     bool ValidateRequiredTags();

     bool ValidateTagName(const std::string& tag_name);

     bool ValidateAttributeValue(const std::string& key, const std::string& value, const std::string& tag_name);

     bool ValidateIPAddress(const std::string& ip);

     std::string TrimWhitespace(const std::string& str);

     bool ProcessCommentsAndIncludes(const std::string& content, std::string& out_content);

     bool LoadIncludeFile(const std::string& filename, std::string& out_content);
};

/* Represents a configuration tag with attributes */

class ConfigTag
{
   public:

     /* Constructor */

     ConfigTag(const std::string& name) : Name(name)
     {
     }

     /* Get tag name */

     const std::string& GetName() const
     {
          return Name;
     }

     /* Get attribute value as string */

     std::string GetString(const std::string& key, const std::string& default_value = "") const;

     /* Get attribute value as integer */

     int GetInt(const std::string& key, int default_value = 0) const;

     /* Get attribute value as boolean */

     bool GetBool(const std::string& key, bool default_value = false) const;

     /* Get attribute value as double */

     double GetDouble(const std::string& key, double default_value = 0.0) const;

     /* Get attribute value as integer with range validation */

     int GetIntRange(const std::string& key, int default_value, int min_value, int max_value) const;

     /* Get attribute value as double with range validation */

     double GetDoubleRange(const std::string& key, double default_value, double min_value, double max_value) const;

     /* Check if attribute exists */

     bool HasAttribute(const std::string& key) const;

     /* Get string attribute that must not be empty/whitespace */

     std::string GetStringNonEmpty(const std::string& key, const std::string& default_value = "") const;

     /* Get path attribute with security validation */

     std::string GetPath(const std::string& key, const std::string& default_value = "") const;

     /* Get unsigned integer attribute */

     unsigned int GetUnsignedInt(const std::string& key, unsigned int default_value = 0) const;

     /* Get size attribute with unit parsing (supports KB, MB, GB, TB) */

     size_t GetSize(const std::string& key, size_t default_value = 0) const;

     /* Get all attributes */

     const std::unordered_map<std::string, std::string>& GetAttributes() const
     {
          return Attributes;
     }

     /* Set attribute (used by parser) */

     void SetAttribute(const std::string& key, const std::string& value);

   private:

     std::string Name;

     std::unordered_map<std::string, std::string> Attributes;
};
