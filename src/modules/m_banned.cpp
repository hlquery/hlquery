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
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/modules.h"
#include "search/document_collection_store.h"
#include "utils/jsonbuilder.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

/* Empty seed set that users can leave unchanged if they want no defaults. */

const std::vector<std::string> EmptySeedWords = {};

/* Selects which seed list should be applied on first module start. */

const std::vector<std::string> &ActiveSeedWords = EmptySeedWords;

/* Normalizes one stored banned word to lowercase. */

std::string NormalizeWord(const std::string &Word)
{
     std::string normalized_word = Word;

     std::transform(normalized_word.begin(), normalized_word.end(), normalized_word.begin(),
                    [](unsigned char Character)
                    {
                         return static_cast<char>(std::tolower(Character));
                    });

     return normalized_word;
}

/* Normalizes one banned-word scope and accepts common aliases. */

std::string NormalizeScope(const std::string &Scope)
{
     const std::string normalized_scope = NormalizeWord(Scope);

     if (normalized_scope == "col" || normalized_scope == "cols" || normalized_scope == "collection" || normalized_scope == "collections")
     {
          return "collection";
     }

     if (normalized_scope == "doc" || normalized_scope == "docs" || normalized_scope == "document" || normalized_scope == "documents")
     {
          return "document";
     }

     if (normalized_scope == "alias" || normalized_scope == "aliases")
     {
          return "alias";
     }

     if (normalized_scope.empty() || normalized_scope == "all" || normalized_scope == "*")
     {
          return "all";
     }

     return "";
}

/* Returns the supported scope names in display order. */

const std::vector<std::string> &GetSupportedScopes()
{
     static const std::vector<std::string> scopes = {
          "all",
          "collection",
          "document",
          "alias"};

     return scopes;
}
class BannedRuntimeModule final : public AutoRuntimeModule<BannedRuntimeModule>
{
   private:
     /* Guards the in-memory banned-word cache. */

     mutable std::shared_mutex cache_mutex;

     /* Stores normalized banned words by scope for fast pre-check lookups. */

     std::unordered_map<std::string, std::unordered_set<std::string>> banned_words_cache;

     /* Rebuilds the full in-memory cache from module storage. */

     void ReloadCache()
     {
          std::unordered_map<std::string, std::unordered_set<std::string>> rebuilt_cache;

          for (const auto &scope : GetSupportedScopes())
          {
               rebuilt_cache[scope] = std::unordered_set<std::string>();
          }

          for (const auto &scope : GetSupportedScopes())
          {
               std::vector<std::string> keys = ListStorageKeys("word/" + scope + "/*");

               for (const auto &key : keys)
               {
                    const std::string prefix = "word/" + scope + "/";

                    if (key.rfind(prefix, 0) == 0)
                    {
                         rebuilt_cache[scope].insert(key.substr(prefix.size()));
                    }
               }
          }

          std::unique_lock<std::shared_mutex> lock(cache_mutex);

          banned_words_cache = std::move(rebuilt_cache);
     }

     /* Adds one normalized word to the in-memory cache. */

     void CacheWord(const std::string &Scope, const std::string &Word)
     {
          std::unique_lock<std::shared_mutex> lock(cache_mutex);

          banned_words_cache[Scope].insert(Word);
     }

     /* Removes one normalized word from the in-memory cache. */

     void UncacheWord(const std::string &Scope, const std::string &Word)
     {
          std::unique_lock<std::shared_mutex> lock(cache_mutex);

          auto scope_it = banned_words_cache.find(Scope);

          if (scope_it != banned_words_cache.end())
          {
               scope_it->second.erase(Word);
          }
     }

     /* Seeds the initial banned-word set once per module storage namespace. */

     bool SeedDefaults()
     {
          if (GetStorageValue("meta/seeded") == "1")
          {
               return true;
          }

          for (const auto &word : ActiveSeedWords)
          {
               if (!word.empty())
               {
                    SetStorageValue("word/all/" + NormalizeWord(word), "1");
               }
          }

          SetStorageValue("meta/seeded", "1");

          return true;
     }

     /* Returns cached banned words in sorted order. */

     std::vector<std::string> GetWords(const std::string &Scope) const
     {
          std::vector<std::string> words;
          std::shared_lock<std::shared_mutex> lock(cache_mutex);

          const auto scope_it = banned_words_cache.find(Scope);

          if (scope_it != banned_words_cache.end())
          {
               words.reserve(scope_it->second.size());

               for (const auto &word : scope_it->second)
               {
                    words.push_back(word);
               }
          }

          std::sort(words.begin(), words.end());

          return words;
     }

     /* Resolves the target scope from command parameters. */

     std::string GetCommandScope(const ModuleCommandRequest &Request, bool RequireExplicitScope) const
     {
          if (!Request.Parameters.empty())
          {
               if (!RequireExplicitScope && Request.Parameters.size() == 1)
               {
                    return "all";
               }

               return NormalizeScope(Request.Parameters[0]);
          }

          const auto scope_it = Request.NamedParameters.find("scope");

          if (scope_it != Request.NamedParameters.end())
          {
               return NormalizeScope(scope_it->second);
          }

          return RequireExplicitScope ? "" : "all";
     }

     /* Resolves the target word from positional or named command parameters. */

     std::string GetCommandWord(const ModuleCommandRequest &Request, bool ScopedCommand) const
     {
          if (!Request.Parameters.empty())
          {
               if (ScopedCommand && Request.Parameters.size() >= 2)
               {
                    return NormalizeWord(Request.Parameters[1]);
               }

               if (!ScopedCommand)
               {
                    return NormalizeWord(Request.Parameters[0]);
               }
          }

          const auto word_it = Request.NamedParameters.find("word");

          if (word_it != Request.NamedParameters.end())
          {
               return NormalizeWord(word_it->second);
          }

          return "";
     }

     /* Returns the first banned word that matches this text for one scope. */

     std::string FindMatchedWord(const std::string &Scope, const std::string &Text) const
     {
          const std::vector<std::string> words = GetWords(Scope);
          std::string normalized_text = NormalizeWord(Text);

          for (char &character : normalized_text)
          {
               if (!std::isalnum(static_cast<unsigned char>(character)))
               {
                    character = ' ';
               }
          }

          for (const auto &word : words)
          {
               if (word.empty())
               {
                    continue;
               }

               if (normalized_text == word)
               {
                    return word;
               }

               if (normalized_text.find(" " + word + " ") != std::string::npos)
               {
                    return word;
               }

               if (normalized_text.rfind(word + " ", 0) == 0)
               {
                    return word;
               }

               if (normalized_text.size() >= word.size() + 1 && normalized_text.compare(normalized_text.size() - word.size() - 1, word.size() + 1, " " + word) == 0)
               {
                    return word;
               }
          }

          return "";
     }

     /* Builds a standard deny response for banned entity names. */

     ModulePreCheckResult BuildBlockedResult(const std::string &EntityName, const std::string &Scope, const std::string &MatchedWord) const
     {
          ModulePreCheckResult result;

          result.Action = ModulePreCheckAction::Deny;
          result.HttpStatus = 400;
          result.ProtocolCode = Code::VALIDATION_FAILED;
          result.Message = "Banned word blocked.";
          result.Details = "The " + EntityName + " matched banned word '" + MatchedWord + "' in scope '" + Scope + "'.";

          return result;
     }

     /* Checks one entity name against the specific and global scope lists. */

     ModulePreCheckResult CheckEntityName(const std::string &Scope, const std::string &EntityName, const std::string &EntityValue) const
     {
          std::string matched_word = FindMatchedWord(Scope, EntityValue);

          if (!matched_word.empty())
          {
               return BuildBlockedResult(EntityName, Scope, matched_word);
          }

          matched_word = FindMatchedWord("all", EntityValue);

          if (!matched_word.empty())
          {
               return BuildBlockedResult(EntityName, "all", matched_word);
          }

          return ModulePreCheckResult();
     }

     /* Checks one document payload against document and global banned words. */

     ModulePreCheckResult CheckDocument(const Document &DocumentObj) const
     {
          ModulePreCheckResult result = CheckEntityName("document", "document ID", DocumentObj.ID);

          if (result.Action == ModulePreCheckAction::Deny)
          {
               return result;
          }

          result = CheckEntityName("document", "document title", DocumentObj.Title);

          if (result.Action == ModulePreCheckAction::Deny)
          {
               return result;
          }

          result = CheckEntityName("document", "document content", DocumentObj.Content);

          if (result.Action == ModulePreCheckAction::Deny)
          {
               return result;
          }

          for (const auto &field_pair : DocumentObj.Fields)
          {
               result = CheckEntityName("document", "document field", field_pair.second);

               if (result.Action == ModulePreCheckAction::Deny)
               {
                    return result;
               }
          }

          return ModulePreCheckResult();
     }

   public:
     BannedRuntimeModule()
         : AutoRuntimeModule("banned", true)
     {
     }

     bool Start(const ServerConfig &, std::string &) override
     {
          if (!SeedDefaults())
          {
               return false;
          }

          ReloadCache();

          return true;
     }

     void Stop() override
     {
          std::unique_lock<std::shared_mutex> lock(cache_mutex);

          banned_words_cache.clear();
     }

     ModulePreCheckResult OnPreCreateCollection(const std::string &Collection, const std::string &, const std::string &, bool) override
     {
          return CheckEntityName("collection", "collection name", Collection);
     }

     ModulePreCheckResult OnPreUpdateCollection(const std::string &Collection, const std::string &, const std::string &, bool) override
     {
          return CheckEntityName("collection", "collection name", Collection);
     }

     ModulePreCheckResult OnPreAddDocument(const std::string &, const Document &DocumentObj, const std::string &, const std::string &, bool) override
     {
          return CheckDocument(DocumentObj);
     }

     ModulePreCheckResult OnPreUpdateDocument(const std::string &, const Document &DocumentObj, const std::string &, const std::string &, bool) override
     {
          return CheckDocument(DocumentObj);
     }

     ModulePreCheckResult OnPreUpsertAlias(const std::string &AliasName, const std::string &, const std::string &, bool) override
     {
          return CheckEntityName("alias", "alias name", AliasName);
     }

     ModuleAPIDescription GetAPIDescription() const override
     {
          ModuleAPIDescription description;

          description.Name = "banned";
          description.Summary = "Manages banned-word entries stored by the banned module.";
          description.Syntax = "hlquery-cli module banned <status|help|list|add|del> [scope] [word]";
          description.MinParameters = 0;
          description.MaxParameters = 2;
          description.Parameters.push_back({"scope", "string", "One of all, col, doc, or alias.", false});
          description.Parameters.push_back({"word", "string", "Banned word to add or delete.", false});
          description.Examples.push_back("hlquery-cli module banned status");
          description.Examples.push_back("hlquery-cli module banned list all");
          description.Examples.push_back("hlquery-cli module banned add col poo");
          description.Examples.push_back("hlquery-cli module banned add doc danger");
          description.Examples.push_back("hlquery-cli module banned del alias legacy");

          return description;
     }

     std::vector<ModuleCommandSpec> GetCommandSpecs() const override
     {
          std::vector<ModuleCommandSpec> commands;

          ModuleCommandSpec help_command;

          help_command.Route = "help";
          help_command.Summary = "Shows banned module commands and usage.";
          help_command.Syntax = "module banned help";

          commands.push_back(help_command);

          ModuleCommandSpec status_command;

          status_command.Route = "status";
          status_command.Summary = "Shows module status and current banned-word count.";
          status_command.Syntax = "module banned status";

          commands.push_back(status_command);

          ModuleCommandSpec list_command;

          list_command.Route = "list";
          list_command.Summary = "Lists banned words for one scope.";
          list_command.Syntax = "module banned list [all|col|doc|alias]";
          list_command.MaxParameters = 1;
          list_command.Parameters.push_back({"scope", "string", "One of all, col, doc, or alias.", false});

          commands.push_back(list_command);

          ModuleCommandSpec add_command;

          add_command.Route = "add";
          add_command.Summary = "Adds one banned word to one scope.";
          add_command.Syntax = "module banned add [all|col|doc|alias] <word>";
          add_command.MinParameters = 1;
          add_command.MaxParameters = 2;
          add_command.Parameters.push_back({"scope", "string", "One of all, col, doc, or alias.", false});
          add_command.Parameters.push_back({"word", "string", "Word to add.", true});

          commands.push_back(add_command);

          ModuleCommandSpec del_command;

          del_command.Route = "del";
          del_command.Summary = "Deletes one banned word from one scope.";
          del_command.Syntax = "module banned del [all|col|doc|alias] <word>";
          del_command.MinParameters = 1;
          del_command.MaxParameters = 2;
          del_command.Parameters.push_back({"scope", "string", "One of all, col, doc, or alias.", false});
          del_command.Parameters.push_back({"word", "string", "Word to delete.", true});

          commands.push_back(del_command);

          return commands;
     }

     ModuleCommandResponse HandleCommand(const ModuleCommandRequest &Request) override
     {
          const std::string route = Request.Route.empty() ? "status" : Request.Route;

          if (route == "status")
          {
               nlohmann::json scopes_json = nlohmann::json::array();
               int count = 0;

               for (const auto &scope : GetSupportedScopes())
               {
                    const std::vector<std::string> words = GetWords(scope);

                    scopes_json.push_back(scope);
                    count += static_cast<int>(words.size());
               }

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                                    .Add("module", "banned")
                                    .Add("seeded", (GetStorageValue("meta/seeded") == "1"))
                                    .Add("scopes", scopes_json)
                                    .Add("count", count)
                                    .Add("message", "Banned-word module is loaded.")
                                    .ToString();

               return response;
          }

          if (route == "help")
          {
               nlohmann::json commands_json = nlohmann::json::array();

               for (const auto &command : GetCommandSpecs())
               {
                    nlohmann::json command_json;

                    command_json["route"] = command.Route;
                    command_json["summary"] = command.Summary;
                    command_json["syntax"] = command.Syntax;
                    command_json["min_parameters"] = command.MinParameters;
                    command_json["max_parameters"] = command.MaxParameters;

                    commands_json.push_back(command_json);
               }

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                                    .Add("module", "banned")
                                    .Add("message", "Available banned module commands.")
                                    .Add("commands", commands_json)
                                    .ToString();

               return response;
          }

          if (route == "list")
          {
               const std::string scope = GetCommandScope(Request, false);

               if (scope.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Invalid scope parameter.").ToString();

                    return response;
               }

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                                    .Add("module", "banned")
                                    .Add("scope", scope)
                                    .Add("words", nlohmann::json(GetWords(scope)))
                                    .Add("count", static_cast<int>(GetWords(scope).size()))
                                    .ToString();

               return response;
          }

          if (route == "add")
          {
               const std::string scope = GetCommandScope(Request, false);
               const bool scoped_command = (Request.Parameters.size() >= 2 || Request.NamedParameters.count("scope") > 0);
               const std::string word = GetCommandWord(Request, scoped_command);

               if (scope.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Invalid scope parameter.").ToString();

                    return response;
               }

               if (word.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Missing word parameter.").ToString();

                    return response;
               }

               const bool stored = SetStorageValue("word/" + scope + "/" + word, "1");

               if (!stored)
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 500;
                    response.Body = JsonBuilder().Add("error", "Failed to store banned word.").ToString();

                    return response;
               }

               CacheWord(scope, word);

               ModuleCommandResponse response;
               response.Success = true;
               response.Body = JsonBuilder()
                                    .Add("module", "banned")
                                    .Add("scope", scope)
                                    .Add("word", word)
                                    .Add("message", "Banned word added.")
                                    .ToString();

               return response;
          }

          if (route == "del" || route == "delete")
          {
               const std::string scope = GetCommandScope(Request, false);
               const bool scoped_command = (Request.Parameters.size() >= 2 || Request.NamedParameters.count("scope") > 0);
               const std::string word = GetCommandWord(Request, scoped_command);

               if (scope.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Invalid scope parameter.").ToString();

                    return response;
               }

               if (word.empty())
               {
                    ModuleCommandResponse response;

                    response.StatusCode = 400;
                    response.Body = JsonBuilder().Add("error", "Missing word parameter.").ToString();

                    return response;
               }

               const bool deleted = DeleteStorageValue("word/" + scope + "/" + word);

               if (deleted)
               {
                    UncacheWord(scope, word);
               }

               ModuleCommandResponse response;
               response.Success = deleted;
               response.StatusCode = deleted ? 200 : 404;
               response.Body = JsonBuilder()
                                    .Add("module", "banned")
                                    .Add("scope", scope)
                                    .Add("word", word)
                                    .Add("deleted", deleted)
                                    .Add("message", deleted ? "Banned word deleted." : "Banned word not found.")
                                    .ToString();

               return response;
          }

          return RuntimeModule::HandleCommand(Request);
     }
};

MODULE_LOAD(BannedRuntimeModule)
