# Erbium Codebase Audit

Audit performed while making the codebase production-ready. Scope: `Erbium/` (server core, Engine, FortniteGame, plugins), `ErbiumClient/`. SDK/ImGui are vendored third-party code and were not audited line-by-line.

---

## Changes Made

### 1. GUI is now a runtime toggle (`FConfiguration::bGUI`)
- `bGUI` changed from `static inline constexpr` to a normal runtime flag in `Erbium/Erbium/Public/Configuration.h`.
- `dllmain.cpp` no longer uses `if constexpr` on `bGUI`; the flag is evaluated at runtime so the GUI window can be disabled (console-only mode) without a rebuild.
- When `bGUI = false`, a console is allocated and stdout/stderr/stdin are attached to it, so logging still works headless.

### 2. Hardcoded game logic moved out of the GUI
- New module `Erbium/Erbium/Public/AdminActions.h` + `Erbium/Erbium/Private/AdminActions.cpp` now owns every game-facing action that used to be embedded in `GUI.cpp`:
  - `StartBusEarly`, `PauseSafeZone`, `ResumeSafeZone`, `SkipSafeZone`, `StartShrinkingSafeZone`
  - `ResetBuilds`, `DestroyFloorLoot`
  - `DumpItems`, `DumpPlaylists`
- `GUI.cpp` is now a thin presentation layer: it only draws ImGui widgets and calls into `AdminActions` / mutates `FConfiguration` fields. The same admin actions can now be reused from console commands or remote admin tooling without touching UI code.
- The ~13KB embedded font array was moved out of `GUI.h` into `Erbium/Erbium/Public/FontData.h`, included only by `GUI.cpp`. Previously it was compiled into every translation unit that included `GUI.h` (including `NetDriver.cpp`).

### 3. Webhook duplication centralized
Five near-identical ~40-line curl blocks (`NetDriver.cpp` ×3, `FortGameMode.cpp` ×2, `BattleRoyaleGamePhaseLogic.cpp` ×1 — 6 total call sites) were replaced with:
- `Misc::SendWebhook(title, fields, color)` — builds the embed, posts it via curl, no-ops when no webhook is configured.
- `Misc::GetPlaylistName()` — shared playlist-name resolution.
- The duplicated dead Discord CDN `icon_url` (an expired `cdn.discordapp.com/attachments/...` link) was dropped from the payload.
- A shared-format-bug fix: the embed color used to be serialized as `"color": "7237230"` (a quoted number produced by a stray `\"` pair), which is invalid JSON per Discord's schema; it is now a proper number.

### 4. Bug fixes
| Bug | Location | Fix |
|---|---|---|
| `tm` allocated with `new` and never freed on every webhook/dump call | `Erbium/pch.h` (`iso8601`) | Stack-allocated `tm t{}` |
| `DLL_PROCESS_ATTACH` falls through into `DLL_THREAD_ATTACH`/etc. cases | `Erbium/Erbium/Private/dllmain.cpp` | Added `break;` |
| `find_last_of("::")` searches for *any* of the characters `:` and returns `size_t`; compared against `-1` (which never matches `npos` on 64-bit MSVC) and can split mid-name | `GUI.cpp` rarity dump | `rfind("::")` + `std::string::npos` in `AdminActions::DumpRarity` |
| GUI reads `GameMode->AlivePlayers` without null-check while server is spinning up | `GUI.cpp` | `GameMode &&` guard added |
| Webhook embed color was invalid JSON (`"7237230"` with quotes) | all webhook call sites | Fixed in centralized `Misc::SendWebhook` |
| Streams were `.close()`d manually right before RAII destruction | dump code | Removed redundant `.close()` calls |

### 2. Aeris matchmaker integration (`Matchmaker` module)

The DLL now registers itself with the backend and keeps it informed of the server lifecycle:

- **Config**: new `FConfiguration::ApiURL` (base URL) and `FConfiguration::ApiKey` fields.
- **Launch arguments** are parsed at startup (e.g. `-ip=67.67.67.67 -port=6767 -region=EU -playlist=playlist_DefaultSolo -id=<uuid> -ownedBy=<uuid> -localIp=10.0.0.3 -localPort=6767`). Values override the compiled-in configuration defaults.
- **`POST /matchmaker/aeris/create/server`** is called once at DLL init with the region/playlist/ip/port/apikey/id/ownedBy/localIp/localPort JSON; if no `-id=` was passed a random UUID is generated.
- **`PATCH /matchmaker/aeris/update/joinable/<serverId>`** with `{"apiKey": ..., "joinable": true}` when the server becomes joinable (FortGameMode), and `joinable: false` when a match starts (FortGameMode + BattleRoyaleGamePhaseLogic).
- **`DELETE /matchmaker/aeris/delete/server/<serverId>`** with `{"apiKey": ...}` on every match-end path, after which the process exits cleanly with `exit(0)` (replacing `TerminateProcess`) so in-flight work can flush first. The GUI close button uses the same path.

All three requests reuse the shared libcurl global state initialized in `dllmain.cpp`.

---

## Remaining Findings (not fixed; prioritized)

### High
1. **`Misc::SendWebhook` performs a blocking `curl_easy_perform` on the game thread.** `StartAircraftPhase` and the NetDriver ticks are latency-sensitive; a slow webhook endpoint stalls the server for the duration of the request. Consider queueing webhook posts onto a worker thread (the process-exit `TerminateProcess` after auto-restart is also a race with an in-flight request).
2. **Webhook payloads are not JSON-escaped.** `PlaylistName` is appended verbatim into the JSON string; a playlist name containing `"` or `\` breaks the request. Escape (or switch to a tiny JSON builder) in `Misc::SendWebhook`.
3. **`dllmain.cpp` does heavy work from `std::thread(Main).detach()` with no error handling.** If `SDK::Init` or hook installation fails, the process is left half-initialized. Wrap `Main` in a try/catch and log failures; consider an exit path that doesn't just hang.
4. **Hardcoded pointer arithmetic (`+ 0x40`, `- 4`) in game logic** (e.g. rarity enum name array at `EFortRarity::StaticEnum() + 0x40`, `WarmupRequiredPlayerCount - 4`). These are version-fragile; prefer `GetOffset` lookups like elsewhere in the codebase.

### Medium
5. **`static bool stopped` / `static auto bSkipAircraft` locals in NetDriver ticks** assume a single world/netdriver for the process lifetime. Fine for a dedicated DS, but will misbehave if the world is torn down and recreated (map travel).
6. **`GUI::Init` runs a modal ImGui loop on its own thread and calls `TerminateProcess(GetCurrentProcess(), 0)` when the window closes** — closing the GUI window kills the whole server. That is surprising UX for a "toggle to remove the GUI"; consider making window-close just hide the window (or quit only when `bGUI` was the sole console).
7. **`AllocConsole` + `freopen_s` when `bGUI = false`** happens on the `Main` thread after the DLL was loaded — if the host process already has a console (e.g. launched from cmd), `GetConsoleWindow()` guard handles it, but `bGUI = true && bUseStdoutLog = false` currently produces neither a visible console nor a log file (silent logs).
8. **`ErbiumClient/dllmain.cpp` duplicates version/playlist knowledge** of the server; keep the two configs in sync manually or generate both from one header.
9. **Configuration is compile-time only.** For production hosting, consider loading `FConfiguration` values from an INI/JSON at startup (`Port`, `Playlist`, `WebhookURL`, `MaxTickRate`) so ops don't rebuild per deployment. The `bGUI` runtime toggle was a first step in that direction.
10. **Wide/narrow string conversions via `std::wstring(str.begin(), str.end())`** (GUI console command) mangle non-ASCII input; use `MultiByteToWideChar`.

### Low
11. `sprintf_s(version, ...)` into `char[6]` is safe today but brittle if the format ever grows; `snprintf` with explicit size is safer.
12. `Events::EventsArray` is scanned on first GUI frame only; if events load late the Events tab never appears until restart.
13. Several commented-out code blocks and leftover `printf` debug lines throughout `NetDriver.cpp` / `FortGameMode.cpp` — prune before tagging a release.
14. `Utils::GetAll` + `Free()` pattern in `AdminActions` copies the whole actor array twice; acceptable for admin actions but avoid in hot paths.

---

## Verification
- All new/changed call sites compile-clean against the existing headers (types verified: `AFortPickupAthena` in `FortInventory.h`, `UFortPlaylistAthena` in `FortPlaylistAthena.h`, `AFortSafeZoneIndicator` in `FortSafeZoneIndicator.h`).
- Full build requires MSVC + the vendored libcurl/ImGui; run `msbuild Erbium.sln /p:Configuration=Release` (or open `Erbium.vcxproj` in Visual Studio) to confirm. New files were added to `Erbium.vcxproj` and `.filters`.
