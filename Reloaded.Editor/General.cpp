#include "pch.h"
#include "General.h"
#include "Hooks.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "MemoryWriter.h"
#include "AnimationBrowser.h"
#include "RebuildAllMaps.h"
#include <mimalloc.h>
#include <unordered_map>

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

// Play Map minimizes the editor by calling CloseWindow. Route that call
// through this wrapper so minimizing only happens when enabled.
static BOOL WINAPI ReloadedCloseWindow(HWND hWnd)
{
    if (g_ReloadedMinimizeOnPlay)
        return CloseWindow(hWnd);
    return TRUE;
}

static void InstallMinimizeOnPlayHook()
{
    uintptr_t fn_ptr = reinterpret_cast<uintptr_t>(ReloadedCloseWindow);
    MemoryWriter::WriteBytes(0x11AF23CC, &fn_ptr, sizeof(fn_ptr));
}

// Force Play Map's launch HWND argument to 0 so the game opens in its own
// window instead of reparenting into the editor.
static void InstallNoEmbedOnPlayPatch()
{
    const uint8_t patch[] = { 0xB9, 0x00, 0x00, 0x00, 0x00, 0x90 };
    MemoryWriter::WriteBytes(0x10E2131A, patch, sizeof(patch));
}

static const char s_github_url[] = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor";
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

static void __cdecl OpenRebuildAllMaps()
{
    RebuildAllMaps::Show(GetActiveWindow());
}

// Game View (J) - Simulates the in-game view in the viewport
static const uint32_t kGEditor            = 0x1165dfa0;
static const uint32_t kEditor_Level       = 0x130;
static const uint32_t kEditor_RedrawVtbl  = 0xE8;   // RedrawLevel(ULevel*)
static const uint32_t kActor_Flags        = 0x2E8;  // dword holding bHidden
static const uint32_t kMask_Hidden        = 0x1000; // its bit
static const uint32_t kActor_Texture      = 0x228;  // editor icon sprite
static const uint32_t kActor_DrawType     = 0x2D9;
static const uint8_t  kDrawType_Particle  = 10;
static const uint32_t kObj_Class          = 0x24;
static const uint32_t kObj_Name           = 0x20;
static const uint32_t kClass_Super        = 0x28;
static const uint32_t kGNames             = 0x1169cfbc;

// Manual exclusion for SComputerObjectiveTrigger because it has bHidden=true but is visible in game
static const char* const kGameViewKeep[] = { "SComputerObjectiveTrigger" };

// Visible in game (corona, light beam, rain) but the editor also billboards their icon
static const char* const kGameViewIconOnly[] = { "Light", "ERainVolume" };

static bool g_gameView = false;

enum : uint8_t { GV_None = 0, GV_IconOnly = 1, GV_Keep = 2 };
static std::unordered_map<void*, uint8_t> g_gameViewClassCache;

// Hides the rain volume wireframe but keeps the rain visible
JMP_HOOK(0x1114aa20, RainVolumeBoundsHook)
{
    static int s_resume = 0x1114aa25;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_bounds

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_bounds:
        ret  8
    }
}

// AActor::RenderEditorInfo draws overlays used by the editor (icons, radii, etc.)
JMP_HOOK(0x11191110, ActorEditorInfoHook)
{
    static int s_resume = 0x11191115;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_info

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_info:
        ret  0xc
    }
}

// Skip ULevel::RenderGEs in the editor viewport to hide GE lines without affecting hit-testing
JMP_HOOK(0x10eced35, GEViewportRenderHook)
{
    static int s_renderGEs = 0x11119b20;
    static int s_resume    = 0x10eced3a;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_ge

        call dword ptr [s_renderGEs]   // callee cleans its arg
        jmp  dword ptr [s_resume]

    skip_ge:
        add  esp, 4                    // drop the pushed render context
        jmp  dword ptr [s_resume]
    }
}

static bool GV_ClassChainHas(void* cls, const char* want)
{
    int* gnames = *reinterpret_cast<int**>(kGNames);
    if (!gnames) return false;

    for (int depth = 0; cls && depth < 16; ++depth)
    {
        int entry = gnames[*reinterpret_cast<int*>(static_cast<char*>(cls) + kObj_Name)];
        if (entry && _stricmp(reinterpret_cast<const char*>(entry + 0x0C), want) == 0)
            return true;
        cls = *reinterpret_cast<void**>(static_cast<char*>(cls) + kClass_Super);
    }
    return false;
}

static uint8_t GV_ClassifyClass(void* cls)
{
    auto it = g_gameViewClassCache.find(cls);
    if (it != g_gameViewClassCache.end())
        return it->second;

    uint8_t kind = GV_None;
    for (const char* keep : kGameViewKeep)
        if (GV_ClassChainHas(cls, keep)) { kind = GV_Keep; break; }

    if (kind == GV_None)
        for (const char* icon : kGameViewIconOnly)
            if (GV_ClassChainHas(cls, icon)) { kind = GV_IconOnly; break; }

    g_gameViewClassCache[cls] = kind;
    return kind;
}

// Emitters, coronas, and light beams are visible in game, so only their editor icon is suppressed
static bool GV_IsIconOnly(void* actor)
{
    if (*reinterpret_cast<uint8_t*>(static_cast<char*>(actor) + kActor_DrawType) == kDrawType_Particle)
        return true;
    void* cls = *reinterpret_cast<void**>(static_cast<char*>(actor) + kObj_Class);
    return cls && GV_ClassifyClass(cls) == GV_IconOnly;
}

static int __cdecl GV_ShouldHide(void* actor)
{
    if (!g_gameView || !actor) return 0;

    DWORD flags = *reinterpret_cast<DWORD*>(static_cast<char*>(actor) + kActor_Flags);
    if (!(flags & kMask_Hidden)) return 0;

    if (GV_IsIconOnly(actor)) return 0;

    void* cls = *reinterpret_cast<void**>(static_cast<char*>(actor) + kObj_Class);
    if (cls && GV_ClassifyClass(cls) == GV_Keep) return 0;

    return 1;
}

static int __cdecl GV_SkipSprite(void* actor)
{
    if (!actor) return 1;
    if (!*reinterpret_cast<void**>(static_cast<char*>(actor) + kActor_Texture)) return 1; // engine assumes non-null
    return (g_gameView && GV_IsIconOnly(actor)) ? 1 : 0;
}

// FLevelSceneNode::FilterActor(AActor*) handles actor visibility for rendering and hit-testing
JMP_HOOK(0x110a15e0, SceneNodeFilterActorHook)
{
    static int s_resume = 0x110a15e6;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jz   pass_through

        push ecx                           // this
        push dword ptr [esp + 8]           // actor
        call GV_ShouldHide
        add  esp, 4
        pop  ecx
        test eax, eax
        jnz  hide_actor

    pass_through:
        mov  eax, dword ptr [ecx + 4]      // replay overwritten prologue
        mov  ecx, dword ptr [eax + 0x30]
        jmp  dword ptr [s_resume]

    hide_actor:
        xor  eax, eax
        ret  4
    }
}

// DrawSprite assumes Actor->Texture is non-null, and in Game View icon-only actors skip their icon
JMP_HOOK(0x110a3270, DrawSpriteNullTextureFix)
{
    static int s_resume = 0x110a3275;

    __asm
    {
        push ecx
        push edx
        push dword ptr [esp + 12]          // the actor
        call GV_SkipSprite
        add  esp, 4
        pop  edx
        pop  ecx
        test eax, eax
        jnz  skip_sprite

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_sprite:
        ret                                // caller cleans the arg
    }
}

static void __cdecl ToggleGameView()
{
    void* gEditor = *reinterpret_cast<void**>(kGEditor);
    if (!gEditor) return;

    void* level = *reinterpret_cast<void**>(static_cast<char*>(gEditor) + kEditor_Level);
    if (!level) return;

    g_gameView = !g_gameView;
    g_gameViewClassCache.clear();   // class pointers may have been recycled by a map load

    void* vtable = *reinterpret_cast<void**>(gEditor);
    void* redraw = *reinterpret_cast<void**>(static_cast<char*>(vtable) + kEditor_RedrawVtbl);
    __asm {
        mov  ecx, gEditor
        push level
        mov  eax, redraw
        call eax
    }
}

JMP_HOOK(0x10e57b30, MenuBarDispatch)
{
    static int s_continue = 0x10e57b35;

    __asm {
        cmp  dword ptr [esp+4], 40066 // Reloaded Options
        je   do_reloaded_options
        cmp  dword ptr [esp+4], 40067 // Show Animation Browser
        je   do_anim_browser
        cmp  dword ptr [esp+4], 40902 // Rebuild All Maps
        je   do_rebuild_all
        cmp  dword ptr [esp+4], 40900 // Reloaded Github
        je   do_github
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

    do_rebuild_all:
        call OpenRebuildAllMaps
        retn 4

    do_github:
        push offset s_github_url
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

        // J: Game View
        cmp  dword ptr [esp + 4], 0x4A
        je   do_game_view

        // F7: Attempts to compile UnrealScript but fails (scripts are stripped). Disabled to prevent accidentally pressing F7 and crashing.
        cmp  dword ptr [esp + 4], 0x76
        je   swallow

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    do_game_view:
        call ToggleGameView
        ret  8

    do_f12:
        call OpenReloadedOptions
    swallow:
        ret  8
    }
}

// Duplication offset, and exact placement for paste
JMP_HOOK(0x10eb8573, DupOffsetHook)
{
    static int s_skip   = 0x10eb861c;
    static int s_is_dup = 0x10eb8579;

    __asm
    {
        cmp  dword ptr [ebp + 0xc], 0
        jz   skip_offset                // Real paste: honor the text exactly

        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jnz  skip_offset

        jmp  dword ptr [s_is_dup]

    skip_offset:
        jmp  dword ptr [s_skip]
    }
}

JMP_HOOK(0x10eb8722, DupOffsetHook2)
{
    static int s_skip   = 0x10eb897b;
    static int s_is_dup = 0x10eb8728;

    __asm
    {
        cmp  dword ptr [ebp + 0xc], 0
        jz   skip_offset2               // Real paste: honor the text exactly

        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jnz  skip_offset2

        jmp  dword ptr [s_is_dup]

    skip_offset2:
        // The skipped branches are the only code that writes [ebp-0x1c].
        // 0x10eb897b passes it to the selection-move code, or it runs on stack garbage.
        xor  eax, eax
        mov  dword ptr [ebp - 0x1c], eax
        mov  dword ptr [ebp - 0x18], eax
        mov  dword ptr [ebp - 0x14], eax
        jmp  dword ptr [s_skip]
    }
}

void General::Initialize()
{
    INSTALL_HOOKS;
    InstallMemoryHooks();
    InstallMinimizeOnPlayHook();
    InstallNoEmbedOnPlayPatch();
}
