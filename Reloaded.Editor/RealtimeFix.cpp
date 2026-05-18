#include "pch.h"
#include "RealtimeFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"

INIT_HOOKS;

int  g_ReloadedMaxFPS            = 120;
bool g_ReloadedMuteSounds        = false;
bool g_ReloadedNoDuplicateOffset = false;

static LARGE_INTEGER s_rtFreq      = {};
static LARGE_INTEGER s_rtLastFrame = {};
static bool          s_rtReady     = false;

static void __cdecl DoRealtimeCap()
{
    if (g_ReloadedMaxFPS <= 0)
        return;

    if (!s_rtReady)
    {
        QueryPerformanceFrequency(&s_rtFreq);
        QueryPerformanceCounter(&s_rtLastFrame);
        s_rtReady = true;
        return;
    }

    const LONGLONG ticksPerFrame =
        s_rtFreq.QuadPart / static_cast<LONGLONG>(g_ReloadedMaxFPS);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const LONGLONG remaining = ticksPerFrame - (now.QuadPart - s_rtLastFrame.QuadPart);
    if (remaining > 0)
    {
        const DWORD sleepMs =
            static_cast<DWORD>(remaining * 1000 / s_rtFreq.QuadPart);
        if (sleepMs > 1)
            Sleep(sleepMs - 1);

        do { QueryPerformanceCounter(&now); }
        while ((now.QuadPart - s_rtLastFrame.QuadPart) < ticksPerFrame);
    }
    else
    {
        QueryPerformanceCounter(&now);
    }

    s_rtLastFrame = now;
}

CALL_HOOK(0x10fd96ff, RealtimeCapThrottle)
{
    __asm
    {
        call    DoRealtimeCap
        ret
    }
}

// Animated texture fix
static float s_animTickThreshold = 1.0f / 33.333333f;  

JMP_HOOK(0x11096de8, TexTickAccumulate)
{
    static int s_epilogue = 0x11096df2;

    __asm
    {
        fld     dword ptr [ebp + 8]
        fadd    dword ptr [esi + 0x8c]
        fcom    dword ptr [s_animTickThreshold]
        fstp    dword ptr [esi + 0x8c]
        fnstsw  ax
        test    ah, 0x05
        jnp     done

        mov     edx, dword ptr [esi]
        mov     ecx, esi
        call    dword ptr [edx + 0xac]

        xor     eax, eax
        mov     dword ptr [esi + 0x8c], eax

    done:
        jmp     dword ptr [s_epilogue]
    }
}

// Mute viewport sounds in Realtime Preview by skipping UUNIAudioSubsystem::Update
JMP_HOOK(0x10f6e980, AudioUpdateMuteHook)
{
    static int s_resume = 0x10f6e985;

    __asm
    {
        cmp  byte ptr [g_ReloadedMuteSounds], 0
        jz   not_muted
        ret  4

    not_muted:
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]
    }
}

void RealtimeFix::Initialize()
{
    INSTALL_HOOKS;

    // Throttle animated textures
    uint8_t nop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x11096ded, nop5, 5);

    // NOP the Sleep call in on-demand viewport tick
    // Must NOP both PUSH + CALL (RET 4) to avoid unbalancing the stack
    uint8_t nop7[7] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10f0cd92, nop7, 7);

    // NOP the 2 trailing bytes of the original Sleep call,
    // already handled by RealtimeCapThrottle hook
    uint8_t nop2[2] = { 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10fd9704, nop2, 2);
}
