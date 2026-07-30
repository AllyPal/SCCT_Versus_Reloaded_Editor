#include "pch.h"
#include "MapUnlock.h"
#include "MemoryWriter.h"
#include "StringOperations.h"
#include "logger.h"
#include <cstring>

// -UnlockPackages, extended to the stock maps (e.g. AQU03)
namespace {

    struct MapGuard
    {
        const char* name;
        uintptr_t   refuse;   // first byte of the refuse block, always `push <msg>`
        uintptr_t   allow;    // jump target when the stock name check fails
        uint8_t     orig[5];  // expected bytes at `refuse`
    };

    const MapGuard kMapGuards[] =
    {
        { "Save As",        0x10E3CEA6, 0x10E3CEBF, { 0x68, 0x88, 0x69, 0x46, 0x11 } },
        { "Save",           0x10E3DB2B, 0x10E3DB60, { 0x68, 0x88, 0x69, 0x46, 0x11 } },
        { "Build & Save .sds", 0x10E3E490, 0x10E3E487, { 0x68, 0x70, 0x6B, 0x46, 0x11 } },
        { "Build & Save .sdc", 0x10E3ED6D, 0x10E3EBC1, { 0x68, 0x70, 0x6B, 0x46, 0x11 } },
        { "Rebuild",        0x10E5C431, 0x10E5C2F8, { 0x68, 0x28, 0xB9, 0x46, 0x11 } },
        { "Play Map",       0x10E2121B, 0x10E2122E, { 0x68, 0x20, 0x19, 0x46, 0x11 } },
    };

    bool HasCommandLineFlag(const char* flag)
    {
        const char* cmd = GetCommandLineA();
        const size_t len = strlen(flag);
        for (const char* p = cmd; *p; ++p)
            if (_strnicmp(p, flag, len) == 0)
                return true;
        return false;
    }

}

void MapUnlock::Initialize()
{
    // Parsed here rather than reused from SoundBrowser.cpp so this does not
    // depend on Initialize() ordering in dllmain
    if (!HasCommandLineFlag("-UnlockPackages"))
        return;

    for (const MapGuard& guard : kMapGuards)
    {
        const uint8_t* site = reinterpret_cast<const uint8_t*>(guard.refuse);

        if (memcmp(site, guard.orig, sizeof(guard.orig)) != 0)
        {
            Logger::log("MapUnlock: unexpected bytes at "
                + StringOperations::toHexString(guard.refuse)
                + " (" + guard.name + ") - skipped");
            continue;
        }

        uint8_t jump[5];
        jump[0] = 0xE9; // JMP rel32
        *reinterpret_cast<int32_t*>(jump + 1) =
            static_cast<int32_t>(guard.allow - (guard.refuse + sizeof(jump)));

        MemoryWriter::WriteBytes(guard.refuse, jump, sizeof(jump));
    }
}
