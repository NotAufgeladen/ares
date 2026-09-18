#include "../Erbium/Erbium/Public/HttpClient.h"

#include <Windows.h>
#include <cstdio>
#include <iostream>
#include <string>

namespace
{
std::string GetEnvironment(const char* Name)
{
    const DWORD Size = GetEnvironmentVariableA(Name, nullptr, 0);
    if (!Size)
        return {};
    std::string Value(Size - 1, '\0');
    GetEnvironmentVariableA(Name, Value.data(), Size);
    return Value;
}

bool Send(const std::string& Method, const std::string& Url, const std::string& Body)
{
    std::cout << Method << " request" << std::endl;
    const auto Response = HttpClient::Request(Method, Url, Body);
    if (!Response.IsTransportSuccess())
    {
        std::cerr << "Transport error: " << Response.Error << std::endl;
        return false;
    }
    std::cout << "HTTP " << Response.StatusCode << " " << Response.Body << std::endl;
    return Response.IsHttpSuccess();
}
}

int main()
{
    const std::string BaseUrl = GetEnvironment("ARES_API_URL");
    const std::string ApiKey = GetEnvironment("ARES_API_KEY");
    if (BaseUrl.empty() || ApiKey.empty())
    {
        std::cerr << "Set ARES_API_URL and ARES_API_KEY before running this test." << std::endl;
        return 2;
    }

    char Suffix[13];
    sprintf_s(Suffix, "%04lx%08lx", GetCurrentProcessId() & 0xffff, (DWORD)GetTickCount64());
    const std::string Id = std::string("00000000-0000-4000-8000-") + Suffix;
    const std::string CreateBody = "{\"region\":\"EU\",\"playlist\":\"playlist_DefaultSolo\",\"ip\":\"127.0.0.1\",\"port\":7777,\"apikey\":\"" + ApiKey +
                                   "\",\"id\":\"" + Id + "\",\"ownedBy\":\"00000000-0000-0000-0000-000000000000\",\"localIp\":\"127.0.0.1\",\"localPort\":7777}";
    if (!Send("PATCH", BaseUrl + "/matchmaker/aeris/create/server", CreateBody))
        return 1;

    const bool Joinable = Send("PATCH", BaseUrl + "/matchmaker/aeris/update/joinable/" + Id,
                               "{\"apikey\":\"" + ApiKey + "\",\"joinable\":true}");
    const bool Deleted = Send("POST", BaseUrl + "/matchmaker/AERIS/delete/server/" + Id + "/" + ApiKey, "");
    return Joinable && Deleted ? 0 : 1;
}
