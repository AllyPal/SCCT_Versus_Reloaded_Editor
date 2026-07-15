#include "pch.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"
#include "GEKeybindSwap.h"
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

#define IDC_EDIT_MAXFPS             1001
#define IDC_EDIT_GE_LEDGE           1002
#define IDC_EDIT_GE_HOH             1003
#define IDC_EDIT_GE_PIPE            1004
#define IDC_EDIT_GE_LADDER          1005
#define IDC_EDIT_GE_ZIPLINE         1006
#define IDC_EDIT_GE_FENCE           1007
#define IDC_CHECK_MUTE_SOUNDS       1008
#define IDC_CHECK_NO_DUPE_OFFSET    1009
#define IDC_CHECK_MIN_PLAY          1010

// In-memory DLGTEMPLATE builder
struct OptionsDialogBuf
{
    std::vector<uint8_t> buf;
    void a2() { while (buf.size() & 1) buf.push_back(0); }
    void a4() { while (buf.size() & 3) buf.push_back(0); }
    void w(WORD v)   { buf.push_back(v & 0xFF); buf.push_back(v >> 8); }
    void dw(DWORD v) { w(static_cast<WORD>(v)); w(static_cast<WORD>(v >> 16)); }
    void ws(const wchar_t* s) { for (; *s; ++s) w(static_cast<WORD>(*s)); w(0); }
    void atom(WORD a) { w(0xFFFF); w(a); }
};

// Emit a complete DLGITEMTEMPLATE using the "BUTTON" system atom (0x0080).
// Used for both GROUPBOX (BS_GROUPBOX) and PUSHBUTTON / DEFPUSHBUTTON.
static void EmitButton(OptionsDialogBuf& b,
    DWORD style, short x, short y, short cx, short cy,
    WORD id, const wchar_t* text)
{
    b.a4();
    b.dw(style); b.dw(0);
    b.w(x); b.w(y); b.w(cx); b.w(cy);
    b.w(id);
    b.atom(0x0080);
    b.ws(text);
    b.w(0);
}

// Emit a STATIC control (atom 0x0082) - LTEXT or GROUPBOX title.
static void EmitStatic(OptionsDialogBuf& b,
    DWORD style, short x, short y, short cx, short cy,
    WORD id, const wchar_t* text)
{
    b.a4();
    b.dw(style); b.dw(0);
    b.w(x); b.w(y); b.w(cx); b.w(cy);
    b.w(id);
    b.atom(0x0082);
    b.ws(text);
    b.w(0);
}

// Emit an EDIT control (atom 0x0081).
static void EmitEdit(OptionsDialogBuf& b,
    DWORD style, short x, short y, short cx, short cy, WORD id)
{
    b.a4();
    b.dw(style); b.dw(0);
    b.w(x); b.w(y); b.w(cx); b.w(cy);
    b.w(id);
    b.atom(0x0081);
    b.ws(L"");
    b.w(0);
}

static std::vector<uint8_t> BuildReloadedOptionsDlgTemplate()
{
    OptionsDialogBuf b;

    b.dw(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
    b.dw(0);
    b.w(24);              // number of controls
    b.w(0);  b.w(0);      // x, y (DS_CENTER overrides)
    b.w(290); b.w(206);   // width, height
    b.w(0);               // no menu
    b.w(0);               // default class
    b.ws(L"Reloaded Options");
    b.w(8);
    b.ws(L"MS Sans Serif");

    // Viewport group (y = 5 to 48)
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
               5, 5, 280, 55, 0xFFFF, L"Viewport");

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 18, 38, 8, 0xFFFF, L"Max FPS:");

    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL,
             54, 16, 38, 12, IDC_EDIT_MAXFPS);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               96, 18, 185, 8, 0xFFFF,
               L"Controls the Realtime Preview frame rate cap.");

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
               13, 32, 190, 10, IDC_CHECK_MUTE_SOUNDS,
               L"Mute sounds in Realtime Preview");

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
               13, 44, 190, 10, IDC_CHECK_NO_DUPE_OFFSET,
               L"No position offset on duplicate");

    // Geometric Event Keybinds group (y = 65 to 146)
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
               5, 65, 280, 81, 0xFFFF, L"Geometric Event Keybinds");

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 76, 265, 16, 0xFFFF,
               L"Shift+key for each type (A-Z). Some keys aren't allowed if reserved by UnrealEd.");

    // Row 1: Ledge Grab and Hand-over-hand
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 96, 62, 8, 0xFFFF, L"Ledge Grab:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 94, 20, 12, IDC_EDIT_GE_LEDGE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               140, 96, 70, 8, 0xFFFF, L"Hand-over-hand:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             214, 94, 20, 12, IDC_EDIT_GE_HOH);

    // Row 2: Pipe and Ladder
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 112, 62, 8, 0xFFFF, L"Pipe:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 110, 20, 12, IDC_EDIT_GE_PIPE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               140, 112, 70, 8, 0xFFFF, L"Ladder:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             214, 110, 20, 12, IDC_EDIT_GE_LADDER);

    // Row 3: Zipline and Fence
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 128, 62, 8, 0xFFFF, L"Zipline:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 126, 20, 12, IDC_EDIT_GE_ZIPLINE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               140, 128, 70, 8, 0xFFFF, L"Fence:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             214, 126, 20, 12, IDC_EDIT_GE_FENCE);

    // General group (y = 150 to 180)
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
               5, 150, 280, 30, 0xFFFF, L"General");

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
               13, 163, 265, 10, IDC_CHECK_MIN_PLAY,
               L"Minimize Unreal Editor on Play Map");

    // OK and Cancel buttons (right-aligned to the wider dialog)
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
               180, 186, 50, 14, IDOK, L"OK");

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
               235, 186, 50, 14, IDCANCEL, L"Cancel");

    return b.buf;
}

static std::string GetIniPath()
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';
    return std::string(exePath) + "Reloaded_Editor.ini";
}

// Read a single GE keybind letter from INI.
// Falls back to defaultKey if the value is absent or not A-Z.
static uint8_t LoadGEKey(const std::string& ini,
                         const char* key, char defaultKey)
{
    char buf[4] = {};
    GetPrivateProfileStringA("GEKeybinds", key,
                             nullptr, buf, sizeof(buf), ini.c_str());
    char c = static_cast<char>(toupper(static_cast<unsigned char>(buf[0])));
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c)
                                  : static_cast<uint8_t>(defaultKey);
}

static void LoadSettings()
{
    const std::string ini = GetIniPath();

    int fps = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "MaxFPS", 120, ini.c_str()));
    if (fps < 0)   fps = 0;
    if (fps > 999) fps = 999;
    g_ReloadedMaxFPS = fps;

    int mute = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "MuteSounds", 0, ini.c_str()));
    g_ReloadedMuteSounds = (mute != 0);

    int noDupeOffset = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "NoDuplicateOffset", 0, ini.c_str()));
    g_ReloadedNoDuplicateOffset = (noDupeOffset != 0);

    int minOnPlay = static_cast<int>(
        GetPrivateProfileIntA("General", "MinimizeOnPlay", 0, ini.c_str()));
    g_ReloadedMinimizeOnPlay = (minOnPlay != 0);

    g_KeyLedgeGrab    = LoadGEKey(ini, "LedgeGrab",    'E'); // default: L
    g_KeyHandOverHand = LoadGEKey(ini, "HandOverHand", 'H');
    g_KeyPipe         = LoadGEKey(ini, "Pipe",         'P');
    g_KeyLadder       = LoadGEKey(ini, "Ladder",       'L'); // default: E
    g_KeyZipline      = LoadGEKey(ini, "Zipline",      'Z');
    g_KeyFence        = LoadGEKey(ini, "Fence",        'F');
}

static void SaveSettings()
{
    // Write the INI manually to keep blank lines between sections.
    char text[512];
    int len = snprintf(text, sizeof(text),
        "[Viewport]\r\n"
        "MaxFPS=%d\r\n"
        "MuteSounds=%d\r\n"
        "NoDuplicateOffset=%d\r\n"
        "\r\n"
        "[GEKeybinds]\r\n"
        "LedgeGrab=%c\r\n"
        "HandOverHand=%c\r\n"
        "Pipe=%c\r\n"
        "Ladder=%c\r\n"
        "Zipline=%c\r\n"
        "Fence=%c\r\n"
        "\r\n"
        "[General]\r\n"
        "MinimizeOnPlay=%d\r\n",
        g_ReloadedMaxFPS,
        g_ReloadedMuteSounds ? 1 : 0,
        g_ReloadedNoDuplicateOffset ? 1 : 0,
        static_cast<char>(g_KeyLedgeGrab),
        static_cast<char>(g_KeyHandOverHand),
        static_cast<char>(g_KeyPipe),
        static_cast<char>(g_KeyLadder),
        static_cast<char>(g_KeyZipline),
        static_cast<char>(g_KeyFence),
        g_ReloadedMinimizeOnPlay ? 1 : 0);
    if (len <= 0)
        return;

    const std::string ini = GetIniPath();
    HANDLE h = CreateFileA(ini.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

static void SetGEEdit(HWND hDlg, int id, uint8_t key)
{
    char s[2] = { static_cast<char>(key), '\0' };
    SetDlgItemTextA(hDlg, id, s);
}

static char ReadGEEdit(HWND hDlg, int id)
{
    char buf[4] = {};
    GetDlgItemTextA(hDlg, id, buf, sizeof(buf));
    char c = static_cast<char>(toupper(static_cast<unsigned char>(buf[0])));
    return (c >= 'A' && c <= 'Z') ? c : 0;
}

static INT_PTR CALLBACK ReloadedOptionsDlgProc(
    HWND hDlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SendDlgItemMessageA(hDlg, IDC_EDIT_MAXFPS, EM_SETLIMITTEXT, 3, 0);
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", g_ReloadedMaxFPS);
            SetDlgItemTextA(hDlg, IDC_EDIT_MAXFPS, buf);
        }

        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_LEDGE,   EM_SETLIMITTEXT, 1, 0);
        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_HOH,     EM_SETLIMITTEXT, 1, 0);
        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_PIPE,    EM_SETLIMITTEXT, 1, 0);
        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_LADDER,  EM_SETLIMITTEXT, 1, 0);
        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_ZIPLINE, EM_SETLIMITTEXT, 1, 0);
        SendDlgItemMessageA(hDlg, IDC_EDIT_GE_FENCE,   EM_SETLIMITTEXT, 1, 0);

        SetGEEdit(hDlg, IDC_EDIT_GE_LEDGE,   g_KeyLedgeGrab);
        SetGEEdit(hDlg, IDC_EDIT_GE_HOH,     g_KeyHandOverHand);
        SetGEEdit(hDlg, IDC_EDIT_GE_PIPE,    g_KeyPipe);
        SetGEEdit(hDlg, IDC_EDIT_GE_LADDER,  g_KeyLadder);
        SetGEEdit(hDlg, IDC_EDIT_GE_ZIPLINE, g_KeyZipline);
        SetGEEdit(hDlg, IDC_EDIT_GE_FENCE,   g_KeyFence);

        CheckDlgButton(hDlg, IDC_CHECK_MUTE_SOUNDS,
                       g_ReloadedMuteSounds ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(hDlg, IDC_CHECK_NO_DUPE_OFFSET,
                       g_ReloadedNoDuplicateOffset ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(hDlg, IDC_CHECK_MIN_PLAY,
                       g_ReloadedMinimizeOnPlay ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;   // let Windows set default focus

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            {
                // Read and validate MaxFPS
                char fpsBuf[16] = {};
                GetDlgItemTextA(hDlg, IDC_EDIT_MAXFPS, fpsBuf, sizeof(fpsBuf));
                int fps = atoi(fpsBuf);
                if (fps < 0)   fps = 0;
                if (fps > 1000) fps = 1000;

                // Read GE keys
                char keys[6];
                keys[0] = ReadGEEdit(hDlg, IDC_EDIT_GE_LEDGE);
                keys[1] = ReadGEEdit(hDlg, IDC_EDIT_GE_HOH);
                keys[2] = ReadGEEdit(hDlg, IDC_EDIT_GE_PIPE);
                keys[3] = ReadGEEdit(hDlg, IDC_EDIT_GE_LADDER);
                keys[4] = ReadGEEdit(hDlg, IDC_EDIT_GE_ZIPLINE);
                keys[5] = ReadGEEdit(hDlg, IDC_EDIT_GE_FENCE);

                static const char* const kNames[6] = {
                    "Ledge Grab", "Hand-over-hand",
                    "Pipe",       "Ladder",
                    "Zipline",    "Fence"
                };

                // Validate: all fields must be A-Z
                for (int i = 0; i < 6; ++i)
                {
                    if (keys[i] == 0)
                    {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "\"%s\" is empty or not a letter (A-Z).",
                                 kNames[i]);
                        MessageBoxA(hDlg, msg, "Reloaded Options", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                }

                // Validate: no conflicts with known UnrealEd shortcuts
                struct ReservedKey { char key; const char* action; };
                static const ReservedKey kReserved[] = {
                    { 'A', "Select All Actors"  },
                    { 'S', "Actor Scaling Mode" },
                    { 'R', "Actor Rotate Mode"  },
                };

                for (int i = 0; i < 6; ++i)
                {
                    for (const auto& r : kReserved)
                    {
                        if (keys[i] == r.key)
                        {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "Shift+%c cannot be used because UnrealEd uses it for \"%s\".\n\n"
                                     "Please choose a different key for \"%s\".",
                                     keys[i], r.action, kNames[i]);
                            MessageBoxA(hDlg, msg, "Reloaded Options", MB_OK | MB_ICONWARNING);
                            return TRUE;
                        }
                    }
                }

                // Validate: all keys must be unique
                for (int i = 0; i < 6; ++i)
                    for (int j = i + 1; j < 6; ++j)
                        if (keys[i] == keys[j])
                        {
                            char msg[160];
                            snprintf(msg, sizeof(msg),
                                     "\"%s\" and \"%s\" share the same key (Shift+%c).\n"
                                     "Each GE type must use a different key.",
                                     kNames[i], kNames[j], keys[i]);
                            MessageBoxA(hDlg, msg, "Reloaded Options", MB_OK | MB_ICONWARNING);
                            return TRUE;
                        }

                bool mute = (IsDlgButtonChecked(hDlg, IDC_CHECK_MUTE_SOUNDS) == BST_CHECKED);
                bool noDupeOffset = (IsDlgButtonChecked(hDlg, IDC_CHECK_NO_DUPE_OFFSET) == BST_CHECKED);
                bool minOnPlay = (IsDlgButtonChecked(hDlg, IDC_CHECK_MIN_PLAY) == BST_CHECKED);

                g_ReloadedMaxFPS           = fps;
                g_ReloadedMuteSounds       = mute;
                g_ReloadedNoDuplicateOffset = noDupeOffset;
                g_ReloadedMinimizeOnPlay   = minOnPlay;
                g_KeyLedgeGrab    = static_cast<uint8_t>(keys[0]);
                g_KeyHandOverHand = static_cast<uint8_t>(keys[1]);
                g_KeyPipe         = static_cast<uint8_t>(keys[2]);
                g_KeyLadder       = static_cast<uint8_t>(keys[3]);
                g_KeyZipline      = static_cast<uint8_t>(keys[4]);
                g_KeyFence        = static_cast<uint8_t>(keys[5]);

                GEKeybindSwap::ApplyGEKeybinds();

                SaveSettings();
                EndDialog(hDlg, IDOK);
            }
            return TRUE;

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowReloadedOptionsDialog(HWND hParent)
{
    std::vector<uint8_t> tmpl = BuildReloadedOptionsDlgTemplate();
    DialogBoxIndirectParamA(
        GetModuleHandleA(NULL),
        reinterpret_cast<LPCDLGTEMPLATEA>(tmpl.data()),
        hParent,
        ReloadedOptionsDlgProc,
        0);
}

void ReloadedOptions::Initialize()
{
    LoadSettings();

    // Create a default INI if one doesn't exist
    const std::string ini = GetIniPath();
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        SaveSettings();
}
