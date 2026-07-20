#include "pch.h"
#include "DeintersectFix.h"
#include "Hooks.h"

INIT_HOOKS;

// Fixes the guaranteed crash in "Brush From Deintersection" (and the same
// latent crash in "Brush From Intersection").
//
// SCCT Versus extends bspAddNode to save a copy of the source FPoly by
// directly copying:
//
//     EdPoly->Actor->Brush->Polys->Element.Data[EdPoly->iBrushPoly]
//
// into a 0x154-byte "saved poly" attached to each new FBspSurf. Stock UE2
// never dereferences Actor/iBrushPoly here.
//
// During Intersect/Deintersect, Brush->EmptyModel() frees the brush's poly
// array before bspBuild() creates new surfaces. The copied Actor/iBrushPoly
// still refer to the original brush, so the saved-poly copy can dereference
// NULL (most commonly) or index past the rebuilt array.
//
// Validate the source chain and iBrushPoly before the copy.
// On failure, use the engine's Actor==NULL fallback (0x1108727F),
// which FPoly::Init()s the saved poly.
//
// Entry:
//   EDI = SavedPoly, EBX = new FBspSurf.
JMP_HOOK(0x11087190, BspAddNodeSavedPolyGuard)
{
    static int s_copy_done = 0x110871bf;
    static int s_init_poly = 0x1108727f;

    __asm {
        mov  eax, dword ptr [edi + 0x14C]   // SavedPoly->SourceActor
        test eax, eax
        jz   use_init

        mov  edx, dword ptr [eax + 0x238]
        test edx, edx
        jz   use_init

        mov  ecx, dword ptr [edx + 0x50]
        test ecx, ecx
        jz   use_init

        mov  esi, dword ptr [ecx + 0x28]
        test esi, esi
        jz   use_init

        mov  edx, dword ptr [edi + 0x150]
        test edx, edx
        js   use_init
        cmp  edx, dword ptr [ecx + 0x2C]
        jge  use_init

        imul edx, edx, 0x14C // sizeof(FPoly)
        add  esi, edx
        mov  ecx, 0x53       // sizeof(FPoly) / 4
        rep  movsd
        jmp  dword ptr [s_copy_done]

    use_init:
        // Clear stale source info and use the engine fallback
        mov  dword ptr [edi + 0x14C], 0
        mov  dword ptr [edi + 0x150], -1
        jmp  dword ptr [s_init_poly]
    }
}

void DeintersectFix::Initialize()
{
    INSTALL_HOOKS;
}
