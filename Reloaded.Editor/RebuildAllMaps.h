#pragma once
#include <windows.h>

class RebuildAllMaps
{
public:
    static void Initialize();

    // Gated on -UnlockPackages.
    static bool Available();

    static void Show(HWND hParent);
};
