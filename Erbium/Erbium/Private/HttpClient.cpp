#include "../Public/HttpClient.h"

#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <memory>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
struct WinHttpHandleCloser
{
    void operator()(void* Handle) const
    {
        if (Handle)
            WinHttpCloseHandle(Handle);
    }
};

using WinHttpHandle = std::unique_ptr<void, WinHttpHandleCloser>;

std::wstring Utf8ToWide(const std::string& Value)
{
    if (Value.empty())
        return {};

    const int Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(), (int)Value.size(), nullptr, 0);
    if (Size <= 0)
        return {};

    std::wstring Result((size_t)Size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(), (int)Value.size(), Result.data(), Size);
    return Result;
}

std::string WindowsError(const char* Operation)
{
    const DWORD Code = GetLastError();
    char* Message = nullptr;
    const DWORD Size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, Code, 0, reinterpret_cast<char*>(&Message), 0, nullptr);
    std::string Result = std::string(Operation) + " failed (" + std::to_string(Code) + ")";
    if (Size && Message)
    {
        while (Size > 0 && (Message[strlen(Message) - 1] == '\r' || Message[strlen(Message) - 1] == '\n'))
            Message[strlen(Message) - 1] = '\0';
        Result += ": ";
        Result += Message;
    }
    if (Message)
        LocalFree(Message);
    return Result;
}
}

HttpClient::Response HttpClient::Request(const std::string& Method, const std::string& Url, const std::string& Body, int TimeoutMs)
{
    Response Result;
    const std::wstring WideUrl = Utf8ToWide(Url);
    const std::wstring WideMethod = Utf8ToWide(Method);
    if (WideUrl.empty() || WideMethod.empty())
    {
        Result.Error = "Invalid UTF-8 method or URL";
        return Result;
    }

    URL_COMPONENTS Components{};
    Components.dwStructSize = sizeof(Components);
    Components.dwSchemeLength = (DWORD)-1;
    Components.dwHostNameLength = (DWORD)-1;
    Components.dwUrlPathLength = (DWORD)-1;
    Components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(WideUrl.c_str(), (DWORD)WideUrl.size(), 0, &Components))
    {
        Result.Error = WindowsError("WinHttpCrackUrl");
        return Result;
    }

    const std::wstring Host(Components.lpszHostName, Components.dwHostNameLength);
    std::wstring Path(Components.lpszUrlPath, Components.dwUrlPathLength);
    if (Components.dwExtraInfoLength)
        Path.append(Components.lpszExtraInfo, Components.dwExtraInfoLength);
    if (Path.empty())
        Path = L"/";

    WinHttpHandle Session(WinHttpOpen(L"Erbium/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!Session)
    {
        Result.Error = WindowsError("WinHttpOpen");
        return Result;
    }
    WinHttpSetTimeouts(Session.get(), TimeoutMs, TimeoutMs, TimeoutMs, TimeoutMs);

    WinHttpHandle Connection(WinHttpConnect(Session.get(), Host.c_str(), Components.nPort, 0));
    if (!Connection)
    {
        Result.Error = WindowsError("WinHttpConnect");
        return Result;
    }

    const DWORD Flags = Components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle RequestHandle(WinHttpOpenRequest(Connection.get(), WideMethod.c_str(), Path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Flags));
    if (!RequestHandle)
    {
        Result.Error = WindowsError("WinHttpOpenRequest");
        return Result;
    }

    constexpr wchar_t Headers[] = L"Content-Type: application/json\r\nAccept: application/json";
    void* RequestBody = Body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(Body.data());
    if (!WinHttpSendRequest(RequestHandle.get(), Headers, (DWORD)-1L, RequestBody, (DWORD)Body.size(), (DWORD)Body.size(), 0))
    {
        Result.Error = WindowsError("WinHttpSendRequest");
        return Result;
    }
    if (!WinHttpReceiveResponse(RequestHandle.get(), nullptr))
    {
        Result.Error = WindowsError("WinHttpReceiveResponse");
        return Result;
    }

    DWORD StatusSize = sizeof(Result.StatusCode);
    if (!WinHttpQueryHeaders(RequestHandle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &Result.StatusCode, &StatusSize, WINHTTP_NO_HEADER_INDEX))
    {
        Result.Error = WindowsError("WinHttpQueryHeaders");
        return Result;
    }

    for (;;)
    {
        DWORD Available = 0;
        if (!WinHttpQueryDataAvailable(RequestHandle.get(), &Available))
        {
            Result.Error = WindowsError("WinHttpQueryDataAvailable");
            return Result;
        }
        if (!Available)
            break;

        std::vector<char> Buffer(Available);
        DWORD Read = 0;
        if (!WinHttpReadData(RequestHandle.get(), Buffer.data(), Available, &Read))
        {
            Result.Error = WindowsError("WinHttpReadData");
            return Result;
        }
        Result.Body.append(Buffer.data(), Read);
    }

    return Result;
}
