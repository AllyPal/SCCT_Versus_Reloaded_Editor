#include "pch.h"
#include "RebuildAllMaps.h"
#include "logger.h"
#include "LightmapFix.h"
#include "EngineLog.h"
#include "MapCheckLog.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
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

const char* NameFromIndex(int index)
{
    void** namesData = *reinterpret_cast<void***>(GNAMES_DATA);
    const int namesNum = *reinterpret_cast<int*>(GNAMES_NUM);
    if (!namesData || index < 0 || index >= namesNum)
        return nullptr;

    void* entry = namesData[index];
    return entry ? static_cast<char*>(entry) + FNAME_ENTRY_STR_OFFSET : nullptr;
}

const char* LoadedLevelPackage()
{
    void* level = EditorLevel();
    if (!level) return nullptr;
    void* outer = *reinterpret_cast<void**>(static_cast<char*>(level) + UOBJ_OUTER_OFFSET);
    if (!outer) return nullptr;

    return NameFromIndex(*reinterpret_cast<int*>(static_cast<char*>(outer) + UOBJ_FNAME_OFFSET));
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
//  A build holds the editor's UI thread for minutes, so the window runs on its own
//  thread; pumping from inside the build would re-enter editor code mid-rebuild.
const char kWndClassName[] = "ReloadedRebuildAllMaps";
const UINT WM_FEED_LINE  = WM_APP + 1;   // lParam: strdup'd line, freed here
const UINT WM_FEED_START = WM_APP + 2;
const UINT WM_FEED_DONE  = WM_APP + 3;
const int  IDC_STOP      = 1;
const int  kButtonWidth  = 90;
const int  kButtonHeight = 23;
const int  kMargin       = 8;

HWND         g_hWnd;
HWND         g_hFeed;
HWND         g_hStop;
HANDLE       g_readyEvent;
RECT         g_anchor;          // frame rect to centre on, captured before hiding
volatile LONG g_cancelled;
volatile LONG g_running;

void AppendToFeed(const char* text)
{
    if (!g_hFeed)
        return;

    std::string line(text);
    line += "\r\n";
    const int end = GetWindowTextLengthA(g_hFeed);
    SendMessageA(g_hFeed, EM_SETSEL, end, end);
    SendMessageA(g_hFeed, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageA(g_hFeed, EM_SCROLLCARET, 0, 0);
}

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

void RequestStop()
{
    InterlockedExchange(&g_cancelled, 1);
    SetWindowTextA(g_hStop, "Stopping");
    EnableWindow(g_hStop, FALSE);
    AppendToFeed("Stop requested - finishing the current map first.");
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        LayoutChildren(hWnd);
        return 0;

    case WM_FEED_LINE:
    {
        char* text = reinterpret_cast<char*>(lParam);
        AppendToFeed(text);
        free(text);
        return 0;
    }

    case WM_FEED_START:
        SetWindowTextA(g_hFeed, "");
        SetWindowTextA(g_hStop, "Stop");
        EnableWindow(g_hStop, TRUE);
        return 0;

    case WM_FEED_DONE:
        SetWindowTextA(g_hStop, "Close");
        EnableWindow(g_hStop, TRUE);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STOP)
        {
            if (g_running)
                RequestStop();
            else
                DestroyWindow(hWnd);
        }
        return 0;

    // Mid-run this only requests a stop - the batch owns the window until it ends.
    case WM_CLOSE:
        if (g_running)
            RequestStop();
        else
            DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_hWnd  = nullptr;
        g_hFeed = nullptr;
        g_hStop = nullptr;
        PostQuitMessage(0);
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

bool CreateProgressWindow()
{
    if (!EnsureWndClassRegistered())
        return false;

    const int width  = 560;
    const int height = 420;
    const int x = g_anchor.left + ((g_anchor.right - g_anchor.left) - width) / 2;
    const int y = g_anchor.top + ((g_anchor.bottom - g_anchor.top) - height) / 2;

    // No owner: cross-thread ownership would tie this window's input to the frozen frame.
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    g_hWnd = CreateWindowExA(0, kWndClassName, kTitle,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             x, y, width, height, nullptr, nullptr, hInst, nullptr);
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

DWORD WINAPI ProgressThread(LPVOID)
{
    const bool created = CreateProgressWindow();
    SetEvent(g_readyEvent);
    if (!created)
        return 0;

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

bool ShowProgressWindow(HWND frame)
{
    if (g_hWnd)
    {
        PostMessageA(g_hWnd, WM_FEED_START, 0, 0);
        return true;
    }

    if (!frame || !GetWindowRect(frame, &g_anchor))
        SetRect(&g_anchor, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    g_readyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_readyEvent)
        return false;

    HANDLE thread = CreateThread(nullptr, 0, ProgressThread, nullptr, 0, nullptr);
    if (thread)
    {
        WaitForSingleObject(g_readyEvent, 5000);
        CloseHandle(thread);
    }
    CloseHandle(g_readyEvent);
    g_readyEvent = nullptr;
    return g_hWnd != nullptr;
}

// Detail that belongs in the run's log but would clutter the feed.
void __cdecl LogLine(const char* text)
{
    Logger::log(std::string("RebuildAllMaps: ") + text);
}

void __cdecl AppendLine(const char* text)
{
    if (g_hWnd)
    {
        char* copy = _strdup(text);
        if (copy && !PostMessageA(g_hWnd, WM_FEED_LINE, 0, reinterpret_cast<LPARAM>(copy)))
            free(copy);
    }
    if (*text)
        LogLine(text);
}

void RouteToFeed(void (__cdecl *sink)(const char*))
{
    EngineLog::SetSink(sink);
    MapCheckLog::SetSink(sink);
    LightmapFix::SetDamageSink(sink);
}

std::string ElapsedText(DWORD ms)
{
    const unsigned secs = ms / 1000;
    char text[64];
    if (secs >= 3600)
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%uh %02um %02us", secs / 3600, (secs / 60) % 60, secs % 60);
    else if (secs >= 60)
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%um %02us", secs / 60, secs % 60);
    else
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%us", secs);
    return text;
}

void ListMaps(const std::vector<std::string>& names, const char* what)
{
    if (names.empty())
        return;

    char heading[160];
    _snprintf_s(heading, sizeof(heading), _TRUNCATE, "%d map%s %s:",
                static_cast<int>(names.size()), names.size() == 1 ? "" : "s", what);

    AppendLine("");
    AppendLine(heading);
    for (const std::string& name : names)
        AppendLine(("  " + name).c_str());
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

        LightmapFix::RepairSavedMap(path);

        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "SAVEMAPPROP MAP=\"%s\"", fileName);
        ExecEditorCommand(cmd);
        return Map_Ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return Map_Crashed;
    }
}

struct MapJob
{
    std::string fileName;
    bool        damaged;
};

// Damaged maps go first: a fault in one ends the run outright, so front-loading them settles
// their fate before the sound maps, which a later pass can still pick up.
std::vector<MapJob> PlanRun(const std::vector<std::string>& maps)
{
    std::vector<MapJob> damaged, sound;
    for (const std::string& name : maps)
    {
        const bool isDamaged = LightmapFix::ScanForDamage((std::string(kMapsDir) + name).c_str()) != 0;
        (isDamaged ? damaged : sound).push_back(MapJob{ name, isDamaged });
    }
    damaged.insert(damaged.end(), sound.begin(), sound.end());
    return damaged;
}

void RunBatch(const std::vector<std::string>& maps, HWND frame)
{
    InterlockedExchange(&g_running, 1);
    InterlockedExchange(&g_cancelled, 0);
    g_probeTrusted = false;

    ShowWindow(frame, SW_HIDE);
    EnableWindow(frame, FALSE);
    RouteToFeed(&AppendLine);

    int rebuilt = 0, skipped = 0, failed = 0;
    std::vector<std::string> repaired, needsAttention;
    std::string crashedOn;

    AppendLine("Scanning for damaged maps...");
    const std::vector<MapJob> jobs = PlanRun(maps);
    const int total = static_cast<int>(jobs.size());

    std::vector<std::string> damagedNames;
    for (const MapJob& job : jobs)
        if (job.damaged)
            damagedNames.push_back(BaseName(job.fileName));
    if (damagedNames.empty())
        AppendLine("No damaged maps found.");
    else
        ListMaps(damagedNames, "damaged - these are rebuilt first");

    for (int i = 0; i < total && !g_cancelled; ++i)
    {
        const MapJob&     job  = jobs[i];
        const std::string base = BaseName(job.fileName);
        const std::string path = std::string(kMapsDir) + job.fileName;

        char header[64];
        _snprintf_s(header, sizeof(header), _TRUNCATE, "%d/%d", i + 1, total);
        if (i > 0)
            AppendLine("");
        AppendLine(header);
        AppendLine(("Loading: " + base).c_str());

        if (!CanOpenForWrite(path.c_str()))
        {
            AppendLine(("SKIPPED: " + base + " is read-only or locked").c_str());
            needsAttention.push_back(base + " - read-only or locked");
            ++skipped;
            continue;
        }

        const DWORD started = GetTickCount();
        const MapResult result = ProcessMap(frame, path.c_str(), job.fileName.c_str(), base.c_str());
        const std::string took = "Took " + ElapsedText(GetTickCount() - started);

        if (result == Map_Ok)
        {
            ++rebuilt;
            if (job.damaged)
            {
                repaired.push_back(base);
                AppendLine(("Repaired: " + base).c_str());
            }
        }
        else if (result == Map_Crashed)
        {
            ++failed;
            needsAttention.push_back(base + " - crashed the editor, beyond repair");
            AppendLine(("ERROR: " + base + " crashed the editor - stopping.").c_str());
            AppendLine("Restart the editor before rebuilding anything else.");
            crashedOn = base;
        }
        else
        {
            const char* what = (result == Map_LoadFailed) ? "load" : "save";
            AppendLine((std::string("ERROR: Failed to ") + what + " " + base).c_str());
            needsAttention.push_back(base + " - failed to " + what);
            ++failed;
        }

        AppendLine(took.c_str());
        if (!crashedOn.empty())
            break;
    }

    RouteToFeed(nullptr);
    EnableWindow(frame, TRUE);
    ShowWindow(frame, SW_SHOW);
    InvalidateRect(frame, nullptr, TRUE);

    const char* outcome = !crashedOn.empty() ? "Stopped" : g_cancelled ? "Cancelled" : "Finished";
    char summary[192];
    _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                "%s %d/%d - %d rebuilt, %d skipped, %d failed.",
                outcome, rebuilt + skipped + failed, total, rebuilt, skipped, failed);
    AppendLine("");
    AppendLine(summary);

    ListMaps(repaired, "repaired - damage healed by the rebuild");
    ListMaps(needsAttention, "to check or rebuild by hand");

    if (!g_probeTrusted)
        AppendLine("Note: could not confirm which map was open, so failed loads may be unreported.");

    InterlockedExchange(&g_running, 0);
    if (g_hWnd)
        PostMessageA(g_hWnd, WM_FEED_DONE, 0, 0);

    // The editor dies moments later, taking this window with it; blocking keeps the feed readable.
    if (!crashedOn.empty())
    {
        char alert[256];
        _snprintf_s(alert, sizeof(alert), _TRUNCATE,
            "%s crashed the editor - the run has stopped.\r\n\r\n"
            "Close the editor and start it again.",
            crashedOn.c_str());
        MessageBoxA(g_hWnd, alert, kTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
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
        "itself. Expect this to run for hours at the Reloaded lightmap resolution. The "
        "editor window is hidden until the run ends.\n\n"
        "Back up Packages\\MapsEd before continuing.",
        static_cast<int>(maps.size()));

    if (MessageBoxA(hParent, prompt, kTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    if (!ShowProgressWindow(hParent))
        return;

    RunBatch(maps, hParent);
}
