#include "pch.h"
#include "../Public/Moderation.h"
#include "../Public/Configuration.h"
#include "../Public/Finders.h"
#include "../Public/Hooking.hpp"
#include "../Public/Matchmaker.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include <deque>
#include <memory>
#include <string>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace
{
struct FPendingKick
{
    std::string RequestId;
    std::string Player;
    std::string Reason;
    HANDLE Completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    volatile LONG Cancelled = 0;
    bool bSucceeded = false;
    std::string Result = "Kick command timed out on the game server.";

    ~FPendingKick()
    {
        if (Completed)
            CloseHandle(Completed);
    }
};

using KickPlayerFn = bool (*)(AFortGameSession*, AFortPlayerControllerAthena*, const FText&);

struct FModerationState
{
    SRWLOCK QueueLock = SRWLOCK_INIT;
    std::deque<std::shared_ptr<FPendingKick>> Queue;
    KickPlayerFn KickPlayerOriginal = nullptr;
    volatile LONG Started = 0;
};

FModerationState& GetState()
{
    // Intentionally process-lifetime storage: the worker is stopped by process
    // termination and must never race C++ static destruction during shutdown.
    static FModerationState* State = new FModerationState();
    return *State;
}

bool KickPlayerDetour(AFortGameSession*, AFortPlayerControllerAthena*, const FText&)
{
    // This is the behavior ARES previously installed by overwriting the native
    // function with "return true". Moderator kicks call the saved trampoline.
    return true;
}

std::wstring Utf8ToWide(const std::string& Value)
{
    if (Value.empty())
        return {};

    const int Length = MultiByteToWideChar(CP_UTF8, 0, Value.data(), (int)Value.size(), nullptr, 0);
    if (Length <= 0)
        return {};

    std::wstring Result(Length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, Value.data(), (int)Value.size(), Result.data(), Length);
    return Result;
}

std::string JsonEscape(const std::string& Value)
{
    std::string Result;
    Result.reserve(Value.size() + 8);
    for (const unsigned char Character : Value)
    {
        switch (Character)
        {
        case '"': Result += "\\\""; break;
        case '\\': Result += "\\\\"; break;
        case '\b': Result += "\\b"; break;
        case '\f': Result += "\\f"; break;
        case '\n': Result += "\\n"; break;
        case '\r': Result += "\\r"; break;
        case '\t': Result += "\\t"; break;
        default:
            if (Character >= 0x20)
                Result.push_back((char)Character);
            break;
        }
    }
    return Result;
}

bool ReadJsonString(const std::string& Json, const char* Key, std::string& Out)
{
    const std::string Needle = std::string("\"") + Key + "\"";
    size_t Position = Json.find(Needle);
    if (Position == std::string::npos)
        return false;

    Position = Json.find(':', Position + Needle.size());
    if (Position == std::string::npos)
        return false;
    Position = Json.find('"', Position + 1);
    if (Position == std::string::npos)
        return false;

    Out.clear();
    for (++Position; Position < Json.size(); ++Position)
    {
        const char Character = Json[Position];
        if (Character == '"')
            return true;
        if (Character != '\\')
        {
            Out.push_back(Character);
            continue;
        }

        if (++Position >= Json.size())
            return false;
        switch (Json[Position])
        {
        case '"': Out.push_back('"'); break;
        case '\\': Out.push_back('\\'); break;
        case '/': Out.push_back('/'); break;
        case 'b': Out.push_back('\b'); break;
        case 'f': Out.push_back('\f'); break;
        case 'n': Out.push_back('\n'); break;
        case 'r': Out.push_back('\r'); break;
        case 't': Out.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

bool SendJson(HINTERNET WebSocket, const std::string& Json)
{
    return WinHttpWebSocketSend(WebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                (void*)Json.data(), (DWORD)Json.size()) == NO_ERROR;
}

bool ReceiveJson(HINTERNET WebSocket, std::string& Json)
{
    Json.clear();
    char Buffer[4096];

    for (;;)
    {
        DWORD BytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE Type{};
        const DWORD Error = WinHttpWebSocketReceive(WebSocket, Buffer, sizeof(Buffer), &BytesRead, &Type);
        if (Error != NO_ERROR || Type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return false;

        if (Type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            Type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            continue;

        Json.append(Buffer, BytesRead);
        if (Type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return true;
        if (Json.size() > 64 * 1024)
            return false;
    }
}

HINTERNET ConnectWebSocket(HINTERNET Session, HINTERNET& OutConnection)
{
    std::wstring Url = Utf8ToWide(Matchmaker::ModerationUrl);
    if (Url.empty())
        return nullptr;
    bool bSecure = false;
    if (Url.starts_with(L"wss://"))
    {
        Url.replace(0, 6, L"https://");
        bSecure = true;
    }
    else if (Url.starts_with(L"ws://"))
    {
        Url.replace(0, 5, L"http://");
    }
    else
    {
        printf("[Moderation] URL must begin with ws:// or wss://.\n");
        return nullptr;
    }
    while (!Url.empty() && Url.back() == L'/')
        Url.pop_back();
    Url += L"/kick/" + Utf8ToWide(Matchmaker::ServerId);

    URL_COMPONENTS Components{};
    Components.dwStructSize = sizeof(Components);
    Components.dwSchemeLength = (DWORD)-1;
    Components.dwHostNameLength = (DWORD)-1;
    Components.dwUrlPathLength = (DWORD)-1;
    Components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(Url.c_str(), (DWORD)Url.size(), 0, &Components))
        return nullptr;

    bSecure = bSecure || Components.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring Host(Components.lpszHostName, Components.dwHostNameLength);
    std::wstring Path(Components.lpszUrlPath, Components.dwUrlPathLength);
    if (Components.dwExtraInfoLength)
        Path.append(Components.lpszExtraInfo, Components.dwExtraInfoLength);

    HINTERNET Connection = WinHttpConnect(Session, Host.c_str(), Components.nPort, 0);
    if (!Connection)
        return nullptr;

    HINTERNET Request = WinHttpOpenRequest(Connection, L"GET", Path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           bSecure ? WINHTTP_FLAG_SECURE : 0);
    if (!Request)
    {
        WinHttpCloseHandle(Connection);
        return nullptr;
    }

    std::wstring ApiHeader = L"X-API-Key: " + Utf8ToWide(Matchmaker::ModerationKey);
    bool bOk = WinHttpAddRequestHeaders(Request, ApiHeader.c_str(), (DWORD)-1,
                                        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) &&
               WinHttpSetOption(Request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
               WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(Request, nullptr);

    HINTERNET WebSocket = bOk ? WinHttpWebSocketCompleteUpgrade(Request, 0) : nullptr;
    WinHttpCloseHandle(Request);
    if (!WebSocket)
        WinHttpCloseHandle(Connection);
    else
        OutConnection = Connection;
    return WebSocket;
}

DWORD WINAPI ModerationThread(void*)
{
    HINTERNET Session = WinHttpOpen(L"ARES moderation/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!Session)
    {
        printf("[Moderation] WinHttpOpen failed (%lu).\n", GetLastError());
        return 0;
    }

    for (;;)
    {
        HINTERNET Connection = nullptr;
        HINTERNET WebSocket = ConnectWebSocket(Session, Connection);
        if (!WebSocket)
        {
            printf("[Moderation] Connection failed (%lu); retrying in 5 seconds.\n", GetLastError());
            Sleep(5000);
            continue;
        }

        printf("[Moderation] Connected as server %s.\n", Matchmaker::ServerId.c_str());
        std::string Json;
        while (ReceiveJson(WebSocket, Json))
        {
            std::string Action;
            auto Command = std::make_shared<FPendingKick>();
            if (!ReadJsonString(Json, "action", Action) || Action != "kick" ||
                !ReadJsonString(Json, "request_id", Command->RequestId) ||
                !ReadJsonString(Json, "player", Command->Player))
                continue;
            ReadJsonString(Json, "reason", Command->Reason);

            auto& State = GetState();
            AcquireSRWLockExclusive(&State.QueueLock);
            State.Queue.push_back(Command);
            ReleaseSRWLockExclusive(&State.QueueLock);

            const DWORD WaitResult = WaitForSingleObject(Command->Completed, 15000);
            if (WaitResult != WAIT_OBJECT_0)
            {
                InterlockedExchange(&Command->Cancelled, 1);
                Command->bSucceeded = false;
                Command->Result = "Game server did not process the kick within 15 seconds.";
            }

            const std::string Response =
                "{\"action\":\"kick_result\",\"request_id\":\"" + JsonEscape(Command->RequestId) +
                "\",\"success\":" + (Command->bSucceeded ? "true" : "false") +
                ",\"message\":\"" + JsonEscape(Command->Result) + "\"}";
            if (!SendJson(WebSocket, Response))
                break;
        }

        WinHttpWebSocketClose(WebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(WebSocket);
        WinHttpCloseHandle(Connection);
        printf("[Moderation] Disconnected; retrying in 5 seconds.\n");
        Sleep(5000);
    }
}

AFortPlayerControllerAthena* FindPlayer(UNetDriver* Driver, const std::wstring& RequestedName, std::string& ActualName)
{
    auto MatchConnection = [&](UNetConnection* Connection) -> AFortPlayerControllerAthena*
    {
        AFortPlayerControllerAthena* Controller = Connection ? Connection->PlayerController : nullptr;
        if (!Controller || !Controller->PlayerState)
            return nullptr;

        FString Name = Controller->PlayerState->GetPlayerName();
        const bool bMatches = Name.CStr() && _wcsicmp(Name.CStr(), RequestedName.c_str()) == 0;
        if (bMatches)
            ActualName = Name.ToString();
        Name.Free();
        return bMatches ? Controller : nullptr;
    };

    for (UNetConnection* Connection : Driver->ClientConnections)
    {
        if (auto Controller = MatchConnection(Connection))
            return Controller;
        if (!Connection)
            continue;
        for (UNetConnection* Child : Connection->Children)
            if (auto Controller = MatchConnection(Child))
                return Controller;
    }
    return nullptr;
}
}

void Moderation::Initialize()
{
    auto& State = GetState();
    if (Matchmaker::ServerId.empty())
    {
        printf("[Moderation] Disabled because the matchmaker server ID is unavailable.\n");
        return;
    }
    const uintptr_t KickPlayer = FindKickPlayer();
    if (!KickPlayer || !Hooking::InternalHook(KickPlayer, (void*)KickPlayerDetour,
                                              (void**)&State.KickPlayerOriginal))
    {
        printf("[Moderation] ERROR: failed to preserve native GameSession::KickPlayer.\n");
        return;
    }

    if (InterlockedCompareExchange(&State.Started, 1, 0) != 0)
        return;

    HANDLE Thread = CreateThread(nullptr, 0, ModerationThread, nullptr, 0, nullptr);
    if (!Thread)
    {
        InterlockedExchange(&State.Started, 0);
        printf("[Moderation] ERROR: failed to start WebSocket worker (%lu).\n", GetLastError());
        return;
    }
    CloseHandle(Thread);
}

void Moderation::Tick(UNetDriver* Driver)
{
    if (!Driver)
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver)
        return;

    std::shared_ptr<FPendingKick> Command;
    auto& State = GetState();
    AcquireSRWLockExclusive(&State.QueueLock);
    if (!State.Queue.empty())
    {
        Command = State.Queue.front();
        State.Queue.pop_front();
    }
    ReleaseSRWLockExclusive(&State.QueueLock);
    if (!Command)
        return;
    if (InterlockedCompareExchange(&Command->Cancelled, 0, 0) != 0)
        return;

    std::string ActualName;
    AFortPlayerControllerAthena* PlayerController = FindPlayer(Driver, Utf8ToWide(Command->Player), ActualName);
    auto* GameMode = (AFortGameMode*)World->AuthorityGameMode;
    if (!PlayerController)
    {
        Command->Result = "Player '" + Command->Player + "' is not connected to this server.";
    }
    else if (!GameMode || !GameMode->GameSession || !State.KickPlayerOriginal)
    {
        Command->Result = "The native game-session kick function is unavailable.";
    }
    else
    {
        std::wstring Reason = Utf8ToWide(Command->Reason.empty() ? "Kicked by a moderator." : Command->Reason);
        FText KickReason = UKismetTextLibrary::Conv_StringToText(FString(Reason.c_str()));
        Command->bSucceeded = State.KickPlayerOriginal(GameMode->GameSession, PlayerController, KickReason);
        Command->Result = Command->bSucceeded
                              ? "Kicked " + ActualName + "."
                              : "GameSession::KickPlayer rejected the kick for " + ActualName + ".";
        printf("[Moderation] %s\n", Command->Result.c_str());
    }

    SetEvent(Command->Completed);
}
