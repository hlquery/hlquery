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

#include <map>
#include <string>

/* HTTPResponse struct represents a response from the server. */

struct HTTPResponse
{
     /* HTTP status code returned by the server. */

     int StatusCode;

     /* HTTP status text returned by the server. */

     std::string StatusText;

     /* HTTP response headers as key/value pairs. */

     std::map<std::string, std::string> Headers;

     /* Response body payload. */

     std::string Body;

     /* Constructor for HTTPResponse. */

     HTTPResponse() : StatusCode(-1)
     {

     }
};

/* HTTPClient class for making requests. */

class HTTPClient
{
   private:

     /* Base URL for requests, including scheme and host. */

     std::string BaseURL;

     /* Hostname extracted from the base URL. */

     std::string Host;

     /* Port extracted from the base URL (or default). */

     int Port;

     /* If true, keep raw server output without formatting. */

     bool RawMode;

     /* Bearer or API token used for Authorization header. */

     std::string AuthToken;

     /* Parses a URL. */

     void ParseURL(const std::string &url);

   public:

     /* Constructor for HTTPClient. */

     HTTPClient(const std::string &base_url, bool raw_mode = false, const std::string &auth_token = "");

     /* Makes a request to the server. */

     HTTPResponse MakeRequest(const std::string &method, const std::string &path, const std::string &body = "");

     /* Sets raw mode. */

     void SetRawMode(bool raw)
     {
          RawMode = raw;
     }

     /* Sets auth token. */

     void SetAuthToken(const std::string &token)
     {
          AuthToken = token;
     }
};
