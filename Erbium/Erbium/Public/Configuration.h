#pragma once

struct FConfiguration
{
    static inline auto Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    static inline auto MaxTickRate = 30;
    static inline auto bLateGame = false;
    static inline auto LateGameZone = 3;          // starting zone
    static inline auto bLateGameLongZone = false; // zone doesnt close for a long time
    static inline auto bEnableCheats = false;
    static inline auto SiphonAmount = 50; // set to 0 to disable
    static inline auto bInfiniteMats = false;
    static inline auto bInfiniteAmmo = false;
    static inline auto bForceRespawns = false; // build your client with this too!
    static inline auto bJoinInProgress = false;
    static inline auto bAutoRestart = true;
    static inline auto bKeepInventory = false;
    static inline auto Port = 7777;
    static inline auto bEnableIris = true;
    static inline auto bGUI = false;
    static inline constexpr auto bCustomCrashReporter = true;
    static inline constexpr auto bUseStdoutLog = false;
    static inline constexpr auto WebhookURL = ""; // fill in if you want status to send to a webhook
    static inline constexpr auto ApiURL = "https://api-v3-dev.phnx.lol";     // base matchmaker API url, e.g. "https://api.example.com" (no trailing slash)
    static inline constexpr auto ApiKey = "AERISGV_lUi8VA476DqKnbRqCna0cOAegt9fbJYKa5UZUEDk1jNy2ya451UwEt9jXq5K8D1oHU52B67PB4xWkiiboCXQ4BXK278mKx4f5VCQIwE3XL74bpFrIGa6aeZI6pjvmplpOYPxef5osVIfSvXF3oYT85H2MWmDgqriYz0DYhufVcTatYQrOs46410KfeK3lgFfW0lYW4xzVe1ruCgAcwbttndzKNNam8NWzHFLW2nctFt8G5dmpPa2iQxgg18iznoW";     // AERIS_API_KEY sent with matchmaker requests
};
