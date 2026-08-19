#include "pch.h"
#include "RebuildAllMaps.h"
#include "logger.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------
//  Editor internals
// ---------------------------------------------------------------------
#define GEDITOR_GLOBAL      0x1165dfa0u
#define EXEC_LOG_DEV        0x115befb0u   // GLog/GNull output device ptr
#define EDITOR_LEVEL_OFFSET 0x130u

// The editor's own single-map rebuild recipe; the bracket pair must wrap every
// build - it stashes and restores actor state the build would otherwise lose.
const uint32_t kPrepareForBuild   = 0x10E06A1A;  // __thiscall(GEditor, FArray*, FArray*)
const uint32_t kRestoreAfterBuild = 0x10E02EEC;  // __thiscall(GEditor, FArray*, FArray*)
const uint32_t kBuildAll          = 0x10E04DD2;  // geometry/BSP/lighting/paths, per Build Options
const uint32_t kSaveMap           = 0x10E0416B;  // __thiscall(GEditor, path) -> UBOOL

// UE2 FArray header; PrepareForBuild frees the previous map's contents, so one
// pair serves the whole run.
struct FArrayHeader { void* data; int num; int max; };
FArrayHeader g_buildStateA;
FArrayHeader g_buildStateB;

// GEditor+0x28 = FExec sub-object; vtable[0] = FExec::Exec (2-arg, callee-cleanup).
// Its return only says a topic claimed the command, so nothing here reads it.
void __cdecl ExecEditorCommand(const char* cmd)
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    if (!gEditor) return;
    void* fexec   = static_cast<char*>(gEditor) + 0x28;
    void* vtable  = *reinterpret_cast<void**>(fexec);
    if (!vtable) return;
    void* execFn  = *reinterpret_cast<void**>(vtable);
    void* logDev  = *reinterpret_cast<void**>(EXEC_LOG_DEV);
    __asm {
        push logDev
        push cmd
        mov  ecx, fexec
        mov  eax, execFn
        call eax
    }
}

void __cdecl CallBuildBracket(uint32_t fn)
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    if (!gEditor) return;
    void* first  = &g_buildStateA;
    void* second = &g_buildStateB;
    __asm {
        push second
        push first
        mov  ecx, gEditor
        mov  eax, fn
        call eax
    }
}

void __cdecl BuildLoadedMap()
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    if (!gEditor) return;
    const uint32_t fn = kBuildAll;
    __asm {
        mov  ecx, gEditor
        mov  eax, fn
        call eax
    }
}

int __cdecl SaveLoadedMap(const char* path)
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    if (!gEditor) return 0;
    const uint32_t fn = kSaveMap;
    int  saved = 0;
    __asm {
        push path
        mov  ecx, gEditor
        mov  eax, fn
        call eax
        mov  saved, eax
    }
    return saved;
}

void* EditorLevel()
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    return gEditor ? *reinterpret_cast<void**>(static_cast<char*>(gEditor) + EDITOR_LEVEL_OFFSET)
                   : nullptr;
}

// UObject and GNames offsets as used by SoundBrowser and AnimationBrowser.
#define UOBJ_OUTER_OFFSET       0x18
#define UOBJ_FNAME_OFFSET       0x20
#define GNAMES_DATA             0x1169cfbcu
#define GNAMES_NUM              0x1169cfc0u
#define FNAME_ENTRY_STR_OFFSET  0x0Cu

const char* LoadedLevelPackage()
{
    void* level = EditorLevel();
    if (!level) return nullptr;
    void* outer = *reinterpret_cast<void**>(static_cast<char*>(level) + UOBJ_OUTER_OFFSET);
    if (!outer) return nullptr;

    void** namesData = *reinterpret_cast<void***>(GNAMES_DATA);
    const int namesNum = *reinterpret_cast<int*>(GNAMES_NUM);
    if (!namesData || namesNum <= 0) return nullptr;

    const int index = *reinterpret_cast<int*>(static_cast<char*>(outer) + UOBJ_FNAME_OFFSET);
    if (index < 0 || index >= namesNum) return nullptr;
    void* entry = namesData[index];
    return entry ? static_cast<char*>(entry) + FNAME_ENTRY_STR_OFFSET : nullptr;
}

// ---------------------------------------------------------------------
//  Load verification
// ---------------------------------------------------------------------
// MAP LOAD reports success even when it leaves the previous level in place - the
// save would then overwrite this map with that level.  A probe only counts once
// it has identified a map, so one that never matches leaves loads unverified.
struct LoadProbe
{
    char package[128];
    char caption[256];
};

bool g_probeTrusted;

bool ContainsNoCase(const char* haystack, const char* needle)
{
    const size_t len = strlen(needle);
    if (!len) return false;
    for (const char* p = haystack; *p; ++p)
        if (_strnicmp(p, needle, len) == 0)
            return true;
    return false;
}

void CaptureProbe(HWND frame, LoadProbe* probe)
{
    probe->package[0] = '\0';
    probe->caption[0] = '\0';

    const char* package = LoadedLevelPackage();
    if (package)
        strncpy_s(probe->package, package, _TRUNCATE);
    if (frame)
        GetWindowTextA(frame, probe->caption, sizeof(probe->caption));
}

bool ProbeShowsMap(const LoadProbe& probe, const char* base)
{
    return _stricmp(probe.package, base) == 0 || ContainsNoCase(probe.caption, base);
}

bool ProbeUnchanged(const LoadProbe& before, const LoadProbe& after)
{
    return strcmp(before.package, after.package) == 0
        && strcmp(before.caption, after.caption) == 0;
}

// ---------------------------------------------------------------------
//  Map enumeration
// ---------------------------------------------------------------------
// Same directory the editor's own batch command globs; relative to System\.
const char kMapsDir[] = "..\\Packages\\MapsEd\\";
const char kTitle[]   = "Rebuild All Maps";

std::vector<std::string> FindMaps()
{
    std::vector<std::string> maps;
    WIN32_FIND_DATAA find = {};
    HANDLE h = FindFirstFileA((std::string(kMapsDir) + "*.sdc").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE)
        return maps;

    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            maps.push_back(find.cFileName);
    } while (FindNextFileA(h, &find));

    FindClose(h);
    return maps;
}

std::string BaseName(const std::string& fileName)
{
    const size_t dot = fileName.find_last_of('.');
    return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

bool CanOpenForWrite(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------
//  Progress window
// ---------------------------------------------------------------------
const char kWndClassName[] = "ReloadedRebuildAllMaps";
const int  IDC_STOP        = 1;
const int  kButtonWidth    = 90;
const int  kButtonHeight   = 23;
const int  kMargin         = 8;

HWND g_hWnd;
HWND g_hFeed;
HWND g_hStop;
bool g_running;
bool g_cancelled;

void LayoutChildren(HWND hWnd)
{
    if (!g_hFeed || !g_hStop)
        return;

    RECT rc;
    GetClientRect(hWnd, &rc);
    const int feedHeight = rc.bottom - kButtonHeight - kMargin * 3;
    MoveWindow(g_hFeed, kMargin, kMargin,
               rc.right - kMargin * 2, feedHeight > 0 ? feedHeight : 0, TRUE);
    MoveWindow(g_hStop, rc.right - kMargin - kButtonWidth,
               rc.bottom - kMargin - kButtonHeight, kButtonWidth, kButtonHeight, TRUE);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        LayoutChildren(hWnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STOP)
        {
            if (g_running)
            {
                g_cancelled = true;
                EnableWindow(g_hStop, FALSE);
            }
            else
            {
                DestroyWindow(hWnd);
            }
        }
        return 0;

    // Mid-run this only requests a stop - the batch owns the window until it ends.
    case WM_CLOSE:
        if (g_running)
            g_cancelled = true;
        else
            DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_hWnd  = nullptr;
        g_hFeed = nullptr;
        g_hStop = nullptr;
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool EnsureWndClassRegistered()
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSEXA existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExA(hInst, kWndClassName, &existing))
        return true;

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWndClassName;
    return RegisterClassExA(&wc) != 0;
}

bool CreateProgressWindow(HWND hParent)
{
    if (!EnsureWndClassRegistered())
        return false;

    const int width  = 560;
    const int height = 420;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT pr;
    if (hParent && GetWindowRect(hParent, &pr))
    {
        x = pr.left + ((pr.right - pr.left) - width) / 2;
        y = pr.top + ((pr.bottom - pr.top) - height) / 2;
    }

    HINSTANCE hInst = GetModuleHandleA(nullptr);
    g_hWnd = CreateWindowExA(0, kWndClassName, kTitle,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             x, y, width, height, hParent, nullptr, hInst, nullptr);
    if (!g_hWnd)
        return false;

    g_hFeed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE
                              | ES_READONLY | ES_AUTOVSCROLL,
                              0, 0, 0, 0, g_hWnd, nullptr, hInst, nullptr);
    g_hStop = CreateWindowExA(0, "BUTTON", "Stop",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              0, 0, 0, 0, g_hWnd,
                              reinterpret_cast<HMENU>(IDC_STOP), hInst, nullptr);
    if (!g_hFeed || !g_hStop)
    {
        DestroyWindow(g_hWnd);
        return false;
    }

    HGDIOBJ font = GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(g_hFeed, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageA(g_hStop, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageA(g_hFeed, EM_SETLIMITTEXT, 0, 0);

    LayoutChildren(g_hWnd);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);
    return true;
}

// The batch runs on the editor's UI thread; without pumping, nothing repaints
// and Stop never arrives.
void PumpUI()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(msg.wParam));
            g_cancelled = true;
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// Detail that belongs in the run's log but would clutter the feed.
void __cdecl LogLine(const char* text)
{
    Logger::log(std::string("RebuildAllMaps: ") + text);
}

void __cdecl AppendLine(const char* text)
{
    if (g_hFeed)
    {
        std::string line(text);
        line += "\r\n";
        const int end = GetWindowTextLengthA(g_hFeed);
        SendMessageA(g_hFeed, EM_SETSEL, end, end);
        SendMessageA(g_hFeed, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageA(g_hFeed, EM_SCROLLCARET, 0, 0);
    }
    if (*text)
        LogLine(text);
    PumpUI();
}

// ---------------------------------------------------------------------
//  Per-map work
// ---------------------------------------------------------------------
enum MapResult { Map_Ok, Map_LoadFailed, Map_SaveFailed, Map_Crashed };

// Locals must stay POD - __try forbids anything that needs unwinding.
MapResult ProcessMap(HWND frame, const char* path, const char* fileName, const char* base)
{
    char cmd[MAX_PATH + 64];
    char note[MAX_PATH + 64];
    LoadProbe before, after;

    __try
    {
        CaptureProbe(frame, &before);

        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "MAP LOAD FILE=\"%s\"", path);
        ExecEditorCommand(cmd);
        if (!EditorLevel())
            return Map_LoadFailed;

        CaptureProbe(frame, &after);
        _snprintf_s(note, sizeof(note), _TRUNCATE,
                    "probe after load: package=\"%s\" caption=\"%s\"", after.package, after.caption);
        LogLine(note);

        if (ProbeShowsMap(after, base))
            g_probeTrusted = true;
        else if (g_probeTrusted && ProbeUnchanged(before, after))
            return Map_LoadFailed;

        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "LOADMAPPROP MAP=\"%s\"", fileName);
        ExecEditorCommand(cmd);

        _snprintf_s(note, sizeof(note), _TRUNCATE, "Rebuilding: %s", base);
        AppendLine(note);

        CallBuildBracket(kPrepareForBuild);
        BuildLoadedMap();
        CallBuildBracket(kRestoreAfterBuild);

        ExecEditorCommand("BRUSHCLIP DELETE");
        ExecEditorCommand("POLYGON DELETE");
        ExecEditorCommand("REMOVEALLREF");

        _snprintf_s(note, sizeof(note), _TRUNCATE, "Saving: %s", base);
        AppendLine(note);

        if (!SaveLoadedMap(path))
            return Map_SaveFailed;

        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "SAVEMAPPROP MAP=\"%s\"", fileName);
        ExecEditorCommand(cmd);
        return Map_Ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return Map_Crashed;
    }
}

void RunBatch(const std::vector<std::string>& maps, HWND frame)
{
    g_running      = true;
    g_cancelled    = false;
    g_probeTrusted = false;
    EnableWindow(frame, FALSE);

    const int total = static_cast<int>(maps.size());
    int rebuilt = 0, failed = 0;
    bool crashed = false;

    for (int i = 0; i < total && !g_cancelled; ++i)
    {
        const std::string base = BaseName(maps[i]);
        const std::string path = std::string(kMapsDir) + maps[i];

        char header[64];
        _snprintf_s(header, sizeof(header), _TRUNCATE, "%d/%d", i + 1, total);
        if (i > 0)
            AppendLine("");
        AppendLine(header);
        AppendLine(("Loading: " + base).c_str());

        if (!CanOpenForWrite(path.c_str()))
        {
            AppendLine(("ERROR: " + base + " is read-only or locked - skipped").c_str());
            ++failed;
            continue;
        }

        const MapResult result = ProcessMap(frame, path.c_str(), maps[i].c_str(), base.c_str());
        if (result == Map_Ok)
        {
            ++rebuilt;
        }
        else if (result == Map_Crashed)
        {
            AppendLine(("ERROR: " + base + " crashed the rebuild - stopping").c_str());
            AppendLine("Restart the editor before rebuilding anything else.");
            ++failed;
            crashed = true;
            break;
        }
        else
        {
            AppendLine((std::string("ERROR: Failed to ")
                        + (result == Map_LoadFailed ? "load " : "save ") + base).c_str());
            ++failed;
        }
    }

    const char* outcome = crashed ? "Stopped" : g_cancelled ? "Cancelled" : "Finished";
    char summary[160];
    _snprintf_s(summary, sizeof(summary), _TRUNCATE, "%s %d/%d - %d rebuilt, %d failed.",
                outcome, rebuilt + failed, total, rebuilt, failed);
    AppendLine("");
    AppendLine(summary);
    if (!g_probeTrusted)
        AppendLine("Note: could not confirm which map was open, so failed loads may be unreported.");

    EnableWindow(frame, TRUE);
    g_running = false;

    if (g_hStop)
    {
        SetWindowTextA(g_hStop, "Close");
        EnableWindow(g_hStop, TRUE);
    }
}

bool HasCommandLineFlag(const char* flag)
{
    const char* cmd = GetCommandLineA();
    const size_t len = strlen(flag);
    for (const char* p = cmd; *p; ++p)
        if (_strnicmp(p, flag, len) == 0)
            return true;
    return false;
}

bool g_unlocked;

} // namespace

void RebuildAllMaps::Initialize()
{
    g_unlocked = HasCommandLineFlag("-UnlockPackages");
}

bool RebuildAllMaps::Available()
{
    return g_unlocked;
}

void RebuildAllMaps::Show(HWND hParent)
{
    if (!g_unlocked)
        return;

    if (g_running)
    {
        if (g_hWnd)
            SetForegroundWindow(g_hWnd);
        return;
    }

    const std::vector<std::string> maps = FindMaps();
    if (maps.empty())
    {
        MessageBoxA(hParent, "No .sdc maps found in Packages\\MapsEd.",
                    kTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    char prompt[640];
    _snprintf_s(prompt, sizeof(prompt), _TRUNCATE,
        "Rebuild and save all %d maps in Packages\\MapsEd?\n\n"
        "Each map is loaded, rebuilt using your current Build Options, and saved over "
        "itself. Expect this to run for hours at the Reloaded lightmap resolution, and "
        "the editor cannot be used until it finishes.\n\n"
        "Back up Packages\\MapsEd before continuing.",
        static_cast<int>(maps.size()));

    if (MessageBoxA(hParent, prompt, kTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    if (g_hWnd)
        DestroyWindow(g_hWnd);
    if (!CreateProgressWindow(hParent))
        return;

    RunBatch(maps, hParent);
}
