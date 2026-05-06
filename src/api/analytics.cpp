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

#include "api/searchapi.h"
#include "api/common.h"
#include "core/hlquery.h"
#include "core/modulemanager.h"
#include "utils/protocol.h"
#include "vendor/json/json.hpp"

HttpResponse SearchAPI::HandleAnalyticsClick(const HttpRequest &Request)
{
     if (Request.Method != "POST")
     {
          return HttpResponse(Status::METHOD_NOT_ALLOWED, StatusText(Status::METHOD_NOT_ALLOWED), "application/json");
     }

     try
     {
          nlohmann::json Body = nlohmann::json::parse(Request.Body);

          const std::string Collection = Body.value("collection", "");
          const std::string Query = Body.value("query", "");
          const std::string DocID = Body.value("doc_id", Body.value("document_id", ""));
          const int Rank = Body.value("rank", -1);

          if (Collection.empty() || DocID.empty())
          {
               return BuildErrorResponse(Status::BAD_REQUEST,
                                         Code::SEARCH_INVALID_PARAMETER,
                                         "Invalid click analytics payload.",
                                         "Both collection and doc_id are required.");
          }

          FOREACH_MOD(OnAnalyticsClick, Collection, Query, DocID, Rank, Request.RemoteAddress, Request.APIKeyID, !Request.APIKeyID.empty());

          nlohmann::json ResponseJSON;
          ResponseJSON["ok"] = true;
          ResponseJSON["collection"] = Collection;
          ResponseJSON["doc_id"] = DocID;

          HttpResponse Response(Status::OK, StatusText(Status::OK), "application/json");
          Response.Body = ResponseJSON.dump();
          return Response;
     }
     catch (const nlohmann::json::exception &E)
     {
          return BuildErrorResponse(Status::BAD_REQUEST,
                                    Code::VALIDATION_INVALID_JSON,
                                    "Invalid JSON body.",
                                    "Failed to parse click analytics payload: " + std::string(E.what()));
     }
}
