#include "pch.h"
#include "GEKeybindSwap.h"
#include "MemoryWriter.h"

// ============================================================================
//  GE Keybind Configuration
// ============================================================================
//
//  The six globals below drive both the binary patch at startup and the
//  Reloaded Options dialog at runtime.  ReloadedOptions::Initialize() loads
//  them from Reloaded_Editor.ini before GEKeybindSwap::Initialize() is
//  called, so any INI-persisted values take effect from the very first frame.
//
//  Rules:
//    - Values must be uppercase ASCII letters ('A'–'Z').
//    - All six must be unique — two types sharing a key silently breaks both.
//    - Avoid Shift+keys already bound by the editor accelerator table:
//        A, B, C, G, I, J, O, Q, R, S, T, U, V, W, X, Y
//    - Pole (Shift+O) is intentionally excluded from user configuration;
//      it is an unused GE type left in the engine by accident.
//
//  Default mapping (same as the original compile-time constants):
//    Ledge Grab      Shift+E    (original default was Shift+L; swapped by design)
//    Hand-over-hand  Shift+H
//    Pipe            Shift+P
//    Ladder          Shift+L    (original default was Shift+E; swapped by design)
//    Zipline         Shift+Z
//    Fence           Shift+F
//
// ============================================================================

uint8_t g_KeyLedgeGrab    = 'E';
uint8_t g_KeyHandOverHand = 'H';
uint8_t g_KeyPipe         = 'P';
uint8_t g_KeyLadder       = 'L';
uint8_t g_KeyZipline      = 'Z';
uint8_t g_KeyFence        = 'F';

// Pole is not user-configurable — kept as a local constant.
static constexpr uint8_t KEY_POLE = 'O';

// ============================================================================
//  How the key binding works (read before changing site lists)
// ============================================================================
//
//  GE type detection reads live key-state bytes directly from an input array
//  embedded in the viewport object.  The array base starts at offset 0xD70
//  inside the input sub-object; each entry is one byte indexed by VK code:
//
//    flag_byte = *(viewport->inputObj + 0xD70 + VK_code)
//
//  Derived offsets used throughout the GE dispatch code:
//
//    GE type        default key   VK     struct offset   lo byte
//
//    Ledge Grab     Shift+L        0x4C   0xDBC           0xBC
//    Hand-over-hand Shift+H        0x48   0xDB8           0xB8
//    Pipe           Shift+P        0x50   0xDC0           0xC0
//    Ladder         Shift+E        0x45   0xDB5           0xB5
//    Zipline        Shift+Z        0x5A   0xDCA           0xCA
//    Fence          Shift+F        0x46   0xDB6           0xB6
//    Pole           Shift+O        0x4F   0xDBF           0xBF
//
//  Every occurrence in the binary is a 4-byte little-endian displacement word:
//    [lo_byte] 0D 00 00
//  Only the first (lo) byte encodes the VK-derived offset; the upper three
//  bytes are always 0x00 0x00 0x0D (fixed).  Remapping a key therefore
//  requires writing a single new lo byte at each reference site.
//
//  The lo byte formula:  lo = (uint8_t)(0xD70 + VK_code)
//                           = (uint8_t)(0x70  + VK_code)   [same result]
//
// ============================================================================
//  Binary reference sites
// ============================================================================
//
//   Ledge Grab (PATH A gate checks + GE dispatch)
//
//  10ec9414  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ed096c  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler gate
//  10ed1239  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler gate
//  10ed26d0  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler gate
//  10ed28f2  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    ClickHActor PATH A
//
//   Hand-over-hand
//
//  10ec90e7  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ec968c  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  10ed0c27  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0e48  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed3832  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3ce1  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4752  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed49f8  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
//   Pipe
//
//  10ec947a  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ec96b8  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  10ed0c3b  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0f08  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed3846  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3da1  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4816  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4ac2  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
//   Ladder
//
//  10ec9436  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ec970d  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  10ed0c31  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0e8f  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed383c  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3d28  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4799  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4a3f  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
//   Zipline
//
//  10ec9374  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  NOTE: 10ec92b5 checks VK_Z but dispatches "POLY SELECT ALL", not Zipline GE
//  placement — it belongs to AccelTablePatch, not this file.
//  10ed0c45  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0f81  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed3850  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3e1a  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4893  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4b45  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
//   Fence
//
//  10ec90a7  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ec9771  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  10ed0c4f  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0fc5  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed385a  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3e5e  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed48d7  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed4b89  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
//   Pole (Unused GE type; always use Pipe instead. Likely left in the engine by accident.)
//
//  10ec917b  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn
//  10ec9458  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   GE dispatch fn (2nd)
//  10ec978a  8A 88 [lo] 0D 00 00        MOV CL,[EAX+off]   bitfield toggle
//  10ed0c1d  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed0e01  80 BA [lo] 0D 00 00 00     CMP [EDX+off],0    click-handler
//  10ed3828  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed3c9a  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed470b  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//  10ed49b1  80 BE [lo] 0D 00 00 00     CMP [ESI+off],0    ClickHActor PATH B
//
// ============================================================================

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
        0x10ec9414,   // GE dispatch fn
        0x10ed096c,   // click-handler gate
        0x10ed1239,   // click-handler gate
        0x10ed26d0,   // click-handler gate
        0x10ed28f2,   // ClickHActor PATH A gate
    };
    PatchGEType(g_KeyLedgeGrab, kLedgeGrab, _countof(kLedgeGrab));

    // Hand-over-hand
    static constexpr uintptr_t kHOH[] = {
        0x10ec90e7,   // GE dispatch fn
        0x10ec968c,   // bitfield toggle
        0x10ed0c27,   // click-handler
        0x10ed0e48,   // click-handler
        0x10ed3832,   // ClickHActor PATH B
        0x10ed3ce1,   // ClickHActor PATH B
        0x10ed4752,   // ClickHActor PATH B
        0x10ed49f8,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyHandOverHand, kHOH, _countof(kHOH));

    // Pipe
    static constexpr uintptr_t kPipe[] = {
        0x10ec947a,   // GE dispatch fn
        0x10ec96b8,   // bitfield toggle
        0x10ed0c3b,   // click-handler
        0x10ed0f08,   // click-handler
        0x10ed3846,   // ClickHActor PATH B
        0x10ed3da1,   // ClickHActor PATH B
        0x10ed4816,   // ClickHActor PATH B
        0x10ed4ac2,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyPipe, kPipe, _countof(kPipe));

    // Ladder
    static constexpr uintptr_t kLadder[] = {
        0x10ec9436,   // GE dispatch fn
        0x10ec970d,   // bitfield toggle
        0x10ed0c31,   // click-handler
        0x10ed0e8f,   // click-handler
        0x10ed383c,   // ClickHActor PATH B
        0x10ed3d28,   // ClickHActor PATH B
        0x10ed4799,   // ClickHActor PATH B
        0x10ed4a3f,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyLadder, kLadder, _countof(kLadder));

    // Zipline
    static constexpr uintptr_t kZipline[] = {
        // NOTE: 0x10ec92b5 is intentionally absent — that byte controls "POLY SELECT ALL"
        // (same VK_Z check, different action).  It is owned by AccelTablePatch.
        0x10ec9374,   // bitfield toggle
        0x10ed0c45,   // click-handler
        0x10ed0f81,   // click-handler
        0x10ed3850,   // ClickHActor PATH B
        0x10ed3e1a,   // ClickHActor PATH B
        0x10ed4893,   // ClickHActor PATH B
        0x10ed4b45,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyZipline, kZipline, _countof(kZipline));

    // Fence
    static constexpr uintptr_t kFence[] = {
        0x10ec90a7,   // GE dispatch fn
        0x10ec9771,   // bitfield toggle
        0x10ed0c4f,   // click-handler
        0x10ed0fc5,   // click-handler
        0x10ed385a,   // ClickHActor PATH B
        0x10ed3e5e,   // ClickHActor PATH B
        0x10ed48d7,   // ClickHActor PATH B
        0x10ed4b89,   // ClickHActor PATH B
    };
    PatchGEType(g_KeyFence, kFence, _countof(kFence));

    // Pole — not user-configurable, patched once with the fixed KEY_POLE value.
    static constexpr uintptr_t kPole[] = {
        0x10ec917b,   // GE dispatch fn
        0x10ec9458,   // GE dispatch fn (2nd reference)
        0x10ec978a,   // bitfield toggle
        0x10ed0c1d,   // click-handler
        0x10ed0e01,   // click-handler
        0x10ed3828,   // ClickHActor PATH B
        0x10ed3c9a,   // ClickHActor PATH B
        0x10ed470b,   // ClickHActor PATH B
        0x10ed49b1,   // ClickHActor PATH B
    };
    PatchGEType(KEY_POLE, kPole, _countof(kPole));
}

void GEKeybindSwap::Initialize()
{
    // g_Key* globals were already set by ReloadedOptions::Initialize() from INI.
    // Just apply them to the binary now.
    ApplyGEKeybinds();
}
