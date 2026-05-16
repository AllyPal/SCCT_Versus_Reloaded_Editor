#pragma once
#include <cstdint>

// Runtime-configurable GE keybind letters (uppercase A–Z ASCII).
// Defaults match the compile-time values in GEKeybindSwap.cpp.
// Written by ReloadedOptions::Initialize() (INI load) and by the dialog's OK
// handler; applied to the binary by GEKeybindSwap::ApplyGEKeybinds().
// Pole is intentionally excluded — it is an unused GE type.
extern uint8_t g_KeyLedgeGrab;
extern uint8_t g_KeyHandOverHand;
extern uint8_t g_KeyPipe;
extern uint8_t g_KeyLadder;
extern uint8_t g_KeyZipline;
extern uint8_t g_KeyFence;

class GEKeybindSwap
{
public:
    // Patch all GE binary sites using the current g_Key* values.
    // Called at Initialize() and again after the Reloaded Options dialog OK.
    static void ApplyGEKeybinds();

    // Entry point: called once from DllMain after ReloadedOptions::Initialize()
    // has loaded INI values into the g_Key* globals.
    static void Initialize();
};
