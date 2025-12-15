#pragma once
#include "logger.h"
#include "StringOperations.h"
class MemoryWriter
{
public:
    static bool __cdecl WriteBytes(uintptr_t targetAddress, const void* bytes, size_t length) {
        //debug_cout << "Writing bytes at " << StringOperations::toHexString(targetAddress) << "\n";

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), length, PAGE_READWRITE, &oldProtect)) {
            Logger::log("Failed to change memory protection");
            return false;
        }

        memcpy(reinterpret_cast<void*>(targetAddress), bytes, length);
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), length, oldProtect, &oldProtect)) {
            Logger::log("Failed to restore memory protection");
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(targetAddress), length);
        //debug_cout << "Finished writing bytes at " << StringOperations::toHexString(targetAddress) << "\n";
        return true;
    }

    static bool __cdecl WriteFunctionPtr(uintptr_t targetAddress, void(*function)()) {
        //debug_cout << "Writing function pointer at " << StringOperations::toHexString(targetAddress) << "\n";
        uintptr_t functionAddress = reinterpret_cast<uintptr_t>(function);
        uint32_t offset = static_cast<uint32_t>(functionAddress);

        uint8_t offsetBytes[4];
        *reinterpret_cast<uint32_t*>(offsetBytes) = static_cast<uint32_t>(offset);

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(offsetBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Logger::log("Failed to change memory protection");
            return false;
        }

        memcpy(reinterpret_cast<void*>(targetAddress), offsetBytes, sizeof(offsetBytes));
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(offsetBytes), oldProtect, &oldProtect)) {
            Logger::log("Failed to restore memory protection");
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(targetAddress), sizeof(offsetBytes));
        //debug_cout << "Finished writing function pointer at " << StringOperations::toHexString(targetAddress) << "\n";
        return true;
    }

    static bool __cdecl WriteJump(uintptr_t targetAddress, void(*function)()) {
        //debug_cout << "Writing jump at " + StringOperations::toHexString(targetAddress) << "\n";
        uintptr_t functionAddress = reinterpret_cast<uintptr_t>(function);
        uintptr_t relativeAddress = (functionAddress - targetAddress - 5);
        uint8_t jump[5];
        jump[0] = 0xE9; // JMP opcode
        *reinterpret_cast<uint32_t*>(jump + 1) = static_cast<uint32_t>(relativeAddress);

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(jump), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            debug_cout << "Failed to change memory protection" << "\n";
            return false;
        }

        memcpy(reinterpret_cast<void*>(targetAddress), jump, sizeof(jump));
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(jump), oldProtect, &oldProtect)) {
            debug_cout << "Failed to restore memory protection" << "\n";
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(targetAddress), sizeof(jump));
        //debug_cout << "Finished writing jump at " + StringOperations::toHexString(targetAddress) << "\n";
        return true;
    }

    static bool __cdecl WriteCall(uintptr_t targetAddress, void(*function)()) {
        //debug_cout << "Writing call at " + StringOperations::toHexString(targetAddress) << "\n";
        uintptr_t functionAddress = reinterpret_cast<uintptr_t>(function);
        uintptr_t relativeAddress = (functionAddress - targetAddress - 5);
        uint8_t jump[5];
        jump[0] = 0xE8; // CALL opcode
        *reinterpret_cast<uint32_t*>(jump + 1) = static_cast<uint32_t>(relativeAddress);

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(jump), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            debug_cout << "Failed to change memory protection" << "\n";
            return false;
        }

        memcpy(reinterpret_cast<void*>(targetAddress), jump, sizeof(jump));
        if (!VirtualProtect(reinterpret_cast<LPVOID>(targetAddress), sizeof(jump), oldProtect, &oldProtect)) {
            debug_cout << "Failed to restore memory protection" << "\n";
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(targetAddress), sizeof(jump));
        //debug_cout << "Finished writing call at " + StringOperations::toHexString(targetAddress) << "\n";
        return true;
    }
};

