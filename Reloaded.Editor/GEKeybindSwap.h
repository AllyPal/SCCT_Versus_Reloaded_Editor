#pragma once
#include <cstdint>

extern uint8_t g_KeyLedgeGrab;
extern uint8_t g_KeyHandOverHand;
extern uint8_t g_KeyPipe;
extern uint8_t g_KeyLadder;
extern uint8_t g_KeyZipline;
extern uint8_t g_KeyFence;

class GEKeybindSwap
{
public:
    static void ApplyGEKeybinds();
    static void Initialize();
};
