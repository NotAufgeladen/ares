#pragma once
#include "../../pch.h"
#include "Utils.h"
#include <string>
#include <utility>
#include <vector>

// these dont really fit into a class
class Misc
{
public:
    static inline bool bHookedAll = false;

    // Sends an embed to FConfiguration::WebhookURL. Only used when a webhook is
    // configured; every call site used to duplicate this curl boilerplate.
    static void SendWebhook(const std::string& Title, const std::vector<std::pair<std::string, std::string>>& Fields, int Color = 7237230);
    static std::string GetPlaylistName();

    static int GetNetMode();
    DefHookOg(void*, SendRequestNow, void*, void*, int);
    DefHookOg(float, GetMaxTickRate, UEngine*, float, bool);
    static uint32 CheckCheckpointHeartBeat();
    DefHookOg(void, ApplyHomebaseEffectsOnPlayerSetup, __int64*, __int64, __int64, __int64, UObject*, char, unsigned __int8);
    static void InitClient();

    InitHooks;
};