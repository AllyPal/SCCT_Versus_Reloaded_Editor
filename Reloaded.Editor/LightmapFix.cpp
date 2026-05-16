#include "pch.h"
#include "LightmapFix.h"
#include "Hooks.h"
#include "logger.h"

#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <format>

INIT_HOOKS;

// ---------------------------------------------------------------------------
//  SDC chunk parsing
// ---------------------------------------------------------------------------

struct SDCChunk
{
    uint32_t uncompSize;
    uint32_t compSize;
    long     headerOffset;   // file offset of this chunk's 8-byte [uncomp][comp] header
};

static bool ParseSDCChunks(const char* path, std::vector<SDCChunk>& chunks)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    long offset = 0;
    while (offset <= fileSize - 8)
    {
        uint32_t uncomp = 0, comp = 0;
        if (fread(&uncomp, 4, 1, f) != 1) break;
        if (fread(&comp,   4, 1, f) != 1) break;
        if (comp == 0 || uncomp == 0)     break;

        SDCChunk c;
        c.uncompSize   = uncomp;
        c.compSize     = comp;
        c.headerOffset = offset;
        chunks.push_back(c);

        offset += 8 + (long)comp;
        if (fseek(f, offset, SEEK_SET) != 0) break;
    }

    fclose(f);
    return !chunks.empty();
}

// Decompress one chunk into a heap buffer.  Returns empty on any failure.
static std::vector<uint8_t> ReadAndDecompress(const char* path, const SDCChunk& chunk)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return {};

    fseek(f, chunk.headerOffset + 8, SEEK_SET);

    std::vector<uint8_t> compData(chunk.compSize);
    if (fread(compData.data(), 1, chunk.compSize, f) != chunk.compSize)
    {
        fclose(f);
        return {};
    }
    fclose(f);

    std::vector<uint8_t> outData(chunk.uncompSize);
    uLongf destLen = chunk.uncompSize;
    if (uncompress(outData.data(), &destLen, compData.data(), chunk.compSize) != Z_OK)
        return {};

    outData.resize((size_t)destLen);
    return outData;
}

// ---------------------------------------------------------------------------
//  The fix
//
//  Why merge to a single chunk instead of just patching chunk 0's header:
//
//  FArchiveFileReaderCompressed allocates its compressed-data buffer ONCE at
//  construction time using the FIRST chunk's compSize.  For nuked maps the
//  first chunk compresses to ~1.1 MB, but later chunks can be up to ~1.4 MB.
//  When the reader tries to load those larger chunks it overruns the buffer
//  and aborts — so the map still can't load even with a patched header.
//
//  Also: the reader stores TotalSize = first chunk's uncompressed size only
//  (~15 MB).  A seek to the export table (often > 15 MB) exceeds this limit
//  and triggers an archive error.
//
//  Merging everything into one chunk side-steps both problems:
//    - One buffer, allocated for the full (correct) compressed size.
//    - TotalSize = real total uncompressed size.
//    - No secondary-chunk buffer overrun.
//
//  Implementation note — streaming vs. bulk:
//  Large maps with many 512-px lightmaps produce 20+ chunks totalling 400+ MB
//  uncompressed.  Allocating a flat 400+ MB buffer in a 32-bit process throws
//  std::bad_alloc; if that exception propagates through inline-asm hook code
//  with no SEH frame it manifests as a General Protection Fault (the GPF the
//  user sees after a successful save).  The implementation below processes one
//  decompressed chunk (~15 MB) at a time through a zlib deflate stream,
//  writing compressed output directly to a temp file.  Peak extra allocation
//  is ~15 MB regardless of how many chunks the writer produced.
// ---------------------------------------------------------------------------

static const uint32_t UE2_MAGIC = 0x9E2A83C1u;

static bool FixSDCFile(const char* path)
{
    if (!path || !*path)
        return false;

    const char* ext = strrchr(path, '.');
    if (!ext || _stricmp(ext, ".sdc") != 0)
        return false;

    std::vector<SDCChunk> chunks;
    if (!ParseSDCChunks(path, chunks))
        return false;

    // Single-chunk files are always valid (the writer bug only fires when the
    // package exceeds the 15 MB buffer and spills into a second chunk).
    if (chunks.size() < 2)
        return false;

    // -----------------------------------------------------------------------
    //  Step 1: Read ONLY the last chunk to recover the real FPackageFileSummary.
    //
    //  The writer's Seek(0) only resets its in-buffer position counter; it
    //  does not rewind the underlying file.  The real 64-byte summary is
    //  written at the start of the LAST chunk's decompressed buffer.
    //
    //  We intentionally avoid building a combined buffer here: for large maps
    //  (e.g. 29 chunks × 15 MB = 420+ MB) a flat allocation in a 32-bit
    //  process throws std::bad_alloc and, propagating through inline-asm hook
    //  code, manifests as a General Protection Fault.  Instead we stream one
    //  chunk at a time through a zlib deflate object writing to a temp file.
    // -----------------------------------------------------------------------
    auto lastChunkData = ReadAndDecompress(path, chunks.back());
    if (lastChunkData.size() < 64)
    {
        Logger::log("LightmapFix: Failed to decompress last chunk.");
        return false;
    }

    uint32_t magicL     = *reinterpret_cast<uint32_t*>(lastChunkData.data());
    uint32_t nameCountL = *reinterpret_cast<uint32_t*>(lastChunkData.data() + 12);
    if (magicL != UE2_MAGIC || nameCountL == 0)
        return false;   // last chunk doesn't carry the real header; unexpected layout

    uint8_t realSummary[64];
    std::memcpy(realSummary, lastChunkData.data(), 64);
    lastChunkData.clear();  // free ~3–15 MB before touching chunk 0

    // -----------------------------------------------------------------------
    //  Step 2: Read chunk 0 and validate the placeholder pattern.
    // -----------------------------------------------------------------------
    auto chunk0Data = ReadAndDecompress(path, chunks[0]);
    if (chunk0Data.size() < 64)
    {
        Logger::log("LightmapFix: Failed to decompress chunk 0.");
        return false;
    }

    uint32_t magic0     = *reinterpret_cast<uint32_t*>(chunk0Data.data());
    uint32_t nameCount0 = *reinterpret_cast<uint32_t*>(chunk0Data.data() + 12);
    if (magic0 != UE2_MAGIC || nameCount0 != 0)
        return false;   // not the corruption pattern we expect; leave alone

    // Overwrite the placeholder with the real summary.
    std::memcpy(chunk0Data.data(), realSummary, 64);

    // -----------------------------------------------------------------------
    //  Step 3: compute total uncompressed size.
    // -----------------------------------------------------------------------
    uint32_t totalUncomp = 0;
    for (auto& c : chunks)
        totalUncomp += c.uncompSize;

    Logger::log(std::format(
        "LightmapFix: Nuked .sdc confirmed - {} chunks, {} MB uncompressed. Streaming merge: {}",
        chunks.size(),
        totalUncomp / (1024u * 1024u),
        path));

    // -----------------------------------------------------------------------
    //  Step 4: stream-compress all chunks to a temp file.
    //
    //  Only one decompressed chunk (~15 MB) is live at any moment.  The zlib
    //  deflate object accumulates compressed output into a 256 KB ring buffer
    //  that is flushed to disk on each pass.  Peak extra allocation ≈ 15 MB.
    // -----------------------------------------------------------------------
    std::string tmpPath = std::string(path) + ".fix";
    FILE* fout = nullptr;
    if (fopen_s(&fout, tmpPath.c_str(), "wb") != 0 || !fout)
    {
        Logger::log("LightmapFix: Cannot create temp file.");
        return false;
    }

    // Write a placeholder header; we'll seek back to fill compSize after deflate.
    uint32_t hdr[2] = { totalUncomp, 0u };
    if (fwrite(hdr, 4, 2, fout) != 2)
    {
        fclose(fout);
        remove(tmpPath.c_str());
        return false;
    }

    z_stream zs = {};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
    {
        fclose(fout);
        remove(tmpPath.c_str());
        return false;
    }

    static const uInt kBufSize = 256u * 1024u;   // 256 KB output ring
    std::vector<uint8_t> outBuf(kBufSize);
    uint32_t totalComp = 0;
    bool ok = true;

    for (size_t i = 0; i < chunks.size() && ok; ++i)
    {
        std::vector<uint8_t> tempData;
        uint8_t* srcPtr;
        uInt     srcSize;

        if (i == 0)
        {
            // Already decompressed and patched above.
            srcPtr  = chunk0Data.data();
            srcSize = static_cast<uInt>(chunk0Data.size());
        }
        else
        {
            tempData = ReadAndDecompress(path, chunks[i]);
            if (tempData.empty())
            {
                Logger::log(std::format("LightmapFix: Failed to decompress chunk {}.", i));
                ok = false;
                break;
            }
            srcPtr  = tempData.data();
            srcSize = static_cast<uInt>(tempData.size());
        }

        // Feed this chunk into the deflate stream.
        zs.next_in  = srcPtr;
        zs.avail_in = srcSize;
        while (zs.avail_in > 0 && ok)
        {
            zs.next_out  = outBuf.data();
            zs.avail_out = kBufSize;
            if (deflate(&zs, Z_NO_FLUSH) == Z_STREAM_ERROR) { ok = false; break; }
            uInt n = kBufSize - zs.avail_out;
            if (n > 0 && fwrite(outBuf.data(), 1, n, fout) != n) { ok = false; break; }
            totalComp += n;
        }
    }

    // Flush and finish the deflate stream.
    if (ok)
    {
        int ret;
        do {
            zs.next_in   = nullptr;
            zs.avail_in  = 0;
            zs.next_out  = outBuf.data();
            zs.avail_out = kBufSize;
            ret = deflate(&zs, Z_FINISH);
            if (ret == Z_STREAM_ERROR) { ok = false; break; }
            uInt n = kBufSize - zs.avail_out;
            if (n > 0 && fwrite(outBuf.data(), 1, n, fout) != n) { ok = false; break; }
            totalComp += n;
        } while (ret != Z_STREAM_END);
    }

    deflateEnd(&zs);

    // Patch the compSize into the header now that we know the final value.
    if (ok)
    {
        fseek(fout, 4, SEEK_SET);
        ok = (fwrite(&totalComp, 4, 1, fout) == 1);
    }

    fclose(fout);

    if (!ok)
    {
        remove(tmpPath.c_str());
        Logger::log("LightmapFix: Streaming compression failed; temp file removed.");
        return false;
    }

    // -----------------------------------------------------------------------
    //  Step 5: atomically replace the original file with the fixed one.
    // -----------------------------------------------------------------------
    remove(path);
    if (rename(tmpPath.c_str(), path) != 0)
    {
        Logger::log(std::format("LightmapFix: Failed to rename {} to {}.", tmpPath, path));
        return false;
    }

    Logger::log(std::format(
        "LightmapFix: Done. Single-chunk SDC written ({} MB compressed).",
        totalComp / (1024u * 1024u)));
    return true;
}

// ---------------------------------------------------------------------------
//  Shared hook state
//
//  Both save paths (Save As and regular Save) call the same underlying
//  SaveMap function via the JMP thunk at 0x10e0416b -> FUN_10ee4d30.
//  We capture the filename pointer just before the call, let the original
//  run, then post-process the .sdc file on success.
//
//  s_savedFilename holds a pointer into the CALLER's stack frame.  It is
//  only dereferenced synchronously (inside the hook, before returning), so
//  lifetime is guaranteed.
// ---------------------------------------------------------------------------

static const char* s_savedFilename = nullptr;
static int         s_origSaveFn    = 0x10e0416b;  // JMP -> FUN_10ee4d30
static uintptr_t   s_savedRetAddr  = 0;            // original caller return address

// ---------------------------------------------------------------------------
//  Path helper: given a MapsEd path, return the corresponding Maps path.
//
//  "...\Packages\MapsEd\TestMap.sdc" -> "...\Packages\Maps\TestMap.sdc"
//
//  Does a case-insensitive search for "\MapsEd\" (i.e., "\MAPSED\" after
//  uppercasing) and replaces it with "\Maps\".  Returns empty string if the
//  MapsEd marker isn't found (so we won't accidentally touch unrelated files).
// ---------------------------------------------------------------------------
static std::string MapsEdToMapsPath(const char* mapsEdPath)
{
    if (!mapsEdPath || !*mapsEdPath)
        return {};

    std::string path(mapsEdPath);

    // Upper-case a copy for the case-insensitive search
    std::string upper = path;
    for (auto& c : upper)
        c = (char)toupper((unsigned char)c);

    const std::string marker = "\\MAPSED\\";   // uppercase form of the MapsEd folder name
    size_t pos = upper.find(marker);
    if (pos == std::string::npos)
        return {};

    // Replace the "\MapsEd\" segment (8 chars) with "\Maps\" (6 chars)
    return path.substr(0, pos) + "\\Maps\\" + path.substr(pos + marker.size());
}

static void __cdecl RunFixIfNeeded()
{
    if (s_savedFilename && *s_savedFilename)
    {
        try
        {
            // Fix the editor map (Packages\MapsEd\)
            FixSDCFile(s_savedFilename);

            // Fix the runtime map (Packages\Maps\) - same writer bug, same fix
            std::string mapsPath = MapsEdToMapsPath(s_savedFilename);
            if (!mapsPath.empty())
                FixSDCFile(mapsPath.c_str());
        }
        catch (const std::exception& ex)
        {
            Logger::log(std::format("LightmapFix: Exception in FixSDCFile: {}", ex.what()));
        }
        catch (...)
        {
            Logger::log("LightmapFix: Unknown exception in FixSDCFile; map may be nuked.");
        }
    }
    s_savedFilename = nullptr;
}

// ---------------------------------------------------------------------------
//  Why JMP instead of CALL to invoke FUN_10ee4d30:
//
//  FUN_10ee4d30 is __thiscall with 1 stack arg (filename), cleaned by RETN 4.
//  The original call site does:
//      PUSH filename        ; [ESP+4] on entry to callee
//      CALL FUN_10e0416b    ; pushes return addr → callee sees [ESP+4]=filename
//
//  Our CALL_HOOK replaces that CALL.  On hook entry the stack is already:
//      [ESP+0] = original caller retaddr
//      [ESP+4] = filename (pushed by the PUSH before our hook)
//
//  If we used `call dword ptr [s_origSaveFn]`, it would push one more address,
//  giving FUN_10ee4d30 [ESP+4] = our hook's internal retaddr - NOT the filename.
//  The function copies that garbage into a local buffer, _strupr-s it, finds no
//  "\MAPSED\" in the result, and shows "maps must be saved in MapsEd directory".
//  Then RETN 4 cleans the wrong slot and our final RET jumps to the filename
//  string as code → crash.
//
//  Fix: overwrite [ESP+0] with our continuation label, then JMP (no extra push).
//  FUN_10ee4d30 sees a clean stack identical to the original call site.
//  Its RETN 4 returns to our continuation and removes filename, leaving the
//  original return address (saved in s_savedRetAddr) to be pushed and ret'd.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  Hook: File > Save As  (FUN_10e3c870)
//
//  Patches the CALL instruction at 0x10e3cf18:
//    LEA ECX, [ESP+0x18c]     ; ECX = filename char* (local buffer)
//    PUSH ECX                 ; push filename arg        <- [ESP+4] on entry
//    MOV ECX, [0x1165dfa0]    ; ECX = editor this
//    CALL 0x10e0416b          ; <-- we replace this CALL
//    MOV EBP, EAX             ; next instruction (our continuation lands here)
// ---------------------------------------------------------------------------
CALL_HOOK(0x10e3cf18, SaveAsHook)
{
    __asm {
        // [ESP+0] = original retaddr (0x10e3cf1d)
        // [ESP+4] = filename char*
        // ECX     = editor this

        mov  eax, [esp+4]
        mov  [s_savedFilename], eax      // capture filename

        // Save original retaddr; plant our continuation in its place
        mov  eax, [esp]
        mov  [s_savedRetAddr], eax
        mov  dword ptr [esp], offset sa_cont

        // JMP, not CALL - no extra return address pushed.
        // FUN_10ee4d30 now sees [ESP+0]=sa_cont, [ESP+4]=filename (correct).
        // Its RETN 4 returns to sa_cont and removes filename.
        jmp  dword ptr [s_origSaveFn]

    sa_cont:
        // EAX = save result; filename slot already cleaned by RETN 4.
        // Stack is now exactly what the original caller expects below filename.

        test eax, eax
        jz   sa_done

        push eax
        call RunFixIfNeeded              // __cdecl, no args
        pop  eax

    sa_done:
        // Restore original retaddr and return to caller
        push dword ptr [s_savedRetAddr]
        ret
    }
}

// ---------------------------------------------------------------------------
//  Hook: File > Save  (FUN_10e3d300, Ctrl+S / Build All)
//
//  Patches the CALL instruction at 0x10e3dbe5:
//    MOV EAX, [ESP+0x8]       ; EAX = filename char* (from FString)
//    MOV ECX, [0x1165dfa0]    ; ECX = editor this
//    PUSH EAX                 ; push filename arg        <- [ESP+4] on entry
//    CALL 0x10e0416b          ; <-- we replace this CALL
//    MOV ECX, [0x115befb0]    ; next instruction (our continuation lands here)
//
//  Same JMP strategy as SaveAsHook.
// ---------------------------------------------------------------------------
CALL_HOOK(0x10e3dbe5, SaveHook)
{
    __asm {
        // [ESP+0] = original retaddr (0x10e3dbea)
        // [ESP+4] = filename char*
        // ECX     = editor this

        mov  eax, [esp+4]
        mov  [s_savedFilename], eax

        mov  eax, [esp]
        mov  [s_savedRetAddr], eax
        mov  dword ptr [esp], offset sv_cont

        jmp  dword ptr [s_origSaveFn]

    sv_cont:
        test eax, eax
        jz   sv_done

        push eax
        call RunFixIfNeeded
        pop  eax

    sv_done:
        push dword ptr [s_savedRetAddr]
        ret
    }
}

// ---------------------------------------------------------------------------
//  Module init
// ---------------------------------------------------------------------------
void LightmapFix::Initialize()
{
    INSTALL_HOOKS;

    // -----------------------------------------------------------------------
    //  Patch A: Fix FSurfaceLayout::AddSurface off-by-one  [FUN_1119ebb0]
    //
    //  The bin-packing loop in AddSurface reads (pseudo-C):
    //
    //      iVar4 = Height - SizeY;
    //      if (0 < iVar4)            // ← JLE 0x1119ec65  ← BUG
    //          for (Y = 0; Y < iVar4; Y++) { ... find best row ... }
    //      if (BestX <= Width - SizeX && BestY <= Height - SizeY)
    //          place surface, return 1;
    //      return 0;
    //
    //  When SizeY == Height (a LIGHTMAP_MAX_RES surface in the original 512-
    //  pixel-tall atlas), iVar4 == 0 and `0 < 0` is false → loop is skipped
    //  entirely.  BestY keeps its initialised-to-Height sentinel, so the final
    //  check "BestY <= Height - SizeY" evaluates to "Height <= 0" → false →
    //  AddSurface returns 0 even on a fresh empty atlas.
    //
    //  SetupLightMap then calls CreateLightMapTexture (new fresh atlas) and
    //  retries with verify() — which also returns 0 for the same reason →
    //  assertion crash:
    //    "Assertion failed: LightMapLayout.AddSurface(...) [UnModelLight.cpp:747]"
    //
    //  Fix: JLE (7E) → JL (7C).  With JL the loop body runs for Y=0 when
    //  iVar4==0.  On a fresh atlas AllocatedTexels are all 0, so MaxX=0 and
    //  Waste=0 → BestX=0, BestY=0 → placement at (0,0) succeeds.
    //
    //  Address: 0x1119ebe9
    //  Before:  7E 7A  (JLE +0x7A → 0x1119ec65)
    //  After:   7C 7A  (JL  +0x7A → 0x1119ec65)
    // -----------------------------------------------------------------------
    {
        static const uint8_t jl_patch[] = { 0x7C };
        if (MemoryWriter::WriteBytes(0x1119ebe9u, jl_patch, sizeof(jl_patch)))
            Logger::log("LightmapFix: AddSurface JLE->JL off-by-one patch applied (0x1119ebe9).");
        else
            Logger::log("LightmapFix: WARNING - AddSurface JLE->JL patch FAILED (0x1119ebe9).");
    }

}
