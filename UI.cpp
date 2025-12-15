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

//static int SkipPointlessSecondPopupEntry = 0x10E3EBE0;
//__declspec(naked) void SkipPointlessSecondPopup() {
//    static int Return = 0x10E3EC26;
//    __asm {
//        jmp dword ptr[Return]
//    }
//}
//
//static int VerboseSavePopupEntry = 0x10E3E071;
//__declspec(naked) void VerboseSavePopup() {
//    static int Return = 0x10E3E076;
//    __asm {
//        push offset verbose_save_message
//        jmp dword ptr[Return]
//    }
//}
//
//static int SkipPointlessSecondPopupOnExitEntry = 0x10E3E0B8;
//__declspec(naked) void SkipPointlessSecondPopupOnExit() {
//    static int Return = 0x10E3ED8F;
//    __asm {
//        jmp dword ptr[Return]
//    }
//}

void UI::Initialize()
{
    INSTALL_HOOKS;
}