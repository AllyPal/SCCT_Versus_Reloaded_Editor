#include "pch.h"
#include "General.h"
#include "Hooks.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"    // g_ReloadedNoDuplicateOffset
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "MemoryWriter.h"
#include <mimalloc.h>

INIT_HOOKS;

JMP_HOOK(0x110518AD, RemoveAudioSizeLimit) {
    static int Return = 0x110518B3;
    __asm {
        jmp dword ptr[Return]
    }
}


void InstallMemoryHooks() {
    uintptr_t fn_ptr;

    fn_ptr = reinterpret_cast<uintptr_t>(mi_malloc);
    MemoryWriter::WriteBytes(0x11AF2114, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_free);
    MemoryWriter::WriteBytes(0x11AF209C, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_realloc);
    MemoryWriter::WriteBytes(0x11AF21F0, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_calloc);
    MemoryWriter::WriteBytes(0x11AF2098, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_strdup);
    MemoryWriter::WriteBytes(0x11AF21C0, &fn_ptr, sizeof(fn_ptr));
}

// Help menu URL items 
//
// FUN_10e57b30 is the editor's WOnCommand dispatcher - a single 31 KB function
// that handles every WM_COMMAND ID through a compiled jump table. New IDs
// cannot be added to that table at run-time.
//
// We hook the very first 5 bytes of the function (PUSH EBP / MOV EBP,ESP /
// PUSH -1 = 55 8B EC 6A FF) with a JMP to our handler. The handler checks the
// command ID before the function's own dispatch runs:
//
//   • ID == 40900  >  ShellExecuteA website URL, RETN 4 (never enters function)
//   • ID == 40901  >  ShellExecuteA wiki URL,    RETN 4
//   • anything else > replay the 5 overwritten bytes, jump to 0x10e57b35
//                      (continue the original function as if unhoooked)
//
// Calling convention of FUN_10e57b30: one DWORD argument on stack (commandID),
// callee-cleans (RETN 4 at each case exit).  At our hook entry:
//   [ESP+0] = caller's return address
//   [ESP+4] = commandID (DWORD)
//
// UnrealEd/Src/Main.cpp:
//     case IDMN_HELP_UDN:   // IDMN_HELP_UDN = 30500
//     ShellExecuteA(NULL, "open", "http://udn.epicgames.com", NULL, NULL, SW_SHOWNORMAL);
//
// Menu IDs in the .rc file:
//   MENUITEM "&Reloaded Editor Website",  40900
//   MENUITEM "&Reloaded Editor Wiki",     40901

static void __cdecl OpenURL(const char* url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

// __cdecl wrapper so the naked JMP_HOOK can call it with a plain CALL instruction.
// GetActiveWindow() returns the main editor window that owns the menu bar —
// the correct parent to pass to DialogBoxIndirectParamA.
static void __cdecl OpenReloadedOptions()
{
    ShowReloadedOptionsDialog(GetActiveWindow());
}

static const char s_website_url[] = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor";
static const char s_wiki_url[]    = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki";

JMP_HOOK(0x10e57b30, HelpMenuDispatch)
{
    // s_continue holds the address immediately after the 5 bytes we overwrote,
    // so the fallthrough path rejoins the original function seamlessly.
    static int s_continue = 0x10e57b35;

    __asm {
        // [esp+4] = commandID (no stack frame yet)
        cmp  dword ptr [esp+4], 40066
        je   do_reloaded_options
        cmp  dword ptr [esp+4], 40900
        je   do_website
        cmp  dword ptr [esp+4], 40901
        je   do_wiki

        //  fallthrough: replay overwritten prologue then continue
        // Original bytes: 55 8B EC 6A FF
        //   PUSH EBP          (55)
        //   MOV  EBP, ESP     (8B EC)
        //   PUSH -1           (6A FF)
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]   // > 0x10e57b35

        //  Reloaded Options dialog (View > Reloaded Options, ID 40066)
    do_reloaded_options:
        call OpenReloadedOptions      // __cdecl, no args – calls ShowReloadedOptionsDialog(GetActiveWindow())
        retn 4                        // clean commandID, return to caller

        //  URL handlers
    do_website:
        push offset s_website_url
        call OpenURL                  // __cdecl - we clean the arg
        add  esp, 4
        retn 4                        // clean commandID, return to caller

    do_wiki:
        push offset s_wiki_url
        call OpenURL
        add  esp, 4
        retn 4
    }
}

// ── F12 → Reloaded Options keyboard shortcut ──────────────────────────────────
//
// WViewportFrame::OnKeyUp (FUN_10f00d10) handles F4/F5/F6/F7/F8 via a switch on
// wParam.  The switch covers wParam 0x2E–0x77 (offset range 0x00–0x49 after
// subtracting 0x2E).  VK_F12 (0x7B) = offset 0x4D, which exceeds 0x49, so it
// falls straight through and the function returns — F12 is simply ignored today.
//
// We hook the very first 5 bytes of the function (the standard MSVC SEH prologue:
// PUSH EBP / MOV EBP,ESP / PUSH -1 = 55 8B EC 6A FF) with a JMP to our stub.
//
// At hook entry no frame has been built yet, so:
//   [ESP+0] = return address
//   [ESP+4] = wParam   (first __thiscall stack arg)
//   [ESP+8] = lParam
//
// If wParam == VK_F12 we call OpenReloadedOptions and RET 8 (same callee-cleanup
// as the original function's own RET 8 exit).  Any other key replays the 5
// overwritten prologue bytes and jumps to 0x10f00d15 to continue normally.
//
// This gives F12 identical gating to F4/F6: it only fires when a viewport frame
// window is focused, with no effect in the Texture Browser, Sound Browser, etc.
//
JMP_HOOK(0x10f00d10, ViewportKeyUpHook)
{
    static int s_resume = 0x10f00d15;   // PUSH 0x1141dfb0 (SEH handler), first byte after our 5

    __asm
    {
        // [esp+4] = wParam (no frame yet)

        // F12: open Reloaded Options dialog.
        cmp  dword ptr [esp + 4], 0x7B      // VK_F12
        je   do_f12

        // F7 (SCRIPT MAKE): swallow silently — crashes this editor build.
        cmp  dword ptr [esp + 4], 0x76      // VK_F7
        je   swallow

        // Replay the 5 overwritten prologue bytes, then continue the original function.
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]           // → 0x10f00d15

    do_f12:
        call OpenReloadedOptions            // __cdecl, no args — calls ShowReloadedOptionsDialog(GetActiveWindow())
    swallow:
        ret  8                              // callee-cleans wParam + lParam; nothing further
    }
}

// ── Duplicate position offset suppression ────────────────────────────────────
//
// edactPasteSelected (FUN_10eb8320) applies position offsets in two distinct
// passes, both gated on the Duplicate flag ([EBP+0xC]).
//
// SITE 1 — 0x10eb8573  (per-actor loop, first pass)
//   CMP [EBP+0xC], 0  /  JZ 0x10eb85d2   (5-byte overwrite)
//   Duplicate==true  → 0x10eb8579: GridSize offset applied to each actor
//   Duplicate==false → 0x10eb85d2: FVector(32,32,32) applied to each actor
//   Both converge at 0x10eb861c.
//
// SITE 2 — 0x10eb8722  (post-loop second pass)
//   CMP [EBP+0xC], 0  /  JZ 0x10eb8755   (5-byte overwrite)
//   Duplicate==true  → 0x10eb8728: FVector(32,32,0) offset pass
//   Duplicate==false → 0x10eb8755: complex paste-offset setup
//   Both converge at 0x10eb897b.
//
// When g_ReloadedNoDuplicateOffset is set we skip BOTH Duplicate==true offset
// paths, jumping directly to each convergence point.  The result is a 0,0,0
// net offset so the duplicated actors land exactly on top of their originals.
//
// At hook entry edactPasteSelected's own frame is fully valid:
//   [EBP+0xC] = Duplicate param (non-zero → duplicate, zero → paste)
//
// The JZ displacement byte left intact at +5 is harmless: we never fall into
// it from our hook — we always dispatch via an explicit indirect JMP.
//

// Site 1: per-actor loop
JMP_HOOK(0x10eb8573, DupOffsetHook)
{
    static int s_skip   = 0x10eb861c;  // convergence — skip GridSize loop
    static int s_is_dup = 0x10eb8579;  // Duplicate==true  normal path (GridSize)
    static int s_no_dup = 0x10eb85d2;  // Duplicate==false normal path (32,32,32)

    __asm
    {
        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jz   normal

        cmp  dword ptr [ebp + 0xc], 0
        jnz  skip_offset

    normal:
        cmp  dword ptr [ebp + 0xc], 0
        jz   is_no_dup
        jmp  dword ptr [s_is_dup]

    is_no_dup:
        jmp  dword ptr [s_no_dup]

    skip_offset:
        jmp  dword ptr [s_skip]
    }
}

// Site 2: post-loop second-pass offset
JMP_HOOK(0x10eb8722, DupOffsetHook2)
{
    static int s_skip   = 0x10eb897b;  // convergence — skip FVector(32,32,0) pass
    static int s_is_dup = 0x10eb8728;  // Duplicate==true  normal path (FVector(32,32,0))
    static int s_no_dup = 0x10eb8755;  // Duplicate==false normal path (paste setup)

    __asm
    {
        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jz   normal2

        cmp  dword ptr [ebp + 0xc], 0
        jnz  skip_offset2

    normal2:
        cmp  dword ptr [ebp + 0xc], 0
        jz   is_no_dup2
        jmp  dword ptr [s_is_dup]

    is_no_dup2:
        jmp  dword ptr [s_no_dup]

    skip_offset2:
        jmp  dword ptr [s_skip]
    }
}

void General::Initialize()
{
    INSTALL_HOOKS;
    InstallMemoryHooks();
}