#include "pch.h"
#include "BrowserOpenDir.h"
#include "MemoryWriter.h"
#include "logger.h"
#include <windows.h>
#include <commdlg.h>
#include <cstring>

#define IAT_GETOPENFILENAMEA   0x11AF2644u

typedef BOOL (WINAPI *GetOpenFileNameA_t)(LPOPENFILENAMEA);
static GetOpenFileNameA_t g_origGetOpenFileNameA = nullptr;

static const char* DefaultDirForFilter(LPCSTR filter)
{
    if (!filter) return nullptr;

    const char* p = filter;
    int   pairCount = 0;
    while (*p && pairCount < 64 && (p - filter) < 4096)
    {
        if (std::strstr(p, "*.ukx"))    return "..\\Packages\\Animations";
        if (std::strstr(p, "Map Files")) return "..\\Packages\\MapsEd";
        if (std::strstr(p, "*.uax"))    return "..\\Packages\\Sounds";
        if (std::strstr(p, "*.usx"))    return "..\\Packages\\StaticMeshes";
        if (std::strstr(p, "*.utx"))    return "..\\Packages\\Textures";

        p += std::strlen(p) + 1;
        ++pairCount;
    }
    return nullptr;
}

static BOOL WINAPI GetOpenFileNameA_Hook(LPOPENFILENAMEA ofn)
{
    if (ofn)
    {
        const char* defaultDir = DefaultDirForFilter(ofn->lpstrFilter);
        if (defaultDir)
        {
            ofn->lpstrInitialDir = defaultDir;
        }
    }
    return g_origGetOpenFileNameA(ofn);
}

void BrowserOpenDir::Initialize()
{
    g_origGetOpenFileNameA = *reinterpret_cast<GetOpenFileNameA_t*>(
        static_cast<uintptr_t>(IAT_GETOPENFILENAMEA));

    if (!g_origGetOpenFileNameA)
    {
        Logger::log("BrowserOpenDir: IAT slot was null, hook not installed");
        return;
    }

    MemoryWriter::WriteFunctionPtr(IAT_GETOPENFILENAMEA,
        reinterpret_cast<void(*)()>(&GetOpenFileNameA_Hook));
}
