#include "pch.h"
#include "General.h"
#include "Hooks.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "MemoryWriter.h"
#include "AnimationBrowser.h"
#include "NormalMaps.h"
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

static const char s_website_url[] = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor";
static const char s_wiki_url[]    = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki";

static void __cdecl OpenURL(const char* url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void __cdecl OpenReloadedOptions()
{
    ShowReloadedOptionsDialog(GetActiveWindow());
}

static void __cdecl OpenAnimationBrowser()
{
    AnimationBrowser::Show(GetActiveWindow());
}

JMP_HOOK(0x10e57b30, MenuBarDispatch)
{
    static int s_continue = 0x10e57b35;

    __asm {
        cmp  dword ptr [esp+4], 40066 // Reloaded Options
        je   do_reloaded_options
        cmp  dword ptr [esp+4], 40067 // Show Animation Browser
        je   do_anim_browser
        cmp  dword ptr [esp+4], 40900 // Reloaed Website
        je   do_website
        cmp  dword ptr [esp+4], 40901 // Reloaded Wiki
        je   do_wiki

        // Fallthrough: replay overwritten prologue then continue
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    do_reloaded_options:
        call OpenReloadedOptions
        retn 4

    do_anim_browser:
        call OpenAnimationBrowser
        retn 4

    do_website:
        push offset s_website_url
        call OpenURL
        add  esp, 4
        retn 4

    do_wiki:
        push offset s_wiki_url
        call OpenURL
        add  esp, 4
        retn 4
    }
}

JMP_HOOK(0x10f00d10, ViewportKeyUpHook)
{
    static int s_resume = 0x10f00d15;

    __asm
    {
        // F12: Reloaded Options
        cmp  dword ptr [esp + 4], 0x7B
        je   do_f12

        // F11: Normal Maps - scan the open level for Detail-slot carriers
        cmp  dword ptr [esp + 4], 0x7A
        je   do_scan

        // F9: Normal Maps - toggle the render pass on/off
        cmp  dword ptr [esp + 4], 0x78
        je   do_toggle

        // F7: Attempts to compile UnrealScript but fails (scripts are stripped). Disabled to prevent accidentally pressing F7 and crashing.
        cmp  dword ptr [esp + 4], 0x76
        je   swallow

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    do_f12:
        call OpenReloadedOptions
        ret  8
    do_scan:
        call NormalMaps_HotkeyScan
        ret  8
    do_toggle:
        call NormalMaps_HotkeyToggleRender
        ret  8
    swallow:
        ret  8
    }
}

// Duplication offset
JMP_HOOK(0x10eb8573, DupOffsetHook)
{
    static int s_skip   = 0x10eb861c;
    static int s_is_dup = 0x10eb8579;
    static int s_no_dup = 0x10eb85d2;

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

JMP_HOOK(0x10eb8722, DupOffsetHook2)
{
    static int s_skip   = 0x10eb897b;
    static int s_is_dup = 0x10eb8728;
    static int s_no_dup = 0x10eb8755;

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
