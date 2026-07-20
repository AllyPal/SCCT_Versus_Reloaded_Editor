#include "pch.h"
#include "WindowDriftFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"

INIT_HOOKS;

// Windows 10/11 fix for the maximized "window drift" bug.
//
// WWindow::OnActivate() is just a thunk to VerifyPosition(), which runs on
// every WM_ACTIVATE (when the window is activated or deactivated).
//
// VerifyPosition() is a Windows XP off-screen rescue that snaps a window
// to (0,0) whenever GetWindowRect() reports left/top < -4. That threshold
// matched Windows XP's maximized border overhang, but on Windows 10/11 the
// overhang is larger (-8px at 96 DPI, more with DPI scaling), so a
// legitimately maximized window at (-8,-8) is mistaken for an off-screen
// window and shifted down/right, often leaving the bottom edge beneath the
// taskbar.

static void __fastcall VerifyPositionFixed(void* wwindow)
{
    const HWND hwnd = *reinterpret_cast<HWND*>(static_cast<char*>(wwindow) + 4);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) return;

    RECT wr;
    GetWindowRect(hwnd, &wr);
    const LONG vl = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG vr = vl + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG vb = vt + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    LONG newX = wr.left, newY = wr.top;
    if (wr.left >= vr || wr.right <= vl) newX = 0;
    if (wr.top >= vb || wr.bottom <= vt) newY = 0;
    if (newX != wr.left || newY != wr.top)
        SetWindowPos(hwnd, NULL, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOSENDCHANGING);
}

// WWindow::OnActivate(int Active) is __thiscall with ecx = this.
// Its 8-byte body is fully replaced by the 5-byte jump hook.
JMP_HOOK(0x10f81f50, WWindowOnActivateHook)
{
    __asm
    {
        // ecx (this) passes straight through as the __fastcall argument.
        call    VerifyPositionFixed
        ret     4
    }
}

void WindowDriftFix::Initialize()
{
    INSTALL_HOOKS;
}
