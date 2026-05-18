#include "pch.h"
#include "DialogFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"

INIT_HOOKS;

// Closing modal dialogs (class "WObjectProperties") leaves the main editor 
// window unfocused. This hook restores focus to the owner after WM_CLOSE.

// Saved owner HWND captured before the window is destroyed by OnClose().
static HWND s_closeOwner = NULL;

static void __fastcall SaveOwnerBeforeClose(void* wwindow)
{
    const HWND hwnd = *reinterpret_cast<HWND*>(static_cast<char*>(wwindow) + 4);
    s_closeOwner = NULL;
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (!owner) owner = GetParent(hwnd);
    s_closeOwner = owner;
}

static void __fastcall RestoreFocusToOwner(HWND owner)
{
    if (!owner || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);
}

JMP_HOOK(0x10f83f09, WWindowWMCloseHook)
{
    static int s_not_wm_close = 0x10f83f35;
    static int s_defwndproc   = 0x10f8464a;
    static int s_epilogue     = 0x10f83f25;

    __asm
    {
        cmp     eax, 0x10
        jne     not_wm_close

        // Save owner HWND before window is destroyed
        push    eax
        mov     ecx, esi
        call    SaveOwnerBeforeClose
        pop     eax

        // Call WWindow::OnClose (destroys window)
        mov     edx, dword ptr [esi]
        mov     ecx, esi
        call    dword ptr [edx + 0xdc]

        // Restore focus to saved owner
        push    eax
        mov     ecx, dword ptr [s_closeOwner]
        test    ecx, ecx
        jz      wm_close_branch
        call    RestoreFocusToOwner

    wm_close_branch:
        pop     eax

        test    eax, eax
        jnz     wm_close_defwndproc

        // OnClose returned 0: set return value to 0 and jump to epilogue
        xor     eax, eax
        mov     ecx, dword ptr [ebp - 0xc] // restore SEH chain link
        jmp     dword ptr [s_epilogue]

    wm_close_defwndproc:
        jmp     dword ptr [s_defwndproc]

    not_wm_close:
        jmp     dword ptr [s_not_wm_close]
    }
}

void DialogFix::Initialize()
{
    INSTALL_HOOKS;
}
