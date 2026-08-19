#include "pch.h"
#include "LightmapPacker.h"
#include "Hooks.h"
#include "logger.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <vector>

INIT_HOOKS;

#if defined(LIGHTMAP_MULTI_BIN_PACKING) || defined(LIGHTMAP_BUILD_STATS)

static constexpr uint32_t MODEL_TEXTURE_COUNT = 0xE4;

template<typename T>
static T ReadField(const void* base, uint32_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}

#endif

#ifdef LIGHTMAP_MULTI_BIN_PACKING

/*  LightMapLayout::AddSurface (0x111A0410) packs into one shared skyline at 0x119A57C4
    and, on the first lightmap that will not fit, calls Flush (0x111A02F0), which clears
    it and appends a new FLightMapTexture. The closed atlas is unreachable from then on,
    so a large lightmap arriving mid-run strands the space left in the one before it. */

// Engine skyline allocator.
static constexpr uint32_t ALLOC_WIDTH  = 0x00;
static constexpr uint32_t ALLOC_HEIGHT = 0x04;

// The object Flush receives; its model owns the texture array.
static constexpr uint32_t FLUSH_MODEL = 0x13C;

using PackFn = int(__thiscall*)(void* self, int* outX, int* outY, int sizeX, int sizeY);
static const PackFn OriginalPack = reinterpret_cast<PackFn>(0x1119EBB0);

struct AtlasBin
{
    int textureIndex = -1;
    int width = 0;
    int height = 0;
    int minRow = 0;             // shortest row, so a hopeless bin is rejected unscanned
    std::vector<int> rows;      // filled width of each row
};

static std::vector<AtlasBin> bins;
static bool packerActive = false;

// -1 leaves AddSurface on its stock NumTextures - 1.
static int chosenTexture = -1;

// The engine's own rule: least wasted area beneath the block, lowest row breaking ties.
static bool PlaceInBin(AtlasBin& bin, int sizeX, int sizeY, int& outX, int& outY) {
    int bestX = -1;
    int bestY = -1;
    int bestWaste = INT_MAX;

    for (int y = 0; y + sizeY <= bin.height; ++y) {
        int x = 0;
        int rowSum = 0;

        for (int i = 0; i < sizeY; ++i) {
            const int row = bin.rows[y + i];
            if (row > x) x = row;
            rowSum += row;
        }

        if (x + sizeX > bin.width) continue;

        const int waste = (x * sizeY) - rowSum;
        if (waste >= bestWaste) continue;

        bestWaste = waste;
        bestX = x;
        bestY = y;

        if (waste == 0) break;
    }

    if (bestX < 0) return false;

    for (int i = 0; i < sizeY; ++i) {
        bin.rows[bestY + i] = bestX + sizeX;
    }

    bin.minRow = *std::min_element(bin.rows.begin(), bin.rows.end());
    outX = bestX;
    outY = bestY;
    return true;
}

static int PlaceLightmap(void* allocator, int* outX, int* outY, int sizeX, int sizeY) {
    const int width = ReadField<int>(allocator, ALLOC_WIDTH);
    const int height = ReadField<int>(allocator, ALLOC_HEIGHT);

    if (!packerActive || bins.empty() || sizeX <= 0 || sizeY <= 0 || width <= 0 || height <= 0) {
        chosenTexture = -1;
        return OriginalPack(allocator, outX, outY, sizeX, sizeY);
    }

    try {
        for (AtlasBin& bin : bins) {
            if (bin.rows.empty()) {
                bin.rows.assign(height, 0);
                bin.width = width;
                bin.height = height;
                bin.minRow = 0;
            }

            if (bin.width != width || bin.height != height) continue;
            if (width - bin.minRow < sizeX || height < sizeY) continue;

            if (PlaceInBin(bin, sizeX, sizeY, *outX, *outY)) {
                chosenTexture = bin.textureIndex;
                return 1;
            }
        }
    }
    catch (...) {
        // Handing this to the engine would place into a skyline nothing has written to
        // and overlap a packed lightmap. Give up so the retry takes the stock path.
        packerActive = false;
        bins.clear();
        chosenTexture = -1;
        return 0;
    }

    // No bin had room, so the caller opens a texture and asks again.
    chosenTexture = -1;
    return 0;
}

// Flush is about to append a texture; mirror it with a bin carrying the same index.
static void OnNewTexture(void* flushSelf, int startingRun) {
    try {
        const void* model = ReadField<const void*>(flushSelf, FLUSH_MODEL);

        if (startingRun != 0) {
            bins.clear();
            packerActive = true;
        }

        AtlasBin bin;
        bin.textureIndex = ReadField<int>(model, MODEL_TEXTURE_COUNT);
        bins.push_back(bin);
    }
    catch (...) {
        // An empty list drops every placement back onto the engine's single atlas.
        bins.clear();
        packerActive = false;
    }
}

/*  Both placement attempts in AddSurface. The replaced call is __thiscall with callee
    cleanup: ECX is the allocator and the four arguments sit above the return address. */
CALL_HOOK(0x111A06FE, PackLightmapAttempt) {
    static void* allocator;
    static int* outX;
    static int* outY;
    static int sizeX;
    static int sizeY;
    static int result;

    __asm {
        mov [allocator], ecx
        mov eax, [esp + 0x04]
        mov [outX], eax
        mov eax, [esp + 0x08]
        mov [outY], eax
        mov eax, [esp + 0x0C]
        mov [sizeX], eax
        mov eax, [esp + 0x10]
        mov [sizeY], eax
        pushad
    }
    result = PlaceLightmap(allocator, outX, outY, sizeX, sizeY);
    __asm {
        popad
        mov eax, [result]
        ret 0x10
    }
}

CALL_HOOK(0x111A0721, PackLightmapRetry) {
    __asm {
        jmp PackLightmapAttempt
    }
}

// Stock writes NumTextures - 1; write the bin the lightmap actually landed in.
JMP_HOOK(0x111A0758, LightmapTextureIndex) {
    static int Return = 0x111A075F;
    __asm {
        mov eax, [chosenTexture]
        test eax, eax
        jge useChosen
        mov eax, [edi + 0xE4]
        dec eax
    useChosen:
        jmp dword ptr[Return]
    }
}

// Illuminate opens the first texture here: the start of a lighting build.
CALL_HOOK(0x111A0C50, BeginLightmapRun) {
    static int Original = 0x111A02F0;
    static void* flushSelf;

    __asm {
        mov [flushSelf], ecx
        pushad
    }
    OnNewTexture(flushSelf, 1);
    __asm {
        popad
        jmp dword ptr[Original]
    }
}

CALL_HOOK(0x111A070A, RolloverLightmapTexture) {
    static int Original = 0x111A02F0;
    static void* flushSelf;

    __asm {
        mov [flushSelf], ecx
        pushad
    }
    OnNewTexture(flushSelf, 0);
    __asm {
        popad
        jmp dword ptr[Original]
    }
}

#endif

#ifdef LIGHTMAP_BUILD_STATS

/*  FLightBitmapLayout::Flush (0x1119FAF0) reads a whole page back in one ReadPixels, so
    page count is readback count and fill says whether a larger page would hold more. */

// FLightBitmapLayout.
static constexpr uint32_t LBL_MODEL       = 0x00;
static constexpr uint32_t LBL_LOCKED      = 0x08;   // cleared once the page is read back
static constexpr uint32_t LBL_ENTRIES     = 0x0C;   // (lightmap, bitmap) index pairs
static constexpr uint32_t LBL_ENTRY_COUNT = 0x10;

static constexpr uint32_t MODEL_LIGHTMAPS  = 0xEC;
static constexpr uint32_t LIGHTMAP_STRIDE  = 0xA4;
static constexpr uint32_t LIGHTMAP_BITMAPS = 0x8C;
static constexpr uint32_t BITMAP_STRIDE    = 0x34;
static constexpr uint32_t BITMAP_SIZE_X    = 0x10;
static constexpr uint32_t BITMAP_SIZE_Y    = 0x14;

// Mirrors the readback viewport Illuminate sets up.
static constexpr long long PAGE_EDGE = 0x200;

using FlushFn = void(__fastcall*)(void* self);
static const FlushFn OriginalFlush = reinterpret_cast<FlushFn>(0x1119FAF0);

// LightMapLayout::AddSurface - __cdecl, the caller pops its three arguments.
using AddSurfaceFn = int(__cdecl*)(int a, int b, int c);
static const AddSurfaceFn OriginalAddSurface = reinterpret_cast<AddSurfaceFn>(0x111A0410);

static int statPages = 0;
static long long statBitmaps = 0;
static long long statTexels = 0;
static long long statTicks = 0;
static int statSurfaces = 0;
static long long statLayoutTicks = 0;

static int TimedAddSurface(int a, int b, int c) {
    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
    const int result = OriginalAddSurface(a, b, c);
    QueryPerformanceCounter(&after);

    statSurfaces++;
    statLayoutTicks += after.QuadPart - before.QuadPart;
    return result;
}

// Texels the packed bitmaps occupy in the page about to be read back.
static long long MeasurePageFill(const void* self, int& bitmaps) {
    long long texels = 0;
    bitmaps = 0;

    try {
        const auto* entries = ReadField<const uint8_t*>(self, LBL_ENTRIES);
        const void* model = ReadField<const void*>(self, LBL_MODEL);
        if (entries == nullptr || model == nullptr) return 0;

        const auto* lightmaps = ReadField<const uint8_t*>(model, MODEL_LIGHTMAPS);
        if (lightmaps == nullptr) return 0;

        bitmaps = ReadField<int>(self, LBL_ENTRY_COUNT);

        for (int i = 0; i < bitmaps; ++i) {
            const uint8_t* lightmap = lightmaps + (ReadField<int>(entries, i * 8) * LIGHTMAP_STRIDE);
            const auto* list = ReadField<const uint8_t*>(lightmap, LIGHTMAP_BITMAPS);
            if (list == nullptr) continue;

            const uint8_t* bitmap = list + (ReadField<int>(entries, (i * 8) + 4) * BITMAP_STRIDE);
            texels += static_cast<long long>(ReadField<int>(bitmap, BITMAP_SIZE_X))
                * ReadField<int>(bitmap, BITMAP_SIZE_Y);
        }
    }
    catch (...) {
        return 0;
    }

    return texels;
}

static void TimedFlush(void* self) {
    // A layout with nothing rendered into it skips the readback entirely.
    const bool readsBack = ReadField<int>(self, LBL_LOCKED) != 0;

    int bitmaps = 0;
    const long long texels = readsBack ? MeasurePageFill(self, bitmaps) : 0;

    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
    OriginalFlush(self);
    QueryPerformanceCounter(&after);

    if (!readsBack) return;

    statPages++;
    statBitmaps += bitmaps;
    statTexels += texels;
    statTicks += after.QuadPart - before.QuadPart;
}

static void LogBuildSummary(const void* self) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    const long long capacity = statPages * PAGE_EDGE * PAGE_EDGE;
    const double fill = capacity > 0 ? (100.0 * statTexels) / capacity : 0.0;
    const double scale = freq.QuadPart > 0 ? 1.0 / freq.QuadPart : 0.0;

    int atlases = 0;
    try {
        const void* model = ReadField<const void*>(self, LBL_MODEL);
        if (model != nullptr) atlases = ReadField<int>(model, MODEL_TEXTURE_COUNT);
    }
    catch (...) {
    }

    std::ostringstream line;
    line << "Lightmap build: " << atlases << " atlases, "
         << statPages << " shadow pages, " << statBitmaps << " bitmaps, "
         << std::fixed << std::setprecision(1) << fill << "% page fill, "
         << std::setprecision(3) << (statTicks * scale) << "s in readback, "
         << statSurfaces << " surfaces laid out in " << (statLayoutTicks * scale) << "s";
    Logger::log(line.str());

    statPages = 0;
    statBitmaps = 0;
    statTexels = 0;
    statTicks = 0;
    statSurfaces = 0;
    statLayoutTicks = 0;
}

CALL_HOOK(0x1119FEB5, FlushShadowPageA) {
    static void* self;
    __asm {
        mov [self], ecx
        pushad
    }
    TimedFlush(self);
    __asm {
        popad
        ret
    }
}

CALL_HOOK(0x1119FFA9, FlushShadowPageB) {
    __asm {
        jmp FlushShadowPageA
    }
}

CALL_HOOK(0x111A0CC1, AddSurfaceTimer) {
    static int argA;
    static int argB;
    static int argC;
    static int result;

    __asm {
        mov eax, [esp + 0x04]
        mov [argA], eax
        mov eax, [esp + 0x08]
        mov [argB], eax
        mov eax, [esp + 0x0C]
        mov [argC], eax
        pushad
    }
    result = TimedAddSurface(argA, argB, argC);
    __asm {
        popad
        mov eax, [result]
        ret
    }
}

// Illuminate's closing flush, so the totals are complete when this one returns.
CALL_HOOK(0x111A1144, FlushShadowPageFinal) {
    static void* self;
    __asm {
        mov [self], ecx
        pushad
    }
    TimedFlush(self);
    LogBuildSummary(self);
    __asm {
        popad
        ret
    }
}

#endif

void LightmapPacker::Initialize()
{
    INSTALL_HOOKS;
}
