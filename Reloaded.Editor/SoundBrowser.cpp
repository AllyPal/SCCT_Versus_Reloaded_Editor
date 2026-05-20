#include "pch.h"
#include "SoundBrowser.h"
#include "Hooks.h"
#include "MemoryWriter.h"
#include <commctrl.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

INIT_HOOKS;

// USound field offsets (from FUN_10e7db10 disassembly)
#define USOUND_FLAGS_OFFSET   0x60
#define USOUND_VOLUME_OFFSET  0x64
#define USOUND_RADIUS_OFFSET  0x68

// SF_STREAM (0x100) = SF_TYPE_RANDOM; Ubisoft reused the Stream bit for Random composite type.
// The "Stream" checkbox in Sound Properties uses SF_UAS_STREAM (0x4) instead.
// SF_UNK_0200 (0x200): dead in the PC editor DLL - exhaustive Ghidra search found zero
//   TEST instructions reading USound+0x60 with a 0x200 mask. Originally labelled "Xbox HD
//   Stream" in stock editor dialog (control 1338); real Xbox HD Stream is SF_XBOXHD_STREAM (0x4000).
#define SF_2D           0x00000002u
#define SF_LOOP         0x00000010u
#define SF_STREAM       0x00000100u   // = SF_TYPE_RANDOM; NOT the stream flag
#define SF_UNK_0200     0x00000200u   // dead in PC editor; not wired to any dialog control
#define SF_OVR_VOLUME   0x00000400u

// Sound Properties dialog / control IDs
#define IDD_SOUND_PROPS      144
#define IDC_CHK_STREAM      1334
#define IDC_CHK_LOOP        1335   // resource label "2D"; sets SF_LOOP (0x10) - swapped per browser
#define IDC_CHK_2D          1336   // resource label "Loop"; sets SF_2D  (0x02) - swapped per browser
#define IDC_CHK_OVR_RADIUS  1337
#define IDC_EDIT_RADIUS     1288
#define IDC_CHK_XBOXHD      1338   // "Xbox HD Stream"; sets SF_XBOXHD_STREAM (0x4000)
#define IDC_CHK_OVR_VOLUME  1339
#define IDC_EDIT_VOLUME     1289
#define IDC_CHK_SURROUND    1341   // "Surround"; sets SF_TYPE_SURROUND (0x2000)

// Import Sound dialog (engine resource 156)
#define IDD_IMPORT_SOUND     156
#define IDC_IMPORT_FILE      1068
#define IDC_IMPORT_PACKAGE   1067
#define IDC_IMPORT_GROUP     1066
#define IDC_IMPORT_NAME      1065
#define IDB_IMPORT_OKALL     3
#define IDB_IMPORT_SKIP      4

// Rename dialog (engine resource 19805; same dialog used by Texture/StaticMesh browsers)
#define IDDIALOG_RENAME      19805
#define IDEC_NEWPACKAGE      1066
#define IDEC_NEWGROUP        1067
#define IDEC_NAME            1065

// Sound Browser menu command IDs
#define CMD_SAVE    0x9D29
#define CMD_EXPORT  0x9D2A
#define CMD_IMPORT  0x9D2B
#define CMD_DELETE       0x7647
#define CMD_RENAME       0x77A9

#define CMD_NEW          0x9D36u
#define CMD_RANDOM_PROPS 0x9D39u
#define CMD_SWITCH_PROPS 0x9D3Bu
#define CMD_SEQ_PROPS    0x9D3Cu
#define CMD_BUILD_UAS    0x9DA8u
#define CMD_EXPORT_UAS   0x9DA9u

// GEditor at 0x1165dfa0; FExec sub-object at GEditor+0x28; vtable[0] = FExec::Exec.
// vtable[0x224/4] on GEditor itself is UEditorEngine::Get (3 args, __thiscall, 12-byte cleanup).
// All AUDIO IMPORT / OBJ EXPORT / OBJ SAVEPACKAGE commands go through FExec::Exec (vtable[0]).
#define GEDITOR_GLOBAL      0x1165dfa0u
#define EXEC_LOG_DEV        0x115befb0u   // PTR_PTR_115befb0 - GLog/GNull output device ptr

// Refresh thunks (__thiscall, ECX = WBrowserSound*)
#define REFRESH_PACKAGES_THUNK  0x10e051d3u
#define REFRESH_GROUPS_THUNK    0x10e053efu
#define REFRESH_LIST_THUNK      0x10e022d5u

// vtable[0x224/4] = vtable[0x89] = UEditorEngine::Get; used for DELETE CLASS=SOUND
#define EDITOR_GET_VTABLE_OFFSET  0x224u

// GObjects: TArray<UObject*> – Data at 0x11697b70, Num at 0x11697b74
#define GOBJECTS_DATA 0x11697b70u
#define GOBJECTS_NUM  0x11697b74u

// GNames: base at 0x1169cfbc, count at 0x1169cfc0
// FNameEntry* = ((int*)(*(int*)GNAMES_DATA))[FName.Index]; string at FNameEntry+0x0C
#define GNAMES_DATA             0x1169cfbcu
#define GNAMES_NUM              0x1169cfc0u
#define FNAME_ENTRY_STR_OFFSET  0x0C

// UObject field offsets
#define UOBJ_INDEX_OFFSET  0x04
#define UOBJ_OUTER_OFFSET  0x18
#define UOBJ_FNAME_OFFSET  0x20

// Composite sound resource dialogs
#define IDD_CREATE_RESOURCE  142
// IDC_CREATE_NAME = 1288 (same value as IDC_EDIT_RADIUS; different dialog, no conflict)
#define IDC_RB_RANDOM    1290
#define IDC_RB_SEQUENCE  1291
#define IDC_RB_SWITCH    1292

struct NewResourceCtx { char name[256]; DWORD typeFlag; };

#define IDD_RANDOM           145
#define IDC_RAND_LIST        1335
#define IDC_RAND_INSERT      1336
#define IDC_RAND_REMOVE      1337
#define IDC_RAND_EDIT        1338
#define IDC_RAND_EQUAL_PROB  1009
#define IDC_RAND_SILENCE_LBL 1342

#define IDD_RANDOM_ELEM      146
#define IDC_RELEM_COMBO      1005
// IDC_RELEM_WEIGHT = 1288 (literal; use the value directly in RandomElemDlgProc)

#define IDD_SEQUENCE         19837
#define IDC_SEQ_LIST         1335
#define IDC_SEQ_INSERT       1336
#define IDC_SEQ_REMOVE       1337
#define IDC_SEQ_EDIT         1338
#define IDC_SEQ_MOVEUP       1303
#define IDC_SEQ_MOVEDOWN     1304

#define IDD_SEQ_ELEM         19838
#define IDC_SELEM_COMBO      1005
#define IDC_SELEM_REPEAT     1346
#define IDC_SELEM_TIMEDLOOP  1347

// Sequence repeat count field layout (TArray<int> at +0x78):
//   Bits 12:0  (0x1FFF) – value: repeat count (normal) OR loop duration in seconds (timed)
//   Bit  13    (0x2000) – timed-loop flag: if set, treat value as loop seconds
//   Bits 15:14           – unknown flags; preserve on round-trip
//
// e.g. raw = 0x200A > timed loop, 10 seconds
//      raw = 3      > normal, play 3 times
#define SEQ_TIMED_LOOP_FLAG  0x2000   // bit 13
#define SEQ_VALUE_MASK       0x1FFF   // lower 13 bits

#define IDD_SURROUND         289
#define CMD_SURROUND_PROPS   0x9D3Du
#define IDC_SURR_EDIT_FL     1419
#define IDC_SURR_EDIT_FR     1420
#define IDC_SURR_EDIT_BL     1421
#define IDC_SURR_EDIT_BR     1422
#define IDC_SURR_EDIT_CTR    1423
#define IDC_SURR_EDIT_LFE    1424
#define IDC_SURR_BROWSE_FL   1303
#define IDC_SURR_BROWSE_FR   1304
#define IDC_SURR_BROWSE_BL   1305
#define IDC_SURR_BROWSE_BR   1306
#define IDC_SURR_BROWSE_CTR  1307
#define IDC_SURR_BROWSE_LFE  1308

#define IDD_SWITCH           147
#define IDC_SW_LIST          1335
#define IDC_SW_INSERT        1336
#define IDC_SW_REMOVE        1337
#define IDC_SW_EDIT          1338

#define IDD_SWITCH_ELEM      148
#define IDC_SWELEM_COMBO     1005
#define IDC_SWELEM_SURFACE   1343

// ESurfaceType: value 0 (SURFACE_Generic) is the default/wildcard
#define SW_NUM_SURFACE_TYPES  26

// USound composite field offsets
// TArray<USound*> at +0x6c: child pointers (USound::Serialize / FUN_11117a70)
// TArray<int>     at +0x78: parallel per-child data (weights for Random, repeat counts for Sequence, ESurfaceType for Switch)
#define USOUND_TARRAY_DATA_OFFSET  0x6c  // TArray<USound*>.Data
#define USOUND_REF_COUNT_OFFSET    0x70  // TArray<USound*>.Num
#define USOUND_TARRAY_MAX_OFFSET   0x74  // TArray<USound*>.Max
#define USOUND_WARRAY_DATA_OFFSET  0x78  // TArray<int>.Data
#define USOUND_WARRAY_NUM_OFFSET   0x7c  // TArray<int>.Num
#define USOUND_WARRAY_MAX_OFFSET   0x80  // TArray<int>.Max
#define USOUND_REF_LIST_OFFSET     0x74  // legacy alias (Sequence only)

// SF_TYPE_RANDOM = SF_STREAM (0x100): Ubisoft intentionally reuses the Stream bit for Random type.
// SF_TYPE_SWITCH = 0x80: confirmed from live flag reads (Ubisoft Switch sounds have flags=0x80);
//   FUN_10e7db38: `(char)uVar1 < '\0'` = bit 7 set = 0x80 = Switch (ESI=2).
#define SF_TYPE_RANDOM     SF_STREAM       // = 0x00000100u (intentional per Ubisoft)
#define SF_TYPE_SEQUENCE   0x00000800u
#define SF_TYPE_SWITCH     0x00000080u
#define SF_TYPE_SURROUND   0x00002000u

// FUN_10e7db38 early-exits on these flags; Patch 3 in Initialize() NOPs those branches.
// SF_UAS_STREAM: UAX contains only a stub - actual audio is in the map's .uas file; Export won't yield usable audio.
// SF_XBOXHD_STREAM: WAV data IS present in the UAX; Ubisoft hid these from the PC browser because the PC build has no .hds pipeline.
#define SF_UAS_STREAM      0x00000004u    // OGG/UAS external stream stub (no WAV in UAX)
#define SF_XBOXHD_STREAM   0x00004000u    // Xbox HD Stream (.hds); suppressed from PC editor browser

static inline float LinearToDB(float linear)
{
    if (linear <= 0.0f) return -96.0f;
    return 20.0f * log10f(linear);
}
static inline float DBToLinear(float db)
{
    return powf(10.0f, db / 20.0f);
}

struct ImportSoundData
{
    char filePath [MAX_PATH * 2];   // full path to the WAV (display only in dialog)
    char pkgName  [256];            // Package: edit field (1067)
    char grpName  [256];            // Group:   edit field (1066)
    char soundName[256];            // Name:    edit field (1065)
};

struct RenameSoundData
{
    char oldName   [256];
    char oldGroup  [256];
    char oldPackage[256];
    char newName   [256];
    char newGroup  [256];
    char newPackage[256];
};

struct CompositePropsCtx {
    void* pSound;    // USound* being edited
    void* pBrowser;  // WBrowserSound* (this_ptr) – needed to enumerate available sounds
};

struct RandomElemCtx {
    void* pBrowser;    // WBrowserSound* for combo population
    void* pParent;     // USound* of parent Random (excluded from picker)
    void* pChildSound; // in: current child USound* (NULL = new); out: selected
    int   nWeight;     // in: current weight (integer); out: updated weight
};
struct SeqElemCtx {
    void* pBrowser;
    void* pParent;     // USound* of parent Sequence (excluded from picker)
    void* pChildSound; // in/out
    int   nRepeat;     // in/out: times to play (normal) OR seconds to loop (timed)
    BOOL  bTimedLoop;  // in/out: if TRUE repeat field is seconds, encoded as SEQ_TIMED_LOOP_FLAG|secs
};

#define MAX_COMPOSITE_CHILDREN 64

struct RandomChildInfo {
    void* pSound;      // USound* of child
    int   nWeight;     // integer weight (shown as % of total in UI)
    char  szName[64];  // display name fetched from browser ListView
};
struct SeqChildInfo {
    void* pSound;
    int   nRepeat;     // times to play (bTimedLoop=FALSE) OR seconds (bTimedLoop=TRUE)
    BOOL  bTimedLoop;  // if TRUE, engine value is SEQ_TIMED_LOOP_FLAG | nRepeat
    char  szName[64];
};

struct RandomPropsCtx {
    void* pSound;
    void* pBrowser;
    int   nCount;
    RandomChildInfo children[MAX_COMPOSITE_CHILDREN];
};
struct SeqPropsCtx {
    void* pSound;
    void* pBrowser;
    int   nCount;
    SeqChildInfo children[MAX_COMPOSITE_CHILDREN];
};

struct SwitchElemCtx {
    void* pBrowser;
    void* pParent;       // USound* of parent Switch (excluded from sound picker)
    void* pChildSound;   // in: current child (NULL = new); out: selected
    int   nSurfaceType;  // in/out: ESurfaceType index (0-25)
};

struct SwitchChildInfo {
    void* pSound;
    int   nSurfaceType;  // ESurfaceType enum value (0-25)
    char  szName[64];
};

struct SwitchPropsCtx {
    void* pSound;
    void* pBrowser;
    int   nCount;
    SwitchChildInfo children[SW_NUM_SURFACE_TYPES]; // max one entry per surface type
};

//   TArray<int> at USound+0x78: Data[0] = channel bitmask
//     bit 0 (0x01) = FL    bit 1 (0x02) = FR    bit 2 (0x04) = BL
//     bit 3 (0x08) = BR    bit 4 (0x10) = Center bit 5 (0x20) = LFE
//   Channel sound names: stored in <name>.mux via GetPrivateProfileStringA,
//     section "Channels", keys "FL"/"FR"/"BL"/"BR"/"Center"/"LFE".
#define SURROUND_NUM_CHANNELS 6
static const char* const kSurroundKeys[SURROUND_NUM_CHANNELS] =
    { "FL", "FR", "BL", "BR", "Center", "LFE" };
static const DWORD kSurroundBits[SURROUND_NUM_CHANNELS] =
    { 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u };

struct SurroundPropsCtx {
    void*  pSound;
    void*  pBrowser;
    DWORD  channelMask;                         // bitmask from TArray<int>.Data[0]
    char   szNames[SURROUND_NUM_CHANNELS][128]; // bare FName strings from .mux
    char   szMuxPath[MAX_PATH];                 // absolute path to the .mux file
};

// Picker context: reuses IDD_RANDOM_ELEM combo, hides the weight field.
struct SurroundPickerCtx {
    void* pBrowser;
    void* pParent;    // USound* excluded from combo (the surround sound itself)
    void* pSelected;  // out: chosen USound* (NULL = cancelled)
};

// USound* -> display name; populated by StoreLParamHelper for cross-package/group resolution
static std::unordered_map<void*, std::string> g_soundNameCache;

// Forward declarations
static void  __cdecl StoreLParamHelper    (void* this_ptr, int item_index, void* usound_ptr, const char* name);
static void* __cdecl GetSelectedSound     (void* this_ptr);
static void  __cdecl ShowPropertiesDialogHelper(void* this_ptr);
static INT_PTR CALLBACK SoundPropsDlgProc  (HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK ImportSoundDlgProc (HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK RenameSoundDlgProc (HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK UASPkgPickerDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

static void  __cdecl ExecEditorCommand      (const char* cmd);
static void  __cdecl CallEditorGet          (const char* section, const char* key);
static void  __cdecl RefreshPackages        (void* this_ptr);
static void  __cdecl RefreshGroups          (void* this_ptr);
static void  __cdecl RefreshList            (void* this_ptr);
static void           NavigateToPackageGroup(void* this_ptr, const char* pkg, const char* grp);
static void           NavigateAfterDelete   (void* this_ptr);
static void  __cdecl SB_HandleSave        (void* this_ptr);
static void  __cdecl SB_HandleMakeUAS     (void* this_ptr);
static void  __cdecl SB_HandleExportUAS   (void* this_ptr);
static void  __cdecl SB_HandleExport      (void* this_ptr);
static void  __cdecl SB_HandleImport      (void* this_ptr);
static void  __cdecl SB_HandleDelete      (void* this_ptr);
static void  __cdecl SB_HandleRename      (void* this_ptr);

// Composite sound support
static bool              WriteSilentWAV      (char* outPath, size_t pathSize);
static void              GetSoundName        (void* pBrowser, void* pSound, char* outBuf, int maxLen);
static void              PopulateComboWithSounds(void* pBrowser, HWND hCombo, void* excludeSound);
static void*             FindSoundByName     (void* this_ptr, const char* name);
static void              RandList_Insert     (void* stRes, void* elem);
static void              SeqList_Insert      (void* stRes, void* elem);
static void              RandProps_RefreshList(HWND hDlg, RandomPropsCtx* ctx);
static void              SeqProps_RefreshList (HWND hDlg, SeqPropsCtx*   ctx);
static INT_PTR CALLBACK  CreateResourceDlgProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  RandomPropsDlgProc   (HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  RandomElemDlgProc    (HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  SeqPropsDlgProc      (HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  SeqElemDlgProc       (HWND, UINT, WPARAM, LPARAM);
static void __cdecl      SB_HandleNew         (void* this_ptr);
static void __cdecl      SB_HandleRandomProps (void* this_ptr);
static void __cdecl      SB_HandleSwitchProps (void* this_ptr);
static void __cdecl      SB_HandleSeqProps    (void* this_ptr);
static INT_PTR CALLBACK  SwitchPropsDlgProc   (HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  SwitchElemDlgProc    (HWND, UINT, WPARAM, LPARAM);
static void              GetSoundFName         (void* pSound, char* outBuf, int maxLen);
static void              GetMuxPath            (void* pSound, char* outPath, int maxLen);
static INT_PTR CALLBACK  SurroundPickerDlgProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK  SurroundPropsDlgProc (HWND, UINT, WPARAM, LPARAM);
static void __cdecl      SB_HandleSurroundProps(void* this_ptr);
#ifdef _DEBUG
// Debug only: enumerate all sounds in current package (including delisted ones)
static void __cdecl      SB_ShowFlagsDump     (void* this_ptr);
#endif

static HWND GetParentHWND    (void* t) { return *reinterpret_cast<HWND*>(static_cast<char*>(t) + 0x04); }
static HWND GetPackageComboHWND(void* t)
{
    void* obj = *reinterpret_cast<void**>(static_cast<char*>(t) + 0x8c);
    return *reinterpret_cast<HWND*>(static_cast<char*>(obj) + 4);
}
static HWND GetGroupComboHWND(void* t)
{
    void* obj = *reinterpret_cast<void**>(static_cast<char*>(t) + 0x90);
    return *reinterpret_cast<HWND*>(static_cast<char*>(obj) + 4);
}
static HWND GetListHWND(void* t)
{
    void* obj = *reinterpret_cast<void**>(static_cast<char*>(t) + 0x94);
    return *reinterpret_cast<HWND*>(static_cast<char*>(obj) + 4);
}


// Hook 1 @ 0x10E7DD57: displaced: XOR ECX,ECX / MOV [EBP+0Ch],EAX; return 0x10E7DD5C
JMP_HOOK(0x10E7DD57, StoreSoundLParam) {
    static int Return = 0x10E7DD5C;
    __asm {
        push eax                       // save item_index
        push dword ptr [ebp+0x8]       // arg4: name char* (= pszText passed to this fn)
        push ebx                       // arg3: USound*
        push eax                       // arg2: item_index
        push dword ptr [ebp-0x54]      // arg1: this_ptr
        call StoreLParamHelper
        add  esp, 16                   // 4 args × 4 bytes
        pop  eax                       // restore item_index
        xor  ecx, ecx                  // re-execute displaced byte 1
        mov  dword ptr [ebp+0xC], eax  // re-execute displaced bytes 2-4
        jmp  dword ptr [Return]
    }
}


// Hook 2 @ 0x10E7E41E (SEH epilogue in OnCommand): displaced: MOV ECX,[EBP-0Ch] + first 2 bytes of MOV FS:[0],ECX; return 0x10E7E428
JMP_HOOK(0x10E7E41E, ShowSoundProperties) {
    static int Return = 0x10E7E428;
    __asm {
        cmp  dword ptr [ebp+0x8], 0x9F  // only act for Properties command
        jne  skip_props
        push esi                         // arg: this
        call ShowPropertiesDialogHelper
        add  esp, 4
    skip_props:
        mov  ecx, dword ptr [ebp-0xC]   // re-execute displaced: MOV ECX,[EBP-0Ch]
        mov  dword ptr fs:[0], ecx       // re-execute displaced: MOV FS:[0],ECX
        jmp  dword ptr [Return]
    }
}


// Hook 3 @ 0x10E7E690: displaced: PUSH EAX / MOV ECX,ESI / first 2 bytes of CALL 0x10E069A2; return 0x10E7E698
JMP_HOOK(0x10E7E690, SoundBrowserCommands) {
    static int OrigFn = 0x10E069A2;
    static int Return = 0x10E7E698;
    __asm {
        cmp  eax, CMD_SAVE
        je   do_save
        cmp  eax, CMD_EXPORT
        je   do_export
        cmp  eax, CMD_IMPORT
        je   do_import
        cmp  eax, CMD_DELETE
        je   do_delete
        cmp  eax, CMD_RENAME
        je   do_rename
        cmp  eax, CMD_NEW
        je   do_new
        cmp  eax, CMD_RANDOM_PROPS
        je   do_rand_props
        cmp  eax, CMD_SWITCH_PROPS
        je   do_switch_props
        cmp  eax, CMD_SEQ_PROPS
        je   do_seq_props
        cmp  eax, CMD_BUILD_UAS
        je   do_build_uas
        cmp  eax, CMD_EXPORT_UAS
        je   do_export_uas
        cmp  eax, CMD_SURROUND_PROPS
        je   do_surround_props

        // Pass-through: re-execute displaced block, call parent handler
        push eax                        // displaced: PUSH EAX (cmd_id)
        mov  ecx, esi                   // displaced: MOV ECX, ESI (this)
        call dword ptr [OrigFn]         // displaced: CALL 0x10E069A2 (cleans arg itself)
        jmp  dword ptr [Return]         // > 0x10E7E698

    do_save:
        push esi                        // arg: this_ptr
        call SB_HandleSave
        add  esp, 4
        jmp  dword ptr [Return]

    do_export:
        push esi
        call SB_HandleExport
        add  esp, 4
        jmp  dword ptr [Return]

    do_import:
        push esi
        call SB_HandleImport
        add  esp, 4
        jmp  dword ptr [Return]

    do_delete:
        push esi
        call SB_HandleDelete
        add  esp, 4
        jmp  dword ptr [Return]

    do_rename:
        push esi
        call SB_HandleRename
        add  esp, 4
        jmp  dword ptr [Return]

    do_new:
        push esi
        call SB_HandleNew
        add  esp, 4
        jmp  dword ptr [Return]

    do_rand_props:
        push esi
        call SB_HandleRandomProps
        add  esp, 4
        jmp  dword ptr [Return]

    do_switch_props:
        push esi
        call SB_HandleSwitchProps
        add  esp, 4
        jmp  dword ptr [Return]

    do_seq_props:
        push esi
        call SB_HandleSeqProps
        add  esp, 4
        jmp  dword ptr [Return]

    do_build_uas:
        push esi
        call SB_HandleMakeUAS
        add  esp, 4
        jmp  dword ptr [Return]

    do_export_uas:
        push esi
        call SB_HandleExportUAS
        add  esp, 4
        jmp  dword ptr [Return]

    do_surround_props:
        push esi
        call SB_HandleSurroundProps
        add  esp, 4
        jmp  dword ptr [Return]
    }
}


// GEditor+0x28 = FExec sub-object; vtable[0] = FExec::Exec (2-arg, callee-cleanup)
static void __cdecl ExecEditorCommand(const char* cmd)
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    // FExec sub-object is at GEditor + 0x28; its vtable[0] = FExec::Exec
    void* fexec   = static_cast<char*>(gEditor) + 0x28;
    void* vtable  = *reinterpret_cast<void**>(fexec);
    void* execFn  = *reinterpret_cast<void**>(vtable);    // vtable[0]
    void* logDev  = *reinterpret_cast<void**>(EXEC_LOG_DEV); // *PTR_PTR_115befb0
    __asm {
        push logDev         // arg2: GLog output device
        push cmd            // arg1: ANSI command string
        mov  ecx, fexec     // __thiscall this = GEditor+0x28 (FExec*)
        mov  eax, execFn
        call eax            // FExec::Exec - callee cleans 2*4 = 8 bytes
    }
}


// UEditorEngine::Get via vtable[0x224/4] on GEditor (NOT FExec); used for DELETE CLASS=SOUND
static void __cdecl CallEditorGet(const char* section, const char* key)
{
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    void* vtable  = *reinterpret_cast<void**>(gEditor);
    void* getFn   = *reinterpret_cast<void**>(static_cast<char*>(vtable) + EDITOR_GET_VTABLE_OFFSET);
    void* logDev  = *reinterpret_cast<void**>(EXEC_LOG_DEV); // FOutputDevice*
    __asm {
        push logDev    // arg3: FOutputDevice& (passed as pointer)
        push key       // arg2: key / command string
        push section   // arg1: section string
        mov  ecx, gEditor
        mov  eax, getFn
        call eax       // UEditorEngine::Get - callee cleans 3*4 = 12 bytes
    }
}


static void __cdecl RefreshPackages(void* this_ptr)
{
    void* fn = reinterpret_cast<void*>(REFRESH_PACKAGES_THUNK);
    __asm {
        mov  ecx, this_ptr
        mov  eax, fn
        call eax
    }
}
static void __cdecl RefreshGroups(void* this_ptr)
{
    void* fn = reinterpret_cast<void*>(REFRESH_GROUPS_THUNK);
    __asm {
        mov  ecx, this_ptr
        mov  eax, fn
        call eax
    }
}
static void __cdecl RefreshList(void* this_ptr)
{
    void* fn = reinterpret_cast<void*>(REFRESH_LIST_THUNK);
    __asm {
        mov  ecx, this_ptr
        mov  eax, fn
        call eax
    }
}

static void NavigateToPackageGroup(void* this_ptr, const char* pkg, const char* grp)
{
    if (!pkg || pkg[0] == '\0') return;

    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    RefreshPackages(this_ptr);

    LRESULT pkgIdx = SendMessageA(hPkgCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)pkg);
    if (pkgIdx != CB_ERR)
        SendMessageA(hPkgCombo, CB_SETCURSEL, (WPARAM)pkgIdx, 0);

    RefreshGroups(this_ptr);

    if (grp && grp[0] != '\0')
    {
        LRESULT grpIdx = SendMessageA(hGrpCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)grp);
        if (grpIdx != CB_ERR)
            SendMessageA(hGrpCombo, CB_SETCURSEL, (WPARAM)grpIdx, 0);
    }

    RefreshList(this_ptr);
}


// Uses list item count (not CB_GETCOUNT on the group combo) as ground truth;
// RefreshGroups may not clear before repopulating, making CB_GETCOUNT unreliable.
static void NavigateAfterDelete(void* this_ptr)
{
    HWND hList     = GetListHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    if (SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0) > 0)
        return;

    RefreshGroups(this_ptr);
    RefreshList(this_ptr);

    if (SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0) > 0)
        return;

    // Whole package is empty - step to adjacent package and remove it from combo.
    // UPackage persists in memory so PACKAGES CLASS=Sound keeps returning it;
    // CB_DELETESTRING removes it visually until a full RefreshPackages rebuilds the list.
    LRESULT pkgCount = SendMessageA(hPkgCombo, CB_GETCOUNT, 0, 0);
    LRESULT curPkg   = SendMessageA(hPkgCombo, CB_GETCURSEL, 0, 0);
    if (curPkg == CB_ERR) curPkg = 0;

    LRESULT targetPkg;
    if (curPkg + 1 < pkgCount)
        targetPkg = curPkg + 1;
    else if (curPkg > 0)
        targetPkg = curPkg - 1;
    else
        targetPkg = 0;

    SendMessageA(hPkgCombo, CB_DELETESTRING, (WPARAM)curPkg, 0);
    if (targetPkg > curPkg)
        targetPkg--;

    SendMessageA(hPkgCombo, CB_SETCURSEL, (WPARAM)targetPkg, 0);
    RefreshGroups(this_ptr);
    RefreshList(this_ptr);
}


static void __cdecl SB_HandleSave(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    char pkgName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    if (pkgName[0] == '\0')
    {
        MessageBoxA(hParent, "Select a package first.", "Message",
                    MB_OK);
        return;
    }

    char filePath[MAX_PATH * 2] = {};
    snprintf(filePath, sizeof(filePath), "%s.uax", pkgName);

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hParent;
    ofn.lpstrFilter = "Sound Packages (*.uax)\0*.uax\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = "uax";
    ofn.lpstrFile   = filePath;
    ofn.nMaxFile    = sizeof(filePath);
    ofn.lpstrTitle  = "Save Sound Package";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&ofn))
        return;

    char cmd[MAX_PATH * 2 + 128];
    snprintf(cmd, sizeof(cmd),
             "OBJ SAVEPACKAGE PACKAGE=\"%s\" FILE=\"%s\"",
             pkgName, filePath);
    ExecEditorCommand(cmd);
}


// === Build UAS pipeline ============================================================
//
// Ghidra-verified call shape of UpdateStreamFile (0x10F77AC0), __thiscall:
//
//   char UpdateStreamFile(this, int packageList, int platform, int format, float quality)
//
//   packageList :  0  > scan audio subsystem's internal package list at [this+0x2c]
//                       and filter sounds to those with SF_UAS_STREAM (0x4)
//                  1  > filter sounds to SF_XBOXHD_STREAM (0x4000) only
//                  2  > SF_UAS_STREAM AND NOT SF_XBOXHD_STREAM
//                  ptr > BROKEN; the flag-filter else-branch skips ALL sounds when this
//                        isn't one of {0,1,2}, so caller-supplied TArrays don't work.
//                        We must instead pre-populate [this+0x2c] / [this+0x30].
//
//   platform   :  != 2 > "..\\packages\\sounds\\<map>.uas"  (PC path)
//                 == 2 > "..\\packages\\soundsxbox\\<map>.*" (Xbox build)
//
//   format     :  0 > raw WAV from "..\\packages\\sounds\\<name>.wav"
//                 1 > OGG from "..\\packages\\sounds\\stemp\\<name>.ogg"
//                     (calls EncodeSound > CreateProcess("oggenc %s -q %.1f -o %s"))
//                 2 > XB ADPCM from "..\\packages\\sounds\\stemp\\<name>.xb"
//                     (calls EncodeSound > CreateProcess("xbadpcmencode.exe %s %s"))
//
//   quality    :  -q value passed to oggenc.exe (5.0 is the Ubisoft-shipped default).
//                 Ignored for format != 1.
//
// Engine call sites:
//   ExportToXBox @ 0x10F71E42: UpdateStreamFile(this, 0, 2, 1, 5.0f) > soundsxbox\<map>.uas OGG
//   ExportToXBox @ 0x10F71E6F: UpdateStreamFile(this, 0, 2, 2, 5.0f) > soundsxbox\<map>.hds XB
//   AUDIO UPDATEBIGFILE      : UpdateStreamFile(this, 0, !lowQ, 0, q) > sounds\<map>.uas WAV (PC)
//
// Neither shipped path produces "PC + OGG", which is what we want for SCCT Versus on PC.
// Our build calls (this, 0, 1, 1, 5.0f) directly: PC path + OGG content + quality 5.
//
// The previous Reloaded.Editor implementation had an asm push-order bug: it pushed
// (qualBits, 2, fmt, 0) which the callee read as (packageList=0, platform=fmt, format=2,
// quality=qualBits) - so "OGG mode" actually requested XB ADPCM, and stemp\*.xb files
// never existed, producing a ~1 KB UAS stub. Fixed below.
//
// ExportToXBox's PHASE 2 (calling UpdateStreamFile) is dead-code in the shipped binary
// because local_50 is zeroed at 0x10F71BAB and the JGE at 0x10F71C30 always skips past
// the calls. We invoke UpdateStreamFile directly via SB_HandleMakeUAS.
#define AUDIO_SUBSYSTEM_PTR  0x1168D290u
#define UPDATE_STREAM_FILE   0x10F77AC0u

// USound class object pointer (used to walk class chain in GObjects scans).
#define USOUND_CLASS_PTR     0x11823128u

// Audio subsystem internal package list: TArray-like layout where each entry is a
// zero-padded ASCII name of 0x4C bytes. Populated by AUDIO SETMAP and ExportToXBox.
// We temporarily swap our own buffer in for the duration of UpdateStreamFile.
#define AUDIO_PKGLIST_DATA_OFFSET   0x2Cu   // entry array pointer
#define AUDIO_PKGLIST_COUNT_OFFSET  0x30u   // entry count
#define AUDIO_PKGLIST_ENTRY_SIZE    0x4Cu   // 76 bytes per entry (Ghidra-verified)

// UpdateStreamFile writes its output to "..\\Packages\\Sounds\\temp_<map>.uas" first
// and renames it to "<map>.uas" on success via internal MoveFileA. Clean any stale
// temp_*.uas from a previous failed run; we don't touch .hds (Xbox-only, irrelevant on PC).
static void USF_ClearTempFiles()
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("..\\Packages\\Sounds\\temp_*.uas", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        char tempPath[MAX_PATH];
        snprintf(tempPath, sizeof(tempPath), "..\\Packages\\Sounds\\%s", fd.cFileName);
        DeleteFileA(tempPath);
    }
    while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

static bool UAS_GetObjectFName(uintptr_t obj, char* buf, int bufSize)
{
    if (!obj) return false;
    int nameIdx = *reinterpret_cast<int*>(obj + UOBJ_FNAME_OFFSET);
    uintptr_t gBase = *reinterpret_cast<uintptr_t*>(GNAMES_DATA);
    int       gNum  = *reinterpret_cast<int*>(GNAMES_NUM);
    if (nameIdx < 0 || nameIdx >= gNum) return false;
    uintptr_t entry = *reinterpret_cast<uintptr_t*>(gBase + (uintptr_t)nameIdx * 4);
    if (!entry) return false;
    const char* name = reinterpret_cast<const char*>(entry + FNAME_ENTRY_STR_OFFSET);
    strncpy_s(buf, bufSize, name, _TRUNCATE);
    return true;
}

static bool UAS_GetPackageName(uintptr_t obj, char* buf, int bufSize)
{
    buf[0] = '\0';
    if (!obj) return false;
    uintptr_t cur = obj;
    for (int guard = 0; guard < 16; ++guard)
    {
        uintptr_t outer = *reinterpret_cast<uintptr_t*>(cur + UOBJ_OUTER_OFFSET);
        if (!outer || outer == cur) break;
        cur = outer;
    }
    return UAS_GetObjectFName(cur, buf, bufSize);
}

#define IDC_UASPKG_LIST        200
#define IDC_UASPKG_ALL         201
#define IDC_UASPKG_NONE        202

#define IDC_UAS_QUALITY_SLIDER 1332
#define IDC_UAS_QUALITY_LABEL  1333

struct DlgBldBuf {
    std::vector<uint8_t> buf;
    void a2() { while (buf.size() & 1) buf.push_back(0); }
    void a4() { while (buf.size() & 3) buf.push_back(0); }
    void w (WORD  v) { buf.push_back(v & 0xFF); buf.push_back(v >> 8); }
    void dw(DWORD v) { w((WORD)v); w((WORD)(v >> 16)); }
    void ws(const wchar_t* s) { for (; *s; ++s) w((WORD)*s); w(0); }
    void atom(WORD a) { w(0xFFFF); w(a); }
};

// OGG quality picker. WAV/Uncompressed deliberately not offered: PC SCCT Versus
// only accepts OGG-encoded UAS streams; uncompressed PCM payloads cause crashes
// when the engine's OGG decoder hits the end of a "stream."
struct UASCompCtx {
    int  quality;   // out: slider pos 0–100, representing 0.0–10.0 in 0.1 steps
    bool cancelled; // out: true if user hit Cancel
};

static std::vector<uint8_t> BuildUASCompDlgTemplate()
{
    DlgBldBuf b;

    DWORD dlgStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU
                   | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    b.dw(dlgStyle);
    b.dw(0);            // exStyle
    b.w(4);             // cdit = 4 controls (label, slider, OK, Cancel)
    b.w(0); b.w(0);     // x, y
    b.w(200); b.w(72);  // cx, cy
    b.w(0);             // no menu
    b.w(0);             // default dialog class
    b.ws(L"OGG Vorbis");
    b.w(8);
    b.ws(L"MS Sans Serif");

    // Quality label
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | SS_LEFT);
    b.dw(0);
    b.w(7); b.w(8); b.w(186); b.w(10);
    b.w(IDC_UAS_QUALITY_LABEL);
    b.atom(0x0082);   // STATIC
    b.ws(L"Quality: 5.0");
    b.w(0);

    // Quality slider
    b.a4();
    b.dw(TBS_HORZ | TBS_AUTOTICKS | WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    b.dw(0);
    b.w(5); b.w(22); b.w(190); b.w(20);
    b.w(IDC_UAS_QUALITY_SLIDER);
    b.ws(L"msctls_trackbar32");
    b.ws(L"");
    b.w(0);

    // OK
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(34); b.w(52); b.w(60); b.w(14);
    b.w(IDOK);
    b.atom(0x0080);
    b.ws(L"OK");
    b.w(0);

    // Cancel
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(106); b.w(52); b.w(60); b.w(14);
    b.w(IDCANCEL);
    b.atom(0x0080);
    b.ws(L"Cancel");
    b.w(0);

    return b.buf;
}

static INT_PTR CALLBACK UASCompDlgProc(HWND hDlg, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    UASCompCtx* ctx = reinterpret_cast<UASCompCtx*>(
        GetWindowLongPtrA(hDlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ctx = reinterpret_cast<UASCompCtx*>(lParam);
        SetWindowLongPtrA(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        {
            HWND hSlider = GetDlgItem(hDlg, IDC_UAS_QUALITY_SLIDER);
            SendMessageA(hSlider, TBM_SETRANGE,   TRUE, MAKELPARAM(0, 100));
            SendMessageA(hSlider, TBM_SETTICFREQ, 10,   0);
            SendMessageA(hSlider, TBM_SETPOS,     TRUE, static_cast<LPARAM>(ctx->quality));
            char label[64];
            snprintf(label, sizeof(label), "Quality: %.1f", ctx->quality / 10.0);
            SetDlgItemTextA(hDlg, IDC_UAS_QUALITY_LABEL, label);
        }
        return TRUE;

    case WM_HSCROLL:
        {
            HWND hSlider = GetDlgItem(hDlg, IDC_UAS_QUALITY_SLIDER);
            int pos = static_cast<int>(SendMessageA(hSlider, TBM_GETPOS, 0, 0));
            char label[64];
            snprintf(label, sizeof(label), "Quality: %.1f", pos / 10.0);
            SetDlgItemTextA(hDlg, IDC_UAS_QUALITY_LABEL, label);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            {
                HWND hSlider = GetDlgItem(hDlg, IDC_UAS_QUALITY_SLIDER);
                ctx->quality   = static_cast<int>(SendMessageA(hSlider, TBM_GETPOS, 0, 0));
                ctx->cancelled = false;
                EndDialog(hDlg, IDOK);
            }
            return TRUE;
        case IDCANCEL:
            ctx->cancelled = true;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static std::vector<uint8_t> BuildUASPkgDlgTemplate()
{
    DlgBldBuf b;

    DWORD dlgStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU
                   | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    b.dw(dlgStyle);
    b.dw(0);
    b.w(6);
    b.w(0); b.w(0);
    b.w(260); b.w(190);
    b.w(0);
    b.w(0);
    b.ws(L"Message");
    b.w(8);
    b.ws(L"MS Shell Dlg");

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | SS_LEFT);
    b.dw(0);
    b.w(7); b.w(5); b.w(246); b.w(10);
    b.w(0xFFFF);
    b.atom(0x0082);
    b.ws(L"Select packages to include in the UAS:");
    b.w(0);

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP
       | LBS_MULTIPLESEL | LBS_HASSTRINGS | LBS_NOTIFY);
    b.dw(0);
    b.w(7); b.w(18); b.w(246); b.w(130);
    b.w(IDC_UASPKG_LIST);
    b.atom(0x0083);
    b.ws(L"");
    b.w(0);

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(7); b.w(162); b.w(58); b.w(14);
    b.w(IDC_UASPKG_ALL);
    b.atom(0x0080);
    b.ws(L"Select All");
    b.w(0);

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(68); b.w(162); b.w(58); b.w(14);
    b.w(IDC_UASPKG_NONE);
    b.atom(0x0080);
    b.ws(L"Clear All");
    b.w(0);

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(147); b.w(162); b.w(50); b.w(14);
    b.w(IDOK);
    b.atom(0x0080);
    b.ws(L"Build");
    b.w(0);

    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(200); b.w(162); b.w(53); b.w(14);
    b.w(IDCANCEL);
    b.atom(0x0080);
    b.ws(L"Cancel");
    b.w(0);

    return b.buf;
}

struct UASPkgPickerCtx {
    const std::vector<std::string>* packages;  // in:  ordered package list
    std::vector<bool>               selected;  // out: per-index selection result
    bool                            cancelled; // out: true if user hit Cancel
};

static INT_PTR CALLBACK UASPkgPickerDlgProc(HWND hDlg, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    UASPkgPickerCtx* ctx = reinterpret_cast<UASPkgPickerCtx*>(
        GetWindowLongPtrA(hDlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
        ctx = reinterpret_cast<UASPkgPickerCtx*>(lParam);
        SetWindowLongPtrA(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        {
            HWND hList = GetDlgItem(hDlg, IDC_UASPKG_LIST);
            const std::vector<std::string>& pkgs = *ctx->packages;
            for (size_t i = 0; i < pkgs.size(); ++i)
            {
                int idx = static_cast<int>(
                    SendMessageA(hList, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(pkgs[i].c_str())));
                // Honour caller-supplied defaults (Interface + Amb_<map>).
                bool preselect = (i < ctx->selected.size() && ctx->selected[i]);
                SendMessageA(hList, LB_SETSEL, preselect ? TRUE : FALSE,
                             static_cast<LPARAM>(idx));
            }
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            {
                HWND hList = GetDlgItem(hDlg, IDC_UASPKG_LIST);
                int n = static_cast<int>(SendMessageA(hList, LB_GETCOUNT, 0, 0));
                ctx->selected.resize(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i)
                    ctx->selected[static_cast<size_t>(i)] =
                        (SendMessageA(hList, LB_GETSEL,
                                      static_cast<WPARAM>(i), 0) > 0);
                ctx->cancelled = false;
                EndDialog(hDlg, IDOK);
            }
            return TRUE;

        case IDCANCEL:
            ctx->cancelled = true;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case IDC_UASPKG_ALL:
            SendMessageA(GetDlgItem(hDlg, IDC_UASPKG_LIST),
                         LB_SETSEL, TRUE,  static_cast<LPARAM>(-1));
            return TRUE;

        case IDC_UASPKG_NONE:
            SendMessageA(GetDlgItem(hDlg, IDC_UASPKG_LIST),
                         LB_SETSEL, FALSE, static_cast<LPARAM>(-1));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL CALLBACK FindMapLabelProc(HWND hWnd, LPARAM lParam)
{
    char buf[256];
    if (GetWindowTextA(hWnd, buf, sizeof(buf)) > 0 &&
        strncmp(buf, "Map: ", 5) == 0)
    {
        strncpy_s(reinterpret_cast<char*>(lParam), 256, buf, _TRUNCATE);
        return FALSE;   // found – stop enumeration
    }
    return TRUE;        // keep looking
}

// EnumLoadedSoundPackages - walk GObjects, find every UPackage that owns at least one
// SF_UAS_STREAM-flagged USound. These are the packages that contribute audio to a UAS.
// Output is alphabetically de-duplicated.
static std::vector<std::string> UAS_EnumLoadedSoundPackages()
{
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    void** gobjData  = *reinterpret_cast<void***>(GOBJECTS_DATA);
    int    gobjCount = *reinterpret_cast<int*>   (GOBJECTS_NUM);
    if (!gobjData || gobjCount <= 0) return result;

    const uintptr_t kUSoundClass = USOUND_CLASS_PTR;

    for (int i = 0; i < gobjCount; ++i)
    {
        uintptr_t obj = reinterpret_cast<uintptr_t>(gobjData[i]);
        if (!obj) continue;

        // Class chain walk: object + 0x24 = class, class + 0x28 = parent class
        uintptr_t cls = *reinterpret_cast<uintptr_t*>(obj + 0x24);
        bool isSound = false;
        for (int d = 0; cls && d < 16; ++d)
        {
            if (cls == kUSoundClass) { isSound = true; break; }
            cls = *reinterpret_cast<uintptr_t*>(cls + 0x28);
        }
        if (!isSound) continue;

        uint32_t flags = *reinterpret_cast<uint32_t*>(obj + USOUND_FLAGS_OFFSET);
        if (!(flags & SF_UAS_STREAM)) continue;

        char pkgName[256] = {};
        if (!UAS_GetPackageName(obj, pkgName, sizeof(pkgName))) continue;
        if (pkgName[0] == '\0') continue;

        if (seen.insert(pkgName).second)
            result.emplace_back(pkgName);
    }

    std::sort(result.begin(), result.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    return result;
}

// OGG encoding pre-pass.
//
// EncodeSound (called internally by UpdateStreamFile) shells out via
//   CreateProcessA("oggenc <wav> -q <q> -o <ogg>")
// with no full path, so the editor only finds oggenc if it's on the system PATH -
// which it usually isn't. We work around that by pre-running oggenc ourselves with
// the full "System\oggenc.exe" path. The engine's IsSourceFileOlderThanOutput
// staleness check (OGG newer than WAV > skip) then short-circuits the re-encode,
// and UpdateStreamFile just packages our pre-built OGGs.
//
// WAV lookup order (first hit wins):
//   ..\\packages\\sounds\\<name>.wav             (flat - matches engine's EncodeSound)
//   ..\\packages\\sounds\\<pkg>\\<name>.wav      (by-package - user convenience)
//
// OGG output is always written flat to ..\\packages\\sounds\\stemp\\<name>.ogg (which
// is exactly where the engine's stemp\<name>.ogg reader looks).
//
// Returns number of successful encodes; missing WAVs are silently skipped - the
// staleness check will then fail for them and oggenc will be invoked by the engine
// (which will also fail without PATH, but that's the user's responsibility to fix).
struct UAS_PkgFilter {
    std::unordered_set<std::string> ciNames;
    bool empty() const { return ciNames.empty(); }
    bool contains(const char* name) const {
        if (!name) return false;
        std::string lc(name);
        for (auto& c : lc) c = (char)tolower((unsigned char)c);
        return ciNames.count(lc) != 0;
    }
    void add(const std::string& s) {
        std::string lc(s);
        for (auto& c : lc) c = (char)tolower((unsigned char)c);
        ciNames.insert(lc);
    }
};

// Per-sound outcome of the OGG pre-pass. Reported back to the user in the
// Build UAS success dialog so they can see exactly which WAVs were missing
// or which oggenc.exe invocations failed.
enum class UAS_SoundStatus {
    EncodedOk,        // .ogg produced this run by oggenc.exe
    UserOggCopied,    // user supplied <name>.ogg next to the WAV - copied as-is
    StempOggReused,   // no WAV source, but an existing stemp\*.ogg was found - reused
    WavMissing,       // no WAV source AND no pre-built .ogg
    OggencFailed,     // CreateProcess succeeded but oggenc.exe returned non-zero
    OggencNotRun,     // CreateProcess for System\oggenc.exe failed (oggenc missing?)
};

struct UAS_SoundReport {
    std::string                package;
    std::string                name;
    uint32_t                   flags;     // raw USound+0x60 dword
    UAS_SoundStatus            status;
    std::string                wavPath;   // resolved WAV path, or "" if missing
    DWORD                      exitCode;  // oggenc.exe exit code (0 = OK)
    long long                  wavSize;   // source WAV bytes (-1 if missing)
    long long                  oggSize;   // resulting OGG bytes (-1 if missing)
};

// Resolve the absolute path of oggenc.exe sitting next to ChaosTheory_Editor.exe.
// The Reloaded.Editor DLL is loaded INTO ChaosTheory_Editor.exe, so the exe's
// own directory is what GetModuleFileNameA(NULL, ...) returns. We can't trust
// CWD here - when the editor is launched via shortcut or the launcher, CWD is
// often the install root (one level up from System\), so a relative path like
// "System\\oggenc.exe" then resolves to "System\\System\\oggenc.exe" > not found.
//
// Returns empty string on failure.
static std::string UAS_LocateOggenc()
{
    char modPath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, modPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();

    // Strip the exe filename > directory + trailing backslash
    char* lastSlash = strrchr(modPath, '\\');
    if (!lastSlash) return std::string();
    *(lastSlash + 1) = '\0';

    std::string oggencPath = modPath;
    oggencPath += "oggenc.exe";
    if (GetFileAttributesA(oggencPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return std::string();
    return oggencPath;
}

static int USF_RunOggEncoderPass(float quality,
                                  const UAS_PkgFilter& filter,
                                  std::vector<UAS_SoundReport>* outReport = nullptr)
{
    void** gobjData  = *reinterpret_cast<void***>(GOBJECTS_DATA);
    int    gobjCount = *reinterpret_cast<int*>   (GOBJECTS_NUM);
    if (!gobjData || gobjCount <= 0) return 0;

    const uintptr_t kUSoundClass = USOUND_CLASS_PTR;

    // Engine's EncodeSound expects pre-built OGGs at "..\\packages\\sounds\\temp\\".
    // The literal Ghidra-decoded format string is "%stemp\\" with %s = the parent
    // "..\\packages\\sounds\\", which resolves to "..\\packages\\sounds\\temp\\"
    // - NOT "stemp" (I misread the leading %s as a literal "s" for far too long).
    // EncodeSound creates this dir itself but doing it up front is harmless.
    CreateDirectoryA("..\\packages\\sounds\\temp", nullptr);

    // Resolve oggenc.exe once. If it can't be found we'll report every sound as
    // "OGGENC MISSING" so the user gets a clear, single-cause diagnostic.
    const std::string oggencAbs = UAS_LocateOggenc();

    // Local helper for file size, reused across the function.
    auto sizeOf = [](const char* p) -> long long {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a))
            return -1;
        return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    };

    int encoded = 0;

    for (int i = 0; i < gobjCount; ++i)
    {
        uintptr_t obj = reinterpret_cast<uintptr_t>(gobjData[i]);
        if (!obj) continue;

        uintptr_t cls = *reinterpret_cast<uintptr_t*>(obj + 0x24);
        bool isSound = false;
        for (int d = 0; cls && d < 16; ++d)
        {
            if (cls == kUSoundClass) { isSound = true; break; }
            cls = *reinterpret_cast<uintptr_t*>(cls + 0x28);
        }
        if (!isSound) continue;

        uint32_t flags = *reinterpret_cast<uint32_t*>(obj + USOUND_FLAGS_OFFSET);

        // Stream is the qualifier for UAS inclusion. Sounds without 0x4 are
        // not UAS candidates regardless of the Xbox HD flag - they belong in
        // the .uax (PC) or .hds (Xbox) instead.
        if (!(flags & SF_UAS_STREAM)) continue;

        char pkgName[256] = {};
        UAS_GetPackageName(obj, pkgName, sizeof(pkgName));
        if (!filter.empty() && !filter.contains(pkgName)) continue;

        char sndName[256] = {};
        if (!UAS_GetObjectFName(obj, sndName, sizeof(sndName))) continue;
        if (sndName[0] == '\0') continue;

        // Engine reads pre-built OGGs from "..\\packages\\sounds\\temp\\<name>.ogg"
        // (the engine's literal format string is "%stemp\\%s.ogg" with %s =
        // the parent dir - see the CreateDirectoryA call above for context).
        char oggPath[MAX_PATH];
        snprintf(oggPath, sizeof(oggPath),
                 "..\\packages\\sounds\\temp\\%s.ogg", sndName);

        // NTFS file-tunneling workaround: if we overwrite an existing OGG, NTFS
        // preserves the creation time of the OLD file. The engine's staleness
        // check (IsSourceFileOlderThanOutput @ 0x10F74D40) uses CreationTime -
        // NOT LastWriteTime - so a "fresh" OGG written via overwrite still looks
        // older than the WAV, which causes UpdateStreamFile to invoke its own
        // EncodeSound > CreateProcess("oggenc") which fails (no full path) and
        // takes the whole call down with it.
        //
        // Deleting the OGG first forces a brand-new file with current-timestamp
        // creation time, so the staleness check then properly sees the source
        // as older than the output and short-circuits the re-encode.
        DeleteFileA(oggPath);

        // STEP A: prefer a user-supplied <name>.ogg sitting alongside the WAVs.
        //
        //   ..\\packages\\sounds\\<name>.ogg              (flat)
        //   ..\\packages\\sounds\\<pkg>\\<name>.ogg       (by-package)
        //
        // If found, we copy it as-is into stemp\<name>.ogg and SKIP oggenc.
        // OGG is a lossy format - running an already-encoded .ogg back through
        // oggenc would stack two lossy passes and degrade the audio. So the
        // user can drop their original .ogg next to the WAV (or replace the
        // WAV with the .ogg entirely if they only have the encoded source)
        // and we'll honour it verbatim.
        char userOggPath[MAX_PATH];
        bool userOggFound = false;
        snprintf(userOggPath, sizeof(userOggPath),
                 "..\\packages\\sounds\\%s.ogg", sndName);
        if (GetFileAttributesA(userOggPath) != INVALID_FILE_ATTRIBUTES) {
            userOggFound = true;
        } else {
            snprintf(userOggPath, sizeof(userOggPath),
                     "..\\packages\\sounds\\%s\\%s.ogg", pkgName, sndName);
            if (GetFileAttributesA(userOggPath) != INVALID_FILE_ATTRIBUTES)
                userOggFound = true;
        }

        if (userOggFound)
        {
            // CopyFileA with bFailIfExists=FALSE - destination was just
            // DeleteFileA'd above so this is effectively a fresh write.
            // The copy gets a new NTFS creation timestamp on its own, so
            // the engine's staleness check will skip re-encoding even if a
            // matching WAV happens to also exist (we never run oggenc on
            // the WAV when we already have the user's OGG).
            BOOL copied = CopyFileA(userOggPath, oggPath, FALSE);
            if (outReport)
            {
                UAS_SoundReport r;
                r.package  = pkgName;
                r.name     = sndName;
                r.flags    = flags;
                r.status   = copied ? UAS_SoundStatus::UserOggCopied
                                    : UAS_SoundStatus::OggencFailed;
                r.wavPath  = userOggPath;
                r.exitCode = copied ? 0 : GetLastError();
                r.wavSize  = sizeOf(userOggPath);
                r.oggSize  = sizeOf(oggPath);
                outReport->push_back(std::move(r));
            }
            if (copied) ++encoded;
            continue;
        }

        // STEP B: find WAV source: prefer flat, fall back to package subfolder.
        char wavPath[MAX_PATH];
        bool wavFound = false;
        snprintf(wavPath, sizeof(wavPath),
                 "..\\packages\\sounds\\%s.wav", sndName);
        if (GetFileAttributesA(wavPath) != INVALID_FILE_ATTRIBUTES) {
            wavFound = true;
        } else {
            snprintf(wavPath, sizeof(wavPath),
                     "..\\packages\\sounds\\%s\\%s.wav", pkgName, sndName);
            if (GetFileAttributesA(wavPath) != INVALID_FILE_ATTRIBUTES)
                wavFound = true;
        }

        if (!wavFound)
        {
            // No source WAV. If a pre-built .ogg already lives in stemp\, the
            // engine will reuse it (the staleness check trivially passes when
            // the WAV doesn't exist). Otherwise this sound will appear in the
            // .uas header but with no audio payload (or be silently dropped
            // by UpdateStreamFile's "CreateFileA stemp\.ogg failed" check).
            bool haveOgg = (GetFileAttributesA(oggPath) != INVALID_FILE_ATTRIBUTES);
            if (outReport)
            {
                UAS_SoundReport r;
                r.package  = pkgName;
                r.name     = sndName;
                r.flags    = flags;
                r.status   = haveOgg ? UAS_SoundStatus::StempOggReused
                                     : UAS_SoundStatus::WavMissing;
                r.wavPath  = "";
                r.exitCode = 0;
                r.wavSize  = -1;
                r.oggSize  = haveOgg ? sizeOf(oggPath) : -1;
                outReport->push_back(std::move(r));
            }
            if (haveOgg) ++encoded;
            continue;
        }

        // Build command line. CreateProcessA with lpApplicationName non-NULL
        // bypasses path-resolution entirely and uses the absolute path we
        // computed. lpCommandLine still needs argv[0] (oggenc.exe) as its first
        // token - most CLI tools (including vorbis-tools oggenc) parse it.
        UAS_SoundStatus status = UAS_SoundStatus::OggencNotRun;
        DWORD exitCode = (DWORD)-1;

        if (oggencAbs.empty())
        {
            // oggenc.exe not found next to ChaosTheory_Editor.exe - no point
            // attempting CreateProcess, just record the diagnostic.
            if (outReport)
            {
                UAS_SoundReport r;
                r.package  = pkgName;
                r.name     = sndName;
                r.flags    = flags;
                r.status   = UAS_SoundStatus::OggencNotRun;
                r.wavPath  = wavPath;
                r.exitCode = 0;
                outReport->push_back(std::move(r));
            }
            continue;
        }

        char cmdLine[MAX_PATH * 3];
        snprintf(cmdLine, sizeof(cmdLine),
                 "\"%s\" \"%s\" -q %.1f -o \"%s\"",
                 oggencAbs.c_str(), wavPath, quality, oggPath);

        // Working dir = editor exe's directory. That makes the ..\packages\sounds\
        // relative paths in wavPath/oggPath resolve the same way the engine
        // resolves them in its own CreateProcessA call to oggenc.
        std::string editorDir = oggencAbs.substr(0, oggencAbs.find_last_of('\\') + 1);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(oggencAbs.c_str(), cmdLine, nullptr, nullptr,
                           FALSE, 0, nullptr, editorDir.c_str(), &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 30000);
            exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            status = (exitCode == 0) ? UAS_SoundStatus::EncodedOk
                                     : UAS_SoundStatus::OggencFailed;
            if (exitCode == 0) ++encoded;
        }
        if (outReport)
        {
            UAS_SoundReport r;
            r.package  = pkgName;
            r.name     = sndName;
            r.flags    = flags;
            r.status   = status;
            r.wavPath  = wavPath;
            r.exitCode = exitCode;
            r.wavSize  = sizeOf(wavPath);
            r.oggSize  = sizeOf(oggPath);
            outReport->push_back(std::move(r));
        }
    }

    return encoded;
}

static const char* UAS_StatusToShortString(UAS_SoundStatus s)
{
    switch (s) {
    case UAS_SoundStatus::EncodedOk:      return "OK";
    case UAS_SoundStatus::UserOggCopied:  return "OGG COPIED";
    case UAS_SoundStatus::StempOggReused: return "REUSED";
    case UAS_SoundStatus::WavMissing:     return "NO WAV";
    case UAS_SoundStatus::OggencFailed:   return "OGGENC FAIL";
    case UAS_SoundStatus::OggencNotRun:   return "OGGENC MISSING";
    }
    return "?";
}

// Write a per-sound report so the user can diff/inspect outside the modal dialog.
// Lines look like:  Amb_Aqua  aqua_musicA       flags=0x004  OK
static void USF_WriteReport(const std::vector<UAS_SoundReport>& report,
                            const char* mapName,
                            int totalEncoded,
                            int totalSounds)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path),
             "..\\Packages\\Sounds\\%s.uas.buildlog.txt", mapName);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;

    fprintf(f, "Build UAS log for %s.uas\r\n", mapName);
    fprintf(f, "  Total Stream-flagged sounds in selected packages: %d\r\n", totalSounds);
    fprintf(f, "  Encoded successfully:                              %d\r\n", totalEncoded);
    fprintf(f, "  Missing / failed:                                  %d\r\n\r\n",
            totalSounds - totalEncoded);
    fprintf(f, "%-24s  %-40s  %-12s  %-13s  %-13s  %s\r\n",
            "Package", "Sound", "Flags", "WAV bytes", "OGG bytes", "Status / WAV path");
    fprintf(f, "%-24s  %-40s  %-12s  %-13s  %-13s  %s\r\n",
            "-------", "-----", "-----", "---------", "---------", "-----------------");

    long long totalOgg = 0;
    int       oggCount = 0;
    for (const auto& r : report)
    {
        char wavSz[24], oggSz[24];
        if (r.wavSize >= 0) snprintf(wavSz, sizeof(wavSz), "%lld", r.wavSize);
        else                snprintf(wavSz, sizeof(wavSz), "-");
        if (r.oggSize >= 0) snprintf(oggSz, sizeof(oggSz), "%lld", r.oggSize);
        else                snprintf(oggSz, sizeof(oggSz), "-");

        if (r.oggSize > 0) { totalOgg += r.oggSize; ++oggCount; }

        fprintf(f, "%-24s  %-40s  0x%08X  %-13s  %-13s  %s",
                r.package.c_str(), r.name.c_str(), r.flags,
                wavSz, oggSz,
                UAS_StatusToShortString(r.status));
        if (r.status == UAS_SoundStatus::OggencFailed)
            fprintf(f, " (exit=%lu, wav=%s)", r.exitCode, r.wavPath.c_str());
        else if (r.status == UAS_SoundStatus::EncodedOk ||
                 r.status == UAS_SoundStatus::OggencNotRun)
            fprintf(f, " (%s)", r.wavPath.c_str());
        fprintf(f, "\r\n");
    }
    fprintf(f, "\r\nTotal OGG payload bytes (sum across %d files): %lld\r\n",
            oggCount, totalOgg);
    fprintf(f, "Expected .uas size order of magnitude: total OGG + per-sound header (~76 bytes)\r\n");
    fclose(f);
}

// Dump the first N bytes of a file as a hex+ascii ribbon, appended to a log.
// Useful for inspecting on-disk layout of the produced .uas vs. a reference file.
static void UAS_AppendHeadDump(const char* logPath,
                                const char* label,
                                const char* filePath,
                                size_t bytes)
{
    FILE* f = nullptr;
    if (fopen_s(&f, logPath, "a") != 0 || !f) return;
    fprintf(f, "\r\n=== Head dump: %s (%s) ===\r\n", label, filePath);

    HANDLE h = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(f, "  (could not open: GetLastError=%lu)\r\n", GetLastError());
        fclose(f);
        return;
    }
    std::vector<unsigned char> buf(bytes, 0);
    DWORD got = 0;
    ReadFile(h, buf.data(), (DWORD)bytes, &got, nullptr);
    CloseHandle(h);
    buf.resize(got);

    for (size_t off = 0; off < buf.size(); off += 16)
    {
        fprintf(f, "  %08zX  ", off);
        for (size_t j = 0; j < 16; ++j) {
            if (off + j < buf.size()) fprintf(f, "%02X ", buf[off + j]);
            else                       fprintf(f, "   ");
        }
        fprintf(f, " ");
        for (size_t j = 0; j < 16 && off + j < buf.size(); ++j) {
            unsigned char c = buf[off + j];
            fputc((c >= 0x20 && c < 0x7F) ? c : '.', f);
        }
        fprintf(f, "\r\n");
    }
    fclose(f);
}

// Package-list payload passed to UpdateStreamFile as its arg1 (packageList).
//
// HEAP-CORRUPTION HISTORY (left in this comment as a tombstone so we don't
// repeat the mistake):
//
//   Earlier this code swapped an std::vector<char> buffer into the audio
//   subsystem's [this+0x2c] / [this+0x30] fields, expecting that field to
//   be "the audio subsystem's package list". It is - but it's also engine-
//   owned memory: UpdateStreamFile's cleanup helper FUN_10F779C0 calls
//   FMalloc::Free on whatever pointer is sitting at [+0x2c]. Passing it a
//   pointer into our std::vector storage corrupted the FMallocWindows heap,
//   which only manifested several ticks later as a GPF:
//       History: FMallocWindows::Free <- UEditorEngine::Tick <- UpdateWorld
//   That's why menus opened (USER32 doesn't allocate from FMalloc) but
//   viewport ticks froze (the editor's tick allocates and re-enters the
//   broken heap).
//
// CORRECT APPROACH (what this struct does):
//
//   UpdateStreamFile's body has two branches for arg1:
//     - arg1 == 0:        scan internal [this+0x2c] (the engine-owned list)
//     - arg1 == non-null: treat as TArray<FString>*, copy names out of it
//                         (engine never frees anything we own)
//
//   So we build a UE2-shaped TArray<FString> in our memory and pass its
//   address as arg1. The engine just READS our list - copies the names into
//   its own FMalloc-allocated working buffer (local_ae4) and proceeds. Our
//   memory is never touched by FMalloc::Free.
//
//   The flag filter (Stream vs Xbox HD vs both) is keyed off arg3 and is
//   independent of arg1, so we still get the SF_UAS_STREAM-only filter.
//
//   The final-extension selector also depends on arg1 only when arg3 == 1
//   (the Xbox HD path). With arg3 == 0 (our path), both temp and final
//   extensions are .uas regardless of arg1 - so we can pass a non-NULL
//   pointer here without ending up with a .hds file.
#pragma pack(push, 4)
struct UE2_FString {
    char* Data;   // pointer to null-terminated ASCII bytes
    int   Num;    // length including null terminator
    int   Max;    // capacity (treated identically to Num here)
};
struct UE2_TArray_FString {
    void* Data;   // pointer to UE2_FString[Num]
    int   Num;    // element count
    int   Max;    // capacity
};
#pragma pack(pop)

struct UASPkgListBuf {
    std::vector<char>        nameStorage;
    std::vector<UE2_FString> fstrings;
    UE2_TArray_FString       arr;
};

static void UAS_BuildPackageListBuf(const std::vector<std::string>& pkgs,
                                     UASPkgListBuf& out)
{
    if (pkgs.empty())
    {
        out.arr.Data = nullptr;
        out.arr.Num  = 0;
        out.arr.Max  = 0;
        return;
    }

    // Pre-size both backing buffers up front so .data() pointers are stable.
    size_t totalBytes = 0;
    for (const auto& p : pkgs) totalBytes += p.size() + 1;
    out.nameStorage.resize(totalBytes);
    out.fstrings.resize(pkgs.size());

    size_t offset = 0;
    for (size_t i = 0; i < pkgs.size(); ++i)
    {
        const auto& p = pkgs[i];
        memcpy(out.nameStorage.data() + offset, p.c_str(), p.size() + 1);
        out.fstrings[i].Data = out.nameStorage.data() + offset;
        out.fstrings[i].Num  = static_cast<int>(p.size() + 1);
        out.fstrings[i].Max  = static_cast<int>(p.size() + 1);
        offset += p.size() + 1;
    }

    out.arr.Data = out.fstrings.data();
    out.arr.Num  = static_cast<int>(pkgs.size());
    out.arr.Max  = static_cast<int>(pkgs.size());
}

// SEH wrapper: must have no C++ destructors in the __try function. Caller wraps the
// AudioPkgListScope and any std::vector usage; we just push the 4 stack args plus ECX
// and call UpdateStreamFile. Returns the AL return of UpdateStreamFile (non-zero = OK).
//
// REAL parameter mapping (re-verified via assembly @ 0x10f7838c, 0x10f7842f, 0x10f77c7a):
//
//   arg1 ([ESP+0xb04])  - final file extension selector:
//                         0 > .uas    (engine writes <map>.uas)
//                         else > .hds (engine writes <map>.hds)
//
//   arg2 ([ESP+0xb08])  - ENCODER passed to EncodeSound as its param_4:
//                         1 > oggenc.exe        > .ogg in stemp\
//                         2 > xbadpcmencode.exe > .xb  in stemp\
//                         (the engine stores this verbatim in packing_entry+0x10
//                          and later uses it to pick the stemp file extension)
//
//   arg3 ([ESP+0xb0c])  - FILTER + temp file extension:
//                         0 > require SF_UAS_STREAM      | temp .uas
//                         1 > require SF_XBOXHD_STREAM   | temp .hds (Xbox HD Sources!)
//                         2 > require SF_UAS_STREAM AND NOT SF_XBOXHD_STREAM | temp .uas
//
//   arg4 ([ESP+0xb10])  - quality float bits passed to oggenc as -q value
//
// For PC SCCT Versus we want:
//   - Stream-flagged sounds (SF_UAS_STREAM)        > arg3 = 0
//   - .uas output                                  > arg1 = 0, arg3 != 1
//   - OGG encoded payload                          > arg2 = 1
//   - quality 5.0                                  > arg4 = 0x40A00000
//
// Engine's own calls in ExportToXBox always pass arg2=2 (XB ADPCM encoder), which is
// the Xbox-side encoding. There's NO shipped editor call that combines Stream filter
// + OGG encoder, which is exactly why nobody could rebuild PC UAS files cleanly -
// you had to know the parameters didn't actually mean what they look like.
static bool USF_InvokeOggBuild(DWORD qualityBits, void* packageListPtr)
{
    const DWORD pAudio   = AUDIO_SUBSYSTEM_PTR;
    const DWORD pfnUSF   = UPDATE_STREAM_FILE;
    const DWORD qualBits = qualityBits;
    const DWORD pkgList  = reinterpret_cast<DWORD>(packageListPtr);
    char        ok       = 0;

    __try
    {
        __asm {
            push qualBits   // arg4: quality (-q 5.0 to oggenc)
            push 0          // arg3: filter = SF_UAS_STREAM (temp file ext = .uas)
            push 1          // arg2: encoder = oggenc.exe (> stemp\<name>.ogg)
            push pkgList    // arg1: TArray<FString>* of package names
            mov  ecx, pAudio
            mov  eax, pfnUSF
            call eax
            mov  ok, al
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return ok != 0;
}

// Focus-restoration helper. The UnrealEd UI is a constellation of top-level
// WWindow subclasses (WUnrealEdFrame, WBrowserSound, WLog, WObjectProperties,
// ...) - they are SIBLINGS at the top level, NOT ancestors of each other. So
// walking up the ownership chain from the Sound Browser only finds the
// Sound Browser; it never reaches WUnrealEdFrame which is what owns the
// viewports the user actually wants to interact with.
//
// What disables WUnrealEdFrame in our flow: the engine's "Updating bigfile"
// progress dialog (FUN_10F779C0, invoked from inside UpdateStreamFile) shows
// itself owned by the main editor frame, which implicitly EnableWindow(FALSE)s
// the owner for the duration. On some dismissal paths it fails to re-enable.
// DialogFix.cpp already patches the WObjectProperties variant of this same
// bug; here we do the equivalent for our Build-UAS flow without needing a
// targeted hook.
//
// Fix: enumerate every top-level window owned by our UI thread and force
// EnableWindow(TRUE) on any that's currently disabled, then activate the
// window that looks most like the main editor frame.
struct UAS_FocusEnumCtx
{
    HWND mainFrame;   // best-guess top-level editor frame
    HWND fallback;    // first visible top-level we saw (used if no frame found)
    int  reEnabledCount;
};

static BOOL CALLBACK UAS_FocusEnumProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindow(hwnd)) return TRUE;
    UAS_FocusEnumCtx* ctx = reinterpret_cast<UAS_FocusEnumCtx*>(lParam);

    if (!IsWindowEnabled(hwnd))
    {
        EnableWindow(hwnd, TRUE);
        ++ctx->reEnabledCount;
    }

    char className[128] = {};
    char titleText[256] = {};
    GetClassNameA(hwnd, className, sizeof(className));
    GetWindowTextA(hwnd, titleText, sizeof(titleText));

    if (!ctx->mainFrame && IsWindowVisible(hwnd))
    {
        // UnrealEd window classes are WWindow subclasses: "WUnrealEdFrame",
        // "WLog", "WBrowserSound", "WObjectProperties", etc. The main viewport
        // frame is "WUnrealEdFrame"; its title also starts with "UnrealEd".
        if (strstr(className, "UnrealEdFrame") ||
            strncmp(titleText, "UnrealEd", 8) == 0)
        {
            ctx->mainFrame = hwnd;
        }
    }
    if (!ctx->fallback && IsWindowVisible(hwnd))
        ctx->fallback = hwnd;

    return TRUE;
}

// Deferred-message-box mechanism. Showing the result MessageBoxA directly
// from inside SB_HandleMakeUAS - which itself runs from inside the editor's
// menu-command handler - blocks the editor's main message loop on its own
// modal pump. After MessageBoxA returns, control unwinds back through the
// command handler, but by then WUnrealEdFrame's idle tick (which is what
// drives viewport rendering, the in-editor real-time audio mixer, etc.) has
// been off for the entire duration of the build + dialog. Several UE2
// editors (including UT2004's) have observable bugs where the tick doesn't
// fully resume after a long modal pause and the viewports stay frozen even
// though menus respond.
//
// Workaround: SetTimer with a TimerProc. The timer fires AFTER our handler
// returns to the editor's main loop, so by the time we MessageBoxA the
// editor has already run at least one full Tick() and the post-dialog
// re-activation lands cleanly.
struct UAS_DeferredDialog {
    std::string body;
    std::string title;
};
static UAS_DeferredDialog g_pendingDialog;
static HWND               g_pendingDialogStartHwnd = NULL;

// Forward decl needed because the timer proc activates the editor before
// we hit the helper's definition lower in the file.
static void UAS_RestoreEditorFocus(HWND startHwnd);

static VOID CALLBACK UAS_DeferredDialogTimerProc(HWND hwnd, UINT /*uMsg*/,
                                                  UINT_PTR idEvent, DWORD /*dwTime*/)
{
    KillTimer(hwnd, idEvent);

    // Owner = NULL > MessageBox is task-modal but doesn't disable any
    // specific window. That side-steps the EnableWindow(FALSE) state that
    // both DialogFix.cpp's WObjectProperties path and our earlier owned
    // MessageBoxes were leaving behind.
    MessageBoxA(NULL, g_pendingDialog.body.c_str(),
                g_pendingDialog.title.c_str(),
                MB_OK | MB_SETFOREGROUND | MB_TOPMOST);

    UAS_RestoreEditorFocus(g_pendingDialogStartHwnd);

    g_pendingDialog.body.clear();
    g_pendingDialog.title.clear();
    g_pendingDialogStartHwnd = NULL;
}

static void UAS_ShowResultDialogDeferred(HWND startHwnd,
                                          const char* title,
                                          const std::string& body)
{
    g_pendingDialog.body  = body;
    g_pendingDialog.title = title;
    g_pendingDialogStartHwnd = startHwnd;

    // 50ms is enough for the editor's main loop to run a tick or two before
    // we present the dialog. Owner HWND for the timer is the Sound Browser
    // so the timer dies if the browser does.
    SetTimer(startHwnd, 0x7757u /* "USND" */, 50,
             UAS_DeferredDialogTimerProc);
}

static void UAS_RestoreEditorFocus(HWND startHwnd)
{
    UAS_FocusEnumCtx ctx = {};

    // EnumThreadWindows is scoped to our UI thread, so we won't touch other
    // applications' disabled windows. Every WWindow in the editor lives on
    // this same thread.
    EnumThreadWindows(GetCurrentThreadId(),
                      UAS_FocusEnumProc,
                      reinterpret_cast<LPARAM>(&ctx));

    // Also fix the owner chain from startHwnd up - covers any nested cases
    // the top-level sweep wouldn't visit (e.g. a re-parented child window).
    HWND h = startHwnd;
    for (int guard = 0; guard < 32 && h && IsWindow(h); ++guard)
    {
        if (!IsWindowEnabled(h)) EnableWindow(h, TRUE);
        HWND next = GetWindow(h, GW_OWNER);
        if (!next) next = GetParent(h);
        if (!next || next == h) break;
        h = next;
    }

    // Activate the main editor frame so viewports regain input. Prefer the
    // identified WUnrealEdFrame; fall back to any visible top-level; final
    // fallback is whatever the caller passed in (the Sound Browser, usually).
    HWND target = ctx.mainFrame;
    if (!target) target = ctx.fallback;
    if (!target) target = startHwnd;

    if (target && IsWindow(target))
    {
        // SW_SHOWNA: no-op if already visible & not minimised; restores if
        // minimised, without stealing activation from the wrong child.
        ShowWindow(target, SW_SHOWNA);
        BringWindowToTop(target);
        SetActiveWindow(target);
        SetForegroundWindow(target);

        // Force a full repaint of the editor frame and every child viewport.
        // RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW makes
        // Windows synchronously dispatch WM_PAINT to the frame and all
        // descendants - that wakes WUnrealEdFrame's render path back up
        // even if its tick had stalled. Without this, viewports often stay
        // showing whatever was visible right before the modal pause.
        RedrawWindow(target, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);

        // Pump a few messages on the way out so any queued WM_PAINT /
        // WM_TIMER / engine-tick triggers actually run before we return
        // control to the caller. Bounded loop so we can't get stuck.
        MSG msg;
        for (int i = 0; i < 16; ++i)
        {
            if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) break;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

static void __cdecl SB_HandleMakeUAS(void* this_ptr)
{
    HWND hBrowser = GetParentHWND(this_ptr);

    // 1. Resolve the current map name from the browser's status bar.
    //    Used both to default the package picker and to verify that some
    //    audio context exists (UpdateStreamFile derives the output filename
    //    from the audio subsystem's current map at [this+0x14]ish via
    //    AUDIO SETMAP - we re-issue that below to make sure it's current).
    char mapText[256] = {};
    EnumChildWindows(hBrowser, FindMapLabelProc,
                     reinterpret_cast<LPARAM>(mapText));
    const char* mapName = mapText + 5;  // skip "Map: " prefix
    if (mapText[0] == '\0' ||
        mapName[0]  == '\0' ||
        strcmp(mapName, "None") == 0)
    {
        MessageBoxA(hBrowser,
                    "Load a map before building a UAS package.",
                    "Message", MB_OK);
        return;
    }

    // 2. Enumerate every loaded UAX package that owns at least one SF_UAS_STREAM
    //    sound. These are the only packages that contribute to a .uas - non-stream
    //    sounds stay inline in the .uax. Sorted alphabetically.
    std::vector<std::string> allPkgs = UAS_EnumLoadedSoundPackages();
    if (allPkgs.empty())
    {
        MessageBoxA(hBrowser,
                    "No Stream-flagged sounds found in any loaded package.\r\n"
                    "Open the relevant Amb_*.uax / Interface.uax packages first,\r\n"
                    "then click Build UAS again.",
                    "Build UAS", MB_OK);
        return;
    }

    // 3. Package picker. Default selection:
    //    - "Interface"             - present in every map's UAS per Ubisoft convention.
    //    - "Amb_<mapName>"         - the map-specific ambient package, if it exists.
    UASPkgPickerCtx pkgCtx = {};
    pkgCtx.packages  = &allPkgs;
    pkgCtx.cancelled = true;
    pkgCtx.selected.assign(allPkgs.size(), false);

    char ambGuess[64];
    snprintf(ambGuess, sizeof(ambGuess), "Amb_%s", mapName);
    for (size_t i = 0; i < allPkgs.size(); ++i)
    {
        if (_stricmp(allPkgs[i].c_str(), "Interface") == 0 ||
            _stricmp(allPkgs[i].c_str(), ambGuess)    == 0)
        {
            pkgCtx.selected[i] = true;
        }
    }

    {
        std::vector<uint8_t> dlgTpl = BuildUASPkgDlgTemplate();
        DialogBoxIndirectParamA(
            GetModuleHandleA(nullptr),
            reinterpret_cast<LPCDLGTEMPLATE>(dlgTpl.data()),
            hBrowser,
            UASPkgPickerDlgProc,
            reinterpret_cast<LPARAM>(&pkgCtx));
    }
    if (pkgCtx.cancelled) return;

    std::vector<std::string> chosenPkgs;
    UAS_PkgFilter pkgFilter;
    for (size_t i = 0; i < allPkgs.size(); ++i)
    {
        if (pkgCtx.selected[i])
        {
            chosenPkgs.push_back(allPkgs[i]);
            pkgFilter.add(allPkgs[i]);
        }
    }
    if (chosenPkgs.empty())
    {
        MessageBoxA(hBrowser, "Select at least one package.", "Build UAS", MB_OK);
        return;
    }

    // 4. OGG quality slider (no WAV/uncompressed option - PC SCCT Versus only
    //    plays back OGG-encoded UAS streams).
    UASCompCtx compCtx = {};
    compCtx.quality   = 50;   // 5.0 = Ubisoft-shipped default
    compCtx.cancelled = true;
    {
        std::vector<uint8_t> dlgTpl = BuildUASCompDlgTemplate();
        DialogBoxIndirectParamA(
            GetModuleHandleA(nullptr),
            reinterpret_cast<LPCDLGTEMPLATE>(dlgTpl.data()),
            hBrowser,
            UASCompDlgProc,
            reinterpret_cast<LPARAM>(&compCtx));
    }
    if (compCtx.cancelled) return;
    const float quality = compCtx.quality / 10.0f;

    // 5. Detach the audio subsystem from <map>.uas BEFORE the rebuild.
    //
    //    The audio subsystem holds a streaming read handle on <map>.uas while
    //    a map is active. UpdateStreamFile closes its own [this+0x10] /
    //    [this+0xc] handle slots on entry, but anything else that opened a
    //    second handle (cached stream, an actively-playing sound, the engine's
    //    preview/scrub paths, Windows Defender's live scan, etc.) keeps the
    //    file locked. That's why the engine's MoveFileA fails with Access
    //    Denied (5) and our MoveFileExA fallback can also lose the race.
    //
    //    "AUDIO SETMAP NAME=\"\"" tells the audio subsystem to drop its
    //    current map > it releases all of its <map>.uas references. The
    //    rebuild then has unrestricted write/rename access on the file.
    //    UpdateStreamFile derives its target filename from the audio
    //    subsystem's still-set internal map string (cached at ~[this+0x14])
    //    plus the AudioPkgListScope we install below, so output still lands
    //    at the right path even with the "current" map cleared.
    ExecEditorCommand("AUDIO SETMAP NAME=\"\"");

    // 6. Pre-encode source WAVs > OGGs in stemp\, while recording a per-sound
    //    outcome for the post-build report.
    //
    //    Without this pre-pass, EncodeSound's CreateProcessA("oggenc <args>") fails
    //    (oggenc isn't on PATH from the editor's working dir) and we'd end up with
    //    a stub UAS - which was the original symptom that brought us here.
    std::vector<UAS_SoundReport> report;
    int encoded = USF_RunOggEncoderPass(quality, pkgFilter, &report);

    // 7. Force a clean rebuild before invoking UpdateStreamFile.
    //    The engine reads the EXISTING <map>.uas (if present) and merges its
    //    entries into the new file verbatim - without re-applying the
    //    SF_UAS_STREAM filter. So Xbox HD-only sounds that the user dropped in
    //    place from an Ubisoft Xbox build (or a previous dirty rebuild) slip
    //    through and end up in the PC .uas where they don't belong.
    //
    //    Deleting the existing .uas forces UpdateStreamFile to build solely
    //    from the GObjects scan, which DOES apply the flag filter. The engine
    //    handles the "old file missing" case (CreateFileA returns
    //    INVALID_HANDLE_VALUE > it just skips the merge block).
    {
        char uasPath[MAX_PATH];
        snprintf(uasPath, sizeof(uasPath),
                 "..\\Packages\\Sounds\\%s.uas", mapName);
        DeleteFileA(uasPath);
    }
    USF_ClearTempFiles();  // remove any stale temp_*.uas from a previous failed run

    DWORD qualBits;
    memcpy(&qualBits, &quality, sizeof(qualBits));

    // Helper: file size in bytes, or -1 if file doesn't exist.
    auto fileSize = [](const char* p) -> long long {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a))
            return -1;
        return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    };

    char uasPathDiag[MAX_PATH], tempUasDiag[MAX_PATH], tempHdsDiag[MAX_PATH];
    snprintf(uasPathDiag,  sizeof(uasPathDiag),
             "..\\Packages\\Sounds\\%s.uas", mapName);
    snprintf(tempUasDiag, sizeof(tempUasDiag),
             "..\\Packages\\Sounds\\temp_%s.uas", mapName);
    snprintf(tempHdsDiag, sizeof(tempHdsDiag),
             "..\\Packages\\Sounds\\temp_%s.hds", mapName);

    // Pre-call snapshot so we can detect whether the engine actually touched
    // <map>.uas (vs. leaving the prior content in place).
    const long long preFinalSize = fileSize(uasPathDiag);

    bool  ok           = false;
    DWORD lastErr      = 0;
    {
        // Build the TArray<FString>* payload from the selected packages and
        // hand it to UpdateStreamFile as arg1. Engine reads our list, copies
        // names into its own buffer, never frees anything we own > no heap
        // corruption, no editor freeze, no GPF on close.
        UASPkgListBuf pkgListBuf;
        UAS_BuildPackageListBuf(chosenPkgs, pkgListBuf);
        void* arg1 = (pkgListBuf.arr.Num > 0) ? &pkgListBuf.arr : nullptr;

        SetLastError(0);
        ok = USF_InvokeOggBuild(qualBits, arg1);
        // GetLastError reflects the last failing Win32 call inside UpdateStreamFile
        // (e.g. CreateFileA on temp_<map>, ReadFile on stemp\<name>.ogg, the
        // MoveFileA from temp_<map>.hds > <map>.uas). Capture immediately
        // before any other API call can clobber it.
        lastErr = GetLastError();
    }

    // NOTE: previously we tried sending "AUDIO SETMAP NAME=<map>" here and
    // calling GWarn->EndSlowTask() to recover from the post-build editor
    // freeze. Both made things strictly worse:
    //   - AUDIO SETMAP tears down audio resources the Sound Browser is
    //     showing, so its package list and tabs blank out on every build.
    //   - The GWarn vtable[0x10] call at this point destabilises the editor
    //     to the point of immediate process exit.
    //
    // The viewport freeze is its own engine-side bug we still need to find a
    // different angle for. Until then, leave the audio subsystem untouched
    // here - the build product on disk is correct regardless.
    AllowSetForegroundWindow(ASFW_ANY);

    // 8a. Workaround: the engine builds the temp output as "temp_<map>.hds"
    //     (when format=1, which is the only way to get OGG encoding), then
    //     internally MoveFileA's it to "<map>.uas". That rename has been
    //     observed to fail on some setups - leaving the .hds in place and
    //     <map>.uas either unchanged or absent. If we still see the .hds
    //     temp lying around, finish the rename ourselves with MoveFileExA +
    //     MOVEFILE_REPLACE_EXISTING (which the engine doesn't use).
    bool weMovedHds = false;
    bool weMovedUas = false;
    if (GetFileAttributesA(tempHdsDiag) != INVALID_FILE_ATTRIBUTES)
    {
        if (MoveFileExA(tempHdsDiag, uasPathDiag,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            weMovedHds = true;
            ok = true;
        }
        else
        {
            lastErr = GetLastError();   // overwrite with our rename's failure code
        }
    }
    else if (GetFileAttributesA(tempUasDiag) != INVALID_FILE_ATTRIBUTES)
    {
        // format != 1 path produces temp_<map>.uas instead - same recovery.
        if (MoveFileExA(tempUasDiag, uasPathDiag,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            weMovedUas = true;
            ok = true;
        }
        else
        {
            lastErr = GetLastError();
        }
    }

    // 8b. Final filesystem probe for the dialog.
    const long long finalSize = fileSize(uasPathDiag);
    const long long tempUasSize = fileSize(tempUasDiag);
    const long long tempHdsSize = fileSize(tempHdsDiag);
    const bool      finalChanged = (finalSize >= 0) && (finalSize != preFinalSize);

    // The engine returns 0 (failure) even when the file ends up correctly written
    // - there's a non-fatal cleanup-path error inside UpdateStreamFile that flips
    // its return byte. We trust the filesystem instead: if <map>.uas changed size
    // from before the call, the build was effectively successful regardless of
    // what AL said. If our MoveFileExA fallback ran successfully it also lands
    // here. Only if neither the engine NOR our fallback produced a fresh file do
    // we consider it a real failure.
    if (finalChanged) ok = true;
    if (!finalChanged && !weMovedHds && !weMovedUas) ok = false;

    // But filesystem-changed is necessary, not sufficient: when every source
    // is missing the engine still writes a ~1 KB stub <map>.uas with headers
    // and zero audio payload. That trips finalChanged=true and would wrongly
    // report success. Require at least one sound to have produced usable
    // audio (encoded counts EncodedOk + UserOggCopied + StempOggReused).
    if (!report.empty() && encoded == 0) ok = false;

    // Close the engine's "Updating bigfile" progress window if it got left
    // on screen.
    //
    // What the engine actually creates (cross-checked against UT2004's
    // UnrealEd/Inc/DlgProgress.h and UnrealEd/Src/res/UnrealEd.rc):
    //
    //   IDDIALOG_PROGRESS DIALOG  0, 0, 237, 37
    //   STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION
    //   CAPTION "Progress"
    //   BEGIN
    //       CONTROL "",IDSC_MSG,"Static",...        // ID 1088 — the
    //                                               // "Updating bigfile :
    //                                               // adding <name>" line
    //       CONTROL "Progress1",IDPG_PROGRESS,      // ID 1087 — progress bar
    //               "msctls_progress32",...
    //   END
    //
    // It's spawned via CreateDialogParamA from this resource template, so
    // its window class is the standard dialog class "#32770" — NOT
    // "WDlgProgress" (that's only UE2's internal logical class name, never
    // visible to USER32). My previous class-name match couldn't find it.
    //
    // Reliable signature: GetWindowTextA == "Progress" AND it has a child
    // control with ID 1087 (msctls_progress32). That uniquely identifies
    // this dialog in the editor's UI thread without hitting any other
    // window that happens to be titled "Progress".
    struct Local {
        static BOOL CALLBACK Proc(HWND hwnd, LPARAM /*lParam*/) {
            if (!IsWindow(hwnd)) return TRUE;
            char title[64] = {};
            GetWindowTextA(hwnd, title, sizeof(title));
            if (_stricmp(title, "Progress") != 0) return TRUE;

            // Confirm IDPG_PROGRESS (1087) child exists, so we don't
            // accidentally close some other window the user opened.
            HWND hProgressBar = GetDlgItem(hwnd, 1087);
            if (!hProgressBar) return TRUE;

            // Hide first for immediate visual feedback, then destroy.
            // WM_CLOSE alone is unreliable: WDlgProgress::OnCancel() is
            // empty (see comment in UT2004's DlgProgress.h: "cancel
            // button needs to work"), so the dialog ignores it.
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
            return TRUE;
        }
    };
    EnumThreadWindows(GetCurrentThreadId(), Local::Proc, 0);

    // 9. Write the per-sound build log next to the .uas so the user can inspect
    //    it. Also surface the summary inline in the result dialog.
    USF_WriteReport(report, mapName, encoded, (int)report.size());

    // 9b. Append head-dumps of:
    //       - the produced output (.uas if rename worked, else the temp_*)
    //       - any original UAS we found alongside (for structural comparison)
    //     This makes it trivial to spot a wrong file layout (e.g. .hds-shaped
    //     header where .uas-shaped is expected, missing OggS magic, etc.)
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath),
             "..\\Packages\\Sounds\\%s.uas.buildlog.txt", mapName);

    const char* probePath = nullptr;
    const char* probeLabel = nullptr;
    if (fileSize(uasPathDiag) > 0)        { probePath = uasPathDiag;  probeLabel = "produced .uas"; }
    else if (fileSize(tempHdsDiag) > 0)   { probePath = tempHdsDiag;  probeLabel = "leftover temp_*.hds (rename failed)"; }
    else if (fileSize(tempUasDiag) > 0)   { probePath = tempUasDiag;  probeLabel = "leftover temp_*.uas (rename failed)"; }
    if (probePath) UAS_AppendHeadDump(logPath, probeLabel, probePath, 512);

    // Reference: if user has "<map>-Original.uas" sitting in Sounds\ we'll
    // dump its head for direct comparison. Common naming for these backups.
    char refPath[MAX_PATH];
    snprintf(refPath, sizeof(refPath),
             "..\\Packages\\Sounds\\%s-Original.uas", mapName);
    if (fileSize(refPath) > 0)
        UAS_AppendHeadDump(logPath, "reference (-Original.uas)", refPath, 512);

    // Collect names of sounds that didn't make it into the build. EncodedOk,
    // UserOggCopied, and StempOggReused are all success states (the engine
    // got valid audio); everything else means missing or unusable source.
    std::string missingList;
    int missingCount = 0;
    for (const auto& r : report)
    {
        if (r.status == UAS_SoundStatus::EncodedOk      ||
            r.status == UAS_SoundStatus::UserOggCopied  ||
            r.status == UAS_SoundStatus::StempOggReused)
            continue;
        if (missingCount < 30)
        {
            char line[300];
            snprintf(line, sizeof(line), "  %s.%s\r\n",
                     r.package.c_str(), r.name.c_str());
            missingList += line;
        }
        ++missingCount;
    }
    if (missingCount > 30)
    {
        char tail[64];
        snprintf(tail, sizeof(tail), "  ...and %d more\r\n", missingCount - 30);
        missingList += tail;
    }

    if (ok)
    {
        MessageBoxA(NULL, "UAS built successfully.", "Message",
                    MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        UAS_RestoreEditorFocus(hBrowser);
    }
    else
    {
        std::string msg;
        if (missingCount > 0)
        {
            char tmp[128];
            snprintf(tmp, sizeof(tmp),
                     "Build failed - missing source audio for %d sound%s:\r\n\r\n",
                     missingCount, (missingCount == 1 ? "" : "s"));
            msg += tmp;
            msg += missingList;
            msg += "\r\nDrop a .wav or .ogg with each of those names into\r\n"
                   "..\\Packages\\Sounds\\ and try Build UAS again.";
        }
        else
        {
            msg += "Build failed.\r\n\r\n"
                   "All source audio was found, but the engine couldn't write\r\n"
                   "..\\Packages\\Sounds\\<map>.uas. The most common cause is the\r\n"
                   "file being open by another process. Close the game if it's\r\n"
                   "running and try again.";
        }
        MessageBoxA(NULL, msg.c_str(), "Message",
                    MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        UAS_RestoreEditorFocus(hBrowser);
    }
}

static void __cdecl SB_HandleExportUAS(void* this_ptr)
{
    HWND hBrowser = GetParentHWND(this_ptr);

    // 1. Ask the user which .uas file to extract from.
    char uasPath[MAX_PATH * 2] = {};
    {
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hBrowser;
        ofn.lpstrFilter = "Sound Packages (*.uas)\0*.uas\0All Files (*.*)\0*.*\0";
        ofn.lpstrDefExt = "uas";
        ofn.lpstrFile   = uasPath;
        ofn.nMaxFile    = sizeof(uasPath);
        ofn.lpstrTitle  = "Export Sound";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&ofn)) return;
    }

    // 2. Read the entire file into memory.
    std::vector<uint8_t> data;
    {
        HANDLE h = CreateFileA(uasPath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            MessageBoxA(NULL,
                "Could not open the selected .uas package.",
                "Message",
                MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
            UAS_RestoreEditorFocus(hBrowser);
            return;
        }
        DWORD fileSize = GetFileSize(h, nullptr);
        if (fileSize < 8 || fileSize == INVALID_FILE_SIZE) {
            CloseHandle(h);
            MessageBoxA(NULL, "That file is too small to be a valid .uas.",
                "Message", MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
            UAS_RestoreEditorFocus(hBrowser);
            return;
        }
        data.resize(fileSize);
        DWORD got = 0;
        ReadFile(h, data.data(), fileSize, &got, nullptr);
        CloseHandle(h);
        if (got != fileSize) data.resize(got);
    }
    const size_t fsize = data.size();
    auto rdU32 = [&](size_t off) -> uint32_t {
        if (off + 4 > fsize) return 0;
        uint32_t v;
        memcpy(&v, data.data() + off, 4);
        return v;
    };

    // 3. Parse sound entries. Two file layouts coexist in the wild:
    //
    //   (a) UNIFIED — one sound table immediately after the package list.
    //       AquaD-Original.uas and TME30.uas both look like this: every
    //       sound from every package is concatenated into a single block
    //       starting at offset (4 + 68 * pkgCount).
    //
    //   (b) PER-PACKAGE — each package's 4-byte offset field points to its
    //       own sound table (header + entries). Files we rebuild with our
    //       Build UAS using two packages (Amb_<map>.uax + Interface.uax)
    //       have been seen in this form, so iterating only the unified
    //       slot drops every package after the first.
    //
    // We probe BOTH locations and dedupe by sound start-offset (each OGG
    // payload sits at a unique file offset, so the same physical sound
    // can't be double-counted).
    uint32_t pkgCount = rdU32(0);
    if (pkgCount > 64u) {
        MessageBoxA(NULL,
            "Header is not a recognized UAS layout (package_count looked wrong).",
            "Message", MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        UAS_RestoreEditorFocus(hBrowser);
        return;
    }

    struct ParsedSound { char name[65]; uint32_t startOff; uint32_t endOff; };
    std::vector<ParsedSound>      collected;
    std::unordered_set<uint32_t>  visitedTables;  // sound-table offsets already walked
    std::unordered_set<uint32_t>  seenStart;      // dedupe by audio start offset

    int rejected = 0;
    std::string rejectedList;

    auto tryParseTable = [&](size_t tableOff) {
        if (tableOff + 4u > fsize) return;
        if (!visitedTables.insert(static_cast<uint32_t>(tableOff)).second) return;

        uint32_t count = rdU32(tableOff);
        if (count == 0 || count > 100000u) return;
        if (tableOff + 4u + (size_t)count * 80u > fsize) return;

        for (uint32_t i = 0; i < count; ++i)
        {
            size_t entry = tableOff + 4u + (size_t)i * 80u;
            ParsedSound s = {};
            memcpy(s.name, data.data() + entry, 64);
            s.name[64] = '\0';
            memcpy(&s.startOff, data.data() + entry + 64,     4);
            memcpy(&s.endOff,   data.data() + entry + 64 + 8, 4);

            bool valid =
                (s.startOff < fsize) &&
                (s.endOff <= fsize) &&
                (s.endOff > s.startOff) &&
                ((s.endOff - s.startOff) >= 4) &&
                (memcmp(data.data() + s.startOff, "OggS", 4) == 0);
            if (!valid) {
                ++rejected;
                if (rejected <= 10) {
                    char line[300];
                    snprintf(line, sizeof(line),
                             "  %s  (start=0x%X end=0x%X)\r\n",
                             s.name[0] ? s.name : "<empty>",
                             s.startOff, s.endOff);
                    rejectedList += line;
                }
                continue;
            }
            if (!seenStart.insert(s.startOff).second) continue;
            collected.push_back(s);
        }
    };

    // (a) Unified table — right after the package entries.
    tryParseTable(4u + (size_t)pkgCount * 68u);

    // (b) Per-package — each package's offset field, if it looks valid.
    for (uint32_t p = 0; p < pkgCount; ++p)
    {
        size_t pkgEntry = 4u + (size_t)p * 68u;
        if (pkgEntry + 68u > fsize) break;
        uint32_t pkgOff = rdU32(pkgEntry + 64);
        if (pkgOff < 4u || pkgOff >= fsize) continue;
        tryParseTable(pkgOff);
    }

    if (collected.empty()) {
        MessageBoxA(NULL,
            "No valid sound entries found in that UAS file.\r\n"
            "(No OGG payloads passed the OggS-magic validation.)",
            "Message", MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        UAS_RestoreEditorFocus(hBrowser);
        return;
    }

    // 4. Make output folder: same name as the .uas (no _Extracted suffix).
    char outDir[MAX_PATH * 2] = {};
    {
        const char* lastSlash = strrchr(uasPath, '\\');
        size_t dirLen = lastSlash ? (size_t)(lastSlash - uasPath + 1) : 0;
        memcpy(outDir, uasPath, dirLen);
        const char* base = lastSlash ? lastSlash + 1 : uasPath;
        char stem[256];
        strncpy_s(stem, sizeof(stem), base, _TRUNCATE);
        char* dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        snprintf(outDir + dirLen, sizeof(outDir) - dirLen, "%s", stem);
        CreateDirectoryA(outDir, nullptr);
    }

    // 5. Write each collected sound's OGG payload.
    int exported = 0;
    int skipped  = rejected;
    std::string skippedList = rejectedList;

    for (const auto& s : collected)
    {
        char outFile[MAX_PATH * 3];
        snprintf(outFile, sizeof(outFile), "%s\\%s.ogg", outDir,
                 s.name[0] ? s.name : "_unnamed");

        HANDLE hOut = CreateFileA(outFile, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE) {
            ++skipped;
            continue;
        }
        DWORD wrote = 0;
        WriteFile(hOut, data.data() + s.startOff,
                  (DWORD)(s.endOff - s.startOff), &wrote, nullptr);
        CloseHandle(hOut);
        ++exported;
    }

    // 6. Result dialog
    std::string msg;
    char tmp[600];
    snprintf(tmp, sizeof(tmp),
             "UAS package exported successfully to the Sounds folder.");
    msg += tmp;
    if (skipped > 0)
    {
        snprintf(tmp, sizeof(tmp),
                 "\r\nSkipped %d entr%s (invalid offset or missing OggS magic):\r\n",
                 skipped, skipped == 1 ? "y" : "ies");
        msg += tmp;
        msg += skippedList;
        if (skipped > 10) {
            snprintf(tmp, sizeof(tmp), "  ...and %d more\r\n", skipped - 10);
            msg += tmp;
        }
    }
    AllowSetForegroundWindow(ASFW_ANY);
    MessageBoxA(NULL, msg.c_str(),
        exported > 0 ? "Message" : "No audio files found in UAS package.",
        MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    UAS_RestoreEditorFocus(hBrowser);
}

static void __cdecl SB_HandleExport(void* this_ptr)
{
    HWND hParent = GetParentHWND(this_ptr);
    HWND hList   = GetListHWND(this_ptr);

    // Collect all selected ListView indices
    int selItems[512];
    int selCount = 0;
    LRESULT idx  = -1;
    while (selCount < 512)
    {
        idx = SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(idx), LVNI_SELECTED);
        if (idx < 0) break;
        selItems[selCount++] = static_cast<int>(idx);
    }

    if (selCount == 0)
    {
        MessageBoxA(hParent, "Select a sound first.", "Message",
                    MB_OK);
        return;
    }

    char pkgName[256] = {};
    GetWindowTextA(GetPackageComboHWND(this_ptr), pkgName, sizeof(pkgName));

    for (int i = 0; i < selCount; ++i)
    {
        char soundName[256] = {};
        LVITEMA lvi    = {};
        lvi.mask       = LVIF_TEXT;
        lvi.iItem      = selItems[i];
        lvi.iSubItem   = 0;
        lvi.pszText    = soundName;
        lvi.cchTextMax = sizeof(soundName);
        SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

        if (soundName[0] == '\0') continue;

        // Pre-fill the save path with SoundName.wav
        char filePath[MAX_PATH * 2] = {};
        snprintf(filePath, sizeof(filePath), "%s.wav", soundName);

        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hParent;
        ofn.lpstrFilter = "Wave Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
        ofn.lpstrDefExt = "wav";
        ofn.lpstrFile   = filePath;
        ofn.nMaxFile    = sizeof(filePath);
        ofn.lpstrTitle  = "Export Sound";
        ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameA(&ofn))
            break;

        char cmd[MAX_PATH * 2 + 256];
        snprintf(cmd, sizeof(cmd),
                 "OBJ EXPORT TYPE=SOUND PACKAGE=\"%s\" NAME=\"%s\" FILE=\"%s\"",
                 pkgName, soundName, filePath);
        ExecEditorCommand(cmd);
    }
}

static void __cdecl SB_HandleImport(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    char defPkg[256] = {};
    char defGrp[256] = {};
    GetWindowTextA(hPkgCombo, defPkg, sizeof(defPkg));
    GetWindowTextA(hGrpCombo, defGrp, sizeof(defGrp));

    static char fileBuffer[32768];  // static to avoid large stack alloc; zeroed each call
    memset(fileBuffer, 0, sizeof(fileBuffer));

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hParent;
    ofn.lpstrFilter = "Wave Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = "wav";
    ofn.lpstrFile   = fileBuffer;
    ofn.nMaxFile    = sizeof(fileBuffer);
    ofn.lpstrTitle  = "Import Sounds";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
                    | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn))
        return;

    // Single-file: "C:\dir\file.wav\0"; Multi-file: "C:\dir\0file1.wav\0file2.wav\0\0"
    const char* firstStr  = fileBuffer;
    bool        multiSel  = (*(firstStr + strlen(firstStr) + 1) != '\0');

    char dirBuf[MAX_PATH] = {};
    const char* cursor    = firstStr;

    if (multiSel)
    {
        strncpy_s(dirBuf, firstStr, _TRUNCATE);
        size_t dl = strlen(dirBuf);
        if (dl > 0 && dirBuf[dl - 1] != '\\' && dirBuf[dl - 1] != '/')
            { dirBuf[dl] = '\\'; dirBuf[dl + 1] = '\0'; }
        cursor = firstStr + strlen(firstStr) + 1; // advance to first filename
    }

    HINSTANCE hExeInst    = GetModuleHandleA(NULL);
    bool      okAllMode   = false;
    bool      anyImported = false;
    char      lastPkg[256] = {};
    char      lastGrp[256] = {};
    strncpy_s(lastPkg, defPkg, _TRUNCATE);
    strncpy_s(lastGrp, defGrp, _TRUNCATE);

    for (;;)
    {
        if (multiSel && *cursor == '\0')
            break;  // end of list

        char fullPath[MAX_PATH * 2] = {};
        if (multiSel)
            snprintf(fullPath, sizeof(fullPath), "%s%s", dirBuf, cursor);
        else
            strncpy_s(fullPath, cursor, _TRUNCATE);

        ImportSoundData data = {};
        strncpy_s(data.filePath, fullPath,  _TRUNCATE);
        strncpy_s(data.pkgName,  lastPkg,   _TRUNCATE);
        strncpy_s(data.grpName,  lastGrp,   _TRUNCATE);

        const char* fileOnly = fullPath;
        for (const char* q = fullPath; *q; ++q)
            if (*q == '\\' || *q == '/') fileOnly = q + 1;
        strncpy_s(data.soundName, sizeof(data.soundName), fileOnly, _TRUNCATE);
        char* dot = strrchr(data.soundName, '.');
        if (dot) *dot = '\0';

        INT_PTR dlgResult = IDOK;
        if (!okAllMode)
        {
            dlgResult = DialogBoxParamA(hExeInst,
                                        MAKEINTRESOURCEA(IDD_IMPORT_SOUND),
                                        hParent,
                                        ImportSoundDlgProc,
                                        reinterpret_cast<LPARAM>(&data));
        }

        if (dlgResult == IDCANCEL)
            break;

        if (dlgResult == IDB_IMPORT_SKIP)
        {
            if (!multiSel) break;
            cursor += strlen(cursor) + 1;
            continue;
        }

        if (dlgResult == IDB_IMPORT_OKALL)
            okAllMode = true;

        if (data.pkgName[0] == '\0')
        {
            if (!multiSel) break;
            cursor += strlen(cursor) + 1;
            continue;
        }

        strncpy_s(lastPkg, data.pkgName, _TRUNCATE);
        strncpy_s(lastGrp, data.grpName, _TRUNCATE);

        // Short (8.3) path to prevent spaces confusing the UE2 command parser
        char shortPath[MAX_PATH * 2] = {};
        if (GetShortPathNameA(data.filePath, shortPath, sizeof(shortPath)) == 0)
            strncpy_s(shortPath, sizeof(shortPath), data.filePath, _TRUNCATE);

        char cmd[MAX_PATH * 2 + 256];
        if (data.grpName[0] != '\0')
            snprintf(cmd, sizeof(cmd),
                     "AUDIO IMPORT FILE=\"%s\" PACKAGE=\"%s\" NAME=\"%s\" GROUP=\"%s\"",
                     shortPath, data.pkgName, data.soundName, data.grpName);
        else
            snprintf(cmd, sizeof(cmd),
                     "AUDIO IMPORT FILE=\"%s\" PACKAGE=\"%s\" NAME=\"%s\"",
                     shortPath, data.pkgName, data.soundName);
        ExecEditorCommand(cmd);
        anyImported = true;

        if (!multiSel)
            break;

        cursor += strlen(cursor) + 1;
    }

    if (anyImported)
    {
        RefreshList(this_ptr);
        NavigateToPackageGroup(this_ptr, lastPkg, lastGrp);
    }
}

static void __cdecl SB_HandleDelete(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hList     = GetListHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    LRESULT sel = SendMessageA(hList, LVM_GETNEXTITEM,
                               static_cast<WPARAM>(-1), LVNI_SELECTED);
    if (sel < 0)
        return;

    char soundName[256] = {};
    LVITEMA lvi    = {};
    lvi.mask       = LVIF_TEXT;
    lvi.iItem      = static_cast<int>(sel);
    lvi.iSubItem   = 0;
    lvi.pszText    = soundName;
    lvi.cchTextMax = sizeof(soundName);
    SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

    if (soundName[0] == '\0')
        return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "DELETE CLASS=SOUND OBJECT=\"%s\"", soundName);
    CallEditorGet("OBJ", cmd);

    char pkgName[256] = {};
    char grpName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    GetWindowTextA(hGrpCombo, grpName, sizeof(grpName));

    char fullName[768] = {};
    if (grpName[0])
        snprintf(fullName, sizeof(fullName), "%s.%s.%s", pkgName, grpName, soundName);
    else
        snprintf(fullName, sizeof(fullName), "%s.%s", pkgName, soundName);

    RefreshList(this_ptr);

    LRESULT count = SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; i++)
    {
        char itemText[256] = {};
        LVITEMA check     = {};
        check.mask        = LVIF_TEXT;
        check.iItem       = static_cast<int>(i);
        check.iSubItem    = 0;
        check.pszText     = itemText;
        check.cchTextMax  = sizeof(itemText);
        SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&check));
        if (_stricmp(itemText, soundName) == 0)
        {
            char errMsg[512];
            snprintf(errMsg, sizeof(errMsg),
                     "Can't delete sound.\n\nSound %s is in use.", fullName);
            MessageBoxA(hParent, errMsg, "Message", MB_OK);
            return;
        }
    }

    NavigateAfterDelete(this_ptr);
}

static void __cdecl SB_HandleRename(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hList     = GetListHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    LRESULT sel = SendMessageA(hList, LVM_GETNEXTITEM,
                               static_cast<WPARAM>(-1), LVNI_SELECTED);
    if (sel < 0)
    {
        return;
    }

    char soundName[256] = {};
    LVITEMA lvi    = {};
    lvi.mask       = LVIF_TEXT;
    lvi.iItem      = static_cast<int>(sel);
    lvi.iSubItem   = 0;
    lvi.pszText    = soundName;
    lvi.cchTextMax = sizeof(soundName);
    SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

    if (soundName[0] == '\0')
        return;

    RenameSoundData data = {};
    strncpy_s(data.oldName,    sizeof(data.oldName),    soundName, _TRUNCATE);
    GetWindowTextA(hPkgCombo, data.oldPackage, sizeof(data.oldPackage));
    GetWindowTextA(hGrpCombo, data.oldGroup,   sizeof(data.oldGroup));

    HINSTANCE hExeInst = GetModuleHandleA(NULL);
    INT_PTR result = DialogBoxParamA(hExeInst,
                                     MAKEINTRESOURCEA(IDDIALOG_RENAME),
                                     hParent,
                                     RenameSoundDlgProc,
                                     reinterpret_cast<LPARAM>(&data));
    if (result != IDOK)
        return;

    char cmd[MAX_PATH * 2 + 512];
    snprintf(cmd, sizeof(cmd),
             "OBJ RENAME OLDNAME=\"%s\" OLDGROUP=\"%s\" OLDPACKAGE=\"%s\""
             " NEWNAME=\"%s\" NEWGROUP=\"%s\" NEWPACKAGE=\"%s\"",
             data.oldName,    data.oldGroup,    data.oldPackage,
             data.newName,    data.newGroup,    data.newPackage);
    ExecEditorCommand(cmd);
    RefreshList(this_ptr);
    NavigateToPackageGroup(this_ptr, data.newPackage, data.newGroup);
}

// Cache the most recently seen WBrowserSound instance so
// other modules (AnimationBrowser's "Use" button) can ask which sound
// is currently highlighted without owning their own hooks.  This hook
// fires every time the SoundBrowser populates its listview, so the
// cached pointer is always current as long as the user has opened the
// SoundBrowser at least once this session.
static void* g_lastSoundBrowserPtr = nullptr;

static void __cdecl StoreLParamHelper(void* this_ptr, int item_index,
                                       void* usound_ptr, const char* name)
{
    g_lastSoundBrowserPtr = this_ptr;

    void* list_obj = *reinterpret_cast<void**>(static_cast<char*>(this_ptr) + 0x94);
    HWND  hwndList = *reinterpret_cast<HWND*>(static_cast<char*>(list_obj)  + 4);

    LVITEMA lvi  = {};
    lvi.mask     = LVIF_PARAM;
    lvi.iItem    = item_index;
    lvi.iSubItem = 0;
    lvi.lParam   = reinterpret_cast<LPARAM>(usound_ptr);
    SendMessageA(hwndList, LVM_SETITEM, 0, reinterpret_cast<LPARAM>(&lvi));

    if (usound_ptr && name && name[0] != '\0')
        g_soundNameCache[usound_ptr] = name;
}

namespace SoundBrowser {
    void* PeekSelectedSound()
    {
        if (!g_lastSoundBrowserPtr) return nullptr;
        // GetSelectedSound is declared static earlier in this file; its
        // body reads the listview's current selection's lParam (the
        // USound*) and returns it.  Wrapped in SEH so a stale browser
        // pointer (e.g. after the user closed the SoundBrowser window)
        // can't blow up the caller.
        void* result = nullptr;
        __try { result = GetSelectedSound(g_lastSoundBrowserPtr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }
        return result;
    }
}

static void* __cdecl GetSelectedSound(void* this_ptr)
{
    void* list_obj = *reinterpret_cast<void**>(static_cast<char*>(this_ptr) + 0x94);
    HWND  hwndList = *reinterpret_cast<HWND*>(static_cast<char*>(list_obj)  + 4);

    LRESULT sel = SendMessageA(hwndList, LVM_GETNEXTITEM,
                               static_cast<WPARAM>(-1), LVNI_SELECTED);
    if (sel < 0) return nullptr;

    LVITEMA lvi  = {};
    lvi.mask     = LVIF_PARAM;
    lvi.iItem    = static_cast<int>(sel);
    lvi.iSubItem = 0;
    SendMessageA(hwndList, LVM_GETITEM, 0, reinterpret_cast<LPARAM>(&lvi));

    return reinterpret_cast<void*>(lvi.lParam);
}

static INT_PTR CALLBACK SoundPropsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        void* pSound = reinterpret_cast<void*>(lParam);
        if (!pSound) return TRUE;

        DWORD flags  = *reinterpret_cast<DWORD*>(static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
        float volume = *reinterpret_cast<float*>(static_cast<char*>(pSound) + USOUND_VOLUME_OFFSET);
        float radius = *reinterpret_cast<float*>(static_cast<char*>(pSound) + USOUND_RADIUS_OFFSET);

        // IDC_CHK_STREAM maps to SF_UAS_STREAM (0x4); original code wrongly used SF_STREAM (0x100) = Random type bit
        CheckDlgButton(hDlg, IDC_CHK_STREAM, (flags & SF_UAS_STREAM) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_2D,     (flags & SF_2D)     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_LOOP,   (flags & SF_LOOP)   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_XBOXHD,   (flags & SF_XBOXHD_STREAM) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_SURROUND,  (flags & SF_TYPE_SURROUND) ? BST_CHECKED : BST_UNCHECKED);

        bool hasRadius = (radius > 0.0f);
        CheckDlgButton(hDlg, IDC_CHK_OVR_RADIUS,
                       hasRadius ? BST_CHECKED : BST_UNCHECKED);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4f", radius);
            SetDlgItemTextA(hDlg, IDC_EDIT_RADIUS, buf);
        }

        bool hasVolume = (flags & SF_OVR_VOLUME) != 0;
        CheckDlgButton(hDlg, IDC_CHK_OVR_VOLUME,
                       hasVolume ? BST_CHECKED : BST_UNCHECKED);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f",
                     hasVolume ? LinearToDB(volume) : 0.0f);
            SetDlgItemTextA(hDlg, IDC_EDIT_VOLUME, buf);
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            void* pSound = reinterpret_cast<void*>(GetWindowLongA(hDlg, GWL_USERDATA));
            if (pSound)
            {
                DWORD* pFlags  = reinterpret_cast<DWORD*>(static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
                float* pVolume = reinterpret_cast<float*>(static_cast<char*>(pSound) + USOUND_VOLUME_OFFSET);
                float* pRadius = reinterpret_cast<float*>(static_cast<char*>(pSound) + USOUND_RADIUS_OFFSET);

                DWORD flags = *pFlags;

                auto applyFlag = [&](DWORD mask, int ctrlId) {
                    if (IsDlgButtonChecked(hDlg, ctrlId) == BST_CHECKED)
                        flags |= mask;
                    else
                        flags &= ~mask;
                };

                applyFlag(SF_UAS_STREAM,    IDC_CHK_STREAM);
                applyFlag(SF_2D,            IDC_CHK_2D);
                applyFlag(SF_LOOP,          IDC_CHK_LOOP);
                applyFlag(SF_XBOXHD_STREAM, IDC_CHK_XBOXHD);
                applyFlag(SF_TYPE_SURROUND, IDC_CHK_SURROUND);

                if (IsDlgButtonChecked(hDlg, IDC_CHK_OVR_RADIUS) == BST_CHECKED)
                {
                    char buf[32] = {};
                    GetDlgItemTextA(hDlg, IDC_EDIT_RADIUS, buf,
                                    static_cast<int>(sizeof(buf)));
                    *pRadius = static_cast<float>(atof(buf));
                }
                else
                {
                    *pRadius = 0.0f;
                }

                // 0 dB = unity; silently clear the flag rather than serialising a no-op override
                if (IsDlgButtonChecked(hDlg, IDC_CHK_OVR_VOLUME) == BST_CHECKED)
                {
                    char buf[32] = {};
                    GetDlgItemTextA(hDlg, IDC_EDIT_VOLUME, buf,
                                    static_cast<int>(sizeof(buf)));
                    float db = static_cast<float>(atof(buf));
                    if (db == 0.0f)
                    {
                        flags &= ~SF_OVR_VOLUME;
                        *pVolume = 1.0f;
                    }
                    else
                    {
                        flags |= SF_OVR_VOLUME;
                        *pVolume = DBToLinear(db);
                    }
                }
                else
                {
                    flags &= ~SF_OVR_VOLUME;
                    *pVolume = 1.0f;
                }

                *pFlags = flags;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

// IDDIALOG_RENAME (19805): IDEC_NEWPACKAGE=1066, IDEC_NEWGROUP=1067, IDEC_NAME=1065
static INT_PTR CALLBACK RenameSoundDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        RenameSoundData* d = reinterpret_cast<RenameSoundData*>(lParam);
        if (!d) return TRUE;

        SetDlgItemTextA(hDlg, IDEC_NEWPACKAGE, d->oldPackage);
        SetDlgItemTextA(hDlg, IDEC_NEWGROUP,   d->oldGroup);
        SetDlgItemTextA(hDlg, IDEC_NAME,       d->oldName);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            RenameSoundData* d = reinterpret_cast<RenameSoundData*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (d)
            {
                GetDlgItemTextA(hDlg, IDEC_NEWPACKAGE,
                                d->newPackage, sizeof(d->newPackage));
                GetDlgItemTextA(hDlg, IDEC_NEWGROUP,
                                d->newGroup,   sizeof(d->newGroup));
                GetDlgItemTextA(hDlg, IDEC_NAME,
                                d->newName,    sizeof(d->newName));
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

// dialog 156: IDB_IMPORT_OKALL=3 acts like IDOK; IDB_IMPORT_SKIP=4 acts like IDCANCEL
static INT_PTR CALLBACK ImportSoundDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        ImportSoundData* d = reinterpret_cast<ImportSoundData*>(lParam);
        if (!d) return TRUE;

        SetDlgItemTextA(hDlg, IDC_IMPORT_FILE,    d->filePath);
        SetDlgItemTextA(hDlg, IDC_IMPORT_PACKAGE, d->pkgName);
        SetDlgItemTextA(hDlg, IDC_IMPORT_GROUP,   d->grpName);
        SetDlgItemTextA(hDlg, IDC_IMPORT_NAME,    d->soundName);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:           // button 1 – OK
        case IDB_IMPORT_OKALL: // button 3 – OK All
        {
            ImportSoundData* d = reinterpret_cast<ImportSoundData*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (d)
            {
                GetDlgItemTextA(hDlg, IDC_IMPORT_PACKAGE,
                                d->pkgName,   sizeof(d->pkgName));
                GetDlgItemTextA(hDlg, IDC_IMPORT_GROUP,
                                d->grpName,   sizeof(d->grpName));
                GetDlgItemTextA(hDlg, IDC_IMPORT_NAME,
                                d->soundName, sizeof(d->soundName));
            }
            // Return distinct values so the caller can tell OK from OK All
            EndDialog(hDlg, LOWORD(wParam)); // IDOK (1) or IDB_IMPORT_OKALL (3)
            return TRUE;
        }
        case IDCANCEL:       // button 2 – Cancel
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        case IDB_IMPORT_SKIP:// button 4 – Skip
            EndDialog(hDlg, IDB_IMPORT_SKIP);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

#ifdef _DEBUG
// Shift+Properties: dumps flags for all USounds in the current package to %TEMP%\scct_flag_dump.txt.

static const char* SB_ReadFName(void* pObj)
{
    if (!pObj) return "<null>";

    // FName.Index at UObject+0x20; FNameEntry string at FNameEntry+0x0C
    int fnameIdx = *reinterpret_cast<int*>(static_cast<char*>(pObj) + UOBJ_FNAME_OFFSET);

    int gnamesNum = *reinterpret_cast<int*>(GNAMES_NUM);
    if (fnameIdx < 0 || fnameIdx >= gnamesNum)
        return "<badIdx>";

    int gnamesBase = *reinterpret_cast<int*>(GNAMES_DATA);
    if (!gnamesBase) return "<noGNames>";

    int fnameEntry = *reinterpret_cast<int*>(gnamesBase + fnameIdx * 4);
    if (!fnameEntry) return "<nullEntry>";

    return reinterpret_cast<const char*>(fnameEntry + FNAME_ENTRY_STR_OFFSET);
}

static void __cdecl SB_ShowFlagsDump(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    char pkgName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    if (pkgName[0] == '\0')
    {
        return;
    }

    void** gobjData  = *reinterpret_cast<void***>(GOBJECTS_DATA);
    int    gobjCount = *reinterpret_cast<int*>   (GOBJECTS_NUM);
    if (!gobjData || gobjCount <= 0)
    {
        MessageBoxA(hParent, "GObjects is empty or unavailable.",
                    "Message", MB_OK);
        return;
    }

    std::string text;
    text.reserve(131072);

    {
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
                 "SCCT Sound Browser – Flag Dump\r\n"
                 "Package : %s\r\n"
                 "GObjects.Num = %d\r\n"
                 "\r\n"
                 "%-6s  %-40s  %-10s  %s\r\n"
                 "%-6s  %-40s  %-10s  %s\r\n",
                 pkgName, gobjCount,
                 "Slot",  "Name (Group.Sound)",        "Flags",    "Type",
                 "----",  "------------------",         "-----",    "----");
        text += hdr;
    }

    int nTotal   = 0;
    int nHidden  = 0;
    int nSwitch  = 0;
    int nRandom  = 0;
    int nSeq     = 0;
    int nWave    = 0;

    for (int i = 0; i < gobjCount; i++)
    {
        void* pObj = gobjData[i];
        if (!pObj) continue;

        // ---- Walk Outer chain to find the top-level package ----
        void* pTopOuter = nullptr;
        void* pCur      = pObj;
        for (int depth = 0; depth < 16; depth++)
        {
            void* pOuter = *reinterpret_cast<void**>(
                static_cast<char*>(pCur) + UOBJ_OUTER_OFFSET);
            if (!pOuter)
            {
                pTopOuter = pCur;   // pCur has no Outer > it IS the package
                break;
            }
            pCur = pOuter;
        }

        if (!pTopOuter || pObj == pTopOuter) continue;

        const char* topName = SB_ReadFName(pTopOuter);
        if (_stricmp(topName, pkgName) != 0) continue;

        // ---- Build display name: "GroupName.SoundName" or "SoundName" ----
        char dispName[128] = {};
        {
            void* pOuter = *reinterpret_cast<void**>(
                static_cast<char*>(pObj) + UOBJ_OUTER_OFFSET);

            // Does pOuter have its own Outer? If so, pOuter is a group object.
            bool hasGroup = false;
            if (pOuter && pOuter != pTopOuter)
            {
                void* pOuterOuter = *reinterpret_cast<void**>(
                    static_cast<char*>(pOuter) + UOBJ_OUTER_OFFSET);
                hasGroup = (pOuterOuter != nullptr);
            }

            const char* ownName   = SB_ReadFName(pObj);
            const char* outerName = pOuter ? SB_ReadFName(pOuter) : "";

            if (hasGroup)
                snprintf(dispName, sizeof(dispName), "%s.%s", outerName, ownName);
            else
                snprintf(dispName, sizeof(dispName), "%s", ownName);
        }

        DWORD flags = *reinterpret_cast<DWORD*>(
            static_cast<char*>(pObj) + USOUND_FLAGS_OFFSET);

        const char* typeName;
        bool isHidden = false;

        if (flags & SF_TYPE_SURROUND)
        {
            // Xbox HD 4.0/5.1 surround; may also carry SF_UAS_STREAM (MC_ prefix sounds)
            typeName = (flags & SF_UAS_STREAM) ? "Surround+UASStream" : "Surround";
        }
        else if (flags & SF_TYPE_SWITCH)   { typeName = "Switch";    nSwitch++; }
        else if (flags & SF_TYPE_SEQUENCE) { typeName = "Sequence";  nSeq++;    }
        else if (flags & SF_TYPE_RANDOM)   { typeName = "Random";    nRandom++; }
        else if (flags & SF_XBOXHD_STREAM)
        {
            // Xbox HD Stream (.hds) sound.  WAV data IS present in the UAX; Ubisoft
            // suppressed these from the PC editor browser since the PC build has no
            // .hds pipeline.  Export works normally since the WAV data is intact.
            typeName = "Wave[XboxHDStream]";
            isHidden = true;
        }
        else if (flags & SF_UAS_STREAM)
        {
            // UAS streaming stub - actual OGG audio lives in the map's .uas file.
            // No WAV data in the UAX; Export will not produce usable audio.
            typeName = "Wave[UASStream]";
            isHidden = true;
        }
        else                               { typeName = "Wave";       nWave++;   }

        if (isHidden) nHidden++;
        nTotal++;

        {
            char line[256];
            snprintf(line, sizeof(line),
                     "%-6d  %-40s  0x%08X  %s\r\n",
                     i, dispName, static_cast<unsigned>(flags), typeName);
            text += line;
        }
    }

    {
        char summ[512];
        snprintf(summ, sizeof(summ),
                 "\r\n"
                 "=== Summary for %s ===\r\n"
                 "  Total objects in package : %d\r\n"
                 "  Waves                    : %d\r\n"
                 "  Randoms                  : %d\r\n"
                 "  Sequences                : %d\r\n"
                 "  Switches                 : %d\r\n"
                 "  HIDDEN (0x4 / 0x4000)    : %d\r\n",
                 pkgName,
                 nTotal, nWave, nRandom, nSeq, nSwitch, nHidden);
        text += summ;
    }

    char tmpPath[MAX_PATH] = {};
    GetTempPathA(sizeof(tmpPath), tmpPath);
    strncat_s(tmpPath, sizeof(tmpPath), "scct_flag_dump.txt", _TRUNCATE);

    HANDLE hFile = CreateFileA(tmpPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        MessageBoxA(hParent,
                    "Could not write temp file for flag dump.",
                    "Message", MB_OK);
        return;
    }

    DWORD written = 0;
    WriteFile(hFile, text.c_str(), static_cast<DWORD>(text.size()), &written, NULL);
    CloseHandle(hFile);

    char notepadCmd[MAX_PATH + 32] = {};
    snprintf(notepadCmd, sizeof(notepadCmd), "notepad.exe \"%s\"", tmpPath);

    STARTUPINFOA  si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    if (CreateProcessA(NULL, notepadCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else
    {
        // Fallback: show short summary in a message box if Notepad failed
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Package: %s\r\n"
                 "Total: %d  |  Hidden (0x4/0x4000): %d\r\n"
                 "(Could not open Notepad; file saved to: %s)",
                 pkgName, nTotal, nHidden, tmpPath);
        MessageBoxA(hParent, msg, "Message", MB_OK);
    }
}
#endif // _DEBUG

static void __cdecl ShowPropertiesDialogHelper(void* this_ptr)
{
#ifdef _DEBUG
    // Shift held > flag dump (debug builds only)
    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        SB_ShowFlagsDump(this_ptr);
        return;
    }
#endif

    void* pSound = GetSelectedSound(this_ptr);
    if (!pSound) return;

    HINSTANCE hExeInst = GetModuleHandleA(NULL);
    HWND hParent = *reinterpret_cast<HWND*>(static_cast<char*>(this_ptr) + 4);

    INT_PTR res = DialogBoxParamA(hExeInst,
                    MAKEINTRESOURCEA(IDD_SOUND_PROPS),
                    hParent,
                    SoundPropsDlgProc,
                    reinterpret_cast<LPARAM>(pSound));
    if (res == IDOK)
        RefreshList(this_ptr);
}

// Minimal 1-sample silent WAV; needed by AUDIO IMPORT - the WAV data is ignored for
// composite types, only the USound wrapper and the flags set afterwards matter.
static const uint8_t kSilentWAV[] = {
    // RIFF header
    'R','I','F','F', 0x25,0x00,0x00,0x00,   // chunk size = 37 bytes after this field
    'W','A','V','E',
    // fmt chunk (PCM, 1ch, 8000 Hz, 8-bit)
    'f','m','t',' ', 0x10,0x00,0x00,0x00,
    0x01,0x00,                               // wFormatTag = PCM
    0x01,0x00,                               // nChannels  = 1
    0x40,0x1F,0x00,0x00,                     // nSamplesPerSec = 8000
    0x40,0x1F,0x00,0x00,                     // nAvgBytesPerSec= 8000
    0x01,0x00,                               // nBlockAlign = 1
    0x08,0x00,                               // wBitsPerSample = 8
    // data chunk (1 silent sample)
    'd','a','t','a', 0x01,0x00,0x00,0x00,
    0x80                                     // 128 = silence for unsigned 8-bit PCM
};  // total: 45 bytes

static bool WriteSilentWAV(char* outPath, size_t pathSize)
{
    char tempDir[MAX_PATH] = {};
    GetTempPathA(sizeof(tempDir), tempDir);
    snprintf(outPath, pathSize, "%sscct_composite.wav", tempDir);

    HANDLE h = CreateFileA(outPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL  ok      = WriteFile(h, kSilentWAV, sizeof(kSilentWAV), &written, NULL);
    CloseHandle(h);
    return ok && written == sizeof(kSilentWAV);
}

static void GetSoundName(void* pBrowser, void* pSound, char* outBuf, int maxLen)
{
    outBuf[0] = '\0';
    if (!pSound || !pBrowser) return;

    HWND    hList = GetListHWND(pBrowser);
    LRESULT count = SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0);

    for (LRESULT i = 0; i < count; i++)
    {
        LVITEMA lvi  = {};
        lvi.mask     = LVIF_PARAM;
        lvi.iItem    = static_cast<int>(i);
        lvi.iSubItem = 0;
        SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

        if (reinterpret_cast<void*>(lvi.lParam) == pSound)
        {
            lvi.mask       = LVIF_TEXT;
            lvi.pszText    = outBuf;
            lvi.cchTextMax = maxLen;
            SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));
            return;
        }
    }

    // Fallback: name cache populated by StoreLParamHelper (resolves cross-package refs)
    auto it = g_soundNameCache.find(pSound);
    if (it != g_soundNameCache.end())
    {
        _snprintf_s(outBuf, maxLen, _TRUNCATE, "%s", it->second.c_str());
        return;
    }

    // Last resort: show pointer so the child entry still appears in the list.
    snprintf(outBuf, maxLen, "<?? %p>", pSound);
}

static void PopulateComboWithSounds(void* pBrowser, HWND hCombo, void* excludeSound)
{
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    if (!pBrowser) return;

    HWND    hList = GetListHWND(pBrowser);
    LRESULT count = SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0);

    for (LRESULT i = 0; i < count; i++)
    {
        char    text[256] = {};
        LVITEMA lvi       = {};
        lvi.mask          = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem         = static_cast<int>(i);
        lvi.iSubItem      = 0;
        lvi.pszText       = text;
        lvi.cchTextMax    = sizeof(text);
        SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

        void* pSound = reinterpret_cast<void*>(lvi.lParam);
        if (pSound == excludeSound) continue;

        LRESULT idx = SendMessageA(hCombo, CB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(text));
        if (idx >= 0)
            SendMessageA(hCombo, CB_SETITEMDATA, static_cast<WPARAM>(idx),
                         reinterpret_cast<LPARAM>(pSound));
    }
}

static void* FindSoundByName(void* this_ptr, const char* name)
{
    HWND    hList = GetListHWND(this_ptr);
    LRESULT count = SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0);

    for (LRESULT i = 0; i < count; i++)
    {
        char    text[256] = {};
        LVITEMA lvi       = {};
        lvi.mask          = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem         = static_cast<int>(i);
        lvi.iSubItem      = 0;
        lvi.pszText       = text;
        lvi.cchTextMax    = sizeof(text);
        SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

        if (_stricmp(text, name) == 0)
            return reinterpret_cast<void*>(lvi.lParam);
    }
    return nullptr;
}

static void RandList_Insert(void* stRes, void* elem)
{
    char* s    = static_cast<char*>(stRes);
    char* e    = static_cast<char*>(elem);
    void* last = *reinterpret_cast<void**>(s + 0x18);

    *reinterpret_cast<void**>(e + 0x14) = stRes;  // elem.parent = stRes
    *reinterpret_cast<void**>(e + 0x0C) = last;   // elem.prev   = last
    *reinterpret_cast<void**>(e + 0x10) = nullptr; // elem.next   = NULL

    if (last)
        *reinterpret_cast<void**>(static_cast<char*>(last) + 0x10) = elem; // last.next = elem
    else
        *reinterpret_cast<void**>(s + 0x14) = elem;  // list was empty: first = elem

    *reinterpret_cast<void**>(s + 0x18) = elem;   // last = elem
    *reinterpret_cast<int*>   (s + 0x1C) += 1;    // count++
}

// SeqList_Insert – appends a SequenceElement to the tail of the Sequence stRes list.
// elem layout: +0x00=child, +0x04=prev, +0x08=next, +0x0C=parent, +0x10=repeat, +0x14=timedLoop
// stRes layout: +0x1C=first, +0x20=last, +0x24=count
static void SeqList_Insert(void* stRes, void* elem)
{
    char* s    = static_cast<char*>(stRes);
    char* e    = static_cast<char*>(elem);
    void* last = *reinterpret_cast<void**>(s + 0x20);

    *reinterpret_cast<void**>(e + 0x0C) = stRes;  // elem.parent = stRes
    *reinterpret_cast<void**>(e + 0x04) = last;   // elem.prev   = last
    *reinterpret_cast<void**>(e + 0x08) = nullptr; // elem.next   = NULL

    if (last)
        *reinterpret_cast<void**>(static_cast<char*>(last) + 0x08) = elem; // last.next = elem
    else
        *reinterpret_cast<void**>(s + 0x1C) = elem;  // first = elem

    *reinterpret_cast<void**>(s + 0x20) = elem;   // last = elem
    *reinterpret_cast<int*>   (s + 0x24) += 1;    // count++
}

static void RandProps_RefreshList(HWND hDlg, RandomPropsCtx* ctx)
{
    HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
    SendMessageA(hList, LVM_DELETEALLITEMS, 0, 0);

    int totalWeight = 0;
    for (int i = 0; i < ctx->nCount; i++)
    {
        // Display weight as 0.00-100.00 percentage (internal scale is 0-10000)
        char wBuf[16] = {};
        snprintf(wBuf, sizeof(wBuf), "%.2f", ctx->children[i].nWeight / 100.0f);
        totalWeight += ctx->children[i].nWeight;

        LVITEMA lvi  = {};
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = ctx->children[i].szName;
        lvi.lParam   = static_cast<LPARAM>(i);
        // LVM_INSERTITEMA returns the actual sorted position, which differs from i
        // when the list has LVS_SORTASCENDING.  Use the real index for the subitem.
        LRESULT sortedIdx = SendMessageA(hList, LVM_INSERTITEMA, 0,
                                         reinterpret_cast<LPARAM>(&lvi));

        LVITEMA sub  = {};
        sub.mask     = LVIF_TEXT;
        sub.iItem    = static_cast<int>(sortedIdx);  // actual row, not insertion order
        sub.iSubItem = 1;
        sub.pszText  = wBuf;
        SendMessageA(hList, LVM_SETITEMA, 0, reinterpret_cast<LPARAM>(&sub));
    }

    // Engine normalises by actual weight sum; silence % shows when total < 10000.
    char silBuf[128] = {};
    float silence = (totalWeight < 10000) ? ((10000 - totalWeight) / 100.0f) : 0.0f;
    snprintf(silBuf, sizeof(silBuf), "Probability of silence: %.1f%%", silence);
    SetDlgItemTextA(hDlg, IDC_RAND_SILENCE_LBL, silBuf);
}

static void SeqProps_RefreshList(HWND hDlg, SeqPropsCtx* ctx)
{
    HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
    SendMessageA(hList, LVM_DELETEALLITEMS, 0, 0);

    for (int i = 0; i < ctx->nCount; i++)
    {
        char rBuf[16] = {};
        if (ctx->children[i].bTimedLoop)
            snprintf(rBuf, sizeof(rBuf), "%ds", ctx->children[i].nRepeat);
        else
            snprintf(rBuf, sizeof(rBuf), "%dx", ctx->children[i].nRepeat);

        LVITEMA lvi  = {};
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = ctx->children[i].szName;
        lvi.lParam   = static_cast<LPARAM>(i);
        SendMessageA(hList, LVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&lvi));

        LVITEMA sub  = {};
        sub.mask     = LVIF_TEXT;
        sub.iItem    = i;

        sub.iSubItem = 1; sub.pszText = rBuf;
        SendMessageA(hList, LVM_SETITEMA, 0, reinterpret_cast<LPARAM>(&sub));

        static char* yn[2] = { const_cast<char*>("No"), const_cast<char*>("Yes") };
        sub.iSubItem = 2;
        sub.pszText  = yn[ctx->children[i].bTimedLoop ? 1 : 0];
        SendMessageA(hList, LVM_SETITEMA, 0, reinterpret_cast<LPARAM>(&sub));
    }
}

static int s_newDlgLastType = IDC_RB_RANDOM;   // persists across dialog 142 invocations

// dialog 142 "New Sound": fills ctx->name and ctx->typeFlag (SF_TYPE_RANDOM/SEQUENCE/SWITCH).
static INT_PTR CALLBACK CreateResourceDlgProc(HWND hDlg, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        SetDlgItemTextA(hDlg, 1288, "");    // clear name edit
        // Restore last-used type selection (defaults to Random on first open)
        SendDlgItemMessageA(hDlg, IDC_RB_RANDOM,   BM_SETCHECK, (s_newDlgLastType == IDC_RB_RANDOM)   ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageA(hDlg, IDC_RB_SEQUENCE, BM_SETCHECK, (s_newDlgLastType == IDC_RB_SEQUENCE) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageA(hDlg, IDC_RB_SWITCH,   BM_SETCHECK, (s_newDlgLastType == IDC_RB_SWITCH)   ? BST_CHECKED : BST_UNCHECKED, 0);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            auto* ctx = reinterpret_cast<NewResourceCtx*>(GetWindowLongA(hDlg, GWL_USERDATA));
            if (ctx)
            {
                GetDlgItemTextA(hDlg, 1288, ctx->name, sizeof(ctx->name));
                if (ctx->name[0] == '\0')
                {
                    MessageBoxA(hDlg, "Invalid input.", "Message", MB_OK);
                    return TRUE;   // keep dialog open
                }

                if      (IsDlgButtonChecked(hDlg, IDC_RB_SEQUENCE) == BST_CHECKED)
                    { ctx->typeFlag = SF_TYPE_SEQUENCE; s_newDlgLastType = IDC_RB_SEQUENCE; }
                else if (IsDlgButtonChecked(hDlg, IDC_RB_SWITCH)   == BST_CHECKED)
                    { ctx->typeFlag = SF_TYPE_SWITCH;   s_newDlgLastType = IDC_RB_SWITCH;   }
                else
                    { ctx->typeFlag = SF_TYPE_RANDOM;   s_newDlgLastType = IDC_RB_RANDOM;   }
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

static void __cdecl SB_HandleNew(void* this_ptr)
{
    static const char* title = "Message";
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    char pkg[256] = {}, grp[256] = {};
    GetWindowTextA(hPkgCombo, pkg, sizeof(pkg));
    GetWindowTextA(hGrpCombo, grp, sizeof(grp));

    if (pkg[0] == '\0')
    {
        MessageBoxA(hParent, "Select a package first.", title,
                    MB_OK);
        return;
    }

    NewResourceCtx ctx = {};
    HINSTANCE hExe = GetModuleHandleA(NULL);
    if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_CREATE_RESOURCE), hParent,
                        CreateResourceDlgProc,
                        reinterpret_cast<LPARAM>(&ctx)) != IDOK
        || ctx.name[0] == '\0')
        return;

    DWORD typeFlag    = ctx.typeFlag;
    const char* soundName = ctx.name;

    char wavPath[MAX_PATH * 2] = {};
    if (!WriteSilentWAV(wavPath, sizeof(wavPath)))
    {
        MessageBoxA(hParent, "Failed to write temporary WAV file.", title,
                    MB_OK);
        return;
    }

    // AUDIO IMPORT (use 8.3 short path to avoid spaces confusing UE2 parser)
    char shortPath[MAX_PATH * 2] = {};
    if (GetShortPathNameA(wavPath, shortPath, sizeof(shortPath)) == 0)
        strncpy_s(shortPath, wavPath, _TRUNCATE);

    char cmd[MAX_PATH * 2 + 512];
    if (grp[0] != '\0')
        snprintf(cmd, sizeof(cmd),
                 "AUDIO IMPORT FILE=\"%s\" PACKAGE=\"%s\" NAME=\"%s\" GROUP=\"%s\"",
                 shortPath, pkg, soundName, grp);
    else
        snprintf(cmd, sizeof(cmd),
                 "AUDIO IMPORT FILE=\"%s\" PACKAGE=\"%s\" NAME=\"%s\"",
                 shortPath, pkg, soundName);
    ExecEditorCommand(cmd);

    DeleteFileA(wavPath);

    NavigateToPackageGroup(this_ptr, pkg, grp);

    void* pSound = FindSoundByName(this_ptr, soundName);
    if (!pSound)
    {
        // Import likely succeeded but the ListView doesn't expose the lParam yet
        // (StoreSoundLParam hook needs one RefreshList cycle).  Not a fatal error;
        // the user can open Sound Properties to set flags manually.
        MessageBoxA(hParent,
                    "Import succeeded but could not locate the new sound in the list.\n"
                    "Try refreshing the browser, then use 'Sound Properties' to set the type.",
                    title, MB_OK);
        return;
    }

    char*  ps     = static_cast<char*>(pSound);
    DWORD* pFlags = reinterpret_cast<DWORD*>(ps + USOUND_FLAGS_OFFSET);
    *pFlags |= typeFlag;

    // Switch sound paths may dereference TArray<int>.Data without NULL checks.
    // Keep a valid pointer here; Random/Sequence only use the count.
    *reinterpret_cast<void**>(ps + USOUND_TARRAY_DATA_OFFSET) = nullptr;
    *reinterpret_cast<int*>  (ps + USOUND_REF_COUNT_OFFSET)   = 0;
    *reinterpret_cast<int*>  (ps + USOUND_TARRAY_MAX_OFFSET)  = 0;

    if (typeFlag == SF_TYPE_SWITCH)
    {
        // Allocate a 1-int sentinel so the display code can safely read Data[0]
        // without a NULL dereference.  Value = 0 (no surface types assigned yet).
        int* sentinel = static_cast<int*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(int)));
        *reinterpret_cast<int**>(ps + USOUND_WARRAY_DATA_OFFSET) = sentinel;
        *reinterpret_cast<int*> (ps + USOUND_WARRAY_NUM_OFFSET)  = sentinel ? 1 : 0;
        *reinterpret_cast<int*> (ps + USOUND_WARRAY_MAX_OFFSET)  = sentinel ? 1 : 0;
    }
    else
    {
        *reinterpret_cast<void**>(ps + USOUND_WARRAY_DATA_OFFSET) = nullptr;
        *reinterpret_cast<int*>  (ps + USOUND_WARRAY_NUM_OFFSET)  = 0;
        *reinterpret_cast<int*>  (ps + USOUND_WARRAY_MAX_OFFSET)  = 0;
    }

    // Second refresh: NavigateToPackageGroup ran before the flag was written, so the
    // row would show as "Wave" without this call.
    RefreshList(this_ptr);
}

static INT_PTR CALLBACK RandomElemDlgProc(HWND hDlg, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        auto* ec = reinterpret_cast<RandomElemCtx*>(lParam);
        if (!ec) return TRUE;

        HWND hCombo = GetDlgItem(hDlg, IDC_RELEM_COMBO);
        PopulateComboWithSounds(ec->pBrowser, hCombo, ec->pParent);

        if (ec->pChildSound)
        {
            LRESULT n = SendMessageA(hCombo, CB_GETCOUNT, 0, 0);
            for (LRESULT i = 0; i < n; i++)
            {
                void* p = reinterpret_cast<void*>(
                    SendMessageA(hCombo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
                if (p == ec->pChildSound)
                {
                    SendMessageA(hCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                    break;
                }
            }
        }

        // Display as 0.00-100.00%; internal scale is 0-10000 (Ubisoft), default 100%.
        char wBuf[16] = {};
        float dispWeight = (ec->nWeight > 0) ? (ec->nWeight / 100.0f) : 100.0f;
        snprintf(wBuf, sizeof(wBuf), "%.2f", dispWeight);
        SetDlgItemTextA(hDlg, 1288, wBuf); // IDC_RELEM_WEIGHT = 1288
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            auto* ec = reinterpret_cast<RandomElemCtx*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (ec)
            {
                HWND    hCombo = GetDlgItem(hDlg, IDC_RELEM_COMBO);
                LRESULT sel    = SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0)
                {
                    MessageBoxA(hDlg, "Select a resource first.", "Message",
                                MB_OK);
                    return TRUE;
                }
                ec->pChildSound = reinterpret_cast<void*>(
                    SendMessageA(hCombo, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0));

                char wBuf[16] = {};
                GetDlgItemTextA(hDlg, 1288, wBuf, sizeof(wBuf));
                // User enters 0.01-100.00; multiply by 100 and round to internal 1-10000 scale.
                float pct = static_cast<float>(atof(wBuf));
                if (pct < 0.01f)   pct = 0.01f;
                if (pct > 100.0f)  pct = 100.0f;
                ec->nWeight = static_cast<int>(pct * 100.0f + 0.5f); // round to nearest
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

static INT_PTR CALLBACK RandomPropsDlgProc(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* pctx = reinterpret_cast<CompositePropsCtx*>(lParam);

        auto* ctx = static_cast<RandomPropsCtx*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RandomPropsCtx)));
        if (!ctx) return TRUE;
        ctx->pSound   = pctx->pSound;
        ctx->pBrowser = pctx->pBrowser;
        SetWindowLongA(hDlg, GWL_USERDATA, reinterpret_cast<LONG>(ctx));

        HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 140; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 50;  col.pszText = const_cast<char*>("Weight");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));

        char*  ps         = static_cast<char*>(pctx->pSound);
        void** childData  = *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET);
        int    childCount = *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET);
        int*   weightData = *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET);
        if (childData && childCount > 0)
        {
            int n = (childCount < MAX_COMPOSITE_CHILDREN) ? childCount
                                                          : MAX_COMPOSITE_CHILDREN;
            for (int i = 0; i < n; i++)
            {
                RandomChildInfo& ci = ctx->children[ctx->nCount++];
                ci.pSound  = childData[i];
                ci.nWeight = (weightData && weightData[i] >= 1) ? weightData[i] : 1;
                GetSoundName(ctx->pBrowser, ci.pSound, ci.szName, sizeof(ci.szName));
            }
        }

        RandProps_RefreshList(hDlg, ctx);
        return TRUE;
    }

    case WM_COMMAND:
    {
        auto* ctx = reinterpret_cast<RandomPropsCtx*>(
            GetWindowLongA(hDlg, GWL_USERDATA));
        if (!ctx) return FALSE;

        switch (LOWORD(wParam))
        {
        case IDC_RAND_INSERT:
        {
            if (ctx->nCount >= MAX_COMPOSITE_CHILDREN)
            {
                MessageBoxA(hDlg, "Maximum number of resources reached.", "Message",
                            MB_OK);
                return TRUE;
            }
            RandomElemCtx ec = {};
            ec.pBrowser    = ctx->pBrowser;
            ec.pParent     = ctx->pSound;
            ec.pChildSound = nullptr;
            ec.nWeight     = 10000;  // internal scale; displays as 100%

            HINSTANCE hExe = GetModuleHandleA(NULL);
            if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_RANDOM_ELEM), hDlg,
                                RandomElemDlgProc, reinterpret_cast<LPARAM>(&ec))
                == IDOK && ec.pChildSound)
            {
                for (int i = 0; i < ctx->nCount; i++)
                {
                    if (ctx->children[i].pSound == ec.pChildSound)
                    {
                        for (int j = i; j < ctx->nCount - 1; j++)
                            ctx->children[j] = ctx->children[j + 1];
                        ctx->nCount--;
                        break;
                    }
                }

                int i = ctx->nCount++;
                ctx->children[i].pSound  = ec.pChildSound;
                ctx->children[i].nWeight = ec.nWeight;
                GetSoundName(ctx->pBrowser, ec.pChildSound,
                             ctx->children[i].szName, sizeof(ctx->children[i].szName));
                RandProps_RefreshList(hDlg, ctx);
            }
            return TRUE;
        }

        case IDC_RAND_REMOVE:
        {
            HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0)
            {
                // The ListView has LVS_SORTASCENDING so visual position != array index.
                // Read back lParam to get the actual children[] index.
                LVITEMA lvi2 = {};
                lvi2.mask  = LVIF_PARAM;
                lvi2.iItem = sel;
                SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi2));
                int ai = static_cast<int>(lvi2.lParam);
                if (ai >= 0 && ai < ctx->nCount)
                {
                    for (int i = ai; i < ctx->nCount - 1; i++)
                        ctx->children[i] = ctx->children[i + 1];
                    ctx->nCount--;
                    RandProps_RefreshList(hDlg, ctx);
                }
            }
            return TRUE;
        }

        case IDC_RAND_EDIT:
        {
            HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0)
            {
                LVITEMA lvi2 = {};
                lvi2.mask  = LVIF_PARAM;
                lvi2.iItem = sel;
                SendMessageA(hList, LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&lvi2));
                int ai = static_cast<int>(lvi2.lParam);
                if (ai >= 0 && ai < ctx->nCount)
                {
                    RandomElemCtx ec = {};
                    ec.pBrowser    = ctx->pBrowser;
                    ec.pParent     = ctx->pSound;
                    ec.pChildSound = ctx->children[ai].pSound;
                    ec.nWeight     = ctx->children[ai].nWeight;

                    HINSTANCE hExe = GetModuleHandleA(NULL);
                    if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_RANDOM_ELEM), hDlg,
                                        RandomElemDlgProc, reinterpret_cast<LPARAM>(&ec))
                        == IDOK && ec.pChildSound)
                    {
                        ctx->children[ai].pSound  = ec.pChildSound;
                        ctx->children[ai].nWeight = ec.nWeight;
                        GetSoundName(ctx->pBrowser, ec.pChildSound,
                                     ctx->children[ai].szName,
                                     sizeof(ctx->children[ai].szName));
                        RandProps_RefreshList(hDlg, ctx);
                    }
                }
            }
            return TRUE;
        }

        case IDC_RAND_EQUAL_PROB:
        {
            int equalWeight = (ctx->nCount > 0) ? (10000 / ctx->nCount) : 10000;
            for (int i = 0; i < ctx->nCount; i++)
                ctx->children[i].nWeight = equalWeight;
            RandProps_RefreshList(hDlg, ctx);
            return TRUE;
        }

        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);
            int   n  = ctx->nCount;

            // Old arrays are not freed: may have been set by the engine; freeing across allocators is unsafe.
            void** childArr  = nullptr;
            int*   weightArr = nullptr;
            if (n > 0)
            {
                childArr  = static_cast<void**>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(void*)));
                weightArr = static_cast<int*>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(int)));
                if (!childArr || !weightArr)
                {
                    HeapFree(GetProcessHeap(), 0, childArr);
                    HeapFree(GetProcessHeap(), 0, weightArr);
                    HeapFree(GetProcessHeap(), 0, ctx);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                for (int i = 0; i < n; i++)
                {
                    childArr [i] = ctx->children[i].pSound;
                    weightArr[i] = ctx->children[i].nWeight;
                }
            }

            // Write TArray<USound*> at +0x6c
            *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET) = childArr;
            *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET)   = n;
            *reinterpret_cast<int*>   (ps + USOUND_TARRAY_MAX_OFFSET)  = n;
            // Write TArray<int> at +0x78 (weights)
            *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET) = weightArr;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_NUM_OFFSET)  = n;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_MAX_OFFSET)  = n;

            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
    {
        void* ctx = reinterpret_cast<void*>(GetWindowLongA(hDlg, GWL_USERDATA));
        HeapFree(GetProcessHeap(), 0, ctx);
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    }
    return FALSE;
}

static INT_PTR CALLBACK SeqElemDlgProc(HWND hDlg, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        auto* ec = reinterpret_cast<SeqElemCtx*>(lParam);
        if (!ec) return TRUE;

        HWND hCombo = GetDlgItem(hDlg, IDC_SELEM_COMBO);
        PopulateComboWithSounds(ec->pBrowser, hCombo, ec->pParent);

        if (ec->pChildSound)
        {
            LRESULT n = SendMessageA(hCombo, CB_GETCOUNT, 0, 0);
            for (LRESULT i = 0; i < n; i++)
            {
                void* p = reinterpret_cast<void*>(
                    SendMessageA(hCombo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
                if (p == ec->pChildSound)
                { SendMessageA(hCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0); break; }
            }
        }

        char rBuf[16] = {};
        snprintf(rBuf, sizeof(rBuf), "%d", ec->nRepeat);
        SetDlgItemTextA(hDlg, IDC_SELEM_REPEAT, rBuf);
        CheckDlgButton(hDlg, IDC_SELEM_TIMEDLOOP,
                       ec->bTimedLoop ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            auto* ec = reinterpret_cast<SeqElemCtx*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (ec)
            {
                HWND    hCombo = GetDlgItem(hDlg, IDC_SELEM_COMBO);
                LRESULT sel    = SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0)
                {
                    MessageBoxA(hDlg, "Select a resource first.", "Message",
                                MB_OK);
                    return TRUE;
                }
                ec->pChildSound = reinterpret_cast<void*>(
                    SendMessageA(hCombo, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0));

                ec->bTimedLoop = (IsDlgButtonChecked(hDlg, IDC_SELEM_TIMEDLOOP) == BST_CHECKED);
                char rBuf[16] = {};
                GetDlgItemTextA(hDlg, IDC_SELEM_REPEAT, rBuf, sizeof(rBuf));
                ec->nRepeat = atoi(rBuf);
                if (ec->nRepeat < 0) ec->nRepeat = 0;
                // Clamp to 13-bit value range - bit 13 and above are flags,
                // not part of the count, so anything > 0x1FFF is nonsense.
                if (ec->nRepeat > SEQ_VALUE_MASK)
                    ec->nRepeat = SEQ_VALUE_MASK;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

static INT_PTR CALLBACK SeqPropsDlgProc(HWND hDlg, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* pctx = reinterpret_cast<CompositePropsCtx*>(lParam);

        auto* ctx = static_cast<SeqPropsCtx*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SeqPropsCtx)));
        if (!ctx) return TRUE;
        ctx->pSound   = pctx->pSound;
        ctx->pBrowser = pctx->pBrowser;
        SetWindowLongA(hDlg, GWL_USERDATA, reinterpret_cast<LONG>(ctx));

        HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 115; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 38;  col.pszText = const_cast<char*>("Repeat");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));
        col.cx   = 42;  col.pszText = const_cast<char*>("Timed Loop");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 2, reinterpret_cast<LPARAM>(&col));

        // Sequence stores children in TArray<USound*> at +0x6c and repeat data in TArray<int> at +0x78.
        // +0x74 is the TArray.Max field, not a pointer.
        char*  ps         = static_cast<char*>(pctx->pSound);
        void** childData  = *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET);
        int    childCount = *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET);
        int*   repeatData = *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET);
        if (childData && childCount > 0)
        {
            int n = (childCount < MAX_COMPOSITE_CHILDREN) ? childCount : MAX_COMPOSITE_CHILDREN;
            for (int i = 0; i < n; i++)
            {
                SeqChildInfo& ci = ctx->children[ctx->nCount++];
                ci.pSound        = childData[i];
                int raw          = repeatData ? repeatData[i] : 1;
                if (raw & SEQ_TIMED_LOOP_FLAG)
                {
                    ci.bTimedLoop = TRUE;
                    ci.nRepeat    = raw & SEQ_VALUE_MASK;
                }
                else
                {
                    ci.bTimedLoop = FALSE;
                    ci.nRepeat    = (raw >= 1) ? raw : 1;
                }
                GetSoundName(ctx->pBrowser, ci.pSound, ci.szName, sizeof(ci.szName));
            }
        }

        SeqProps_RefreshList(hDlg, ctx);
        return TRUE;
    }

    case WM_COMMAND:
    {
        auto* ctx = reinterpret_cast<SeqPropsCtx*>(
            GetWindowLongA(hDlg, GWL_USERDATA));
        if (!ctx) return FALSE;

        switch (LOWORD(wParam))
        {
        case IDC_SEQ_INSERT:
        {
            if (ctx->nCount >= MAX_COMPOSITE_CHILDREN)
            {
                MessageBoxA(hDlg, "Maximum number of resources reached.", "Message",
                            MB_OK);
                return TRUE;
            }
            SeqElemCtx ec = {};
            ec.pBrowser    = ctx->pBrowser;
            ec.pParent     = ctx->pSound;
            ec.pChildSound = nullptr;
            ec.nRepeat     = 1;
            ec.bTimedLoop  = FALSE;

            HINSTANCE hExe = GetModuleHandleA(NULL);
            if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SEQ_ELEM), hDlg,
                                SeqElemDlgProc, reinterpret_cast<LPARAM>(&ec))
                == IDOK && ec.pChildSound)
            {
                int i = ctx->nCount++;
                ctx->children[i].pSound     = ec.pChildSound;
                ctx->children[i].nRepeat    = ec.nRepeat;
                ctx->children[i].bTimedLoop = ec.bTimedLoop;
                GetSoundName(ctx->pBrowser, ec.pChildSound,
                             ctx->children[i].szName, sizeof(ctx->children[i].szName));
                SeqProps_RefreshList(hDlg, ctx);
            }
            return TRUE;
        }

        case IDC_SEQ_REMOVE:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0 && sel < ctx->nCount)
            {
                for (int i = sel; i < ctx->nCount - 1; i++)
                    ctx->children[i] = ctx->children[i + 1];
                ctx->nCount--;
                SeqProps_RefreshList(hDlg, ctx);
            }
            return TRUE;
        }

        case IDC_SEQ_EDIT:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0 && sel < ctx->nCount)
            {
                SeqElemCtx ec = {};
                ec.pBrowser    = ctx->pBrowser;
                ec.pParent     = ctx->pSound;
                ec.pChildSound = ctx->children[sel].pSound;
                ec.nRepeat     = ctx->children[sel].nRepeat;
                ec.bTimedLoop  = ctx->children[sel].bTimedLoop;

                HINSTANCE hExe = GetModuleHandleA(NULL);
                if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SEQ_ELEM), hDlg,
                                    SeqElemDlgProc, reinterpret_cast<LPARAM>(&ec))
                    == IDOK && ec.pChildSound)
                {
                    ctx->children[sel].pSound     = ec.pChildSound;
                    ctx->children[sel].nRepeat    = ec.nRepeat;
                    ctx->children[sel].bTimedLoop = ec.bTimedLoop;
                    GetSoundName(ctx->pBrowser, ec.pChildSound,
                                 ctx->children[sel].szName,
                                 sizeof(ctx->children[sel].szName));
                    SeqProps_RefreshList(hDlg, ctx);
                }
            }
            return TRUE;
        }

        case IDC_SEQ_MOVEUP:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel > 0)
            {
                SeqChildInfo tmp       = ctx->children[sel - 1];
                ctx->children[sel - 1] = ctx->children[sel];
                ctx->children[sel]     = tmp;
                SeqProps_RefreshList(hDlg, ctx);
                // Re-select moved item
                LVITEMA lvi = {};
                lvi.mask      = LVIF_STATE;
                lvi.iItem     = sel - 1;
                lvi.state     = LVIS_SELECTED | LVIS_FOCUSED;
                lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageA(hList, LVM_SETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));
            }
            return TRUE;
        }

        case IDC_SEQ_MOVEDOWN:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0 && sel < ctx->nCount - 1)
            {
                SeqChildInfo tmp       = ctx->children[sel + 1];
                ctx->children[sel + 1] = ctx->children[sel];
                ctx->children[sel]     = tmp;
                SeqProps_RefreshList(hDlg, ctx);
                LVITEMA lvi = {};
                lvi.mask      = LVIF_STATE;
                lvi.iItem     = sel + 1;
                lvi.state     = LVIS_SELECTED | LVIS_FOCUSED;
                lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageA(hList, LVM_SETITEMA, 0, reinterpret_cast<LPARAM>(&lvi));
            }
            return TRUE;
        }

        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);
            int   n  = ctx->nCount;

            // Old arrays not freed: freeing across allocators is unsafe (editor-session leak).
            void** childArr  = nullptr;
            int*   repeatArr = nullptr;
            if (n > 0)
            {
                childArr  = static_cast<void**>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(void*)));
                repeatArr = static_cast<int*>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(int)));
                if (!childArr || !repeatArr)
                {
                    HeapFree(GetProcessHeap(), 0, childArr);
                    HeapFree(GetProcessHeap(), 0, repeatArr);
                    HeapFree(GetProcessHeap(), 0, ctx);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                for (int i = 0; i < n; i++)
                {
                    childArr[i] = ctx->children[i].pSound;
                    if (ctx->children[i].bTimedLoop)
                        // bit 13 = timed-loop flag; lower 13 bits = duration in seconds
                        repeatArr[i] = SEQ_TIMED_LOOP_FLAG | (ctx->children[i].nRepeat & SEQ_VALUE_MASK);
                    else
                        repeatArr[i] = (ctx->children[i].nRepeat >= 1)
                                           ? (ctx->children[i].nRepeat & SEQ_VALUE_MASK) : 1;
                }
            }

            // Write TArray<USound*> at +0x6c
            *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET) = childArr;
            *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET)   = n;
            *reinterpret_cast<int*>   (ps + USOUND_TARRAY_MAX_OFFSET)  = n;
            // Write TArray<int> at +0x78 (repeat counts, parallel to Random weights)
            *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET) = repeatArr;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_NUM_OFFSET)  = n;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_MAX_OFFSET)  = n;

            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
    {
        void* ctx = reinterpret_cast<void*>(GetWindowLongA(hDlg, GWL_USERDATA));
        HeapFree(GetProcessHeap(), 0, ctx);
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    }
    return FALSE;
}

static const char* kSurfaceTypeNames[SW_NUM_SURFACE_TYPES] = {
    "SURFACE_Generic",      // 0  – default / wildcard (no surface matched)
    "SURFACE_Carpet",       // 1
    "SURFACE_RoofCanevas",  // 2
    "SURFACE_SnowPowder",   // 3
    "SURFACE_Grass",        // 4
    "SURFACE_ConcreteHard", // 5
    "SURFACE_ConcreteDirt", // 6
    "SURFACE_WoodHard",     // 7
    "SURFACE_WoodBoomy",    // 8
    "SURFACE_RoofTile",     // 9
    "SURFACE_Conveyor",     // 10
    "SURFACE_Scaffolding",  // 11
    "SURFACE_WaterPuddle",  // 12
    "SURFACE_Snow",         // 13
    "SURFACE_MetalHard",    // 14
    "SURFACE_DeepWater",    // 15
    "SURFACE_MetalReverb",  // 16
    "SURFACE_MetalSheet",   // 17
    "SURFACE_MediumWater",  // 18
    "SURFACE_BreakingGlass",// 19
    "SURFACE_Gravel",       // 20
    "SURFACE_Sand",         // 21
    "SURFACE_FenceMetal",   // 22
    "SURFACE_FenceRope",    // 23
    "SURFACE_FenceVine",    // 24
    "SURFACE_FakeCeiling",  // 25
};

static INT_PTR CALLBACK SwitchElemDlgProc(HWND hDlg, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        auto* ec = reinterpret_cast<SwitchElemCtx*>(lParam);
        if (!ec) return TRUE;

        HWND hSoundCombo = GetDlgItem(hDlg, IDC_SWELEM_COMBO);
        PopulateComboWithSounds(ec->pBrowser, hSoundCombo, ec->pParent);

        if (ec->pChildSound)
        {
            LRESULT n = SendMessageA(hSoundCombo, CB_GETCOUNT, 0, 0);
            for (LRESULT i = 0; i < n; i++)
            {
                void* p = reinterpret_cast<void*>(
                    SendMessageA(hSoundCombo, CB_GETITEMDATA,
                                 static_cast<WPARAM>(i), 0));
                if (p == ec->pChildSound)
                {
                    SendMessageA(hSoundCombo, CB_SETCURSEL,
                                 static_cast<WPARAM>(i), 0);
                    break;
                }
            }
        }

        HWND hSurfCombo = GetDlgItem(hDlg, IDC_SWELEM_SURFACE);
        for (int i = 0; i < SW_NUM_SURFACE_TYPES; i++)
            SendMessageA(hSurfCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kSurfaceTypeNames[i]));

        int selSurf = (ec->nSurfaceType >= 0 &&
                       ec->nSurfaceType < SW_NUM_SURFACE_TYPES)
                      ? ec->nSurfaceType : 0;
        SendMessageA(hSurfCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selSurf), 0);

        // Fix dropdown height, some comboboxes have only edit-field height set
        {
            RECT rc;
            GetWindowRect(hSurfCombo, &rc);
            int editH = rc.bottom - rc.top;
            LRESULT itemH = SendMessageA(hSurfCombo, CB_GETITEMHEIGHT, 0, 0);
            if (itemH <= 0) itemH = 15;
            SetWindowPos(hSurfCombo, NULL,
                         0, 0,
                         rc.right - rc.left,
                         editH + static_cast<int>(itemH) * 12 + 4,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            auto* ec = reinterpret_cast<SwitchElemCtx*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (ec)
            {
                HWND    hSoundCombo = GetDlgItem(hDlg, IDC_SWELEM_COMBO);
                LRESULT sel = SendMessageA(hSoundCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0)
                {
                    MessageBoxA(hDlg, "Select a resource first.",
                                "Message", MB_OK);
                    return TRUE;
                }
                ec->pChildSound = reinterpret_cast<void*>(
                    SendMessageA(hSoundCombo, CB_GETITEMDATA,
                                 static_cast<WPARAM>(sel), 0));

                HWND    hSurfCombo = GetDlgItem(hDlg, IDC_SWELEM_SURFACE);
                LRESULT surfSel = SendMessageA(hSurfCombo, CB_GETCURSEL, 0, 0);
                ec->nSurfaceType = (surfSel >= 0 &&
                                    surfSel < SW_NUM_SURFACE_TYPES)
                                   ? static_cast<int>(surfSel) : 0;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

static void SwitchProps_RefreshList(HWND hDlg, SwitchPropsCtx* ctx)
{
    HWND hList = GetDlgItem(hDlg, IDC_SW_LIST);
    // Force LVS_REPORT so sub-item columns are visible regardless of the style
    // set in the dialog resource (user-created dialogs may default to LVS_ICON).
    LONG curStyle = GetWindowLongA(hList, GWL_STYLE);
    SetWindowLongA(hList, GWL_STYLE, (curStyle & ~0x0003L) | LVS_REPORT);

    SendMessageA(hList, LVM_DELETEALLITEMS, 0, 0);

    for (int i = 0; i < ctx->nCount; i++)
    {
        const SwitchChildInfo& ci = ctx->children[i];
        char surfBuf[64] = {};
        if (ci.nSurfaceType >= 0 && ci.nSurfaceType < SW_NUM_SURFACE_TYPES)
            strncpy_s(surfBuf, sizeof(surfBuf),
                      kSurfaceTypeNames[ci.nSurfaceType], _TRUNCATE);
        else
            strncpy_s(surfBuf, sizeof(surfBuf), "?", _TRUNCATE);

        LVITEMA lvi = {};
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = const_cast<char*>(ci.szName);
        lvi.lParam   = static_cast<LPARAM>(i);
        LRESULT insertedIdx = SendMessageA(hList, LVM_INSERTITEMA, 0,
                                           reinterpret_cast<LPARAM>(&lvi));

        LVITEMA sub = {};
        sub.iSubItem = 1;
        sub.pszText  = surfBuf;
        SendMessageA(hList, LVM_SETITEMTEXTA,
                     static_cast<WPARAM>(insertedIdx),
                     reinterpret_cast<LPARAM>(&sub));
    }
}

static INT_PTR CALLBACK SwitchPropsDlgProc(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* pctx = reinterpret_cast<CompositePropsCtx*>(lParam);

        auto* ctx = static_cast<SwitchPropsCtx*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SwitchPropsCtx)));
        if (!ctx) return TRUE;
        ctx->pSound   = pctx->pSound;
        ctx->pBrowser = pctx->pBrowser;
        SetWindowLongA(hDlg, GWL_USERDATA, reinterpret_cast<LONG>(ctx));

        HWND hList = GetDlgItem(hDlg, IDC_SW_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 140; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 130; col.pszText = const_cast<char*>("Surface Type");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));

        char*  ps         = static_cast<char*>(pctx->pSound);
        void** childData  = *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET);
        int    childCount = *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET);
        int*   surfData   = *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET);
        if (childData && childCount > 0)
        {
            int n = (childCount < SW_NUM_SURFACE_TYPES) ? childCount
                                                        : SW_NUM_SURFACE_TYPES;
            for (int i = 0; i < n; i++)
            {
                SwitchChildInfo& ci = ctx->children[ctx->nCount++];
                ci.pSound       = childData[i];
                ci.nSurfaceType = (surfData && surfData[i] >= 0 &&
                                   surfData[i] < SW_NUM_SURFACE_TYPES)
                                  ? surfData[i] : 0;
                GetSoundName(ctx->pBrowser, ci.pSound, ci.szName, sizeof(ci.szName));
            }
        }

        SwitchProps_RefreshList(hDlg, ctx);
        return TRUE;
    }

    case WM_COMMAND:
    {
        auto* ctx = reinterpret_cast<SwitchPropsCtx*>(
            GetWindowLongA(hDlg, GWL_USERDATA));
        if (!ctx) return FALSE;

        switch (LOWORD(wParam))
        {
        case IDC_SW_INSERT:
        {
            if (ctx->nCount >= SW_NUM_SURFACE_TYPES)
            {
                MessageBoxA(hDlg,
                    "All surface types already have a sound assigned.",
                    "Message", MB_OK);
                return TRUE;
            }
            SwitchElemCtx ec = {};
            ec.pBrowser     = ctx->pBrowser;
            ec.pParent      = ctx->pSound;
            ec.pChildSound  = nullptr;
            ec.nSurfaceType = 0;  // default to SURFACE_Generic

            HINSTANCE hExe = GetModuleHandleA(NULL);
            if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SWITCH_ELEM), hDlg,
                                SwitchElemDlgProc, reinterpret_cast<LPARAM>(&ec))
                == IDOK && ec.pChildSound)
            {
                for (int i = 0; i < ctx->nCount; i++)
                {
                    if (ctx->children[i].nSurfaceType == ec.nSurfaceType)
                    {
                        ctx->children[i].pSound = ec.pChildSound;
                        GetSoundName(ctx->pBrowser, ec.pChildSound,
                                     ctx->children[i].szName,
                                     sizeof(ctx->children[i].szName));
                        SwitchProps_RefreshList(hDlg, ctx);
                        return TRUE;
                    }
                }
                int i = ctx->nCount++;
                ctx->children[i].pSound       = ec.pChildSound;
                ctx->children[i].nSurfaceType = ec.nSurfaceType;
                GetSoundName(ctx->pBrowser, ec.pChildSound,
                             ctx->children[i].szName,
                             sizeof(ctx->children[i].szName));
                SwitchProps_RefreshList(hDlg, ctx);
            }
            return TRUE;
        }

        case IDC_SW_REMOVE:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SW_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM,
                             static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0)
            {
                LVITEMA lvi2 = {};
                lvi2.mask  = LVIF_PARAM;
                lvi2.iItem = sel;
                SendMessageA(hList, LVM_GETITEMA, 0,
                             reinterpret_cast<LPARAM>(&lvi2));
                int ai = static_cast<int>(lvi2.lParam);
                if (ai >= 0 && ai < ctx->nCount)
                {
                    for (int i = ai; i < ctx->nCount - 1; i++)
                        ctx->children[i] = ctx->children[i + 1];
                    ctx->nCount--;
                    SwitchProps_RefreshList(hDlg, ctx);
                }
            }
            return TRUE;
        }

        case IDC_SW_EDIT:
        {
            HWND hList = GetDlgItem(hDlg, IDC_SW_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM,
                             static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0)
            {
                LVITEMA lvi2 = {};
                lvi2.mask  = LVIF_PARAM;
                lvi2.iItem = sel;
                SendMessageA(hList, LVM_GETITEMA, 0,
                             reinterpret_cast<LPARAM>(&lvi2));
                int ai = static_cast<int>(lvi2.lParam);
                if (ai >= 0 && ai < ctx->nCount)
                {
                    SwitchElemCtx ec = {};
                    ec.pBrowser     = ctx->pBrowser;
                    ec.pParent      = ctx->pSound;
                    ec.pChildSound  = ctx->children[ai].pSound;
                    ec.nSurfaceType = ctx->children[ai].nSurfaceType;

                    HINSTANCE hExe = GetModuleHandleA(NULL);
                    if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SWITCH_ELEM),
                                        hDlg, SwitchElemDlgProc,
                                        reinterpret_cast<LPARAM>(&ec))
                        == IDOK && ec.pChildSound)
                    {
                        ctx->children[ai].pSound       = ec.pChildSound;
                        ctx->children[ai].nSurfaceType = ec.nSurfaceType;
                        GetSoundName(ctx->pBrowser, ec.pChildSound,
                                     ctx->children[ai].szName,
                                     sizeof(ctx->children[ai].szName));
                        SwitchProps_RefreshList(hDlg, ctx);
                    }
                }
            }
            return TRUE;
        }

        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);
            int   n  = ctx->nCount;

            void** childArr = nullptr;
            int*   surfArr  = nullptr;
            if (n > 0)
            {
                childArr = static_cast<void**>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(void*)));
                surfArr  = static_cast<int*>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                              static_cast<SIZE_T>(n) * sizeof(int)));
                if (!childArr || !surfArr)
                {
                    HeapFree(GetProcessHeap(), 0, childArr);
                    HeapFree(GetProcessHeap(), 0, surfArr);
                    HeapFree(GetProcessHeap(), 0, ctx);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                for (int i = 0; i < n; i++)
                {
                    childArr[i] = ctx->children[i].pSound;
                    surfArr [i] = ctx->children[i].nSurfaceType;
                }
            }

            // Write TArray<USound*> at +0x6c
            *reinterpret_cast<void***>(ps + USOUND_TARRAY_DATA_OFFSET) = childArr;
            *reinterpret_cast<int*>   (ps + USOUND_REF_COUNT_OFFSET)   = n;
            *reinterpret_cast<int*>   (ps + USOUND_TARRAY_MAX_OFFSET)  = n;
            // Write TArray<int> at +0x78 (ESurfaceType values)
            *reinterpret_cast<int**>  (ps + USOUND_WARRAY_DATA_OFFSET) = surfArr;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_NUM_OFFSET)  = n;
            *reinterpret_cast<int*>   (ps + USOUND_WARRAY_MAX_OFFSET)  = n;

            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
    {
        void* ctx = reinterpret_cast<void*>(GetWindowLongA(hDlg, GWL_USERDATA));
        HeapFree(GetProcessHeap(), 0, ctx);
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    }
    return FALSE;
}

static void GetSoundFName(void* pSound, char* outBuf, int maxLen)
{
    // FNameEntry* = ((int*)(*(int*)GNAMES_DATA))[FName.Index]; string at FNameEntry+0x0C
    int   fnIdx  = *reinterpret_cast<int*>(static_cast<char*>(pSound) + UOBJ_FNAME_OFFSET);
    int   gnBase = *reinterpret_cast<int*>(GNAMES_DATA);   // read the GNames array pointer
    int*  gData  = reinterpret_cast<int*>(gnBase);          // treat as int[] of FNameEntry ptrs
    const char* str = reinterpret_cast<const char*>(gData[fnIdx]) + FNAME_ENTRY_STR_OFFSET;
    strncpy_s(outBuf, static_cast<size_t>(maxLen), str, _TRUNCATE);
}

static void GetMuxPath(void* pSound, char* outPath, int maxLen)
{
    char fname[64] = {};
    GetSoundFName(pSound, fname, sizeof(fname));

    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* sep = strrchr(exeDir, '\\');
    if (sep) *(sep + 1) = '\0';

    snprintf(outPath, static_cast<size_t>(maxLen),
             "%s..\\Packages\\Sounds\\%s.mux", exeDir, fname);
}

static INT_PTR CALLBACK SurroundPickerDlgProc(HWND hDlg, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        auto* pc = reinterpret_cast<SurroundPickerCtx*>(lParam);
        if (!pc) return TRUE;

        // Retitle and strip Random-specific controls from IDD_RANDOM_ELEM
        SetWindowTextA(hDlg, "Surround Sound (4.0/5.1)");

        // Hide weight edit field (control 1288)
        ShowWindow(GetDlgItem(hDlg, 1288), SW_HIDE);

        // Hide "Play Probability" static label - find it by text since we don't own its ID
        for (HWND h = GetWindow(hDlg, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        {
            char txt[64] = {};
            GetWindowTextA(h, txt, sizeof(txt));
            if (strstr(txt, "Play") || strstr(txt, "Probability"))
            {
                ShowWindow(h, SW_HIDE);
                break;
            }
        }

        HWND hCombo = GetDlgItem(hDlg, IDC_RELEM_COMBO);
        PopulateComboWithSounds(pc->pBrowser, hCombo, pc->pParent);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            auto* pc = reinterpret_cast<SurroundPickerCtx*>(
                GetWindowLongA(hDlg, GWL_USERDATA));
            if (pc)
            {
                HWND    hCombo = GetDlgItem(hDlg, IDC_RELEM_COMBO);
                LRESULT sel    = SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0)
                {
                    MessageBoxA(hDlg, "Select a sound first.", "Message", MB_OK);
                    return TRUE;
                }
                pc->pSelected = reinterpret_cast<void*>(
                    SendMessageA(hCombo, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0));
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
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

// ---------------------------------------------------------------------------
// Surround Properties dialog (IDD_SURROUND = 289)
//
// Data model (from UpdateStreamFile @ 0x10f784d8 / 0x10f788f9 disassembly):
//   TArray<int> at USound+0x78: Data[0] = channel bitmask (bits 0-5 = FL/FR/BL/BR/Ctr/LFE)
//   Channel sound names: .mux file, section [Channels], keys FL/FR/BL/BR/Center/LFE
//   TArray<USound*> at USound+0x6C: NOT used for Surround - do not touch.
//
// Edit fields 1419-1424: user types (or Browse fills) the bare sound FName per channel.
// A non-empty field sets the corresponding bitmask bit and writes to the .mux.
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SurroundPropsDlgProc(HWND hDlg, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    static const int kEditCtl[SURROUND_NUM_CHANNELS] = {
        IDC_SURR_EDIT_FL,   IDC_SURR_EDIT_FR,   IDC_SURR_EDIT_BL,
        IDC_SURR_EDIT_BR,   IDC_SURR_EDIT_CTR,  IDC_SURR_EDIT_LFE
    };
    static const int kBrowseCtl[SURROUND_NUM_CHANNELS] = {
        IDC_SURR_BROWSE_FL, IDC_SURR_BROWSE_FR, IDC_SURR_BROWSE_BL,
        IDC_SURR_BROWSE_BR, IDC_SURR_BROWSE_CTR,IDC_SURR_BROWSE_LFE
    };

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* pctx = reinterpret_cast<CompositePropsCtx*>(lParam);

        auto* ctx = static_cast<SurroundPropsCtx*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SurroundPropsCtx)));
        if (!ctx) return TRUE;
        ctx->pSound   = pctx->pSound;
        ctx->pBrowser = pctx->pBrowser;
        SetWindowLongA(hDlg, GWL_USERDATA, reinterpret_cast<LONG>(ctx));

        // Read channel bitmask from TArray<int>.Data[0] at USound+0x78
        char* ps       = static_cast<char*>(pctx->pSound);
        int*  maskData = *reinterpret_cast<int**>(ps + USOUND_WARRAY_DATA_OFFSET);
        int   maskNum  = *reinterpret_cast<int*> (ps + USOUND_WARRAY_NUM_OFFSET);
        ctx->channelMask = (maskData && maskNum > 0)
                           ? static_cast<DWORD>(maskData[0]) : 0u;

        // Build .mux path and read channel names
        GetMuxPath(pctx->pSound, ctx->szMuxPath, MAX_PATH);
        for (int i = 0; i < SURROUND_NUM_CHANNELS; i++)
        {
            GetPrivateProfileStringA("Channels", kSurroundKeys[i], "",
                                     ctx->szNames[i], sizeof(ctx->szNames[i]),
                                     ctx->szMuxPath);
            SetDlgItemTextA(hDlg, kEditCtl[i], ctx->szNames[i]);
        }
        return TRUE;
    }

    case WM_COMMAND:
    {
        auto* ctx = reinterpret_cast<SurroundPropsCtx*>(
            GetWindowLongA(hDlg, GWL_USERDATA));
        if (!ctx) return FALSE;

        // Browse buttons: pick a loaded USound, translate its FName > name string
        for (int i = 0; i < SURROUND_NUM_CHANNELS; i++)
        {
            if (LOWORD(wParam) == static_cast<WORD>(kBrowseCtl[i]))
            {
                SurroundPickerCtx pc = { ctx->pBrowser, ctx->pSound, nullptr };
                HINSTANCE hExe = GetModuleHandleA(NULL);
                if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_RANDOM_ELEM), hDlg,
                                    SurroundPickerDlgProc,
                                    reinterpret_cast<LPARAM>(&pc)) == IDOK
                    && pc.pSelected)
                {
                    GetSoundFName(pc.pSelected,
                                  ctx->szNames[i], sizeof(ctx->szNames[i]));
                    SetDlgItemTextA(hDlg, kEditCtl[i], ctx->szNames[i]);
                }
                return TRUE;
            }
        }

        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);

            // Collect current text from edit fields, build new bitmask
            DWORD newMask = 0;
            for (int i = 0; i < SURROUND_NUM_CHANNELS; i++)
            {
                GetDlgItemTextA(hDlg, kEditCtl[i],
                                ctx->szNames[i], sizeof(ctx->szNames[i]));
                if (ctx->szNames[i][0] != '\0')
                    newMask |= kSurroundBits[i];
            }

            // Write channel names to .mux file
            for (int i = 0; i < SURROUND_NUM_CHANNELS; i++)
            {
                WritePrivateProfileStringA("Channels", kSurroundKeys[i],
                                           ctx->szNames[i], ctx->szMuxPath);
            }

            // Write channel bitmask to TArray<int>.Data[0] at USound+0x78
            int* maskArr = static_cast<int*>(
                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(int)));
            if (maskArr)
            {
                maskArr[0] = static_cast<int>(newMask);
                *reinterpret_cast<int**>(ps + USOUND_WARRAY_DATA_OFFSET) = maskArr;
                *reinterpret_cast<int*> (ps + USOUND_WARRAY_NUM_OFFSET)  = 1;
                *reinterpret_cast<int*> (ps + USOUND_WARRAY_MAX_OFFSET)  = 1;
            }

            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            HeapFree(GetProcessHeap(), 0, ctx);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
    {
        void* ctx = reinterpret_cast<void*>(GetWindowLongA(hDlg, GWL_USERDATA));
        HeapFree(GetProcessHeap(), 0, ctx);
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    }
    return FALSE;
}

static void __cdecl SB_HandleSurroundProps(void* this_ptr)
{
    HWND  hParent = GetParentHWND(this_ptr);
    void* pSound  = GetSelectedSound(this_ptr);

    if (!pSound)
        return;

    DWORD flags = *reinterpret_cast<DWORD*>(
        static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
    if (!(flags & SF_TYPE_SURROUND))
        return;

    CompositePropsCtx ctx = { pSound, this_ptr };
    HINSTANCE hExe = GetModuleHandleA(NULL);
    INT_PTR res = DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SURROUND), hParent,
                    SurroundPropsDlgProc, reinterpret_cast<LPARAM>(&ctx));
    if (res == IDOK)
        RefreshList(this_ptr);
}

static void __cdecl SB_HandleRandomProps(void* this_ptr)
{
    HWND  hParent = GetParentHWND(this_ptr);
    void* pSound  = GetSelectedSound(this_ptr);

    if (!pSound)
    {
        return;
    }

    DWORD flags = *reinterpret_cast<DWORD*>(
        static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
    if (!(flags & SF_TYPE_RANDOM))
        return;

    CompositePropsCtx ctx = { pSound, this_ptr };
    HINSTANCE hExe = GetModuleHandleA(NULL);
    INT_PTR res = DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_RANDOM), hParent,
                    RandomPropsDlgProc, reinterpret_cast<LPARAM>(&ctx));
    if (res == IDOK)
        RefreshList(this_ptr);
}

static void __cdecl SB_HandleSwitchProps(void* this_ptr)
{
    HWND  hParent = GetParentHWND(this_ptr);
    void* pSound  = GetSelectedSound(this_ptr);

    if (!pSound)
    {
        return;
    }

    DWORD flags = *reinterpret_cast<DWORD*>(
        static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
    if (!(flags & SF_TYPE_SWITCH))
        return;

    CompositePropsCtx ctx = { pSound, this_ptr };
    HINSTANCE hExe = GetModuleHandleA(NULL);
    INT_PTR res = DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SWITCH), hParent,
                    SwitchPropsDlgProc, reinterpret_cast<LPARAM>(&ctx));

    if (res == IDOK)
        RefreshList(this_ptr);
}

static void __cdecl SB_HandleSeqProps(void* this_ptr)
{
    HWND  hParent = GetParentHWND(this_ptr);
    void* pSound  = GetSelectedSound(this_ptr);

    if (!pSound)
    {
        return;
    }

    DWORD flags = *reinterpret_cast<DWORD*>(
        static_cast<char*>(pSound) + USOUND_FLAGS_OFFSET);
    if (!(flags & SF_TYPE_SEQUENCE))
        return;

    CompositePropsCtx ctx = { pSound, this_ptr };
    HINSTANCE hExe = GetModuleHandleA(NULL);
    INT_PTR res = DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_SEQUENCE), hParent,
                    SeqPropsDlgProc, reinterpret_cast<LPARAM>(&ctx));
    if (res == IDOK)
        RefreshList(this_ptr);
}

void SoundBrowser::Initialize()
{
    // Disable SAVEPACKAGE command extension guard to allow .uax files
    static const uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x11011EC7, nop6, sizeof(nop6));

    // Bypass UObject::SavePackage early-return for non-standard extensions
    static const uint8_t jmpSave[] = { 0xE9, 0x11, 0x00, 0x00, 0x00 };
    MemoryWriter::WriteBytes(0x10fb2757, jmpSave, sizeof(jmpSave));

    // Show Stream resources in Sound Browser
    static const uint8_t jmpSkip0x4[] = { 0xEB, 0x22 };
    MemoryWriter::WriteBytes(0x10e7dc74, jmpSkip0x4, sizeof(jmpSkip0x4));

    // Show Xbox HD Stream resources in Sound Browser
    static const uint8_t nop2[] = { 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10e7dc9b, nop2, sizeof(nop2));

    // Prevent SavePackage crash in AUDIO EXPORTXBOX
    static const uint8_t nop5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10F7188E, nop5, sizeof(nop5));

    INSTALL_HOOKS;
}
