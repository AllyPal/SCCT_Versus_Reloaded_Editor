#pragma once

extern int  g_ReloadedMaxFPS;
extern bool g_ReloadedMuteSounds;
extern bool g_ReloadedNoDuplicateOffset;
extern bool g_ReloadedMinimizeOnPlay;

// Cloth constraint and damping run per step, so it stiffens with frame rate.
extern bool  g_SoftBodyStepRateCapEnabled;
extern float g_SoftBodyMinStepSeconds;

class RealtimeFix
{
public:
    static void Initialize();
};
