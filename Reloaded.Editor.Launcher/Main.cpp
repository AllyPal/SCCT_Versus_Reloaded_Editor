#include <iostream>
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include "Inject.h"

static std::wstring GetExecutablePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    return std::wstring(buffer);
}

static std::wstring GetExecutableDirectory() {
    std::wstring executablePath = GetExecutablePath();
    std::wstring::size_type pos = std::wstring(executablePath).find_last_of(L"\\/");
    return std::wstring(executablePath).substr(0, pos);
}

static std::wstring FindExecutable(const std::wstring& exeName) {
    std::wstring currentPath = GetExecutableDirectory() + L"\\" + exeName;
    if (GetFileAttributes(currentPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return currentPath;
    }

    return L"";
}

#define Editor
//#define Dedicated

static std::wstring FindScctVersusExecutable() {
    // Enhanced file name
    std::wstring enhancedExecutableName = L"ChaosTheory_Editor";
    auto result = FindExecutable(enhancedExecutableName);
    if (!result.empty()) {
        return result;
    }

    // Default file name
    std::wstring execuatableName = L"ChaosTheory_Editor.exe";
    return FindExecutable(execuatableName);
}

std::wstring getCurrentDateTime() {
    std::time_t now = std::time(nullptr);

    std::tm localTime;

    localtime_s(&localTime, &now);

    std::wstringstream wss;
    wss << std::put_time(&localTime, L"%Y-%m-%d %H:%M:%S");

    return wss.str();
}

LPWSTR ConvertToWideString(LPSTR lpStr)
{
    int nChars = MultiByteToWideChar(CP_ACP, 0, lpStr, -1, NULL, 0);
    if (nChars == 0)
    {
        return NULL;
    }

    LPWSTR lpWideStr = new WCHAR[nChars];
    MultiByteToWideChar(CP_ACP, 0, lpStr, -1, lpWideStr, nChars);
    return lpWideStr;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    std::wofstream file(GetExecutablePath() + L".log");
    if (!file.is_open()) {
        std::wcerr << L"Failed to open log file." << std::endl;
        return 1;
    }
    // Redirect std::wcout and std::wcerr to the file stream
    std::wcout.rdbuf(file.rdbuf());
    std::wcerr.rdbuf(file.rdbuf());

    std::wcout << L"Starting at " << getCurrentDateTime() << std::endl;

    auto exePath = FindScctVersusExecutable();
    if (exePath.empty()) {
        std::wcerr << L"ChaosTheory_Editor.exe not found." << std::endl;
        MessageBox(
            NULL,
            L"ChaosTheory_Editor.exe was not found.\n\nPlace Reloaded_Editor.exe and Reloaded.Editor.dll in your game's System folder, next to ChaosTheory_Editor.exe.",
            L"Reloaded Chaos Theory Editor",
            MB_OK | MB_ICONERROR
        );
        return 1;
    }

    auto executableDirectory = GetExecutableDirectory();
    std::wstring dllPath = executableDirectory + L"\\Reloaded.Editor.dll";
    if (!std::filesystem::exists(dllPath)) {
        std::wcerr << L"Reloaded.Editor.dll not found at " << dllPath << std::endl;
        MessageBox(
            NULL,
            L"Reloaded.Editor.dll was not found.\n\nCopy it into your game's System folder, next to Reloaded_Editor.exe.",
            L"Reloaded Chaos Theory Editor",
            MB_OK | MB_ICONERROR
        );
        return 1;
    }

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    std::wstring commandLine(ConvertToWideString(lpCmdLine));
    std::wstring cmdLine = std::format(L"\"{}\" {}", exePath, commandLine);
    LPWSTR cmdLineA = cmdLine.data();
    if (!CreateProcess(exePath.c_str(), cmdLineA, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        std::wcerr << L"Failed to start process." << std::endl;
        std::cin.get();
        return 1;
    }

    std::wcout << L"Suspended process created successfully." << std::endl;
    if (!Inject::InjectDLL(pi.hProcess, dllPath)) {
        std::wcerr << L"Failed to inject DLL. Terminating process" << std::endl;
        TerminateProcess(pi.hProcess, 1);
        std::cin.get();
        return 1;
    }
    std::wcout << L"Code injected successfully." << std::endl;

    ResumeThread(pi.hThread);
    std::wcout << L"Process resumed." << std::endl;

    /*std::this_thread::sleep_for(std::chrono::seconds(1));
    TerminateProcess(pi.hProcess, 1);*/

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    file.close();

    return 0;
}

