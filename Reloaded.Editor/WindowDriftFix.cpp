#include "pch.h"
#include "WindowDriftFix.h"
#include "Hooks.h"

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
//
// OnDestroy (0x10F863A0) saves owned top-level windows in owner-client
// coordinates, but PerformCreateWindowEx (0x10F864F0) restores them as
// screen coordinates, causing saved positions to drift toward the top-left
// every session. Surface Properties (0x10EACAA0) exposed the bug once the
// snap-to-zero above was fixed.
//
// PerformCreateWindowEx's VerifyPosition call at 0x10F8662E runs before
// hWnd exists, so it has never worked. Reused here as the intended
// restore-time clamp.
static const int  kOffsetHWnd  = 4;     // WWindow::hWnd
static const LONG kMinVisibleX = 120;
static const LONG kCaptionBand = 32;
static const LONG kMinCaptionY = 16;    // tolerates the Win10/11 top overhang
static const LONG kMaxSaneSize = 32768; // rejects CW_USEDEFAULT and junk sizes

static bool IsSaneSize(LONG v)
{
    return v > 0 && v < kMaxSaneSize;
}

static RECT GetPrimaryWorkArea()
{
    const POINT origin = { 0, 0 };
    const HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);

    if (primary && GetMonitorInfo(primary, &mi))
        return mi.rcWork;

    RECT wa;
    wa.left = 0;
    wa.top = 0;
    wa.right = GetSystemMetrics(SM_CXSCREEN);
    wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    return wa;
}

// Prefer the owner's monitor so a rescued dialog lands on the same display as
// the editor frame instead of jumping to the primary.
static RECT GetFallbackWorkArea(HWND owner)
{
    if (owner && IsWindow(owner))
    {
        const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONULL);

        MONITORINFO mi;
        mi.cbSize = sizeof(mi);

        if (monitor && GetMonitorInfo(monitor, &mi))
            return mi.rcWork;
    }

    return GetPrimaryWorkArea();
}

// Only the caption strip counts: a window off the top edge has ample overlap
// but no grabbable title bar. DEFAULTTONULL is load bearing, NEAREST never fails.
static bool IsUsablyVisible(const RECT& rc)
{
    RECT caption;
    caption.left = rc.left;
    caption.top = rc.top;
    caption.right = rc.right;
    caption.bottom = rc.top + kCaptionBand;

    const HMONITOR monitor = MonitorFromRect(&caption, MONITOR_DEFAULTTONULL);
    if (!monitor) return false;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(monitor, &mi)) return false;

    RECT visible;
    if (!IntersectRect(&visible, &caption, &mi.rcWork)) return false;

    return (visible.right - visible.left) >= kMinVisibleX
        && (visible.bottom - visible.top) >= kMinCaptionY;
}

// Origin only, so an oversized window may still overhang right/bottom as in stock.
static void ClampOriginToWorkArea(const RECT& wa, LONG& x, LONG& y)
{
    const LONG maxX = wa.right - kMinVisibleX;
    const LONG maxY = wa.bottom - kCaptionBand;

    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;
    if (x > maxX) x = (maxX > wa.left) ? maxX : wa.left;
    if (y > maxY) y = (maxY > wa.top) ? maxY : wa.top;
}

// Far edges first, or a window that fits is left jammed against the edge.
static void ClampRectToWorkArea(const RECT& wa, LONG& x, LONG& y, LONG w, LONG h)
{
    if (IsSaneSize(w) && IsSaneSize(h))
    {
        if (w <= wa.right - wa.left && x + w > wa.right)  x = wa.right - w;
        if (h <= wa.bottom - wa.top && y + h > wa.bottom) y = wa.bottom - h;
    }

    ClampOriginToWorkArea(wa, x, y);
}

static void __fastcall VerifyPositionFixed(void* wwindow)
{
    const HWND hwnd = *reinterpret_cast<HWND*>(static_cast<char*>(wwindow) + kOffsetHWnd);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) return;
    if (GetWindowLongA(hwnd, GWL_STYLE) & WS_CHILD) return;

    RECT wr;
    GetWindowRect(hwnd, &wr);

    // Rescue only. Stock snapped any window on a second monitor back to primary.
    if (IsUsablyVisible(wr)) return;

    const RECT wa = GetFallbackWorkArea(GetWindow(hwnd, GW_OWNER));

    LONG x = wr.left;
    LONG y = wr.top;
    ClampRectToWorkArea(wa, x, y, wr.right - wr.left, wr.bottom - wr.top);

    if (x != wr.left || y != wr.top)
        SetWindowPos(hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

// WWindow::OnActivate(int) is __thiscall; its 8-byte body is fully replaced.
JMP_HOOK(0x10f81f50, WWindowOnActivateHook)
{
    __asm
    {
        call    VerifyPositionFixed         // ecx (this) passes through
        ret     4
    }
}

// Owner-client space is only correct for a genuine child window.
static BOOL __fastcall ShouldConvertRectToOwnerClient(void* wwindow)
{
    const HWND hwnd = *reinterpret_cast<HWND*>(static_cast<char*>(wwindow) + kOffsetHWnd);
    if (!hwnd || !IsWindow(hwnd)) return TRUE;

    return (GetWindowLongA(hwnd, GWL_STYLE) & WS_CHILD) ? TRUE : FALSE;
}

JMP_HOOK(0x10f8640f, WWindowOnDestroySaveRectHook)
{
    static int s_resume = 0x10f86414;

    __asm
    {
        push    ecx
        call    ShouldConvertRectToOwnerClient
        pop     ecx

        push    eax                                 // bConvert
        lea     edx, [ebp - 0x30]                   // &FRect out
        jmp     dword ptr [s_resume]
    }
}

// w and h only carry the restored size when dwStyle has WS_THICKFRAME; otherwise
// they are the caller's values and may be CW_USEDEFAULT, which IsSaneSize rejects.
static void __fastcall ClampRestoredPosition(DWORD dwStyle, int* x, int* y,
                                             int w, int h, HWND owner)
{
    if (!x || !y) return;
    if (dwStyle & WS_CHILD) return;         // child/MDI coordinates are parent-relative

    RECT rc;
    rc.left = *x;
    rc.top = *y;
    rc.right = IsSaneSize(w) ? *x + w : *x + kMinVisibleX;
    rc.bottom = IsSaneSize(h) ? *y + h : *y + kCaptionBand;

    if (IsUsablyVisible(rc)) return;

    const RECT wa = GetFallbackWorkArea(owner);

    LONG cx = *x;
    LONG cy = *y;
    ClampRectToWorkArea(wa, cx, cy, w, h);

    *x = cx;
    *y = cy;
}

JMP_HOOK(0x10f8662e, WWindowRestorePositionHook)
{
    static int s_resume = 0x10f86633;

    __asm
    {
        push    eax
        push    ecx
        push    edx

        mov     eax, dword ptr [ebp + 0x24]
        push    eax                                 // hWndParent (owner)
        mov     eax, dword ptr [ebp + 0x20]
        push    eax                                 // nHeight
        mov     eax, dword ptr [ebp + 0x1c]
        push    eax                                 // nWidth
        lea     eax, [ebp + 0x18]
        push    eax                                 // &y
        lea     edx, [ebp + 0x14]                   // &x
        mov     ecx, dword ptr [ebp + 0x10]         // dwStyle
        call    ClampRestoredPosition               // __fastcall pops 16 bytes

        pop     edx
        pop     ecx
        pop     eax
        jmp     dword ptr [s_resume]
    }
}

void WindowDriftFix::Initialize()
{
    INSTALL_HOOKS;
}
