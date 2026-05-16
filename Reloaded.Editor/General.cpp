#include "pch.h"
#include "General.h"
#include "Hooks.h"
#include "MemoryWriter.h"
#include <mimalloc.h>

INIT_HOOKS;

JMP_HOOK(0x110518AD, RemoveAudioSizeLimit) {
    static int Return = 0x110518B3;
    __asm {
        jmp dword ptr[Return]
    }
}

// Memory redirection addresses to be provided by the user
namespace MemoryAddresses {
    uintptr_t malloc_addr = 0x11AF2114;
    uintptr_t free_addr = 0x11AF209C;
    uintptr_t realloc_addr = 0x11AF21F0;
    uintptr_t calloc_addr = 0x11AF2098;
    uintptr_t strdup_addr = 0x11AF21C0;
}

void InstallMemoryHooks() {
    uintptr_t fn_ptr;

    fn_ptr = reinterpret_cast<uintptr_t>(mi_malloc);
    MemoryWriter::WriteBytes(MemoryAddresses::malloc_addr, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_free);
    MemoryWriter::WriteBytes(MemoryAddresses::free_addr, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_realloc);
    MemoryWriter::WriteBytes(MemoryAddresses::realloc_addr, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_calloc);
    MemoryWriter::WriteBytes(MemoryAddresses::calloc_addr, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_strdup);
    MemoryWriter::WriteBytes(MemoryAddresses::strdup_addr, &fn_ptr, sizeof(fn_ptr));
}

void General::Initialize()
{
    INSTALL_HOOKS;
    InstallMemoryHooks();
}