#pragma once

#include <string>

class AFortGameMode;

// Registers this server with the Aeris matchmaker backend and keeps its state
// in sync. Enable by setting FConfiguration::ApiURL + ApiKey; everything is
// configured through launch arguments:
//
//   -ip=67.67.67.67      public IP reported to the matchmaker
//   -port=7777           public port (defaults to FConfiguration::Port)
//   -localip=10.0.0.3    local IP (defaults to -ip)
//   -localport=7777      local port (defaults to -port)
//   -region=EU           region
//   -playlist=playlist_DefaultSolo
//   -ownedby=<uuid>      owner id
//
// Lifecycle:
//   server loads          -> PATCH /matchmaker/aeris/create/server    (random uuid)
//   server joinable       -> PATCH /matchmaker/aeris/update/joinable/<id>  (joinable: true)
//   match starts          -> PATCH /matchmaker/aeris/update/joinable/<id>  (joinable: false)
//   match ends / exit     -> POST /matchmaker/AERIS/delete/server/<id>/<apikey>
class Matchmaker
{
public:
    static inline std::string ServerId;   // random uuid generated at startup
    static inline std::string Region = "EU";
    static inline std::string Playlist = "playlist_DefaultSolo";
    static inline std::string Ip;
    static inline std::string Port;
    static inline std::string LocalIp;
    static inline std::string LocalPort;
    static inline std::string OwnedBy = "00000000-0000-0000-0000-000000000000";

    // Parses -key=value launch arguments. Call once at startup.
    static void ParseLaunchArgs();

    static bool IsEnabled();

    // PATCH /matchmaker/aeris/create/server
    static void CreateServer();

    // PATCH /matchmaker/aeris/update/joinable/<ServerId>
    static void SetJoinable(bool bJoinable);

    // Watches the raw engine state ID. This avoids calling FName helpers from
    // a native setter hook, which is unsafe on Fortnite 13.40.
    static void StartMatchEndMonitor(AFortGameMode* GameMode, int InProgressStateIndex);

    // POST /matchmaker/AERIS/delete/server/<ServerId>/<apikey>
    static void DeleteServer();

    // Deletes the backend record synchronously, then immediately terminates
    // the dedicated-server process. Safe to call from multiple fallback paths.
    static void ShutdownServer(const char* Reason);
};
