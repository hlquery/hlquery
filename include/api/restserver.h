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

#include <functional>
#include <map>
#include <memory>
#include <string>

/* Represents one normalized REST request passed to the server layer. */

struct RestRequest
{
     /* HTTP method */

     std::string method;

     /* Request path */

     std::string path;

     /* Raw request body */

     std::string body;

     /* Request headers */

     std::map<std::string, std::string> headers;

     /* Parsed query parameters */

     std::map<std::string, std::string> query_params;

     /* Remote client address */

     std::string remote_address;
};

/*
 *  Represents one REST response returned by a route handler.
 * This stores the status code, body payload, content type, and headers.
 */

struct RestResponse
{
     /* HTTP status code */

     int status_code;

     /* Response body payload */

     std::string body;

     /* Response content type */

     std::string content_type;

     /* Extra response headers */

     std::map<std::string, std::string> headers;

     /* Construct one REST response with body text and content type. */

     RestResponse(int code, const std::string& body_text, const std::string& content_type = "application/json")
         : status_code(code), body(body_text), content_type(content_type)
     {

     }
};

/* 
 * Lightweight REST server facade.
 * This class manages route registration, request dispatch,
 * and the database-backed handlers exposed by the REST API.
 */

class RestServer
{
   public:

     /* Construct the REST server with a listening port. */

     explicit RestServer(int port = 9200);

     /* Destroy the REST server and release owned resources. */

     ~RestServer();

     /* Start the REST server lifecycle. */

     bool start();

     /* Stop the REST server lifecycle. */

     void stop();

     /* Returns whether the server is currently marked as running. */

     bool isRunning() const
     {
          return running_;
     }

     /* Attach the database used by route handlers. */

     void setDatabase(std::shared_ptr<Database> database);

     /* Dispatch one REST request to the matching route handler. */

     RestResponse handleRequest(const RestRequest& request);

   private:

     /* Register the built-in route handlers. */

     void setupRoutes();

     /* Handle the cluster health endpoint. */

     RestResponse handleClusterHealth(const RestRequest& request);

     /* Handle the cluster statistics endpoint. */

     RestResponse handleClusterStats(const RestRequest& request);

     /* Handle the node listing endpoint. */

     RestResponse handleNodes(const RestRequest& request);

     /* Handle collection or index creation. */

     RestResponse handleCreateIndex(const RestRequest& request);

     /* Handle collection or index deletion. */

     RestResponse handleDeleteIndex(const RestRequest& request);

     /* Handle collection or index listing. */

     RestResponse handleListIndices(const RestRequest& request);

     /* Handle search requests. */

     RestResponse handleSearch(const RestRequest& request);

     /* Build a structured error response. */

     RestResponse createErrorResponse(int status_code, const std::string& type, const std::string& reason);

     /* Extract the target index name from one request path. */

     std::string extractIndexName(const std::string& path);

     /* Listening port */

     int port;

     /* Running state flag */

     bool running_;

     /* Database dependency used by handlers */

     std::shared_ptr<Database> database_;

     /* Route table keyed by path or route pattern */

     std::map<std::string, std::function<RestResponse(const RestRequest&)>> routes_;
};
