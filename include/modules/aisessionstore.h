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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include "core/modules.h"
#include "utils/tools.h"
#include "vendor/json/json.hpp"

namespace ai_session
{

struct Interaction
{
    std::string Query;
    std::string Intent;
    std::string Route;
    nlohmann::json ResultSummary = nlohmann::json::object();
    int64_t Timestamp = 0;
};

struct DocumentRef
{
    std::string Collection;
    std::string ID;
    std::string Title;
};

struct Memory
{
    std::string ActiveCollection;
    std::vector<DocumentRef> LastDocuments;
    DocumentRef LastOpenedDocument;
    DocumentRef CurrentSubjectDocument;
    std::string LastEntity;
    std::string LastIntent;
    std::string LastTopic;
    std::string LastAnswerMode;
};

struct State
{
    std::string Token;
    int64_t CreatedAt = 0;
    int64_t LastActiveAt = 0;
    std::vector<Interaction> Interactions;
    Memory Context;
};

struct Result
{
    State Session;
    bool IsNew = false;
};

constexpr size_t kDefaultAISessionHistoryLimit = 32;
constexpr int64_t kDefaultAISessionTTLSeconds = 60 * 60 * 24 * 7;

class Store
{
  public:
    explicit Store(const RuntimeModule &InModule, size_t InMaxHistory = kDefaultAISessionHistoryLimit)
        : Module(InModule), MaxHistory(std::max<size_t>(1, InMaxHistory))
    {
    }

    Result Acquire(const std::string &RequestedToken) const
    {
        Result Output;
        std::string Token = NormalizeToken(RequestedToken);
        if (Token.empty())
        {
            Token = GenerateToken();
        }

        if (TryLoad(Token, Output.Session))
        {
            Output.IsNew = false;
            return Output;
        }

        Output.Session = Create(Token);
        Output.IsNew = true;
        return Output;
    }

    void TrackInteraction(State &Session, const Interaction &Entry) const
    {
        if (Session.Token.empty())
        {
            return;
        }

        Interaction SavedEntry = Entry;
        if (SavedEntry.Timestamp == 0)
        {
            SavedEntry.Timestamp = Tools::GetUnixTimestamp();
        }

        Session.LastActiveAt = SavedEntry.Timestamp;
        Session.Interactions.push_back(SavedEntry);

        if (Session.Interactions.size() > MaxHistory)
        {
            const size_t Excess = Session.Interactions.size() - MaxHistory;
            Session.Interactions.erase(Session.Interactions.begin(), Session.Interactions.begin() + Excess);
        }

        Save(Session);
    }

    bool Load(const std::string &Token, State &Session) const
    {
        return TryLoad(Token, Session);
    }

    std::vector<State> ListSessions() const
    {
        std::vector<State> Sessions;
        const std::vector<std::string> Keys = Module.ListStorageKeys("session:*");
        const std::string Prefix = "session:";

        for (const auto &Key : Keys)
        {
            if (Key.rfind(Prefix, 0) != 0)
            {
                continue;
            }

            const std::string Token = Key.substr(Prefix.size());
            State Session;
            if (TryLoad(Token, Session))
            {
                Sessions.push_back(std::move(Session));
            }
        }

        return Sessions;
    }

    size_t Delete(const std::vector<std::string> &Tokens) const
    {
        size_t DeletedCount = 0;
        for (const auto &Token : Tokens)
        {
            if (Delete(Token))
            {
                ++DeletedCount;
            }
        }
        return DeletedCount;
    }

    bool Delete(const std::string &Token) const
    {
        if (Token.empty())
        {
            return false;
        }

        return Module.DeleteStorageValue(SessionKey(Token)) > 0;
    }

  private:
    static std::string NormalizeToken(const std::string &Value)
    {
        const std::string Trimmed = Tools::Trim(Value);
        if (Trimmed.empty())
        {
            return "";
        }

        return Trimmed.substr(0, std::min<size_t>(Trimmed.size(), 128));
    }

    static std::string SessionKey(const std::string &Token)
    {
        return "session:" + Token;
    }

    State Create(const std::string &Token) const
    {
        State Result;
        Result.Token = Token;
        const int64_t Now = Tools::GetUnixTimestamp();
        Result.CreatedAt = Now;
        Result.LastActiveAt = Now;
        return Result;
    }

    bool TryLoad(const std::string &Token, State &Session) const
    {
        if (Token.empty())
        {
            return false;
        }

        const std::string Raw = Module.GetStorageValue(SessionKey(Token));
        if (Raw.empty())
        {
            return false;
        }

        nlohmann::json Parsed;
        try
        {
            Parsed = nlohmann::json::parse(Raw);
        }
        catch (...)
        {
            return false;
        }

        Session.Token = Token;
        Session.CreatedAt = Parsed.value("created_at", Tools::GetUnixTimestamp());
        Session.LastActiveAt = Parsed.value("last_active_at", Session.CreatedAt);

        const int64_t Now = Tools::GetUnixTimestamp();
        if (Session.LastActiveAt > 0 &&
            Session.LastActiveAt + kDefaultAISessionTTLSeconds < Now)
        {
            Module.DeleteStorageValue(SessionKey(Token));
            return false;
        }

        const auto ContextIt = Parsed.find("context");
        if (ContextIt != Parsed.end() && ContextIt->is_object())
        {
            Session.Context.ActiveCollection = ContextIt->value("active_collection", "");
            Session.Context.LastEntity = ContextIt->value("last_entity", "");
            Session.Context.LastIntent = ContextIt->value("last_intent", "");
            Session.Context.LastTopic = ContextIt->value("last_topic", "");
            Session.Context.LastAnswerMode = ContextIt->value("last_answer_mode", "");

            const auto LastOpenedIt = ContextIt->find("last_opened_document");
            if (LastOpenedIt != ContextIt->end() && LastOpenedIt->is_object())
            {
                Session.Context.LastOpenedDocument.Collection = LastOpenedIt->value("collection", "");
                Session.Context.LastOpenedDocument.ID = LastOpenedIt->value("id", "");
                Session.Context.LastOpenedDocument.Title = LastOpenedIt->value("title", "");
            }

            const auto CurrentSubjectIt = ContextIt->find("current_subject_document");
            if (CurrentSubjectIt != ContextIt->end() && CurrentSubjectIt->is_object())
            {
                Session.Context.CurrentSubjectDocument.Collection = CurrentSubjectIt->value("collection", "");
                Session.Context.CurrentSubjectDocument.ID = CurrentSubjectIt->value("id", "");
                Session.Context.CurrentSubjectDocument.Title = CurrentSubjectIt->value("title", "");
            }

            const auto LastDocsIt = ContextIt->find("last_documents");
            if (LastDocsIt != ContextIt->end() && LastDocsIt->is_array())
            {
                for (const auto &Entry : *LastDocsIt)
                {
                    if (!Entry.is_object())
                    {
                        continue;
                    }

                    DocumentRef Ref;
                    Ref.Collection = Entry.value("collection", "");
                    Ref.ID = Entry.value("id", "");
                    Ref.Title = Entry.value("title", "");
                    if (!Ref.ID.empty() || !Ref.Title.empty())
                    {
                        Session.Context.LastDocuments.push_back(std::move(Ref));
                    }
                }
            }
        }

        const auto InteractionsIt = Parsed.find("interactions");
        if (InteractionsIt != Parsed.end() && InteractionsIt->is_array())
        {
            for (const auto &Entry : *InteractionsIt)
            {
                Interaction Record;
                Record.Timestamp = Entry.value("timestamp", Tools::GetUnixTimestamp());
                Record.Query = Entry.value("query", "");
                Record.Intent = Entry.value("intent", "");
                Record.Route = Entry.value("route", "");
                if (Entry.contains("result"))
                {
                    Record.ResultSummary = Entry["result"];
                }
                Session.Interactions.push_back(std::move(Record));
            }
        }

        if (Session.Interactions.size() > MaxHistory)
        {
            const size_t Excess = Session.Interactions.size() - MaxHistory;
            Session.Interactions.erase(Session.Interactions.begin(), Session.Interactions.begin() + Excess);
        }

        return true;
    }

    std::string GenerateToken() const
    {
        return Tools::RandomHex(32);
    }

    bool Save(const State &Session) const
    {
        if (Session.Token.empty())
        {
            return false;
        }

        nlohmann::json Stored;
        Stored["token"] = Session.Token;
        Stored["created_at"] = Session.CreatedAt;
        Stored["last_active_at"] = Session.LastActiveAt;
        nlohmann::json Context = nlohmann::json::object();
        Context["active_collection"] = Session.Context.ActiveCollection;
        Context["last_entity"] = Session.Context.LastEntity;
        Context["last_intent"] = Session.Context.LastIntent;
        Context["last_topic"] = Session.Context.LastTopic;
        Context["last_answer_mode"] = Session.Context.LastAnswerMode;
        Context["last_opened_document"] = {
            {"collection", Session.Context.LastOpenedDocument.Collection},
            {"id", Session.Context.LastOpenedDocument.ID},
            {"title", Session.Context.LastOpenedDocument.Title}};
        Context["current_subject_document"] = {
            {"collection", Session.Context.CurrentSubjectDocument.Collection},
            {"id", Session.Context.CurrentSubjectDocument.ID},
            {"title", Session.Context.CurrentSubjectDocument.Title}};
        Context["last_documents"] = nlohmann::json::array();
        for (const auto &Doc : Session.Context.LastDocuments)
        {
            Context["last_documents"].push_back({
                {"collection", Doc.Collection},
                {"id", Doc.ID},
                {"title", Doc.Title}});
        }
        Stored["context"] = std::move(Context);
        Stored["interactions"] = nlohmann::json::array();

        for (const auto &Entry : Session.Interactions)
        {
            nlohmann::json EntryJSON;
            EntryJSON["timestamp"] = Entry.Timestamp;
            EntryJSON["query"] = Entry.Query;
            EntryJSON["intent"] = Entry.Intent;
            EntryJSON["route"] = Entry.Route;
            EntryJSON["result"] = Entry.ResultSummary;
            Stored["interactions"].push_back(std::move(EntryJSON));
        }

        return Module.SetStorageValue(SessionKey(Session.Token), Stored.dump());
    }

    const RuntimeModule &Module;
    size_t MaxHistory = 24;
};

inline std::string ExtractSessionToken(const std::map<std::string, std::string> &NamedParameters)
{
    static const std::vector<std::string> Keys = {"session", "session_token", "token"};

    for (const auto &Key : Keys)
    {
        auto It = NamedParameters.find(Key);
        if (It == NamedParameters.end())
        {
            continue;
        }

        const std::string Trimmed = Tools::Trim(It->second);
        if (!Trimmed.empty())
        {
            return Trimmed;
        }
    }

    return "";
}

inline std::string ExtractSessionToken(const ModuleCommandRequest &Request)
{
    return ExtractSessionToken(Request.NamedParameters);
}

}
