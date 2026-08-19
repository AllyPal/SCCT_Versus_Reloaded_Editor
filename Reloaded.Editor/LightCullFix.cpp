#include "pch.h"
#include "LightCullFix.h"
#include "Hooks.h"

INIT_HOOKS;

// Lights now obey CullDistance and ForcedVisibilityZoneTag in the viewport, which the engine
// applies to drawing an actor but never to gathering lights. Both are opt in, so an unedited map
// behaves as before.

// AActor properties
#define ACTOR_LOCATION            0x80
#define ACTOR_REGION              0x1AC   // PointRegion, Zone is its first member
#define ACTOR_CULLDISTANCE        0x254
#define ACTOR_VISIBILITYZONETAG   0x2B0
#define ACTOR_TAG                 0x2BC   // what the engine matches the zone tag against

// The gather's frame argument is the scene node itself, not a pointer to one
#define SCENE_VIEWPORT            0x04
#define SCENE_VIEWORIGIN          0x194
#define VIEWPORT_ACTOR            0x30
#define VIEWPORT_NOCULL           0x170   // set by the view modes that draw regardless of distance

// 100.0f as IEEE-754 bits, the floor the sprite alone is held to
#define ICON_CULL_FLOOR           0x42C80000

static int __cdecl ShouldCullLight(void* actor, void* frame)
{
    __try
    {
        const float cullDistance = *(float*)((char*)actor + ACTOR_CULLDISTANCE);
        const unsigned int zoneTag = *(unsigned int*)((char*)actor + ACTOR_VISIBILITYZONETAG);
        if (frame == nullptr || (cullDistance <= 0.0f && zoneTag == 0))
            return 0;

        void* viewport = *(void**)((char*)frame + SCENE_VIEWPORT);
        void* camera = viewport == nullptr ? nullptr : *(void**)((char*)viewport + VIEWPORT_ACTOR);
        if (camera == nullptr || *(int*)((char*)viewport + VIEWPORT_NOCULL) == 1)
            return 0;

        if (cullDistance > 0.0f)
        {
            const float* view = (const float*)((char*)frame + SCENE_VIEWORIGIN);
            const float* origin = (const float*)((char*)actor + ACTOR_LOCATION);
            const float x = view[0] - origin[0];
            const float y = view[1] - origin[1];
            const float z = view[2] - origin[2];
            if (cullDistance * cullDistance < x * x + y * y + z * z)
                return 1;
        }

        // Viewer-relative, where the engine's own use of this property is light-relative
        if (zoneTag != 0)
        {
            void* zone = *(void**)((char*)camera + ACTOR_REGION);
            if (zone == nullptr || *(unsigned int*)((char*)zone + ACTOR_TAG) != zoneTag)
                return 1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

// Replaces the brightness test that opens the gather's accept block. EBX is the light actor and
// [EBP+8] the frame. Culling jumps to the engine's own contributes-nothing exit, so the light is
// never listed and no frustum, shadow or geometry pass follows.
JMP_HOOK(0x111ab64e, LightGatherCullGate)
{
    static int s_resume = 0x111ab654;   // the FCOMP the brightness test continues into
    static int s_reject = 0x111ab6cd;

    __asm
    {
        pushad
        mov   eax, [ebp + 0x8]           // frame
        push  eax
        push  ebx                        // light actor
        call  ShouldCullLight
        add   esp, 8
        test  al, al
        popad                            // POPAD does not modify EFLAGS
        jnz   culled

        // Not culled: replay the displaced FLD and resume in the brightness test
        fld   dword ptr [ebx + 0xF8]
        jmp   dword ptr [s_resume]

    culled:
        jmp   dword ptr [s_reject]
    }
}

// Replaces the engine actor cull's comparison set-up, dropping the two FMULs that applied
// tan(FovAngle / 2) and clamping CullDistance up to the sprite floor. ST0 carries the squared
// distance through untouched, so the clamp compares raw float bits as unsigned: positive floats
// order the same either way, and zero or negative falls outside the range and is left alone.
JMP_HOOK(0x111a3e94, ActorCullDistanceFloor)
{
    static int   s_resume = 0x111a3eb0;
    static float s_cullDistance = 0.0f;
    static float s_zero = 0.0f;

    __asm
    {
        mov   esi, [esi]                 // the primitive's actor
        mov   eax, [esi + ACTOR_CULLDISTANCE]
        cmp   eax, ICON_CULL_FLOOR
        jae   store
        mov   eax, ICON_CULL_FLOOR
    store:
        mov   dword ptr [s_cullDistance], eax

        fld   dword ptr [s_cullDistance]
        fcomp dword ptr [s_zero]         // the sign test FNSTSW reads back
        fld   dword ptr [s_cullDistance]
        jmp   dword ptr [s_resume]
    }
}

void LightCullFix::Initialize()
{
    INSTALL_HOOKS;
}
