#include "pch.h"
#include "UI.h"

#include "Hooks.h"

INIT_HOOKS;

JMP_HOOK(0x10E4319C, SetTitle) {
    static int Return = 0x10E431A1;
    __asm {
        push offset editor_header
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3C138, SetTitle2) {
    static int Return = 0x10E3C13D;
    __asm {
        push offset editor_header
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E2E49F, SetTitleEditorName) {
    static int Return = 0x10E2E4A4;
    __asm {
        push offset editor_header_prefix
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E4B285, SetTitleEditorName2) {
    static int Return = 0x10E4B28A;
    __asm {
        push offset editor_header_prefix
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3EBE0, SkipPointlessSecondPopup) {
    static int Return = 0x10E3EC26;
    __asm {
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3E071, VerboseSavePopup) {
    static int Return = 0x10E3E076;
    __asm {
        push offset verbose_save_message
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3E0B8, SkipPointlessSecondPopupOnExit) {
    static int Return = 0x10E3ED8F;
    __asm {
        jmp dword ptr[Return]
    }
}

// Add Reloaded menu items to the main menu
// Command IDs are handled by MenuBarDispatch (General.cpp)
static int MenuPosByCommand(HMENU menu, UINT cmd)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
        if (GetMenuItemID(menu, i) == cmd)
            return i;
    return -1;
}

// Top-level submenu of the bar that directly contains 'cmd'.
static HMENU SubMenuWithCommand(HMENU bar, UINT cmd)
{
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i)
    {
        HMENU sub = GetSubMenu(bar, i);
        if (sub && MenuPosByCommand(sub, cmd) >= 0)
            return sub;
    }
    return nullptr;
}

static void InjectReloadedMenuItems(HWND frame)
{
    HMENU bar = GetMenu(frame);
    if (!bar) return;

    HMENU view = SubMenuWithCommand(bar, 40065); // "Advanced Options" lives in View
    if (view)
    {
        int pos = MenuPosByCommand(view, 19004); // after "Show Actor Class Browser"
        if (pos >= 0 && MenuPosByCommand(view, 40067) < 0)
            InsertMenuA(view, pos + 1, MF_BYPOSITION | MF_STRING, 40067, "Show &Animation Browser");

        pos = MenuPosByCommand(view, 40065);     // after "Advanced Options"
        if (pos >= 0 && MenuPosByCommand(view, 40066) < 0)
            InsertMenuA(view, pos + 1, MF_BYPOSITION | MF_STRING, 40066, "Reloaded Options\tF12");
    }

    HMENU help = SubMenuWithCommand(bar, 40480); // "Unreal Developer Network" lives in Help
    if (help && MenuPosByCommand(help, 40900) < 0)
    {
        AppendMenuA(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(help, MF_STRING, 40900, "&Reloaded Editor GitHub");
        AppendMenuA(help, MF_STRING, 40901, "&Reloaded Editor Wiki");
    }

    DrawMenuBar(frame);
}

static HWND g_reloadedFrame;
static BOOL CALLBACK FindFrameProc(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    HMENU bar = GetMenu(hwnd);
    if (bar && SubMenuWithCommand(bar, 40065)) // the frame that owns the View menu
    {
        g_reloadedFrame = hwnd;
        return FALSE;
    }
    return TRUE;
}

// The editor frame doesn't exist yet at DLL attach; wait for it, then inject once.
static DWORD WINAPI MenuInjectThread(LPVOID)
{
    for (int i = 0; i < 600; ++i) // up to ~60s
    {
        g_reloadedFrame = nullptr;
        EnumWindows(FindFrameProc, 0);
        if (g_reloadedFrame)
        {
            InjectReloadedMenuItems(g_reloadedFrame);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

void UI::Initialize()
{
    INSTALL_HOOKS;

    MemoryWriter::WriteBytes(0x11463408, &editor_version, sizeof(editor_version));

    HANDLE h = CreateThread(nullptr, 0, MenuInjectThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
