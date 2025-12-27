#include "pch.h"
#include "Shadows.h"
#include "Hooks.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include "ShadowMapFilter.h"

INIT_HOOKS;

#ifdef LIGHTMAP_OVERRIDE_RESOLUTION

JMP_HOOK(0x111A06A3, MaxLightMapResolution) {
    static int Return = 0x111A06A8;
    __asm {
        cmp eax, LIGHTMAP_MAX_RES
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x111A066D, MaxLightMapResolution2) {
    static int Return = 0x111A0672;
    __asm {
        cmp eax, LIGHTMAP_MAX_RES
        jmp dword ptr[Return]
    }
}

#endif

#ifdef LIGHTMAP_DISABLE_DOWNSAMPLING

// fully disable compression - breaks lightmaps in game
//JMP_HOOK(0x11081285, DisableLightmapCompression) {
//    static int Return = 0x1108128E;
//    __asm {
//        jmp dword ptr[Return]
//    }
//}

JMP_HOOK(0x1119EF79, DisableDownsample) {
    static int Return = 0x1119F168;
    static void* sourceBuffer;
    static void* targetBuffer;

    __asm {
        mov ecx, [ebp - 0x1C]
        mov[sourceBuffer], esi
        mov[targetBuffer], ecx
        mov     ecx, [edi + 0x2C]
        lea     ebx, [edi + 0x24]
        pushad
    }
    ShadowMapFilter::ProcessLightmap(sourceBuffer, targetBuffer);
    __asm {
        popad
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119F168, DisableDownsample2) {
    static int Return = 0x1119F16D;
    __asm {
        mov     eax, LIGHTMAP_TEXTURE_BUFFER_SIZE
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119F1A3, DisableDownsample3) {
    static int Return = 0x1119F1A9;
    __asm {
        add     ecx, LIGHTMAP_TEXTURE_BUFFER_SIZE
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119F1EA, DisableDownsample4) {
    static int Return = 0x1119F1EF;
    __asm {
        push LIGHTMAP_TEXTURE_BUFFER_SIZE
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119EF39, DisableDownsample5) {
    static int Return = 0x1119EF3E;
    __asm {
        push LIGHTMAP_TEXTURE_BUFFER_SIZE
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119F242, DisableDownsample6) {
    static int Return = 0x1119F247;
    __asm {
        mov eax, LIGHTMAP_TEXTURE_RES
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119F22B, DisableDownsample7) {
    static int Return = 0x1119F235;
    __asm {
        push LIGHTMAP_TEXTURE_RES
        push LIGHTMAP_TEXTURE_RES
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x1119EF44, DisableDownsample8) {
    static int Return = 0x1119EF49;
    __asm {
        mov ebx, LIGHTMAP_TEXTURE_BUFFER_SIZE
        jmp dword ptr[Return]
    }
}

#endif

void Shadows::Initialize()
{
    INSTALL_HOOKS;
}