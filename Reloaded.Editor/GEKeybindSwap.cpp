#include "pch.h"
#include "GEKeybindSwap.h"
#include "MemoryWriter.h"

uint8_t g_KeyLedgeGrab    = 'E';
uint8_t g_KeyHandOverHand = 'H';
uint8_t g_KeyPipe         = 'P';
uint8_t g_KeyLadder       = 'L';
uint8_t g_KeyZipline      = 'Z';
uint8_t g_KeyFence        = 'F';

// Pole - Unused GE type accidentally left in, disabled by binding it to an unassigned VK code
static constexpr uint8_t KEY_POLE_DISABLED = 0x3F;

// Compute the lo byte of the input-array displacement for a given VK code.
static constexpr uint8_t GEOffset(uint8_t vk) { return static_cast<uint8_t>(0x70 + vk); }

// Patch every binary site for one GE type.
static void PatchGEType(uint8_t newKey, const uintptr_t* sites, size_t count)
{
    const uint8_t lo = GEOffset(newKey);
    for (size_t i = 0; i < count; ++i)
        MemoryWriter::WriteBytes(sites[i], &lo, 1);
}

void GEKeybindSwap::ApplyGEKeybinds()
{
    // Ledge Grab
    static constexpr uintptr_t kLedgeGrab[] = {
        0x10ed096c,   // PATH A gate
        0x10ed1239,   // PATH A gate
        0x10ed26d0,   // PATH A gate
        0x10ed28f2,   // ClickHActor PATH A gate
    };
    PatchGEType(g_KeyLedgeGrab, kLedgeGrab, _countof(kLedgeGrab));

    // Hand-over-hand
    static constexpr uintptr_t kHOH[] = {
        0x10ed0c27,   // click-handler: GE-key-held gate
        0x10ed0e48,   // click-handler: GE spawn
        0x10ed3832,   // ClickHActor PATH B
        0x10ed3ce1,   // ClickHActor PATH B
        0x10ed4752,   // ClickHActor PATH B
        0x10ed49f8,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyHandOverHand, kHOH, _countof(kHOH));

    // Pipe
    static constexpr uintptr_t kPipe[] = {
        0x10ed0c3b,   // click-handler: GE-key-held gate
        0x10ed0f08,   // click-handler: GE spawn
        0x10ed3846,   // ClickHActor PATH B
        0x10ed3da1,   // ClickHActor PATH B
        0x10ed4816,   // ClickHActor PATH B
        0x10ed4ac2,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyPipe, kPipe, _countof(kPipe));

    // Ladder
    static constexpr uintptr_t kLadder[] = {
        0x10ed0c31,   // click-handler: GE-key-held gate
        0x10ed0e8f,   // click-handler: GE spawn
        0x10ed383c,   // ClickHActor PATH B
        0x10ed3d28,   // ClickHActor PATH B
        0x10ed4799,   // ClickHActor PATH B
        0x10ed4a3f,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyLadder, kLadder, _countof(kLadder));

    // Zipline
    static constexpr uintptr_t kZipline[] = {
        0x10ed0c45,   // click-handler: GE-key-held gate
        0x10ed0f81,   // click-handler: GE spawn
        0x10ed3850,   // ClickHActor PATH B
        0x10ed3e1a,   // ClickHActor PATH B
        0x10ed4893,   // ClickHActor PATH B
        0x10ed4b45,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyZipline, kZipline, _countof(kZipline));

    // Fence
    static constexpr uintptr_t kFence[] = {
        0x10ed0c4f,   // click-handler: GE-key-held gate
        0x10ed0fc5,   // click-handler: GE spawn
        0x10ed385a,   // ClickHActor PATH B
        0x10ed3e5e,   // ClickHActor PATH B
        0x10ed48d7,   // ClickHActor PATH B
        0x10ed4b89,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyFence, kFence, _countof(kFence));

    // Pole - Unused GE type accidentally left in, disabled by binding it to an unassigned VK code
    static constexpr uintptr_t kPole[] = {
        0x10ed0c1d,   // click-handler: GE-key-held gate
        0x10ed0e01,   // click-handler: GE spawn
        0x10ed3828,   // ClickHActor PATH B
        0x10ed3c9a,   // ClickHActor PATH B
        0x10ed470b,   // ClickHActor PATH B
        0x10ed49b1,   // ClickHActor PATH B
    };
    PatchGEType(KEY_POLE_DISABLED, kPole, _countof(kPole));
}

void GEKeybindSwap::Initialize()
{
    ApplyGEKeybinds();
}
