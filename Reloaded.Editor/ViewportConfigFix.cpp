#include "pch.h"
#include "ViewportConfigFix.h"
#include "Hooks.h"

INIT_HOOKS;

// Persisting viewport config reads RendMap and ShowFlags off the viewport's camera
// actor, which is absent while a level is being swapped.  Skip the viewport rather
// than substitute zeros: REN_None here makes the editor restore unusable viewports.
#define SKIP_VIEWPORT 0x10e4384a   // loop tail, past every write for this viewport

JMP_HOOK(0x10e436ad, ViewportRendMapGuard)
{
    static int Resume = 0x10e436b3;
    static int Skip   = SKIP_VIEWPORT;
    __asm {
        test edx, edx
        jz   skip
        mov  edx, dword ptr [edx + 0x3c]
        test edx, edx
        jz   skip
        mov  edx, dword ptr [edx + 0x30]
        test edx, edx
        jz   skip
        jmp  dword ptr [Resume]
    skip:
        jmp  dword ptr [Skip]
    }
}

JMP_HOOK(0x10e436de, ViewportShowFlagsGuard)
{
    static int Resume = 0x10e436e4;
    static int Skip   = SKIP_VIEWPORT;
    __asm {
        test edx, edx
        jz   skip
        mov  edx, dword ptr [edx + 0x3c]
        test edx, edx
        jz   skip
        mov  edx, dword ptr [edx + 0x30]
        test edx, edx
        jz   skip
        jmp  dword ptr [Resume]
    skip:
        jmp  dword ptr [Skip]
    }
}

void ViewportConfigFix::Initialize()
{
    INSTALL_HOOKS;
}
