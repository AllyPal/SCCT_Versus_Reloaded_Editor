#include "pch.h"
#include "SizingBoxFix.h"
#include "MemoryWriter.h"
#include "logger.h"

#include <cstring>
#include <string>

// Cleans up the UseSizingBox=True selection overlay

struct BytePatch
{
    uintptr_t   address;
    const char* expected;
    const char* replacement;
    size_t      length;         // Bytes to compare/write, including terminator for strings
    const char* description;
};

static const BytePatch kLabelPatches[] =
{
    { 0x114770AC, "Depth  : %d",            "Breadth: %d",            12, "Depth -> Breadth"   },
    { 0x114770BC, "Height : %d",            "Height: %d",             11, "Height label"       },
    { 0x114770CC, "Width  : %d",            "Width: %d",              10, "Width label"        },
    { 0x11477094, "Tag : %s",               "Tag: %s",                 8, "Tag label"          },
    { 0x11477028, "Platform : COMMON",      "Platform: COMMON",       17, "Platform COMMON"    },
    { 0x11477040, "Platform : PSX2 ONLY",   "Platform: PSX2 ONLY",    20, "Platform PSX2"      },
    { 0x1147705C, "Platform : XBOX ONLY",   "Platform: XBOX ONLY",    20, "Platform XBOX"      },
    { 0x11477078, "Platform : ? unknown ?", "Platform: ? unknown ?",  22, "Platform unknown"   },
};

static const uintptr_t kWidthDestDisp  = 0x10ECD036;
static const uintptr_t kHeightDestDisp = 0x10ECD089;
static const uint8_t   kWidthSlot  = 0x90;
static const uint8_t   kHeightSlot = 0xB8;

static bool ApplyLabel(const BytePatch& p)
{
    const char* live = reinterpret_cast<const char*>(p.address);

    if (std::memcmp(live, p.replacement, std::strlen(p.replacement) + 1) == 0)
        return true; // Already patched

    if (std::memcmp(live, p.expected, std::strlen(p.expected) + 1) != 0)
    {
        Logger::log(std::string("SizingBoxFix: unexpected string at ") + p.description + ", skipped");
        return false;
    }

    // Replacement is never longer than the string it replaces, so writing it
    // with its terminator stays inside the original allocation.
    return MemoryWriter::WriteBytes(p.address, p.replacement, std::strlen(p.replacement) + 1);
}

static bool SwapWidthAndHeightSlots()
{
    const uint8_t widthNow  = *reinterpret_cast<volatile uint8_t*>(kWidthDestDisp);
    const uint8_t heightNow = *reinterpret_cast<volatile uint8_t*>(kHeightDestDisp);

    if (widthNow == kHeightSlot && heightNow == kWidthSlot)
        return true; // Already patched

    if (widthNow != kWidthSlot || heightNow != kHeightSlot)
    {
        Logger::log("SizingBoxFix: unexpected operands at the Width/Height assignments, order left alone");
        return false;
    }

    const bool ok = MemoryWriter::WriteBytes(kWidthDestDisp,  &kHeightSlot, sizeof(kHeightSlot))
                 && MemoryWriter::WriteBytes(kHeightDestDisp, &kWidthSlot,  sizeof(kWidthSlot));

    if (!ok)
        Logger::log("SizingBoxFix: failed to swap the Width/Height assignments");

    return ok;
}

// Fixes a builder brush assert on empty levels.
// Skips the zero-extent box assert so the sizing box can handle it normally.
static const uintptr_t kBoxGetterStart = 0x10FDF6C0;
static const uintptr_t kBoxGetterEnd   = 0x10FDF900;
static const uint8_t   kCheckIdiom[]   = { 0xA0, 0xDD, 0xEE, 0x5B, 0x11, 0x84, 0xC0 };
static const uint8_t   kJzShort        = 0x74;
static const uint8_t   kJmpShort       = 0xEB;

static void DisableDegenerateBoxAsserts()
{
    int patched = 0;
    int already = 0;

    for (uintptr_t at = kBoxGetterStart; at < kBoxGetterEnd - sizeof(kCheckIdiom); ++at)
    {
        if (std::memcmp(reinterpret_cast<const void*>(at), kCheckIdiom, sizeof(kCheckIdiom)) != 0)
            continue;

        // Find the conditional jump that skips the assert body.
        for (uintptr_t scan = at + sizeof(kCheckIdiom); scan <= at + sizeof(kCheckIdiom) + 6; ++scan)
        {
            const uint8_t op   = *reinterpret_cast<volatile uint8_t*>(scan);
            const uint8_t disp = *reinterpret_cast<volatile uint8_t*>(scan + 1);

            if (disp != 0x44 && disp != 0x45)
                continue;

            if (op == kJmpShort) { ++already; break; }

            if (op == kJzShort)
            {
                if (MemoryWriter::WriteBytes(scan, &kJmpShort, sizeof(kJmpShort)))
                    ++patched;
                break;
            }
        }
    }

    Logger::log("SizingBoxFix: isValid() asserts neutralised: "
                + std::to_string(patched) + " patched, "
                + std::to_string(already) + " already done");

    if (patched + already == 0)
        Logger::log("SizingBoxFix: no isValid() assert sites matched - degenerate brush crash not fixed");
}

void SizingBoxFix::Initialize()
{
    for (const BytePatch& p : kLabelPatches)
        ApplyLabel(p);

    SwapWidthAndHeightSlots();
    DisableDegenerateBoxAsserts();
}
