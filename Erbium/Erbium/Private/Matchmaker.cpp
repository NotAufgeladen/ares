#include "pch.h"
#include "../Public/Matchmaker.h"
#include "../Public/Configuration.h"
#include "../Public/HttpClient.h"
#include "../Public/Misc.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include <exception>
#include <objbase.h>
#include <string>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static std::string Narrow(const wchar_t* Wide)
{
    if (!Wide)
        return {};
    std::string Out;
    while (*Wide)
        Out.push_back((char)*Wide++);
    return Out;
}

static std::string GetLaunchArg(const std::wstring& Key, const std::string& Default = {})
{
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return Default;

    std::string Result = Default;
    for (int i = 0; i < argc; i++)
    {
        auto Arg = argv[i];
        if (wcsncmp(Arg, Key.c_str(), Key.size()) == 0 && Arg[Key.size()] == L'=')
            Result = Narrow(Arg + Key.size() + 1);
    }

    LocalFree(argv);
    return Result;
}

void Matchmaker::ParseLaunchArgs()
{
    Ip = GetLaunchArg(L"-ip", "127.0.0.1");
    LocalIp = GetLaunchArg(L"-localip", Ip);

    int PortArg = atoi(GetLaunchArg(L"-port", std::to_string(FConfiguration::Port)).c_str());
    if (PortArg > 0)
        FConfiguration::Port = PortArg;
    Port = std::to_string(FConfiguration::Port);

    int LocalPortArg = atoi(GetLaunchArg(L"-localport", Port).c_str());
    LocalPort = std::to_string(LocalPortArg > 0 ? LocalPortArg : FConfiguration::Port);

    Region = GetLaunchArg(L"-region", Region);
    Playlist = GetLaunchArg(L"-playlist", Playlist);
    OwnedBy = GetLaunchArg(L"-ownedby", OwnedBy);
}

bool Matchmaker::IsEnabled()
{
    return FConfiguration::ApiURL && *FConfiguration::ApiURL;
}

// Sends a matchmaker request and logs both transport and HTTP failures.
// Returns true on HTTP 2xx and prints a diagnostic line on any failure.
static bool ApiRequest(const std::string& Endpoint, const std::string& Payload, const char* Method, const char* DisplayName = nullptr)
{
    const char* RequestName = DisplayName ? DisplayName : Endpoint.c_str();
    try
    {
        std::string Url = std::string(FConfiguration::ApiURL) + Endpoint;

        const auto Response = HttpClient::Request(Method, Url, Payload);
        if (!Response.IsTransportSuccess())
        {
            printf("[Matchmaker] %s failed: %s\n", RequestName, Response.Error.c_str());
            return false;
        }

        printf("[Matchmaker] %s -> %ld%s%s\n", RequestName, Response.StatusCode,
               Response.Body.empty() ? "" : ": ", Response.Body.c_str());
        return Response.IsHttpSuccess();
    }
    catch (const std::exception& Error)
    {
        printf("[Matchmaker] %s threw an exception: %s\n", RequestName, Error.what());
        return false;
    }
    catch (...)
    {
        printf("[Matchmaker] %s failed with an unknown exception\n", RequestName);
        return false;
    }
}

void Matchmaker::CreateServer()
{
    if (!IsEnabled())
        return;

    static std::once_flag CreateOnce;
    std::call_once(CreateOnce, []()
    {
        ParseLaunchArgs();

        GUID Id{};
        if (FAILED(CoCreateGuid(&Id)))
        {
            printf("[Matchmaker] CoCreateGuid failed; registration skipped\n");
            return;
        }

        char IdText[37];
        sprintf_s(IdText, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  Id.Data1, Id.Data2, Id.Data3, Id.Data4[0], Id.Data4[1], Id.Data4[2], Id.Data4[3],
                  Id.Data4[4], Id.Data4[5], Id.Data4[6], Id.Data4[7]);
        ServerId = IdText;

        std::string Payload = "{\"region\":\"" + Region + "\",\"playlist\":\"" + Playlist + "\",\"ip\":\"" + Ip +
                              "\",\"port\":" + Port + ",\"apikey\":\"" + FConfiguration::ApiKey +
                              "\",\"id\":\"" + ServerId + "\",\"ownedBy\":\"" + OwnedBy +
                              "\",\"localIp\":\"" + LocalIp + "\",\"localPort\":" + LocalPort + "}";

        ApiRequest("/matchmaker/aeris/create/server", Payload, "PATCH");
    });
}

void Matchmaker::SetJoinable(bool bJoinable)
{
    if (!IsEnabled())
        return;

    CreateServer();

    ApiRequest("/matchmaker/aeris/update/joinable/" + ServerId,
               "{\"apikey\":\"" + std::string(FConfiguration::ApiKey) + "\",\"joinable\":" + (bJoinable ? "true" : "false") + "}", "PATCH");
}

namespace
{
AFortGameMode* MonitoredGameMode = nullptr;
int MonitoredInProgressState = 0;
volatile LONG MatchEndMonitorStarted = 0;
volatile LONG ShutdownState = 0;
ULONGLONG ShutdownDeadline = 0;
constexpr ULONGLONG EndGameKickGraceMs = 2000;

bool SendEndGameKick(AFortPlayerControllerAthena* PlayerController)
{
    if (!PlayerController)
        return false;

    static UFunction* ClientEndGameKick = nullptr;
    static bool bLookedUpClientEndGameKick = false;
    if (!bLookedUpClientEndGameKick)
    {
        bLookedUpClientEndGameKick = true;
        ClientEndGameKick = PlayerController->GetFunction("ClientEndGameKick");
    }

    if (!ClientEndGameKick)
        return false;

    PlayerController->Call<void>(ClientEndGameKick);
    return true;
}

int KickConnectedPlayers(UNetDriver* Driver)
{
    if (!Driver)
        return 0;

    int KickedPlayers = 0;
    for (UNetConnection* Connection : Driver->ClientConnections)
    {
        if (!Connection)
            continue;

        if (SendEndGameKick(Connection->PlayerController))
            ++KickedPlayers;

        for (UNetConnection* ChildConnection : Connection->Children)
        {
            if (ChildConnection && SendEndGameKick(ChildConnection->PlayerController))
                ++KickedPlayers;
        }
    }

    return KickedPlayers;
}

DWORD WINAPI MatchEndMonitorThread(void*)
{
    Sleep(2000);

    bool bObservedInProgress = false;
    for (;;)
    {
        Sleep(250);

        int CurrentState = 0;
        bool bReadSucceeded = false;
        __try
        {
            CurrentState = MonitoredGameMode
                               ? *reinterpret_cast<volatile int*>(&MonitoredGameMode->MatchState.ComparisonIndex)
                               : 0;
            bReadSucceeded = MonitoredGameMode != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            bReadSucceeded = false;
        }

        if (!bReadSucceeded)
        {
            Matchmaker::ShutdownServer("Match GameMode became unavailable");
            return 0;
        }

        if (CurrentState == MonitoredInProgressState)
        {
            bObservedInProgress = true;
            continue;
        }

        if (bObservedInProgress && CurrentState != 0)
        {
            Matchmaker::ShutdownServer("MatchState left InProgress");
            return 0;
        }
    }
}
}

void Matchmaker::StartMatchEndMonitor(AFortGameMode* GameMode, int InProgressStateIndex)
{
    if (!GameMode || InProgressStateIndex <= 0)
    {
        printf("[Matchmaker] ERROR: match-end monitor received invalid state data.\n");
        return;
    }

    MonitoredGameMode = GameMode;
    MonitoredInProgressState = InProgressStateIndex;

    if (InterlockedCompareExchange(&MatchEndMonitorStarted, 1, 0) != 0)
        return;

    HANDLE Thread = CreateThread(nullptr, 0, MatchEndMonitorThread, nullptr, 0, nullptr);
    if (!Thread)
    {
        InterlockedExchange(&MatchEndMonitorStarted, 0);
        printf("[Matchmaker] ERROR: failed to start match-end monitor (%lu).\n", GetLastError());
        return;
    }

    CloseHandle(Thread);
    printf("[Matchmaker] Match-end monitor started (InProgress state id=%d).\n", InProgressStateIndex);
}

void Matchmaker::DeleteServer()
{
    if (!IsEnabled())
        return;

    if (ServerId.empty())
        return;

    // This legacy backend route is case-sensitive and takes the key in the URL.
    ApiRequest("/matchmaker/AERIS/delete/server/" + ServerId + "/" + FConfiguration::ApiKey, "", "POST",
               "/matchmaker/AERIS/delete/server/<id>/<redacted>");
}

void Matchmaker::TickShutdown(UNetDriver* Driver)
{
    LONG State = InterlockedCompareExchange(&ShutdownState, 0, 0);
    if (State == 0 || !Driver)
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver)
        return;

    if (State == 1 && InterlockedCompareExchange(&ShutdownState, 2, 1) == 1)
    {
        // Do backend cleanup before notifying clients. End-game travel can
        // begin tearing the process down as soon as ClientEndGameKick runs,
        // so delaying this request until the final exit tick is unreliable.
        printf("[Matchmaker] Deleting server registration before player kick.\n");
        fflush(stdout);
        DeleteServer();
        Misc::SendWebhook("Match has ended!", { { "Playlist", Playlist } });

        const int KickedPlayers = KickConnectedPlayers(Driver);
        ShutdownDeadline = GetTickCount64() + EndGameKickGraceMs;
        printf("[Matchmaker] Sent ClientEndGameKick to %d player(s); exiting in %llu ms.\n",
               KickedPlayers, EndGameKickGraceMs);
        fflush(stdout);
        return;
    }

    if (State != 2 || GetTickCount64() < ShutdownDeadline)
        return;

    if (InterlockedCompareExchange(&ShutdownState, 3, 2) != 2)
        return;

    printf("[Matchmaker] Kick grace period elapsed; exiting now.\n");
    fflush(stdout);

    // Keep the server alive until after the reliable return-to-menu RPC has
    // had an opportunity to leave the net driver's outgoing queues.
    TerminateProcess(GetCurrentProcess(), 0);
}

void Matchmaker::ShutdownServer(const char* Reason)
{
    if (InterlockedCompareExchange(&ShutdownState, 1, 0) != 0)
        return;

    printf("[Matchmaker] %s; orderly shutdown requested.\n", Reason ? Reason : "Match ended");
    fflush(stdout);
}
