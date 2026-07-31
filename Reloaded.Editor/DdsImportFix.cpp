#include "pch.h"
#include "DdsImportFix.h"
#include "MemoryWriter.h"
#include "logger.h"

// Improves compatibility with DXTn DDS files that omit DDSD_LINEARSIZE.
//
// UTextureFactory rejects such files before inspecting anything else, even
// though it never reads dwPitchOrLinearSize. Mip sizes are recomputed from the
// image dimensions, making the flag effectively a load-time gate.
//
// This patch changes the initial JNZ to JMP, bypassing that gate. Everything
// else is unchanged: only DXT formats are accepted, DXT2/4 remain rejected,
// and invalid mip chains are still caught by the existing bounds check.
//
// Files missing DDSD_LINEARSIZE are technically out of spec, but many DDS
// exporters omit it and most readers ignore the flag.

static const uintptr_t kLinearSizeBranch = 0x11052560;
static const uint8_t   kJnz = 0x75;
static const uint8_t   kJmp = 0xEB;

void DdsImportFix::Initialize()
{
    const uint8_t current = *reinterpret_cast<volatile uint8_t*>(kLinearSizeBranch);

    if (current == kJmp)
        return; // Already patched

    if (current != kJnz)
    {
        Logger::log("DdsImportFix: unexpected byte at 0x11052560, DDSD_LINEARSIZE gate left intact");
        return;
    }

    if (!MemoryWriter::WriteBytes(kLinearSizeBranch, &kJmp, sizeof(kJmp)))
        Logger::log("DdsImportFix: failed to patch the DDSD_LINEARSIZE gate");
}
