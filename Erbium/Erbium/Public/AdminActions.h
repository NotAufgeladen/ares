#pragma once

// All game-facing admin actions used by the GUI live here so the GUI itself
// contains no game logic and the same actions can be reused (e.g. from console
// commands or remote admin tools) without touching the UI layer.

class AdminActions
{
public:
    static void StartBusEarly();
    static void PauseSafeZone();
    static void ResumeSafeZone();
    static void SkipSafeZone();
    static void StartShrinkingSafeZone();
    static void ResetBuilds();
    static void DestroyFloorLoot();
    static void DumpItems();
    static void DumpPlaylists();
};
