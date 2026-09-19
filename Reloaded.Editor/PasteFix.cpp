#include "pch.h"
#include "PasteFix.h"
#include "Hooks.h"

INIT_HOOKS;

// FVector::GridSnap(const FVector& GridSz) const
// __thiscall with a hidden return buffer as the first argument
static const uintptr_t kFVectorGridSnap = 0x10E05A98;

// Exec_Edit's "Paste > Here" / "At World Origin" operation
// Snapping the delta rather than the destination keeps the group's spacing and leaves on-grid geometry on grid
JMP_HOOK(0x10EF449F, PasteAdjustGridSnap)
{
    static int   s_resume     = 0x10EF44C0;
    static int   s_gridSnap   = static_cast<int>(kFVectorGridSnap);
    static float s_snapped[3] = { 0.0f, 0.0f, 0.0f };

    __asm
    {
        // Adjust = Origin [ebp-0x24] minus bbox center [ebp-0x3c], written over Origin
        fld  dword ptr [ebp - 0x24]
        fsub dword ptr [ebp - 0x3c]
        fstp dword ptr [ebp - 0x24]
        fld  dword ptr [ebp - 0x20]
        fsub dword ptr [ebp - 0x38]
        fstp dword ptr [ebp - 0x20]
        fld  dword ptr [ebp - 0x1c]
        fsub dword ptr [ebp - 0x34]
        fstp dword ptr [ebp - 0x1c]

        test byte ptr [edi + 0x1cc], 1      // Constraints.GridEnabled
        jz   done

        mov  eax, edi
        add  eax, 0x1d4                     // &Constraints.GridSize
        push eax
        lea  edx, [s_snapped]
        push edx
        lea  ecx, [ebp - 0x24]
        call dword ptr [s_gridSnap]

        lea  edx, [s_snapped]
        mov  eax, dword ptr [edx]
        mov  dword ptr [ebp - 0x24], eax
        mov  eax, dword ptr [edx + 4]
        mov  dword ptr [ebp - 0x20], eax
        mov  eax, dword ptr [edx + 8]
        mov  dword ptr [ebp - 0x1c], eax

    done:
        // The apply loop wants its counter zeroed and Adjust.Z on the x87 stack.
        xor  ecx, ecx
        fld  dword ptr [ebp - 0x1c]
        jmp  dword ptr [s_resume]
    }
}

void PasteFix::Initialize()
{
    INSTALL_HOOKS;
}
