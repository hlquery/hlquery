#pragma once

#include <map>
#include <string>

struct HTTPResponse {
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;

    HTTPResponse() : status_code(-1) {}
};
