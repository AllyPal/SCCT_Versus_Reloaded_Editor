#pragma once
#include <vector>
#include <utility>
#include "MemoryWriter.h"

using HookPair = std::pair<uint32_t, void(*)()>;
#define INIT_HOOKS static std::vector<HookPair> jmpHooks; \
static std::vector<HookPair> callHooks;\
static std::vector<HookPair> functionPointerHooks;

#define JMP_HOOK(address, name) \
    void name(); \
    static struct name##_Entry { \
        name##_Entry() { \
            jmpHooks.emplace_back(address, name); \
        } \
    } name##_register; \
    __declspec(naked) void name()

#define CALL_HOOK(address, name) \
    void name(); \
    static struct name##_Entry { \
        name##_Entry() { \
            callHooks.emplace_back(address, name); \
        } \
    } name##_register; \
    __declspec(naked) void name()

#define FNC_HOOK(address, name) \
    void name(); \
    static struct name##_Entry { \
        name##_Entry() { \
            functionPointerHooks.emplace_back(address, name); \
        } \
    } name##_register; \
    __declspec(naked) void name()

#define INSTALL_HOOKS \
    for (const auto& [addr, func] : jmpHooks) {\
        MemoryWriter::WriteJump(addr, func);\
    }\
    for (const auto& [addr, func] : callHooks) {\
        MemoryWriter::WriteCall(addr, func);\
    }\
    for (const auto& [addr, func] : functionPointerHooks) {\
        MemoryWriter::WriteFunctionPtr(addr, func);\
    }