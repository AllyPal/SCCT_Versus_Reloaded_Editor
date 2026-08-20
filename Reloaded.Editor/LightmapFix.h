#pragma once
#include <cstdint>

class LightmapFix
{
public:
    static void Initialize();

    // The Save hooks are call-site patches, so any other .sdc write must call this itself.
    static void RepairSavedMap(const char* mapsEdPath);

    // Stream offset where the pre-fix writer destroyed 64 bytes of map data, or 0.
    static uint32_t ScanForDamage(const char* path);

    // Where the warning for a damaged map goes; without a sink it is a message box.
    typedef void (__cdecl *DamageSink)(const char* message);
    static void SetDamageSink(DamageSink sink);
};
