#pragma once
#include "pch.h"
#include "Graphics.h"
#include "Reloaded.Editor\Include\d3d8to9\source\d3d8.hpp"
#include "Reloaded.Editor\Include\d3d8to9\source\d3dx9.hpp"
#include "Reloaded.Editor\Include\d3d8to9\source\d3d8to9.hpp"
#include "Reloaded.Editor\Include\d3d8to9\source\d3d8types.hpp"
#include "Reloaded.Editor\Include\d3d8to9\source\interface_query.hpp"

#include "Hooks.h"
#include "logger.h"
#include <format>
#include <set>
#include <iostream>
#include <thread>
#include <chrono>
#include <timeapi.h>
#include <Windows.h>
#include "StringOperations.h"
#include "MemoryWriter.h"
#include "GameStructs.h"
#include <stdio.h>
#include <unordered_set>
#include <numbers>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d8to9.lib")

INIT_HOOKS;

static IDirect3D8* d3d;
static IDirect3DDevice8* pDevice;
static Direct3DDevice8* pDevice8;
static IDirect3DDevice9* pDevice9;
static D3DCAPS8* caps;
static int BackBufferWidth = 0;
static int BackBufferHeight = 0;
static int RenderWidth = 0;
static int RenderHeight = 0;
static float cappedAspectRatio = 1;

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT SDKVersion);

IDirect3D8* CreateD3D() {
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    return d3d;
}

JMP_HOOK(0x10F0E863, HookD3DCreate8) {
    static int Return = 0x10F0E871;
    __asm {
        mov byte ptr[ebp - 0x4], 01
        pushad
    }
    CreateD3D();
    __asm {
        popad
        mov eax, dword ptr[d3d]
        jmp dword ptr[Return]
    }
}

void PrintParams(D3DPRESENT_PARAMETERS8* d3dpp) {
    debug_cout << std::format("BackBufferWidth: {}", d3dpp->BackBufferWidth) << "\n";
    debug_cout << std::format("BackBufferHeight: {}", d3dpp->BackBufferHeight) << "\n";
    debug_cout << std::format("BackBufferFormat: {:#x}", static_cast<int>(d3dpp->BackBufferFormat)) << "\n";
    debug_cout << std::format("BackBufferCount: {}", d3dpp->BackBufferCount) << "\n";
    debug_cout << std::format("MultiSampleType: {:#x}", static_cast<int>(d3dpp->MultiSampleType)) << "\n";
    debug_cout << std::format("SwapEffect: {:#x}", static_cast<int>(d3dpp->SwapEffect)) << "\n";
    debug_cout << std::format("Windowed: {}", d3dpp->Windowed) << "\n";
    debug_cout << std::format("EnableAutoDepthStencil: {}", d3dpp->EnableAutoDepthStencil) << "\n";
    debug_cout << std::format("AutoDepthStencilFormat: {:#x}", static_cast<int>(d3dpp->AutoDepthStencilFormat)) << "\n";
    debug_cout << std::format("Flags: {:#x}", d3dpp->Flags) << "\n";
    debug_cout << std::format("FullScreen_RefreshRateInHz: {}", d3dpp->FullScreen_RefreshRateInHz) << "\n";
    debug_cout << std::format("FullScreen_PresentationInterval: {:#x}", d3dpp->FullScreen_PresentationInterval) << "\n";
}

/*
  Credit to https://github.com/crosire/reshade

  Direct3D9SetSwapEffectUpgradeShim has been taken from:
  https://github.com/crosire/reshade/commit/3fe0b050706fb9f3510ed48d619cad71f7cb28f2#diff-74772e50e2921e0bf69f0470e81d072d30c071b6e4b30af8cfd6cda58cf249eeR35
  and lightly modified.

  Copyright (C) 2014 Patrick Mours. All rights reserved.
  License: https://github.com/crosire/reshade#license
*/
void WINAPI Direct3D9SetSwapEffectUpgradeShim(int unknown = 0)
{
    typedef const char* (__stdcall* GetReShadeVersionFunc)();

    auto d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
    {
        Logger::log("d3d9.dll not loaded");
        return;
    }

    auto setSwapEffectUpgradeShim = GetProcAddress(d3d9, reinterpret_cast<LPCSTR>(18));
    if (!setSwapEffectUpgradeShim)
    {
        Logger::log("Unknown ordinal in d3d9.dll");
        return;
    }

    Logger::log("Direct3D9SetSwapEffectUpgradeShim(0)");
    reinterpret_cast<decltype(&Direct3D9SetSwapEffectUpgradeShim)>(setSwapEffectUpgradeShim)(unknown);
}

HRESULT CreateDevice(D3DPRESENT_PARAMETERS8* pPresentationParameters, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, IDirect3DDevice8** ppReturnedDeviceInterface)
{
    Logger::log("Hooked CreateDevice");
    Direct3D9SetSwapEffectUpgradeShim();
    Logger::log("d3d->CreateDevice:");
    PrintParams(pPresentationParameters);

    auto result = d3d->CreateDevice(Adapter, DeviceType, pPresentationParameters->hDeviceWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    pDevice = *ppReturnedDeviceInterface;
    pDevice8 = reinterpret_cast<Direct3DDevice8*>(pDevice);
    pDevice9 = pDevice8->GetProxyInterface();
    return result;
}

HRESULT CreateDeviceOverride(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS8* pPresentationParameters, IDirect3DDevice8** ppReturnedDeviceInterface) {
    HRESULT result;
    result = CreateDevice(pPresentationParameters, Adapter, DeviceType, hFocusWindow, BehaviorFlags, ppReturnedDeviceInterface);

    return result;
}

JMP_HOOK(0x10F10C96, CreatingDevice) {
    static int Return = 0x10F10CB0;
    //static int Fail = 0x;
    static D3DPRESENT_PARAMETERS8* d3dpp;
    static UINT Adapter;
    static D3DDEVTYPE DeviceType;
    static HWND hFocusWindow;
    static DWORD BehaviorFlags;
    static D3DPRESENT_PARAMETERS8* pPresentationParameters;
    static IDirect3DDevice8** ppReturnedDeviceInterface;
    static HRESULT result;
    __asm {

        mov[ppReturnedDeviceInterface], ebx
        lea     ebx, [ecx + 0x46A8]
        mov     ecx, [ecx + 0x4678]
        mov[pPresentationParameters], ebx
        mov[BehaviorFlags], esi
        mov     esi, eax
        mov     eax, [edx]
        mov[hFocusWindow], esi
        mov[DeviceType], edi
        mov[Adapter], ecx
        pushad
    }
    result = CreateDeviceOverride(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    __asm {
        popad
        mov eax, dword ptr[result]
        jmp     dword ptr[Return]
    }
}

const auto MarkerColor = 0xFE0D;

void QuickPushHit(UViewport* viewport) {
    //RECT scissorRect = {
    //    viewport->HitX(),
    //    viewport->HitY(),
    //    viewport->HitX() + viewport->HitXL(),
    //    viewport->HitY() + viewport->HitYL()
    //};
    //pDevice9->SetScissorRect(&scissorRect);
    //pDevice9->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

    D3DRECT hitBox = {
        viewport->HitX(),
        viewport->HitY(),
        viewport->HitX() + viewport->HitXL(),
        viewport->HitY() + viewport->HitYL(),

    };
    pDevice9->Clear(1, &hitBox, D3DCLEAR_TARGET, MarkerColor, 1.0f, 0);
}

JMP_HOOK(0x10F1B451, PushHit) {
    static int Return = 0x10F1B574;
    static UViewport* uViewport;
    __asm {
        mov dword ptr[uViewport], edi
        pushad
    }
    QuickPushHit(uViewport);
    __asm {
        popad
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10F1B5AD, DisableLockRectRelease) {
    __asm {
        mov eax, [ebp - 0x18]
        push eax
        add esp, 0x4
        mov eax, [ebp - 0x18]
        push eax
        add esp, 0x4
        mov ecx, [ebp - 0x0C]
        pop edi
        pop esi
        mov fs : [0x00000000] , ecx
        pop ebx
        mov esp, ebp
        pop ebp
        ret 0x8
    }
}

bool Graphics::IsWine() {
    static const char* (CDECL * pwine_get_version)(void);
    HMODULE hntdll = GetModuleHandle(L"ntdll.dll");
    if (!hntdll)
    {
        return false;
    }

    auto gwv = (void*)GetProcAddress(hntdll, "wine_get_version");
    return gwv;
}

// A simplified rewrite of the original PopHit implementation
bool CheckHit(UViewport* viewport) {
    const RECT hitBox = {
        viewport->HitX(),
        viewport->HitY(),
        viewport->HitX() + viewport->HitXL(),
        viewport->HitY() + viewport->HitYL(),
    };

    IDirect3DSurface8* surface = nullptr;
    pDevice8->GetRenderTarget(&surface);

    D3DLOCKED_RECT lockedRect;
    surface->LockRect(&lockedRect, &hitBox, D3DLOCK_READONLY);

    bool hit = false;
    BYTE* rowPointer = (BYTE*)lockedRect.pBits;
    for (int y = 0; y < viewport->HitYL(); y++) {
        DWORD* pixel = (DWORD*)rowPointer;

        for (int x = 0; x < viewport->HitXL(); x++) {
            if (pixel[x] != MarkerColor) {
                hit = true;
                break;
            }
        }

        if (hit) break;
        rowPointer += lockedRect.Pitch;
    }

    surface->UnlockRect();
    surface->Release();

    return hit;
}

static IDirect3DSurface9* smallSurfVRAM = nullptr;
static IDirect3DSurface9* smallSurfSysMem = nullptr;

// Optimized verison of PopHit for dx9
bool QuickPopHit(UViewport* viewport) {
    int hitWidth = viewport->HitXL();
    int hitHeight = viewport->HitYL();

    const RECT sourceRect = {
        viewport->HitX(),
        viewport->HitY(),
        viewport->HitX() + hitWidth,
        viewport->HitY() + hitHeight,
    };

    IDirect3DSurface9* renderTarget = nullptr;
    pDevice9->GetRenderTarget(0, &renderTarget);

    D3DSURFACE_DESC desc;
    renderTarget->GetDesc(&desc);

    static int cachedW = 0;
    static int cachedH = 0;

    if (hitWidth != cachedW || hitHeight != cachedH || !smallSurfVRAM || !smallSurfSysMem) {
        if (smallSurfVRAM) smallSurfVRAM->Release();
        if (smallSurfSysMem) smallSurfSysMem->Release();

        pDevice9->CreateRenderTarget(hitWidth, hitHeight, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &smallSurfVRAM, nullptr);
        pDevice9->CreateOffscreenPlainSurface(hitWidth, hitHeight, desc.Format, D3DPOOL_SYSTEMMEM, &smallSurfSysMem, nullptr);

        cachedW = hitWidth;
        cachedH = hitHeight;
    }

    pDevice9->StretchRect(renderTarget, &sourceRect, smallSurfVRAM, nullptr, D3DTEXF_NONE);
    renderTarget->Release();

    pDevice9->GetRenderTargetData(smallSurfVRAM, smallSurfSysMem);

    D3DLOCKED_RECT lockedRect;
    smallSurfSysMem->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);

    bool hit = false;
    BYTE* rowPointer = (BYTE*)lockedRect.pBits;

    for (int y = 0; y < hitHeight; y++) {
        DWORD* pixel = (DWORD*)rowPointer;

        for (int x = 0; x < hitWidth; x++) {
            if (pixel[x] != MarkerColor) {
                hit = true;
                break;
            }
        }

        if (hit) break;
        rowPointer += lockedRect.Pitch;
    }

    smallSurfSysMem->UnlockRect();

    //pDevice9->SetRenderState(D3DRS_SCISSORTESTENABLE, false);

    return hit;
}

void OnCreateDevice() {
    if (smallSurfVRAM) smallSurfVRAM->Release();
    if (smallSurfSysMem) smallSurfSysMem->Release();

    smallSurfVRAM = nullptr;
    smallSurfSysMem = nullptr;
    debug_wcout << "Freed resources\n";
}

JMP_HOOK(0x10F1B772, PopHit) {
    static uint32_t hit;
    static UViewport* viewport;
    __asm {
        pushad
        mov [viewport], ebx
    }
    hit = QuickPopHit(viewport);

    static int Return = 0x10F1BA3B;
    __asm {
        popad
        mov edi, dword ptr[hit]
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10F10B84, BeforeCreateDevice) {
    static int Return = 0x10F10B8A;
    __asm {
        mov     eax, [eax + 0x46A4]
        pushad
    }
    OnCreateDevice();
    __asm {
        popad
        jmp dword ptr[Return]
    }
}

#ifdef LIGHTMAP_OVERRIDE

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

void Graphics::Initialize()
{
    INSTALL_HOOKS;

    uint8_t nops[] = { 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x11463408, &editor_version, sizeof(editor_version));

    // Fix lighting
    if (IsWine()) {
        // TODO
    }
    else {
        MemoryWriter::WriteBytes(0x111A5B27, nops, 2);
    }
}