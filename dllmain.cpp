// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <Windows.h>
#include "logger.h"
#include "Rendering.h"
#include "Debug.h"
#include "UI.h"
#include "General.h"
#include "Shadows.h"

INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT;


std::wstring GetDllPath(HINSTANCE hModule) {
    std::vector<wchar_t> pathBuffer(MAX_PATH);
    const DWORD result = GetModuleFileName(hModule, pathBuffer.data(), pathBuffer.size());
    if (result == 0) {
        return L"";
    }

    pathBuffer.resize(result);
    return std::wstring(pathBuffer.begin(), pathBuffer.end());
}

static std::wstring GetExecutableDirectory(std::wstring executablePath) {
    std::wstring::size_type pos = std::wstring(executablePath).find_last_of(L"\\/");
    return std::wstring(executablePath).substr(0, pos);
}

void RedirectToConsole()
{
#ifdef _DEBUG
    AllocConsole();
#else
    return;
#endif
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << "SCCT Versus Reloaded injected successfully" << "\n";
}

void LogStackTrace(std::wofstream& logFile, DWORD* stackPointer) {
    logFile << "Stack Trace:\n";
    for (int i = 0; i < 40; ++i) {
        DWORD returnAddress = 0;
        if (IsBadReadPtr(stackPointer + i, sizeof(DWORD))) break;

        returnAddress = stackPointer[i];
        logFile << std::format(L"Stack [{}]: 0x{:08X}\n", i, returnAddress);
    }
    logFile << "\n";
}

void LogException(EXCEPTION_POINTERS* exceptionInfo) {
    std::wofstream logFile("SCCT_Versus_Editor_crash.log", std::ios::trunc);

    if (logFile.is_open()) {
        logFile << "Exception Code: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionCode << '\n';
        logFile << "Exception Address: 0x" << exceptionInfo->ExceptionRecord->ExceptionAddress << '\n';

        // Log register values
        CONTEXT* context = exceptionInfo->ContextRecord;
        logFile << "Registers:\n";
        logFile << "EIP: 0x" << context->Eip << '\n';
        logFile << "ESP: 0x" << context->Esp << '\n';
        logFile << "EBP: 0x" << context->Ebp << '\n';
        logFile << "EAX: 0x" << context->Eax << '\n';
        logFile << "EBX: 0x" << context->Ebx << '\n';
        logFile << "ECX: 0x" << context->Ecx << '\n';
        logFile << "EDX: 0x" << context->Edx << '\n';

        LogStackTrace(logFile, reinterpret_cast<DWORD*>(context->Esp));

        logFile << "End of exception details\n\n";
        logFile.close();
    }
}

// Custom unhandled exception filter
LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    LogException(exceptionInfo);
    // Optionally, you can terminate or continue the process.
    return EXCEPTION_EXECUTE_HANDLER;
}

const int BaseAddress = 0x10900000;
BOOL CALLBACK InitFunction(PINIT_ONCE InitOnce, PVOID Parameter, PVOID* Context) {
    HMODULE hModule = static_cast<HMODULE>(Parameter);
    auto dllPath = GetDllPath(hModule);
    auto directoryPath = GetExecutableDirectory(dllPath);

    RedirectToConsole();

    Logger::Initialize(dllPath);
    Logger::log("");
    
    Rendering::Initialize();
    UI::Initialize();
    General::Initialize();
    Shadows::Initialize();

#ifdef _DEBUG
    Debug::Initialize();
#endif

    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);

    return TRUE;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
            InitOnceExecuteOnce(&g_InitOnce, InitFunction, hModule, NULL);
            break;
    }
    return TRUE;
}
