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

struct SDCChunk
{
    uint32_t uncompSize;
    uint32_t compSize;
    long     headerOffset;  // file offset of this chunk's 8-byte [uncomp][comp] header
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

    uint32_t totalUncomp = 0;
    for (auto& c : chunks)
        totalUncomp += c.uncompSize;

    Logger::log(std::format(
        "LightmapFix: Nuked .sdc confirmed - {} chunks, {} MB uncompressed. Streaming merge: {}",
        chunks.size(),
        totalUncomp / (1024u * 1024u),
        path));

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

    static const uInt kBufSize = 256u * 1024u;  // 256 KB output ring
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

static const char* s_savedFilename = nullptr;
static int         s_origSaveFn    = 0x10e0416b;
static uintptr_t   s_savedRetAddr  = 0;

static std::string MapsEdToMapsPath(const char* mapsEdPath)
{
    if (!mapsEdPath || !*mapsEdPath)
        return {};

    std::string path(mapsEdPath);

    // Upper-case a copy for the case-insensitive search
    std::string upper = path;
    for (auto& c : upper)
        c = (char)toupper((unsigned char)c);

    const std::string marker = "\\MAPSED\\";
    size_t pos = upper.find(marker);
    if (pos == std::string::npos)
        return {};

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

            // Fix the runtime map (Packages\Maps\)
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

CALL_HOOK(0x10e3cf18, SaveAsHook)
{
    __asm {
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

void LightmapFix::Initialize()
{
    INSTALL_HOOKS;

    {
        static const uint8_t jl_patch[] = { 0x7C };
        if (MemoryWriter::WriteBytes(0x1119ebe9u, jl_patch, sizeof(jl_patch)))
            Logger::log("LightmapFix: Patched successfully.");
        else
            Logger::log("LightmapFix: Patch failed.");
    }
}
