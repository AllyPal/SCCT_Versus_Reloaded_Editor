#pragma once

// LightmapFix: One in-process patch + post-save .sdc header repair.
//
// --- Patch A: FSurfaceLayout::AddSurface off-by-one (0x1119ebe9) ---
// The bin-packing loop uses JLE (≤0) to guard entry.  When SizeY == Height,
// Height-SizeY == 0 and JLE skips the loop entirely, so AddSurface always
// returns 0 even on a fresh empty atlas → verify() assertion crash at
// UnModelLight.cpp:747.  Changed to JL (<0) so the loop runs for Y=0.
// This is sufficient on its own: a 512×512 surface can land at (0,0) in
// the original 512-wide atlas, and all CompressLightmaps / renderer
// constants remain correct for the 512×512 atlas size.
//
// --- SDC header repair ---
// The SCCT editor's FArchiveFileWriterCompressed::Seek(0) only resets its
// internal buffer position; it does NOT rewind the underlying file stream.
// When a package exceeds 15 MB uncompressed (the single-buffer limit) the
// save produces a multi-chunk .sdc where:
//   - Chunk 0 holds the *placeholder* FPackageFileSummary (name_count == 0)
//   - The last chunk starts with the *real* FPackageFileSummary
//
// The reader always loads from chunk 0, so it sees an empty package.
//
// Fix: after every save, detect this layout and copy the real 64-byte summary
// from the last chunk into chunk 0, then re-compress and rewrite.

class LightmapFix
{
public:
    static void Initialize();
};
