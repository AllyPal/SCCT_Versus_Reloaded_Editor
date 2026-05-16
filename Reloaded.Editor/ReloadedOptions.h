#pragma once
#include <windows.h>

// Shows the "Reloaded Options" modal dialog parented to hParent.
// Applies changes and saves to ReloadedEditor.ini on OK.
void ShowReloadedOptionsDialog(HWND hParent);

class ReloadedOptions
{
public:
    // Load persisted settings from ReloadedEditor.ini into runtime variables.
    // Call once during DLL initialisation, before RealtimeFix::Initialize().
    static void Initialize();
};
