#pragma once

#include <string>

namespace HttpClient
{
struct Response
{
    long StatusCode = 0;
    std::string Body;
    std::string Error;

    bool IsTransportSuccess() const { return Error.empty(); }
    bool IsHttpSuccess() const { return StatusCode >= 200 && StatusCode < 300; }
};

Response Request(const std::string& Method, const std::string& Url, const std::string& Body, int TimeoutMs = 10000);
}
