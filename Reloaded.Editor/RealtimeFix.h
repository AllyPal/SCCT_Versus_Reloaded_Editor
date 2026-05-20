#pragma once

extern int  g_ReloadedMaxFPS;
extern bool g_ReloadedMuteSounds;
extern bool g_ReloadedNoDuplicateOffset;

extern bool  g_SoftBodyFixedTimestepEnabled;
extern float g_SoftBodyFixedDT;

class RealtimeFix
{
public:
    static void Initialize();
};
