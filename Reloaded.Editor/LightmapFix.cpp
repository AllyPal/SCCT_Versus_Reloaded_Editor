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

// ---------------------------------------------------------------------
//  Catching the summary rewrite before it lands
// ---------------------------------------------------------------------
//  The package summary is written twice: a placeholder at offset 0, then the real
//  one seeked back in once the counts are known.  This archive implements Seek(0)
//  by rewinding the 15 MB buffer, which is offset 0 only while chunk 0 is still
//  unflushed; past that the summary lands on whatever object sits at the boundary.
//  Both the seek and the write after it are suppressed and the bytes kept here for
//  FixSDCFile.  A save writes two packages - the MapsEd copy and the runtime one -
//  so each capture is tagged with the archive size at the seek and matched by size.
#define ARCHIVE_BYTES_EMITTED   0x1E01050   // total uncompressed bytes already flushed
#define ARCHIVE_LOGICAL_SIZE    0x44        // bytes handed to the archive so far

struct SummaryCapture
{
    uint32_t packageSize;
    int      length;
    uint8_t  bytes[512];
};

static const int      kCaptureSlots = 4;
static SummaryCapture s_captures[kCaptureSlots];
static int            s_captureSlot;
static void*          s_captureArchive;

static void ClearSummaryCaptures()
{
    for (SummaryCapture& capture : s_captures)
    {
        capture.packageSize = 0;
        capture.length      = 0;
    }
    s_captureArchive = nullptr;
    s_captureSlot    = 0;
}

extern "C" void __cdecl LightmapFix_BeginSummaryCapture(void* archive, uint32_t packageSize)
{
    s_captureSlot = (s_captureSlot + 1) % kCaptureSlots;
    s_captures[s_captureSlot].packageSize = packageSize;
    s_captures[s_captureSlot].length      = 0;
    s_captureArchive = archive;
}

// Where the name table starts is also how long the summary is. Zero until enough is captured.
static uint32_t CapturedSummaryLength(const SummaryCapture& capture)
{
    static const int kNameOffsetField = 16;

    if (capture.length < kNameOffsetField + static_cast<int>(sizeof(uint32_t)))
        return 0;
    if (*reinterpret_cast<const uint32_t*>(capture.bytes) != UE2_MAGIC)
        return 0;

    const uint32_t nameOffset = *reinterpret_cast<const uint32_t*>(capture.bytes + kNameOffsetField);
    return (nameOffset > 0 && nameOffset <= sizeof(capture.bytes)) ? nameOffset : 0;
}

extern "C" void __cdecl LightmapFix_CaptureSummary(const void* data, int length)
{
    if (length <= 0)
        return;

    SummaryCapture& capture = s_captures[s_captureSlot];

    const int room = static_cast<int>(sizeof(capture.bytes)) - capture.length;
    if (length > room)
        length = room;

    std::memcpy(capture.bytes + capture.length, data, length);
    capture.length += length;

    // A save writes two packages and the second archive lands on the first's address, so staying
    // armed past the summary swallows its writes too. Disarm once nothing more can be kept.
    const uint32_t summaryLength = CapturedSummaryLength(capture);
    if (summaryLength && capture.length >= static_cast<int>(summaryLength))
    {
        capture.length    = static_cast<int>(summaryLength);
        s_captureArchive  = nullptr;
    }
    else if (capture.length >= static_cast<int>(sizeof(capture.bytes)))
    {
        s_captureArchive = nullptr;
    }
}

static const SummaryCapture* FindSummaryCapture(uint32_t packageSize)
{
    for (const SummaryCapture& capture : s_captures)
        if (capture.length >= 64 && capture.packageSize == packageSize)
            return &capture;
    return nullptr;
}

JMP_HOOK(0x10e32d30, CompressedSeekHook)
{
    static int Resume = 0x10e32d35;
    __asm {
        cmp  dword ptr [esp + 4], 0     // only Seek(0) exists; the rest already appErrors
        jne  run_stock
        mov  eax, [ecx + ARCHIVE_BYTES_EMITTED]
        test eax, eax
        jz   run_stock                  // nothing flushed yet, so 0 really is offset 0

        pushad
        mov  eax, [ecx + ARCHIVE_LOGICAL_SIZE]
        push eax
        push ecx
        call LightmapFix_BeginSummaryCapture
        add  esp, 8
        popad
        ret  4                          // no rewind: the buffer keeps the real data

    run_stock:
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [Resume]
    }
}

JMP_HOOK(0x10e32ed0, CompressedSerializeHook)
{
    static int Resume = 0x10e32ed5;
    __asm {
        mov  eax, dword ptr [s_captureArchive]
        test eax, eax
        jz   run_stock
        cmp  eax, ecx
        jne  run_stock

        pushad                          // [esp+0x20] retaddr, +0x24 data, +0x28 length
        mov  eax, dword ptr [esp + 0x28]
        mov  edx, dword ptr [esp + 0x24]
        push eax
        push edx
        call LightmapFix_CaptureSummary
        add  esp, 8
        popad
        ret  8

    run_stock:
        push ebx
        mov  ebx, dword ptr [esp + 0x0c]
        jmp  dword ptr [Resume]
    }
}

// ---------------------------------------------------------------------
//  Warning about maps the old writer already damaged
// ---------------------------------------------------------------------
//  Those 64 bytes are gone, so such a map can only be flagged.  The old repair
//  copied the stray summary into the header, so a damaged file is one whose bytes
//  at a chunk boundary match its own first 64.
#define CHUNK_LIMIT 0xF00000u

static LightmapFix::DamageSink s_damageSink;

uint32_t LightmapFix::ScanForDamage(const char* path)
{
    if (!path || !*path)
        return 0;

    std::vector<SDCChunk> chunks;
    if (!ParseSDCChunks(path, chunks))
        return 0;

    uint8_t  header[64];
    uint32_t base = 0;

    for (const SDCChunk& chunk : chunks)
    {
        std::vector<uint8_t> data = ReadAndDecompress(path, chunk);
        if (data.size() < sizeof(header))
            return 0;

        if (base == 0)
            std::memcpy(header, data.data(), sizeof(header));

        const uint32_t first = ((base + CHUNK_LIMIT - 1) / CHUNK_LIMIT) * CHUNK_LIMIT;
        for (uint32_t at = first; at + sizeof(header) <= base + data.size(); at += CHUNK_LIMIT)
            if (at > 0 && std::memcmp(header, data.data() + (at - base), sizeof(header)) == 0)
                return at;

        base += chunk.uncompSize;
    }
    return 0;
}

void LightmapFix::SetDamageSink(DamageSink sink)
{
    s_damageSink = sink;
}

static void __cdecl WarnIfMapDamaged(const char* cmd)
{
    static const char kVerb[] = "MAP LOAD FILE=";
    if (!cmd || _strnicmp(cmd, kVerb, sizeof(kVerb) - 1) != 0)
        return;
    const char* p = cmd + sizeof(kVerb) - 1;
    const char  terminator = (*p == '"') ? '"' : ' ';
    if (*p == '"') ++p;

    std::string path;
    while (*p && *p != terminator)
        path += *p++;

    const uint32_t at = path.empty() ? 0 : LightmapFix::ScanForDamage(path.c_str());
    if (!at)
        return;

    const size_t slash = path.find_last_of("\\/");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    Logger::log(std::format("LightmapFix: {} damaged at stream offset {} ({} MB)",
                            name, at, at / (1024u * 1024u)));

    char message[320];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
        "%s was damaged by an older version of the editor. Rebuilding and saving it repairs "
        "the file; the small piece that was destroyed cannot be brought back.",
        name.c_str());

    if (s_damageSink)
        s_damageSink(message);
    else
        MessageBoxA(nullptr, message, "Reloaded Editor", MB_OK | MB_ICONWARNING);
}

// Every map load - File > Open, the MRU, the bulk rebuild - reaches Exec as MAP LOAD FILE="..".
JMP_HOOK(0x110183b0, EditorExecHook)
{
    static int Resume = 0x110183b5;
    __asm {
        pushad                          // [esp+0x20] retaddr, +0x24 command
        mov  eax, dword ptr [esp + 0x24]
        push eax
        call WarnIfMapDamaged
        add  esp, 4
        popad

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [Resume]
    }
}

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

    uint32_t totalUncomp = 0;
    for (const SDCChunk& c : chunks)
        totalUncomp += c.uncompSize;

    std::vector<uint8_t> realSummary;
    const SummaryCapture* capture = FindSummaryCapture(totalUncomp);

    if (capture)
    {
        // Caught on the way out, so no payload was overwritten to recover from.
        realSummary.assign(capture->bytes, capture->bytes + capture->length);
    }
    else
    {
        // Saved before the interception: the summary overwrote the last chunk's first 64 bytes.
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

        realSummary.assign(lastChunkData.begin(), lastChunkData.begin() + 64);
        Logger::log("LightmapFix: summary recovered from the last chunk - "
                    "64 payload bytes at the 15 MB boundary were already destroyed");
    }

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
    if (realSummary.size() > chunk0Data.size())
        realSummary.resize(chunk0Data.size());
    std::memcpy(chunk0Data.data(), realSummary.data(), realSummary.size());

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
        Logger::log(std::format("LightmapFix: merge failed; {} left as it was.", path));
        return false;
    }

    // The original stays on disk until the replacement is installed.
    const std::string bakPath = std::string(path) + ".bak";
    remove(bakPath.c_str());
    if (rename(path, bakPath.c_str()) != 0)
    {
        remove(tmpPath.c_str());
        Logger::log(std::format("LightmapFix: cannot move {} aside; left as it was.", path));
        return false;
    }
    if (rename(tmpPath.c_str(), path) != 0)
    {
        rename(bakPath.c_str(), path);
        remove(tmpPath.c_str());
        Logger::log(std::format("LightmapFix: cannot install {}; original restored.", path));
        return false;
    }
    remove(bakPath.c_str());

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

void LightmapFix::RepairSavedMap(const char* mapsEdPath)
{
    if (!mapsEdPath || !*mapsEdPath)
        return;

    try
    {
        FixSDCFile(mapsEdPath);

        std::string mapsPath = MapsEdToMapsPath(mapsEdPath);
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

    // A capture belongs to the save that made it, spilled or not.
    ClearSummaryCaptures();
}

static void __cdecl RunFixIfNeeded()
{
    LightmapFix::RepairSavedMap(s_savedFilename);
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
        if (!MemoryWriter::WriteBytes(0x1119ebe9u, jl_patch, sizeof(jl_patch)))
            Logger::log("LightmapFix: Patch failed.");
    }
}
