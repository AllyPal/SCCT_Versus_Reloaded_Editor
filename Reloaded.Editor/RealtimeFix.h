#pragma once

extern int  g_ReloadedMaxFPS;
extern bool g_ReloadedMuteSounds;
extern bool g_ReloadedNoDuplicateOffset;

// Soft body (ESoftBodyActor / UESoftBody) fixed-timestep substep.
// The integrator is FPS-dependent and was tuned for 30 Hz; at higher framerates
// the Verlet velocity term + per-tick constraint relaxation make it jitter and
// explode. We accumulate dt per-instance and re-tick the original Update() at
// a fixed 1/30 s step so the cloth motion stays at its 30 Hz pace.
extern bool  g_SoftBodyFixedTimestepEnabled;
extern float g_SoftBodyFixedDT;        // seconds per substep (default 1/30)

class RealtimeFix
{
public:
    static void Initialize();
};
