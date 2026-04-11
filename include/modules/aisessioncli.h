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

#include <sstream>
#include <string>
#include <vector>

#include "core/modules.h"
#include "modules/aisessionstore.h"
#include "utils/tools.h"
#include "vendor/json/json.hpp"

namespace ai_session
{
namespace cli
{
inline std::vector<std::string> SplitTokenList(const std::string &Value)
{
     std::vector<std::string> Tokens;
     std::istringstream Stream(Value);
     std::string Part;

     while (std::getline(Stream, Part, ','))
     {
          const std::string Trimmed = Tools::Trim(Part);
          if (!Trimmed.empty())
          {
               Tokens.push_back(Trimmed);
          }
     }

     return Tokens;
}

inline std::vector<std::string> CollectSessionTokens(const ModuleCommandRequest &Request)
{
     std::vector<std::string> Tokens;

     auto AppendRaw = [&](const std::string &Raw)
     {
          if (Raw.empty())
          {
               return;
          }

          for (const auto &Entry : SplitTokenList(Raw))
          {
               Tokens.push_back(Entry);
          }
     };

     auto It = Request.NamedParameters.find("token");
     if (It != Request.NamedParameters.end())
     {
          AppendRaw(It->second);
     }

     It = Request.NamedParameters.find("tokens");
     if (It != Request.NamedParameters.end())
     {
          AppendRaw(It->second);
     }

     if (Request.PositionalParameters.size() > 1)
     {
          for (size_t I = 1; I < Request.PositionalParameters.size(); ++I)
          {
               AppendRaw(Request.PositionalParameters[I]);
          }
     }

     return Tokens;
}

inline std::string ResolveSessionAction(const ModuleCommandRequest &Request, const std::string &DefaultAction = "list")
{
     if (!Request.PositionalParameters.empty())
     {
          const std::string Candidate = Tools::ToLower(Tools::Trim(Request.PositionalParameters[0]));
          if (!Candidate.empty())
          {
               return Candidate;
          }
     }

     auto It = Request.NamedParameters.find("action");
     if (It != Request.NamedParameters.end())
     {
          const std::string Candidate = Tools::ToLower(Tools::Trim(It->second));
          if (!Candidate.empty())
          {
               return Candidate;
          }
     }

     return DefaultAction;
}

inline nlohmann::json FormatInteractionSummary(const Interaction &Interaction)
{
     nlohmann::json Summary;
     Summary["timestamp"] = static_cast<long long>(Interaction.Timestamp);
     Summary["query"] = Interaction.Query;
     Summary["intent"] = Interaction.Intent;
     Summary["route"] = Interaction.Route;
     if (!Interaction.ResultSummary.empty())
     {
          Summary["result"] = Interaction.ResultSummary;
     }
     return Summary;
}

inline nlohmann::json BuildSessionSummary(const State &Session, size_t PreviewLimit = 3)
{
     nlohmann::json Summary;
     Summary["token"] = Session.Token;
     Summary["created_at"] = static_cast<long long>(Session.CreatedAt);
     Summary["last_active_at"] = static_cast<long long>(Session.LastActiveAt);
     Summary["interaction_count"] = static_cast<int>(Session.Interactions.size());

     if (!Session.Interactions.empty())
     {
          nlohmann::json Recents = nlohmann::json::array();
          const size_t Preview = std::min<size_t>(PreviewLimit, Session.Interactions.size());
          const size_t Start = Session.Interactions.size() - Preview;

          for (size_t I = Start; I < Session.Interactions.size(); ++I)
          {
               Recents.push_back(FormatInteractionSummary(Session.Interactions[I]));
          }

          Summary["recent_interactions"] = std::move(Recents);
     }

     nlohmann::json Context;
     Context["active_collection"] = Session.Context.ActiveCollection;
     Context["last_entity"] = Session.Context.LastEntity;
     Context["last_intent"] = Session.Context.LastIntent;
     Context["last_topic"] = Session.Context.LastTopic;
     Context["last_answer_mode"] = Session.Context.LastAnswerMode;
     if (!Session.Context.LastOpenedDocument.ID.empty())
     {
          Context["last_opened_document"] = {
               {"collection", Session.Context.LastOpenedDocument.Collection},
               {"id", Session.Context.LastOpenedDocument.ID},
               {"title", Session.Context.LastOpenedDocument.Title}};
     }
     if (!Session.Context.CurrentSubjectDocument.ID.empty())
     {
          Context["current_subject_document"] = {
               {"collection", Session.Context.CurrentSubjectDocument.Collection},
               {"id", Session.Context.CurrentSubjectDocument.ID},
               {"title", Session.Context.CurrentSubjectDocument.Title}};
     }
     if (!Session.Context.LastDocuments.empty())
     {
          nlohmann::json Docs = nlohmann::json::array();
          for (const auto &Doc : Session.Context.LastDocuments)
          {
               Docs.push_back({
                    {"collection", Doc.Collection},
                    {"id", Doc.ID},
                    {"title", Doc.Title}});
          }
          Context["last_documents"] = std::move(Docs);
     }
     Summary["context"] = std::move(Context);

     return Summary;
}

inline nlohmann::json BuildSessionDetail(const State &Session)
{
     nlohmann::json Detail;
     Detail["token"] = Session.Token;
     Detail["created_at"] = static_cast<long long>(Session.CreatedAt);
     Detail["last_active_at"] = static_cast<long long>(Session.LastActiveAt);
     nlohmann::json Interactions = nlohmann::json::array();

     for (const auto &Entry : Session.Interactions)
     {
          Interactions.push_back(FormatInteractionSummary(Entry));
     }

     Detail["interactions"] = std::move(Interactions);
     Detail["context"] = BuildSessionSummary(Session).value("context", nlohmann::json::object());
     return Detail;
}
} // namespace cli
} // namespace ai_session
