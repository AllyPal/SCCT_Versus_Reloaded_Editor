// ReloadedOptions.cpp
// "Reloaded Options" dialog — opened from View > Reloaded Options (command 40066).
//
// Styled after Unreal Engine 2 / UT2004 UnrealEd dialogs:
//   - 8pt MS Sans Serif (matches every UE2 dialog)
//   - DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
//   - GROUPBOX per settings section, LTEXT labels, EDITTEXT for numeric / char entry
//   - Standard 50×14 OK / Cancel buttons at the dialog's lower-right
//
// Equivalent RC resource (for documentation / reference):
//
//   IDD_RELOADED_OPTIONS DIALOG  0, 0, 260, 170
//   STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
//   CAPTION "Reloaded Options"
//   FONT 8, "MS Sans Serif"
//   BEGIN
//     // ── Viewport ──────────────────────────────────────────────────────
//     GROUPBOX        "Viewport",         IDC_STATIC,    5,   5, 239,  55
//     LTEXT           "Max FPS:",         IDC_STATIC,   13,  18,  38,   8
//     EDITTEXT        IDC_EDIT_MAXFPS,                  54,  16,  38,  12, ES_NUMBER | ES_AUTOHSCROLL
//     LTEXT           "Controls the Realtime Preview frame rate cap. (0 = unlimited)",
//                                         IDC_STATIC,   96,  18, 144,   8
//     AUTOCHECKBOX    "Mute sounds in Realtime Preview",
//                                         IDC_CHECK_MUTE_SOUNDS, 13, 32, 190, 10
//     AUTOCHECKBOX    "No position offset on duplicate",
//                                         IDC_CHECK_NO_DUPE_OFFSET, 13, 44, 190, 10
//
//     // ── Geometric Event Keybinds ───────────────────────────────────────
//     GROUPBOX        "Geometric Event Keybinds", IDC_STATIC, 5, 65, 239, 81
//     LTEXT           "Shift+key for each type (A\x2013Z). Some keys aren't allowed if reserved by UnrealEd.",
//                                         IDC_STATIC,   13,  76, 225,  16
//     LTEXT           "Ledge Grab:",      IDC_STATIC,   13,  96,  62,   8
//     EDITTEXT        IDC_EDIT_GE_LEDGE,              79,  94,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//     LTEXT           "Hand-over-hand:",  IDC_STATIC,  121,  96,  70,   8
//     EDITTEXT        IDC_EDIT_GE_HOH,               195,  94,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//     LTEXT           "Pipe:",            IDC_STATIC,   13, 112,  62,   8
//     EDITTEXT        IDC_EDIT_GE_PIPE,               79, 110,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//     LTEXT           "Ladder:",          IDC_STATIC,  121, 112,  70,   8
//     EDITTEXT        IDC_EDIT_GE_LADDER,            195, 110,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//     LTEXT           "Zipline:",         IDC_STATIC,   13, 128,  62,   8
//     EDITTEXT        IDC_EDIT_GE_ZIPLINE,             79, 126,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//     LTEXT           "Fence:",           IDC_STATIC,  121, 128,  70,   8
//     EDITTEXT        IDC_EDIT_GE_FENCE,             195, 126,  20,  12, ES_UPPERCASE | ES_AUTOHSCROLL
//
//     DEFPUSHBUTTON   "OK",     IDOK,                 139, 151,  50,  14
//     PUSHBUTTON      "Cancel", IDCANCEL,             194, 151,  50,  14
//   END
//
// The dialog is built in-memory (DLGTEMPLATE) — no RC resource-file changes
// needed, consistent with the project's existing SoundBrowser dialogs.
//
// Settings are persisted to "Reloaded_Editor.ini" beside ChaosTheory_Editor.exe:
//   [Viewport]
//   MaxFPS=240
//   [GEKeybinds]
//   LedgeGrab=E
//   HandOverHand=H
//   Pipe=P
//   Ladder=L
//   Zipline=Z
//   Fence=F

#include "pch.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"    // g_ReloadedMaxFPS, g_ReloadedMuteSounds
#include "GEKeybindSwap.h"
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

// ── Control IDs ───────────────────────────────────────────────────────────────
#define IDC_EDIT_MAXFPS        1001
#define IDC_EDIT_GE_LEDGE      1002
#define IDC_EDIT_GE_HOH        1003
#define IDC_EDIT_GE_PIPE       1004
#define IDC_EDIT_GE_LADDER     1005
#define IDC_EDIT_GE_ZIPLINE    1006
#define IDC_EDIT_GE_FENCE      1007
#define IDC_CHECK_MUTE_SOUNDS      1008
#define IDC_CHECK_NO_DUPE_OFFSET   1009

// ── In-memory DLGTEMPLATE builder ─────────────────────────────────────────────
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

// ── Helpers ───────────────────────────────────────────────────────────────────
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
    b.atom(0x0080);   // "BUTTON"
    b.ws(text);
    b.w(0);
}

// Emit a STATIC control (atom 0x0082) — LTEXT or GROUPBOX title.
static void EmitStatic(OptionsDialogBuf& b,
    DWORD style, short x, short y, short cx, short cy,
    WORD id, const wchar_t* text)
{
    b.a4();
    b.dw(style); b.dw(0);
    b.w(x); b.w(y); b.w(cx); b.w(cy);
    b.w(id);
    b.atom(0x0082);   // "STATIC"
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
    b.atom(0x0081);   // "EDIT"
    b.ws(L"");
    b.w(0);
}

// ── Dialog layout (dialog units, 8pt MS Sans Serif) ───────────────────────────
//
//  ┌──────────────────────────────────────────────────────────┐
//  │ Reloaded Options                                    [X]  │
//  │ ┌─ Viewport ──────────────────────────────────────────┐  │
//  │ │  Max FPS: [  240  ]  Controls the Realtime Preview │  │
//  │ │                      frame rate cap. (0=unlimited) │  │
//  │ │  [✓] Mute sounds in Realtime Preview               │  │
//  │ │  [ ] No position offset on duplicate               │  │
//  │ └─────────────────────────────────────────────────────┘  │
//  │ ┌─ Geometric Event Keybinds ──────────────────────────┐  │
//  │ │  Shift+key for each type (A–Z). Some keys aren't  │  │
//  │ │  allowed if reserved by UnrealEd.                  │  │
//  │ │  Ledge Grab:    [E]    Hand-over-hand:  [H]        │  │
//  │ │  Pipe:          [P]    Ladder:          [L]        │  │
//  │ │  Zipline:       [Z]    Fence:           [F]        │  │
//  │ └─────────────────────────────────────────────────────┘  │
//  │                                    [  OK  ] [Cancel]     │
//  └──────────────────────────────────────────────────────────┘
//   260 × 170 dialog units   |   22 controls
//
static std::vector<uint8_t> BuildReloadedOptionsDlgTemplate()
{
    OptionsDialogBuf b;

    // ── Dialog header ─────────────────────────────────────────────────────────
    // cdit = 21
    const DWORD dlgStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU
                         | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    b.dw(dlgStyle);
    b.dw(0);
    b.w(22);              // cdit
    b.w(0);  b.w(0);      // x, y  (DS_CENTER overrides)
    b.w(260); b.w(170);   // cx, cy
    b.w(0);               // no menu
    b.w(0);               // default dialog class
    b.ws(L"Reloaded Options");
    b.w(8);
    b.ws(L"MS Sans Serif");

    // ═══════════════════════════════════════════════════════════════════════════
    //  VIEWPORT GROUP  (y = 5 … 48)
    // ═══════════════════════════════════════════════════════════════════════════

    // GroupBox "Viewport"
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
               5, 5, 239, 55, 0xFFFF, L"Viewport");

    // "Max FPS:" label
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 18, 38, 8, 0xFFFF, L"Max FPS:");

    // Max FPS edit — ES_NUMBER: digits only
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL,
             54, 16, 38, 12, IDC_EDIT_MAXFPS);

    // Description + hint, inline to the right of the edit box
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               96, 18, 144, 8, 0xFFFF,
               L"Controls the Realtime Preview frame rate cap. (0 = unlimited)");

    // "Mute sounds in Realtime Preview" checkbox
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
               13, 32, 190, 10, IDC_CHECK_MUTE_SOUNDS,
               L"Mute sounds in Realtime Preview");

    // "No position offset on duplicate" checkbox
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
               13, 44, 190, 10, IDC_CHECK_NO_DUPE_OFFSET,
               L"No position offset on duplicate");

    // ═══════════════════════════════════════════════════════════════════════════
    //  GEOMETRIC EVENT KEYBINDS GROUP  (y = 65 … 146)
    // ═══════════════════════════════════════════════════════════════════════════

    // GroupBox "Geometric Event Keybinds"
    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
               5, 65, 239, 81, 0xFFFF, L"Geometric Event Keybinds");

    // Note text (two lines — h=16)
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 76, 225, 16, 0xFFFF,
               L"Shift+key for each type (A-Z). Some keys aren't allowed if reserved by UnrealEd.");

    // ── Row 1: Ledge Grab  |  Hand-over-hand ─────────────────────────────────
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 96, 62, 8, 0xFFFF, L"Ledge Grab:");
    // ES_UPPERCASE: auto-uppercases any typed letter
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 94, 20, 12, IDC_EDIT_GE_LEDGE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               121, 96, 70, 8, 0xFFFF, L"Hand-over-hand:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             195, 94, 20, 12, IDC_EDIT_GE_HOH);

    // ── Row 2: Pipe  |  Ladder ───────────────────────────────────────────────
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 112, 62, 8, 0xFFFF, L"Pipe:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 110, 20, 12, IDC_EDIT_GE_PIPE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               121, 112, 70, 8, 0xFFFF, L"Ladder:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             195, 110, 20, 12, IDC_EDIT_GE_LADDER);

    // ── Row 3: Zipline  |  Fence ─────────────────────────────────────────────
    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               13, 128, 62, 8, 0xFFFF, L"Zipline:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             79, 126, 20, 12, IDC_EDIT_GE_ZIPLINE);

    EmitStatic(b, WS_CHILD | WS_VISIBLE | SS_LEFT,
               121, 128, 70, 8, 0xFFFF, L"Fence:");
    EmitEdit(b, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_LEFT | ES_UPPERCASE | ES_AUTOHSCROLL,
             195, 126, 20, 12, IDC_EDIT_GE_FENCE);

    // ═══════════════════════════════════════════════════════════════════════════
    //  OK / CANCEL  (y = 151)   — standard 50×14 UE2 button size
    // ═══════════════════════════════════════════════════════════════════════════

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
               139, 151, 50, 14, IDOK, L"OK");

    EmitButton(b, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
               194, 151, 50, 14, IDCANCEL, L"Cancel");

    return b.buf;
}

// ── INI persistence ───────────────────────────────────────────────────────────
// All settings live in Reloaded_Editor.ini beside ChaosTheory_Editor.exe.

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
// Falls back to `defaultKey` if the value is absent or not A–Z.
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

    // [Viewport]
    int fps = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "MaxFPS", 240, ini.c_str()));
    if (fps < 0)   fps = 0;
    if (fps > 999) fps = 999;
    g_ReloadedMaxFPS = fps;

    int mute = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "MuteSounds", 0, ini.c_str()));
    g_ReloadedMuteSounds = (mute != 0);

    int noDupeOffset = static_cast<int>(
        GetPrivateProfileIntA("Viewport", "NoDuplicateOffset", 0, ini.c_str()));
    g_ReloadedNoDuplicateOffset = (noDupeOffset != 0);

    // [GEKeybinds]  — defaults match GEKeybindSwap.cpp compile-time values
    g_KeyLedgeGrab    = LoadGEKey(ini, "LedgeGrab",    'E');
    g_KeyHandOverHand = LoadGEKey(ini, "HandOverHand", 'H');
    g_KeyPipe         = LoadGEKey(ini, "Pipe",         'P');
    g_KeyLadder       = LoadGEKey(ini, "Ladder",       'L');
    g_KeyZipline      = LoadGEKey(ini, "Zipline",      'Z');
    g_KeyFence        = LoadGEKey(ini, "Fence",        'F');
}

static void SaveSettings()
{
    const std::string ini = GetIniPath();

    // [Viewport]
    char val[16];
    snprintf(val, sizeof(val), "%d", g_ReloadedMaxFPS);
    WritePrivateProfileStringA("Viewport", "MaxFPS", val, ini.c_str());

    WritePrivateProfileStringA("Viewport", "MuteSounds",
                               g_ReloadedMuteSounds ? "1" : "0", ini.c_str());

    WritePrivateProfileStringA("Viewport", "NoDuplicateOffset",
                               g_ReloadedNoDuplicateOffset ? "1" : "0", ini.c_str());

    // [GEKeybinds]
    char letter[2] = { 0, 0 };

    letter[0] = static_cast<char>(g_KeyLedgeGrab);
    WritePrivateProfileStringA("GEKeybinds", "LedgeGrab", letter, ini.c_str());

    letter[0] = static_cast<char>(g_KeyHandOverHand);
    WritePrivateProfileStringA("GEKeybinds", "HandOverHand", letter, ini.c_str());

    letter[0] = static_cast<char>(g_KeyPipe);
    WritePrivateProfileStringA("GEKeybinds", "Pipe", letter, ini.c_str());

    letter[0] = static_cast<char>(g_KeyLadder);
    WritePrivateProfileStringA("GEKeybinds", "Ladder", letter, ini.c_str());

    letter[0] = static_cast<char>(g_KeyZipline);
    WritePrivateProfileStringA("GEKeybinds", "Zipline", letter, ini.c_str());

    letter[0] = static_cast<char>(g_KeyFence);
    WritePrivateProfileStringA("GEKeybinds", "Fence", letter, ini.c_str());
}

// ── Dialog procedure ──────────────────────────────────────────────────────────

// Populate a GE edit box from a uint8_t key value.
static void SetGEEdit(HWND hDlg, int id, uint8_t key)
{
    char s[2] = { static_cast<char>(key), '\0' };
    SetDlgItemTextA(hDlg, id, s);
}

// Read one character from a GE edit box, uppercase it.
// Returns 0 if the field is empty or contains a non-letter.
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
    // ── Initialise controls from current runtime values ────────────────────────
    case WM_INITDIALOG:
        // Max FPS edit (3 digits max)
        SendDlgItemMessageA(hDlg, IDC_EDIT_MAXFPS, EM_SETLIMITTEXT, 3, 0);
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", g_ReloadedMaxFPS);
            SetDlgItemTextA(hDlg, IDC_EDIT_MAXFPS, buf);
        }

        // GE key edits (1 char each)
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

        // Mute sounds checkbox
        CheckDlgButton(hDlg, IDC_CHECK_MUTE_SOUNDS,
                       g_ReloadedMuteSounds ? BST_CHECKED : BST_UNCHECKED);

        // No position offset on duplicate checkbox
        CheckDlgButton(hDlg, IDC_CHECK_NO_DUPE_OFFSET,
                       g_ReloadedNoDuplicateOffset ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;   // let Windows set default focus

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            {
                // ── Read and validate MaxFPS ───────────────────────────────────
                char fpsBuf[16] = {};
                GetDlgItemTextA(hDlg, IDC_EDIT_MAXFPS, fpsBuf, sizeof(fpsBuf));
                int fps = atoi(fpsBuf);
                if (fps < 0)   fps = 0;
                if (fps > 1000) fps = 1000;

                // ── Read GE keys ───────────────────────────────────────────────
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

                // ── Validate: all fields must be A–Z ──────────────────────────
                for (int i = 0; i < 6; ++i)
                {
                    if (keys[i] == 0)
                    {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "\"%s\" is empty or not a letter (A\x96Z).",
                                 kNames[i]);
                        MessageBoxA(hDlg, msg, "Reloaded Options", MB_OK | MB_ICONWARNING);
                        return TRUE;   // keep dialog open
                    }
                }

                // ── Validate: no conflicts with known UnrealEd shortcuts ─────────
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
                            return TRUE;   // keep dialog open
                        }
                    }
                }

                // ── Validate: all keys must be unique ─────────────────────────
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

                // ── Read mute-sounds checkbox ─────────────────────────────────
                bool mute = (IsDlgButtonChecked(hDlg, IDC_CHECK_MUTE_SOUNDS) == BST_CHECKED);

                // ── Read no-dupe-offset checkbox ──────────────────────────────
                bool noDupeOffset = (IsDlgButtonChecked(hDlg, IDC_CHECK_NO_DUPE_OFFSET) == BST_CHECKED);

                // ── Commit ────────────────────────────────────────────────────
                g_ReloadedMaxFPS           = fps;
                g_ReloadedMuteSounds       = mute;
                g_ReloadedNoDuplicateOffset = noDupeOffset;
                g_KeyLedgeGrab    = static_cast<uint8_t>(keys[0]);
                g_KeyHandOverHand = static_cast<uint8_t>(keys[1]);
                g_KeyPipe         = static_cast<uint8_t>(keys[2]);
                g_KeyLadder       = static_cast<uint8_t>(keys[3]);
                g_KeyZipline      = static_cast<uint8_t>(keys[4]);
                g_KeyFence        = static_cast<uint8_t>(keys[5]);

                // Re-patch the binary with the new GE key bytes immediately.
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

// ── Public API ────────────────────────────────────────────────────────────────

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
    // Load all persisted settings into runtime globals.
    // Must run before GEKeybindSwap::Initialize() patches the binary.
    LoadSettings();
}
