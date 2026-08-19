#include "pch.h"
#include "Shadows.h"
#include "Hooks.h"
#include "logger.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>
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

/*  UModel::CompressLightmaps (0x1119EDF0) blits every lightmap belonging to one
    FLightMapTexture into a single scratch atlas before handing it to the filter,
    so filtering the atlas as one image bleeds neighbouring lightmaps together. */

// UModel - ECX on entry to the compress loop, spilled to [EBP-0x20]
static constexpr uint32_t MODEL_LIGHTMAPS      = 0xEC;
static constexpr uint32_t MODEL_LIGHTMAP_COUNT = 0xF0;

// FLightMapTexture - one atlas
static constexpr uint32_t LMTEX_INDICES     = 0x08; // int32[] into UModel::LightMaps
static constexpr uint32_t LMTEX_INDEX_COUNT = 0x0C;

// FLightMap - offsets assigned by the atlas packer at 0x1119EBB0
static constexpr uint32_t LIGHTMAP_SIZEOF   = 0xA4;
static constexpr uint32_t LIGHTMAP_OFFSET_X = 0x14;
static constexpr uint32_t LIGHTMAP_OFFSET_Y = 0x18;
static constexpr uint32_t LIGHTMAP_SIZE_X   = 0x1C;
static constexpr uint32_t LIGHTMAP_SIZE_Y   = 0x20;

template<typename T>
static T ReadField(const void* base, uint32_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}

static std::vector<LightmapRect> CollectLightmapRects(const void* model, const void* texture) {
    std::vector<LightmapRect> rects;

    if (model == nullptr || texture == nullptr)
        return rects;

    const auto* lightmaps = ReadField<const uint8_t*>(model, MODEL_LIGHTMAPS);
    const int32_t lightmapCount = ReadField<int32_t>(model, MODEL_LIGHTMAP_COUNT);
    const auto* indices = ReadField<const int32_t*>(texture, LMTEX_INDICES);
    const int32_t indexCount = ReadField<int32_t>(texture, LMTEX_INDEX_COUNT);

    if (lightmaps == nullptr || indices == nullptr || lightmapCount <= 0 || indexCount <= 0)
        return rects;

    const int atlasRes = static_cast<int>(LIGHTMAP_TEXTURE_RES);
    rects.reserve(indexCount);

    for (int32_t i = 0; i < indexCount; ++i) {
        const int32_t index = indices[i];
        if (index < 0 || index >= lightmapCount)
            continue;

        const uint8_t* lightmap = lightmaps + (index * LIGHTMAP_SIZEOF);
        LightmapRect rect{
            ReadField<int32_t>(lightmap, LIGHTMAP_OFFSET_X),
            ReadField<int32_t>(lightmap, LIGHTMAP_OFFSET_Y),
            ReadField<int32_t>(lightmap, LIGHTMAP_SIZE_X),
            ReadField<int32_t>(lightmap, LIGHTMAP_SIZE_Y),
        };

        // A rect reaching past the atlas would walk the filter off the end of the buffer.
        if (rect.w <= 0 || rect.h <= 0)
            continue;
        if (rect.x < 0 || rect.y < 0 || rect.x >= atlasRes || rect.y >= atlasRes)
            continue;

        rect.w = min(rect.w, atlasRes - rect.x);
        rect.h = min(rect.h, atlasRes - rect.y);
        rects.push_back(rect);
    }

    return rects;
}

#ifdef LIGHTMAP_BUILD_STATS
static int statFilterAtlases = 0;
static long long statFilterRects = 0;
static long long statFilterTicks = 0;
static int statFilterFallbacks = 0;
#endif

static void FilterLightmapTexture(void* sourceBuffer, void* targetBuffer, const void* model, const void* texture) {
    std::vector<LightmapRect> rects;

    try {
        rects = CollectLightmapRects(model, texture);
    }
    catch (...) {
        // No rects means the whole atlas is filtered as one image, which still works.
    }

#ifdef LIGHTMAP_BUILD_STATS
    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
#endif

    // The filter allocates, and this runs under a naked hook that cannot unwind.
    try {
        ShadowMapFilter::ProcessLightmapAtlas(sourceBuffer, targetBuffer, rects);
    }
    catch (...) {
    }

#ifdef LIGHTMAP_BUILD_STATS
    QueryPerformanceCounter(&after);

    statFilterAtlases++;
    statFilterRects += static_cast<long long>(rects.size());
    if (rects.empty()) statFilterFallbacks++;
    statFilterTicks += after.QuadPart - before.QuadPart;
#endif
}

JMP_HOOK(0x1119EF79, DisableDownsample) {
    static int Return = 0x1119F168;
    static void* sourceBuffer;
    static void* targetBuffer;
    static void* model;
    static void* texture;

    __asm {
        mov ecx, [ebp - 0x1C]
        mov[sourceBuffer], esi
        mov[targetBuffer], ecx
        mov ecx, [ebp - 0x20]
        mov[model], ecx
        mov[texture], edi
        mov     ecx, [edi + 0x2C]
        lea     ebx, [edi + 0x24]
        pushad
    }
    FilterLightmapTexture(sourceBuffer, targetBuffer, model, texture);
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

#ifdef LIGHTMAP_BUILD_STATS

// CompressLightmaps loops every atlas internally, so one call site covers the whole run.
using CompressFn = void(__fastcall*)(void* self);
static const CompressFn OriginalCompress = reinterpret_cast<CompressFn>(0x1119EDF0);

static void TimedCompressLightmaps(void* self) {
    statFilterAtlases = 0;
    statFilterRects = 0;
    statFilterTicks = 0;
    statFilterFallbacks = 0;

    LARGE_INTEGER before, after, freq;
    QueryPerformanceCounter(&before);
    OriginalCompress(self);
    QueryPerformanceCounter(&after);
    QueryPerformanceFrequency(&freq);

    const double scale = freq.QuadPart > 0 ? 1.0 / freq.QuadPart : 0.0;
    const double filter = statFilterTicks * scale;
    const double total = (after.QuadPart - before.QuadPart) * scale;

    std::ostringstream line;
    line << "Lightmap compress: " << statFilterAtlases << " atlases, "
         << statFilterRects << " rects (" << statFilterFallbacks << " whole-atlas), "
         << std::fixed << std::setprecision(3) << filter << "s filtering, "
         << total << "s total";
    Logger::log(line.str());
}

CALL_HOOK(0x11081289, CompressLightmapsTimer) {
    static void* self;
    __asm {
        mov [self], ecx
        pushad
    }
    TimedCompressLightmaps(self);
    __asm {
        popad
        ret
    }
}

#endif
#endif

void Shadows::Initialize()
{
    INSTALL_HOOKS;
}
