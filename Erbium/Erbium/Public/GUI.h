#pragma once

enum EGSStatus
{
    NotReady,
    Joinable,
    StartedMatch
};

class GUI
{
public:
    static inline char windowTitle[67];
    static inline EGSStatus gsStatus = NotReady;
    static void Init();
};
