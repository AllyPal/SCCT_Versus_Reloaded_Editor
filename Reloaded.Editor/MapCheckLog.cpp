#include "pch.h"
#include "MapCheckLog.h"
#include "Hooks.h"
#include "logger.h"
#include <cstdio>

INIT_HOOKS;

static MapCheckLog::Sink s_sink;

static void __cdecl RecordEntry(const char* message)
{
    if (!message || !*message)
        return;

    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "Map Check: %s", message);

    Logger::log(line);
    if (s_sink)
        s_sink(line);
}

// FFeedbackContextWindows::MapCheck_Add(int type, AActor* actor, const char* message).
// Bypasses GLog entirely, so EngineLog cannot see these.
JMP_HOOK(0x10e3a520, MapCheckAddHook)
{
    static int Resume = 0x10e3a525;
    __asm {
        pushad                          // [esp+0x20] retaddr, +0x24 type, +0x28 actor, +0x2c message
        mov  eax, dword ptr [esp + 0x2c]
        push eax
        call RecordEntry
        add  esp, 4
        popad

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [Resume]
    }
}

// Tail of Build All, where the editor raises the dialog for whatever the build found.
JMP_HOOK(0x10e10f24, BuildMapCheckHook)
{
    static int Resume = 0x10e10f29;
    static int Skip   = 0x10e10f3b;     // epilogue, past the MapCheck_Show call
    __asm {
        cmp  dword ptr [s_sink], 0
        jne  skip
        mov  eax, dword ptr ds:[0x11818310]
        jmp  dword ptr [Resume]
    skip:
        jmp  dword ptr [Skip]
    }
}

void MapCheckLog::Initialize()
{
    INSTALL_HOOKS;
}

void MapCheckLog::SetSink(Sink sink)
{
    s_sink = sink;
}
