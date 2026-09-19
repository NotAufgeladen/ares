#pragma once

class UNetDriver;

namespace Moderation
{
    // Preserves ARES' normal kick suppression while retaining a trampoline to
    // the native GameSession::KickPlayer implementation for moderator actions.
    void Initialize();

    // Executes queued moderation commands on Unreal's game thread.
    void Tick(UNetDriver* Driver);
}
