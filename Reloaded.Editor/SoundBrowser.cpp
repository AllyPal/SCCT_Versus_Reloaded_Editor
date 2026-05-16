// SoundBrowser.cpp
// Restores full Sound Browser functionality in ChaosTheory_Editor.exe.
//
// Hooks installed:
//   1. StoreSoundLParam    @ 0x10E7DD57 – stores USound* as ListView lParam at insert time
//   2. ShowSoundProperties @ 0x10E7E41E – shows Properties dialog when command 0x9F fires
//   3. SoundBrowserCommands@ 0x10E7E690 – handles Save(0x9D29)/Export(0x9D2A)/Import(0x9D2B)/BuildUAS(0x9DA8)
//
// Memory patches:
//   - NOP 6 bytes @ 0x11011EC7: removes SAVEPACKAGE command handler extension guard
//     (FUN_11011bf0) so it no longer jumps past the save call for .uax / .ukx.
//   - JMP patch 5 bytes @ 0x10fb2757: removes UObject::SavePackage early-return guard
//     (FUN_10fb2610) so it actually serializes instead of silently returning 1.
//   - NOP 5 bytes @ 0x10F7188E: suppresses Phase 1 SavePackage crash in AUDIO EXPORTXBOX
//     so execution reaches Phase 2 (UpdateStreamFile / OGG packer at 0x10F71E33).

#include "pch.h"
#include "SoundBrowser.h"
#include "Hooks.h"
#include "MemoryWriter.h"
#include <commctrl.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

INIT_HOOKS;

// USound field offsets 
// Confirmed from Ghidra decompile of FUN_10e7db10 (AddListSoundsString):
//   param_2[0x18] = flags DWORD     > offset 0x18*4 = 0x60
//   param_2[0x19] = volume linear   > offset 0x19*4 = 0x64  (shown as dB in browser)
//   param_2[0x1a] = override radius > offset 0x1a*4 = 0x68  (shown if > 0.0)
#define USOUND_FLAGS_OFFSET   0x60
#define USOUND_VOLUME_OFFSET  0x64
#define USOUND_RADIUS_OFFSET  0x68

// USound flag bit masks (confirmed from decompile)
// NOTE: SF_STREAM (0x100) is the Random composite type bit — Ubisoft reused it.
//   The "Stream" checkbox in Sound Properties must use SF_UAS_STREAM (0x4) instead.
//   SF_UNK_0200 (0x200): purpose unknown. The PC editor DLL has no code that reads
//   USound+0x60 with a 0x200 TEST — confirmed dead via exhaustive Ghidra search.
//   Not exposed in the Sound Properties dialog; value preserved in the flags DWORD.
#define SF_2D           0x00000002u
#define SF_LOOP         0x00000010u
#define SF_STREAM       0x00000100u   // = SF_TYPE_RANDOM; NOT the stream flag
#define SF_UNK_0200     0x00000200u   // Originally labelled "Xbox HD Stream" in the stock editor dialog (control 1338).
                                      // Exhaustive Ghidra search found zero TEST instructions reading USound+0x60
                                      // with a 0x200 mask — the flag is dead in the PC editor DLL.  The real
                                      // Xbox HD Stream flag turned out to be SF_XBOXHD_STREAM (0x4000).
                                      // 0x200 purpose remains unknown; not wired to any dialog control.
#define SF_OVR_VOLUME   0x00000400u   // confirmed: flags & 0x400 enables volume column

// Sound Properties dialog / control IDs 
#define IDD_SOUND_PROPS      144   // dialog resource in ChaosTheory_Editor.exe
#define IDC_CHK_STREAM      1334
#define IDC_CHK_LOOP        1335   // resource label "2D"; sets SF_LOOP (0x10) - swapped per browser
#define IDC_CHK_2D          1336   // resource label "Loop"; sets SF_2D  (0x02) - swapped per browser
#define IDC_CHK_OVR_RADIUS  1337
#define IDC_EDIT_RADIUS     1288
#define IDC_CHK_XBOXHD      1338   // "Xbox HD Stream"; sets SF_XBOXHD_STREAM (0x4000)
#define IDC_CHK_OVR_VOLUME  1339
#define IDC_EDIT_VOLUME     1289
#define IDC_CHK_SURROUND    1341   // "Surround"; sets SF_TYPE_SURROUND (0x2000)

// Import Sound dialog (engine resource ID 156) 
// Shown after the user picks a file - matches the engine's own import UI.
//   1068 (STATIC) : File path display
//   1067 (EDIT)   : Package name
//   1066 (EDIT)   : Group name
//   1065 (EDIT)   : Sound name
//   1   (BUTTON)  : OK
//   3   (BUTTON)  : OK All
//   4   (BUTTON)  : Skip
//   2   (BUTTON)  : Cancel
#define IDD_IMPORT_SOUND     156
#define IDC_IMPORT_FILE      1068
#define IDC_IMPORT_PACKAGE   1067
#define IDC_IMPORT_GROUP     1066
#define IDC_IMPORT_NAME      1065
#define IDB_IMPORT_OKALL     3
#define IDB_IMPORT_SKIP      4

// Rename dialog (engine resource IDDIALOG_RENAME = 19805) 
// Same dialog used by Texture and StaticMesh browsers.  Package / Group / Name
// labels and edits are built into the dialog template; only OK + Cancel buttons.
//   1066 (EDIT) : IDEC_NEWPACKAGE  - new package name
//   1067 (EDIT) : IDEC_NEWGROUP    - new group name
//   1065 (EDIT) : IDEC_NAME        - new object name
//   1   (BUTTON): IDOK             - OK
//   2   (BUTTON): IDCANCEL         - Cancel
#define IDDIALOG_RENAME      19805
#define IDEC_NEWPACKAGE      1066
#define IDEC_NEWGROUP        1067
#define IDEC_NAME            1065

//  Sound Browser menu command IDs (re-added via Resource Hacker) 
#define CMD_SAVE    0x9D29   // 40233  File > Save
#define CMD_EXPORT  0x9D2A   // 40234  File > Export
#define CMD_IMPORT  0x9D2B   // 40235  File > Import
#define CMD_DELETE       0x7647   // 30279  Right-click > Delete
#define CMD_RENAME       0x77A9   // 30633  Right-click > Rename...

// Composite sound creation and editing (menu items added via Resource Hacker)
// IDs confirmed from final Resource Hacker menu layout:
//   40246 File > New...         (unified: replaces separate New Random/Sequence/Switch)
//   40249 Edit > Random Properties...
//   40250 Edit > Copy Shortcut  (built-in, not handled here)
//   40251 Edit > Switch Properties...
//   40252 Edit > Sequence Properties...
#define CMD_NEW          0x9D36u  // 40246  File > New...  (type chosen inside dialog 142)
#define CMD_RANDOM_PROPS 0x9D39u  // 40249  Edit > Random Properties...
#define CMD_SWITCH_PROPS 0x9D3Bu  // 40251  Edit > Switch Properties...
#define CMD_SEQ_PROPS    0x9D3Cu  // 40252  Edit > Sequence Properties...
#define CMD_BUILD_UAS    0x9DA8u  // 40360  File > Build UAS

// Engine globals / addresses 
// GEditor global at 0x1165dfa0: *0x1165dfa0 = UEditorEngine* (GEditor)
//
// Exec calling convention (confirmed from FUN_10e6fa30 SAVEPACKAGE disassembly):
//   MOV ECX, [0x1165dfa0]   ; ECX = GEditor
//   MOV EDX, [ECX + 0x28]   ; EDX = vtable of FExec sub-object (multiple-inheritance base)
//   ADD ECX, 0x28            ; ECX adjusted: this = GEditor + 0x28 (FExec*)
//   PUSH [0x115befb0]        ; arg2 = GLog/GNull output device (PTR_PTR_115befb0)
//   PUSH cmd                 ; arg1 = ANSI command string
//   CALL [EDX]               ; vtable[0] = FExec::Exec - callee cleans 2 args (8 bytes)
//
// NOTE: vtable[0x224/4] on GEditor is NOT Exec - it is UEditorEngine::Get (3 args).
// AUDIO IMPORT / OBJ EXPORT / OBJ SAVEPACKAGE all go through FExec::Exec (vtable[0]).
#define GEDITOR_GLOBAL      0x1165dfa0u
#define EXEC_LOG_DEV        0x115befb0u   // PTR_PTR_115befb0 - GLog/GNull output device ptr

// Refresh thunks (all __thiscall, ECX = WBrowserSound* this):
//   FUN_10e7f410 - RefreshPackages: queries PACKAGES CLASS=Sound, fills package combo
//   FUN_10e7d320 - RefreshGroups:   queries GROUPS CLASS=Sound PACKAGE=..., fills group combo
//   FUN_10e7d580 - RefreshList:     queries QUERY TYPE=Sound ..., fills sound list
// Thunk addresses confirmed via Ghidra xref analysis.
#define REFRESH_PACKAGES_THUNK  0x10e051d3u
#define REFRESH_GROUPS_THUNK    0x10e053efu
#define REFRESH_LIST_THUNK      0x10e022d5u

// UEditorEngine::Get vtable slot on GEditor (NOT on the FExec sub-object).
// Confirmed from UT2004 source: DELETE CLASS=SOUND uses GUnrealEd->Get("OBJ", cmd, result).
// Call: __thiscall, 3 args (section, key, FOutputDevice&), callee cleans 12 bytes.
#define EDITOR_GET_VTABLE_OFFSET  0x224u   // vtable[0x224/4] = vtable[0x89] = Get

// GObjects array (confirmed from FUN_10fa7a70 decompile)
// Layout: TArray<UObject*> – Data ptr at 0x11697b70, Num (count) at 0x11697b74.
// Access: GObjects.Data[i] = *(UObject**)( *(int*)0x11697b70 + i*4 )
#define GOBJECTS_DATA 0x11697b70u   // *(void**)GOBJECTS_DATA = base of UObject* array
#define GOBJECTS_NUM  0x11697b74u   // *(int*)GOBJECTS_NUM    = total slot count

// GNames array (confirmed from thunk_FUN_10e0d170 decompile = FName::GetName())
// Layout: base pointer at 0x1169cfbc, count at 0x1169cfc0.
// FNameEntry* = ((int*)(*(int*)GNAMES_DATA))[FName.Index]
// char* name  = (char*)FNameEntry + FNAME_ENTRY_STR_OFFSET
#define GNAMES_DATA             0x1169cfbcu  // *(int*)GNAMES_DATA = base of FNameEntry* array
#define GNAMES_NUM              0x1169cfc0u  // *(int*)GNAMES_NUM  = count of name entries
#define FNAME_ENTRY_STR_OFFSET  0x0C         // char* name string is at FNameEntry+0x0C

// UObject field offsets (confirmed from FUN_10fa5600 / FUN_10fa7a70 disassembly):
//   +0x04  INT      Index   (GObjects slot index; confirmed: iVar1 = *(int*)(param_1+4))
//   +0x18  UObject* Outer   (confirmed: MOV ECX, [EDI+0x18] in FUN_10fa5600)
//   +0x20  FName    Name    (confirmed: LEA ECX, [EDI+0x20] before FName::GetName() call)
// FName is {INT NameIndex (4), INT Number (4)} = 8 bytes at +0x20.
#define UOBJ_INDEX_OFFSET  0x04
#define UOBJ_OUTER_OFFSET  0x18
#define UOBJ_FNAME_OFFSET  0x20

// ============================================================
// Composite sound resource dialogs and field offsets
// (from Ghidra disassembly of FUN_10e7db38 / SND_fn_vDestroy*)
// ============================================================

// Dialog 142 – Create new resource (name entry)
//   1288 (EDIT)   : resource name field
//   1 (OK), 2 (Cancel)
#define IDD_CREATE_RESOURCE  142
// IDC_CREATE_NAME = 1288 (same numeric value as IDC_EDIT_RADIUS; different dialog, no conflict)
// Radio buttons in dialog 142 for composite type selection:
#define IDC_RB_RANDOM    1290   // "Random"
#define IDC_RB_SEQUENCE  1291   // "Sequence"
#define IDC_RB_SWITCH    1292   // "Switch"

// Context block passed as lParam to CreateResourceDlgProc / DialogBoxParamA.
// name     : filled with the entered sound name on IDOK
// typeFlag : filled with SF_TYPE_RANDOM / SF_TYPE_SEQUENCE / SF_TYPE_SWITCH on IDOK
struct NewResourceCtx { char name[256]; DWORD typeFlag; };

// Dialog 145 – Random (composite Random sound properties)
//   1335 (SysListView32) : child list   1336 (BUTTON) : Insert resource
//   1337 (BUTTON) : Remove resource     1338 (BUTTON) : Edit resource
//   1009 (BUTTON) : Set equal probabilities
//   1342 (STATIC) : Probability of silence label
//   1 (OK), 2 (Cancel)
#define IDD_RANDOM           145
#define IDC_RAND_LIST        1335
#define IDC_RAND_INSERT      1336
#define IDC_RAND_REMOVE      1337
#define IDC_RAND_EDIT        1338
#define IDC_RAND_EQUAL_PROB  1009
#define IDC_RAND_SILENCE_LBL 1342

// Dialog 146 – Random element properties
//   1005 (COMBOBOX) : resource picker
//   1288 (EDIT)     : play weight (integer; same ID as IDC_EDIT_RADIUS, different dialog)
//   1 (OK), 2 (Cancel)
#define IDD_RANDOM_ELEM      146
#define IDC_RELEM_COMBO      1005
// IDC_RELEM_WEIGHT = 1288 (literal; use the value directly in RandomElemDlgProc)

// Dialog 19837 – Sequence (composite Sequence sound properties)
//   1335 (SysListView32) : child list   1336 (BUTTON) : Insert resource
//   1337 (BUTTON) : Remove resource     1338 (BUTTON) : Edit resource
//   1303 (BUTTON) : Move Up (BS_BITMAP) 1304 (BUTTON) : Move Down (BS_BITMAP)
//   1 (OK), 2 (Cancel)
#define IDD_SEQUENCE         19837
#define IDC_SEQ_LIST         1335
#define IDC_SEQ_INSERT       1336
#define IDC_SEQ_REMOVE       1337
#define IDC_SEQ_EDIT         1338
#define IDC_SEQ_MOVEUP       1303
#define IDC_SEQ_MOVEDOWN     1304

// Dialog 19838 – Sequence element properties
//   1005 (COMBOBOX)      : resource picker
//   1346 (EDIT)          : repeat count
//   1347 (AUTOCHECKBOX)  : timed loop
//   1 (OK), 2 (Cancel)
#define IDD_SEQ_ELEM         19838
#define IDC_SELEM_COMBO      1005
#define IDC_SELEM_REPEAT     1346
#define IDC_SELEM_TIMEDLOOP  1347

// Sequence repeat count field layout (TArray<int> at +0x78):
//   Bits 12:0  (0x1FFF) – value: repeat count (normal) OR loop duration in seconds (timed)
//   Bit  13    (0x2000) – timed-loop flag: if set, treat value as loop seconds
//   Bits 15:14           – unknown flags; preserve on round-trip
//
// e.g. raw = 0x200A → timed loop, 10 seconds
//      raw = 3      → normal, play 3 times
#define SEQ_TIMED_LOOP_FLAG  0x2000   // bit 13
#define SEQ_VALUE_MASK       0x1FFF   // lower 13 bits

// Dialog 289 – Surround sound (4.0/5.1) — already exists in binary, SF_TYPE_SURROUND=0x2000
//   Edit boxes 1419-1424: FL/FR/BL/BR/C/LFE file paths
//   Buttons  1303-1308:   "..." browse buttons per channel
//   (Future: hook "New Surround..." and "Surround Properties..." commands for this dialog)

// Dialog 147 – Switch (composite Switch sound properties)
//   1335 (SysListView32) : child list
//   1336 (BUTTON) : Insert resource   1337 (BUTTON) : Remove resource
//   1338 (BUTTON) : Edit resource
//   1 (OK), 2 (Cancel)
#define IDD_SWITCH           147
#define IDC_SW_LIST          1335
#define IDC_SW_INSERT        1336
#define IDC_SW_REMOVE        1337
#define IDC_SW_EDIT          1338

// Dialog 148 – Switch element properties
//   1005 (COMBOBOX) : resource picker (sound)
//   1343 (COMBOBOX) : surface type picker (ESurfaceType enum)
//   1 (OK), 2 (Cancel)
#define IDD_SWITCH_ELEM      148
#define IDC_SWELEM_COMBO     1005
#define IDC_SWELEM_SURFACE   1343

// ESurfaceType enum (UnrealScript, YD 07/10/2002 – confirmed from game source)
// Stored in TArray<int>[i] parallel to TArray<USound*>[i] for Switch composite sounds.
// Value 0 (SURFACE_Generic) is the default/wildcard: returned when no surface type matches.
#define SW_NUM_SURFACE_TYPES  26

// USound composite field offsets
// Confirmed: USound+0x70 is the child reference count, pushed as %d arg for
// ", %d references" sprintf in FUN_10e7db38 (disassembly: MOV EAX,[EBX+0x70]).
// USound+0x74 holds the stRes* (Ubisoft's runtime linked-list container).
// TArray<USound*> at USound+0x6c  (children, used by USound::Serialize / FUN_11117a70)
#define USOUND_TARRAY_DATA_OFFSET  0x6c  // TArray<USound*>.Data  – flat child pointer array
#define USOUND_REF_COUNT_OFFSET    0x70  // TArray<USound*>.Num   – child count
#define USOUND_TARRAY_MAX_OFFSET   0x74  // TArray<USound*>.Max   – allocated capacity
// TArray<int> at USound+0x78  (weights, parallel to children array)
#define USOUND_WARRAY_DATA_OFFSET  0x78  // TArray<int>.Data      – flat weight array
#define USOUND_WARRAY_NUM_OFFSET   0x7c  // TArray<int>.Num
#define USOUND_WARRAY_MAX_OFFSET   0x80  // TArray<int>.Max
// Legacy alias kept only for the Sequence dialog (still uses old stRes layout – do NOT
// use USOUND_REF_LIST_OFFSET in new Random code)
#define USOUND_REF_LIST_OFFSET     0x74  // (Sequence only)

// Composite type flag bits (in USound+0x60 flags DWORD)
// SF_TYPE_RANDOM = 0x100 = SF_STREAM: Ubisoft intentionally reuses the Stream bit
// for Random type.  All developer-authored Random resources have this bit set.
// SF_TYPE_SWITCH   = 0x0080: Surface-material switch — each child sound is mapped to an
//   ESurfaceType enum value.  TArray<USound*> at +0x6c stores child sounds;
//   TArray<int> at +0x78 stores the parallel ESurfaceType values (0=Generic=default).
//   Confirmed from live flag read: existing Ubisoft Switch sounds have flags=0x00000080.
//   In FUN_10e7db38 display code: `(char)uVar1 < '\0'` = bit 7 set = 0x80 = Switch (ESI=2).
//
// SF_TYPE_SURROUND = 0x2000: Xbox HD audio Wave 4.0/5.1 surround stream (.hds files).
//   Display shows FR/FL/BL/BR/C/LFE channel labels; TArray<int>[0] is a channel bitmask.
//   Dialog 289 "Surround sound (4.0/5.1)" already exists in the binary for editing these.
//   NOTE: Creating these via New Switch was the original (wrong) flag — use 0x2000 for that.
#define SF_TYPE_RANDOM     SF_STREAM       // = 0x00000100u (intentional per Ubisoft)
#define SF_TYPE_SEQUENCE   0x00000800u
#define SF_TYPE_SWITCH     0x00000080u     // material surface-type switch (confirmed)
#define SF_TYPE_SURROUND   0x00002000u     // Xbox HD Wave 4.0/5.1 surround (future use)

// Suppressed-from-browser flag bits — FUN_10e7db38 early-exits on these.
// Patch 3 in Initialize() NOPs out both early-exit branches so these sounds appear.
//
// SF_UAS_STREAM = 0x0004: UAS streaming reference.  The USound object in the UAX is a
//   stub only; the actual OGG-encoded audio lives in the map's .uas file (Unreal Audio
//   Stream, Xbox/PC streaming format).  No WAV data is present in the UAX, so Export
//   will not produce usable audio for these entries.
//   Examples: aqua_musicA, aqua_musicB, aqua_ambCorsaire (Amb_Aqua).
//   MC_-prefix sounds (e.g. MC_aquaExt) combine SF_UAS_STREAM with SF_TYPE_SURROUND.
//
// SF_XBOXHD_STREAM = 0x4000: Xbox HD Stream sounds.  These have WAV data extracted
//   from the Xbox .hds (HD Stream) files and are suppressed from the PC editor browser
//   by Ubisoft — the PC build has no .hds pipeline, so Ubisoft hid them to avoid
//   confusion.  Export works normally since WAV data is present in the UAX.
//   Examples: aqua_ocean, aqua_mer, aqua_VitreRafal1, aqua_Bubble (Amb_Aqua).
#define SF_UAS_STREAM      0x00000004u    // OGG/UAS external stream stub (no WAV in UAX)
#define SF_XBOXHD_STREAM   0x00004000u    // Xbox HD Stream (.hds); hidden from PC editor browser

//  dB / linear conversion helpers 
static inline float LinearToDB(float linear)
{
    if (linear <= 0.0f) return -96.0f;
    return 20.0f * log10f(linear);
}
static inline float DBToLinear(float db)
{
    return powf(10.0f, db / 20.0f);
}

// Import Sound dialog data 
// Passed as lParam to DialogBoxParamA for IDD_IMPORT_SOUND (resource 156).
// On WM_INITDIALOG the fields are written into the controls; on IDOK they are
// read back so the caller can build the AUDIO IMPORT command.
struct ImportSoundData
{
    char filePath [MAX_PATH * 2];   // full path to the WAV (display only in dialog)
    char pkgName  [256];            // Package: edit field (1067)
    char grpName  [256];            // Group:   edit field (1066)
    char soundName[256];            // Name:    edit field (1065)
};

// Rename Sound dialog data 
// Passed as lParam to DialogBoxParamA for IDDIALOG_RENAME (19805).
// oldX fields are pre-filled by SB_HandleRename; newX fields are read back
// on IDOK so the caller can build the OBJ RENAME command.
struct RenameSoundData
{
    char oldName   [256];
    char oldGroup  [256];
    char oldPackage[256];
    char newName   [256];
    char newGroup  [256];
    char newPackage[256];
};

// ============================================================
// Composite sound structs
// ============================================================

// Context passed as lParam to the parent property dialogs (145, 19837).
struct CompositePropsCtx {
    void* pSound;    // USound* being edited
    void* pBrowser;  // WBrowserSound* (this_ptr) – needed to enumerate available sounds
};

// Context passed as lParam to the element-level dialogs (146, 19838).
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

// Per-child info held in memory during the properties dialog session.
// Children are edited as a flat array; TArrays are rebuilt on OK.
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

// Heap-allocated working context for the parent property dialogs.
// Stored in GWL_USERDATA; freed on dialog exit.
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

// ============================================================
// Switch sound structs (ESurfaceType-based composite)
// ESurfaceType enum (26 values, sequential 0-25), from SCCT UnrealScript source.
// TArray<USound*> at +0x6c: child sounds (parallel to surface types).
// TArray<int>     at +0x78: ESurfaceType value per child (0=Generic = default/wildcard).
// Both arrays share count at USound+0x70.
// ============================================================
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

// ============================================================
// stRes linked-list field offsets (confirmed from Ghidra:
//   SND_fn_vDestroyRandomElementM  @ 0x112786d0
//   SND_fn_vDestroySequenceElement @ 0x11278580)
//
// RandomElement (0x18 bytes, allocated by SND_fn_pCreateRandomElementM):
//   +0x00  void* pChildSound   – USound* of child (our convention)
//   +0x04  DWORD unused        – always 0
//   +0x08  int   nWeight       – integer weight (default 1)
//   +0x0C  void* pPrev         – doubly-linked list prev pointer (runtime only)
//   +0x10  void* pNext         – doubly-linked list next pointer (runtime only)
//   +0x14  void* pParentStRes  – pointer back to parent stRes (runtime only)
//
// Random parent stRes (at USound+0x74):
//   +0x14  void* pFirst   +0x18  void* pLast   +0x1C  int nCount
//
// SequenceElement (we allocate 0x18 bytes; SND engine only uses first 0x10):
//   +0x00  void* pChildSound
//   +0x04  void* pPrev         – runtime only
//   +0x08  void* pNext         – runtime only
//   +0x0C  void* pParentStRes  – runtime only
//   +0x10  int   nRepeat       – our extra field (beyond SND's 0x10-byte layout)
//   +0x14  int   bTimedLoop    – our extra field
// (The SND engine's _SND_fn_vFreeSnd_4 frees by pointer, not by size, so
//  allocating 0x18 bytes is safe even though SND creates 0x10-byte elements.)
//
// Sequence parent stRes (at USound+0x74):
//   +0x1C  void* pFirst   +0x20  void* pLast   +0x24  int nCount
// ============================================================

// Global name cache: USound* → display name (populated by StoreLParamHelper)
// Allows GetSoundName to resolve cross-package/cross-group references that are
// not currently visible in the filtered ListView.
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
#ifdef _DEBUG
// Debug only: enumerate all sounds in current package (including delisted ones)
static void __cdecl      SB_ShowFlagsDump     (void* this_ptr);
#endif

// WBrowserSound layout helpers 
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


// ============================================================
// Hook 1 – StoreSoundLParam
//
//   Address : 0x10E7DD57
//   Displaced: 33 C9          XOR ECX, ECX
//              89 45 0C       MOV [EBP+0Ch], EAX
//   Return  : 0x10E7DD5C
//
//   EAX = item_index, EBX = USound*, [EBP-0x54] = this
// ============================================================
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


// ============================================================
// Hook 2 – ShowSoundProperties
//
//   Address : 0x10E7E41E  (SEH epilogue in OnCommand)
//   Displaced: 8B 4D F4      MOV ECX, [EBP-0Ch]
//              64 89         first 2 bytes of 7-byte MOV FS:[0],ECX
//   Return  : 0x10E7E428    (byte AFTER the complete 7-byte instruction)
//
//   [EBP+8] = Command ID, ESI = this
// ============================================================
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


// ============================================================
// Hook 3 – SoundBrowserCommands
//
//   Address : 0x10E7E690  (LAB_10e7e690: fallthrough block for unhandled cmds)
//   Displaced: 50             PUSH EAX          (1 byte)
//              8B CE          MOV ECX, ESI      (2 bytes)
//              E8 0A 8B F8 FF CALL 0x10e069a2   (5 bytes, first 2 bytes displaced)
//   Return  : 0x10E7E698     (byte after the complete 5-byte CALL instruction)
//
//   On entry:
//     EAX = cmd_id  (loaded from [EBP+8] at function entry, still intact here)
//     ESI = this    (WBrowserSound*, set at 0x10E7E3EC)
//
//   Pass-through: re-execute the displaced PUSH/MOV, call OrigFn (which does
//   its own RET-cleanup of the arg), then jump to Return.
//   Our commands: call the handler directly, then jump to Return.
// ============================================================
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
    }
}


// ExecEditorCommand 
// Replicates the exact calling sequence from FUN_10e6fa30 (OBJ SAVEPACKAGE):
//
//   MOV ECX, [0x1165dfa0]   ; ECX = GEditor
//   MOV EDX, [ECX + 0x28]   ; EDX = vtable of FExec sub-object (mult-inherit base)
//   ADD ECX, 0x28            ; ECX = GEditor+0x28 = FExec* (adjusted this)
//   PUSH [0x115befb0]        ; arg2 = GLog output device (PTR_PTR_115befb0)
//   PUSH cmd                 ; arg1 = ANSI command string
//   CALL [EDX]               ; FExec::Exec(cmd, outDev) - callee cleans 2 args
//
// The function at vtable[0x224/4] (used by DELETE SOUND) is UEditorEngine::Get,
// which takes 3 args and is a completely different dispatch path.  All editable
// commands (AUDIO IMPORT, OBJ EXPORT, OBJ SAVEPACKAGE) go through FExec::Exec.
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


// CallEditorGet 
// Calls UEditorEngine::Get on GEditor directly (NOT the FExec sub-object).
// Used for DELETE CLASS=SOUND - confirmed from UT2004 source and prior
// Ghidra analysis that delete goes through vtable[0x224/4] (slot 0x89) on
// GEditor with signature: Get(const char* section, const char* key, FOutputDevice& out).
//
// We pass GLog as the output device so any error text is captured by the engine
// log.  Callee cleans 3×4 = 12 bytes (__thiscall).
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


// RefreshPackages / RefreshGroups / RefreshList
// Thin __thiscall wrappers around the engine's three-step combo/list refresh.
//
//   RefreshPackages (FUN_10e7f410): queries PACKAGES CLASS=Sound, refills package combo,
//                                   always resets package selection to index 0.
//   RefreshGroups   (FUN_10e7d320): queries GROUPS CLASS=Sound PACKAGE=..., refills group
//                                   combo from current package selection, resets to index 0.
//   RefreshList     (FUN_10e7d580): queries QUERY TYPE=Sound PACKAGE=... GROUP=..., refills
//                                   the sound list from current package+group selection.
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


// NavigateToPackageGroup
// After import or rename, land the browser on the given package/group.
// Strategy (mirrors UT2004 BrowserSound source):
//   1. RefreshPackages - repopulates the package combo so any new package appears.
//      NOTE: RefreshPackages always resets selection to index 0; we override that next.
//   2. CB_SETCURSEL on the package combo to the target package.
//   3. RefreshGroups  - repopulates the group combo from the newly selected package.
//      NOTE: RefreshGroups always resets selection to index 0; we override that next.
//   4. CB_SETCURSEL on the group combo to the target group (if any).
//   5. RefreshList    - populates the sound list from current package+group.
static void NavigateToPackageGroup(void* this_ptr, const char* pkg, const char* grp)
{
    if (!pkg || pkg[0] == '\0') return;

    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    // 1. Repopulate the package combo (new package will now be listed)
    RefreshPackages(this_ptr);

    // 2. Select the target package
    LRESULT pkgIdx = SendMessageA(hPkgCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)pkg);
    if (pkgIdx != CB_ERR)
        SendMessageA(hPkgCombo, CB_SETCURSEL, (WPARAM)pkgIdx, 0);

    // 3. Repopulate the group combo for the selected package
    RefreshGroups(this_ptr);

    // 4. Select the target group
    if (grp && grp[0] != '\0')
    {
        LRESULT grpIdx = SendMessageA(hGrpCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)grp);
        if (grpIdx != CB_ERR)
            SendMessageA(hGrpCombo, CB_SETCURSEL, (WPARAM)grpIdx, 0);
    }

    // 5. Populate the sound list for the final selection
    RefreshList(this_ptr);
}


// NavigateAfterDelete
// Called only on a SUCCESSFUL delete (sound confirmed gone from list).
// Uses the sound LIST count as the ground truth rather than the group combo
// state, because the compiled RefreshGroups may not clear the combo before
// repopulating, making CB_GETCOUNT unreliable as an "empty package" signal.
static void NavigateAfterDelete(void* this_ptr)
{
    HWND hList     = GetListHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    // Sounds remain in the current group - nothing to do
    if (SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0) > 0)
        return;

    // Current group is empty.  RefreshGroups re-queries the group combo;
    // if other groups with sounds exist they land at index 0 automatically.
    // RefreshList then shows whatever that first group contains.
    RefreshGroups(this_ptr);
    RefreshList(this_ptr);

    // If sounds appeared after the refresh, we landed on a valid group - done.
    if (SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0) > 0)
        return;

    // Still empty: the whole package has no sounds.  Step to the adjacent
    // package (next, or previous if we were at the end of the list), then
    // remove the now-empty package entry from the combo entirely.
    // (UPackage persists in memory so PACKAGES CLASS=Sound keeps returning it;
    // CB_DELETESTRING removes it visually until a full RefreshPackages rebuilds
    // the list.)
    LRESULT pkgCount = SendMessageA(hPkgCombo, CB_GETCOUNT, 0, 0);
    LRESULT curPkg   = SendMessageA(hPkgCombo, CB_GETCURSEL, 0, 0);
    if (curPkg == CB_ERR) curPkg = 0;

    LRESULT targetPkg;
    if (curPkg + 1 < pkgCount)
        targetPkg = curPkg + 1;         // move to the next package
    else if (curPkg > 0)
        targetPkg = curPkg - 1;         // we were at the end, step back
    else
        targetPkg = 0;                  // only one package, stay put

    // Delete the empty package from the combo.
    // Items above curPkg shift down by one, so adjust targetPkg if needed.
    SendMessageA(hPkgCombo, CB_DELETESTRING, (WPARAM)curPkg, 0);
    if (targetPkg > curPkg)
        targetPkg--;

    SendMessageA(hPkgCombo, CB_SETCURSEL, (WPARAM)targetPkg, 0);
    RefreshGroups(this_ptr);
    RefreshList(this_ptr);
}


// SB_HandleSave
static void __cdecl SB_HandleSave(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    // Read current package name from combo edit field
    char pkgName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    if (pkgName[0] == '\0')
    {
        MessageBoxA(hParent, "Select a package first.", "Message",
                    MB_OK);
        return;
    }

    // Pre-fill filename with PackageName.uax
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

    // OBJ SAVEPACKAGE PACKAGE="Amb_Aqua" FILE="C:\...\Amb_Aqua.uax"
    char cmd[MAX_PATH * 2 + 128];
    snprintf(cmd, sizeof(cmd),
             "OBJ SAVEPACKAGE PACKAGE=\"%s\" FILE=\"%s\"",
             pkgName, filePath);
    ExecEditorCommand(cmd);
}


// ============================================================
// SB_HandleMakeUAS
// Directly calls UUNIAudioSubsystem::UpdateStreamFile (0x10F77AC0)
// to OGG-encode qualifying stream sounds and write a .uas file for
// the currently loaded map.  Triggered by File > Build UAS (CMD_BUILD_UAS = 40360).
//
// Why a direct call is necessary:
//   UAudioSubsystem_ExportToXBox (0x10F70BA0) contains a dead-code
//   gate at 0x10F71C30-0x10F71C33:
//
//     10f71bab: MOV dword ptr [EBP-0x4C], 0x0   ; local_50 zeroed here
//     ...
//     10f71c2d: XOR EBX, EBX                     ; EBX (iVar8) = 0
//     10f71c30: CMP EBX, dword ptr [EBP + -0x4c] ; compare 0 with local_50 (=0)
//     10f71c33: JGE 0x10f71ee4                   ; 0 >= 0: ALWAYS taken
//     ; UpdateStreamFile calls at 0x10F71E33 / 0x10F71E60 NEVER reached
//
//   local_50 is zeroed by the MOV at 0x10F71BAB (after the cleanup
//   for-loop exits).  Patching the JGE target would cause an infinite
//   loop (loop-back edge: INC EBX; JMP 0x10F71C30 at 0x10F71EDE-0x10F71EDF).
//   Calling UpdateStreamFile directly sidesteps the broken gate entirely.
//
// Parameters (confirmed from Ghidra decompile of 0x10F77AC0):
//   this  = 0x1168D290  — UAudioSubsystem singleton
//   arg1  = 0           — param_2=0 (null): GObjects filter selects sounds with SF_UAS_STREAM (0x4)
//                         MUST be 0. Non-zero values are treated as a package-list pointer by an
//                         earlier code section and cause an access violation on deref.
//   arg2  = 1           — encode format 1: OGG (2 = XB/Xbox ADPCM, NOT what we want)
//   arg3  = 2           — handle slot 2: output file handle at [this+0xc]  (UAS slot)
//   arg4  = 5.0f        — float parameter (matches ExportToXBox's own calls)
//
// Prerequisites:
//   - A map must be loaded (UpdateStreamFile names the .uas from the current map)
//   - Target USounds must have SF_UAS_STREAM (0x4) set via Sound Properties > Stream
//   - WAV files must exist at ..\packages\sounds\<pkg>\<name>.wav on disk
//   - oggenc.exe must be on PATH or in the editor working directory
// ============================================================

#define AUDIO_SUBSYSTEM_PTR  0x1168D290u
#define UPDATE_STREAM_FILE   0x10F77AC0u

// ---- GObjects layout (confirmed from Ghidra) ----
// GObjects array pointer : 0x11697B70  (MOV EAX,[0x11697B70] → array base)
// GObjects count         : 0x11697B74
// USound class object    : 0x11823128  (confirmed from UpdateStreamFile CMP)
// UObject layout:
//   +0x18  Outer  (UObject* — outer package/group)
//   +0x24  Class  (UClass*  — class of this object)
//   +0x28  on UClass: SuperClass pointer
//   +0x60  on USound: Flags (uint32)
// ----------------------------------------------------------------
// Pre-call: delete stale temp files and existing target .uas so
// UpdateStreamFile's internal MoveFileA can succeed.
// ----------------------------------------------------------------
static void USF_ClearTempPattern(const char* pattern, bool changeExt)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        char tempPath[MAX_PATH];
        snprintf(tempPath, sizeof(tempPath), "..\\packages\\sounds\\%s", fd.cFileName);

        char uasPath[MAX_PATH] = {};
        const char* baseName = fd.cFileName + 5; // skip "temp_"
        snprintf(uasPath, sizeof(uasPath), "..\\packages\\sounds\\%s", baseName);
        if (changeExt)
        {
            char* dot = strrchr(uasPath, '.');
            if (dot) strcpy_s(dot, 5, ".uas");
        }
        DeleteFileA(tempPath);
        DeleteFileA(uasPath);
    }
    while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

static void USF_ClearTempFiles()
{
    USF_ClearTempPattern("..\\packages\\sounds\\temp_*.hds", true);
    USF_ClearTempPattern("..\\packages\\sounds\\temp_*.uas", false);
}

// ----------------------------------------------------------------
// Post-call: rename any temp files that UpdateStreamFile's MoveFileA
// failed to rename (fails silently if destination already exists).
// ----------------------------------------------------------------
static void USF_FinishRenamePattern(const char* pattern, bool changeExt)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        char tempPath[MAX_PATH];
        snprintf(tempPath, sizeof(tempPath), "..\\packages\\sounds\\%s", fd.cFileName);

        char uasPath[MAX_PATH] = {};
        const char* baseName = fd.cFileName + 5; // skip "temp_"
        snprintf(uasPath, sizeof(uasPath), "..\\packages\\sounds\\%s", baseName);
        if (changeExt)
        {
            char* dot = strrchr(uasPath, '.');
            if (dot) strcpy_s(dot, 5, ".uas");
        }
        DeleteFileA(uasPath);
        MoveFileA(tempPath, uasPath);
    }
    while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

static void USF_FinishTempFiles()
{
    USF_FinishRenamePattern("..\\packages\\sounds\\temp_*.hds", true);
    USF_FinishRenamePattern("..\\packages\\sounds\\temp_*.uas", false);
}

// ================================================================
// Build UAS – Package Picker helpers
// ================================================================

// UAS_GetObjectFName — reads the FName string of a UObject into buf.
// Returns true on success.
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

// UAS_GetPackageName — walks the Outer chain to the top-level UObject
// (whose Outer is NULL) and returns its FName string = the package name.
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

// ----------------------------------------------------------------
// In-memory DLGTEMPLATE builder for the package picker dialog.
//
// Dialog layout (dialog units, 8pt MS Shell Dlg):
//   260 x 190 — caption, static label, multiselect listbox, 4 buttons
//
// Controls:
//   IDC_UASPKG_LIST  (200) — LBS_MULTIPLESEL listbox; all items
//                            pre-selected so the user just unticks
//                            unwanted packages
//   IDC_UASPKG_ALL   (201) — "Select All" button
//   IDC_UASPKG_NONE  (202) — "Clear All" button
//   IDOK             (  1) — "Build" (default)
//   IDCANCEL         (  2) — "Cancel"
// ----------------------------------------------------------------
#define IDC_UASPKG_LIST        200
#define IDC_UASPKG_ALL         201
#define IDC_UASPKG_NONE        202

// Compression dialog — IDC values match the original UnrealEd RC resource
#define IDC_UAS_QUALITY_SLIDER 1330
#define IDC_UAS_QUALITY_LABEL  1339

struct DlgBldBuf {
    std::vector<uint8_t> buf;
    void a2() { while (buf.size() & 1) buf.push_back(0); }
    void a4() { while (buf.size() & 3) buf.push_back(0); }
    void w (WORD  v) { buf.push_back(v & 0xFF); buf.push_back(v >> 8); }
    void dw(DWORD v) { w((WORD)v); w((WORD)(v >> 16)); }
    void ws(const wchar_t* s) { for (; *s; ++s) w((WORD)*s); w(0); }
    void atom(WORD a) { w(0xFFFF); w(a); }
};

// ----------------------------------------------------------------
// "Select Compression" dialog — restores the original UnrealEd dialog
// (resource 143:1036) with the "No Compression" button omitted, since
// the PC UAS streaming code only contains an OGG Vorbis decoder.
//
// Layout (dialog units, 8pt MS Sans Serif, matching original RC dims):
//   186 × 71
//   IDC_UAS_QUALITY_LABEL  (1339) — "Quality for Ogg Vorbis: 5" static
//   IDC_UAS_QUALITY_SLIDER (1330) — trackbar, range 0–10, default 5
//   IDOK                   (   1) — "Ogg Vorbis" (default / OK)
//   IDCANCEL               (   2) — "Cancel"
// ----------------------------------------------------------------
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
    b.w(4);             // cdit = 4 controls
    b.w(0); b.w(0);     // x, y
    b.w(186); b.w(71);  // cx, cy (matches original RC)
    b.w(0);             // no menu
    b.w(0);             // default dialog class
    b.ws(L"Build UAS");
    b.w(8);
    b.ws(L"MS Sans Serif");

    // ---- Control 1: "Quality for Ogg Vorbis: 5" label ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | SS_LEFT);
    b.dw(0);
    b.w(7); b.w(7); b.w(172); b.w(10);
    b.w(IDC_UAS_QUALITY_LABEL);
    b.atom(0x0082);   // STATIC
    b.ws(L"Quality for Ogg Vorbis: 5.0");
    b.w(0);

    // ---- Control 2: trackbar ----
    // Class is "msctls_trackbar32" — not a system atom, so written as a string.
    b.a4();
    b.dw(TBS_HORZ | TBS_AUTOTICKS | WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    b.dw(0);
    b.w(5); b.w(17); b.w(175); b.w(20);
    b.w(IDC_UAS_QUALITY_SLIDER);
    b.ws(L"msctls_trackbar32");
    b.ws(L"");  // window title (empty)
    b.w(0);

    // ---- Control 3: "Ogg Vorbis" (IDOK, default) ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(20); b.w(50); b.w(60); b.w(14);
    b.w(IDOK);
    b.atom(0x0080);   // BUTTON
    b.ws(L"OK");
    b.w(0);

    // ---- Control 4: "Cancel" (IDCANCEL) ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(105); b.w(50); b.w(60); b.w(14);
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
            SendMessageA(hSlider, TBM_SETTICFREQ, 10,   0);  // tick every 1.0
            SendMessageA(hSlider, TBM_SETPOS,     TRUE, static_cast<LPARAM>(ctx->quality));
        }
        return TRUE;

    case WM_HSCROLL:
        {
            // Update the label as the user drags the slider
            HWND hSlider = GetDlgItem(hDlg, IDC_UAS_QUALITY_SLIDER);
            int pos = static_cast<int>(SendMessageA(hSlider, TBM_GETPOS, 0, 0));
            char label[64];
            snprintf(label, sizeof(label), "Quality for Ogg Vorbis: %.1f",
                     pos / 10.0);
            SetDlgItemTextA(hDlg, IDC_UAS_QUALITY_LABEL, label);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            {
                HWND hSlider = GetDlgItem(hDlg, IDC_UAS_QUALITY_SLIDER);
                ctx->quality   = static_cast<int>(
                    SendMessageA(hSlider, TBM_GETPOS, 0, 0));
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

    // DLGTEMPLATE header (DS_SETFONT so we can specify the font below)
    DWORD dlgStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU
                   | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    b.dw(dlgStyle);
    b.dw(0);                // exStyle
    b.w(6);                 // cdit = 6 controls
    b.w(0); b.w(0);         // x, y
    b.w(260); b.w(190);     // cx, cy
    b.w(0);                 // no menu
    b.w(0);                 // default dialog class
    b.ws(L"Build UAS"); // title (en-dash)
    // DS_SETFONT: point size + face name
    b.w(8);
    b.ws(L"MS Shell Dlg");

    // ---- Control 1: static label ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | SS_LEFT);
    b.dw(0);
    b.w(7); b.w(5); b.w(246); b.w(10);
    b.w(0xFFFF);            // id (unused for statics)
    b.atom(0x0082);         // STATIC
    b.ws(L"Select packages to include in the UAS:");
    b.w(0);

    // ---- Control 2: multiselect listbox ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP
       | LBS_MULTIPLESEL | LBS_HASSTRINGS | LBS_NOTIFY);
    b.dw(0);
    b.w(7); b.w(18); b.w(246); b.w(130);
    b.w(IDC_UASPKG_LIST);
    b.atom(0x0083);         // LISTBOX
    b.ws(L"");
    b.w(0);

    // ---- Control 3: "Select All" button ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(7); b.w(162); b.w(58); b.w(14);
    b.w(IDC_UASPKG_ALL);
    b.atom(0x0080);         // BUTTON
    b.ws(L"Select All");
    b.w(0);

    // ---- Control 4: "Clear All" button ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(68); b.w(162); b.w(58); b.w(14);
    b.w(IDC_UASPKG_NONE);
    b.atom(0x0080);
    b.ws(L"Clear All");
    b.w(0);

    // ---- Control 5: "Build" (IDOK, default) ----
    b.a4();
    b.dw(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP);
    b.dw(0);
    b.w(147); b.w(162); b.w(50); b.w(14);
    b.w(IDOK);
    b.atom(0x0080);
    b.ws(L"Build");
    b.w(0);

    // ---- Control 6: "Cancel" (IDCANCEL) ----
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

// Context block passed to UASPkgPickerDlgProc via DialogBoxIndirectParamA.
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
            for (const auto& pkg : *ctx->packages)
            {
                int idx = static_cast<int>(
                    SendMessageA(hList, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(pkg.c_str())));
                // Default: nothing selected except Interface (every map uses it)
                bool forceOn = (pkg == "Interface");
                SendMessageA(hList, LB_SETSEL, forceOn ? TRUE : FALSE,
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

// ----------------------------------------------------------------
// EnumChildWindows callback: finds the "Map : <name>" label inside
// the Sound Browser and copies its text into the char[256] buffer
// passed as lParam.  Stops on the first match.
// ----------------------------------------------------------------
static BOOL CALLBACK FindMapLabelProc(HWND hWnd, LPARAM lParam)
{
    char buf[256];
    if (GetWindowTextA(hWnd, buf, sizeof(buf)) > 0 &&
        strncmp(buf, "Map :", 5) == 0)
    {
        strncpy_s(reinterpret_cast<char*>(lParam), 256, buf, _TRUNCATE);
        return FALSE;   // found – stop enumeration
    }
    return TRUE;        // keep looking
}

// ----------------------------------------------------------------
// USF_CallUpdateStreamFile
// Isolated SEH wrapper around the UpdateStreamFile engine call.
// Must contain NO C++ objects with destructors — MSVC forbids __try
// in functions that require object unwinding (C2712).
//
// Behaviour:
//   - Temporarily zeros the AudioSubsystem's preliminary-list COUNT at
//     [AudioSubsystem+0x30], leaving the data pointer at [+0x2C] alone.
//     With count = 0 UpdateStreamFile sets local_ae0 = 0 and never enters
//     its preliminary-list inner loop, so [+0x2C] is never read, freed,
//     or reallocated by the function — eliminating any risk of a heap
//     conflict between MSVCRT new[] and Unreal's FMallocWindows.
//     The count is restored on every exit path (normal and exception).
//   - NOP-patches the map-name filter at kMapFilterAddr so sounds from
//     all packages pass the GObjects scan (package selection is already
//     handled by the SF_UAS_STREAM flag masking in Phase 2a).
//   - Calls UpdateStreamFile ONCE with OGG-encode args.
//     UpdateStreamFile returns 0 for success (UE2 convention); the old
//     retry-loop that tested (result != 0) always ran twice and wrote
//     every sound twice — the retry has been removed entirely.
//   - Restores the count, the filter bytes, and the caller's masked
//     USound flags on every exit path (normal, exception).
//
// The sound list is now provided via a stub UAS file on disk (Phase 2b),
// which UpdateStreamFile reads as carry-over (iStack_acc = N sounds).
// With local_ae0 = 0: piVar6 = 0 + N = N → encoding loop runs N times.
//
// Returns:
//   true  — completed without an SEH exception
//   false — SEH exception caught; all state already restored
// ----------------------------------------------------------------

static bool USF_CallUpdateStreamFile(uintptr_t* maskedObjs,
                                     uint32_t*  maskedFlags,
                                     int        maskCount)
{
    static const uint8_t   kMapFilterJZ[6] = { 0x0F, 0x84, 0x18, 0x0C, 0x00, 0x00 };
    static const uint8_t   kNop6[6]        = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    static const uintptr_t kMapFilterAddr  = 0x10F78364u;

    const DWORD pAudio = AUDIO_SUBSYSTEM_PTR;
    const DWORD pfnUSF = UPDATE_STREAM_FILE;

    // Zero preliminary-list count — touch ONLY [+0x30], never [+0x2C].
    // When count = 0 the function skips the preliminary-list loop and
    // never touches the data pointer, so no Unreal allocator calls touch
    // our memory and no pointer is left dangling after we restore.
    int savedListCount = *reinterpret_cast<int*>((uintptr_t)pAudio + 0x30u);
    *reinterpret_cast<int*>((uintptr_t)pAudio + 0x30u) = 0;

    MemoryWriter::WriteBytes(kMapFilterAddr, kNop6, 6);

    __try
    {
        __asm {
            push 0x40a00000   // 5.0f
            push 2            // param_4: UAS handle slot [this+0xc]
            push 1            // param_3: OGG format
            push 0            // param_2=0: read from AudioSubsystem internal list
            mov  ecx, pAudio
            mov  eax, pfnUSF
            call eax
            // Return value (eax) = 0 means success in UE2 — do NOT test
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        MemoryWriter::WriteBytes(kMapFilterAddr, kMapFilterJZ, 6);
        *reinterpret_cast<int*>((uintptr_t)pAudio + 0x30u) = savedListCount;
        for (int i = 0; i < maskCount; ++i)
            *reinterpret_cast<uint32_t*>(maskedObjs[i] + 0x60) = maskedFlags[i];
        return false;
    }

    MemoryWriter::WriteBytes(kMapFilterAddr, kMapFilterJZ, 6);
    *reinterpret_cast<int*>((uintptr_t)pAudio + 0x30u) = savedListCount;
    for (int i = 0; i < maskCount; ++i)
        *reinterpret_cast<uint32_t*>(maskedObjs[i] + 0x60) = maskedFlags[i];
    return true;
}

static void __cdecl SB_HandleMakeUAS(void* this_ptr)
{
    // ----------------------------------------------------------------
    // Guard: require a real map to be loaded before building.
    //
    // The Sound Browser label covers both cases:
    //   "Map : None"  — no map loaded since the editor started
    //   "Map : "      — blank after the prefix when File > New Map was
    //                   used to leave a previously loaded map
    //
    // mapText/mapName are kept at function scope so Phase 2b.5 can use
    // the map name to locate and delete the existing .uas on disk.
    // ----------------------------------------------------------------
    HWND hBrowser = GetParentHWND(this_ptr);
    char mapText[256] = {};
    EnumChildWindows(hBrowser, FindMapLabelProc,
                     reinterpret_cast<LPARAM>(mapText));

    // mapText + 6 skips the "Map : " prefix; the remainder is the
    // map name, which is either "None", "" (blank), or a real name.
    const char* mapName = mapText + 6;
    {
        if (mapText[0] == '\0' ||
            mapName[0]  == '\0' ||
            strcmp(mapName, "None") == 0)
        {
            MessageBoxA(hBrowser,
                        "Load a map before building a UAS file.",
                        "Message", MB_OK);
            return;
        }
    }

    // ----------------------------------------------------------------
    // Package picker: walk GObjects to find which packages have at least
    // one SF_UAS_STREAM sound, then let the user choose which to include.
    // ----------------------------------------------------------------
    uintptr_t* gObjects = *reinterpret_cast<uintptr_t**>(0x11697B70u);
    int        gCount   = *reinterpret_cast<int*>       (0x11697B74u);

    std::vector<std::string> streamPkgs;
    {
        char pkgBuf[256];
        for (int i = 0; i < gCount; ++i)
        {
            uintptr_t obj = gObjects[i];
            if (!obj) continue;

            // Class walk: must be a USound
            uintptr_t cls = *reinterpret_cast<uintptr_t*>(obj + 0x24);
            bool isUSound = false;
            for (uintptr_t c = cls; c; c = *reinterpret_cast<uintptr_t*>(c + 0x28))
            {
                if (c == 0x11823128u) { isUSound = true; break; }
                if (c < 0x10000000u || c > 0x20000000u) break;
            }
            if (!isUSound) continue;

            uint32_t flags = *reinterpret_cast<uint32_t*>(obj + 0x60);
            if (!(flags & SF_UAS_STREAM)) continue;

            pkgBuf[0] = '\0';
            if (!UAS_GetPackageName(obj, pkgBuf, sizeof(pkgBuf))) continue;
            if (pkgBuf[0] == '\0') continue;

            // Add to list only once
            bool seen = false;
            for (const auto& p : streamPkgs)
                if (p == pkgBuf) { seen = true; break; }
            if (!seen) streamPkgs.push_back(pkgBuf);
        }
    }

    HWND hBrowserWnd = hBrowser;  // hBrowser already obtained above

    if (streamPkgs.empty())
    {
        MessageBoxA(hBrowserWnd,
            "No packages with Stream-flagged sounds are currently loaded.\r\n\r\n"
            "Open the relevant packages in the Sound Browser and mark the\r\n"
            "sounds with Sound Properties > Stream, then try again.",
            "Message", MB_OK);
        return;
    }

    // Show the package picker — all packages pre-selected, user unticks
    // whichever ones they want to exclude.
    UASPkgPickerCtx pickerCtx;
    pickerCtx.packages  = &streamPkgs;
    pickerCtx.cancelled = true;
    {
        std::vector<uint8_t> tmpl = BuildUASPkgDlgTemplate();
        DialogBoxIndirectParamA(
            GetModuleHandleA(nullptr),
            reinterpret_cast<LPCDLGTEMPLATEA>(tmpl.data()),
            hBrowserWnd,
            UASPkgPickerDlgProc,
            reinterpret_cast<LPARAM>(&pickerCtx));
    }
    if (pickerCtx.cancelled) return;

    // Show the compression-quality dialog (mirrors original UnrealEd dialog 143:1036)
    UASCompCtx compCtx;
    compCtx.quality   = 50;  // default 5.0 — matches original editor default
    compCtx.cancelled = true;
    {
        std::vector<uint8_t> tmpl = BuildUASCompDlgTemplate();
        DialogBoxIndirectParamA(
            GetModuleHandleA(nullptr),
            reinterpret_cast<LPCDLGTEMPLATE>(tmpl.data()),
            hBrowserWnd,
            UASCompDlgProc,
            reinterpret_cast<LPARAM>(&compCtx));
    }
    if (compCtx.cancelled) return;

    // Build a fast-lookup set of selected package names
    std::unordered_set<std::string> selectedPkgs;
    for (size_t i = 0; i < pickerCtx.selected.size() && i < streamPkgs.size(); ++i)
        if (pickerCtx.selected[i]) selectedPkgs.insert(streamPkgs[i]);

    if (selectedPkgs.empty())
    {
        MessageBoxA(hBrowserWnd,
            "No packages selected to encode into UAS.",
            "Message", MB_OK);
        return;
    }

    // ----------------------------------------------------------------
    // Phase 2 – Build the UAS directly (bypasses UpdateStreamFile).
    //
    // UpdateStreamFile's preliminary-list / carry-over split makes fresh
    // OGG encoding impossible without injecting into the AudioSubsystem's
    // heap-managed list pointer, which triggers FMallocWindows::Free
    // corruption.  We bypass it entirely:
    //   1. Walk GObjects → collect qualifying USounds (SF_UAS_STREAM,
    //      package in selectedPkgs).
    //   2. Run oggenc.exe for each sound's WAV at
    //        ..\\packages\\sounds\\<pkg>\\<name>.wav
    //   3. Assemble the UAS:  DWORD count  +  N×0x44 entries  +  OGG data.
    //
    // No engine state is touched — no flags patched, no AudioSubsystem
    // fields modified, no NOP patches.
    // ----------------------------------------------------------------

    // Step 2a: collect qualifying sounds from selected packages
    struct SoundInfo { std::string pkg; std::string name; };
    std::vector<SoundInfo> sounds;
    {
        char pkgBuf[256];
        char nameBuf[64];
        for (int i = 0; i < gCount; ++i)
        {
            uintptr_t obj = gObjects[i];
            if (!obj) continue;

            uintptr_t cls = *reinterpret_cast<uintptr_t*>(obj + 0x24);
            bool isUSound = false;
            for (uintptr_t c = cls; c; c = *reinterpret_cast<uintptr_t*>(c + 0x28))
            {
                if (c == 0x11823128u) { isUSound = true; break; }
                if (c < 0x10000000u || c > 0x20000000u) break;
            }
            if (!isUSound) continue;

            if (!(*reinterpret_cast<uint32_t*>(obj + 0x60) & SF_UAS_STREAM)) continue;

            pkgBuf[0] = '\0';
            UAS_GetPackageName(obj, pkgBuf, sizeof(pkgBuf));
            if (pkgBuf[0] == '\0' || !selectedPkgs.count(pkgBuf)) continue;

            nameBuf[0] = '\0';
            UAS_GetObjectFName(obj, nameBuf, sizeof(nameBuf));
            if (nameBuf[0] == '\0') continue;

            sounds.push_back({ pkgBuf, nameBuf });
        }
    }

    if (sounds.empty())
    {
        MessageBoxA(hBrowserWnd,
            "No Stream-flagged sounds found for the selected packages.\r\n\r\n"
            "Make sure the packages are loaded and the sounds have\r\n"
            "Sound Properties > Stream enabled.",
            "Build UAS", MB_OK);
        return;
    }

    // Step 2b: OGG-encode each sound via oggenc.exe
    int count      = static_cast<int>(sounds.size());
    int headerSize = 4 + count * 0x44;
    std::vector<std::vector<uint8_t>> oggBlobs(static_cast<size_t>(count));
    int encoded = 0;
    std::vector<std::string> missingSounds; // sounds with no WAV on disk and no engine export

    for (int i = 0; i < count; ++i)
    {
        // Preferred source: WAV already on disk at the standard layout
        char wavPath[MAX_PATH * 2];
        snprintf(wavPath, sizeof(wavPath),
                 "..\\packages\\sounds\\%s\\%s.wav",
                 sounds[i].pkg.c_str(), sounds[i].name.c_str());

        bool exportedTmpWav = false;

        if (GetFileAttributesA(wavPath) == INVALID_FILE_ATTRIBUTES)
        {
            // WAV not on disk — ask the engine to export the embedded data.
            // This is the same command the Sound Browser's Export button uses.
            char exportCmd[MAX_PATH * 2 + 128];
            snprintf(exportCmd, sizeof(exportCmd),
                     "OBJ EXPORT TYPE=SOUND PACKAGE=\"%s\" NAME=\"%s\" FILE=\"%s\"",
                     sounds[i].pkg.c_str(), sounds[i].name.c_str(), wavPath);
            ExecEditorCommand(exportCmd);
            exportedTmpWav = (GetFileAttributesA(wavPath) != INVALID_FILE_ATTRIBUTES);

            if (!exportedTmpWav)
            {
                missingSounds.push_back(sounds[i].pkg + "." + sounds[i].name);
                continue;
            }
        }

        // Temp OGG output (index-unique to avoid collisions)
        char oggTmp[MAX_PATH * 2];
        snprintf(oggTmp, sizeof(oggTmp),
                 "..\\packages\\sounds\\uas_tmp_%d.ogg", i);

        // Quote paths to handle directory names with spaces
        char cmd[MAX_PATH * 4];
        snprintf(cmd, sizeof(cmd),
                 "oggenc.exe -q %.1f \"%s\" -o \"%s\"",
                 compCtx.quality / 10.0, wavPath, oggTmp);

        STARTUPINFOA si        = {};
        PROCESS_INFORMATION pi = {};
        si.cb                  = sizeof(si);
        si.dwFlags             = STARTF_USESHOWWINDOW;
        si.wShowWindow         = SW_HIDE;
        if (CreateProcessA(nullptr, cmd,
                           nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 30000u);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        // Clean up temp WAV we exported (don't touch pre-existing ones)
        if (exportedTmpWav)
            DeleteFileA(wavPath);

        // Read back the OGG
        FILE* fp = nullptr;
        bool oggOk = false;
        if (fopen_s(&fp, oggTmp, "rb") == 0 && fp)
        {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0)
            {
                oggBlobs[static_cast<size_t>(i)].resize(static_cast<size_t>(sz));
                fread(oggBlobs[static_cast<size_t>(i)].data(), 1,
                      static_cast<size_t>(sz), fp);
                ++encoded;
                oggOk = true;
            }
            fclose(fp);
            DeleteFileA(oggTmp);
        }
        if (!oggOk)
            missingSounds.push_back(sounds[i].pkg + "." + sounds[i].name);
    }

    if (encoded == 0)
    {
        MessageBoxA(hBrowserWnd,
            "No sounds were encoded.\r\n\r\n"
            "Check that WAV source files exist at:\r\n"
            "  ..\\Packages\\Sounds\\<package>\\<soundname>.wav\r\n\r\n"
            "Also verify that oggenc.exe is in the editor's working\r\n"
            "directory or on the system PATH.",
            "Message", MB_OK);
        return;
    }

    // Step 2c: compute per-sound OGG offsets in the output file
    std::vector<int> oggOffsets(static_cast<size_t>(count), 0);
    {
        int pos = headerSize;
        for (int i = 0; i < count; ++i)
        {
            oggOffsets[static_cast<size_t>(i)] = pos;
            pos += static_cast<int>(
                       oggBlobs[static_cast<size_t>(i)].size());
        }
    }

    // Step 2d: write the UAS file
    //
    // Write to a temp path first, then rename over the final .uas.
    // This avoids an "access denied" error when the engine already has the
    // existing .uas open for streaming — we never try to open the live file
    // for writing.  MoveFileExA with MOVEFILE_REPLACE_EXISTING can swap the
    // file in even while it is held open for reading (provided the engine
    // opened it with FILE_SHARE_DELETE, which Unreal's streaming code does).
    char uasPath[MAX_PATH];
    snprintf(uasPath, sizeof(uasPath),
             "..\\packages\\sounds\\%s.uas", mapName);

    char uasTmp[MAX_PATH];
    snprintf(uasTmp, sizeof(uasTmp),
             "..\\packages\\sounds\\%s.uas.tmp", mapName);

    FILE* uasFp = nullptr;
    if (fopen_s(&uasFp, uasTmp, "wb") != 0 || !uasFp)
    {
        MessageBoxA(hBrowserWnd,
            "Failed to create UAS file.\r\n"
            "Check that ..\\Packages\\Sounds\\ is writable.",
            "Message", MB_OK);
        return;
    }

    // 4-byte little-endian sound count
    fwrite(&count, 4, 1, uasFp);

    // N × 0x44-byte header entries: char name[64] + DWORD ogg_offset
    char entryBuf[0x44];
    for (int i = 0; i < count; ++i)
    {
        memset(entryBuf, 0, sizeof(entryBuf));
        strncpy_s(entryBuf, 64, sounds[i].name.c_str(), _TRUNCATE);
        memcpy(entryBuf + 64, &oggOffsets[static_cast<size_t>(i)], 4);
        fwrite(entryBuf, sizeof(entryBuf), 1, uasFp);
    }

    // Concatenated OGG streams
    for (int i = 0; i < count; ++i)
    {
        const auto& blob = oggBlobs[static_cast<size_t>(i)];
        if (!blob.empty())
            fwrite(blob.data(), 1, blob.size(), uasFp);
    }

    fclose(uasFp);

    // Atomically replace the live .uas with the newly built temp file.
    // If the rename fails (engine holds an exclusive lock), leave the temp
    // on disk and tell the user where it is.
    if (!MoveFileExA(uasTmp, uasPath, MOVEFILE_REPLACE_EXISTING))
    {
        char errMsg[MAX_PATH + 128];
        snprintf(errMsg, sizeof(errMsg),
                 "UAS encoded but could not replace the live file.\r\n\r\n"
                 "The new UAS was saved as:\r\n  %s\r\n\r\n"
                 "Close the editor and rename it manually.",
                 uasTmp);
        MessageBoxA(hBrowserWnd, errMsg, "Message", MB_OK);
        return;
    }

    // Completion notice
    std::string msg = "UAS successfully encoded.";
    if (!missingSounds.empty())
    {
        msg += "\r\n\r\nMissing source file for:";
        for (const auto& s : missingSounds)
        {
            msg += "\r\n  ";
            msg += s;
        }
    }
    MessageBoxA(hBrowserWnd, msg.c_str(), "Message", MB_OK);
}



// SB_HandleExport
// Supports multi-selection: collects all selected items, then chains a
// Save dialog for each one in order.  Cancelling any dialog stops the chain.
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

    // Package name is the same for every sound in the current browser view
    char pkgName[256] = {};
    GetWindowTextA(GetPackageComboHWND(this_ptr), pkgName, sizeof(pkgName));

    // Chain through each selected sound — cancel stops the chain
    for (int i = 0; i < selCount; ++i)
    {
        // Get sound name from column 0
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
            break;  // user cancelled — stop the chain

        // OBJ EXPORT TYPE=SOUND PACKAGE="ACT" NAME="Menu_navig_1" FILE="C:\...\Menu_navig_1.wav"
        // (PACKAGE= required - confirmed from UT2004 BrowserSound source)
        char cmd[MAX_PATH * 2 + 256];
        snprintf(cmd, sizeof(cmd),
                 "OBJ EXPORT TYPE=SOUND PACKAGE=\"%s\" NAME=\"%s\" FILE=\"%s\"",
                 pkgName, soundName, filePath);
        ExecEditorCommand(cmd);
    }
}


//  SB_HandleImport 
// Matches UT2004 batch-import UX:
//   1. Open file picker (multi-select enabled).
//   2. For each selected WAV, show the engine's Import Sound dialog (resource
//      156) pre-filled with path, package, group, and a name derived from the
//      filename.  The user can edit any field.
//   3. Dialog result controls the loop:
//        OK      > import this file, show dialog for next
//        OK All  > import this file and all remaining with the same settings
//                  (no dialog shown for remaining files)
//        Skip    > skip this file, show dialog for next
//        Cancel  > stop importing
//   4. After the loop, refresh the list once if anything was imported.
static void __cdecl SB_HandleImport(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);
    HWND hGrpCombo = GetGroupComboHWND(this_ptr);

    // Read current package / group from the browser combos
    char defPkg[256] = {};
    char defGrp[256] = {};
    GetWindowTextA(hPkgCombo, defPkg, sizeof(defPkg));
    GetWindowTextA(hGrpCombo, defGrp, sizeof(defGrp));

    //  Step 1: multi-file open dialog 
    // Buffer must be large enough for many paths; use static to avoid a big
    // stack allocation.  Zero it each call so stale data never bleeds through.
    static char fileBuffer[32768];
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

    //  Step 2: parse the file list 
    // Single-file result:   "C:\dir\file.wav\0"
    // Multi-file result:    "C:\dir\0file1.wav\0file2.wav\0\0"
    // Detect multi-select: the character after the first null is non-null.
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

    //  Step 3: loop 
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

        // Build full path
        char fullPath[MAX_PATH * 2] = {};
        if (multiSel)
            snprintf(fullPath, sizeof(fullPath), "%s%s", dirBuf, cursor);
        else
            strncpy_s(fullPath, cursor, _TRUNCATE);

        // Set up import data for this file
        ImportSoundData data = {};
        strncpy_s(data.filePath, fullPath,  _TRUNCATE);
        strncpy_s(data.pkgName,  lastPkg,   _TRUNCATE);
        strncpy_s(data.grpName,  lastGrp,   _TRUNCATE);

        // Derive sound name from filename (strip path and extension)
        const char* fileOnly = fullPath;
        for (const char* q = fullPath; *q; ++q)
            if (*q == '\\' || *q == '/') fileOnly = q + 1;
        strncpy_s(data.soundName, sizeof(data.soundName), fileOnly, _TRUNCATE);
        char* dot = strrchr(data.soundName, '.');
        if (dot) *dot = '\0';

        // Show dialog (skip when OK All was already pressed)
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
            break;                          // Cancel > stop

        if (dlgResult == IDB_IMPORT_SKIP)
        {
            // Advance and continue
            if (!multiSel) break;
            cursor += strlen(cursor) + 1;
            continue;
        }

        if (dlgResult == IDB_IMPORT_OKALL)
            okAllMode = true;               // OK All > import this + remaining silently

        // Package is required
        if (data.pkgName[0] == '\0')
        {
            if (!multiSel) break;
            cursor += strlen(cursor) + 1;
            continue;
        }

        // Carry settings forward for subsequent OK All imports
        strncpy_s(lastPkg, data.pkgName, _TRUNCATE);
        strncpy_s(lastGrp, data.grpName, _TRUNCATE);

        // Execute AUDIO IMPORT
        // Convert the file path to its short (8.3) form so that spaces in the
        // directory name don't confuse the UE2 command parser.
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
            break;  // single-file: done after one import

        cursor += strlen(cursor) + 1;   // advance to next filename
    }

    if (anyImported)
    {
        RefreshList(this_ptr);
        // Navigate to the last-imported sound's package/group
        NavigateToPackageGroup(this_ptr, lastPkg, lastGrp);
    }
}


//  SB_HandleDelete 
// Confirmed from UT2004 BrowserSound source:
//   appSprintf( cmd, "DELETE CLASS=SOUND OBJECT=\"%s\"", *Name );
//   GUnrealEd->Get( "OBJ", cmd, GetPropResult );
//   if( !GetPropResult.Len() ) RefreshSoundList(); else appMsgf(error)
//
// No confirmation dialog - delete is reversible by not saving the package.
// We pass GLog as the output device (we can't easily construct an
// FStringOutputDevice from outside the engine), so we can't read the error
// text directly.  Instead we detect failure by refreshing the list and
// checking whether the sound is still present: if it is, the engine rejected
// the delete (sound is referenced / in use) and we show an error.
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

    // DELETE CLASS=SOUND OBJECT="name"
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "DELETE CLASS=SOUND OBJECT=\"%s\"", soundName);
    CallEditorGet("OBJ", cmd);

    // Read package / group for the error message
    char pkgName[256] = {};
    char grpName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    GetWindowTextA(hGrpCombo, grpName, sizeof(grpName));

    // Build fully-qualified name: Package.Group.Sound  (or Package.Sound)
    char fullName[768] = {};
    if (grpName[0])
        snprintf(fullName, sizeof(fullName), "%s.%s.%s", pkgName, grpName, soundName);
    else
        snprintf(fullName, sizeof(fullName), "%s.%s", pkgName, soundName);

    // Refresh the list, then check if the sound is still present.
    // If it is, the engine rejected the delete (sound is referenced/in use).
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

    // Delete succeeded - if the current group/package is now empty, navigate away
    NavigateAfterDelete(this_ptr);
}


//  SB_HandleRename 
// Confirmed from UT2004 BrowserSound source:
//   GUnrealEd->Exec("OBJ RENAME OLDNAME=... NEWNAME=...")
//
// Uses engine IDDIALOG_RENAME (19805) - the same dialog as Texture / StaticMesh
// rename.  Template provides Package / Group / Name edits and OK + Cancel only.
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
    // Navigate to the renamed sound's new package/group
    NavigateToPackageGroup(this_ptr, data.newPackage, data.newGroup);
}


//  StoreLParamHelper
// Called from the StoreSoundLParam hook whenever the engine inserts a sound into
// the ListView.  Stores the USound* as lParam, and also caches the display name
// so that GetSoundName can resolve cross-package references later.
static void __cdecl StoreLParamHelper(void* this_ptr, int item_index,
                                       void* usound_ptr, const char* name)
{
    void* list_obj = *reinterpret_cast<void**>(static_cast<char*>(this_ptr) + 0x94);
    HWND  hwndList = *reinterpret_cast<HWND*>(static_cast<char*>(list_obj)  + 4);

    LVITEMA lvi  = {};
    lvi.mask     = LVIF_PARAM;
    lvi.iItem    = item_index;
    lvi.iSubItem = 0;
    lvi.lParam   = reinterpret_cast<LPARAM>(usound_ptr);
    SendMessageA(hwndList, LVM_SETITEM, 0, reinterpret_cast<LPARAM>(&lvi));

    // Cache the name for future cross-package lookups.
    if (usound_ptr && name && name[0] != '\0')
        g_soundNameCache[usound_ptr] = name;
}


//  GetSelectedSound 
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


//  SoundPropsDlgProc 
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

        //  Flag checkboxes 
        // IDC_CHK_STREAM → SF_UAS_STREAM (0x4): marks sound as OGG/UAS external stream stub.
        // The original code incorrectly used SF_STREAM (0x100) which is the Random type bit.
        CheckDlgButton(hDlg, IDC_CHK_STREAM, (flags & SF_UAS_STREAM) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_2D,     (flags & SF_2D)     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_LOOP,   (flags & SF_LOOP)   ? BST_CHECKED : BST_UNCHECKED);
        // 0x4000 = Xbox HD Stream (HDS sounds hidden from PC browser by Ubisoft).
        // 0x2000 = Surround (4.0/5.1); 0x200 = unknown, dead in PC editor DLL.
        CheckDlgButton(hDlg, IDC_CHK_XBOXHD,   (flags & SF_XBOXHD_STREAM) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_SURROUND,  (flags & SF_TYPE_SURROUND) ? BST_CHECKED : BST_UNCHECKED);

        //  Override Radius 
        bool hasRadius = (radius > 0.0f);
        CheckDlgButton(hDlg, IDC_CHK_OVR_RADIUS,
                       hasRadius ? BST_CHECKED : BST_UNCHECKED);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4f", radius);
            SetDlgItemTextA(hDlg, IDC_EDIT_RADIUS, buf);
        }

        //  Override Volume 
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

                applyFlag(SF_UAS_STREAM, IDC_CHK_STREAM);  // was SF_STREAM(0x100)=Random type bug
                applyFlag(SF_2D,        IDC_CHK_2D);
                applyFlag(SF_LOOP,      IDC_CHK_LOOP);
                applyFlag(SF_XBOXHD_STREAM, IDC_CHK_XBOXHD);   // 0x4000 Xbox HD Stream (.hds, hidden from PC browser)
                applyFlag(SF_TYPE_SURROUND, IDC_CHK_SURROUND); // 0x2000 4.0/5.1 surround stream

                //  Override Radius 
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

                //  Override Volume
                // If the checkbox is ticked but the value is exactly 0.0 dB
                // (unity, no modifier applied), silently clear the flag – it's
                // cleaner than serialising a no-op override into the package.
                if (IsDlgButtonChecked(hDlg, IDC_CHK_OVR_VOLUME) == BST_CHECKED)
                {
                    char buf[32] = {};
                    GetDlgItemTextA(hDlg, IDC_EDIT_VOLUME, buf,
                                    static_cast<int>(sizeof(buf)));
                    float db = static_cast<float>(atof(buf));
                    if (db == 0.0f)
                    {
                        flags &= ~SF_OVR_VOLUME;   // 0 dB = unity; disable override
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
                    *pVolume = 1.0f;   // 0 dB / unity when override is off
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


//  RenameSoundDlgProc 
// Dialog proc for IDDIALOG_RENAME (19805) - the same dialog used by the
// Texture and StaticMesh browsers.  lParam = RenameSoundData* pre-filled by
// SB_HandleRename.  Control IDs match the engine template:
//   IDEC_NEWPACKAGE (1066) - Package edit
//   IDEC_NEWGROUP   (1067) - Group edit
//   IDEC_NAME       (1065) - Name edit
//   IDOK (1) / IDCANCEL (2) - only buttons present in this template
static INT_PTR CALLBACK RenameSoundDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        RenameSoundData* d = reinterpret_cast<RenameSoundData*>(lParam);
        if (!d) return TRUE;

        // Pre-fill each edit with the current value so user edits only what changed
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


//  ImportSoundDlgProc 
// Dialog proc for the engine's Import Sound dialog (resource 156).
// lParam = ImportSoundData* - pre-filled by SB_HandleImport before the call.
// On IDOK the edit fields are written back into the struct so the caller
// can build the AUDIO IMPORT command.
// "OK All" (3) behaves like OK (single-file context).
// "Skip" (4) behaves like Cancel.
static INT_PTR CALLBACK ImportSoundDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        ImportSoundData* d = reinterpret_cast<ImportSoundData*>(lParam);
        if (!d) return TRUE;

        // Fill the static file-path display (control 1068) and the three edit fields
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
// ============================================================
// SB_ShowFlagsDump
//
// Iterates the engine's GObjects array to find EVERY UObject whose top-level
// Outer name matches the currently selected UAX package.  For each one it
// reads the flags DWORD at +0x60 and classifies the sound type.  Objects that
// carry flags 0x4 or 0x4000 are marked [HIDDEN] — these are the "delisted"
// sounds that the Sound Browser's display function (FUN_10e7db38) skips via
// an early-exit branch and which therefore never appear in the browser list.
//
// Output is written to %TEMP%\scct_flag_dump.txt and opened in Notepad so the
// user can scroll, search with Ctrl-F, or copy the results.
//
// Trigger: hold Shift while pressing the Properties button / menu item.
//
// Name resolution uses GNames directly (no engine call):
//   GNames.Data = *(int*)0x1169cfbc   (base of FNameEntry* array)
//   FNameEntry string at FNameEntry + 0x0C
//   FName.Index at UObject + 0x20
//
// Outer chain traversal uses UObject+0x18; traversal stops at depth 16 to
// guard against corrupted Outer loops.
// ============================================================

// Safe FName → char* lookup.  Returns a static "<error>" string on any failure
// so the caller can always treat the return value as a valid C string.
static const char* SB_ReadFName(void* pObj)
{
    if (!pObj) return "<null>";

    // FName.Index is at UObject+0x20 (confirmed from FUN_10fa5600 disassembly)
    int fnameIdx = *reinterpret_cast<int*>(static_cast<char*>(pObj) + UOBJ_FNAME_OFFSET);

    // Bounds-check against GNames.Num
    int gnamesNum = *reinterpret_cast<int*>(GNAMES_NUM);
    if (fnameIdx < 0 || fnameIdx >= gnamesNum)
        return "<badIdx>";

    // GNames.Data holds the base pointer of the FNameEntry* array
    int gnamesBase = *reinterpret_cast<int*>(GNAMES_DATA);
    if (!gnamesBase) return "<noGNames>";

    // Dereference: FNameEntry* = GNames.Data[fnameIdx]
    int fnameEntry = *reinterpret_cast<int*>(gnamesBase + fnameIdx * 4);
    if (!fnameEntry) return "<nullEntry>";

    // FNameEntry.Name string starts at FNameEntry+0x0C
    return reinterpret_cast<const char*>(fnameEntry + FNAME_ENTRY_STR_OFFSET);
}

static void __cdecl SB_ShowFlagsDump(void* this_ptr)
{
    HWND hParent   = GetParentHWND(this_ptr);
    HWND hPkgCombo = GetPackageComboHWND(this_ptr);

    // Current package name from the combo edit field
    char pkgName[256] = {};
    GetWindowTextA(hPkgCombo, pkgName, sizeof(pkgName));
    if (pkgName[0] == '\0')
    {
        return;
    }

    // GObjects array
    void** gobjData  = *reinterpret_cast<void***>(GOBJECTS_DATA);
    int    gobjCount = *reinterpret_cast<int*>   (GOBJECTS_NUM);
    if (!gobjData || gobjCount <= 0)
    {
        MessageBoxA(hParent, "GObjects is empty or unavailable.",
                    "Message", MB_OK);
        return;
    }

    // Build report text
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
                pTopOuter = pCur;   // pCur has no Outer → it IS the package
                break;
            }
            pCur = pOuter;
        }

        // Skip if package resolution failed or object IS the package
        if (!pTopOuter || pObj == pTopOuter) continue;

        // Filter by current package name (case-insensitive)
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

        // ---- Read USound flags at +0x60 ----
        DWORD flags = *reinterpret_cast<DWORD*>(
            static_cast<char*>(pObj) + USOUND_FLAGS_OFFSET);

        // ---- Classify ----
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
            // UAS streaming stub — actual OGG audio lives in the map's .uas file.
            // No WAV data in the UAX; Export will not produce usable audio.
            typeName = "Wave[UASStream]";
            isHidden = true;
        }
        else                               { typeName = "Wave";       nWave++;   }

        if (isHidden) nHidden++;
        nTotal++;

        // ---- Format line ----
        {
            char line[256];
            snprintf(line, sizeof(line),
                     "%-6d  %-40s  0x%08X  %s\r\n",
                     i, dispName, static_cast<unsigned>(flags), typeName);
            text += line;
        }
    }

    // ---- Summary ----
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

    // ---- Write to %TEMP%\scct_flag_dump.txt ----
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

    // ---- Open in Notepad (async, non-blocking) ----
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


//  ShowPropertiesDialogHelper
// Normal use: opens the Sound Properties dialog for the selected sound.
// Shift held:  runs SB_ShowFlagsDump — enumerates ALL USound objects in the
//              current package (bypassing the browser filter) and writes a
//              report to a temp file, then opens it in Notepad.  This reveals
//              any "delisted" sounds (flags 0x4 or 0x4000) that would not
//              appear in the Sound Browser under normal operation.
static void __cdecl ShowPropertiesDialogHelper(void* this_ptr)
{
#ifdef _DEBUG
    // Shift held → flag dump (debug builds only)
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


// ============================================================
// Composite sound helpers
// ============================================================

// WriteSilentWAV
// Writes a minimal valid 1-sample silent mono 8-bit 8000 Hz WAV to a temp file.
// Used by SB_HandleNew to give AUDIO IMPORT something real to chew on.
// The tiny WAV data is effectively ignored for Random/Sequence resources; only
// the USound object wrapper and the flags we set afterwards matter.
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


// GetSoundName
// Looks up the display name for a USound* by scanning the browser's ListView.
// If the sound is not currently shown (different package/group), returns "<???>"
// with the pointer value so the child is still identifiable.
static void GetSoundName(void* pBrowser, void* pSound, char* outBuf, int maxLen)
{
    outBuf[0] = '\0';
    if (!pSound || !pBrowser) return;

    // Primary: scan the currently visible ListView (works when the sound is in
    // the currently filtered group/package).
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

    // Fallback: check the global name cache populated by StoreLParamHelper.
    // This resolves cross-package or cross-group references that aren't in the
    // current filtered ListView but were visible at some earlier point in the session.
    auto it = g_soundNameCache.find(pSound);
    if (it != g_soundNameCache.end())
    {
        _snprintf_s(outBuf, maxLen, _TRUNCATE, "%s", it->second.c_str());
        return;
    }

    // Last resort: show pointer so the child entry still appears in the list.
    snprintf(outBuf, maxLen, "<?? %p>", pSound);
}


// PopulateComboWithSounds
// Fills a combo box with all sound names from the browser's current list.
// Skips excludeSound (the parent composite itself, to avoid self-reference).
// CB_GETITEMDATA on each entry returns the associated USound*.
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


// FindSoundByName
// Scans the browser ListView for an item whose column-0 text matches name
// (case-insensitive) and returns its lParam (USound*).  Returns NULL if not found.
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


// ============================================================
// stRes linked-list insert helpers
// These are called only when rebuilding the stRes on dialog OK.
// ============================================================

// RandList_Insert – appends a RandomElement to the tail of the Random stRes list.
// elem layout: +0x00=child, +0x04=unused, +0x08=weight, +0x0C=prev, +0x10=next, +0x14=parent
// stRes layout: +0x14=first, +0x18=last, +0x1C=count
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


// ============================================================
// Random props dialog helpers
// ============================================================

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

    // The engine normalises by the actual sum of weights, so there is no silent gap
    // from imperfect summation.  "SetProbNothing" is a separate explicit field in
    // Ubisoft's CSB format that does not exist in the UAX binary structure.
    // Show the actual sum so the user can verify relative proportions.
    char silBuf[128] = {};
    float silence = (totalWeight < 10000) ? ((10000 - totalWeight) / 100.0f) : 0.0f;
    snprintf(silBuf, sizeof(silBuf), "Probability of silence: %.1f%%", silence);
    SetDlgItemTextA(hDlg, IDC_RAND_SILENCE_LBL, silBuf);
}


// ============================================================
// Sequence props dialog helpers
// ============================================================

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


// ============================================================
// Persists the last-chosen type across invocations of dialog 142.
static int s_newDlgLastType = IDC_RB_RANDOM;

// CreateResourceDlgProc – dialog 142 "New Sound"
//   lParam = NewResourceCtx*
//   On IDOK: fills ctx->name and ctx->typeFlag (SF_TYPE_RANDOM/SEQUENCE/SWITCH).
// ============================================================
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


// ============================================================
// SB_HandleNew
// Unified "File > New..." handler.
//   1. Show dialog 142 to get name + type (Random / Sequence / Switch).
//   2. Write a silent WAV to %TEMP%.
//   3. AUDIO IMPORT it into the current package/group.
//   4. Navigate to the new sound.
//   5. Find the USound* by name in the refreshed ListView.
//   6. Set the composite type flag chosen in the dialog.
//   7. Allocate TArray sentinels as required by the type.
// ============================================================
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

    // 1. Get name + type from user
    NewResourceCtx ctx = {};
    HINSTANCE hExe = GetModuleHandleA(NULL);
    if (DialogBoxParamA(hExe, MAKEINTRESOURCEA(IDD_CREATE_RESOURCE), hParent,
                        CreateResourceDlgProc,
                        reinterpret_cast<LPARAM>(&ctx)) != IDOK
        || ctx.name[0] == '\0')
        return;

    DWORD typeFlag    = ctx.typeFlag;
    const char* soundName = ctx.name;

    // 2. Write silent WAV to temp
    char wavPath[MAX_PATH * 2] = {};
    if (!WriteSilentWAV(wavPath, sizeof(wavPath)))
    {
        MessageBoxA(hParent, "Failed to write temporary WAV file.", title,
                    MB_OK);
        return;
    }

    // 3. AUDIO IMPORT (use 8.3 short path to avoid spaces confusing UE2 parser)
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

    // Clean up temp file (best effort)
    DeleteFileA(wavPath);

    // 4. Navigate to the new package/group so RefreshList picks it up
    NavigateToPackageGroup(this_ptr, pkg, grp);

    // 5. Find the newly imported USound* by name
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

    // 6. Set composite type flag
    char*  ps     = static_cast<char*>(pSound);
    DWORD* pFlags = reinterpret_cast<DWORD*>(ps + USOUND_FLAGS_OFFSET);
    *pFlags |= typeFlag;

    // 7. Initialise both TArrays that USound::Serialize (FUN_11117a70) expects.
    //   +0x6c  TArray<USound*>: Data=NULL, Num=0, Max=0  (empty – no children yet)
    //   +0x78  TArray<int>:     see below
    //
    // SWITCH CRASH NOTE (originally hit when SF_TYPE_SWITCH was wrongly set to 0x2000):
    //   The HD Surround branch (0x2000) in AddListSoundsString (FUN_10e7db38)
    //   unconditionally dereferences TArray<int>.Data as a byte pointer.
    //   The real Switch flag (0x80) does NOT dereference TArray<int>.Data in the
    //   display path, so this sentinel is no longer strictly required for Switch.
    //   Kept here as a defensive measure: if Data is ever NULL when the engine's
    //   Serialize or GetSwitchSound runs, a valid zero value is safer than NULL.
    //   For Random/Sequence, NULL is fine — their display path only reads the count.
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

    // 8. Refresh the list now that the type flag is set so the sound immediately
    //    shows as "Random" / "Sequence" rather than "Wave".  NavigateToPackageGroup
    //    called above populated the list *before* step 6 wrote the flag, so the row
    //    would have appeared as a plain Wave without this second refresh.
    RefreshList(this_ptr);
}

// ============================================================
// RandomElemDlgProc – dialog 146 "Random element properties"
//   lParam = RandomElemCtx*
//   Combo  (1005): picks child USound*
//   Edit   (1288): play weight integer (1-10000, Ubisoft scale: 10000 = 100%)
// ============================================================
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

        // Populate combo with all sounds in the current browser list
        HWND hCombo = GetDlgItem(hDlg, IDC_RELEM_COMBO);
        PopulateComboWithSounds(ec->pBrowser, hCombo, ec->pParent);

        // Pre-select the current child if any
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

        // Pre-fill weight edit.  Display as percentage with 2 decimal places
        // (e.g. 3333 internal → "33.33"); internal storage is 0-10000 (Ubisoft scale).
        // New elements default to 100%.
        char wBuf[16] = {};
        float dispWeight = (ec->nWeight > 0) ? (ec->nWeight / 100.0f) : 100.0f;
        snprintf(wBuf, sizeof(wBuf), "%.2f", dispWeight);
        SetDlgItemTextA(hDlg, 1288, wBuf);   // IDC_RELEM_WEIGHT = 1288
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
                ec->nWeight = static_cast<int>(pct * 100.0f + 0.5f);  // round to nearest
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


// ============================================================
// RandomPropsDlgProc – dialog 145 "Random"
//   lParam = CompositePropsCtx*
//   Reads existing children from stRes linked list on open;
//   rebuilds the stRes on OK.
// ============================================================
static INT_PTR CALLBACK RandomPropsDlgProc(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* pctx = reinterpret_cast<CompositePropsCtx*>(lParam);

        // Allocate working context (stored in GWL_USERDATA; freed on close)
        auto* ctx = static_cast<RandomPropsCtx*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RandomPropsCtx)));
        if (!ctx) return TRUE;
        ctx->pSound   = pctx->pSound;
        ctx->pBrowser = pctx->pBrowser;
        SetWindowLongA(hDlg, GWL_USERDATA, reinterpret_cast<LONG>(ctx));

        // Set up ListView columns: Sound (140px) | Weight (50px)
        HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 140; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 50;  col.pszText = const_cast<char*>("Weight");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));

        // Read existing child list from TArray<USound*> at +0x6c and TArray<int> at +0x78.
        // These are the flat arrays that USound::Serialize (FUN_11117a70) reads directly.
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
        // ---- Insert resource ----
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
                // If the sound is already in the pool, remove the old entry first
                // so the new one (potentially with a different weight) replaces it.
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

        // ---- Remove resource ----
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

        // ---- Edit resource ----
        case IDC_RAND_EDIT:
        {
            HWND hList = GetDlgItem(hDlg, IDC_RAND_LIST);
            int  sel   = static_cast<int>(
                SendMessageA(hList, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (sel >= 0)
            {
                // Read back lParam to get the actual children[] index (auto-sorted list).
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

        // ---- Set equal probabilities ----
        // Ubisoft convention: 10000 = 100%.  Divide evenly (e.g. 2 sounds → 5000 each,
        // 3 sounds → 3333 each), matching the values observed in original UAX files.
        case IDC_RAND_EQUAL_PROB:
        {
            int equalWeight = (ctx->nCount > 0) ? (10000 / ctx->nCount) : 10000;
            for (int i = 0; i < ctx->nCount; i++)
                ctx->children[i].nWeight = equalWeight;
            RandProps_RefreshList(hDlg, ctx);
            return TRUE;
        }

        // ---- OK: write flat TArrays that USound::Serialize (FUN_11117a70) expects ----
        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);
            int   n  = ctx->nCount;

            // Allocate flat arrays: void*[] for children, int[] for weights.
            // We do NOT free the old arrays — they may have been set by the engine
            // or a prior IDOK, and freeing across allocators is unsafe.  Small
            // editor-session leak; acceptable.
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
                    // Allocation failed — clean up and bail without touching USound
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
            // Write TArray<int> at +0x78
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


// ============================================================
// SeqElemDlgProc – dialog 19838 "Sequence element properties"
//   lParam = SeqElemCtx*
//   Combo  (1005): picks child USound*
//   Edit   (1346): repeat count
//   Check  (1347): timed loop
// ============================================================
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
                // Clamp to 13-bit value range — bit 13 and above are flags,
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


// ============================================================
// SeqPropsDlgProc – dialog 19837 "Sequence"
//   lParam = CompositePropsCtx*
// ============================================================
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

        // Set up ListView columns: Sound | Repeat | Timed Loop
        // Repeat shows "3x" for normal repeats, "10s" for timed-loop seconds.
        HWND hList = GetDlgItem(hDlg, IDC_SEQ_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 115; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 38;  col.pszText = const_cast<char*>("Repeat");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));
        col.cx   = 42;  col.pszText = const_cast<char*>("Timed Loop");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 2, reinterpret_cast<LPARAM>(&col));

        // Read existing child list from TArray<USound*> at +0x6c and TArray<int> at +0x78.
        // Ubisoft's Serialize (FUN_11117a70) confirmed: both Random AND Sequence store
        // children in the flat TArray<USound*> (count at +0x70), and per-element data
        // (weights/repeat counts) in TArray<int> at +0x78.  Reading stRes at +0x74 was
        // wrong – that field is TArray<USound*>.Max, typically the integer 2, not a pointer.
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
        // ---- Insert resource ----
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

        // ---- Remove resource ----
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

        // ---- Edit resource ----
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

        // ---- Move Up ----
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

        // ---- Move Down ----
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

        // ---- OK: write TArray<USound*> + TArray<int> (repeat counts) ----
        // Ghidra decompile of FUN_11117a70 (USound::Serialize) confirmed:
        // both Random AND Sequence store children in a flat TArray<USound*> at
        // +0x6c and per-element data (weights / repeat counts) in TArray<int>
        // at +0x78.  The old stRes linked-list approach was wrong and caused a
        // GPF when opening Ubisoft-authored Sequence assets.
        case IDOK:
        {
            char* ps = static_cast<char*>(ctx->pSound);
            int   n  = ctx->nCount;

            // Allocate flat arrays: void*[] for children, int[] for repeat counts.
            // We do NOT free the old arrays — they may have been set by the engine
            // or a prior IDOK, and freeing across allocators is unsafe.  Small
            // editor-session leak; acceptable.
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
                    // Allocation failed — clean up and bail without touching USound
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
                        // Set timed-loop flag; lower 13 bits = duration in seconds
                        repeatArr[i] = SEQ_TIMED_LOOP_FLAG | (ctx->children[i].nRepeat & SEQ_VALUE_MASK);
                    else
                        // cap is enforced in SeqElemDlgProc IDOK (max SEQ_VALUE_MASK = 0x1FFF)
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


// ============================================================
// ESurfaceType name table (UnrealScript, YD 07/10/2002)
// Index matches enum ordinal; used to populate IDC_SWELEM_SURFACE combobox.
// ============================================================
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


// ============================================================
// SwitchElemDlgProc – dialog 148 "Switch element properties"
//   lParam = SwitchElemCtx*
//   Combo  (1005): picks child USound*
//   Combo  (1343): picks ESurfaceType (0-25)
// ============================================================
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

        // Populate sound combo
        HWND hSoundCombo = GetDlgItem(hDlg, IDC_SWELEM_COMBO);
        PopulateComboWithSounds(ec->pBrowser, hSoundCombo, ec->pParent);

        // Pre-select current child sound if editing
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

        // Populate surface type combo with all 26 ESurfaceType entries
        HWND hSurfCombo = GetDlgItem(hDlg, IDC_SWELEM_SURFACE);
        for (int i = 0; i < SW_NUM_SURFACE_TYPES; i++)
            SendMessageA(hSurfCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kSurfaceTypeNames[i]));

        // Pre-select current surface type
        int selSurf = (ec->nSurfaceType >= 0 &&
                       ec->nSurfaceType < SW_NUM_SURFACE_TYPES)
                      ? ec->nSurfaceType : 0;
        SendMessageA(hSurfCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selSurf), 0);

        // Fix dropdown height: user-created comboboxes in Resource Hacker often
        // have their total height set to just the edit-field height (~12 DLUs),
        // which leaves no space for the drop-down list.  Resize the window now so
        // the drop-down portion is tall enough to show ~12 visible items.
        {
            RECT rc;
            GetWindowRect(hSurfCombo, &rc);
            int editH = rc.bottom - rc.top;   // natural single-line height in px
            // Aim for edit field + 12 rows.  CB_GETITEMHEIGHT returns item height.
            LRESULT itemH = SendMessageA(hSurfCombo, CB_GETITEMHEIGHT, 0, 0);
            if (itemH <= 0) itemH = 15;       // fallback if control not yet sized
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
                // Read selected sound
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

                // Read selected surface type
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


// ============================================================
// SwitchProps_RefreshList – redraws IDC_SW_LIST for SwitchPropsCtx.
//   Columns: Sound (140px) | Surface Type (130px)
// ============================================================
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
        // Copy surface name to a local buffer so pszText is never a const literal ptr
        char surfBuf[64] = {};
        if (ci.nSurfaceType >= 0 && ci.nSurfaceType < SW_NUM_SURFACE_TYPES)
            strncpy_s(surfBuf, sizeof(surfBuf),
                      kSurfaceTypeNames[ci.nSurfaceType], _TRUNCATE);
        else
            strncpy_s(surfBuf, sizeof(surfBuf), "?", _TRUNCATE);

        // Column 0 – sound name
        LVITEMA lvi = {};
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = const_cast<char*>(ci.szName);
        lvi.lParam   = static_cast<LPARAM>(i);
        LRESULT insertedIdx = SendMessageA(hList, LVM_INSERTITEMA, 0,
                                           reinterpret_cast<LPARAM>(&lvi));

        // Column 1 – surface type name
        // Use LVM_SETITEMTEXTA (wParam = item index) rather than LVM_SETITEMA
        // so the message unambiguously targets the sub-item text.
        LVITEMA sub = {};
        sub.iSubItem = 1;
        sub.pszText  = surfBuf;
        SendMessageA(hList, LVM_SETITEMTEXTA,
                     static_cast<WPARAM>(insertedIdx),
                     reinterpret_cast<LPARAM>(&sub));
    }
}


// ============================================================
// SwitchPropsDlgProc – dialog 147 "Switch"
//   lParam = CompositePropsCtx*
//   Reads existing children from TArray<USound*>+TArray<int> on open;
//   rebuilds them on OK.
// ============================================================
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

        // Set up ListView columns: Sound (140px) | Surface Type (130px)
        HWND hList = GetDlgItem(hDlg, IDC_SW_LIST);
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 140; col.pszText = const_cast<char*>("Sound");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));
        col.cx   = 130; col.pszText = const_cast<char*>("Surface Type");
        SendMessageA(hList, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&col));

        // Read existing children from TArray<USound*> at +0x6c and TArray<int> at +0x78.
        // TArray<int>[i] stores the ESurfaceType enum value for child i.
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
        // ---- Insert resource ----
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
                // Replace existing entry for this surface type if one already exists
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
                // New surface type entry
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

        // ---- Remove resource ----
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

        // ---- Edit resource ----
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

        // ---- OK: write flat TArrays ----
        // TArray<USound*> at +0x6c: child sounds.
        // TArray<int>     at +0x78: parallel ESurfaceType values.
        // Shared count    at +0x70.
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


// ============================================================
// SB_HandleRandomProps / SB_HandleSwitchProps / SB_HandleSeqProps
// Entry points called by SoundBrowserCommands hook.
// ============================================================
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


//  Public initialiser

void SoundBrowser::Initialize()
{
    //  Patch 1: SAVEPACKAGE command handler extension guard 
    // FUN_11011bf0 checks the FILE= path against 7 allowed extensions
    // (.utx/.usx/.sdc/-i.utc/.sds/.upx/-i.utx) via strstr.  If none match,
    // a 6-byte near JZ at 0x11011EC7 jumps to LAB_110125f6 (no save).
    // .uax is not in that list, so it bailed before even calling SavePackage.
    //
    //   Before:  0F 84 2D 07 00 00   JZ  0x110125F6  (skip save)
    //   After:   90 90 90 90 90 90   NOP x6          (always continue)
    static const uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x11011EC7, nop6, sizeof(nop6));

    //  Patch 2: UObject::SavePackage early-return guard
    // FUN_10fb2610 (UObject::SavePackage) has its own extension check at entry.
    // It calls strstr against the same 7 extensions.  If none match, execution
    // falls through to:
    //
    //   10fb2757: B8 01 00 00 00   MOV EAX, 1
    //   10fb275c: ...epilog...
    //   10fb276c: C3               RET          <- returns without serialising!
    //
    // Immediately after the RET, 0x10fb276d is the filename-whitelist/save block
    // that continues to the actual linker serialisation when no name matches.
    //
    // Fix: replace "MOV EAX,1" (5 bytes) with a near JMP to 0x10fb276d.
    //   Before:  B8 01 00 00 00   MOV EAX, 1
    //   After:   E9 11 00 00 00   JMP 0x10fb276d
    //
    // Offset: next_ip=0x10fb275c, target=0x10fb276d, rel=0x11 (correct)
    static const uint8_t jmpSave[] = { 0xE9, 0x11, 0x00, 0x00, 0x00 };
    MemoryWriter::WriteBytes(0x10fb2757, jmpSave, sizeof(jmpSave));

    //  Patch 3: AddListSoundsString – show streaming/delisted sounds in the browser
    //
    // FUN_10e7db38 (AddListSoundsString / WBrowserSound::AddListSoundsString) is
    // called once per USound returned by the engine's QUERY command.  For plain-wave
    // sounds it performs two early-exit checks that silently drop sounds from the
    // ListView without adding them.  Both checks are for non-standard streaming types
    // that Ubisoft apparently excluded from the browser UI.
    //
    // Check A – flag 0x4 (PS2-style stream / background music stream):
    //   10e7dc6f: MOV EAX, [EBX+0x60]    ; load flags
    //   10e7dc72: TEST AL, 0x4            ; test bit 2
    //   10e7dc74: JZ  0x10e7dc98          ; if 0x4 NOT set → skip to 0x4000 check
    //   10e7dc76: <cleanup + return -1>   ; 0x4 IS set → exit without adding to list
    //
    //   Fix: change JZ (74 22) → JMP (EB 22).  The unconditional short jump always
    //   lands at 0x10e7dc98, bypassing the early-exit block entirely.
    //
    //   Before: 74 22   JZ  0x10e7dc98
    //   After:  EB 22   JMP 0x10e7dc98
    static const uint8_t jmpSkip0x4[] = { 0xEB, 0x22 };
    MemoryWriter::WriteBytes(0x10e7dc74, jmpSkip0x4, sizeof(jmpSkip0x4));

    // Check B – flag 0x4000 (SF_XBOXHD_STREAM: Xbox HD Stream sounds suppressed from the
    //   PC editor browser by Ubisoft — the PC build has no .hds pipeline so these were
    //   intentionally hidden.  WAV data IS present in the UAX; Export works normally):
    //   10e7dc98: TEST AH, 0x40           ; test bit 14 (0x4000 = 0x40 in AH)
    //   10e7dc9b: JNZ 0x10e7dc76          ; if 0x4000 IS set → jump to early exit
    //   10e7dc9d: FLD float ptr [EBX+0x68]; else → continue with radius display
    //
    //   Fix: replace JNZ (75 D9) with two NOPs (90 90).  Execution always falls
    //   through to the radius display path, so SF_XBOXHD_STREAM sounds appear as waves.
    //
    //   Before: 75 D9   JNZ 0x10e7dc76
    //   After:  90 90   NOP NOP
    static const uint8_t nop2[] = { 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10e7dc9b, nop2, sizeof(nop2));

    //  Patch 4: Suppress Phase 1 SavePackage crash in AUDIO EXPORTXBOX
    //
    // UAudioSubsystem_ExportToXBox (0x10F70BA0) has two SavePackage calls:
    //   Call 1 @ 0x10F711DC  — fires before WAV data is zeroed (safe)
    //   Call 2 @ 0x10F7188E  — fires AFTER WAV data zeroing at 0x10F714EF (crash)
    //
    // Phase 1 XB-encodes each qualifying USound (those without SF_UAS_STREAM),
    // zeros the WAV data fields (+0x34/+0x38/+0x3C), then calls SavePackage to write
    // the UAX stub.  The SF_XBOXHD_STREAM branch correctly ORs SF_UAS_STREAM into the
    // flags before serialising.  The regular encode path does not — so USound::Serialize
    // sees zeroed WAV + no SF_UAS_STREAM and GPFs.
    //
    // Fix: NOP the 5-byte CALL at 0x10F7188E (Phase 1's second SavePackage invocation).
    //   Stack balance is preserved — ADD ESP,24 at 0x10F71893 still cleans the 6 pushes (6 args x 4 bytes = 24 bytes).
    //
    //   Before:  E8 xx xx xx xx   CALL SavePackage  (5 bytes)
    //   After:   90 90 90 90 90   NOP x5
    static const uint8_t nop5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    MemoryWriter::WriteBytes(0x10F7188E, nop5, sizeof(nop5));

    INSTALL_HOOKS;
}
