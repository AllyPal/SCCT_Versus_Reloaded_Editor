#include "pch.h"
#include "DialogFix.h"
#include "Hooks.h"
#include "MemoryWriter.h"

INIT_HOOKS;

// ============================================================================
//  Dialog X-button fix  (WWindow::WndProc WM_CLOSE intercept)
// ============================================================================
//
//  "Brush Scaling" and "Convert To Static Mesh Options" are WProperties-family
//  windows (class "WObjectProperties") created with CreateWindowExA and the
//  WS_EX_TOOLWINDOW style.  Unlike standard dialogs (#32770 class), tool
//  windows do NOT get automatic focus restoration when destroyed.  The editor
//  main window is left un-focused and appears unresponsive.
//
//  WWindow::WndProc (FUN_10f83d70) is the central message dispatcher for all
//  WWindow-derived editor windows.  WM_CLOSE is dispatched at 0x10f83f09:
//
//    10f83f09  CMP  EAX, 0x10          ; WM_CLOSE? ← hook here (5 bytes)
//    10f83f0c  JNZ  0x10f83f35         ;   no → next handler
//    10f83f0e  MOV  EDX, [ESI]         ; vtable ptr
//    10f83f10  MOV  ECX, ESI           ; this
//    10f83f12  CALL [EDX+0xDC]         ; virtual OnClose() → DestroyWindow
//    10f83f18  TEST EAX, EAX
//    10f83f1a  JNZ  0x10f8464a         ; nonzero → DefWindowProc
//    10f83f20  XOR  EAX, EAX           ; return value = 0
//    10f83f22  MOV  ECX, [EBP-0xC]     ; restore SEH chain link
//    10f83f25  MOV  FS:[0], ECX        ; epilogue continues...
//    10f83f32  RET  0xC                ; return 0 – message handled
//
//  Per the UT2004 source, WWindow::OnClose() always returns true (nonzero),
//  so the path taken is always: OnClose() → JNZ → DefWindowProc (0x10f8464a).
//  Focus restoration must therefore happen UNCONDITIONALLY after OnClose(),
//  before the JNZ branch – not only in the "returns 0" path.
//
//  Hook strategy:
//    JMP_HOOK at 0x10f83f09 overwrites 5 bytes:
//      83 F8 10  – CMP EAX, 0x10
//      0F 85     – first 2 bytes of JNZ 0x10f83f35 (6-byte instruction)
//
//    In the hook:
//      • Non-WM_CLOSE: replay the CMP result, jump to the original JNZ target
//        (0x10f83f35) so all other messages are handled normally.
//      • WM_CLOSE:
//          1. Save the owner HWND *before* OnClose() destroys the window.
//          2. Call OnClose() through the vtable (exactly as the original does).
//          3. UNCONDITIONALLY restore focus to the saved owner (EAX saved
//             around the call so the return value is preserved).
//          4. If OnClose returned 0: do the standard return-0 epilogue
//             (jump to 0x10f83f25).
//          5. If OnClose returned nonzero: fall through to DefWindowProc
//             (jump to 0x10f8464a).
//
//  Register invariants at hook entry (from WndProc prologue):
//    EAX = message id (Msg parameter)
//    ESI = WWindow* (this)   [MOV ESI,ECX at 0x10f83d93, callee-saved]
//    EBP = valid stack frame [push ebp / mov ebp,esp at entry]
//
// ============================================================================

// Saved owner HWND captured before the window is destroyed by OnClose().
// Single-threaded Win32 app – no synchronisation needed.
static HWND s_closeOwner = NULL;

//  Helper: capture the owner HWND while the window is still alive 
// __fastcall: WWindow* in ECX.  WWindow::hWnd is stored at offset 4 (confirmed
// from the WM_ACTIVATE handler: MOV EAX,[ESI+0x4] → SendMessageA(EAX,...)).
static void __fastcall SaveOwnerBeforeClose(void* wwindow)
{
    const HWND hwnd = *reinterpret_cast<HWND*>(static_cast<char*>(wwindow) + 4);
    s_closeOwner = NULL;
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (!owner) owner = GetParent(hwnd);
    s_closeOwner = owner;
}

// Helper: restore keyboard focus to the saved owner 
// __fastcall: owner HWND in ECX.
static void __fastcall RestoreFocusToOwner(HWND owner)
{
    if (!owner || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);
}

//  JMP hook at 0x10f83f09 
// Overwrites:  83 F8 10 0F 85  (CMP EAX,0x10 + first 2 bytes of JNZ 0x10f83f35)
JMP_HOOK(0x10f83f09, WWindowWMCloseHook)
{
    // Jump targets used in the __asm block below.
    static int s_not_wm_close = 0x10f83f35;  // original JNZ target: next message check
    static int s_defwndproc   = 0x10f8464a;  // OnClose returned nonzero → DefWindowProc
    static int s_epilogue     = 0x10f83f25;  // OnClose returned 0  → epilogue start

    __asm
    {
        // Replicate the original CMP + conditional branch 
        cmp     eax, 0x10
        jne     not_wm_close

        //  WM_CLOSE path 

        // 1. Save the owner HWND while the HWND is still alive.
        //    SaveOwnerBeforeClose is __fastcall: arg in ECX; may clobber EAX/ECX/EDX.
        push    eax                         // preserve EAX = 0x10
        mov     ecx, esi                    // arg0 = WWindow* (this)
        call    SaveOwnerBeforeClose
        pop     eax                         // restore EAX = 0x10

        // 2. Replay the original vtable dispatch (now destroys the window):
        //      MOV EDX,[ESI]   (8B 16)
        //      MOV ECX,ESI     (8B CE)
        //      CALL [EDX+0xDC]
        mov     edx, dword ptr [esi]        // vtable ptr (reloaded; may have been clobbered)
        mov     ecx, esi                    // this
        call    dword ptr [edx + 0xdc]      // virtual OnClose() – window is destroyed here

        // 3. Unconditionally restore focus regardless of OnClose return value.
        //    WWindow::OnClose() always returns true (nonzero) per UT2004 source,
        //    so restoring only in the "returns 0" branch means it never fires.
        //    Save EAX (OnClose return value) around the call since RestoreFocusToOwner
        //    is __fastcall and may clobber EAX/ECX/EDX.
        push    eax                             // save OnClose return value
        mov     ecx, dword ptr [s_closeOwner]   // __fastcall arg0 = saved owner HWND
        test    ecx, ecx
        jz      wm_close_branch
        call    RestoreFocusToOwner             // clobbers EAX/ECX/EDX – that's fine

    wm_close_branch:
        pop     eax                             // restore OnClose return value

        // 4. Branch as the original code would.
        test    eax, eax
        jnz     wm_close_defwndproc

        // 4a. OnClose returned 0: replay XOR EAX,EAX + epilogue jump.
        xor     eax, eax                    // return value = 0
        mov     ecx, dword ptr [ebp - 0xc] // SEH chain link (restored in epilogue)
        jmp     dword ptr [s_epilogue]      // → 0x10f83f25  (MOV FS:[0],ECX; ...)

    wm_close_defwndproc:
        // 4b. OnClose returned nonzero: let DefWindowProc deal with it.
        jmp     dword ptr [s_defwndproc]    // → 0x10f8464a

        // Non-WM_CLOSE path 
    not_wm_close:
        // Replicate the original JNZ taken: jump to the next handler.
        jmp     dword ptr [s_not_wm_close]  // → 0x10f83f35
    }
}

void DialogFix::Initialize()
{
    INSTALL_HOOKS;
}
