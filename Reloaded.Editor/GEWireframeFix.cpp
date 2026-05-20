#include "pch.h"
#include "GEWireframeFix.h"
#include "Hooks.h"

INIT_HOOKS;

// Fix ClickHActor crash in wireframe mode (brush actors only).
//
// Brush GE path performs unsafe double dereference of HActor chain,
// where intermediate pointer becomes NULL in wireframe rendering.
// StaticMesh path does not exhibit this behavior.
//
// Fix adds validation before second dereference and exits cleanly
// via function epilogue when invalid.
JMP_HOOK(0x10ed28fd, GEWireframeNullActorFix_PathA)
{
    static int return_to_cmp = 0x10ed2903;
    static int clean_exit    = 0x10ed3319;

    __asm {
        mov  ebx, dword ptr [ebp + 0xc]
        mov  eax, dword ptr [ebx + 0xc]
        test eax, eax
        jz   actor_null_a
        jmp  dword ptr [return_to_cmp]
    actor_null_a:
        jmp  dword ptr [clean_exit]
    }
}

// Wireframe brush GE crash fix (Paths A & B).
//
// Brush hit data uses a two-level pointer chain (HActor > P1 > P2).
// In wireframe mode, P2 may be NULL while P1 is valid.
//
// Fix: fallback to P1 when P2 is NULL to prevent dereference crash.
JMP_HOOK(0x10ed2d8b, GEWireframeDoubleDerefFix_PathA)
{
    static int return_to_next = 0x10ed2d91;
    static int clean_exit     = 0x10ed3319;

    __asm {
        mov  edx, dword ptr [ebx + 4]
        test edx, edx
        jz   deref_null_a
        mov  eax, dword ptr [edx + 4]
        test eax, eax
        jnz  deref_ok_a
        mov  eax, edx
    deref_ok_a:
        jmp  dword ptr [return_to_next]
    deref_null_a:
        jmp  dword ptr [clean_exit]
    }
}

JMP_HOOK(0x10ed3866, GEWireframeNullActorFix_PathB)
{
    static int return_to_cmp = 0x10ed386c;
    static int clean_exit    = 0x10ed3319;

    __asm {
        mov  ecx, dword ptr [ebp + 0xc]
        mov  eax, dword ptr [ecx + 0xc]
        test eax, eax
        jz   actor_null_b
        jmp  dword ptr [return_to_cmp]
    actor_null_b:
        jmp  dword ptr [clean_exit]
    }
}

JMP_HOOK(0x10ed3ef6, GEWireframeDoubleDerefFix_PathB)
{
    static int return_to_next = 0x10ed3efc;
    static int clean_exit     = 0x10ed3319;

    __asm {
        mov  edx, dword ptr [ecx + 4]
        test edx, edx
        jz   deref_null_b
        mov  eax, dword ptr [edx + 4]
        test eax, eax
        jnz  deref_ok_b
        mov  eax, edx
    deref_ok_b:
        jmp  dword ptr [return_to_next]
    deref_null_b:
        jmp  dword ptr [clean_exit]
    }
}

void GEWireframeFix::Initialize()
{
    INSTALL_HOOKS;
}
