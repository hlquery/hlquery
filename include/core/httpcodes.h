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

#include <string>

class HttpCodes
{
   public:

     struct code
     {
          /* 2xx Success */

          static constexpr int OK = 200;
          static constexpr int CREATED = 201;
          static constexpr int ACCEPTED = 202;
          static constexpr int NO_CONTENT = 204;
          static constexpr int PARTIAL_CONTENT = 206;
          static constexpr int MULTI_STATUS = 207;

          /* 3xx Redirection */

          static constexpr int MOVED_PERMANENTLY = 301;
          static constexpr int FOUND = 302;
          static constexpr int SEE_OTHER = 303;
          static constexpr int NOT_MODIFIED = 304;
          static constexpr int TEMPORARY_REDIRECT = 307;
          static constexpr int PERMANENT_REDIRECT = 308;

          /* 4xx Client Errors */

          static constexpr int BAD_REQUEST = 400;
          static constexpr int UNAUTHORIZED = 401;
          static constexpr int PAYMENT_REQUIRED = 402;
          static constexpr int FORBIDDEN = 403;
          static constexpr int NOT_FOUND = 404;
          static constexpr int METHOD_NOT_ALLOWED = 405;
          static constexpr int NOT_ACCEPTABLE = 406;
          static constexpr int PROXY_AUTHENTICATION_REQUIRED = 407;
          static constexpr int REQUEST_TIMEOUT = 408;
          static constexpr int CONFLICT = 409;
          static constexpr int GONE = 410;
          static constexpr int LENGTH_REQUIRED = 411;
          static constexpr int PRECONDITION_FAILED = 412;
          static constexpr int PAYLOAD_TOO_LARGE = 413;
          static constexpr int URI_TOO_LONG = 414;
          static constexpr int UNSUPPORTED_MEDIA_TYPE = 415;
          static constexpr int RANGE_NOT_SATISFIABLE = 416;
          static constexpr int EXPECTATION_FAILED = 417;
          static constexpr int IM_A_TEAPOT = 418;
          static constexpr int UNPROCESSABLE_ENTITY = 422;
          static constexpr int LOCKED = 423;
          static constexpr int FAILED_DEPENDENCY = 424;
          static constexpr int TOO_EARLY = 425;
          static constexpr int UPGRADE_REQUIRED = 426;
          static constexpr int PRECONDITION_REQUIRED = 428;
          static constexpr int TOO_MANY_REQUESTS = 429;
          static constexpr int REQUEST_HEADER_FIELDS_TOO_LARGE = 431;
          static constexpr int UNAVAILABLE_FOR_LEGAL_REASONS = 451;

          /* 5xx Server Errors */

          static constexpr int INTERNAL_SERVER_ERROR = 500;
          static constexpr int NOT_IMPLEMENTED = 501;
          static constexpr int BAD_GATEWAY = 502;
          static constexpr int SERVICE_UNAVAILABLE = 503;
          static constexpr int GATEWAY_TIMEOUT = 504;
          static constexpr int HTTP_VERSION_NOT_SUPPORTED = 505;
          static constexpr int VARIANT_ALSO_NEGOTIATES = 506;
          static constexpr int INSUFFICIENT_STORAGE = 507;
          static constexpr int LOOP_DETECTED = 508;
          static constexpr int NOT_EXTENDED = 510;
          static constexpr int NETWORK_AUTHENTICATION_REQUIRED = 511;
     };

     static const char* StatusText(int value)
     {
          switch (value)
          {
               case code::OK:
                    return "OK";
               case code::CREATED:
                    return "Created";
               case code::ACCEPTED:
                    return "Accepted";
               case code::NO_CONTENT:
                    return "No Content";
               case code::PARTIAL_CONTENT:
                    return "Partial Content";
               case code::MULTI_STATUS:
                    return "Multi-Status";

               case code::MOVED_PERMANENTLY:
                    return "Moved Permanently";
               case code::FOUND:
                    return "Found";
               case code::SEE_OTHER:
                    return "See Other";
               case code::NOT_MODIFIED:
                    return "Not Modified";
               case code::TEMPORARY_REDIRECT:
                    return "Temporary Redirect";
               case code::PERMANENT_REDIRECT:
                    return "Permanent Redirect";

               case code::BAD_REQUEST:
                    return "Bad Request";
               case code::UNAUTHORIZED:
                    return "Unauthorized";
               case code::PAYMENT_REQUIRED:
                    return "Payment Required";
               case code::FORBIDDEN:
                    return "Forbidden";
               case code::NOT_FOUND:
                    return "Not Found";
               case code::METHOD_NOT_ALLOWED:
                    return "Method Not Allowed";
               case code::NOT_ACCEPTABLE:
                    return "Not Acceptable";
               case code::PROXY_AUTHENTICATION_REQUIRED:
                    return "Proxy Authentication Required";
               case code::REQUEST_TIMEOUT:
                    return "Request Timeout";
               case code::CONFLICT:
                    return "Conflict";
               case code::GONE:
                    return "Gone";
               case code::LENGTH_REQUIRED:
                    return "Length Required";
               case code::PRECONDITION_FAILED:
                    return "Precondition Failed";
               case code::PAYLOAD_TOO_LARGE:
                    return "Payload Too Large";
               case code::URI_TOO_LONG:
                    return "URI Too Long";
               case code::UNSUPPORTED_MEDIA_TYPE:
                    return "Unsupported Media Type";
               case code::RANGE_NOT_SATISFIABLE:
                    return "Range Not Satisfiable";
               case code::EXPECTATION_FAILED:
                    return "Expectation Failed";
               case code::IM_A_TEAPOT:
                    return "I'm a teapot";
               case code::UNPROCESSABLE_ENTITY:
                    return "Unprocessable Entity";
               case code::LOCKED:
                    return "Locked";
               case code::FAILED_DEPENDENCY:
                    return "Failed Dependency";
               case code::TOO_EARLY:
                    return "Too Early";
               case code::UPGRADE_REQUIRED:
                    return "Upgrade Required";
               case code::PRECONDITION_REQUIRED:
                    return "Precondition Required";
               case code::TOO_MANY_REQUESTS:
                    return "Too Many Requests";
               case code::REQUEST_HEADER_FIELDS_TOO_LARGE:
                    return "Request Header Fields Too Large";
               case code::UNAVAILABLE_FOR_LEGAL_REASONS:
                    return "Unavailable For Legal Reasons";

               case code::INTERNAL_SERVER_ERROR:
                    return "Internal Server Error";
               case code::NOT_IMPLEMENTED:
                    return "Not Implemented";
               case code::BAD_GATEWAY:
                    return "Bad Gateway";
               case code::SERVICE_UNAVAILABLE:
                    return "Service Unavailable";
               case code::GATEWAY_TIMEOUT:
                    return "Gateway Timeout";
               case code::HTTP_VERSION_NOT_SUPPORTED:
                    return "HTTP Version Not Supported";
               case code::VARIANT_ALSO_NEGOTIATES:
                    return "Variant Also Negotiates";
               case code::INSUFFICIENT_STORAGE:
                    return "Insufficient Storage";
               case code::LOOP_DETECTED:
                    return "Loop Detected";
               case code::NOT_EXTENDED:
                    return "Not Extended";
               case code::NETWORK_AUTHENTICATION_REQUIRED:
                    return "Network Authentication Required";

               default:
                    return "Unknown";
          }
     }
};

inline const char* StatusText(int value)
{
     return HttpCodes::StatusText(value);
}
