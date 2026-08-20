#include "pch.h"
#include "StaticMeshCollisionFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"
#include "logger.h"
#include <format>

INIT_HOOKS;

// Fixes a crash when importing high poly static meshes.
//
// SCCT repacked the default UE2 collision BSP nodes to use signed 16-bit
// triangle/child indices. BuildCollisionBsp still produces 32-bit indices and
// silently truncates them when writing the tree.
//
// Since split triangles are duplicated into both children, BSP node count can
// greatly exceed triangle count (~8x in practice), eventually overflowing the
// 16-bit index range.
//
// The crash occurs in the post-build bounding-box pass. Child indices are
// sign-extended and only checked with `idx >= Num()`, while -1 is treated as
// the null sentinel. Wrapped negatives therefore pass validation, index before
// the node array, and eventually crash in FBox::IsValid().
//
// Fixes:
//   1. Cap the tree at 32767 nodes by returning BuildCollisionBsp's existing
//      empty-subtree result (-1), preventing wrapped indices.
//   2. Patch the bounds check from signed (JGE) to unsigned (JAE), rejecting
//      invalid indices in malformed collision trees.
//   3. Skip the FBox::IsValid() assert in that pass so a mesh saved before the cap
//      loads with degenerate bounds instead of taking the editor down.
//
// 32767 is the highest valid node index because child indices are signed
// 16-bit and -1 is reserved as "no child".
volatile unsigned char g_SMCollisionNodeCapHit = 0;

// BuildCollisionBsp(UStaticMesh* Mesh, INT* List, INT Count, BYTE* ClassMatrix,
//                   INT bSimplified, FArray* Boxes)
//
// Hooked over the initial `Count == 0` JZ. This is the earliest point where
// the function's prologue is complete, making its existing "return -1"
// epilogue safe to jump to.
//
// Preserves the original empty-list path, caps the node count at 32767, and
// restores ECX before rejoining because the displaced code expects it to be 0.
JMP_HOOK(0x1117257e, BuildCollisionBspNodeCap)
{
    static int s_return_empty = 0x11172b15;   // OR EAX,-1 / unwind / RET
    static int s_continue     = 0x11172584;   // instruction after the displaced JZ

    __asm
    {
        // Original behavior: empty triangle list -> return -1.
        jz      return_empty

        mov     eax, dword ptr [ebp + 0x18]     // bSimplified
        test    eax, eax
        mov     eax, dword ptr [ebp + 0x08]     // UStaticMesh*
        jz      load_main
        mov     eax, dword ptr [eax + 0x154]    // SimplifiedCollisionNodes.Num()
        jmp     have_count
    load_main:
        mov     eax, dword ptr [eax + 0x118]    // CollisionNodes.Num()
    have_count:
        cmp     eax, 32767                      // signed 16-bit node index ceiling
        jl      carry_on

        mov     byte ptr [g_SMCollisionNodeCapHit], 1
        jmp     dword ptr [s_return_empty]

    carry_on:
        xor     ecx, ecx                        // 0x11172595 expects ECX == 0
        jmp     dword ptr [s_continue]

    return_empty:
        jmp     dword ptr [s_return_empty]
    }
}

// A byte that is neither the original nor the patch means the address moved.
static bool PatchByte(uintptr_t address, uint8_t expected, uint8_t patched, const char* what)
{
    const uint8_t current = *reinterpret_cast<volatile uint8_t*>(address);
    if (current == patched)
        return true;

    if (current != expected)
    {
        Logger::log(std::format(
            "StaticMeshCollisionFix: unexpected byte at 0x{:08X}, skipping the {} patch", address, what));
        return false;
    }

    return MemoryWriter::WriteBytes(address, &patched, sizeof(patched));
}

bool StaticMeshCollisionFix::WasCollisionTruncated()
{
    return g_SMCollisionNodeCapHit != 0;
}

void StaticMeshCollisionFix::Initialize()
{
    INSTALL_HOOKS;

    PatchByte(0x11172469, 0x8D, 0x83, "collision node bounds check");  // 0F 8D condition byte: JGE -> JAE
    PatchByte(0x1117424D, 0x74, 0xEB, "degenerate bounds assert");     // JZ over the assert body -> JMP
}
