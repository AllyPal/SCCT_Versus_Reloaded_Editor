#include "pch.h"
#include "ProjectorDetachFix.h"
#include "Hooks.h"

INIT_HOOKS;

// Fixes a crash on editor shutdown after a map containing Projector actors
// has been built.
//
// SCCT's AProjector::Detach unregisters its RenderInfo from a level-owned
// registry. During StaticExit the level has already been destroyed, so the
// registry scan dereferences freed memory.
//
// A projector only reaches this path after it has attached a RenderInfo
// (normally after Build All), which is why maps that were never built exit
// cleanly.
//
// During UObject::StaticExit, GExitPurge is set before PurgeGarbage runs.
// Skip the registry removal during this teardown path and jump directly to
// the existing cleanup tail, which clears RenderInfo without touching the
// destroyed level.
//
#define GEXITPURGE_ADDRESS 0x11693314

// Hook before RenderInfo/level access. The displaced instructions are restored
// on the normal path. During GExitPurge, skip registry removal and jump to the
// existing cleanup tail to avoid touching the destroyed level.
JMP_HOOK(0x110e2187, ProjectorDetachExitGuard)
{
    static int s_gexitpurge      = GEXITPURGE_ADDRESS;
    static int s_continue        = 0x110e2190;   // test ecx, ecx
    static int s_clear_and_return= 0x110e2214;   // mov dword ptr [edx+464h], 0

    __asm
    {
        mov     eax, dword ptr [s_gexitpurge]
        cmp     dword ptr [eax], 0
        jne     shutting_down

        // Restore displaced instructions
        mov     eax, dword ptr [ebp - 0x14]     // this
        mov     ecx, dword ptr [eax + 0x464]    // RenderInfo
        jmp     dword ptr [s_continue]

    shutting_down:
        // Skip registry removal during exit purge
        mov     edx, dword ptr [ebp - 0x14]
        jmp     dword ptr [s_clear_and_return]
    }
}

void ProjectorDetachFix::Initialize()
{
    INSTALL_HOOKS;
}
