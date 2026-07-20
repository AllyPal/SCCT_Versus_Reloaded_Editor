#include "pch.h"
#include "General.h"
#include "Hooks.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "MemoryWriter.h"
#include "AnimationBrowser.h"
#include <mimalloc.h>
#include <unordered_map>
#include <unordered_set>

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

// Game View (J) - Simulates the in-game view in the viewport
// Copies bHidden to bHiddenEd and only clears the flags it set when disabled
static const uint32_t kGEditor            = 0x1165dfa0;
static const uint32_t kEditor_Level       = 0x130;
static const uint32_t kEditor_RedrawVtbl  = 0xE8;   // RedrawLevel(ULevel*)
static const uint32_t kLevel_ActorsData   = 0x2C;
static const uint32_t kLevel_ActorsNum    = 0x30;
static const uint32_t kActor_Flags        = 0x2E8;  // dword holding bHidden
static const uint32_t kMask_Hidden        = 0x1000; // its bit
static const uint32_t kActor_EdFlags      = 0x2F4;  // dword holding bHiddenEd
static const uint32_t kMask_HiddenEd      = 0x08;   // its bit
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
static std::unordered_set<void*> g_gameViewHidden;          // compared only, never dereferenced
static std::unordered_map<void*, void*> g_gameViewSprites;  // actor -> saved Texture

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

// AActor::RenderEditorInfo assumes Actor->Texture is non-null
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

// DrawSprite assumes Actor->Texture is non-null. Callers must guard it or it will crash on cleared icons
JMP_HOOK(0x110a3270, DrawSpriteNullTextureFix)
{
    static int s_resume = 0x110a3275;

    __asm
    {
        mov  eax, dword ptr [esp + 4]      // the actor
        test eax, eax
        jz   skip_sprite
        cmp  dword ptr [eax + 0x228], 0    // Actor->Texture
        jz   skip_sprite

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_sprite:
        ret                                // caller cleans the arg
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

static bool GV_ClassChainHas(void* actor, const char* want)
{
    int* gnames = *reinterpret_cast<int**>(kGNames);
    if (!gnames) return false;

    void* cls = *reinterpret_cast<void**>(static_cast<char*>(actor) + kObj_Class);
    for (int depth = 0; cls && depth < 16; ++depth)
    {
        int entry = gnames[*reinterpret_cast<int*>(static_cast<char*>(cls) + kObj_Name)];
        if (entry && _stricmp(reinterpret_cast<const char*>(entry + 0x0C), want) == 0)
            return true;
        cls = *reinterpret_cast<void**>(static_cast<char*>(cls) + kClass_Super);
    }
    return false;
}

static bool GV_IsKeptClass(void* actor)
{
    for (const char* keep : kGameViewKeep)
        if (GV_ClassChainHas(actor, keep))
            return true;
    return false;
}

static bool GV_IsIconOnlyClass(void* actor)
{
    for (const char* cls : kGameViewIconOnly)
        if (GV_ClassChainHas(actor, cls))
            return true;
    return false;
}

static void** GV_Actors(void* gEditor, int* outCount)
{
    void* level = *reinterpret_cast<void**>(static_cast<char*>(gEditor) + kEditor_Level);
    if (!level) return nullptr;
    *outCount = *reinterpret_cast<int*>(static_cast<char*>(level) + kLevel_ActorsNum);
    return *reinterpret_cast<void***>(static_cast<char*>(level) + kLevel_ActorsData);
}

static void GV_Apply(void** actors, int count)
{
    for (int i = 0; i < count; ++i)
    {
        void* actor = actors[i];
        if (!actor) continue;
        DWORD* flags = reinterpret_cast<DWORD*>(static_cast<char*>(actor) + kActor_Flags);
        DWORD* ed    = reinterpret_cast<DWORD*>(static_cast<char*>(actor) + kActor_EdFlags);
        void** tex   = reinterpret_cast<void**>(static_cast<char*>(actor) + kActor_Texture);

        // Emitter particles, and a light's corona / light beam, are visible in game
        // and die with the actor, so only hide the editor icon
        const bool iconOnly =
            *reinterpret_cast<uint8_t*>(static_cast<char*>(actor) + kActor_DrawType)
                == kDrawType_Particle ||
            GV_IsIconOnlyClass(actor);

        if ((*flags & kMask_Hidden) && !(*ed & kMask_HiddenEd)
                && !iconOnly && !GV_IsKeptClass(actor))
        {
            *ed |= kMask_HiddenEd;
            g_gameViewHidden.insert(actor);
            continue;
        }

        if (iconOnly && *tex)
        {
            g_gameViewSprites[actor] = *tex;
            *tex = nullptr;
        }
    }
    g_gameView = true;
}

// Refresh from the live actor list because a map reload invalidates the saved pointers.
static void GV_Restore(void** actors, int count)
{
    for (int i = 0; i < count; ++i)
    {
        void* actor = actors[i];
        if (!actor) continue;

        if (g_gameViewHidden.count(actor))
            *reinterpret_cast<DWORD*>(static_cast<char*>(actor) + kActor_EdFlags)
                &= ~kMask_HiddenEd;

        auto its = g_gameViewSprites.find(actor);
        if (its != g_gameViewSprites.end())
        {
            void** tex = reinterpret_cast<void**>(static_cast<char*>(actor) + kActor_Texture);

            // Restore only if Texture is still null (the value set by GV_Apply).
            // If it's non-null, the texture was changed or this actor was reused,
            // so restoring the saved pointer could overwrite valid data.
            if (!*tex)
                *tex = its->second;
        }
    }
    g_gameViewHidden.clear();
    g_gameViewSprites.clear();
    g_gameView = false;
}

// bHiddenEd is saved, so Game View must be off before SavePackage to avoid baking hidden state into the map
static void __cdecl GameViewClearForSave()
{
    if (!g_gameView) return;

    void* gEditor = *reinterpret_cast<void**>(kGEditor);
    if (!gEditor) return;

    int count = 0;
    void** actors = GV_Actors(gEditor, &count);
    if (actors) GV_Restore(actors, count);
}

JMP_HOOK(0x10fb2610, SavePackageGameViewHook)
{
    static int s_resume = 0x10fb2615;

    __asm
    {
        pushad
        call GameViewClearForSave
        popad

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]
    }
}

static void __cdecl ToggleGameView()
{
    void* gEditor = *reinterpret_cast<void**>(kGEditor);
    if (!gEditor) return;

    void* level = *reinterpret_cast<void**>(static_cast<char*>(gEditor) + kEditor_Level);
    if (!level) return;

    int count = 0;
    void** actors = GV_Actors(gEditor, &count);
    if (!actors) return;

    if (g_gameView) GV_Restore(actors, count);
    else            GV_Apply(actors, count);

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
    InstallMinimizeOnPlayHook();
    InstallNoEmbedOnPlayPatch();
}
