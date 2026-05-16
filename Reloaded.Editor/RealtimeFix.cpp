#include "pch.h"
#include "RealtimeFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"

INIT_HOOKS;

// ============================================================================
//  TASK 1 – Remove 30 FPS realtime viewport cap  (two Sleep sites)
// ============================================================================
//
//  Site A – on-demand / non-realtime viewport tick (FUN_10f0ccf0):
//    Throttles by sleeping when elapsed frame time < 1000/MaxFPS ms.
//    PUSH EAX + CALL [IAT_Sleep] at 0x10f0cd92 (7 bytes) → 7 × NOP.
//
//  Site B – realtime viewport SleepFloat wrapper (FUN_10fd96f0):
//    Converts a float seconds argument to ms and calls Sleep; invoked every
//    realtime-preview tick, capping it at 30 FPS independently of Site A.
//    PUSH EAX + CALL [IAT_Sleep] at 0x10fd96ff (7 bytes).
//    Rather than NOPing entirely, Task 3 replaces these 7 bytes with a
//    5-byte CALL to RealtimeCapThrottle + 2 trailing NOPs.
//
//  In both cases Sleep is __stdcall (callee-cleanup via RET 4); NOPing only
//  the CALL would leave the argument PUSH live and unbalance the stack by
//  4 bytes.  Both the PUSH and the CALL must be replaced.
//
//  50 FF 15 C8 1F AF 11  →  90 90 90 90 90 90 90  (Site A)
//  50 FF 15 C8 1F AF 11  →  E8 xx xx xx xx 90 90  (Site B – see Task 3)
//
// ============================================================================
//  TASK 2 – Animated texture / cubemap frame-rate limiter
// ============================================================================
//
//  UTexture::Tick (FUN_11096da0 at 0x11096da0, vtable slot 0xA0) is shared by
//  both the UTexture vtable (0x114914d8) and UCubemap vtable (0x114915d0).
//
//  When MaxFrameRate (this+0x88) is 0.0f the function calls LoadNextAnim
//  (vtable[0xAC]) unconditionally every editor frame – making animated texture
//  frames advance at the full, uncapped realtime rate.
//
//  Disassembly of the MaxFrameRate==0 branch we replace (10 bytes):
//
//    11096de8: 8B 16              MOV  EDX,[ESI]            ; vtable ptr
//    11096dea: 8B CE              MOV  ECX,ESI              ; this
//    11096dec: FF 92 AC 00 00 00  CALL dword ptr [EDX+0ACh] ; LoadNextAnim – every frame!
//    11096df2: ... epilogue / RET 4 ...
//
//  We write a 5-byte JMP at 0x11096de8 and NOP the remaining 5 bytes.
//  The hook accumulates DeltaSeconds into Accumulator (this+0x8C) and only
//  calls LoadNextAnim once the accumulator reaches 1/33.333 s (~0.030 s,
//  ~33 fps).  After the call (or skip) execution rejoins the epilogue at
//  0x11096df2.
//
//  Register state guaranteed by Tick's own prologue when the hook fires:
//    ESI     = this               (MOV ESI,ECX at 0x11096dbd)
//    EBP     = Tick's frame       (PUSH EBP / MOV EBP,ESP at 0x11096da0)
//    [EBP+8] = DeltaSeconds       (first __thiscall float stack arg)
//    [EBP-C] = saved FS:[0]       (SEH chain; epilogue at 0x11096df2 uses it)
//
//  Note on vtable offset: the user spec cites LoadNextAnim at vtable offset
//  0xA4, but disassembly of the Tick body shows the actual call is
//  [EDX+0xAC].  Slot 0xA4 holds a sibling helper (ConstantTimeTick); the
//  correct runtime call is at 0xAC.  Both are addressed here accordingly.
// ============================================================================

// ============================================================================
//  TASK 3 – Enforce 240 FPS cap on the realtime viewport
// ============================================================================
//
//  Site B's 7-byte block at 0x10fd96ff is patched to:
//    E8 xx xx xx xx   CALL RealtimeCapThrottle   (5 bytes, via CALL_HOOK)
//    90 90            NOP NOP                     (2 trailing bytes)
//
//  RealtimeCapThrottle (naked) calls DoRealtimeCap (__cdecl) and RETs.
//  DoRealtimeCap uses QueryPerformanceCounter to throttle the realtime
//  viewport to at most REALTIME_MAX_FPS frames per second.
//
//  The throttle coarse-sleeps for most of the wait period, then busy-spins
//  for sub-millisecond precision so the target is hit accurately without
//  over-sleeping.
//
//  Register discipline at 0x10fd96ff (inside SleepFloat's body):
//    EBP      – saved by SleepFloat's own prologue; untouched by our hook.
//    EAX      – held the computed ms value about to be pushed; caller-saved,
//               freely clobberable.
//    ECX/EDX  – caller-saved, freely clobberable (WINAPI uses them internally).
//    EBX/ESI/EDI – callee-saved; MSVC preserves them automatically in
//               DoRealtimeCap and in every WINAPI call it makes.
//
// ============================================================================

// Exposed via RealtimeFix.h — written by ReloadedOptions dialog, read every frame.
int  g_ReloadedMaxFPS          = 240;
bool g_ReloadedMuteSounds      = false;
bool g_ReloadedNoDuplicateOffset = false;

static LARGE_INTEGER s_rtFreq      = {};
static LARGE_INTEGER s_rtLastFrame = {};
static bool          s_rtReady     = false;

static void __cdecl DoRealtimeCap()
{
    // 0 = unlimited: bypass the throttle entirely.
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
        // Coarse sleep: let the scheduler handle most of the wait.
        // Subtract 1 ms to leave headroom for the sub-ms busy-spin below.
        const DWORD sleepMs =
            static_cast<DWORD>(remaining * 1000 / s_rtFreq.QuadPart);
        if (sleepMs > 1)
            Sleep(sleepMs - 1);

        // Busy-spin for sub-millisecond precision.
        do { QueryPerformanceCounter(&now); }
        while ((now.QuadPart - s_rtLastFrame.QuadPart) < ticksPerFrame);
    }
    else
    {
        QueryPerformanceCounter(&now);
    }

    s_rtLastFrame = now;
}

// CALL_HOOK writes a 5-byte CALL at 0x10fd96ff (inside SleepFloat, where the
// original PUSH EAX + CALL [Sleep] lived).  The naked stub calls DoRealtimeCap
// then RETs back into SleepFloat at 0x10fd9704 (2 trailing NOPs → epilogue).
CALL_HOOK(0x10fd96ff, RealtimeCapThrottle)
{
    __asm
    {
        call    DoRealtimeCap
        ret
    }
}

static float s_animTickThreshold = 1.0f / 33.333333f;  // ~0.030 012 s  (~33 fps)

JMP_HOOK(0x11096de8, TexTickAccumulate)
{
    static int s_epilogue = 0x11096df2;

    __asm
    {
        // ── Accumulate DeltaSeconds into Accumulator (this+0x8C) ────────────
        fld     dword ptr [ebp + 8]             // ST0 = DeltaSeconds
        fadd    dword ptr [esi + 0x8c]          // ST0 = Accumulator + DeltaSeconds

        // ── Compare with threshold before committing ─────────────────────────
        // FCOM compares ST(0) with m32fp without popping.
        // C0=1 → ST(0) < threshold  (below)
        // C0=0 → ST(0) ≥ threshold  (at or above)
        fcom    dword ptr [s_animTickThreshold]
        fstp    dword ptr [esi + 0x8c]          // store new Accumulator; pop ST0

        fnstsw  ax
        // TEST AH,0x05 isolates C0 (bit 0) and C2 (bit 2).
        // below threshold  → result 0x01 → 1 bit set → PF=0 → JNP taken  → done
        // at/above threshold → result 0x00 → 0 bits  → PF=1 → JNP not taken → call
        test    ah, 0x05
        jnp     done                            // below threshold – skip this frame

        // ── Threshold met: advance animation and reset accumulator ───────────
        mov     edx, dword ptr [esi]            // vtable ptr
        mov     ecx, esi                        // this
        call    dword ptr [edx + 0xac]          // LoadNextAnim  (vtable slot 0xAC)

        xor     eax, eax
        mov     dword ptr [esi + 0x8c], eax     // Accumulator = 0.0f

    done:
        jmp     dword ptr [s_epilogue]          // → Tick epilogue (0x11096df2)
    }
}

// ============================================================================
//  TASK 4 – Mute viewport sounds in Realtime Preview
// ============================================================================
//
//  UUNIAudioSubsystem::Update (FUN_10f6e980) is called every realtime-preview
//  frame to register and update ambient/world sounds.  Skipping it prevents
//  any new viewport sounds from being submitted to DirectSound.
//
//  The Sound Browser drives playback through a separate vtable slot
//  (PlaySound, FUN_10f6f030 area) and is therefore unaffected.
//
//  Prologue bytes patched (5): 55 8B EC 6A FF
//    PUSH EBP  /  MOV EBP,ESP  /  PUSH -1   (standard MSVC C++ SEH prologue)
//  Calling convention: __thiscall, one FSceneNode* arg on stack → RET 4.
//  Resume point after the 5-byte JMP: 0x10f6e985.
//
// ============================================================================

JMP_HOOK(0x10f6e980, AudioUpdateMuteHook)
{
    static int s_resume = 0x10f6e985;  // first byte after our 5-byte JMP

    __asm
    {
        // Skip the entire Update when sounds are muted.
        cmp  byte ptr [g_ReloadedMuteSounds], 0
        jz   not_muted

        // Muted: return immediately.  One 4-byte arg on stack → RET 4.
        ret  4

    not_muted:
        // Re-execute the 5 original prologue bytes, then continue.
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]
    }
}

void RealtimeFix::Initialize()
{
    // ── Tasks 2 & 3: install JMP hook (TexTickAccumulate) and
    //                 CALL hook (RealtimeCapThrottle) ──────────────────────
    // INSTALL_HOOKS writes:
    //   • A 5-byte JMP  at 0x11096de8 → TexTickAccumulate    (Task 2)
    //   • A 5-byte CALL at 0x10fd96ff → RealtimeCapThrottle  (Task 3)
    INSTALL_HOOKS;

    // ── Task 2: NOP the 5 tail bytes of the original 10-byte block ──────────
    //  WriteJump above covered 0x11096de8–0x11096dec (5 bytes); bytes
    //  0x11096ded–0x11096df1 were part of the old CALL [EDX+0xAC] and must
    //  be inert so a debugger or exception handler doesn't misread them.
    uint8_t nop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x11096ded, nop5, 5);

    // ── Task 1a: NOP the PUSH EAX + CALL [Sleep] in the on-demand viewport tick ──
    //  0x10f0cd92: 50                -- PUSH EAX  (Sleep's __stdcall argument)
    //  0x10f0cd93: FF 15 C8 1F AF 11 -- CALL dword ptr [IAT_Sleep]
    //  Sleep uses RET 4 (callee-cleanup).  NOPing only the CALL leaves the PUSH
    //  live, unbalancing the stack by 4 bytes before the Draw call.  NOP both.
    uint8_t nop7[7] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10f0cd92, nop7, 7);

    // ── Task 1b / Task 3: NOP the 2 trailing bytes of Site B's 7-byte block ──
    //  INSTALL_HOOKS wrote a 5-byte CALL at 0x10fd96ff (Task 3), covering
    //  the original PUSH EAX (1 byte) and the first 4 bytes of CALL [Sleep].
    //  The remaining 2 bytes of the 6-byte CALL [IAT_Sleep] opcode sit at
    //  0x10fd9704–0x10fd9705 and must be NOPed so they are unreachable junk.
    uint8_t nop2[2] = { 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10fd9704, nop2, 2);
}
