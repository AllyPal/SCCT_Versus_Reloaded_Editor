#pragma once

// =====================================================================
//  NormalMaps - native normal-mapping for the SCCT Versus engine
// =====================================================================
//  Hijacks the (otherwise unused) Material "Detail" slot as the carrier
//  for a tangent-space normal map: a Shader with Diffuse = colour map and
//  Detail = normal map.
//
//  Master switch g_ReloadedNormalMaps. Defaults false; loaded from
//  Reloaded_Editor.ini ([NormalMaps] Enabled) by ReloadedOptions.
//
//  Stage 1 (current): press F11 in a viewport to scan the open level for
//  Shaders carrying a Detail texture. Read-only; confirms the carrier set
//  and the live Detail property offset that Stage 2/3 will consume.
// =====================================================================

extern bool g_ReloadedNormalMaps;

// F11 / F9 hotkey entry points. extern "C" so the naked-asm key
// dispatcher in General.cpp can call them by an undecorated name.
// F11 rescans the level; F9 toggles the render pass for A/B compare.
extern "C" void __cdecl NormalMaps_HotkeyScan();
extern "C" void __cdecl NormalMaps_HotkeyToggleRender();

// d3d8to9 draw-path instrumentation. Called from d3d8to9_device.cpp's
// SetTexture and Draw* wrappers; defined in NormalMaps.cpp.
extern "C" void __cdecl NormalMaps_OnSetTexture(unsigned int stage, void* texture8);
extern "C" int  __cdecl NormalMaps_OnDraw(void);          // returns 1 if a carrier draw
extern "C" void __cdecl NormalMaps_PreCarrierDraw(void);  // wrap the carrier draw
extern "C" void __cdecl NormalMaps_PostCarrierDraw(void);
extern "C" void __cdecl NormalMaps_OnDeviceLost(void);    // release device resources

// Returns a neutral mid-gray IDirect3DTexture9* when 'texture8' is a
// hijacked Detail-slot carrier normal map (else null), so d3d8to9 can
// bind it in place and the engine's stock detail overlay is cancelled.
extern "C" void* __cdecl NormalMaps_QueryCarrierGray(void* texture8);

class NormalMaps
{
public:
    static void Initialize();
};
