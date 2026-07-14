#include "pch.h"
#include "AmbientSoundZone.h"
#include "Hooks.h"
#include "logger.h"

INIT_HOOKS;

// AmbientSounds with bAffectOwnZoneOnly will now only play in the listener's zone
static const uint32_t kFn_UpdateAmbientSounds = 0x10F6D390;
static int            g_resumeAddr            = 0x10F6D395;

static const uint32_t kFn_UpdateVoiceLoop     = 0x10F6F056;
static int            g_voiceResumeAddr       = 0x10F6F05B;
static int            g_voiceStopAddr         = 0x10F6F0B9; // engine's voice-stop path

// Actor / subsystem layout
static const uint32_t kActor_ZoneFlags        = 0x2E8;   // dword holding bAffectOwnZoneOnly
static const uint32_t kMask_AffectOwnZoneOnly = 0x200;   // its bit
static const uint32_t kActor_RegionZone       = 0x1AC;   // AActor::Region.Zone
static const uint32_t kSub_ListenerZone       = 0x5C;    // listener PointRegion.Zone

// Returns nonzero if the ambient sound should be gated this frame
extern "C" static int __cdecl ShouldGateAmbientSound(void* actor, void* subsystem)
{
    if (actor == nullptr || subsystem == nullptr)
        return 0;

    __try
    {
        uint32_t flags =
            *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(actor) + kActor_ZoneFlags);

        if (flags & kMask_AffectOwnZoneOnly)
        {
            void* actorZone =
                *reinterpret_cast<void**>(reinterpret_cast<char*>(actor) + kActor_RegionZone);
            void* listenerZone =
                *reinterpret_cast<void**>(reinterpret_cast<char*>(subsystem) + kSub_ListenerZone);

            // If either zone is unknown, fall back to the default behavior
            if (actorZone != nullptr && listenerZone != nullptr && actorZone != listenerZone)
                return 1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

// Drop a gated actor before its sound starts
JMP_HOOK(kFn_UpdateAmbientSounds, UpdateAmbientSounds_Hook)
{
    __asm
    {
        pushad
        mov   eax, [esp + 0x28]          // arg2 = AActor* (0x20 pushad + retaddr + arg1)
        mov   ecx, [esp + 0x18]          // saved ECX = UUNIAudioSubsystem*
        push  ecx
        push  eax
        call  ShouldGateAmbientSound
        add   esp, 8
        test  al, al
        popad                            // POPAD does not modify EFLAGS
        jnz   gate_silence

        // Not gated: replay the 5 overwritten prologue bytes, resume original body
        push  ebp
        mov   ebp, esp
        push  -1
        jmp   dword ptr [g_resumeAddr]

    gate_silence:
        // Skip adding the actor to the audible list, so its sound never starts
        // Original is __thiscall, cleans 0x10
        ret   0x10
    }
}

// Stop a playing voice once its actor is gated
JMP_HOOK(kFn_UpdateVoiceLoop, AmbientSoundVoiceGate_Hook)
{
    __asm
    {
        pushad
        mov   eax, [edx + 0x10]          // eax = voice.Actor (EDX = &voice on entry)
        push  ecx
        push  eax
        call  ShouldGateAmbientSound
        add   esp, 8
        test  al, al
        popad                            // POPAD does not modify EFLAGS
        jnz   voice_stop

        // Not gated: replay the 5 overwritten bytes, resume original body
        mov   esi, [edx + 0x10]
        test  esi, esi
        jmp   dword ptr [g_voiceResumeAddr]

    voice_stop:
        jmp   dword ptr [g_voiceStopAddr]
    }
}

void AmbientSoundZone::Initialize()
{
    INSTALL_HOOKS;
}
