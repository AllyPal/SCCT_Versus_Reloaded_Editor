#include "pch.h"
#include "NormalMaps.h"
#include "logger.h"
#include "Rendering.h"
#include "Hooks.h"
#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

// =====================================================================
//  NormalMaps - Stages 1-3
// =====================================================================
//  Stage 1  carrier detection : scan the level's Shaders for a Detail
//                               texture (the normal-map carrier).
//  Stage 2  the shader        : tangent-space normal-mapping VS/PS, built
//                               at runtime via D3DCompiler.
//  Stage 3  render integration:
//    - d3d8to9's draw path is instrumented (our own code).
//    - CacheTexture (0x10f163c0) is hooked to bridge each carrier's
//      UTexture to the live IDirect3DTexture8* d3d8to9 binds.
//    - When a carrier surface is drawn, its draw is rendered with our
//      shader: per-pixel tangent-space normal mapping lit by a camera
//      head-lamp, so the bumps respond as the viewer moves.
//
//  All gated by g_ReloadedNormalMaps; with the toggle off the module
//  installs nothing and renders exactly as stock.
// =====================================================================

INIT_HOOKS;

bool g_ReloadedNormalMaps = false;

// Unwraps a d3d8to9 texture to the real D3D9 texture (defined in d3d8to9).
extern "C" void* __cdecl NormalMaps_GetTextureProxy(void* baseTexture8);

// --- Engine globals / offsets (ChaosTheory_Editor.exe, build 2110) ----
static constexpr uintptr_t GOBJECTS_DATA = 0x11697B70;
static constexpr uintptr_t GOBJECTS_NUM  = 0x11697B74;
static constexpr uintptr_t GNAMES_DATA   = 0x1169CFBC;
static constexpr uintptr_t GNAMES_NUM    = 0x1169CFC0;

static constexpr uint32_t FNAME_STR_OFF  = 0x0C;
static constexpr uint32_t UOBJ_NAME_OFF  = 0x20;
static constexpr uint32_t UOBJ_CLASS_OFF = 0x24;

static constexpr uint32_t USTRUCT_PROPLINK_OFF = 0x58;
static constexpr uint32_t UPROP_OFFSET_OFF     = 0x3C;
static constexpr uint32_t UPROP_LINKNEXT_OFF   = 0x40;

// FD3DTexture+0x2C = IDirect3DTexture8* (the d3d8to9 texture).
static constexpr uint32_t FD3DTEX_D3DTEX_OFF = 0x2C;

// =====================================================================
//  Stage 1 - level material scan
// =====================================================================
struct ScanResult
{
    enum { MAX = 256, LEN = 96 };
    bool  ok;
    int   detailOffset;
    int   shaderCount;
    int   carrierCount;
    char  shaderName[MAX][LEN];
    char  detailName[MAX][LEN];
    char  detailClass[MAX][LEN];
    void* detailObj[MAX];
};
static ScanResult s_scan;

static const char* FNameStr(void** gNames, int gNamesNum, int idx)
{
    if (idx < 0 || idx >= gNamesNum) return nullptr;
    void* e = gNames[idx];
    if (!e) return nullptr;
    return reinterpret_cast<const char*>(static_cast<char*>(e) + FNAME_STR_OFF);
}

static const char* ObjName(void* obj, void** gNames, int gNamesNum)
{
    if (!obj) return nullptr;
    return FNameStr(gNames, gNamesNum,
        *reinterpret_cast<int*>(static_cast<char*>(obj) + UOBJ_NAME_OFF));
}

static const char* ObjClassName(void* obj, void** gNames, int gNamesNum)
{
    if (!obj) return nullptr;
    void* cls = *reinterpret_cast<void**>(static_cast<char*>(obj) + UOBJ_CLASS_OFF);
    return ObjName(cls, gNames, gNamesNum);
}

static int FindPropertyOffset(void* cls, const char* propName,
                              void** gNames, int gNamesNum)
{
    void* p = *reinterpret_cast<void**>(
        static_cast<char*>(cls) + USTRUCT_PROPLINK_OFF);
    for (int guard = 0; p && guard < 8192; ++guard)
    {
        const char* nm = ObjName(p, gNames, gNamesNum);
        if (nm && strcmp(nm, propName) == 0)
            return *reinterpret_cast<int*>(
                static_cast<char*>(p) + UPROP_OFFSET_OFF);
        p = *reinterpret_cast<void**>(
            static_cast<char*>(p) + UPROP_LINKNEXT_OFF);
    }
    return -1;
}

static void CopyStr(char* dst, const char* src)
{
    strncpy_s(dst, ScanResult::LEN, src ? src : "?", _TRUNCATE);
}

// =====================================================================
//  Stage 4 - engine light enumeration
// =====================================================================
//  Every UE2 actor carries lighting fields (LightType, LightBrightness,
//  LightHue, LightSaturation, LightRadius) plus a Location. A placed
//  light is simply an actor whose LightType != LT_None. We find them by
//  walking GObjects: an object is an actor if its class exposes a
//  "LightType" property (resolved with the same reflection used for
//  "Detail"). The property offsets are identical for every actor, so
//  they are resolved once and cached; an open-addressing hash caches the
//  actor / not-actor verdict per class so repeat scans stay cheap.

struct EngineLight
{
    float pos[3];     // world-space position
    float color[3];   // RGB, brightness already folded in
    float radius;     // world-space falloff radius
    float rawBright;  // raw LightBrightness  (diagnostic)
    float rawRadius;  // raw LightRadius      (diagnostic)
};
static const int   MAX_LIGHTS = 1024;
static EngineLight  s_lights[MAX_LIGHTS];
static int          s_lightCount = 0;

// LightRadius is a float in this engine; this scales it to world units.
// Tunable - raise it if normal-mapped surfaces look under-lit.
static const float  LIGHT_RADIUS_SCALE = 40.0f;

// Actor light-property offsets - resolved once (-1 = unresolved/absent).
static int s_offLightType       = -1;
static int s_offLocation        = -1;
static int s_offLightBrightness = -1;
static int s_offLightHue        = -1;
static int s_offLightSaturation = -1;
static int s_offLightRadius     = -1;

struct ClassEntry { void* cls; bool isActor; };
static ClassEntry s_classHash[4096];   // zero-init: cls == null means empty

// Is 'cls' an actor class? Cached per class pointer. The first actor
// class seen also resolves the shared light-property offsets.
static bool ClassIsActor(void* cls, void** gN, int gNn)
{
    unsigned base = static_cast<unsigned>(
        reinterpret_cast<uintptr_t>(cls) >> 4) & 4095u;
    for (unsigned probe = 0; probe < 4096; ++probe)
    {
        ClassEntry& e = s_classHash[(base + probe) & 4095u];
        if (e.cls == cls) return e.isActor;
        if (e.cls == nullptr)
        {
            int  ltOff   = FindPropertyOffset(cls, "LightType", gN, gNn);
            bool isActor = (ltOff > 0);
            if (isActor && s_offLightType < 0)
            {
                s_offLightType       = ltOff;
                s_offLocation        = FindPropertyOffset(cls, "Location",        gN, gNn);
                s_offLightBrightness = FindPropertyOffset(cls, "LightBrightness", gN, gNn);
                s_offLightHue        = FindPropertyOffset(cls, "LightHue",        gN, gNn);
                s_offLightSaturation = FindPropertyOffset(cls, "LightSaturation", gN, gNn);
                s_offLightRadius     = FindPropertyOffset(cls, "LightRadius",     gN, gNn);
            }
            e.cls = cls;
            e.isActor = isActor;
            return isActor;
        }
    }
    return false;
}

static unsigned char ByteAt(void* obj, int off, unsigned char fallback)
{
    if (off <= 0) return fallback;
    return *reinterpret_cast<unsigned char*>(static_cast<char*>(obj) + off);
}

static float FloatAt(void* obj, int off, float fallback)
{
    if (off <= 0) return fallback;
    return *reinterpret_cast<float*>(static_cast<char*>(obj) + off);
}

// Classic HSV -> RGB. h6 in [0,6), s & v in [0,1].
static void HSVtoRGB(float h6, float s, float v, float* rgb)
{
    if (h6 >= 6.0f) h6 -= 6.0f;
    if (h6 <  0.0f) h6 += 6.0f;
    int   seg = static_cast<int>(h6);
    float f   = h6 - static_cast<float>(seg);
    float p   = v * (1.0f - s);
    float q   = v * (1.0f - s * f);
    float t   = v * (1.0f - s * (1.0f - f));
    float r, g, b;
    switch (seg)
    {
        case 0:  r=v; g=t; b=p; break;
        case 1:  r=q; g=v; b=p; break;
        case 2:  r=p; g=v; b=t; break;
        case 3:  r=p; g=q; b=v; break;
        case 4:  r=t; g=p; b=v; break;
        default: r=v; g=p; b=q; break;
    }
    rgb[0]=r; rgb[1]=g; rgb[2]=b;
}

// SCCT light colour: Hue 0-255 round the wheel; Saturation is INVERTED
// (255 = white, 0 = fully saturated); LightBrightness is a float (0-255
// nominal) folded in as the HSV value.
static void LightColorFromHSV(unsigned char hue, unsigned char sat,
                              float bright, float* rgb)
{
    float h6 = (static_cast<float>(hue) / 255.0f) * 6.0f;
    float s  = 1.0f - static_cast<float>(sat) / 255.0f;
    float v  = bright / 255.0f;
    HSVtoRGB(h6, s, v, rgb);
}

// If 'obj' is an actor with LightType != LT_None, append it to s_lights.
static void CollectIfLight(void* obj, void* cls, void** gN, int gNn)
{
    if (s_lightCount >= MAX_LIGHTS) return;
    if (!ClassIsActor(cls, gN, gNn)) return;
    if (s_offLightType <= 0 || s_offLocation <= 0) return;

    unsigned char lt = *reinterpret_cast<unsigned char*>(
        static_cast<char*>(obj) + s_offLightType);
    if (lt == 0) return;   // LT_None - not an active light

    EngineLight& L = s_lights[s_lightCount];

    const float* loc = reinterpret_cast<const float*>(
        static_cast<char*>(obj) + s_offLocation);
    L.pos[0] = loc[0];
    L.pos[1] = loc[1];
    L.pos[2] = loc[2];

    unsigned char hue = ByteAt (obj, s_offLightHue,        0);
    unsigned char sat = ByteAt (obj, s_offLightSaturation, 255);
    float  bright     = FloatAt(obj, s_offLightBrightness, 64.0f);
    float  rawRad     = FloatAt(obj, s_offLightRadius,     16.0f);

    LightColorFromHSV(hue, sat, bright, L.color);
    L.rawBright = bright;
    L.rawRadius = rawRad;
    L.radius    = rawRad * LIGHT_RADIUS_SCALE;    // float -> world units
    if (L.radius < 64.0f) L.radius = 64.0f;

    ++s_lightCount;
}

static void ScanWorker(ScanResult* r)
{
    r->ok = false;
    r->detailOffset = -1;
    r->shaderCount = 0;
    r->carrierCount = 0;

    __try
    {
        void** gObj   = *reinterpret_cast<void***>(GOBJECTS_DATA);
        int    gObjN  = *reinterpret_cast<int*>   (GOBJECTS_NUM);
        void** gName  = *reinterpret_cast<void***>(GNAMES_DATA);
        int    gNameN = *reinterpret_cast<int*>   (GNAMES_NUM);
        if (!gObj || gObjN <= 0 || !gName || gNameN <= 0) return;

        s_lightCount = 0;     // light list is rebuilt fresh every scan

        for (int i = 0; i < gObjN; ++i)
        {
            void* obj = gObj[i];
            if (!obj) continue;

            void* cls = *reinterpret_cast<void**>(
                static_cast<char*>(obj) + UOBJ_CLASS_OFF);
            if (!cls) continue;

            // Stage 4 - collect every active light actor.
            CollectIfLight(obj, cls, gName, gNameN);

            // Stage 1 - carrier (a Shader carrying a Detail texture).
            const char* clsName = ObjName(cls, gName, gNameN);
            if (!clsName || strcmp(clsName, "Shader") != 0) continue;

            r->shaderCount++;

            if (r->detailOffset < 0)
                r->detailOffset =
                    FindPropertyOffset(cls, "Detail", gName, gNameN);
            if (r->detailOffset <= 0) continue;

            void* detail = *reinterpret_cast<void**>(
                static_cast<char*>(obj) + r->detailOffset);
            if (!detail) continue;

            if (r->carrierCount < ScanResult::MAX)
            {
                int k = r->carrierCount++;
                CopyStr(r->shaderName[k],  ObjName(obj, gName, gNameN));
                CopyStr(r->detailName[k],  ObjName(detail, gName, gNameN));
                CopyStr(r->detailClass[k], ObjClassName(detail, gName, gNameN));
                r->detailObj[k] = detail;
            }
        }
        r->ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        r->ok = false;
    }
}

// =====================================================================
//  Stage 3 - carrier bridge + draw instrumentation
// =====================================================================
struct CarrierBridge
{
    char  name[64];
    void* utexture;    // the Detail (normal map) UTexture object
    void* d3dtex;      // IDirect3DBaseTexture8* d3d8to9 binds (null until found)
    void* textureItf;  // engine texture-interface passed to CacheTexture
};
static CarrierBridge s_bridge[16];
static int      s_bridgeCount = 0;

static void*    s_boundTex[8]      = {};
static unsigned s_drawCount        = 0;
static unsigned s_carrierDrawCount = 0;

// Set by OnDraw when the current draw is a carrier; consumed by PreCarrierDraw.
static void* s_curNormalTex = nullptr;
static DWORD s_lastAutoScan = 0;      // GetTickCount of the last auto-scan

// F10 A/B toggle. When false the additive normal-map pass is skipped,
// but the detail-overlay suppression stays on - so the surface shows
// plain engine lighting for a clean side-by-side comparison.
static bool s_renderEnabled = true;

static void RunScan(bool verbose);    // forward decls (used by OnDraw)
static void EnsureShaders();

// POD-only, SEH-guarded matcher. A carrier UTexture can be freed mid
// lighting-rebuild; the __try turns a stale-pointer read into a clean
// miss instead of a crash. No C++ objects here, so __try is legal.
static bool MatchCacheTexture_Raw(void* texture, void* fd3dtexture,
                                  int* idxOut, void** itfOut, void** d3dOut)
{
    *idxOut = -1;
    __try
    {
        for (int i = 0; i < s_bridgeCount; ++i)
        {
            CarrierBridge& b = s_bridge[i];
            bool hit = false;

            if (b.textureItf)
            {
                hit = (texture == b.textureItf);
            }
            else if (b.utexture)
            {
                for (uint32_t k = 0; k <= 0xC0 && !hit; k += 4)
                {
                    if (static_cast<char*>(texture) - k == b.utexture)
                        hit = true;
                    else if (*reinterpret_cast<void**>(
                                 static_cast<char*>(b.utexture) + k) == texture)
                        hit = true;
                }
            }
            if (!hit) continue;

            *idxOut = i;
            *itfOut = texture;
            *d3dOut = *reinterpret_cast<void**>(
                static_cast<char*>(fd3dtexture) + FD3DTEX_D3DTEX_OFF);
            return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

extern "C" void __cdecl NormalMaps_OnCacheTexture(void* texture, void* fd3dtexture)
{
    if (!g_ReloadedNormalMaps || s_bridgeCount == 0) return;
    if (!texture || !fd3dtexture) return;

    int   idx = -1;
    void* itf = nullptr;
    void* d3d = nullptr;
    if (!MatchCacheTexture_Raw(texture, fd3dtexture, &idx, &itf, &d3d)) return;
    if (idx < 0 || idx >= s_bridgeCount) return;

    CarrierBridge& b = s_bridge[idx];
    if (!b.textureItf) b.textureItf = itf;
    if (d3d != b.d3dtex)
    {
        b.d3dtex = d3d;
        char m[200];
        snprintf(m, sizeof(m),
            "NormalMaps: bridge linked - '%s'  D3D texture=0x%08X",
            b.name, static_cast<unsigned>(reinterpret_cast<uintptr_t>(d3d)));
        Logger::log(m);
    }
}

// Hook inside FD3DRenderInterface::CacheTexture. At 0x10f16462:
//   ESI = FD3DTexture* ,  [EBP+8] = the texture that was cached.
JMP_HOOK(0x10F16462, CacheTextureTap)
{
    static int Return = 0x10F16468;
    __asm
    {
        pushad
        push esi
        push dword ptr [ebp + 8]
        call NormalMaps_OnCacheTexture
        add  esp, 8
        popad
        mov  dword ptr [esi + 0x20], eax
        pop  edi
        mov  eax, esi
        jmp  dword ptr [Return]
    }
}

extern "C" void __cdecl NormalMaps_OnSetTexture(unsigned int stage, void* texture8)
{
    if (!g_ReloadedNormalMaps) return;
    if (stage < 8) s_boundTex[stage] = texture8;
}

extern "C" int __cdecl NormalMaps_OnDraw(void)
{
    if (!g_ReloadedNormalMaps) return 0;
    ++s_drawCount;

    // Auto-refresh the scan + bridge + shaders so the feature works
    // without anyone pressing F11.
    DWORD now = GetTickCount();
    if (now - s_lastAutoScan > 3000)
    {
        s_lastAutoScan = now;
        RunScan(false);
        EnsureShaders();
    }

    // F10 compare toggle - keep scanning, but report no carrier draws so
    // the additive pass is skipped and the surface renders plain.
    if (!s_renderEnabled) return 0;

    for (int s = 0; s < 8; ++s)
    {
        void* t = s_boundTex[s];
        if (!t) continue;
        for (int i = 0; i < s_bridgeCount; ++i)
            if (s_bridge[i].d3dtex && s_bridge[i].d3dtex == t)
            {
                ++s_carrierDrawCount;
                s_curNormalTex = t;       // hand the normal map to PreCarrierDraw
                return 1;
            }
    }
    return 0;
}

// =====================================================================
//  Stage 3 - the shader override
// =====================================================================
//  Forward decls of the compiled shaders (built by EnsureShaders).
static int s_shaderState = 0;
static IDirect3DVertexShader9* s_vs = nullptr;
static IDirect3DPixelShader9*  s_ps = nullptr;

// Neutral 4x4 mid-gray texture. Bound in place of a hijacked carrier
// normal map so the engine's stock detail-overlay becomes a no-op.
static IDirect3DTexture9* s_grayTex = nullptr;

// Saved device state, restored after the carrier draw.
static IDirect3DVertexShader9* s_saveVS   = nullptr;
static IDirect3DPixelShader9*  s_savePS   = nullptr;
static IDirect3DBaseTexture9*  s_saveTex1 = nullptr;
static DWORD s_saveAddrU = 1;   // D3DTADDRESS_WRAP
static DWORD s_saveAddrV = 1;
static DWORD s_saveMinF = 2;    // D3DTEXF_LINEAR
static DWORD s_saveMagF = 2;
static DWORD s_saveMipF = 2;
static DWORD s_saveZFunc  = 0;
static DWORD s_saveZWrite = 0;
static DWORD s_saveABE    = 0;
static DWORD s_saveSrcB   = 0;
static DWORD s_saveDstB   = 0;
static DWORD s_saveSlopeBias = 0;
static DWORD s_saveDepthBias = 0;
static bool s_overrideActive = false;

// D3DRS_DEPTHBIAS / D3DRS_SLOPESCALEDEPTHBIAS take the bit-pattern of a
// float. Negative values pull the polygon toward the camera so our
// additive pass reliably wins the LESSEQUAL depth test - the slope-
// scaled term grows with the surface's depth gradient, which is exactly
// what spikes when the camera gets very close to a wall.
static DWORD AsBiasDword(float f)
{
    return *reinterpret_cast<DWORD*>(&f);
}

static void Mat4Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& o)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o.m[r][c] = a.m[r][0]*b.m[0][c] + a.m[r][1]*b.m[1][c]
                      + a.m[r][2]*b.m[2][c] + a.m[r][3]*b.m[3][c];
}

// Pick up to 16 lights for this draw (the ones nearest the camera) and
// pack them into the pixel-shader constant layout lpr[] = c2..c17 (xyz
// world pos, w falloff radius). With no engine lights it falls back to
// a single white head-lamp at the camera. Unused slots are parked far
// off-world so the shader's fixed loop attenuates them to nothing.
static const int NM_MAXSEL = 16;

static int SelectLights(const float* eye, float* lpr)
{
    for (int k = 0; k < NM_MAXSEL; ++k)
    {
        lpr[k*4+0] = 1.0e9f; lpr[k*4+1] = 1.0e9f;
        lpr[k*4+2] = 1.0e9f; lpr[k*4+3] = 1.0f;
    }

    if (s_lightCount <= 0)
    {
        // head-lamp fallback - white light at the camera, no falloff
        lpr[0] = eye[0]; lpr[1] = eye[1]; lpr[2] = eye[2]; lpr[3] = 1.0e6f;
        return 1;
    }

    int   idx[NM_MAXSEL];
    float d2 [NM_MAXSEL];
    int   n = 0;
    for (int i = 0; i < s_lightCount; ++i)
    {
        float dx = s_lights[i].pos[0] - eye[0];
        float dy = s_lights[i].pos[1] - eye[1];
        float dz = s_lights[i].pos[2] - eye[2];
        float dd = dx*dx + dy*dy + dz*dz;

        if (n < NM_MAXSEL || dd < d2[n - 1])
        {
            int j = (n < NM_MAXSEL) ? n++ : NM_MAXSEL - 1;  // grow / evict
            while (j > 0 && d2[j - 1] > dd)
            { d2[j] = d2[j - 1]; idx[j] = idx[j - 1]; --j; }
            d2[j] = dd; idx[j] = i;
        }
    }

    for (int k = 0; k < n; ++k)
    {
        const EngineLight& L = s_lights[idx[k]];
        lpr[k*4+0] = L.pos[0]; lpr[k*4+1] = L.pos[1];
        lpr[k*4+2] = L.pos[2]; lpr[k*4+3] = L.radius;
    }
    return n;
}

extern "C" void __cdecl NormalMaps_PreCarrierDraw(void)
{
    s_overrideActive = false;
    if (s_shaderState != 1 || !s_curNormalTex) return;

    IDirect3DDevice9* dev = Rendering::GetDevice9();
    if (!dev) return;

    // Only override real 3D world rendering. A perspective projection has
    // _44 == 0; texture-browser thumbnails and other 2D/ortho draws have
    // _44 == 1 and must be left completely alone.
    D3DMATRIX world, view, proj, wv, wvp;
    dev->GetTransform(D3DTS_PROJECTION, &proj);
    if (proj._44 > 0.5f || proj._44 < -0.5f) return;

    IDirect3DBaseTexture9* normal9 = reinterpret_cast<IDirect3DBaseTexture9*>(
        NormalMaps_GetTextureProxy(s_curNormalTex));
    if (!normal9) return;

    dev->GetTransform(D3DTS_WORLD, &world);
    dev->GetTransform(D3DTS_VIEW,  &view);
    Mat4Mul(world, view, wv);
    Mat4Mul(wv, proj, wvp);

    // camera (head-lamp) position in world space, from the view matrix
    float eye[4] = {
        -(view._41*view._11 + view._42*view._12 + view._43*view._13),
        -(view._41*view._21 + view._42*view._22 + view._43*view._23),
        -(view._41*view._31 + view._42*view._32 + view._43*view._33),
        1.0f
    };
    // x bumpScale   y fadeStart   z reliefScale   w fadeEnd  (world units)
    float params[4] = { 2.0f, 2000.0f, 3.0f, 9000.0f };

    // pick this draw's lights - real engine lights, or a head-lamp fallback
    float lightPosRad[NM_MAXSEL * 4];
    SelectLights(eye, lightPosRad);

    // --- save every piece of state we touch (after all early-outs) ---
    s_saveVS = nullptr; s_savePS = nullptr; s_saveTex1 = nullptr;
    dev->GetVertexShader(&s_saveVS);
    dev->GetPixelShader(&s_savePS);
    dev->GetTexture(1, &s_saveTex1);
    dev->GetSamplerState(1, D3DSAMP_ADDRESSU,  &s_saveAddrU);
    dev->GetSamplerState(1, D3DSAMP_ADDRESSV,  &s_saveAddrV);
    dev->GetSamplerState(1, D3DSAMP_MINFILTER, &s_saveMinF);
    dev->GetSamplerState(1, D3DSAMP_MAGFILTER, &s_saveMagF);
    dev->GetSamplerState(1, D3DSAMP_MIPFILTER, &s_saveMipF);
    dev->GetRenderState(D3DRS_ZFUNC,            &s_saveZFunc);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &s_saveZWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s_saveABE);
    dev->GetRenderState(D3DRS_SRCBLEND,         &s_saveSrcB);
    dev->GetRenderState(D3DRS_DESTBLEND,        &s_saveDstB);
    dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &s_saveSlopeBias);
    dev->GetRenderState(D3DRS_DEPTHBIAS,           &s_saveDepthBias);

    // --- our relief pass: re-draw the surface with a modulate-2x blend
    // (result = 2 * src * dst) so the bump shading multiplies whatever
    // the engine already rendered - brightening and darkening the bumps
    // without re-lighting the surface or double-counting. Depth-write
    // off so the engine's depth buffer and selection highlight survive.
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(s_ps);
    dev->SetVertexShaderConstantF(0, &wvp._11,   4);
    dev->SetVertexShaderConstantF(4, &world._11, 4);
    dev->SetPixelShaderConstantF (0, eye,         1);  // c0    EyePos
    dev->SetPixelShaderConstantF (1, params,      1);  // c1    Params
    dev->SetPixelShaderConstantF (2, lightPosRad, NM_MAXSEL);  // c2-17 LightPosRad
    dev->SetTexture(1, normal9);                  // s1 = normal map
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  D3DTADDRESS_WRAP);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  D3DTADDRESS_WRAP);
    // Trilinear filtering so the normal map blends smoothly between mip
    // levels - without it the detail snaps between mips and visibly
    // pops in and out as the camera distance changes.
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ZFUNC,            D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_DESTCOLOR);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_SRCCOLOR);
    // Pull the relief pass camera-ward. The slope-scaled term grows
    // with the surface's depth gradient, which is exactly what spikes
    // when the camera gets very close to a wall and made it shimmer.
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, AsBiasDword(-2.0f));
    dev->SetRenderState(D3DRS_DEPTHBIAS,           AsBiasDword(-0.00002f));

    s_overrideActive = true;
}

extern "C" void __cdecl NormalMaps_PostCarrierDraw(void)
{
    if (!s_overrideActive) return;
    s_overrideActive = false;

    IDirect3DDevice9* dev = Rendering::GetDevice9();
    if (!dev) return;

    dev->SetRenderState(D3DRS_ZFUNC,            s_saveZFunc);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     s_saveZWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s_saveABE);
    dev->SetRenderState(D3DRS_SRCBLEND,         s_saveSrcB);
    dev->SetRenderState(D3DRS_DESTBLEND,        s_saveDstB);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, s_saveSlopeBias);
    dev->SetRenderState(D3DRS_DEPTHBIAS,           s_saveDepthBias);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  s_saveAddrU);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  s_saveAddrV);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, s_saveMinF);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, s_saveMagF);
    dev->SetSamplerState(1, D3DSAMP_MIPFILTER, s_saveMipF);
    dev->SetVertexShader(s_saveVS);
    dev->SetPixelShader(s_savePS);
    dev->SetTexture(1, s_saveTex1);

    if (s_saveVS)   { s_saveVS->Release();   s_saveVS = nullptr; }
    if (s_savePS)   { s_savePS->Release();   s_savePS = nullptr; }
    if (s_saveTex1) { s_saveTex1->Release(); s_saveTex1 = nullptr; }
}

// =====================================================================
//  Stage 1 scan + Stage 3 bridge-table build
// =====================================================================
static void RunScan(bool verbose)
{
    if (verbose) Logger::log("NormalMaps: ===== level material scan =====");
    ScanWorker(&s_scan);

    if (!s_scan.ok)
    {
        if (verbose)
            Logger::log("NormalMaps: scan aborted - object table not ready");
        return;
    }

    if (verbose)
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "NormalMaps: %d Shader(s); Detail at offset 0x%X; %d carrier(s):",
            s_scan.shaderCount, s_scan.detailOffset, s_scan.carrierCount);
        Logger::log(buf);
        for (int k = 0; k < s_scan.carrierCount; ++k)
        {
            snprintf(buf, sizeof(buf),
                "NormalMaps:   Shader '%s'  ->  Detail %s '%s'",
                s_scan.shaderName[k], s_scan.detailClass[k], s_scan.detailName[k]);
            Logger::log(buf);
        }

        snprintf(buf, sizeof(buf),
            "NormalMaps: %d engine light(s) collected (LightType offset 0x%X)",
            s_lightCount, s_offLightType);
        Logger::log(buf);
        for (int k = 0; k < s_lightCount && k < 12; ++k)
        {
            snprintf(buf, sizeof(buf),
                "NormalMaps:   light %2d  pos(%.0f, %.0f, %.0f)  "
                "rgb(%.2f, %.2f, %.2f)  LightRadius %.1f -> %.0f  bright %.1f",
                k, s_lights[k].pos[0], s_lights[k].pos[1], s_lights[k].pos[2],
                s_lights[k].color[0], s_lights[k].color[1], s_lights[k].color[2],
                s_lights[k].rawRadius, s_lights[k].radius, s_lights[k].rawBright);
            Logger::log(buf);
        }
    }

    // Rebuild the bridge table, carrying over links already discovered.
    CarrierBridge old[16];
    int oldCount = s_bridgeCount;
    for (int i = 0; i < oldCount && i < 16; ++i) old[i] = s_bridge[i];

    s_bridgeCount = 0;
    for (int k = 0; k < s_scan.carrierCount && s_bridgeCount < 16; ++k)
    {
        if (!s_scan.detailObj[k]) continue;
        CarrierBridge& b = s_bridge[s_bridgeCount++];
        strncpy_s(b.name, sizeof(b.name), s_scan.detailName[k], _TRUNCATE);
        b.utexture   = s_scan.detailObj[k];
        b.d3dtex     = nullptr;
        b.textureItf = nullptr;
        for (int o = 0; o < oldCount; ++o)
            if (old[o].utexture == b.utexture)
            {
                b.d3dtex     = old[o].d3dtex;
                b.textureItf = old[o].textureItf;
                break;
            }
    }
}

// =====================================================================
//  Stage 2 - the tangent-space normal-mapping shader
// =====================================================================
//  VS inputs : POSITION, TEXCOORD0 only (universal - no normal/tangent).
//  The PS reconstructs the surface normal and the tangent frame per-pixel
//  from screen-space derivatives, so nothing depends on the engine's
//  vertex format.

static const char* kNormalMapVS = R"HLSL(
float4x4 WorldViewProj : register(c0);
float4x4 World         : register(c4);

struct VIn  { float3 Pos : POSITION; float2 UV : TEXCOORD0; };
struct VOut { float4 Pos : POSITION; float2 UV : TEXCOORD0; float3 WPos : TEXCOORD1; };

VOut main(VIn i)
{
    VOut o;
    // No depth bias here: a clip-space bias is uniform in screen space,
    // but the engine-vs-shader depth mismatch grows as ~1/w near a
    // surface. The additive pass instead uses hardware slope-scaled
    // depth bias (set in NormalMaps_PreCarrierDraw), which adapts.
    o.Pos  = mul(float4(i.Pos, 1.0), WorldViewProj);
    o.UV   = i.UV;
    o.WPos = mul(float4(i.Pos, 1.0), World).xyz;
    return o;
}
)HLSL";

static const char* kNormalMapPS = R"HLSL(
sampler2D NormalTex   : register(s1);

float3 EyePos          : register(c0);   // camera world position
float4 Params          : register(c1);   // x bumpScale  y fadeStart  z reliefScale  w fadeEnd
float4 LightPosRad[16] : register(c2);   // xyz world pos, w falloff radius

struct PIn { float2 UV : TEXCOORD0; float3 WPos : TEXCOORD1; };

float4 main(PIn p) : COLOR
{
    // view direction + distance to the camera (used by the fade below)
    float3 toEye   = EyePos - p.WPos;
    float  camDist = length(toEye);
    float3 V       = toEye / max(camDist, 0.001);

    // Screen-space derivatives of world position and UV.
    float3 dp1  = ddx(p.WPos);
    float3 dp2  = ddy(p.WPos);
    float2 duv1 = ddx(p.UV);
    float2 duv2 = ddy(p.UV);

    // Geometric normal, faced toward the camera.
    float3 N = normalize(cross(dp1, dp2));
    if (dot(N, V) < 0.0) N = -N;

    // Cotangent frame (Schuler): the true tangent basis for this UV
    // mapping. Unlike a naive tangent estimate it does not skew with the
    // triangle shape or the viewing angle, so the bump strength stays
    // consistent as the camera turns.
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float  invmax = rsqrt(max(dot(T, T), dot(B, B)));
    T *= invmax;
    B *= invmax;

    // Tangent-space normal - a plain trilinear sample; the sampler's mip
    // chain handles minification and the distance fade below controls
    // how the effect bows out. Z is rebuilt from XY so a mipped/flat
    // texel can never collapse the vector and NaN out.
    float2 nXY = (tex2D(NormalTex, p.UV).xy * 2.0 - 1.0) * Params.x;
    float  nZ  = sqrt(saturate(1.0 - dot(nXY, nXY)));
    float3 Nw  = normalize(nXY.x * T + nXY.y * B + nZ * N);

    // Scalar bump RELIEF: per light, how much the bumped normal changes
    // the diffuse shading versus the flat surface. We use only each
    // light's DIRECTION and reach, never its brightness or colour, so
    // the relief reads equally in dim and bright areas - the engine's
    // own render still supplies the real brightness and colour. Where
    // the map is flat (or has mipped flat at distance) every delta is
    // 0, so the relief is 0 and the surface is left exactly as drawn.
    float relief = 0.0;
    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float3 Lv  = LightPosRad[i].xyz - p.WPos;
        float  d   = length(Lv);
        float3 L   = Lv / max(d, 0.001);
        float  rad = max(LightPosRad[i].w, 1.0);

        // smooth radial falloff: 1 at the light, 0 past its radius
        float atten = saturate(1.0 - d / rad);
        atten = atten * atten;

        relief += (saturate(dot(Nw, L)) - saturate(dot(N, L))) * atten;
    }
    relief = clamp(relief * Params.z, -1.0, 1.0);
    relief = (relief == relief) ? relief : 0.0;   // scrub NaN -> neutral

    // One controlled distance behaviour: smoothly fade the whole effect
    // out between fadeStart and fadeEnd. The mip chain no longer drives
    // how the effect disappears, so it can't collapse through a stack of
    // harsh mip bands - it just eases off. Full strength within
    // fadeStart, completely gone past fadeEnd.
    relief *= 1.0 - smoothstep(Params.y, Params.w, camDist);

    // Output for a modulate-2x blend: result = 2 * src * dst. src = 0.5
    // leaves the engine's pixel untouched; >0.5 brightens the bump and
    // <0.5 darkens it, so bumps gain true light-and-shade relief on top
    // of whatever lighting the engine already produced.
    float s = saturate(0.5 + 0.5 * relief);
    return float4(s, s, s, 1.0);
}
)HLSL";

static bool CompileShader(pD3DCompile compileFn, const char* src,
                          const char* profile, ID3DBlob** outBlob)
{
    *outBlob = nullptr;
    ID3DBlob* errors = nullptr;
    // D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (0x8): match D3D's row-major matrices.
    HRESULT hr = compileFn(src, strlen(src), nullptr, nullptr, nullptr,
                           "main", profile, 0x8, 0, outBlob, &errors);
    if (FAILED(hr) || !*outBlob)
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "NormalMaps: %s compile FAILED (hr=0x%08X): %s",
            profile, static_cast<unsigned>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer())
                   : "(no compiler message)");
        Logger::log(msg);
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

static void EnsureShaders()
{
    if (s_shaderState != 0) return;

    IDirect3DDevice9* dev = Rendering::GetDevice9();
    if (!dev)
    {
        Logger::log("NormalMaps: shader build deferred - D3D9 device not ready");
        return;
    }

    HMODULE lib = LoadLibraryA("D3DCompiler_47.dll");
    if (!lib)
    {
        Logger::log("NormalMaps: D3DCompiler_47.dll could not be loaded");
        s_shaderState = -1;
        return;
    }
    pD3DCompile compileFn =
        reinterpret_cast<pD3DCompile>(GetProcAddress(lib, "D3DCompile"));
    if (!compileFn)
    {
        Logger::log("NormalMaps: D3DCompile entry point not found");
        s_shaderState = -1;
        return;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    bool compiled =
        CompileShader(compileFn, kNormalMapVS, "vs_3_0", &vsBlob) &&
        CompileShader(compileFn, kNormalMapPS, "ps_3_0", &psBlob);

    if (compiled)
    {
        HRESULT hrV = dev->CreateVertexShader(
            static_cast<const DWORD*>(vsBlob->GetBufferPointer()), &s_vs);
        HRESULT hrP = dev->CreatePixelShader(
            static_cast<const DWORD*>(psBlob->GetBufferPointer()), &s_ps);

        if (SUCCEEDED(hrV) && SUCCEEDED(hrP) && s_vs && s_ps)
        {
            Logger::log("NormalMaps: shaders OK - compiled & created");
            s_shaderState = 1;
        }
        else
        {
            char msg[200];
            snprintf(msg, sizeof(msg),
                "NormalMaps: shader creation FAILED (VS hr=0x%08X, PS hr=0x%08X)",
                static_cast<unsigned>(hrV), static_cast<unsigned>(hrP));
            Logger::log(msg);
            if (s_vs) { s_vs->Release(); s_vs = nullptr; }
            if (s_ps) { s_ps->Release(); s_ps = nullptr; }
            s_shaderState = -1;
        }
    }
    else
    {
        s_shaderState = -1;
    }

    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();
}

// =====================================================================
//  Stage 4 - neutralising the engine's stock detail-texture overlay
// =====================================================================
//  We hijacked the Material "Detail" slot to carry a normal map, but the
//  engine still applies whatever sits in that slot as a detail-texture
//  overlay (a modulate-2x blend). UE2 detail textures are authored
//  mid-gray = neutral precisely so that blend leaves the surface
//  unchanged. So: whenever the engine binds a carrier normal map, we
//  substitute this flat mid-gray texture - the engine's detail overlay
//  then multiplies the surface by ~1.0 and vanishes. Our own additive
//  pass samples the real normal map directly and is unaffected.
static void EnsureGrayTex()
{
    if (s_grayTex) return;

    IDirect3DDevice9* dev = Rendering::GetDevice9();
    if (!dev) return;

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
    {
        Logger::log("NormalMaps: neutral detail texture creation FAILED");
        return;
    }

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)))
    {
        for (int y = 0; y < 4; ++y)
        {
            DWORD* row = reinterpret_cast<DWORD*>(
                static_cast<char*>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < 4; ++x)
                row[x] = 0xFF808080;   // opaque mid-gray (modulate-2x neutral)
        }
        tex->UnlockRect(0);
    }

    s_grayTex = tex;
    Logger::log("NormalMaps: neutral detail texture created");
}

// Called from d3d8to9's SetTexture. If 'texture8' is one of our hijacked
// Detail-slot carrier normal maps - and we are rendering the 3D world,
// not a 2D texture-browser thumbnail - returns the neutral gray texture
// to bind in its place. Returns null otherwise (bind normally).
extern "C" void* __cdecl NormalMaps_QueryCarrierGray(void* texture8)
{
    if (!g_ReloadedNormalMaps || !texture8 || s_bridgeCount == 0)
        return nullptr;

    bool isCarrier = false;
    for (int i = 0; i < s_bridgeCount; ++i)
        if (s_bridge[i].d3dtex && s_bridge[i].d3dtex == texture8)
        {
            isCarrier = true;
            break;
        }
    if (!isCarrier) return nullptr;

    IDirect3DDevice9* dev = Rendering::GetDevice9();
    if (!dev) return nullptr;

    // Only neutralise the detail overlay for real 3D world rendering.
    // Texture-browser thumbnails and other 2D/ortho draws use an
    // orthographic projection (_44 ~= 1); leave those alone so the
    // normal-map asset still previews correctly in the browser.
    D3DMATRIX proj;
    dev->GetTransform(D3DTS_PROJECTION, &proj);
    if (proj._44 > 0.5f || proj._44 < -0.5f) return nullptr;

    EnsureGrayTex();
    return s_grayTex;
}

// Called (via Rendering::OnCreateDevice) just before the editor releases
// its D3D device. The editor check()s that the device refcount reaches
// zero; our compiled shaders hold a reference, so they must be released
// here or the assertion (int 3 at 0x10F10B94) fires.
extern "C" void __cdecl NormalMaps_OnDeviceLost(void)
{
    if (s_vs) { s_vs->Release(); s_vs = nullptr; }
    if (s_ps) { s_ps->Release(); s_ps = nullptr; }
    if (s_grayTex) { s_grayTex->Release(); s_grayTex = nullptr; }
    s_saveVS = nullptr;
    s_savePS = nullptr;
    s_saveTex1 = nullptr;
    s_overrideActive = false;
    s_shaderState = 0;          // EnsureShaders() rebuilds on the new device
}

// =====================================================================
//  F11 - rescan + status (diagnostic)
// =====================================================================
extern "C" void __cdecl NormalMaps_HotkeyScan()
{
    if (!g_ReloadedNormalMaps)
    {
        Logger::log("NormalMaps: F11 ignored - feature is off");
        return;
    }

    RunScan(true);
    EnsureShaders();

    char buf[200];
    snprintf(buf, sizeof(buf),
        "NormalMaps: draws since last F11: %u  (carrier draws: %u)",
        s_drawCount, s_carrierDrawCount);
    Logger::log(buf);
    s_drawCount = 0;
    s_carrierDrawCount = 0;

    int bridged = 0;
    for (int i = 0; i < s_bridgeCount; ++i)
        if (s_bridge[i].d3dtex) ++bridged;
    snprintf(buf, sizeof(buf),
        "NormalMaps: bridge %d/%d linked   shaders %s",
        bridged, s_bridgeCount,
        s_shaderState == 1 ? "ready" : (s_shaderState < 0 ? "FAILED" : "pending"));
    Logger::log(buf);
    Logger::log("NormalMaps: ===== F11 complete =====");
}

// =====================================================================
//  F9 - A/B compare toggle (normal-map render pass on/off)
// =====================================================================
extern "C" void __cdecl NormalMaps_HotkeyToggleRender()
{
    if (!g_ReloadedNormalMaps)
    {
        Logger::log("NormalMaps: F9 ignored - feature is off");
        return;
    }
    s_renderEnabled = !s_renderEnabled;
    Logger::log(s_renderEnabled
        ? "NormalMaps: F9 - render pass ON (normal mapping)"
        : "NormalMaps: F9 - render pass OFF (plain rendering)");
}

// ---------------------------------------------------------------------
void NormalMaps::Initialize()
{
    if (!g_ReloadedNormalMaps)
    {
        Logger::log("NormalMaps: disabled");
        return;
    }

    INSTALL_HOOKS;   // CacheTexture tap (toggle-gated)

    Logger::log("NormalMaps: Stage 3 - normal-map render override active; "
                "open a map and look at the carrier surfaces");
}
