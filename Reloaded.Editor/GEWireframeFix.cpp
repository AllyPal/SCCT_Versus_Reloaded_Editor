#include "pch.h"
#include "GEWireframeFix.h"
#include "Hooks.h"

INIT_HOOKS;

// ============================================================
// FUN_10ed2890 - ClickHActor (SCCT build 2110)
//
// When a GeometricEvent (GE) is placed on a brush actor in
// wireframe view, ClickHActor crashes with a GPF.  StaticMesh
// actors work fine in wireframe; the crash is brush-exclusive.
//
// ROOT CAUSE - double dereference of HActor+4
// -----------------------------------------------
// After the brush-type (0x3) and model-not-null checks pass,
// both GE code paths extract position data via:
//
//   P1 = *(HActor + 4)          -- non-null (proxy parent ref)
//   P2 = *(P1 + 4)              -- NULL for brush in wireframe
//   pos = *(P2 + 0xc)           -- GPF: read through NULL
//
// StaticMesh actors use a single dereference (*(P1+0xc)) and
// are unaffected.
//
// HOOKS
// -----
// Four hooks total; two per GE code path:
//
//   [1] Null-Actor guard  -- defensive: exits if Actor* itself
//       is NULL (should not occur in practice but keeps both
//       paths safe regardless of render mode).
//
//   [2] Double-deref guard -- the actual crash fix: tests P1
//       and P2 before either is dereferenced; redirects to the
//       function epilogue when P2 is NULL.
//
// GE type routing:
//   PATH A  flag 0xdbc  -- LedgeGrab
//   PATH B  flags 0xdb8/0xdb6/0xdbf/0xdb5/0xdc0/0xdca
//           -- HOH / Fence / Pole / NarrowLadder / Pipe / ZipLine
//
// Epilogue (clean exit): 0x10ed3319  MOV ECX,[EBP-0xc] / ... / RET 8
// ============================================================


// =============================================================
// PATH A  (LedgeGrab, flag 0xdbc)
// =============================================================

// --- [1] PATH A: null-Actor guard ---
//
// Overwrites 5 bytes:
//   10ed28fd  MOV EBX,[EBP+0xc]   3 bytes  \
//   10ed2900  MOV EAX,[EBX+0xc]   first 2  /  replaced by JMP
//
// Normal return : 0x10ed2903  (CMP byte ptr [EAX+0x2d9],0x8)
// Null exit     : 0x10ed3319  (epilogue)

JMP_HOOK(0x10ed28fd, GEWireframeNullActorFix_PathA)
{
    static int return_to_cmp = 0x10ed2903;
    static int clean_exit    = 0x10ed3319;

    __asm {
        mov  ebx, dword ptr [ebp + 0xc]     // restore: MOV EBX,[EBP+0xc]
        mov  eax, dword ptr [ebx + 0xc]     // restore: MOV EAX,[EBX+0xc]
        test eax, eax
        jz   actor_null_a
        jmp  dword ptr [return_to_cmp]
    actor_null_a:
        jmp  dword ptr [clean_exit]
    }
}

// --- [2] PATH A: double-deref guard + P1 fallback (actual crash fix) ---
//
// Reached at LAB_10ed2d6d after brush type==0x3 and model!=NULL
// checks pass.  The code then does:
//   10ed2d8b  MOV EDX,[EBX+4]     EDX = P1 = *(HActor+4)
//   10ed2d8e  MOV EAX,[EDX+4]     EAX = P2 = *(P1+4)   <- NULL in wireframe
//   10ed2d91  MOV ECX,[EAX+0xc]   GPF when EAX == NULL
//
// P2 holds position data (XYZ at +0xc/+0x10/+0x14) used by the
// following GE creation code.  The StaticMesh path uses P1 in exactly
// the same way (+0xc/+0x10/+0x14), so P1 is a valid position source.
// When P2 is NULL (brush in wireframe), substitute EAX = EDX (P1)
// so the GE is created at the P1 hit position instead of aborting.
//
// Overwrites 5 bytes:
//   10ed2d8b  MOV EDX,[EBX+4]   3 bytes  \
//   10ed2d8e  MOV EAX,[EDX+4]   first 2  /  replaced by JMP
//
// Normal return : 0x10ed2d91  (MOV ECX,[EAX+0xc] -- EAX = P2 or P1 fallback)
// Null exit     : 0x10ed3319  (epilogue -- only if P1 itself is NULL)

JMP_HOOK(0x10ed2d8b, GEWireframeDoubleDerefFix_PathA)
{
    static int return_to_next = 0x10ed2d91;
    static int clean_exit     = 0x10ed3319;

    __asm {
        mov  edx, dword ptr [ebx + 4]       // restore: MOV EDX,[EBX+4]  (P1)
        test edx, edx
        jz   deref_null_a                   // P1 NULL → bail (shouldn't happen)
        mov  eax, dword ptr [edx + 4]       // restore: MOV EAX,[EDX+4]  (P2)
        test eax, eax
        jnz  deref_ok_a                     // P2 valid → continue normally
        mov  eax, edx                       // P2 NULL → fall back: use P1 as position source
    deref_ok_a:
        jmp  dword ptr [return_to_next]
    deref_null_a:
        jmp  dword ptr [clean_exit]
    }
}


// =============================================================
// PATH B  (HOH / Fence / Pole / NarrowLadder / Pipe / ZipLine)
// =============================================================

// --- [1] PATH B: null-Actor guard ---
//
// NOTE: hook must start at 0x10ed3866, NOT 0x10ed3869.
//   A JMP at 0x10ed3869 (3-byte MOV EAX) would overwrite the first 2
//   bytes of the CMP at 0x10ed386c, corrupting the return target.
//   Starting at 0x10ed3866 (3-byte MOV ECX) keeps 0x10ed386c intact.
//
// Overwrites 5 bytes:
//   10ed3866  MOV ECX,[EBP+0xc]   3 bytes  \
//   10ed3869  MOV EAX,[ECX+0xc]   first 2  /  replaced by JMP
//
// Normal return : 0x10ed386c  (CMP byte ptr [EAX+0x2d9],0x8 -- intact)
// Null exit     : 0x10ed3319  (epilogue)

JMP_HOOK(0x10ed3866, GEWireframeNullActorFix_PathB)
{
    static int return_to_cmp = 0x10ed386c;
    static int clean_exit    = 0x10ed3319;

    __asm {
        mov  ecx, dword ptr [ebp + 0xc]     // restore: MOV ECX,[EBP+0xc]
        mov  eax, dword ptr [ecx + 0xc]     // restore: MOV EAX,[ECX+0xc]
        test eax, eax
        jz   actor_null_b
        jmp  dword ptr [return_to_cmp]
    actor_null_b:
        jmp  dword ptr [clean_exit]
    }
}

// --- [2] PATH B: double-deref guard + P1 fallback (actual crash fix) ---
//
// Reached at LAB_10ed3ed5 after brush type==0x3 and model!=NULL
// checks pass.  ECX = HActor* = param_2 (reloaded at 0x10ed3ed5).
//   10ed3ef6  MOV EDX,[ECX+4]     EDX = P1 = *(HActor+4)
//   10ed3ef9  MOV EAX,[EDX+4]     EAX = P2 = *(P1+4)   <- NULL in wireframe
//   10ed3efc  MOV ECX,[EAX+0xc]   GPF when EAX == NULL
//
// Same P1 fallback strategy as Path A: substitute EAX = EDX (P1)
// when P2 is NULL so the GE gets placed at the P1 hit position.
//
// Overwrites 5 bytes:
//   10ed3ef6  MOV EDX,[ECX+4]   3 bytes  \
//   10ed3ef9  MOV EAX,[EDX+4]   first 2  /  replaced by JMP
//
// Normal return : 0x10ed3efc  (MOV ECX,[EAX+0xc] -- EAX = P2 or P1 fallback)
// Null exit     : 0x10ed3319  (epilogue -- only if P1 itself is NULL)

JMP_HOOK(0x10ed3ef6, GEWireframeDoubleDerefFix_PathB)
{
    static int return_to_next = 0x10ed3efc;
    static int clean_exit     = 0x10ed3319;

    __asm {
        mov  edx, dword ptr [ecx + 4]       // restore: MOV EDX,[ECX+4]  (P1)
        test edx, edx
        jz   deref_null_b                   // P1 NULL → bail (shouldn't happen)
        mov  eax, dword ptr [edx + 4]       // restore: MOV EAX,[EDX+4]  (P2)
        test eax, eax
        jnz  deref_ok_b                     // P2 valid → continue normally
        mov  eax, edx                       // P2 NULL → fall back: use P1 as position source
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
