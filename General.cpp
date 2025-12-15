#include "pch.h"
#include "General.h"

#include "Hooks.h"

INIT_HOOKS;

JMP_HOOK(0x110518AD, RemoveAudioSizeLimit) {
    static int Return = 0x110518B3;
    __asm {
        jmp dword ptr[Return]
    }
}

void General::Initialize()
{
    INSTALL_HOOKS;
}