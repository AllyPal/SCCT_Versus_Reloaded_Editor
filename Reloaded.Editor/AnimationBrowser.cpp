#include "pch.h"
#include "AnimationBrowser.h"
#include "PropertyGrid.h"
#include "SoundBrowser.h"     // PeekSelectedSound for "Use" button
#include "logger.h"
#include <shellapi.h>         // ExtractIconExA for window icon
#pragma comment(lib, "shell32.lib")
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>    // GET_X_LPARAM / GET_Y_LPARAM
#include <mimalloc.h>    // mi_free for TArray sub-allocation cleanup
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

#define GEDITOR_GLOBAL              0x1165dfa0u
#define EXEC_LOG_DEV                0x115befb0u
#define EDITOR_GET_VTABLE_OFFSET    0x224u

#define ANIM_BROWSER_MENU_RES       15166

#define IDMN_FileOpen               40493
#define IDMN_FileSave               40495
#define IDMN_FILE_IMPORTLOD         40498
#define IDMN_FILE_IMPORTMESH        40499
#define IDMN_FILE_IMPORTANIM        40500
#define IDMN_FILE_IMPORTANIMMORE    40501

#define IDMN_VIEW_INFO              40526
#define IDMN_VIEW_BONES             40337
#define IDMN_VIEW_INFLUENCES        40492
#define IDMN_VIEW_BOUNDS            40502
#define IDMN_VIEW_BACKFACE          40506
#define IDMN_VIEW_WIRE              40507
#define IDMN_VIEW_HIDESKIN          40356
#define IDMN_VIEW_REFPOSE           40509
#define IDMN_VIEW_PILLS             40655
#define IDMN_VIEW_RAWOFFSET         40514
#define IDMN_VIEW_BONENAMES         40522
#define IDMN_VIEW_LEVELANIM         145
#define IDMN_REFRESH                40375

#define IDMN_EDIT_LINKANIM          40515
#define IDMN_EDIT_UNLINKANIM        40516
#define IDMN_AB_LOAD_ENTIRE_PACKAGE 40517
#define IDMN_EDIT_APPLY             40520
#define IDMN_EDIT_UNDO              40521
#define IDMN_EDIT_COPYSHORTCUT      40588
#define IDMN_EDIT_CHECKUNUSEDBONES  200
#define IDMN_EDIT_CHECKSCRIPTREFS   216

#define IDMN_EDIT_MESHPROP          40497
#define IDMN_EDIT_COPYMESHPROPS     40530
#define IDMN_EDIT_PASTEMESHPROPS    40531
#define IDMN_EDIT_RENAMEMESH        40591
#define IDMN_EDIT_DELETEMESH        40592
#define IDMN_MESH_REDIGESTLOD       40625
#define IDMN_MESH_CYCLELOD          40633
#define IDMN_MESH_IMPORTLOD         40634

#define IDMN_EDIT_PREFS             40519
#define IDMN_EDIT_ANIMPROP          40496
#define IDMN_EDIT_SEQUPROP          40513
#define IDMN_EDIT_NOTIFICATIONS     40518
#define IDMN_EDIT_ADDNOTIFY         40523
#define IDMN_EDIT_COPYNOTIFIES      40524
#define IDMN_EDIT_PASTENOTIFIES     40527
#define IDMN_EDIT_CLEARNOTIFIES     40525
#define IDMN_EDIT_GROUPS            40529
#define IDMN_EDIT_CLEARGROUPS       40532
#define IDMN_EDIT_COPYGROUPS        40533
#define IDMN_EDIT_PASTEGROUPS       40534
#define IDMN_EDIT_RENAMEANIM        40589
#define IDMN_EDIT_DELETEANIM        40590

// Our own child-control IDs (do not collide with the inherited menu).
#define IDC_AB_PACKAGE_LABEL        2001
#define IDC_AB_PACKAGE_COMBO        2002
#define IDC_AB_MESH_LABEL           2003
#define IDC_AB_MESH_COMBO           2004
#define IDC_AB_ANIM_LABEL           2005
#define IDC_AB_ANIM_COMBO           2006
#define IDC_AB_SEQ_LABEL            2007
#define IDC_AB_SEQ_LIST             2008
#define IDC_AB_PROPS_TABS           2009
#define IDC_AB_TAB_MESH_PAGE        2010
#define IDC_AB_TAB_ANIM_PAGE        2011
#define IDC_AB_TAB_SEQ_PAGE         2012
#define IDC_AB_TAB_NOTIFY_PAGE      2013
#define IDC_AB_TAB_PREFS_PAGE       2014
#define IDC_AB_TOOLBAR              2015

// Animation Browser toolbar bitmap embedded in ChaosTheory_Editor.exe.
#define RES_AB_TOOLBAR_BITMAP       29786

// Sequence tab controls (3000 range so they don't collide with menu).
#define IDC_AB_SEQ_RATE_EDIT        3001
#define IDC_AB_SEQ_NAME_STATIC      3002
#define IDC_AB_SEQ_STARTFRAME_STATIC 3003
#define IDC_AB_SEQ_NUMFRAMES_STATIC 3004
#define IDC_AB_SEQ_BOOKMARK_STATIC  3005
#define IDC_AB_SEQ_GROUPS_STATIC    3006
#define IDC_AB_SEQ_COMPRESSION_EDIT 3007

// Notify tab controls (3100 range).
#define IDC_AB_NOTIFY_LIST          3101

// Animation Set tab controls (3200 range).
#define IDC_AB_ANIM_GLOBALCOMP_EDIT 3201
#define IDC_AB_ANIM_GRID            3202

// Prefs tab controls (3300 range).
#define IDC_AB_PREFS_WELD_CHECK     3301
#define IDC_AB_PREFS_GRID           3302

// Mesh tab controls (3400 range).
#define IDC_AB_MESH_DEFAULTANIM_STATIC 3401
#define IDC_AB_MESH_MATERIAL_LIST   3402
#define IDC_AB_MESH_MATERIAL_HEADER 3403
#define IDC_AB_MESH_GRID            3404

// Sequence tab grid (3500 range).
#define IDC_AB_SEQ_GRID             3501

// Tab indices, in the order added.  Must match UT2004 WBrowserAnimation
// for consistency with the menu items (IDMN_EDIT_MESHPROP -> Mesh tab,
// IDMN_EDIT_ANIMPROP -> AnimSet tab, etc.).
enum AB_TabIndex
{
    AB_TAB_MESH = 0,
    AB_TAB_ANIM = 1,
    AB_TAB_SEQ  = 2,
    AB_TAB_NOTIFY = 3,
    AB_TAB_PREFS = 4,
    AB_TAB_COUNT
};

// ---------------------------------------------------------------------
//  Module-level state.  This is a singleton window: there is no need to
//  support multiple instances of the Animation Browser at once.
// ---------------------------------------------------------------------
static const char  kWndClassName[] = "ReloadedAnimationBrowser";
static HWND        g_hWnd          = nullptr;
static HMENU       g_hMenu         = nullptr;

static HWND        g_hPackageCombo = nullptr;
static HWND        g_hMeshCombo    = nullptr;
static HWND        g_hAnimCombo    = nullptr;
static HWND        g_hSeqList      = nullptr;
static HWND        g_hTabs         = nullptr;
static HWND        g_hToolbar      = nullptr;
static HWND        g_hTabPage[AB_TAB_COUNT] = {};
static HFONT       g_hBoldFont     = nullptr;
static HBRUSH      g_hCategoryBrush = nullptr;
static int         g_currentTab    = AB_TAB_MESH;

// Each property control's relative position within its tab page.  Tab
// pages get repositioned as the window resizes, and so should all of
// their contents.  Property controls are direct children of the main
// window (not of the tab page STATIC) so WM_COMMAND routes naturally to
// our WndProc.  Visibility is toggled on tab switch.
struct TabControl
{
    HWND hwnd;
    int  relX, relY, w, h;
    bool stretch;        // when true, width auto-extends to the page edge
    bool stretchHeight;  // when true, height auto-extends to fill the page
};
static std::vector<TabControl> g_tabContent[AB_TAB_COUNT];

// Category header HWNDs - tracked separately so WM_CTLCOLORSTATIC can
// paint them with the UT2004-style gray background.
static std::vector<HWND> g_categoryHeaders;

// Layout constants for the UT2004-style property panel.
static const int   kPropRowHeight       = 20;
static const int   kPropCategoryHeight  = 22;
static const int   kPropCategoryPadTop  = 6;
static const int   kPropPadX            = 8;
static const int   kPropNameWidth       = 120;
static const int   kPropEditX           = kPropNameWidth + 8;

// Initial size mirrors UT2004's default WBrowserAnimation footprint.
static const int   kInitialWidth   = 820;
static const int   kInitialHeight  = 560;
static const int   kSeqListWidth   = 182;  // matches UT2004 SeqWinWidth
static const int   kComboRowHeight = 22;
static const int   kToolbarHeight  = 30;    // room for the
                                            //  restored AB toolbar
                                            //  (bitmap 29786 from the
                                            //  editor EXE, 16x16 tiles
                                            //  inside 23x23 buttons).
static const int   kComboRowsTop   = kToolbarHeight + 4;

// ---------------------------------------------------------------------
//  Editor exec helpers (FExec::Exec and UEditorEngine::Get).
//  These are byte-for-byte the same shape SoundBrowser.cpp uses; sharing
//  is intentionally avoided so this translation unit is self-contained.
// ---------------------------------------------------------------------
static void __cdecl ExecEditorCommand(const char* cmd)
{
    if (!cmd || !*cmd) return;
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

// UEditorEngine::Get via vtable[0x224/4] on GEditor itself (NOT the
// FExec sub-object).  This is the dispatcher UnrealEd's browsers use
// for the OBJ topic - critically, `OBJ DELETE` / `OBJ RENAME` only
// take effect through Get, not through FExec::Exec (Exec silently
// no-ops them).  SoundBrowser.cpp deletes sounds the same way.
//   section = topic, e.g. "OBJ"
//   key     = operation + args, e.g. "DELETE CLASS=Sound OBJECT=\"foo\""
static void __cdecl CallEditorGet(const char* section, const char* key)
{
    if (!section || !key) return;
    void* gEditor = *reinterpret_cast<void**>(GEDITOR_GLOBAL);
    if (!gEditor) return;
    void* vtable  = *reinterpret_cast<void**>(gEditor);
    if (!vtable) return;
    void* getFn   = *reinterpret_cast<void**>(
        static_cast<char*>(vtable) + EDITOR_GET_VTABLE_OFFSET);
    void* logDev  = *reinterpret_cast<void**>(EXEC_LOG_DEV);
    __asm {
        push logDev     // arg3: FOutputDevice&
        push key        // arg2: key / command string
        push section    // arg1: section string
        mov  ecx, gEditor
        mov  eax, getFn
        call eax        // UEditorEngine::Get - callee cleans 3*4 = 12 bytes
    }
}

// NOTE: A fake FOutputDevice approach for capturing
// UEditorEngine::Get output proved unsafe - UE2's FOutputDevice has
// `bSuppressEventTag` and `bAutoEmitLineTerminator` UBOOL fields
// immediately after the vtable pointer that the engine reads AND writes
// during dispatch.  Replacing those bytes with a std::string instance
// caused the engine to corrupt our string memory and then GPF inside
// ObjTopicHandler::Get.
//
// A later pass will replace this with a direct GObjects walk (same way
// SoundBrowser.cpp does object access via GOBJECTS_DATA / GOBJECTS_NUM)
// once we've confirmed the UObject::Class offset for SCCT in Ghidra.
//
// For now, the package / mesh / anim combos are user-editable: pick by
// typing a name, or let File > Open auto-add the just-loaded package.

// ---------------------------------------------------------------------
//  Combo / list refreshers
// ---------------------------------------------------------------------
// Legacy entry point: earlier callers used this for File > Save /
// Import to read combo values.  Forwarded to ComboReadValue (defined
// below) so all combo reads go through the same authoritative path.
static std::string ComboReadValue(HWND hCombo);
static std::string GetComboText(HWND hCombo) { return ComboReadValue(hCombo); }

// ---------------------------------------------------------------------
//  combo enumeration via direct GObjects walk
// ---------------------------------------------------------------------
//  Walker is SEH-protected so a wrong UObject::Class offset can only
//  produce empty combos - never a GPF.  The class offset is also auto-
//  probed at first use: 0x28 (FName=8 bytes), 0x24 (FName=4 bytes),
//  0x2C (with LastIsA cache) are tried in turn; whichever produces a
//  validly-named UClass is cached.
//
//  Other offsets are taken from SoundBrowser.cpp (known-good in SCCT):
//    0x18  Outer       (UObject*)
//    0x20  Name        (FName.Index)
#define UOBJ_OUTER_OFFSET       0x18
#define UOBJ_FNAME_OFFSET       0x20

#define GOBJECTS_DATA           0x11697b70u
#define GOBJECTS_NUM            0x11697b74u
#define GNAMES_DATA             0x1169cfbcu
#define GNAMES_NUM              0x1169cfc0u
#define FNAME_ENTRY_STR_OFFSET  0x0Cu

// =====================================================================
//  UProperty / UStruct field offsets (Ghidra-verified)
// =====================================================================
//  From decompiling UProperty::Serialize (FUN_10fe1790) and UStruct::Link
//  (FUN_10fb8a70) at image base 0x10E00000.  Match what UE2 build 2110
//  expects across UField -> UProperty -> UStruct.
//
//  UField layout (after UObject ends at +0x28 in SCCT):
//    +0x28  UField*  SuperField   (a.k.a. UStruct::SuperStruct on UStruct)
//    +0x2C  UField*  Next         (chain through UStruct::Children)
//
//  UStruct adds (UStruct extends UField):
//    +0x30  UField*    Children       (head of Children chain)
//    +0x34  INT        PropertiesSize
//    +0x58  UProperty* PropertyLink   (flat, includes inherited)
//
//  UProperty extends UField, adds:
//    +0x30  WORD       ArrayDim
//    +0x32  WORD       ElementSize     (set by Link, not serialized)
//    +0x34  DWORD      PropertyFlags   (CPF_Edit = 0x40000)
//    +0x38  FName      Category
//    +0x3C  DWORD      Offset          (in-object byte offset; set by Link)
//    +0x40  UProperty* PropertyLinkNext
//    +0x44  UProperty* ConfigOrderNext
//    +0x48  UProperty* ConstructorLinkNext
//    +0x4C  UProperty* RepOwner / DestructorLinkNext
//    +0x60  WORD       RepOffset       (only valid when CPF_Net is set)
//
//  Per-subclass extra fields (offsets follow common UProperty body):
//    UObjectProperty:  PropertyClass  (UClass*)  - filters object pickers
//    UClassProperty :  MetaClass      (UClass*)
//    UByteProperty  :  Enum           (UEnum*)   - for enum dropdowns
//    UBoolProperty  :  BitMask        (DWORD)    - which bit in the byte
//    UStructProperty:  Struct         (UScriptStruct*)
//    UArrayProperty :  Inner          (UProperty*) of element
//  These offsets are discovered when typed accessors land.
#define UFIELD_SUPERFIELD_OFFSET        0x28
#define UFIELD_NEXT_OFFSET              0x2C
#define USTRUCT_CHILDREN_OFFSET         0x30
#define USTRUCT_PROPERTIESSIZE_OFFSET   0x34
#define USTRUCT_PROPERTYLINK_OFFSET     0x58
#define UPROP_ARRAYDIM_OFFSET           0x30
#define UPROP_ELEMENTSIZE_OFFSET        0x32
#define UPROP_PROPERTYFLAGS_OFFSET      0x34
#define UPROP_CATEGORY_OFFSET           0x38
#define UPROP_OFFSET_OFFSET             0x3C
#define UPROP_PROPERTYLINKNEXT_OFFSET   0x40

// UObjectProperty subclass adds:
//   +0x64  UClass*  PropertyClass    (the UClass any assigned object
//                                     must be derived from)
// Ghidra-verified via UObjectProperty::Serialize at FUN_10fe2cf0 which
// calls `Ar << *(UObject**)(this + 0x64)` after the UProperty super.
#define UOBJPROP_PROPERTYCLASS_OFFSET   0x64

// Standard UE2 CPF_* flags - subset we care about for property editing.
// Values from UT2004 source UnObjBas.h; UStruct::Link decompile confirms
// CPF_NeedCtorLink (0x00400000) and CPF_Config (0x00004000) match.
#define CPF_Edit         0x00000001u   // var() declared in script (editable)
#define CPF_Const        0x00000002u   // can't write at runtime
#define CPF_EditConst    0x00020000u   // visible in editor, not editable
#define CPF_NeedCtorLink 0x00400000u   // requires constructor/destructor link
#define CPF_EditInline   0x04000000u   // embedded subobject - expand inline

// Cached, discovered at first walk.  0 means not yet probed.
static int g_uobjClassOffset = 0;

// POD result buckets (no destructors -> safe inside __try/__except).
struct GO_RawList
{
    enum { MAX_ITEMS = 1024, MAX_LEN = 96 };
    int  count;
    char items[MAX_ITEMS][MAX_LEN];
};

static GO_RawList s_namesBuf;
static GO_RawList s_packagesBuf;

static void GO_RawList_Reset(GO_RawList* l) { l->count = 0; }

static void GO_RawList_AddUnique(GO_RawList* l, const char* s)
{
    if (!l || !s || !*s || l->count >= GO_RawList::MAX_ITEMS) return;
    for (int i = 0; i < l->count; ++i)
        if (_stricmp(l->items[i], s) == 0) return;
    strncpy_s(l->items[l->count], GO_RawList::MAX_LEN, s, _TRUNCATE);
    l->count++;
}

// The dangerous part: POD-only function so __try/__except is legal.
// Returns true on clean walk; false if an AV interrupted it.
static bool GO_WalkRaw(const char* className,
                      int classOffset,
                      const char* filterPackage,
                      GO_RawList* namesOut,
                      GO_RawList* packagesOut)
{
    if (namesOut) GO_RawList_Reset(namesOut);
    if (packagesOut) GO_RawList_Reset(packagesOut);

    __try
    {
        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return true;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return true;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;

            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(clsName, className) != 0) continue;

            // Walk Outer chain to the top UPackage.
            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pkgIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pkgIdx < 0 || pkgIdx >= gNamesNum) continue;
            void* pkgEntry = gNamesData[pkgIdx];
            if (!pkgEntry) continue;
            const char* pkgName = (char*)pkgEntry + FNAME_ENTRY_STR_OFFSET;

            if (filterPackage && _stricmp(filterPackage, pkgName) != 0)
                continue;

            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            void* objEntry = gNamesData[objIdx];
            if (!objEntry) continue;
            const char* objName = (char*)objEntry + FNAME_ENTRY_STR_OFFSET;

            GO_RawList_AddUnique(namesOut, objName);
            GO_RawList_AddUnique(packagesOut, pkgName);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Probe Class field offset by searching for any UObject whose alleged
// UClass at the candidate offset has a sane name in GNames.  Returns
// the first offset that produces at least 5 valid matches (heuristic
// to avoid coincidence) - SEH-protected so misses can't crash.
static int GO_ProbeClassOffset()
{
    static const int candidates[] = { 0x28, 0x24, 0x2C, 0x30 };
    static const int numCandidates =
        static_cast<int>(sizeof(candidates) / sizeof(candidates[0]));

    for (int c = 0; c < numCandidates; ++c)
    {
        int off = candidates[c];
        __try
        {
            void** gObjData = *(void***)GOBJECTS_DATA;
            int    gObjNum  = *(int*)   GOBJECTS_NUM;
            if (!gObjData || gObjNum <= 0) continue;

            void** gNamesData = *(void***)GNAMES_DATA;
            int    gNamesNum  = *(int*)   GNAMES_NUM;
            if (!gNamesData || gNamesNum <= 0) continue;

            int hits = 0;
            int scanLimit = (gObjNum < 2000) ? gObjNum : 2000;
            for (int i = 0; i < scanLimit; ++i)
            {
                void* obj = gObjData[i];
                if (!obj) continue;
                void* cls = *(void**)((char*)obj + off);
                if (!cls) continue;
                int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
                if (clsIdx <= 0 || clsIdx >= gNamesNum) continue;
                void* entry = gNamesData[clsIdx];
                if (!entry) continue;
                const char* name = (char*)entry + FNAME_ENTRY_STR_OFFSET;
                // Sanity: a UClass FName entry is short ASCII, no NULs in
                // the first 16 chars, mostly alnum/underscore.
                bool sane = true;
                for (int k = 0; k < 16; ++k)
                {
                    char c = name[k];
                    if (c == 0) break;
                    if (c < 0x20 || c > 0x7E) { sane = false; break; }
                }
                if (sane && name[0] != 0) ++hits;
            }
            if (hits >= 5) return off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // try the next candidate
        }
    }
    return 0;
}

// std::vector-friendly wrapper.  Splits POD walking from C++ wiring so
// __try/__except can live in a destructor-free function.
static void GO_WalkByClass(const char* className,
                           std::vector<std::string>* namesOut,
                           std::vector<std::string>* packagesOut,
                           const char* filterPackage)
{
    if (g_uobjClassOffset == 0)
    {
        g_uobjClassOffset = GO_ProbeClassOffset();
        if (g_uobjClassOffset == 0)
        {
            Logger::log("AnimationBrowser: could not probe UObject::Class offset");
            return;
        }
        char msg[96];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "AnimationBrowser: UObject::Class offset = 0x%X",
                    g_uobjClassOffset);
        Logger::log(msg);
    }

    if (!GO_WalkRaw(className, g_uobjClassOffset,
                    filterPackage, &s_namesBuf, &s_packagesBuf))
    {
        Logger::log("AnimationBrowser: GObjects walk AV'd - aborted");
        return;
    }

    if (namesOut)
    {
        namesOut->reserve(namesOut->size() + s_namesBuf.count);
        for (int i = 0; i < s_namesBuf.count; ++i)
            namesOut->emplace_back(s_namesBuf.items[i]);
    }
    if (packagesOut)
    {
        packagesOut->reserve(packagesOut->size() + s_packagesBuf.count);
        for (int i = 0; i < s_packagesBuf.count; ++i)
            packagesOut->emplace_back(s_packagesBuf.items[i]);
    }
}

// `forceFirst`:
//   false  - if the previous edit text still matches a list entry, keep
//            it selected (used by Refresh to preserve the user's choice).
//   true   - always select the first entry (used when the cascade is
//            invalidated by a parent-combo change, e.g. switching package
//            wipes any stale mesh/anim selection).
// Empty list  -> combo edit cleared.
static void FillCombo(HWND hCombo, const std::vector<std::string>& items,
                      bool forceFirst)
{
    if (!hCombo) return;

    char prev[256] = "";
    if (!forceFirst)
        GetWindowTextA(hCombo, prev, sizeof(prev));

    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& s : items)
        SendMessageA(hCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(s.c_str()));

    if (items.empty())
    {
        SetWindowTextA(hCombo, "");
        return;
    }

    if (!forceFirst && prev[0])
    {
        LRESULT idx = SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                   reinterpret_cast<LPARAM>(prev));
        if (idx != CB_ERR) { SendMessageA(hCombo, CB_SETCURSEL, idx, 0); return; }
    }

    SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
}

static void RefreshPackages()
{
    if (!g_hPackageCombo) return;
    std::vector<std::string> pkgs;
    GO_WalkByClass("SkeletalMesh",  nullptr, &pkgs, nullptr);
    GO_WalkByClass("MeshAnimation", nullptr, &pkgs, nullptr);

    // Two walks can return the same package - merge case-insensitively.
    std::sort(pkgs.begin(), pkgs.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    pkgs.erase(std::unique(pkgs.begin(), pkgs.end(),
                           [](const std::string& a, const std::string& b) {
                               return _stricmp(a.c_str(), b.c_str()) == 0;
                           }),
               pkgs.end());

    // Packages: preserve current selection if still present so an idle
    // View > Refresh doesn't yank the user off their working package.
    FillCombo(g_hPackageCombo, pkgs, /*forceFirst=*/false);
}

// Authoritative read of a combo's "current value":
//   - If a listbox row is selected (CB_GETCURSEL valid), return that row's
//     text via CB_GETLBTEXT.  This is the row state that's updated
//     synchronously when the user picks from the dropdown, so refreshing
//     on CBN_CLOSEUP / CBN_SELCHANGE reads the correct new value even
//     before the edit field is synced.
//   - If no row is selected (user typed something that doesn't match an
//     entry), fall back to GetWindowText (the typed text).
static std::string ComboReadValue(HWND hCombo)
{
    if (!hCombo) return {};

    int idx = static_cast<int>(SendMessageA(hCombo, CB_GETCURSEL, 0, 0));
    if (idx != CB_ERR)
    {
        int len = static_cast<int>(
            SendMessageA(hCombo, CB_GETLBTEXTLEN, idx, 0));
        if (len > 0 && len < 4096)
        {
            std::string s(static_cast<size_t>(len), '\0');
            SendMessageA(hCombo, CB_GETLBTEXT, idx,
                         reinterpret_cast<LPARAM>(&s[0]));
            return s;
        }
    }

    char buf[256] = "";
    GetWindowTextA(hCombo, buf, sizeof(buf));
    return buf;
}

static std::string PackageComboText() { return ComboReadValue(g_hPackageCombo); }
static std::string MeshComboText()    { return ComboReadValue(g_hMeshCombo); }
static std::string AnimComboText()    { return ComboReadValue(g_hAnimCombo); }

// window title reflects the current mesh and animation set
// selection, "Animation Tool - <Mesh> - <AnimSet>".  Called after each
// combo selection change and after refreshes that auto-pick top items
// (File>Open, package change, import dialog completion).
static void UpdateWindowTitle()
{
    if (!g_hWnd) return;
    std::string mesh = MeshComboText();
    std::string anim = AnimComboText();
    char title[256];
    if (mesh.empty() && anim.empty())
        strncpy_s(title, "Animation Tool", _TRUNCATE);
    else if (anim.empty())
        _snprintf_s(title, _TRUNCATE,
                    "Animation Tool - %s", mesh.c_str());
    else if (mesh.empty())
        _snprintf_s(title, _TRUNCATE,
                    "Animation Tool - %s", anim.c_str());
    else
        _snprintf_s(title, _TRUNCATE,
                    "Animation Tool - %s - %s",
                    mesh.c_str(), anim.c_str());
    SetWindowTextA(g_hWnd, title);
}

// `forceFirst` defaults to true so the long-standing "package change
// resets dependent combos to top of list" behavior is preserved.  The
// The auto-refresh path passes false because it wants to keep
// the user's current selection while it silently picks up newly-
// loaded packages in the background.
static void RefreshMeshList(bool forceFirst = true)
{
    if (!g_hMeshCombo) return;
    std::string pkg = PackageComboText();
    if (pkg.empty()) { SendMessageA(g_hMeshCombo, CB_RESETCONTENT, 0, 0); return; }
    std::vector<std::string> meshes;
    GO_WalkByClass("SkeletalMesh", &meshes, nullptr, pkg.c_str());
    std::sort(meshes.begin(), meshes.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    FillCombo(g_hMeshCombo, meshes, forceFirst);
}

static void RefreshAnimList(bool forceFirst = true)
{
    if (!g_hAnimCombo) return;
    std::string pkg = PackageComboText();
    if (pkg.empty()) { SendMessageA(g_hAnimCombo, CB_RESETCONTENT, 0, 0); return; }
    std::vector<std::string> anims;
    GO_WalkByClass("MeshAnimation", &anims, nullptr, pkg.c_str());
    std::sort(anims.begin(), anims.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    FillCombo(g_hAnimCombo, anims, forceFirst);
}

// ---------------------------------------------------------------------
//  Sequence-list enumeration via direct UMeshAnimation read.
// ---------------------------------------------------------------------
//  UMeshAnimation layout (UT2004 source, applies to SCCT too):
//    UObject base                              sizeof(UObject) bytes
//    INT                 InternalVersion       +0
//    TArray<FNamedBone>  RefBones              +4   (12 bytes)
//    TArray<MotionChunk> Moves                 +16  (12 bytes)
//    TArray<FMeshAnimSeq>AnimSeqs              +28  (12 bytes)
//
//  So AnimSeqs.Data is at sizeof(UObject) + 28 = classOffset + 32.
//  We compute these dynamically off the probed Class offset so we work
//  with either 4-byte FName (classOff=0x24) or 8-byte FName (0x28).
//
//  FMeshAnimSeq layout:
//    FName               Name        +0   (FName.Index at +0)
//    TArray<FName>       Groups      sizeof(FName)
//    INT                 StartFrame
//    INT                 NumFrames
//    FLOAT               Rate
//    TArray<FMeshAnimNotify> Notifys
//    FLOAT               Bookmark
//  Stride = sizeof(FName) + 40.

struct GO_SeqList
{
    enum { MAX_ITEMS = 1024, MAX_LEN = 96 };
    int  count;
    char items[MAX_ITEMS][MAX_LEN];
};
static GO_SeqList s_seqBuf;

// SEH-protected sequence enumeration.  POD only.  Returns false on AV.
static bool GO_EnumSequencesRaw(int classOffset,
                               const char* package,
                               const char* animName,
                               GO_SeqList* out)
{
    out->count = 0;

    __try
    {
        const int sizeOfFName     = classOffset - UOBJ_FNAME_OFFSET; // 4 or 8
        const int sizeOfUObject   = classOffset + 4;
        const int animSeqsDataOff = sizeOfUObject + 4 + 12 + 12;
        const int animSeqsNumOff  = animSeqsDataOff + 4;
        const int seqStride       = sizeOfFName + 40;

        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return true;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return true;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            // Class match: UMeshAnimation.
            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(clsName, "MeshAnimation") != 0) continue;

            // Object name match.
            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            void* objEntry = gNamesData[objIdx];
            if (!objEntry) continue;
            const char* objNameStr = (char*)objEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(objNameStr, animName) != 0) continue;

            // Package match - walk Outer chain to top.
            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pkgIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pkgIdx < 0 || pkgIdx >= gNamesNum) continue;
            void* pkgEntry = gNamesData[pkgIdx];
            if (!pkgEntry) continue;
            const char* pkgName = (char*)pkgEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(pkgName, package) != 0) continue;

            // Read AnimSeqs TArray.
            void* seqsData = *(void**)((char*)obj + animSeqsDataOff);
            int   seqsNum  = *(int*)  ((char*)obj + animSeqsNumOff);
            if (!seqsData || seqsNum <= 0) return true;
            if (seqsNum > 8192) return true;  // sanity cap

            char* seq = (char*)seqsData;
            for (int j = 0; j < seqsNum && out->count < GO_SeqList::MAX_ITEMS;
                 ++j, seq += seqStride)
            {
                int seqIdx = *(int*)(seq);  // Name.Index at offset 0
                if (seqIdx < 0 || seqIdx >= gNamesNum) continue;
                void* seqEntry = gNamesData[seqIdx];
                if (!seqEntry) continue;
                const char* seqName = (char*)seqEntry + FNAME_ENTRY_STR_OFFSET;
                if (!seqName[0]) continue;

                strncpy_s(out->items[out->count], GO_SeqList::MAX_LEN,
                          seqName, _TRUNCATE);
                out->count++;
            }
            return true;  // found the target anim; done
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// =====================================================================
//  UT2004-style property panel framework
// =====================================================================
//  Mirrors the look of UnrealEd's WProperties: bold category bars over
//  a white-ish background with name/value rows below.  Controls live as
//  direct children of the main window (not the tab page STATIC) so
//  WM_COMMAND routes to our WndProc without subclassing.  Visibility is
//  toggled per-tab.  Relative (x,y,w,h) within the tab page rect is
//  stored per-control so PositionChildControls can place them after a
//  window resize.
// =====================================================================

static HFONT GetBoldFont()
{
    if (!g_hBoldFont)
    {
        LOGFONTA lf = {};
        lf.lfHeight  = -11;     // ~8pt at 96dpi
        lf.lfWeight  = FW_BOLD;
        lf.lfCharSet = ANSI_CHARSET;
        strncpy_s(lf.lfFaceName, "MS Shell Dlg", _TRUNCATE);
        g_hBoldFont = CreateFontIndirectA(&lf);
    }
    return g_hBoldFont;
}

static HBRUSH GetCategoryBrush()
{
    if (!g_hCategoryBrush)
        g_hCategoryBrush = CreateSolidBrush(RGB(210, 210, 210));
    return g_hCategoryBrush;
}

static bool IsCategoryHeader(HWND hCtl)
{
    for (HWND h : g_categoryHeaders)
        if (h == hCtl) return true;
    return false;
}

// Adds a UT2004-style category header (bold, gray bar).  Returns the
// next Y position to use for child rows below.
static int AddCategoryHeader(int tabIdx, HWND hParent,
                             int relX, int relY, int width,
                             const char* label)
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    HWND h = CreateWindowExA(0, "STATIC", label,
                             WS_CHILD | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                             0, 0, 0, 0,
                             hParent, (HMENU)0xFFFF, hInst, nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(GetBoldFont()), TRUE);

    g_categoryHeaders.push_back(h);
    g_tabContent[tabIdx].push_back({
        h, relX, relY + kPropCategoryPadTop, width,
        kPropCategoryHeight - kPropCategoryPadTop,
        /*stretch=*/true, /*stretchHeight=*/false });
    return relY + kPropCategoryHeight + 2;
}

// Adds a "Label:" + value-control row.  Returns the value control HWND
// for later refresh.  If `readOnly`, the value EDIT is gray + ES_READONLY.
static HWND AddPropertyRow(int tabIdx, HWND hParent,
                           int relX, int relY, int rowWidth,
                           int controlId, const char* label, bool readOnly)
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    HWND lbl = CreateWindowExA(0, "STATIC", label,
                               WS_CHILD | SS_LEFT,
                               0, 0, 0, 0,
                               hParent, (HMENU)0xFFFF, hInst, nullptr);
    SendMessageA(lbl, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    g_tabContent[tabIdx].push_back({
        lbl, relX + kPropPadX, relY + 3, kPropNameWidth, kPropRowHeight - 3,
        /*stretch=*/false, /*stretchHeight=*/false });

    // Read-only values render as plain STATIC (no border, no caret) like
    // UT2004 WProperties does for non-editable rows.  Editable values use
    // EDIT with a border so it's clear the user can type into them.
    HWND val;
    if (readOnly)
    {
        val = CreateWindowExA(0, "STATIC", "",
                              WS_CHILD | SS_LEFT,
                              0, 0, 0, 0,
                              hParent, (HMENU)(INT_PTR)controlId,
                              hInst, nullptr);
    }
    else
    {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT | WS_BORDER;
        val = CreateWindowExA(0, "EDIT", "", style,
                              0, 0, 0, 0,
                              hParent, (HMENU)(INT_PTR)controlId,
                              hInst, nullptr);
    }
    SendMessageA(val, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    g_tabContent[tabIdx].push_back({
        val, relX + kPropEditX, relY + (readOnly ? 3 : 0),
        rowWidth - kPropEditX - kPropPadX,
        readOnly ? kPropRowHeight - 3 : kPropRowHeight - 2,
        /*stretch=*/true, /*stretchHeight=*/false });

    return val;
}

// Sequence tab construction.  Mirrors UT2004 USequEditProps:
//   SequenceProperties / SequenceName, Rate, Compression
//   Sequence Info       / StartFrame, NumFrames, Bookmark   (read-only)
//   Groups              / TArray<FName>                     (read-only for now)
static void BuildSequenceTab(HWND hParent, int pageW)
{
    HWND grid = PropertyGrid::Create(hParent, IDC_AB_SEQ_GRID,
                                     0, 0, pageW, 200);
    g_tabContent[AB_TAB_SEQ].push_back({
        grid, kPropPadX, 4, pageW - kPropPadX * 2, 200,
        /*stretch=*/true, /*stretchHeight=*/true });
}

// =====================================================================
//  Per-FMeshAnimSeq read/write
// =====================================================================
// Cached FMeshAnimSeq* for the currently selected sequence row.
static void* g_currentSeqPtr = nullptr;

// SEH-protected lookup: returns the FMeshAnimSeq* inside the
// UMeshAnimation whose (package, animName) matches, with the given
// seqName.  Layout offsets are derived from the auto-probed Class
// offset so this works for either 4- or 8-byte FName builds.
static void* GO_FindMeshAnimSeqRaw(int classOffset,
                                  const char* package,
                                  const char* animName,
                                  const char* seqName)
{
    __try
    {
        const int sizeOfFName     = classOffset - UOBJ_FNAME_OFFSET;
        const int sizeOfUObject   = classOffset + 4;
        const int animSeqsDataOff = sizeOfUObject + 4 + 12 + 12;
        const int animSeqsNumOff  = animSeqsDataOff + 4;
        const int seqStride       = sizeOfFName + 40;

        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return nullptr;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return nullptr;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(clsName, "MeshAnimation") != 0) continue;

            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            void* objEntry = gNamesData[objIdx];
            if (!objEntry) continue;
            const char* objName = (char*)objEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(objName, animName) != 0) continue;

            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pkgIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pkgIdx < 0 || pkgIdx >= gNamesNum) continue;
            void* pkgEntry = gNamesData[pkgIdx];
            if (!pkgEntry) continue;
            const char* pkgName = (char*)pkgEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(pkgName, package) != 0) continue;

            void* seqsData = *(void**)((char*)obj + animSeqsDataOff);
            int   seqsNum  = *(int*)  ((char*)obj + animSeqsNumOff);
            if (!seqsData || seqsNum <= 0 || seqsNum > 8192) return nullptr;

            char* seq = (char*)seqsData;
            for (int j = 0; j < seqsNum; ++j, seq += seqStride)
            {
                int sIdx = *(int*)seq;
                if (sIdx < 0 || sIdx >= gNamesNum) continue;
                void* sEntry = gNamesData[sIdx];
                if (!sEntry) continue;
                const char* sName = (char*)sEntry + FNAME_ENTRY_STR_OFFSET;
                if (_stricmp(sName, seqName) == 0)
                    return seq;
            }
            return nullptr;
        }
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// SEH-protected reader: pulls out the visible FMeshAnimSeq fields.
static bool GO_ReadMeshAnimSeqRaw(int classOffset, void* seqPtr,
                                  char nameOut[64],
                                  int* startFrame, int* numFrames,
                                  float* rate, float* bookmark,
                                  char groupsOut[256])
{
    if (nameOut)    nameOut[0]   = '\0';
    if (groupsOut)  groupsOut[0] = '\0';
    if (startFrame) *startFrame  = 0;
    if (numFrames)  *numFrames   = 0;
    if (rate)       *rate        = 0.0f;
    if (bookmark)   *bookmark    = 0.0f;
    if (!seqPtr) return false;

    __try
    {
        const int sizeOfFName    = classOffset - UOBJ_FNAME_OFFSET;
        // FMeshAnimSeq layout (FName at 0, then Groups TArray=12 bytes,
        // then StartFrame/NumFrames/Rate, then Notifys TArray=12, then
        // Bookmark).  Compute from the FName size.
        const int groupsDataOff  = sizeOfFName;
        const int groupsNumOff   = sizeOfFName + 4;
        const int startOff       = sizeOfFName + 12;
        const int numOff         = sizeOfFName + 16;
        const int rateOff        = sizeOfFName + 20;
        const int bookmarkOff    = sizeOfFName + 36;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;

        char* seq = (char*)seqPtr;

        if (nameOut && gNamesData)
        {
            int nIdx = *(int*)seq;
            if (nIdx >= 0 && nIdx < gNamesNum && gNamesData[nIdx])
            {
                const char* s = (char*)gNamesData[nIdx] + FNAME_ENTRY_STR_OFFSET;
                strncpy_s(nameOut, 64, s, _TRUNCATE);
            }
        }

        if (startFrame) *startFrame = *(int*)(seq + startOff);
        if (numFrames)  *numFrames  = *(int*)(seq + numOff);
        if (rate)       *rate       = *(float*)(seq + rateOff);
        if (bookmark)   *bookmark   = *(float*)(seq + bookmarkOff);

        if (groupsOut && gNamesData)
        {
            void* gData = *(void**)(seq + groupsDataOff);
            int   gNum  = *(int*)  (seq + groupsNumOff);
            if (gData && gNum > 0 && gNum <= 64)
            {
                // TArray<FName> stride = sizeOfFName (4 or 8) per entry.
                char* g = (char*)gData;
                int written = 0;
                for (int i = 0; i < gNum; ++i)
                {
                    int ix = *(int*)(g + i * sizeOfFName);
                    if (ix < 0 || ix >= gNamesNum) continue;
                    void* e = gNamesData[ix];
                    if (!e) continue;
                    const char* s = (char*)e + FNAME_ENTRY_STR_OFFSET;
                    int rem = 256 - written - 3;
                    if (rem <= 0) break;
                    if (written > 0)
                    {
                        groupsOut[written++] = ',';
                        groupsOut[written++] = ' ';
                    }
                    int len = (int)strlen(s);
                    if (len > rem) len = rem;
                    memcpy(groupsOut + written, s, len);
                    written += len;
                    groupsOut[written] = '\0';
                }
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-protected: deletes the FMeshAnimSeq matching `seqName` from the
// UMeshAnimation owning it.  Frees the sub-TArrays (Groups, Notifys)
// via mi_free (the editor's IAT was hooked to mimalloc by General.cpp,
// so this is the same allocator that grew them), then memmove's
// subsequent elements down by one stride and decrements Num.
//
// Doesn't shrink the TArray's Max - that's a separate Realloc which
// the next save will obsolete anyway when the package is re-serialized.
static bool GO_DeleteMeshAnimSeqRaw(int classOffset,
                                    const char* package,
                                    const char* animName,
                                    const char* seqName)
{
    __try
    {
        const int sizeOfFName     = classOffset - UOBJ_FNAME_OFFSET;
        const int sizeOfUObject   = classOffset + 4;
        const int animSeqsDataOff = sizeOfUObject + 4 + 12 + 12;
        const int animSeqsNumOff  = animSeqsDataOff + 4;
        const int seqStride       = sizeOfFName + 40;
        const int groupsDataOff   = sizeOfFName;       // within FMeshAnimSeq
        const int notifysDataOff  = sizeOfFName + 24;  // within FMeshAnimSeq

        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return false;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return false;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(clsName, "MeshAnimation") != 0) continue;

            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            const char* objNameStr = (char*)gNamesData[objIdx] + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(objNameStr, animName) != 0) continue;

            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pkgIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pkgIdx < 0 || pkgIdx >= gNamesNum) continue;
            const char* pkgName = (char*)gNamesData[pkgIdx] + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(pkgName, package) != 0) continue;

            char* seqsData = (char*)*(void**)((char*)obj + animSeqsDataOff);
            int   seqsNum  = *(int*)  ((char*)obj + animSeqsNumOff);
            if (!seqsData || seqsNum <= 0) return false;

            // Find the seq by name.
            int seqIndex = -1;
            for (int j = 0; j < seqsNum; ++j)
            {
                int nameIdx = *(int*)(seqsData + j * seqStride);
                if (nameIdx < 0 || nameIdx >= gNamesNum) continue;
                const char* nm = (char*)gNamesData[nameIdx] + FNAME_ENTRY_STR_OFFSET;
                if (_stricmp(nm, seqName) == 0) { seqIndex = j; break; }
            }
            if (seqIndex < 0) return false;

            char* target = seqsData + seqIndex * seqStride;

            // Note: NOT calling mi_free on the seq's sub-TArrays
            // (Groups.Data / Notifys.Data).  Serialization-allocated
            // TArrays in SCCT don't reliably come through the IAT-hooked
            // mimalloc path - mi_free on those pointers AVs and the SEH
            // wrapper would convert the AV into "Failed to delete".
            // The leaked memory is at most a few hundred bytes per delete
            // and is reclaimed by the next save/reload of the package.
            (void)groupsDataOff;
            (void)notifysDataOff;

            int tailElems = (seqsNum - seqIndex - 1);
            if (tailElems > 0)
                memmove(target, target + seqStride, tailElems * seqStride);

            *(int*)((char*)obj + animSeqsNumOff) = seqsNum - 1;
            return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// =====================================================================
//  Notify enumeration (FMeshAnimSeq::Notifys -> ListView)
// =====================================================================
//  FMeshAnimNotify layout (from UT2004 UnAnim.h, packed at 4 bytes):
//    FLOAT             Time          + 0
//    FName             Function      + 4
//    UAnimNotify*      NotifyObject  + 4 + sizeOfFName
//  Stride = 8 + sizeOfFName  (12 for FName=4, 16 for FName=8).

struct GO_NotifyList
{
    enum { MAX_ITEMS = 512, MAX_LEN = 96 };
    int  count;
    float times[MAX_ITEMS];
    char  classes[MAX_ITEMS][MAX_LEN];
    char  functions[MAX_ITEMS][MAX_LEN];
};
static GO_NotifyList s_notifyBuf;

// SEH-protected: enumerates the FMeshAnimNotify entries of the
// FMeshAnimSeq at `seqPtr` into `out`.  Returns false on AV.
static bool GO_EnumNotifiesRaw(int classOffset, void* seqPtr, GO_NotifyList* out)
{
    out->count = 0;
    if (!seqPtr) return true;

    __try
    {
        const int sizeOfFName     = classOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff  = sizeOfFName + 24;
        const int notifysNumOff   = sizeOfFName + 28;
        const int notifyStride    = 8 + sizeOfFName;
        const int notifyObjOffset = 4 + sizeOfFName;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;

        char* seq = (char*)seqPtr;
        void* nData = *(void**)(seq + notifysDataOff);
        int   nNum  = *(int*)  (seq + notifysNumOff);
        if (!nData || nNum <= 0 || nNum > 8192) return true;

        char* p = (char*)nData;
        for (int i = 0; i < nNum && out->count < GO_NotifyList::MAX_ITEMS;
             ++i, p += notifyStride)
        {
            float t    = *(float*)(p + 0);
            int   fIdx = *(int*)  (p + 4);
            void* obj  = *(void**)(p + notifyObjOffset);

            const char* fname = "";
            if (gNamesData && fIdx >= 0 && fIdx < gNamesNum && gNamesData[fIdx])
                fname = (char*)gNamesData[fIdx] + FNAME_ENTRY_STR_OFFSET;

            const char* clsName = "(null)";
            if (obj)
            {
                void* cls = *(void**)((char*)obj + classOffset);
                if (cls)
                {
                    int cIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
                    if (gNamesData && cIdx >= 0 && cIdx < gNamesNum && gNamesData[cIdx])
                        clsName = (char*)gNamesData[cIdx] + FNAME_ENTRY_STR_OFFSET;
                }
            }

            int idx = out->count;
            out->times[idx] = t;
            strncpy_s(out->classes  [idx], GO_NotifyList::MAX_LEN, clsName, _TRUNCATE);
            strncpy_s(out->functions[idx], GO_NotifyList::MAX_LEN, fname,   _TRUNCATE);
            out->count++;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-protected write: pokes a new Rate into the FMeshAnimSeq.
static bool GO_WriteMeshAnimSeqRateRaw(int classOffset, void* seqPtr, float v)
{
    if (!seqPtr) return false;
    __try
    {
        const int sizeOfFName = classOffset - UOBJ_FNAME_OFFSET;
        const int rateOff = sizeOfFName + 20;
        *(float*)((char*)seqPtr + rateOff) = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Compression is a transient editor-side knob (UT2004 USequEditProps
// stores it but FMeshAnimSeq has no Compression field).  Default 1.0
// matches USequEditProps.defaultproperties.
static float g_seqCompression = 1.0f;

// Forward decl - defined immediately after.
static void RefreshNotifyTab();

// Introspection helpers - defined later, used by RefreshNotifyTab
// for a one-shot diagnostic dump.
static void UProp_DumpObjectOnce(void* obj, const char* tag);

// SEH-isolated UObject::Class pointer read - defined later, used by the
// per-element populator to dispatch the NotifyName/EventName row.
static void* ReadClassPtrSafe(void* obj);

// Forward decls.  Notify_PopulateChildren (defined
// earlier in file, before BuildNotifyTab) needs to call into the typed
// accessor library and the UProperty walker which both live further
// down.

// UPropInfo is hoisted here (full definition, not just forward decl)
// because Notify_AddPropRow's callback body reads info.typeName /
// info.offset directly.  The duplicate definition further down was
// removed in favor of this single source of truth.
struct UPropInfo
{
    void*    propPtr;
    char     name[64];
    char     typeName[64];
    char     category[64];
    int      offset;
    int      arrayDim;
    int      elementSize;
    unsigned flags;
};

struct PropBinding;   // opaque - only pointer use before its definition
typedef bool (*UPropCallback)(const UPropInfo& info, void* userdata);

// PropKind enum is hoisted here (rather than living next to PropBinding's
// full definition) because Notify_PopulateChildren and its helpers use
// PROP_KIND_FLOAT / PROP_KIND_NAME in `case` labels before the typed-
// accessor section is reached - and `case` requires a complete enum.
enum PropKind
{
    PROP_KIND_UNKNOWN = 0,
    PROP_KIND_FLOAT,
    PROP_KIND_INT,
    PROP_KIND_BYTE,
    PROP_KIND_BOOL,
    PROP_KIND_OBJECT,
    PROP_KIND_CLASS,
    PROP_KIND_NAME,
    PROP_KIND_STR,
    PROP_KIND_STRUCT,
    PROP_KIND_ARRAY,
};

static bool UProp_ForEachEditable(void* obj, UPropCallback cb, void* userdata);
static int  PropKindFromTypeName(const char* tn);
static bool PropKindIsEditable(int kind);
static void PropBindings_Reset();
static PropBinding* PropBindings_Alloc(void* obj, int offset, int kind);
static void PropBinding_Get(char* buf, int size, void* userdata);
static void PropBinding_Set(const char* text, void* userdata);

// Forward decls: Groups_Insert (defined above the FArray
// thunk in source order) needs the thunk + the seq-tab refresh
// message ID.  Both live further down; declare them here so the
// earlier definitions compile.
static void FArray_Insert(void* farray, int index, int count, int elementSize);
#define WM_AB_REFRESH_SEQ_TAB       (WM_USER + 0x102)
// Typed-accessor / object-property helpers used before
// their definitions (the ObjectPicker + struct-expansion code lives
// above the typed-accessor lib).
static bool PropRead_Object(void* obj, int off, void** out);
static bool PropRead_Float (void* obj, int off, float* out);
static bool PropRead_Int   (void* obj, int off, int* out);

// FName.Index -> string helpers are also defined later but needed by
// NotifyField_Get for the FName-resolving path.
static const char* FNameIdx_PeekRaw(int nameIdx);
static bool        FNameIdx_GetStr(int nameIdx, char* outBuf, int outSize);

// =====================================================================
//  FName interning (Ghidra-verified)
// =====================================================================
//  FName::FName(FName* out, const char* str, EFindName find) at 0x10FB9610
//  __thiscall - this/out in ECX, str + find on stack, callee cleans 8.
//  find:
//    0 = FNAME_Find    (return existing index or 0 if not present)
//    1 = FNAME_Add     (add to GNames if not present)
//    2 = FNAME_Replace (replace existing entry's flags)
//
//  Returns the FName.Index (the integer slot into GNames).  Use this
//  for any operation that needs to convert a typed string into the
//  FName the engine uses internally - rename, add Group, etc.
#define FNAME_CTOR_ADDR     0x10FB9610u
#define FNAME_FIND          0
#define FNAME_ADD           1

static int FName_Intern(const char* nameStr, int findMode)
{
    // Note: don't use a parameter named `str` here - x86 inline asm
    // treats STR (Store Task Register) as a keyword and MSVC warns.
    if (!nameStr || !*nameStr) return 0;
    int outIdx = 0;
    int* outPtr = &outIdx;
    void* fn = reinterpret_cast<void*>(static_cast<uintptr_t>(FNAME_CTOR_ADDR));
    __asm {
        push findMode
        push nameStr
        mov  ecx, outPtr
        mov  eax, fn
        call eax           // __thiscall - callee cleans 8 bytes
    }
    return outIdx;
}

// =====================================================================
//  Sequence tab grid - cached values + accessors
// =====================================================================
// RefreshSequenceTab caches the FMeshAnimSeq fields into these globals
// so the PropertyGrid getters can read them without re-walking GObjects
// on every paint.  Editing Rate or Compression updates the underlying
// data via the setters, then the cache is refreshed for display.
static char  g_curSeqName[64]  = "";
static int   g_curStartFrame   = 0;
static int   g_curNumFrames    = 0;
static float g_curRate         = 0.0f;
static float g_curBookmark     = 0.0f;
static char  g_curGroupsStr[256] = "";

// Forward decls - referenced by helpers defined ahead of these symbols.
static void RefreshSequenceList();
static void SelectTab(int tabIndex);
static bool FormatUObjectRef(void* obj, char* outBuf, int outSize);

static void Get_SeqName     (char* b, int n, void*) { strncpy_s(b, n, g_curSeqName, _TRUNCATE); }
static void Set_SeqName(const char* t, void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0 || !t || !*t) return;
    if (_stricmp(t, g_curSeqName) == 0) return;   // no-op rename

    int nameIdx = FName_Intern(t, FNAME_ADD);
    if (nameIdx == 0) return;

    // FMeshAnimSeq.Name lives at offset 0 of the seq struct; only the
    // Index half matters (the FName.Number field is left untouched if
    // present in this build's FName layout).
    *static_cast<int*>(g_currentSeqPtr) = nameIdx;

    // Cache the new name for the read-back path.
    strncpy_s(g_curSeqName, t, _TRUNCATE);

    // Rebuild the sequence listbox so the renamed entry shows under its
    // new label, and re-select it so the user doesn't lose context.
    RefreshSequenceList();
    LRESULT row = SendMessageA(g_hSeqList, LB_FINDSTRINGEXACT, (WPARAM)-1,
                               reinterpret_cast<LPARAM>(t));
    if (row != LB_ERR)
        SendMessageA(g_hSeqList, LB_SETCURSEL, (WPARAM)row, 0);
}
static void Get_StartFrame  (char* b, int n, void*) { _snprintf_s(b, n, _TRUNCATE, "%d",   g_curStartFrame); }
static void Get_NumFrames   (char* b, int n, void*) { _snprintf_s(b, n, _TRUNCATE, "%d",   g_curNumFrames); }
static void Get_Bookmark    (char* b, int n, void*) { _snprintf_s(b, n, _TRUNCATE, "%.6f", g_curBookmark); }
static void Get_SeqRate     (char* b, int n, void*) { _snprintf_s(b, n, _TRUNCATE, "%.6f", g_curRate); }
static void Get_SeqCompr    (char* b, int n, void*) { _snprintf_s(b, n, _TRUNCATE, "%.6f", g_seqCompression); }

static void Set_SeqRate(const char* t, void*)
{
    g_curRate = static_cast<float>(atof(t));
    if (g_currentSeqPtr && g_uobjClassOffset > 0)
        GO_WriteMeshAnimSeqRateRaw(g_uobjClassOffset, g_currentSeqPtr, g_curRate);
}
static void Set_SeqCompr(const char* t, void*)
{
    float v = static_cast<float>(atof(t));
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_seqCompression = v;
}

// Groups array (TArray<FName> on FMeshAnimSeq).  Turned
// into a fully-editable array - elements can be typed (interned via
// FName_Intern) and the array can be grown/shrunk via Insert/Delete/
// Empty plus Empty/Add buttons on the array header.
//
// Layout reminder: FMeshAnimSeq.Groups is a TArray<FName> at
//   +sizeOfFName  (Data)
//   +sizeOfFName+4 (Num)
//   +sizeOfFName+8 (Max)
// with stride = sizeOfFName (4 bytes per FName index in SCCT).

static int  Groups_Count(void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return 0;
    int sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq = static_cast<char*>(g_currentSeqPtr);
    int gNum = *(int*)(seq + sizeOfFName + 4);
    if (gNum < 0 || gNum > 256) return 0;
    return gNum;
}
static void Groups_Get(int i, char* b, int n, void*)
{
    if (!b || n <= 0) return;
    b[0] = '\0';
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return;
    int sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq = static_cast<char*>(g_currentSeqPtr);
    void* gData = *(void**)(seq + sizeOfFName);
    int   gNum  = *(int*) (seq + sizeOfFName + 4);
    if (!gData || i < 0 || i >= gNum) return;
    int nameIdx = *(int*)((char*)gData + i * sizeOfFName);
    void** gNamesData = *(void***)GNAMES_DATA;
    int    gNamesNum  = *(int*)   GNAMES_NUM;
    if (!gNamesData || nameIdx < 0 || nameIdx >= gNamesNum) return;
    void* entry = gNamesData[nameIdx];
    if (!entry) return;
    strncpy_s(b, n, (char*)entry + FNAME_ENTRY_STR_OFFSET, _TRUNCATE);
}

// Per-element setter: intern the user-typed text via the engine's own
// FName::FName ctor (FName_Intern wrapper) and write the resulting
// index into Groups[i].  Empty text -> NAME_None.
static void Groups_Set(int i, const char* text, void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return;
    if (!text) return;
    int sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq = static_cast<char*>(g_currentSeqPtr);
    int nameIdx = (text[0]) ? FName_Intern(text, FNAME_ADD) : 0;

    __try
    {
        void* gData = *(void**)(seq + sizeOfFName);
        int   gNum  = *(int*) (seq + sizeOfFName + 4);
        if (!gData || i < 0 || i >= gNum) return;
        *(int*)((char*)gData + i * sizeOfFName) = nameIdx;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Grow Groups by one slot via engine FArray::Insert (append mode), then
// memmove the tail down to free up beforeIdx, then zero the new slot.
// Same pattern Notify_Insert uses to avoid the engine's buggy in-place
// memmove on mid-array insert.
static bool Groups_Insert(int beforeIdx, void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;

    int   sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq         = static_cast<char*>(g_currentSeqPtr);
    void* farrayPtr   = seq + sizeOfFName;   // Groups.Data field
    int   stride      = sizeOfFName;
    int   numBefore   = 0;

    __try { numBefore = *(int*)(seq + sizeOfFName + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (beforeIdx < 0 || beforeIdx > numBefore ||
        numBefore < 0 || numBefore > 256) return false;

    bool grown = false;
    __try
    {
        FArray_Insert(farrayPtr, numBefore, 1, stride);
        grown = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { grown = false; }
    if (!grown) return false;

    __try
    {
        char* data = (char*)*(void**)farrayPtr;
        if (!data) return false;
        if (beforeIdx < numBefore)
        {
            memmove(data + (beforeIdx + 1) * stride,
                    data +  beforeIdx      * stride,
                    (numBefore - beforeIdx) * stride);
        }
        memset(data + beforeIdx * stride, 0, stride);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Groups_Delete(int index, void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    int   sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq         = static_cast<char*>(g_currentSeqPtr);
    __try
    {
        void* data = *(void**)(seq + sizeOfFName);
        int   num  = *(int*) (seq + sizeOfFName + 4);
        if (!data || index < 0 || index >= num) return false;

        char* target = (char*)data + index * sizeOfFName;
        int tail = num - index - 1;
        if (tail > 0)
            memmove(target, target + sizeOfFName, tail * sizeOfFName);
        *(int*)(seq + sizeOfFName + 4) = num - 1;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Groups_Empty(void*)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    int   sizeOfFName = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
    char* seq         = static_cast<char*>(g_currentSeqPtr);
    __try { *(int*)(seq + sizeOfFName + 4) = 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Header Empty/Add button callbacks.  Mirrors the Notifys
// header pattern - confirm before nuking, defer rebuild via PostMessage.
static void GroupsHeader_Empty(HWND grid, int /*rowIdx*/, void* /*ud*/)
{
    if (MessageBoxA(grid, "Remove all groups from this sequence?",
                    "Empty Groups", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (Groups_Empty(nullptr))
        PostMessageA(g_hWnd, WM_AB_REFRESH_SEQ_TAB, 0, 0);
}
static void GroupsHeader_Add(HWND /*grid*/, int /*rowIdx*/, void* /*ud*/)
{
    int n = Groups_Count(nullptr);
    if (Groups_Insert(n, nullptr))
        PostMessageA(g_hWnd, WM_AB_REFRESH_SEQ_TAB, 0, 0);
}

static void RefreshSequenceTab()
{
    if (!g_hWnd) return;
    HWND grid = GetDlgItem(g_hWnd, IDC_AB_SEQ_GRID);
    if (!grid) return;

    // Resolve current sequence selection.
    char seqName[128] = "";
    int sel = static_cast<int>(SendMessageA(g_hSeqList, LB_GETCURSEL, 0, 0));
    if (sel != LB_ERR)
        SendMessageA(g_hSeqList, LB_GETTEXT, sel,
                     reinterpret_cast<LPARAM>(seqName));

    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();

    if (!seqName[0] || pkg.empty() || anim.empty() || g_uobjClassOffset == 0)
    {
        g_currentSeqPtr = nullptr;
        g_curSeqName[0] = '\0';
        g_curStartFrame = g_curNumFrames = 0;
        g_curRate = g_curBookmark = 0.0f;
        g_curGroupsStr[0] = '\0';
    }
    else
    {
        g_currentSeqPtr = GO_FindMeshAnimSeqRaw(g_uobjClassOffset,
                                                pkg.c_str(), anim.c_str(), seqName);
        if (g_currentSeqPtr)
        {
            GO_ReadMeshAnimSeqRaw(g_uobjClassOffset, g_currentSeqPtr,
                                  g_curSeqName, &g_curStartFrame, &g_curNumFrames,
                                  &g_curRate, &g_curBookmark, g_curGroupsStr);
        }
        else
        {
            strncpy_s(g_curSeqName, seqName, _TRUNCATE);
            g_curStartFrame = g_curNumFrames = 0;
            g_curRate = g_curBookmark = 0.0f;
            g_curGroupsStr[0] = '\0';
        }
    }

    // Rebuild grid contents.
    PropertyGrid::BeginUpdate(grid);
    PropertyGrid::Clear(grid);

    PropertyGrid::AddCategory(grid, "SequenceProperties");
    PropertyGrid::AddEditableRow(grid, "SequenceName", Get_SeqName, Set_SeqName, nullptr);
    PropertyGrid::AddEditableRow(grid, "Rate",        Get_SeqRate,  Set_SeqRate,  nullptr);
    PropertyGrid::AddEditableRow(grid, "Compression", Get_SeqCompr, Set_SeqCompr, nullptr);

    PropertyGrid::AddCategory(grid, "Sequence Info");
    PropertyGrid::AddEditableRow(grid, "StartFrame", Get_StartFrame,
                                 [](const char*, void*) {}, nullptr);
    PropertyGrid::AddEditableRow(grid, "NumFrames",  Get_NumFrames,
                                 [](const char*, void*) {}, nullptr);
    PropertyGrid::AddEditableRow(grid, "Bookmark",   Get_Bookmark,
                                 [](const char*, void*) {}, nullptr);

    PropertyGrid::AddCategory(grid, "Groups");
    PropertyGrid::ArrayOps groupsOps = {
        Groups_Count,
        Groups_Get,
        Groups_Set,      // editable elements
        Groups_Insert,   // Insert Above
        Groups_Delete,   // Delete
        Groups_Empty,    // Empty Array
        nullptr          // no per-element children
    };
    int groupsHeaderRow = PropertyGrid::AddArray(grid, "Groups",
                                                 groupsOps, nullptr);
    if (groupsHeaderRow >= 0)
    {
        PropertyGrid::AddRowButton(grid, groupsHeaderRow, "Empty",
                                   GroupsHeader_Empty, nullptr);
        PropertyGrid::AddRowButton(grid, groupsHeaderRow, "Add",
                                   GroupsHeader_Add, nullptr);
    }

    PropertyGrid::EndUpdate(grid);
    RefreshNotifyTab();
}

// =====================================================================
//  Sequence list context menu (UT2004 menu resource 127)
// =====================================================================
//  Right-clicking a sequence opens the surviving "Context" menu:
//    Properties / Rename / Notifications / Groups / Delete / Copy Shortcut
//  Properties / Rename / Groups all just switch the right pane to the
//  Sequence tab; Notifications switches to the Notify tab; Delete
//  removes the sequence from the underlying UMeshAnimation; Copy
//  Shortcut puts the resource path on the clipboard.

#define IDC_AB_SEQ_CTXMENU_RES   127
#define IDM_CTX_PROPERTIES       40537
#define IDM_CTX_RENAME           40540
#define IDM_CTX_NOTIFICATIONS    40538
#define IDM_CTX_GROUPS           40539
#define IDM_CTX_DELETE           40238
#define IDM_CTX_COPYSHORTCUT     40588

static void CopyAnsiToClipboard(const char* text)
{
    if (!text || !*text) return;
    size_t len = std::strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) return;
    void* mem = GlobalLock(hMem);
    if (mem) { memcpy(mem, text, len); GlobalUnlock(hMem); }
    if (OpenClipboard(g_hWnd))
    {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, hMem);
        CloseClipboard();
        // SetClipboardData transfers ownership; do NOT GlobalFree.
    }
    else
    {
        GlobalFree(hMem);
    }
}

static void DeleteCurrentSequence()
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return;
    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();
    if (pkg.empty() || anim.empty() || g_curSeqName[0] == '\0') return;

    char prevName[64] = "";
    strncpy_s(prevName, g_curSeqName, _TRUNCATE);

    if (!GO_DeleteMeshAnimSeqRaw(g_uobjClassOffset,
                                 pkg.c_str(), anim.c_str(), prevName))
    {
        MessageBoxA(g_hWnd, "Failed to delete sequence.",
                    "Message", MB_OK);
        return;
    }

    g_currentSeqPtr = nullptr;
    RefreshSequenceList();
    RefreshSequenceTab();
}

static void ShowSequenceContextMenu(int screenX, int screenY)
{
    HMENU root = LoadMenuA(GetModuleHandleA(nullptr),
                           MAKEINTRESOURCEA(IDC_AB_SEQ_CTXMENU_RES));
    if (!root) return;
    HMENU sub = GetSubMenu(root, 0);
    if (!sub) { DestroyMenu(root); return; }

    UINT cmd = TrackPopupMenu(sub,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        screenX, screenY, 0, g_hWnd, nullptr);
    DestroyMenu(root);

    switch (cmd)
    {
    case IDM_CTX_PROPERTIES:
    case IDM_CTX_RENAME:
    case IDM_CTX_GROUPS:
        SelectTab(AB_TAB_SEQ);
        break;
    case IDM_CTX_NOTIFICATIONS:
        SelectTab(AB_TAB_NOTIFY);
        break;
    case IDM_CTX_DELETE:
        DeleteCurrentSequence();
        break;
    case IDM_CTX_COPYSHORTCUT:
        // Just the bare sequence name - that's what property fields and
        // PlayAnim/LoopAnim/etc. callers actually take as input.
        CopyAnsiToClipboard(g_curSeqName);
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK SeqListSubclassProc(HWND hList, UINT msg,
                                            WPARAM wParam, LPARAM lParam,
                                            UINT_PTR /*idSubclass*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_RBUTTONDOWN)
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // Select the row under the cursor so the menu acts on the right
        // sequence (LBN_SELCHANGE will run as part of LB_SETCURSEL =>
        // the Sequence/Notify tabs refresh automatically).
        LRESULT hit = SendMessageA(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        if (HIWORD(hit) == 0)
        {
            SendMessageA(hList, LB_SETCURSEL, LOWORD(hit), 0);
            // LB_SETCURSEL doesn't fire LBN_SELCHANGE, so refresh manually.
            RefreshSequenceTab();
        }

        POINT pt = { x, y };
        ClientToScreen(hList, &pt);
        ShowSequenceContextMenu(pt.x, pt.y);
        return 0;
    }
    return DefSubclassProc(hList, msg, wParam, lParam);
}

// =====================================================================
//  Notify tab (PropertyGrid array - view/delete/insert/empty)
// =====================================================================
//  FMeshAnimNotify layout (from UnAnim.h, packed-4):
//    +0   FLOAT  Time
//    +4   FName  Function     (sizeOfFName bytes)
//    +4+f UObject* NotifyObject
//  Total = 8 + sizeOfFName.
//
//  The array lives at FMeshAnimSeq + sizeOfFName + 24 (Data/Num/Max).
//  Add/Edit-class needs StaticConstructObject - the next probe.  For
//  this turn we wire: View, Delete, Empty, Insert (creates a None
//  notify with the given Time and no NotifyObject yet).

// Read a single FMeshAnimNotify into a formatted display string:
//   "NotifyFrame=0.500000  AnimNotify_Sound'Pkg.Group.Name'"
// or "NotifyFrame=0.500000  None"
// (Mirrors the expanded child row's "NotifyFrame" label so the array-
// element summary stays consistent with what the user sees inside.)
// SEH-protected.
static void Notify_Get(int index, char* outBuf, int outSize, void* /*userdata*/)
{
    if (outBuf && outSize > 0) outBuf[0] = '\0';
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return;

    __try
    {
        const int sizeOfFName     = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff  = sizeOfFName + 24;
        const int notifysNumOff   = sizeOfFName + 28;
        const int notifyStride    = 8 + sizeOfFName;
        const int notifyObjOffset = 4 + sizeOfFName;

        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || index < 0 || index >= num) return;

        char* p = data + index * notifyStride;
        float t   = *(float*)(p + 0);
        void* obj = *(void**)(p + notifyObjOffset);

        char refBuf[256] = "None";
        if (obj) FormatUObjectRef(obj, refBuf, sizeof(refBuf));

        _snprintf_s(outBuf, outSize, _TRUNCATE,
                    "NotifyFrame=%.6f  %s", t, refBuf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (outBuf && outSize > 0) outBuf[0] = '\0';
    }
}

static int Notify_Count(void* /*userdata*/)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return 0;
    int num = 0;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysNumOff  = sizeOfFName + 28;
        num = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (num >= 0 && num <= 8192) ? num : 0;
}

static bool Notify_Delete(int index, void* /*userdata*/)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;

        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || index < 0 || index >= num) return false;

        char* target = data + index * notifyStride;
        int tail = num - index - 1;
        if (tail > 0)
            memmove(target, target + notifyStride, tail * notifyStride);
        *(int*)((char*)g_currentSeqPtr + notifysNumOff) = num - 1;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Notify_Empty(void* /*userdata*/)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysNumOff  = sizeOfFName + 28;
        *(int*)((char*)g_currentSeqPtr + notifysNumOff) = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// =====================================================================
//  Engine UObject::StaticConstructObject thunk + helpers
// =====================================================================
//  __cdecl UObject* StaticConstructObject(UClass* InClass, UObject* Outer,
//      FName Name, DWORD Flags, UObject* Template, FOutputDevice* Error,
//      UObject* SubobjectRoot)        at 0x10fadf80
//  Error must be non-null - the function asserts otherwise.  We forward
//  the engine's own [0x115befb0] FOutputDevice* (GError-equivalent).
#define STATIC_CONSTRUCT_OBJECT_ADDR    0x10fadf80u
#define GERROR_PTR_ADDR                 0x115befb0u

typedef void* (__cdecl *StaticConstructObject_t)(
    void* cls, void* outer, int fnameIdx, unsigned flags,
    void* tmpl, void* error, void* subobjRoot);

static void* CallStaticConstructObject(void* cls, void* outer,
                                       int fnameIdx, unsigned flags)
{
    if (!cls) return nullptr;
    void* err = nullptr;
    __try { err = *(void**)GERROR_PTR_ADDR; }
    __except (EXCEPTION_EXECUTE_HANDLER) { err = nullptr; }
    if (!err) return nullptr;

    auto fn = reinterpret_cast<StaticConstructObject_t>(
        static_cast<uintptr_t>(STATIC_CONSTRUCT_OBJECT_ADDR));
    void* result = nullptr;
    __try {
        result = fn(cls, outer, fnameIdx, flags, nullptr, err, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }
    return result;
}

// POD-only AnimNotify subclass list: name + UClass pointer.
struct AnimNotifyClassEntry { char name[64]; void* cls; };
#define ANIMNOTIFY_CLASSES_MAX  32
static AnimNotifyClassEntry g_animNotifyClasses[ANIMNOTIFY_CLASSES_MAX];
static int                  g_animNotifyClassCount = 0;

// Walks GObjects looking for UClass instances whose SuperField chain
// includes "AnimNotify" (excluding the abstract base itself).  Result is
// stored in g_animNotifyClasses[] sorted alphabetically.
static void RebuildAnimNotifyClassList()
{
    g_animNotifyClassCount = 0;
    if (g_uobjClassOffset == 0) return;

    __try
    {
        void** gObjData   = *(void***)GOBJECTS_DATA;
        int    gObjNum    = *(int*)   GOBJECTS_NUM;
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gObjData || !gNamesData || gObjNum <= 0 || gNamesNum <= 0) return;

        for (int i = 0; i < gObjNum && g_animNotifyClassCount < ANIMNOTIFY_CLASSES_MAX; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            // obj must be a UClass (its Class.Name == "Class").
            void* cls = *(void**)((char*)obj + g_uobjClassOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            if (_stricmp((char*)clsEntry + FNAME_ENTRY_STR_OFFSET, "Class") != 0)
                continue;

            // Walk SuperField chain looking for an ancestor named "AnimNotify".
            bool isAnimNotifyDescendant = false;
            void* anc = *(void**)((char*)obj + UFIELD_SUPERFIELD_OFFSET);
            for (int s = 0; s < 16 && anc; ++s)
            {
                int aIdx = *(int*)((char*)anc + UOBJ_FNAME_OFFSET);
                if (aIdx >= 0 && aIdx < gNamesNum)
                {
                    void* aEntry = gNamesData[aIdx];
                    if (aEntry &&
                        _stricmp((char*)aEntry + FNAME_ENTRY_STR_OFFSET, "AnimNotify") == 0)
                    { isAnimNotifyDescendant = true; break; }
                }
                anc = *(void**)((char*)anc + UFIELD_SUPERFIELD_OFFSET);
            }
            if (!isAnimNotifyDescendant) continue;

            // Skip the abstract "AnimNotify" base class itself.  Also
            // skip AnimNotify_Scripted (abstract per its UnrealScript
            // declaration) so it doesn't surface as a pickable option
            // in the New dropdown - construction would succeed but the
            // resulting object can't be evaluated by the engine.
            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            void* objEntry = gNamesData[objIdx];
            if (!objEntry) continue;
            const char* objName = (char*)objEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(objName, "AnimNotify")          == 0) continue;
            if (_stricmp(objName, "AnimNotify_Scripted") == 0) continue;

            strncpy_s(g_animNotifyClasses[g_animNotifyClassCount].name,
                      sizeof(g_animNotifyClasses[0].name), objName, _TRUNCATE);
            g_animNotifyClasses[g_animNotifyClassCount].cls = obj;
            g_animNotifyClassCount++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* truncate on AV */ }

    // Alphabetical sort (case-insensitive) for a nicer dropdown.
    for (int i = 1; i < g_animNotifyClassCount; ++i)
    {
        AnimNotifyClassEntry tmp = g_animNotifyClasses[i];
        int j = i - 1;
        while (j >= 0 && _stricmp(g_animNotifyClasses[j].name, tmp.name) > 0)
        {
            g_animNotifyClasses[j + 1] = g_animNotifyClasses[j];
            --j;
        }
        g_animNotifyClasses[j + 1] = tmp;
    }
}

// Finds a UMeshAnimation by (package, name).  We need this to use it as
// Outer for newly-constructed UAnimNotify_* instances.  SEH-protected,
// POD-only inside __try.
static void* GO_FindUMeshAnimRaw(int classOffset,
                                 const char* package, const char* animName)
{
    if (!package || !animName) return nullptr;
    __try
    {
        void** gObjData   = *(void***)GOBJECTS_DATA;
        int    gObjNum    = *(int*)   GOBJECTS_NUM;
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gObjData || !gNamesData) return nullptr;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;
            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            if (_stricmp((char*)clsEntry + FNAME_ENTRY_STR_OFFSET, "MeshAnimation") != 0)
                continue;

            int objIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (objIdx < 0 || objIdx >= gNamesNum) continue;
            void* objEntry = gNamesData[objIdx];
            if (!objEntry) continue;
            if (_stricmp((char*)objEntry + FNAME_ENTRY_STR_OFFSET, animName) != 0)
                continue;

            // Walk to top to compare package.
            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pIdx < 0 || pIdx >= gNamesNum) continue;
            void* pEntry = gNamesData[pIdx];
            if (!pEntry) continue;
            if (_stricmp((char*)pEntry + FNAME_ENTRY_STR_OFFSET, package) != 0)
                continue;

            return obj;
        }
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// =====================================================================
//  Engine FArray::Insert thunk (Ghidra-verified)
// =====================================================================
//  void __thiscall FArray::Insert(FArray* this, INT Index, INT Count,
//                                 INT ElementSize)   at 0x10e2fe70
//  - Grows storage through the engine's own GMalloc (PTR_DAT_115ac708),
//    so the TArray.Data pointer is one the engine's serializer
//    recognizes.  This matters during package save: the engine walks
//    TArray<UObject*> via FArchiveSaveTagExports and dies if Data came
//    from a foreign heap.  Our prior mi_realloc path tripped exactly
//    that AV in USkeletalMesh::Serialize on the very next File>Save.
//  - Memmoves the tail elements down by Count slots, but does NOT zero
//    the newly inserted slots - that's the caller's job.
//  - Callee cleans 12 stack bytes (RET 0xC).
#define FARRAY_INSERT_ADDR  0x10e2fe70u

static void FArray_Insert(void* farray, int index, int count, int elementSize)
{
    void* self = farray;
    int   idx  = index;
    int   cnt  = count;
    int   es   = elementSize;
    int   fn   = FARRAY_INSERT_ADDR;
    __asm
    {
        push es
        push cnt
        push idx
        mov  ecx, self
        mov  eax, fn
        call eax              // __thiscall - callee cleans 12 bytes
    }
}

// Insert at `beforeIdx` (append when beforeIdx == numBefore).  Creates
// a None-class notify with Time = 0.
//
// Implementation note: we use the engine's FArray::Insert to GROW the
// TArray (so storage allocation goes through the engine's GMalloc and
// save serialization works), but call it in APPEND mode only - this
// avoids the engine's internal memmove path for the mid-array shift,
// which on SCCT was observed to leave the original at-index data in
// place and zero the wrong tail slot.  We then do the shift ourselves
// using the standard memmove (same pattern Notify_Delete uses, known
// to work correctly).
static bool Notify_Insert(int beforeIdx, void* /*userdata*/)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;

    // ---- Step 1: validate inside SEH, collect POD inputs. -------------
    int   numBefore  = 0;
    int   stride     = 0;
    void* farrayPtr  = nullptr;
    bool  okPre      = false;

    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;

        int num = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (beforeIdx >= 0 && beforeIdx <= num && num >= 0 && num < 8192)
        {
            numBefore = num;
            stride    = notifyStride;
            // FArray header (Data/Num/Max) sits at notifysDataOff.
            farrayPtr = (char*)g_currentSeqPtr + notifysDataOff;
            okPre     = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { okPre = false; }

    if (!okPre) return false;

    // ---- Step 2: grow via engine FArray::Insert in APPEND mode.
    // Calling with Index == numBefore makes the engine's internal
    // memmove no-op (size = numBefore - numBefore = 0).  We're only
    // here for the realloc + Num++ behavior.
    bool grown = false;
    __try
    {
        FArray_Insert(farrayPtr, numBefore, 1, stride);
        grown = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { grown = false; }

    if (!grown) return false;

    // ---- Step 3: shift [beforeIdx..numBefore-1] down by one slot to
    // free up space at beforeIdx, then zero the freed slot.  This is
    // the bit the engine's memmove was getting wrong; doing it here
    // with the standard CRT memmove handles the forward-overlap
    // correctly.
    __try
    {
        char* data = (char*)*(void**)farrayPtr;
        if (!data) return false;

        if (beforeIdx < numBefore)
        {
            memmove(data + (beforeIdx + 1) * stride,
                    data +  beforeIdx      * stride,
                    (numBefore - beforeIdx) * stride);
        }
        // Zero the new slot: Time=0, Function=NAME_None, NotifyObject=null.
        memset(data + beforeIdx * stride, 0, stride);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// =====================================================================
//  AnimNotify per-element expansion
// =====================================================================
//  When the user clicks the [+] on a Notify[i] row the grid expands to
//  show that notify's editable properties:
//
//    NotifyFrame    (FMeshAnimNotify::Time at slot+0)
//    NotifyName     (FMeshAnimNotify::Function FName at slot+4)
//    <props of the contained UAnimNotify_* object, walked via PropLink>
//
//  Two distinct binding flavors are needed:
//   - PropBinding for UObject-resident properties (the UAnimNotify_*
//     instance pointer is stable while the user edits, so we cache it).
//   - NotifyFieldBinding for the slot-resident fields (Time, Function);
//     these resolve the TArray Data pointer fresh on every read/write
//     because FArray::Insert may have relocated it.

struct NotifyFieldBinding
{
    int elemIdx;
    int offsetInSlot;   // 0 for Time, 4 for Function
    int kind;           // PROP_KIND_FLOAT, PROP_KIND_NAME
};

#define NOTIFY_FIELD_POOL_SIZE  128
static NotifyFieldBinding g_notifyFieldBindings[NOTIFY_FIELD_POOL_SIZE];
static int                g_notifyFieldBindingCount = 0;

static void NotifyFieldBindings_Reset()
{
    g_notifyFieldBindingCount = 0;
}

// =====================================================================
//  ObjectProperty picker (Sound, etc.)
// =====================================================================
//  Backs an editable text row.  The user types or pastes a UObject
//  reference like:
//      Sound'UAS_Weapons.Sentinel.Fire'   (full form, matches FormatUObjectRef)
//      UAS_Weapons.Sentinel.Fire          (path-only is accepted too)
//  and the setter walks GObjects looking for an object whose ref string
//  matches.  A separate "Use" button (added below for Sound properties)
//  consumes the SoundBrowser's currently-selected USound* directly so
//  the user doesn't have to type anything.
//
//  Plain POD binding - no vectors, so a single function can host both
//  this binding's usage AND a __try block without tripping C2712.

struct ObjectPickerBinding
{
    void* obj;       // UObject containing the property
    int   offset;    // byte offset of the property within obj
};

#define OBJ_PICKER_POOL_SIZE   64

// Notify-tab pool: used by Notify[N] -> UAnimNotify_* sub-properties
// (mostly Sound/StopSound's Sound and BoneName fields).
static ObjectPickerBinding g_objPickerBindings[OBJ_PICKER_POOL_SIZE];
static int                 g_objPickerBindingCount = 0;

// Mesh-tab pool: used by USkeletalMesh::DefaultAnim and Material[].
// Separated from the Notify pool so refreshing one tab doesn't clobber
// the other tab's row userdata pointers.
static ObjectPickerBinding g_meshObjPickerBindings[OBJ_PICKER_POOL_SIZE];
static int                 g_meshObjPickerBindingCount = 0;

static void ObjPickerBindings_Reset() { g_objPickerBindingCount = 0; }
static void MeshObjPickerBindings_Reset() { g_meshObjPickerBindingCount = 0; }

static ObjectPickerBinding* ObjPickerBindings_Alloc(void* obj, int offset)
{
    if (g_objPickerBindingCount >= OBJ_PICKER_POOL_SIZE) return nullptr;
    ObjectPickerBinding* b = &g_objPickerBindings[g_objPickerBindingCount++];
    b->obj    = obj;
    b->offset = offset;
    return b;
}

static ObjectPickerBinding* MeshObjPickerBindings_Alloc(void* obj, int offset)
{
    if (g_meshObjPickerBindingCount >= OBJ_PICKER_POOL_SIZE) return nullptr;
    ObjectPickerBinding* b = &g_meshObjPickerBindings[g_meshObjPickerBindingCount++];
    b->obj    = obj;
    b->offset = offset;
    return b;
}

// SEH-isolated reader of UObjectProperty::PropertyClass (UProperty at
// +0x64 in SCCT).  Pulled into its own function so the caller can have
// non-POD locals (std::vector etc.) without tripping C2712.
static void* ReadObjectPropertyClassSafe(void* propPtr)
{
    if (!propPtr) return nullptr;
    __try
    { return *(void**)((char*)propPtr + UOBJPROP_PROPERTYCLASS_OFFSET); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// UStructProperty::Struct sits at the same +0x64 slot as the other
// UProperty subclass extension fields (UProperty's body ends just
// before +0x64 in SCCT).  Returns the UScriptStruct* describing this
// property's inner layout, or nullptr on AV.
static void* ReadStructPropertyStructSafe(void* propPtr)
{
    return ReadObjectPropertyClassSafe(propPtr);
}

// Walk GObjects looking for an object whose FormatUObjectRef string
// matches `refStr`.  Returns nullptr for empty input or "None".  Handles
// both the canonical "Class'Pkg.Group.Name'" form and a bare "Pkg.
// Group.Name" path-only form (Path-only matches the second half of any
// candidate ref).  POD-only inside __try.
static void* GO_FindObjectByRef(const char* refStr)
{
    if (!refStr || !refStr[0]) return nullptr;
    if (_stricmp(refStr, "None") == 0) return nullptr;

    void* found = nullptr;
    __try
    {
        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return nullptr;

        for (int i = 0; i < gObjNum && !found; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            char candidate[256] = "";
            if (!FormatUObjectRef(obj, candidate, sizeof(candidate)))
                continue;

            // Full-form match: "Class'Path'" == "Class'Path'"
            if (_stricmp(candidate, refStr) == 0) { found = obj; break; }

            // Path-only match: input looks like "Pkg.Group.Name" with
            // no apostrophes; compare against candidate's path part
            // (between the apostrophes).
            if (!std::strchr(refStr, '\''))
            {
                const char* lq = std::strchr(candidate, '\'');
                const char* rq = lq ? std::strrchr(candidate, '\'') : nullptr;
                if (lq && rq && rq > lq + 1)
                {
                    size_t plen = (size_t)(rq - (lq + 1));
                    if (std::strlen(refStr) == plen &&
                        _strnicmp(lq + 1, refStr, plen) == 0)
                    { found = obj; break; }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = nullptr; }
    return found;
}

// PropertyGrid getter: reads the current pointer and formats as a
// UObject ref ("None" when null).
static void ObjPicker_Get(char* buf, int size, void* userdata)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    if (!userdata) return;
    ObjectPickerBinding* b = (ObjectPickerBinding*)userdata;
    if (!b->obj) return;

    void* p = nullptr;
    if (!PropRead_Object(b->obj, b->offset, &p)) return;
    if (!p) { strncpy_s(buf, size, "None", _TRUNCATE); return; }
    FormatUObjectRef(p, buf, size);
}

// PropertyGrid setter: resolve the typed/pasted ref string to a
// UObject* via GObjects walk and write it into the property.
static void ObjPicker_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    ObjectPickerBinding* b = (ObjectPickerBinding*)userdata;
    if (!b->obj) return;

    void* newPtr = GO_FindObjectByRef(text);
    // Allow explicit clearing via empty string or "None" (the find
    // helper already returns nullptr for those cases).
    bool clear = (!text[0] || _stricmp(text, "None") == 0);
    if (!newPtr && !clear) return;   // unknown ref - leave property alone

    __try { *(void**)((char*)b->obj + b->offset) = newPtr; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// =====================================================================
//  Fix: struct-member binding for Vector / Rotator children
// =====================================================================
//  Carries enough context for the child setter to (a) write its single
//  scalar component, AND (b) rebuild the parent's summary row text so
//  the collapsed "(X=..,Y=..,Z=..)" display reflects the new value.
//  Without this, the parent text gets stale the moment the user edits
//  X/Y/Z, because plain PropBinding doesn't know about siblings.

struct StructMemberBinding
{
    void* obj;          // UAnimNotify_* instance
    int   structBase;   // byte offset of the parent struct within obj
    int   memberIdx;    // 0/1/2 - which component (X/Y/Z or Pitch/Yaw/Roll)
    int   kind;         // PROP_KIND_FLOAT (Vector) or PROP_KIND_INT (Rotator)
    HWND  grid;
    int   parentRowIdx;
};

#define STRUCT_MEMBER_POOL_SIZE 96
static StructMemberBinding g_structMemberBindings[STRUCT_MEMBER_POOL_SIZE];
static int                 g_structMemberBindingCount = 0;

static void StructMemberBindings_Reset() { g_structMemberBindingCount = 0; }

static StructMemberBinding* StructMemberBindings_Alloc(
    void* obj, int structBase, int memberIdx, int kind,
    HWND grid, int parentRowIdx)
{
    if (g_structMemberBindingCount >= STRUCT_MEMBER_POOL_SIZE) return nullptr;
    StructMemberBinding* b = &g_structMemberBindings[g_structMemberBindingCount++];
    b->obj          = obj;
    b->structBase   = structBase;
    b->memberIdx    = memberIdx;
    b->kind         = kind;
    b->grid         = grid;
    b->parentRowIdx = parentRowIdx;
    return b;
}

// Forward decls: defined in the typed-accessor library further down.
static bool PropWrite_Float(void* obj, int off, float v);
static bool PropWrite_Int  (void* obj, int off, int v);

static void StructMember_Get(char* buf, int size, void* userdata)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    if (!userdata) return;
    StructMemberBinding* b = (StructMemberBinding*)userdata;
    int off = b->structBase + b->memberIdx * 4;
    if (b->kind == PROP_KIND_FLOAT)
    {
        float v = 0;
        if (PropRead_Float(b->obj, off, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%.6f", v);
    }
    else
    {
        int v = 0;
        if (PropRead_Int(b->obj, off, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%d", v);
    }
}

// Rebuild the parent row's summary string and push it into the grid.
static void StructMember_RefreshParentSummary(StructMemberBinding* b)
{
    if (!b || !b->grid || b->parentRowIdx < 0) return;
    char summary[160] = "";
    if (b->kind == PROP_KIND_FLOAT)
    {
        float x = 0, y = 0, z = 0;
        PropRead_Float(b->obj, b->structBase + 0, &x);
        PropRead_Float(b->obj, b->structBase + 4, &y);
        PropRead_Float(b->obj, b->structBase + 8, &z);
        _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                    "(X=%.6f,Y=%.6f,Z=%.6f)", x, y, z);
    }
    else
    {
        int p = 0, yw = 0, r = 0;
        PropRead_Int(b->obj, b->structBase + 0, &p);
        PropRead_Int(b->obj, b->structBase + 4, &yw);
        PropRead_Int(b->obj, b->structBase + 8, &r);
        _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                    "(Pitch=%d,Yaw=%d,Roll=%d)", p, yw, r);
    }
    PropertyGrid::SetRowValue(b->grid, b->parentRowIdx, summary);
}

static void StructMember_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    StructMemberBinding* b = (StructMemberBinding*)userdata;
    int off = b->structBase + b->memberIdx * 4;
    if (b->kind == PROP_KIND_FLOAT)
        PropWrite_Float(b->obj, off, (float)atof(text));
    else
        PropWrite_Int  (b->obj, off, atoi(text));
    StructMember_RefreshParentSummary(b);
}

// =====================================================================
//  ClassProperty dropdown (EffectClass etc.)
// =====================================================================
//  UnrealEd 2 shows the full GObjects class roster (Actor, Pawn, every
//  Engine + game UClass) in the EffectClass picker - even classes the
//  property's MetaClass theoretically doesn't accept.  We replicate
//  that "sloppy" behavior on purpose; users want the complete list.

struct ClassPickerBinding
{
    void* obj;
    int   offset;
};

#define CLASS_PICKER_POOL_SIZE 16
static ClassPickerBinding g_classPickerBindings[CLASS_PICKER_POOL_SIZE];
static int                g_classPickerBindingCount = 0;

static void ClassPickerBindings_Reset() { g_classPickerBindingCount = 0; }
static ClassPickerBinding* ClassPickerBindings_Alloc(void* obj, int offset)
{
    if (g_classPickerBindingCount >= CLASS_PICKER_POOL_SIZE) return nullptr;
    ClassPickerBinding* b = &g_classPickerBindings[g_classPickerBindingCount++];
    b->obj    = obj;
    b->offset = offset;
    return b;
}

// Global, lazily-rebuilt class roster.  Each entry stores BOTH the
// short class name ("Pawn", shown in the dropdown options) and the
// full canonical reference ("Class'Engine.Pawn'", stored in the
// property after picking).  Capped at 1024 entries (SCCT loads ~1100
// classes; 1024 covers the typical Engine + Editor + script set).
struct ClassEntry { char name[64]; char ref[160]; void* cls; };
#define CLASS_ROSTER_MAX 1024
static ClassEntry  g_classRoster[CLASS_ROSTER_MAX];
static int         g_classRosterCount = 0;
static const char* g_classRosterOpts[CLASS_ROSTER_MAX + 1]; // +1 for "None"

static void RebuildClassRoster()
{
    g_classRosterCount = 0;
    if (g_uobjClassOffset == 0) return;

    // POD-only collection inside SEH so we can format ref strings
    // (which calls FormatUObjectRef, itself SEH-protected) outside
    // this block without C2712 risk.
    static void* matches[CLASS_ROSTER_MAX];
    int matchCount = 0;

    __try
    {
        void** gObjData   = *(void***)GOBJECTS_DATA;
        int    gObjNum    = *(int*)   GOBJECTS_NUM;
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gObjData || !gNamesData) return;

        for (int i = 0; i < gObjNum && matchCount < CLASS_ROSTER_MAX; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            void* cls = *(void**)((char*)obj + g_uobjClassOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx < 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            if (_stricmp((char*)clsEntry + FNAME_ENTRY_STR_OFFSET, "Class") != 0)
                continue;

            matches[matchCount++] = obj;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* truncate */ }

    // Format each into canonical "Class'Package.Name'" form (full
    // ref), then extract just the class-name portion ("Pawn") for the
    // dropdown label.
    for (int i = 0; i < matchCount; ++i)
    {
        ClassEntry* e = &g_classRoster[g_classRosterCount];
        FormatUObjectRef(matches[i], e->ref, sizeof(e->ref));
        e->cls = matches[i];

        // Extract short name: text between the last '.' (or first ')
        // and the closing '.  E.g. "Class'Engine.Pawn'" -> "Pawn".
        e->name[0] = '\0';
        const char* dot = std::strrchr(e->ref, '.');
        const char* lq  = std::strchr (e->ref, '\'');
        const char* nameStart = dot ? dot + 1 : (lq ? lq + 1 : e->ref);
        const char* nameEnd   = std::strchr(nameStart, '\'');
        size_t nlen = nameEnd ? (size_t)(nameEnd - nameStart)
                              : std::strlen(nameStart);
        if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
        std::memcpy(e->name, nameStart, nlen);
        e->name[nlen] = '\0';

        g_classRosterCount++;
    }

    // Alphabetical sort by short name so the dropdown reads as a clean
    // class list (Actor, Brush, Pawn, ...) - the order UnrealEd 2
    // displays them.
    for (int i = 1; i < g_classRosterCount; ++i)
    {
        ClassEntry tmp = g_classRoster[i];
        int j = i - 1;
        while (j >= 0 && _stricmp(g_classRoster[j].name, tmp.name) > 0)
        {
            g_classRoster[j + 1] = g_classRoster[j];
            --j;
        }
        g_classRoster[j + 1] = tmp;
    }
}

static void ClassPicker_Get(char* buf, int size, void* userdata)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    if (!userdata) return;
    ClassPickerBinding* b = (ClassPickerBinding*)userdata;
    void* p = nullptr;
    if (!PropRead_Object(b->obj, b->offset, &p)) return;
    if (!p) { strncpy_s(buf, size, "None", _TRUNCATE); return; }
    // Full canonical ref "Class'Package.ClassName'" - matches what
    // the dropdown options below show, so CB_FINDSTRINGEXACT pre-
    // selects the right row when the combo opens.
    FormatUObjectRef(p, buf, size);
}

static void ClassPicker_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    ClassPickerBinding* b = (ClassPickerBinding*)userdata;

    void* newPtr = nullptr;
    if (text[0] && _stricmp(text, "None") != 0)
    {
        // Dropdown options are short names ("Pawn"); the cached row
        // value the user might see is the full ref ("Class'Engine.
        // Pawn'") - accept either for robustness against a user who
        // pastes a ref string into the combo's edit (when we ever
        // make it CBS_DROPDOWN instead of CBS_DROPDOWNLIST).
        for (int i = 0; i < g_classRosterCount; ++i)
        {
            if (_stricmp(g_classRoster[i].name, text) == 0 ||
                _stricmp(g_classRoster[i].ref,  text) == 0)
            {
                newPtr = g_classRoster[i].cls;
                break;
            }
        }
        if (!newPtr) return; // unknown - leave property alone
    }

    __try { *(void**)((char*)b->obj + b->offset) = newPtr; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-isolated walk of the UField SuperField chain, comparing each
// class's Name FName against a target.  Used to decide whether to
// surface UnrealEd 2's "Use" button on an object property row.
static bool IsClassDescendantNamed(void* cls, const char* targetName)
{
    if (!cls || !targetName) return false;

    int guard = 0;
    while (cls && guard < 16)
    {
        int   nameIdx = -1;
        void* nextCls = nullptr;
        __try
        {
            nameIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            nextCls = *(void**)((char*)cls + UFIELD_SUPERFIELD_OFFSET);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        const char* name = FNameIdx_PeekRaw(nameIdx);
        if (name && _stricmp(name, targetName) == 0) return true;

        cls = nextCls;
        ++guard;
    }
    return false;
}

// "Use" button callback for Sound-typed object properties.  Reads the
// USound* currently selected in the user's SoundBrowser (cached by
// SoundBrowser.cpp's StoreLParamHelper) and writes it into the
// property.  Refreshes the row so the new ref displays immediately.
static void ObjPicker_UseSelectedSound(HWND grid, int rowIdx, void* userdata)
{
    if (!userdata) return;
    ObjectPickerBinding* b = (ObjectPickerBinding*)userdata;
    if (!b->obj) return;

    void* picked = SoundBrowser::PeekSelectedSound();
    if (!picked) return;   // no browser open, or no item selected

    __try { *(void**)((char*)b->obj + b->offset) = picked; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    PropertyGrid::RefreshRow(grid, rowIdx);
}

// "Clear" button callback - writes nullptr into the property so the row
// shows "None".  Same UX UnrealEd 2 ships next to its Use button.
static void ObjPicker_Clear(HWND grid, int rowIdx, void* userdata)
{
    if (!userdata) return;
    ObjectPickerBinding* b = (ObjectPickerBinding*)userdata;
    if (!b->obj) return;

    __try { *(void**)((char*)b->obj + b->offset) = nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    PropertyGrid::RefreshRow(grid, rowIdx);
}

static NotifyFieldBinding* NotifyFieldBindings_Alloc(int elemIdx, int slotOff, int kind)
{
    if (g_notifyFieldBindingCount >= NOTIFY_FIELD_POOL_SIZE) return nullptr;
    NotifyFieldBinding* b = &g_notifyFieldBindings[g_notifyFieldBindingCount++];
    b->elemIdx      = elemIdx;
    b->offsetInSlot = slotOff;
    b->kind         = kind;
    return b;
}

// SEH-isolated readers/writers for the slot fields.  Resolve the current
// Data pointer + slot address on every call so we tolerate FArray::Insert
// relocations and seq-pointer changes.  POD only - safe with __try.

static bool NotifySlot_ReadFloat(int elemIdx, int slotOff, float* out)
{
    *out = 0.0f;
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return false;
        *out = *(float*)(data + elemIdx * notifyStride + slotOff);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool NotifySlot_WriteFloat(int elemIdx, int slotOff, float v)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return false;
        *(float*)(data + elemIdx * notifyStride + slotOff) = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool NotifySlot_ReadNameIdx(int elemIdx, int slotOff, int* out)
{
    *out = 0;
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return false;
        *out = *(int*)(data + elemIdx * notifyStride + slotOff);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool NotifySlot_WriteNameIdx(int elemIdx, int slotOff, int idx)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName    = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff = sizeOfFName + 24;
        const int notifysNumOff  = sizeOfFName + 28;
        const int notifyStride   = 8 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return false;
        *(int*)(data + elemIdx * notifyStride + slotOff) = idx;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-isolated read of a notify slot's NotifyObject pointer.
static void* NotifySlot_ReadObjectPtr(int elemIdx)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return nullptr;
    __try
    {
        const int sizeOfFName     = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff  = sizeOfFName + 24;
        const int notifysNumOff   = sizeOfFName + 28;
        const int notifyStride    = 8 + sizeOfFName;
        const int notifyObjOffset = 4 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return nullptr;
        return *(void**)(data + elemIdx * notifyStride + notifyObjOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// PropertyGrid getter/setter dispatchers for NotifyFieldBinding.

static void NotifyField_Get(char* buf, int size, void* userdata)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    if (!userdata) return;
    NotifyFieldBinding* b = (NotifyFieldBinding*)userdata;

    switch (b->kind)
    {
    case PROP_KIND_FLOAT:
    {
        float v = 0.0f;
        if (NotifySlot_ReadFloat(b->elemIdx, b->offsetInSlot, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%.6f", v);
        break;
    }
    case PROP_KIND_NAME:
    {
        int idx = 0;
        if (NotifySlot_ReadNameIdx(b->elemIdx, b->offsetInSlot, &idx))
            FNameIdx_GetStr(idx, buf, size);
        break;
    }
    default:
        break;
    }
}

static void NotifyField_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    NotifyFieldBinding* b = (NotifyFieldBinding*)userdata;

    switch (b->kind)
    {
    case PROP_KIND_FLOAT:
        NotifySlot_WriteFloat(b->elemIdx, b->offsetInSlot, (float)atof(text));
        break;
    case PROP_KIND_NAME:
    {
        int idx = (text[0]) ? FName_Intern(text, FNAME_ADD) : 0;
        NotifySlot_WriteNameIdx(b->elemIdx, b->offsetInSlot, idx);
        break;
    }
    default:
        break;
    }
}

// Per-property add helper used as a callback to UProp_ForEachEditable.
struct NotifyChildPopulateCtx
{
    HWND  grid;
    int   elemRow;
    void* obj;
};

static bool Notify_AddPropRow(const UPropInfo& info, void* userdata)
{
    NotifyChildPopulateCtx* ctx = (NotifyChildPopulateCtx*)userdata;
    if (!ctx) return false;

    int kind = PropKindFromTypeName(info.typeName);

    // Vector / Rotator struct properties expand inline into
    // three editable scalar children (X/Y/Z floats for Vector, Pitch/
    // Yaw/Roll INTs for Rotator).  Anything else is shown as the
    // generic "(struct)" placeholder.
    if (kind == PROP_KIND_STRUCT)
    {
        void* structCls = ReadStructPropertyStructSafe(info.propPtr);

        char structName[64] = "";
        if (structCls)
        {
            int idx = -1;
            PropRead_Int(structCls, UOBJ_FNAME_OFFSET, &idx);
            if (idx >= 0) FNameIdx_GetStr(idx, structName, sizeof(structName));
        }

        bool isVector  = (_stricmp(structName, "Vector")  == 0);
        bool isRotator = (_stricmp(structName, "Rotator") == 0);

        if (isVector || isRotator)
        {
            // Build a one-line summary for the collapsed parent row.
            // Same look UnrealEd shows in its WObjectProperties grid.
            char summary[160] = "";
            if (isVector)
            {
                float x = 0, y = 0, z = 0;
                PropRead_Float(ctx->obj, info.offset + 0, &x);
                PropRead_Float(ctx->obj, info.offset + 4, &y);
                PropRead_Float(ctx->obj, info.offset + 8, &z);
                _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                            "(X=%.6f,Y=%.6f,Z=%.6f)", x, y, z);
            }
            else
            {
                int p = 0, yw = 0, r = 0;
                PropRead_Int(ctx->obj, info.offset + 0, &p);
                PropRead_Int(ctx->obj, info.offset + 4, &yw);
                PropRead_Int(ctx->obj, info.offset + 8, &r);
                _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                            "(Pitch=%d,Yaw=%d,Roll=%d)", p, yw, r);
            }

            int parentRow = PropertyGrid::AddRowAt(
                ctx->grid, ctx->elemRow, info.name, summary);

            if (parentRow >= 0)
            {
                // Vector member order is X,Y,Z; Rotator is Pitch,Yaw,
                // Roll (the engine's native field order - NOT the
                // alphabetical "P/R/Y" people sometimes assume).
                const char* labels[3];
                int         kindElem;
                if (isVector)
                {
                    labels[0] = "X";     labels[1] = "Y";   labels[2] = "Z";
                    kindElem  = PROP_KIND_FLOAT;
                }
                else
                {
                    labels[0] = "Pitch"; labels[1] = "Yaw"; labels[2] = "Roll";
                    kindElem  = PROP_KIND_INT;
                }
                // Use StructMemberBinding instead of plain PropBinding
                // so the child setter can also refresh the parent row's
                // summary string ("(X=...,Y=...,Z=...)") whenever it
                // writes - otherwise the collapsed display goes stale.
                for (int i = 0; i < 3; ++i)
                {
                    StructMemberBinding* smb = StructMemberBindings_Alloc(
                        ctx->obj, info.offset, i, kindElem,
                        ctx->grid, parentRow);
                    if (smb)
                        PropertyGrid::AddEditableRowAt(
                            ctx->grid, parentRow, labels[i],
                            StructMember_Get, StructMember_Set, smb);
                }
            }
            return true;
        }
        // Fall through to PropBinding's "(struct)" placeholder for
        // anything we don't yet have a custom layout for.
    }

    // BoolProperty gets a True/False dropdown (matches the
    // UnrealEd 2 widget style and the dropdown UX you requested for
    // other typed properties).  PropBinding's setter writes via
    // PropWrite_Byte so toggling sticks.
    if (kind == PROP_KIND_BOOL)
    {
        PropBinding* b = PropBindings_Alloc(ctx->obj, info.offset, kind);
        if (b)
        {
            static const char* boolOpts[] = { "False", "True" };
            PropertyGrid::AddEnumRowAt(
                ctx->grid, ctx->elemRow, info.name,
                boolOpts, 2,
                PropBinding_Get, PropBinding_Set, b);
            return true;
        }
    }

    // ClassProperty (e.g. AnimNotify_Effect's EffectClass)
    // gets a dropdown listing every loaded UClass.  UnrealEd 2 does
    // the same - even classes that wouldn't actually fit the
    // MetaClass restriction show up; "shit like Pawn in there" per
    // the request.
    if (kind == PROP_KIND_CLASS)
    {
        ClassPickerBinding* cpb = ClassPickerBindings_Alloc(
            ctx->obj, info.offset);
        if (cpb)
        {
            RebuildClassRoster();
            g_classRosterOpts[0] = "None";
            for (int i = 0; i < g_classRosterCount; ++i)
                g_classRosterOpts[i + 1] = g_classRoster[i].name;
            int optsCount = g_classRosterCount + 1;
            PropertyGrid::AddEnumRowAt(
                ctx->grid, ctx->elemRow, info.name,
                g_classRosterOpts, optsCount,
                ClassPicker_Get, ClassPicker_Set, cpb);
            return true;
        }
        // Fall through if binding pool exhausted.
    }

    // ObjectProperty becomes an editable text row plus, for
    // USound-derived properties, a "Use" button that consumes the
    // SoundBrowser's currently-selected USound (UE2's "use current"
    // pattern).  No dropdown - the user types/pastes the reference
    // string OR clicks Use.
    if (kind == PROP_KIND_OBJECT)
    {
        void* propClass = ReadObjectPropertyClassSafe(info.propPtr);

        ObjectPickerBinding* opb = ObjPickerBindings_Alloc(ctx->obj,
                                                           info.offset);
        if (opb)
        {
            int newRow = PropertyGrid::AddEditableRowAt(
                ctx->grid, ctx->elemRow, info.name,
                ObjPicker_Get, ObjPicker_Set, opb);

            if (newRow >= 0 && IsClassDescendantNamed(propClass, "Sound"))
            {
                // Buttons are drawn in the order added (leftmost first),
                // so Clear appears to the left of Use.
                PropertyGrid::AddRowButton(
                    ctx->grid, newRow, "Clear",
                    ObjPicker_Clear, opb,
                    PropertyGrid::BTN_VIS_SELECTED);
                PropertyGrid::AddRowButton(
                    ctx->grid, newRow, "Use",
                    ObjPicker_UseSelectedSound, opb,
                    PropertyGrid::BTN_VIS_SELECTED);
            }
            return true;
        }
        // Fall through to read-only display if the binding pool is full.
    }

    PropBinding* b = PropBindings_Alloc(ctx->obj, info.offset, kind);
    if (!b) return false;

    if (PropKindIsEditable(kind))
    {
        PropertyGrid::AddEditableRowAt(ctx->grid, ctx->elemRow, info.name,
                                       PropBinding_Get, PropBinding_Set, b);
    }
    else
    {
        // Read-only display row for kinds we haven't wired write paths for
        // yet (Class/Str/Bool/Struct/Array).
        char vbuf[256] = "";
        PropBinding_Get(vbuf, sizeof(vbuf), b);
        PropertyGrid::AddRowAt(ctx->grid, ctx->elemRow, info.name, vbuf);
    }
    return true;
}

// =====================================================================
//  NewObjectBinding - "New = <class>" inline class picker
// =====================================================================
//  Surfaces under a null Notify slot to let the user pick a class and
//  StaticConstructObject a fresh UAnimNotify_*.  UT2004 ships this as
//  a class dropdown + separate "New" button; we mirror that exactly:
//    - Selecting a class from the dropdown only updates b->chosenClass.
//    - Clicking the "New" button reads b->chosenClass and constructs
//      the object via the engine's StaticConstructObject.
//
//  After creation we also set g_pendingExpandNotifyIdx so the rebuilt
//  tab auto-expands the just-created element + its Notify subrow.
//
//  Refresh after creation is deferred via PostMessage(WM_AB_REFRESH_
//  NOTIFY_TAB) so we don't tear down the row that's mid-EndInlineEdit.

#define WM_AB_REFRESH_NOTIFY_TAB    (WM_USER + 0x100)
#define WM_AB_REFRESH_MESH_TAB      (WM_USER + 0x101)
// WM_AB_REFRESH_SEQ_TAB is forward-defined in the forward-
// decl block earlier in the file (Groups_Insert uses it).

// Element index to auto-expand on the next RefreshNotifyTab pass.  -1
// when nothing is pending.  Cleared after the pending expand fires.
static int g_pendingExpandNotifyIdx = -1;

struct NewObjectBinding
{
    int  elemIdx;
    char chosenClass[64];   // updated whenever the dropdown commits
};
#define NEWOBJ_BINDING_POOL_SIZE   64
static NewObjectBinding g_newObjBindings[NEWOBJ_BINDING_POOL_SIZE];
static int              g_newObjBindingCount = 0;

static void NewObjBindings_Reset() { g_newObjBindingCount = 0; }

static NewObjectBinding* NewObjBindings_Alloc(int elemIdx)
{
    if (g_newObjBindingCount >= NEWOBJ_BINDING_POOL_SIZE) return nullptr;
    NewObjectBinding* b = &g_newObjBindings[g_newObjBindingCount++];
    b->elemIdx        = elemIdx;
    b->chosenClass[0] = '\0';
    return b;
}

// Getter: returns the class name the user has currently picked in the
// dropdown, or the first available class as the default if nothing's
// been picked yet (so the combo opens with a sensible pre-selection).
static void NewObject_Get(char* buf, int size, void* ud)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    NewObjectBinding* b = (NewObjectBinding*)ud;
    if (b && b->chosenClass[0])
    {
        strncpy_s(buf, size, b->chosenClass, _TRUNCATE);
    }
    else if (g_animNotifyClassCount > 0)
    {
        strncpy_s(buf, size, g_animNotifyClasses[0].name, _TRUNCATE);
    }
}

// SEH-isolated write of the NotifyObject pointer at slot+4+sizeOfFName.
static bool NotifySlot_WriteObjectPtr(int elemIdx, void* newObj)
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return false;
    __try
    {
        const int sizeOfFName     = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
        const int notifysDataOff  = sizeOfFName + 24;
        const int notifysNumOff   = sizeOfFName + 28;
        const int notifyStride    = 8 + sizeOfFName;
        const int notifyObjOffset = 4 + sizeOfFName;
        char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
        int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
        if (!data || elemIdx < 0 || elemIdx >= num) return false;
        *(void**)(data + elemIdx * notifyStride + notifyObjOffset) = newObj;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-isolated read of meshAnim->Outer.  Pulled into its own function so
// the caller can use std::string without tripping C2712.
static void* ReadOuterSafe(void* obj)
{
    if (!obj) return nullptr;
    __try { return *(void**)((char*)obj + UOBJ_OUTER_OFFSET); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Combo setter: just stores the chosen class.  Does NOT create the
// object - that happens only when the user clicks the "New" button so
// the UX matches UT2004's two-step "pick then commit" pattern.
static void NewObject_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    NewObjectBinding* b = (NewObjectBinding*)userdata;
    strncpy_s(b->chosenClass, sizeof(b->chosenClass), text, _TRUNCATE);
}

// "New" button callback - reads b->chosenClass, constructs a fresh
// UAnimNotify_* via engine StaticConstructObject, assigns it to the
// notify slot, and queues a tab refresh.  Sets g_pendingExpand so the
// new element auto-expands on the next populate pass.
static void NewObject_DoCreate(HWND /*grid*/, int /*rowIdx*/, void* userdata)
{
    if (!userdata) return;
    NewObjectBinding* b = (NewObjectBinding*)userdata;

    // If the user never opened the dropdown, default to the first class
    // (matches what the getter pre-populates in the combo display).
    const char* className = b->chosenClass[0] ? b->chosenClass :
        (g_animNotifyClassCount > 0 ? g_animNotifyClasses[0].name : nullptr);
    if (!className) return;

    void* cls = nullptr;
    for (int i = 0; i < g_animNotifyClassCount; ++i)
    {
        if (_stricmp(g_animNotifyClasses[i].name, className) == 0)
        {
            cls = g_animNotifyClasses[i].cls;
            break;
        }
    }
    if (!cls) return;

    // Outer = the UMeshAnimation's Outer (i.e., the containing package).
    // Mirrors what SCCT's existing AnimNotify_* objects have:
    // "AnimNotify_Sound'SPerso.AnimNotify_Sound133'" - Outer is SPerso
    // (the package), not the UMeshAnimation under it.
    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();
    void* meshAnim = GO_FindUMeshAnimRaw(g_uobjClassOffset,
                                         pkg.c_str(), anim.c_str());
    if (!meshAnim) return;
    void* outer = ReadOuterSafe(meshAnim);
    if (!outer) return;

    // Flags=0 matches UT2004 ConstructObject<T>() default.  The new
    // object lives under the package via Outer, so the package saver
    // tags+serializes it when reached from FMeshAnimNotify::NotifyObject.
    void* newObj = CallStaticConstructObject(cls, outer, /*Name=*/0,
                                             /*Flags=*/0);
    if (!newObj) return;

    NotifySlot_WriteObjectPtr(b->elemIdx, newObj);

    // Auto-expand the new element and its Notify subrow on next refresh
    // so the user sees the editable Sound/Volume/etc. fields immediately.
    g_pendingExpandNotifyIdx = b->elemIdx;

    // Defer the rebuild until after EndInlineEdit cleans up - clearing
    // rows[] here would invalidate the in-progress edit's row reference.
    PostMessageA(g_hWnd, WM_AB_REFRESH_NOTIFY_TAB, 0, 0);
}

// =====================================================================
//  Notifys array header action buttons
// =====================================================================
//  Empty: calls ops.empty (wipes the array)
//  Add:   calls ops.insert with index == count (append)
//  Both end up routing through RebuildArrayElementsWithChildren so the
//  Notify subtree rebuilds correctly after the mutation.

static void NotifysHeader_Empty(HWND grid, int /*rowIdx*/, void* /*ud*/)
{
    // Confirm so a stray click doesn't blow the whole list away.
    if (MessageBoxA(grid, "Remove all notifies from this sequence?",
                    "Message", MB_YESNO) != IDYES)
        return;
    if (Notify_Empty(nullptr))
        PostMessageA(g_hWnd, WM_AB_REFRESH_NOTIFY_TAB, 0, 0);
}

static void NotifysHeader_Add(HWND /*grid*/, int /*rowIdx*/, void* /*ud*/)
{
    int n = Notify_Count(nullptr);
    if (Notify_Insert(n, nullptr))
    {
        // Auto-expand the new (last) element so the user immediately sees
        // its Notify=None / New dropdown without an extra click.
        g_pendingExpandNotifyIdx = n;
        PostMessageA(g_hWnd, WM_AB_REFRESH_NOTIFY_TAB, 0, 0);
    }
}

// =====================================================================
//  Per-element Delete / Insert button callbacks
// =====================================================================
//  Delete: remove the notify at elemIdx.
//  Insert: prepend a new (null-NotifyObject) slot before elemIdx and
//          auto-expand it so the user immediately sees its New picker.
//  userdata carries the element index as intptr_t.

static void NotifyElement_Delete(HWND grid, int /*rowIdx*/, void* ud)
{
    int elemIdx = (int)(intptr_t)ud;
    if (Notify_Delete(elemIdx, nullptr))
    {
        // Shift per-element expand state down so the surviving entries
        // keep their expansion at the correct (now shifted-down) index.
        PropertyGrid::ShiftArrayElementExpansion(grid, "Notifys",
                                                 elemIdx, -1);
        PostMessageA(g_hWnd, WM_AB_REFRESH_NOTIFY_TAB, 0, 0);
    }
}

static void NotifyElement_Insert(HWND grid, int /*rowIdx*/, void* ud)
{
    // "Insert Above" semantics (matches UT2004): clicking Insert on [N]
    // creates a new empty slot at [N] and shifts the existing entry
    // (and everything after it) down by one.  We DO NOT auto-expand
    // the new empty slot here - the user's previously-expanded entry
    // (which just shifted to [N+1] via the persistence-key shift)
    // stays visibly expanded instead of being hidden under a brand-new
    // "Notify=None" block.
    int elemIdx = (int)(intptr_t)ud;
    if (Notify_Insert(elemIdx, nullptr))
    {
        PropertyGrid::ShiftArrayElementExpansion(grid, "Notifys",
                                                 elemIdx, +1);
        PostMessageA(g_hWnd, WM_AB_REFRESH_NOTIFY_TAB, 0, 0);
    }
}

// PropertyGrid populateChildren callback - invoked once per Notify[i].
// Layout matches UT2004's WBrowserAnimation / WObjectProperties:
//
//   [N]                                       <- the array element row
//     Notify = AnimNotify_Sound'...'          <- read-only object ref
//       AnimNotify_Sound Pkg.Group.Name       <- inline expand header
//         Sound                               <- editable properties of
//         Volume                                 the contained UAnimNotify
//         Radius                                 walked via PropertyLink
//         BoneName
//     NotifyFrame = 0.500000                  <- editable slot.Time
//
// When NotifyObject is null we instead show:
//   [N]
//     Notify = None
//       New                                   <- dropdown of subclasses;
//                                                picking commits via
//                                                StaticConstructObject
//     NotifyFrame = 0.000000
//
// Note we intentionally do NOT expose FMeshAnimNotify::Function (the
// FName at slot+4).  Editing it without rebinding the class corrupts
// the engine's notify-class dispatch and the Sound notify becomes a
// generic script notify on next save+reload.  The notify class is
// determined by the NotifyObject pointer, not by Function.
static void Notify_PopulateChildren(HWND grid, int elemRow, int elemIdx, void* /*userdata*/)
{
    if (!grid || g_uobjClassOffset == 0) return;

    // per-element Delete / Insert buttons.  Visible only when
    // [N] or any descendant (Notify subrow, NotifyFrame, Sound, etc.)
    // is the selected row - matches UT2004's context-sensitive UI.
    void* elemUd = (void*)(intptr_t)elemIdx;
    PropertyGrid::AddRowButton(grid, elemRow, "Insert",
                               NotifyElement_Insert, elemUd,
                               PropertyGrid::BTN_VIS_SELECTED_OR_DESCENDANT);
    PropertyGrid::AddRowButton(grid, elemRow, "Delete",
                               NotifyElement_Delete, elemUd,
                               PropertyGrid::BTN_VIS_SELECTED_OR_DESCENDANT);

    // 1. Notify = <objref>  (read-only row, expandable always)
    void* obj = NotifySlot_ReadObjectPtr(elemIdx);
    char  refBuf[256] = "None";
    if (obj) FormatUObjectRef(obj, refBuf, sizeof(refBuf));

    int notifyRow = PropertyGrid::AddRowAt(grid, elemRow, "Notify", refBuf);

    if (notifyRow >= 0)
    {
        if (obj)
        {
            // 2a. Inline object header + property rows for the contained
            // UAnimNotify_*.  See UT2004 WObjectProperties for the
            // "ClassName Pkg.Group.Name" header convention.
            char header[256] = "";
            strncpy_s(header, sizeof(header), refBuf, _TRUNCATE);
            for (char* p = header; *p; ++p)
                if (*p == '\'') *p = ' ';
            size_t hl = std::strlen(header);
            while (hl > 0 && header[hl - 1] == ' ') header[--hl] = '\0';

            int headerRow = PropertyGrid::AddRowAt(grid, notifyRow, header, "");
            if (headerRow >= 0)
            {
                NotifyChildPopulateCtx ctx = { grid, headerRow, obj };
                UProp_ForEachEditable(obj, &Notify_AddPropRow, &ctx);

                // SCCT-specific hardcoded fields: a few AnimNotify
                // subclasses ship in the editor with the relevant
                // properties stripped of UProperty reflection
                // entirely (they exist as plain native C++ members,
                // not in the class's PropertyLink), so the walker
                // above can't find them no matter what filter we use.
                // We special-case those subclasses by name and bind
                // editable rows directly at the known field offsets.
                //
                // UAnimNotify base ends at +0x2C (verified against
                // AnimNotify_Sound's UProperty dump - its first own
                // field, Sound, sits at off=0x2C).  Subclasses with
                // their own first field place it at the same offset.
                char clsName[64] = "";
                {
                    void* clsObj = ReadClassPtrSafe(obj);
                    if (clsObj)
                    {
                        int nameIdx = -1;
                        PropRead_Int(clsObj, UOBJ_FNAME_OFFSET, &nameIdx);
                        if (nameIdx >= 0)
                            FNameIdx_GetStr(nameIdx, clsName, sizeof(clsName));
                    }
                }

                if (_stricmp(clsName, "AnimNotify_Script") == 0)
                {
                    // var() name NotifyName  -- FName at +0x2C
                    PropBinding* b = PropBindings_Alloc(
                        obj, 0x2C, PROP_KIND_NAME);
                    if (b)
                        PropertyGrid::AddEditableRowAt(
                            grid, headerRow, "NotifyName",
                            PropBinding_Get, PropBinding_Set, b);
                }
                else if (_stricmp(clsName, "AnimNotify_Trigger") == 0)
                {
                    // var() name EventName  -- FName at +0x2C
                    PropBinding* b = PropBindings_Alloc(
                        obj, 0x2C, PROP_KIND_NAME);
                    if (b)
                        PropertyGrid::AddEditableRowAt(
                            grid, headerRow, "EventName",
                            PropBinding_Get, PropBinding_Set, b);
                }
                else if (_stricmp(clsName, "AnimNotify_DestroyEffect") == 0)
                {
                    // var() name DestroyTag         -- FName at +0x2C
                    // var() bool bExpireParticles   -- byte  at +0x30
                    PropBinding* bTag = PropBindings_Alloc(
                        obj, 0x2C, PROP_KIND_NAME);
                    if (bTag)
                        PropertyGrid::AddEditableRowAt(
                            grid, headerRow, "DestroyTag",
                            PropBinding_Get, PropBinding_Set, bTag);

                    PropBinding* bExpire = PropBindings_Alloc(
                        obj, 0x30, PROP_KIND_BOOL);
                    if (bExpire)
                    {
                        static const char* boolOpts[] = { "False", "True" };
                        PropertyGrid::AddEnumRowAt(
                            grid, headerRow, "bExpireParticles",
                            boolOpts, 2,
                            PropBinding_Get, PropBinding_Set, bExpire);
                    }
                }
            }
        }
        else
        {
            // 2b. NotifyObject is null - offer a class-picker dropdown
            // + "New" button.  UT2004 lays this out as
            // "New = <class> [ New ]" - the combo selects a class but
            // doesn't commit; clicking the button is what constructs it.
            // Rebuild the class list each populate so newly-loaded
            // packages contribute their UAnimNotify_* subclasses.
            RebuildAnimNotifyClassList();
            if (g_animNotifyClassCount > 0)
            {
                static const char* optsBuf[ANIMNOTIFY_CLASSES_MAX];
                for (int i = 0; i < g_animNotifyClassCount; ++i)
                    optsBuf[i] = g_animNotifyClasses[i].name;

                NewObjectBinding* nb = NewObjBindings_Alloc(elemIdx);
                if (nb)
                {
                    int newRow = PropertyGrid::AddEnumRowAt(
                        grid, notifyRow, "New",
                        optsBuf, g_animNotifyClassCount,
                        NewObject_Get, NewObject_Set, nb);
                    if (newRow >= 0)
                        PropertyGrid::AddRowButton(
                            grid, newRow, "New",
                            NewObject_DoCreate, nb,
                            PropertyGrid::BTN_VIS_SELECTED);
                }
            }
        }
    }

    // 3. NotifyFrame -> FMeshAnimNotify::Time at slot+0 (FLOAT).  Sibling
    // of the Notify row, not a child - matches UT2004's struct layout.
    NotifyFieldBinding* nfTime = NotifyFieldBindings_Alloc(elemIdx, 0, PROP_KIND_FLOAT);
    if (nfTime)
        PropertyGrid::AddEditableRowAt(grid, elemRow, "NotifyFrame",
                                       NotifyField_Get, NotifyField_Set, nfTime);

    // honor the pending auto-expand request set by
    // NewObject_DoCreate or NotifysHeader_Add.  We expand both the
    // [N] element row and its Notify subrow so the user sees the new
    // object's inline properties immediately.
    if (g_pendingExpandNotifyIdx == elemIdx)
    {
        PropertyGrid::SetRowExpanded(grid, elemRow, true);
        if (notifyRow >= 0)
            PropertyGrid::SetRowExpanded(grid, notifyRow, true);
        g_pendingExpandNotifyIdx = -1;
    }
}

static void BuildNotifyTab(HWND hParent, int pageW)
{
    HWND grid = PropertyGrid::Create(hParent, IDC_AB_NOTIFY_LIST,
                                     0, 0, pageW, 200);
    g_tabContent[AB_TAB_NOTIFY].push_back({
        grid, kPropPadX, 4, pageW - kPropPadX * 2, 200,
        /*stretch=*/true, /*stretchHeight=*/true });
}

static void RefreshNotifyTab()
{
    HWND grid = GetDlgItem(g_hWnd, IDC_AB_NOTIFY_LIST);
    if (!grid) return;

    PropertyGrid::BeginUpdate(grid);
    PropertyGrid::Clear(grid);

    // reset binding pools.  Rows we're about
    // to add hold pointers into these arrays; clearing here invalidates
    // the prior refresh's pointers (rows that referenced them are
    // already gone via PropertyGrid::Clear above).
    PropBindings_Reset();
    NotifyFieldBindings_Reset();
    NewObjBindings_Reset();
    ObjPickerBindings_Reset();
    StructMemberBindings_Reset();
    ClassPickerBindings_Reset();

    PropertyGrid::AddCategory(grid, "Notifys");

    PropertyGrid::ArrayOps notifyOps = {
        Notify_Count,             // count
        Notify_Get,               // get (read-only summary string)
        nullptr,                  // set (per-property edit happens on children)
        Notify_Insert,            // insert
        Notify_Delete,            // del
        Notify_Empty,             // empty
        Notify_PopulateChildren   // per-element property rows
    };
    int notifysHeaderRow = PropertyGrid::AddArray(grid, "Notifys", notifyOps, nullptr);

    // "Empty" / "Add" buttons on the Notifys header row, same
    // as UT2004 ships them in WObjectProperties.  Mirrors the existing
    // right-click context menu so users have both ways to hit the action.
    if (notifysHeaderRow >= 0)
    {
        PropertyGrid::AddRowButton(grid, notifysHeaderRow, "Empty",
                                   NotifysHeader_Empty, nullptr);
        PropertyGrid::AddRowButton(grid, notifysHeaderRow, "Add",
                                   NotifysHeader_Add, nullptr);
    }

    // ---- Diagnostic: dump each AnimNotify subclass's UProperty
    // list once per session.  Collects pointers inside SEH (POD only) and
    // dispatches to the (logger-touching) dump function outside.
    void* objsToDump[16] = {};
    int   objsCount      = 0;

    if (g_currentSeqPtr && g_uobjClassOffset != 0)
    {
        __try
        {
            const int sizeOfFName     = g_uobjClassOffset - UOBJ_FNAME_OFFSET;
            const int notifysDataOff  = sizeOfFName + 24;
            const int notifysNumOff   = sizeOfFName + 28;
            const int notifyStride    = 8 + sizeOfFName;
            const int notifyObjOffset = 4 + sizeOfFName;

            char* data = (char*)*(void**)((char*)g_currentSeqPtr + notifysDataOff);
            int   num  = *(int*)((char*)g_currentSeqPtr + notifysNumOff);
            if (data && num > 0 && num <= 8192)
            {
                int cap = num < 16 ? num : 16;
                for (int i = 0; i < cap; ++i)
                {
                    void* obj = *(void**)(data + i * notifyStride + notifyObjOffset);
                    if (obj) objsToDump[objsCount++] = obj;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { objsCount = 0; }
    }
    for (int i = 0; i < objsCount; ++i)
        UProp_DumpObjectOnce(objsToDump[i], "AnimNotify");

    PropertyGrid::EndUpdate(grid);
}

// =====================================================================
//  Animation Set tab (UAnimEditProps)
// =====================================================================
//  Mirrors UT2004 UAnimEditProps:
//    var(Compression) float GlobalCompression;  // defaultproperties=1.0
//
//  This is a transient editor-side knob (UMeshAnimation has no such
//  field) - used as the COMP= parameter when re-importing animation
//  data.  We track it in a static and seed it from the default value.

static float g_animGlobalCompression = 1.0f;

// Smoke-test of the new PropertyGrid widget.  The
// Animation Set tab has a single field, so it's a low-risk place to
// validate the widget rendering before we port the other tabs.  In
// Turn 1 the value is read-only display; Turn 2 adds inline editing.
static void BuildAnimSetTab(HWND hParent, int pageW)
{
    HWND grid = PropertyGrid::Create(hParent, IDC_AB_ANIM_GRID,
                                     0, 0, pageW, 200);
    g_tabContent[AB_TAB_ANIM].push_back({
        grid, kPropPadX, 4, pageW - kPropPadX * 2, 200,
        /*stretch=*/true, /*stretchHeight=*/true });
}

// Accessors used by the PropertyGrid for the editable Compression row.
// `userdata` points at the underlying float we want the widget to
// drive directly - same pattern UT2004's WObjectProperties uses to
// shuttle individual property values in/out of the editor.
static void GetFloat6(char* buf, int size, void* userdata)
{
    float v = *static_cast<float*>(userdata);
    _snprintf_s(buf, size, _TRUNCATE, "%.6f", v);
}
static void SetFloat6(const char* text, void* userdata)
{
    float v = static_cast<float>(atof(text));
    *static_cast<float*>(userdata) = v;
}
static void SetFloat6_01(const char* text, void* userdata)
{
    float v = static_cast<float>(atof(text));
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    *static_cast<float*>(userdata) = v;
}

static void RefreshAnimSetTab()
{
    HWND grid = GetDlgItem(g_hWnd, IDC_AB_ANIM_GRID);
    if (!grid) return;

    PropertyGrid::BeginUpdate(grid);
    PropertyGrid::Clear(grid);

    PropertyGrid::AddCategory(grid, "Compression");
    PropertyGrid::AddEditableRow(grid, "GlobalCompression",
                                 GetFloat6, SetFloat6_01,
                                 &g_animGlobalCompression);

    PropertyGrid::EndUpdate(grid);
}

// =====================================================================
//  Prefs tab (SkelPrefsEditProps)
// =====================================================================
//  SCCT exposes a single property here: LODStyle (int).  Verified by
//  string search in ChaosTheory_Editor.exe - LODSTYLE= is used as a
//  NEWANIM IMPORT arg and "Skeletal mesh processing. LODStyle: %i"
//  appears as a debug print.  RootZero (present in SCPT) and
//  WeldDuplicateVertices (added later in UT2004's UE2 line) are not
//  in this build.

static int g_prefsLODStyle = 10;

static void BuildPrefsTab(HWND hParent, int pageW)
{
    HWND grid = PropertyGrid::Create(hParent, IDC_AB_PREFS_GRID,
                                     0, 0, pageW, 200);
    g_tabContent[AB_TAB_PREFS].push_back({
        grid, kPropPadX, 4, pageW - kPropPadX * 2, 200,
        /*stretch=*/true, /*stretchHeight=*/true });
}

// Generic int accessors so any int-valued property (LODStyle, RootZero,
// StartFrame, NumFrames if we ever made them editable, ...) can share
// the same getter/setter shape.
static void Get_Int(char* b, int n, void* userdata)
{
    int v = *static_cast<int*>(userdata);
    _snprintf_s(b, n, _TRUNCATE, "%d", v);
}
static void Set_Int(const char* t, void* userdata)
{
    *static_cast<int*>(userdata) = std::atoi(t);
}

static void RefreshPrefsTab()
{
    HWND grid = GetDlgItem(g_hWnd, IDC_AB_PREFS_GRID);
    if (!grid) return;

    PropertyGrid::BeginUpdate(grid);
    PropertyGrid::Clear(grid);

    PropertyGrid::AddCategory(grid, "Import");
    PropertyGrid::AddEditableRow(grid, "LODStyle",
                                 Get_Int, Set_Int, &g_prefsLODStyle);

    PropertyGrid::EndUpdate(grid);
}

// =====================================================================
//  Mesh tab (UMeshEditProps) - framework + DefaultAnimation + Material[]
// =====================================================================
//  UT2004 UMeshEditProps exposes ~30 fields across many categories.  We
//  start with the two the user wants now:
//    var(Animation) MeshAnimation  DefaultAnimation;
//    var(Skin)      array<Material> Material;
//
//  Both fields live on USkeletalMesh.  Reading the actual values
//  requires the USkeletalMesh field offsets - those will be probed via
//  Ghidra in a follow-up turn.  For now the Material column shows
//  "<offset TBD>" so the layout is visible end-to-end.

static void BuildMeshTab(HWND hParent, int pageW)
{
    HWND grid = PropertyGrid::Create(hParent, IDC_AB_MESH_GRID,
                                     0, 0, pageW, 200);
    g_tabContent[AB_TAB_MESH].push_back({
        grid, kPropPadX, 4, pageW - kPropPadX * 2, 200,
        /*stretch=*/true, /*stretchHeight=*/true });
}

// =====================================================================
//  USkeletalMesh::Material offset auto-probe (runtime, SEH-protected)
// =====================================================================
//  We don't statically know the offset of UMesh::Material inside SCCT's
//  USkeletalMesh layout (UPrimitive + UMesh + ULodMesh + USkeletalMesh
//  has accumulated alignment + game-specific extensions that diverge
//  from UT2004 here).  Instead: at first Mesh tab refresh, walk GObjects
//  for any USkeletalMesh whose Material array isn't empty, then scan
//  candidate offsets reading the TArray(Data, Num) and validating Data[0]
//  resolves to a UObject with a material-like class name.  Cache the
//  first hit; subsequent reads use it directly.

// 0  = not yet probed
// -1 = probed and not found (Material display stays empty)
// >0 = offset of Material TArray.Data within USkeletalMesh
static int   g_uskeletalMeshMaterialOffset = 0;
static void* g_currentMeshPtr              = nullptr;

static bool ClassNameLooksLikeMaterial(const char* n)
{
    if (!n) return false;
    return std::strstr(n, "Material")  || std::strstr(n, "Texture")  ||
           std::strstr(n, "Shader")    || std::strstr(n, "FinalBlend") ||
           std::strstr(n, "Combiner")  || std::strstr(n, "Modifier")  ||
           std::strstr(n, "Cubemap")   || std::strstr(n, "Bitmap")    ||
           std::strstr(n, "ConstantColor") || std::strstr(n, "VertexColor");
}

static int ProbeMaterialOffsetForMeshRaw(int classOffset, void* mesh,
                                          int minOff, int maxOff)
{
    __try
    {
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return -1;

        for (int off = minOff; off <= maxOff; off += 4)
        {
            void* data = *(void**)((char*)mesh + off);
            int   num  = *(int*)  ((char*)mesh + off + 4);

            if (num < 1 || num > 64) continue;
            if (!data) continue;

            // First element of a TArray<UMaterial*> is a UObject* pointer.
            void* firstElem = *(void**)data;
            if (!firstElem) continue;

            void* cls = *(void**)((char*)firstElem + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx <= 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;

            if (ClassNameLooksLikeMaterial(clsName))
                return off;
        }
        return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static int GO_ProbeMaterialOffset()
{
    if (g_uobjClassOffset == 0) return -1;

    void** gObjData = *(void***)GOBJECTS_DATA;
    int    gObjNum  = *(int*)   GOBJECTS_NUM;
    if (!gObjData || gObjNum <= 0) return -1;

    void** gNamesData = *(void***)GNAMES_DATA;
    int    gNamesNum  = *(int*)   GNAMES_NUM;
    if (!gNamesData || gNamesNum <= 0) return -1;

    for (int i = 0; i < gObjNum; ++i)
    {
        void* obj = gObjData[i];
        if (!obj) continue;

        void* cls = *(void**)((char*)obj + g_uobjClassOffset);
        if (!cls) continue;
        int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
        if (clsIdx <= 0 || clsIdx >= gNamesNum) continue;
        void* clsEntry = gNamesData[clsIdx];
        if (!clsEntry) continue;
        const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
        if (_stricmp(clsName, "SkeletalMesh") != 0) continue;

        // Found a USkeletalMesh - probe its layout for a Material TArray.
        int off = ProbeMaterialOffsetForMeshRaw(g_uobjClassOffset, obj, 0x40, 0x200);
        if (off > 0) return off;
    }
    return -1;
}

// Find a USkeletalMesh by (package, name) - mirrors GO_FindMeshAnimSeqRaw.
static void* GO_FindSkeletalMeshRaw(int classOffset, const char* package, const char* meshName)
{
    __try
    {
        void** gObjData = *(void***)GOBJECTS_DATA;
        int    gObjNum  = *(int*)   GOBJECTS_NUM;
        if (!gObjData || gObjNum <= 0) return nullptr;

        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || gNamesNum <= 0) return nullptr;

        for (int i = 0; i < gObjNum; ++i)
        {
            void* obj = gObjData[i];
            if (!obj) continue;

            void* cls = *(void**)((char*)obj + classOffset);
            if (!cls) continue;
            int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            if (clsIdx <= 0 || clsIdx >= gNamesNum) continue;
            void* clsEntry = gNamesData[clsIdx];
            if (!clsEntry) continue;
            const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(clsName, "SkeletalMesh") != 0) continue;

            int nameIdx = *(int*)((char*)obj + UOBJ_FNAME_OFFSET);
            if (nameIdx <= 0 || nameIdx >= gNamesNum) continue;
            const char* nm = (char*)gNamesData[nameIdx] + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(nm, meshName) != 0) continue;

            void* top = obj;
            for (int s = 0; s < 16; ++s)
            {
                void* nxt = *(void**)((char*)top + UOBJ_OUTER_OFFSET);
                if (!nxt) break;
                top = nxt;
            }
            int pkgIdx = *(int*)((char*)top + UOBJ_FNAME_OFFSET);
            if (pkgIdx <= 0 || pkgIdx >= gNamesNum) continue;
            const char* pkg = (char*)gNamesData[pkgIdx] + FNAME_ENTRY_STR_OFFSET;
            if (_stricmp(pkg, package) != 0) continue;

            return obj;
        }
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// =====================================================================
//  Canonical UObject reference formatter
// =====================================================================
//  Build "ClassName'TopPkg.Group.Name'" for any UObject* (mirrors what
//  UnrealEd shows in property fields).  Shared by Material array,
//  DefaultAnimation, and any future single-object reference rows.
//  Writes "None" when obj is null.  SEH-protected; returns false on AV
//  and leaves the buffer empty.
static bool FormatUObjectRef(void* obj, char* outBuf, int outSize)
{
    if (!outBuf || outSize <= 0) return false;
    outBuf[0] = '\0';
    if (!obj) { strncpy_s(outBuf, outSize, "None", _TRUNCATE); return true; }

    __try
    {
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData) return false;

        void* cls = *(void**)((char*)obj + g_uobjClassOffset);
        if (!cls) return false;
        int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
        if (clsIdx < 0 || clsIdx >= gNamesNum) return false;
        void* clsEntry = gNamesData[clsIdx];
        if (!clsEntry) return false;
        const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;

        const char* parts[16] = {};
        int depth = 0;
        void* cur = obj;
        while (cur && depth < 16)
        {
            int nameIdx = *(int*)((char*)cur + UOBJ_FNAME_OFFSET);
            if (nameIdx < 0 || nameIdx >= gNamesNum) break;
            void* entry = gNamesData[nameIdx];
            if (!entry) break;
            parts[depth++] = (char*)entry + FNAME_ENTRY_STR_OFFSET;
            cur = *(void**)((char*)cur + UOBJ_OUTER_OFFSET);
        }

        char path[512] = "";
        for (int k = depth - 1; k >= 0; --k)
        {
            if (!parts[k]) continue;
            strncat_s(path, sizeof(path), parts[k], _TRUNCATE);
            if (k > 0) strncat_s(path, sizeof(path), ".", _TRUNCATE);
        }

        _snprintf_s(outBuf, outSize, _TRUNCATE, "%s'%s'", clsName, path);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outBuf[0] = '\0';
        return false;
    }
}

// =====================================================================
//  UProperty introspection
// =====================================================================
//  Walk a UObject's UClass::PropertyLink chain to enumerate every CPF_Edit
//  property: name, subclass (FloatProperty / ObjectProperty / ...), in-
//  object byte Offset, ArrayDim, ElementSize, PropertyFlags.
//
//  PropertyLink only contains a struct's OWN properties, so to get the
//  full set for an instance we walk SuperStruct upward.  We emit base-
//  class properties first to match UnrealEd 2's WObjectProperties order.
//
//  All access is SEH-protected and POD-only.

// FName.Index -> static buffer pointer (in GNames).  Returns NULL on AV
// or out-of-range index.  Caller must NOT outlive GNames (i.e., don't
// stash this across editor-state changes; copy if you need persistence).
static const char* FNameIdx_PeekRaw(int nameIdx)
{
    __try
    {
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || nameIdx < 0 || nameIdx >= gNamesNum) return nullptr;
        void* entry = gNamesData[nameIdx];
        if (!entry) return nullptr;
        return (const char*)entry + FNAME_ENTRY_STR_OFFSET;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Copy-out variant: writes the FName's string into a caller buffer.
// Returns true on success, false on AV/out-of-range.
static bool FNameIdx_GetStr(int nameIdx, char* outBuf, int outSize)
{
    if (!outBuf || outSize <= 0) return false;
    outBuf[0] = '\0';
    __try
    {
        void** gNamesData = *(void***)GNAMES_DATA;
        int    gNamesNum  = *(int*)   GNAMES_NUM;
        if (!gNamesData || nameIdx < 0 || nameIdx >= gNamesNum) return false;
        void* entry = gNamesData[nameIdx];
        if (!entry) return false;
        const char* s = (const char*)entry + FNAME_ENTRY_STR_OFFSET;
        strncpy_s(outBuf, outSize, s, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { outBuf[0] = '\0'; return false; }
}

// (UPropInfo struct and UPropCallback typedef are hoisted to the
// forward-decl block earlier in the file.  See the
// forward decls there.)

// Walks `obj`'s class hierarchy from base->derived, calling `cb` for
// every property whose CPF_Edit flag is set.  Bails on AV (returns false).
// Cap of 256 classes / 4096 props per walk to bound runaway chains.
static bool UProp_ForEachEditable(void* obj, UPropCallback cb, void* userdata)
{
    if (!obj || !cb || g_uobjClassOffset == 0) return false;

    // Step 1: collect the class chain bottom-up (POD-only inside SEH).
    void* chain[256] = {};
    int   chainLen   = 0;
    __try
    {
        void* cls = *(void**)((char*)obj + g_uobjClassOffset);
        while (cls && chainLen < 256)
        {
            chain[chainLen++] = cls;
            cls = *(void**)((char*)cls + UFIELD_SUPERFIELD_OFFSET);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // Step 2: walk base-to-derived (reverse), emit each class's PropertyLink.
    // Dedupe by UProperty pointer in case the engine's PropertyLink chain is
    // "flat" (already contains inherited properties).  At UE2 build 2110
    // it's unclear without an in-editor run, so we handle both shapes.
    void* seenProps[512] = {};
    int   seenCount      = 0;
    int   totalEmitted   = 0;

    for (int ci = chainLen - 1; ci >= 0; --ci)
    {
        void* cls = chain[ci];

        // Per-class SEH so a corrupt PropertyLink in one class doesn't
        // kill enumeration of siblings.
        void* propChain[1024] = {};
        int   propCount = 0;

        __try
        {
            void* prop = *(void**)((char*)cls + USTRUCT_PROPERTYLINK_OFFSET);
            int   guard = 0;
            while (prop && guard < 1024)
            {
                propChain[propCount++] = prop;
                prop = *(void**)((char*)prop + UPROP_PROPERTYLINKNEXT_OFFSET);
                ++guard;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { propCount = 0; }

        for (int pi = 0; pi < propCount && totalEmitted < 4096; ++pi)
        {
            // Dedupe: skip if we've already emitted this UProperty pointer.
            bool dup = false;
            for (int si = 0; si < seenCount; ++si)
                if (seenProps[si] == propChain[pi]) { dup = true; break; }
            if (dup) continue;
            if (seenCount < 512) seenProps[seenCount++] = propChain[pi];

            UPropInfo info = {};
            info.propPtr = propChain[pi];

            bool ok = false;
            __try
            {
                char* p = (char*)propChain[pi];

                unsigned flags = *(unsigned*)(p + UPROP_PROPERTYFLAGS_OFFSET);
                int   propNameIdx = *(int*)(p + UOBJ_FNAME_OFFSET);
                const char* propName = FNameIdx_PeekRaw(propNameIdx);

                // Always hide the inherited UObject::Name (the const
                // "Name" row that appears on every object).
                bool isInheritedName =
                    propName &&
                    (_stricmp(propName, "Name") == 0) &&
                    (flags & CPF_EditConst);

                // Whitelist a handful of named AnimNotify properties
                // that SCCT compiled WITHOUT CPF_Edit even though
                // they're meaningful to edit (the stock UE2 sources
                // mark them var() but Ubisoft's recompile dropped
                // the flag).  Surface them anyway so the user can
                // see + change the values.
                bool isWhitelistedName = false;
                if (propName)
                {
                    static const char* kForceEditable[] = {
                        "NotifyName",        // AnimNotify_Script
                        "EventName",         // AnimNotify_Trigger
                        "DestroyTag",        // AnimNotify_DestroyEffect
                        "bExpireParticles",  // AnimNotify_DestroyEffect
                    };
                    for (const char* wn : kForceEditable)
                    {
                        if (_stricmp(propName, wn) == 0)
                        { isWhitelistedName = true; break; }
                    }
                }

                bool include = (flags & CPF_Edit) || isWhitelistedName;
                if (!include || isInheritedName)
                {
                    ok = false;
                }
                else
                {
                    info.flags       = flags;
                    info.offset      = *(int*)(p + UPROP_OFFSET_OFFSET);
                    info.arrayDim    = *(short*)(p + UPROP_ARRAYDIM_OFFSET);
                    info.elementSize = *(short*)(p + UPROP_ELEMENTSIZE_OFFSET);

                    int nameIdx = *(int*)(p + UOBJ_FNAME_OFFSET);
                    int catIdx  = *(int*)(p + UPROP_CATEGORY_OFFSET);

                    const char* nm = FNameIdx_PeekRaw(nameIdx);
                    if (nm) strncpy_s(info.name, sizeof(info.name), nm, _TRUNCATE);
                    const char* cat = FNameIdx_PeekRaw(catIdx);
                    if (cat) strncpy_s(info.category, sizeof(info.category), cat, _TRUNCATE);

                    void* propCls = *(void**)(p + g_uobjClassOffset);
                    if (propCls)
                    {
                        int tIdx = *(int*)((char*)propCls + UOBJ_FNAME_OFFSET);
                        const char* tn = FNameIdx_PeekRaw(tIdx);
                        if (tn) strncpy_s(info.typeName, sizeof(info.typeName), tn, _TRUNCATE);
                    }
                    ok = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

            if (ok)
            {
                ++totalEmitted;
                if (!cb(info, userdata)) return true;  // early stop
            }
        }
    }
    return true;
}

// =====================================================================
//  Diagnostic dump
// =====================================================================
//  Log every editable property on the supplied UObject the first time we
//  see one of its class.  Lets us verify offsets without needing the
//  expandable-grid UI in place yet.  We key the one-shot gate by class
//  pointer so different AnimNotify subclasses each get one dump.

struct DumpCtx { int count; };

static bool UProp_DumpCallback(const UPropInfo& info, void* userdata)
{
    DumpCtx* ctx = (DumpCtx*)userdata;
    char line[256];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "  [%d] %s  type=%s off=0x%X dim=%d elemSize=%d flags=0x%08X cat=%s",
                ctx->count, info.name, info.typeName, info.offset,
                info.arrayDim, info.elementSize, info.flags, info.category);
    Logger::log(line);
    ++ctx->count;
    return true;
}

// Class-pointer dedupe table - 16 slots is plenty for the handful of
// AnimNotify subclasses SCCT uses.
static void* g_dumpedClasses[16] = {};
static int   g_dumpedClassCount  = 0;

static bool DumpedAlready(void* cls)
{
    for (int i = 0; i < g_dumpedClassCount; ++i)
        if (g_dumpedClasses[i] == cls) return true;
    if (g_dumpedClassCount < 16)
        g_dumpedClasses[g_dumpedClassCount++] = cls;
    return false;
}

// SEH-isolated class pointer read.  Returns null on AV.  Split out so the
// caller (UProp_DumpObjectOnce) can use Logger::log (which pulls in
// std::wofstream, requiring C++ unwinding incompatible with __try).
static void* ReadClassPtrSafe(void* obj)
{
    if (!obj || g_uobjClassOffset == 0) return nullptr;
    __try { return *(void**)((char*)obj + g_uobjClassOffset); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void UProp_DumpObjectOnce(void* obj, const char* tag)
{
    if (!obj || g_uobjClassOffset == 0) return;

    void* cls = ReadClassPtrSafe(obj);
    if (!cls) return;
    if (DumpedAlready(cls)) return;

    char clsRef[256] = "";
    FormatUObjectRef(obj, clsRef, sizeof(clsRef));

    char hdr[320];
    _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "UProp dump %s: %s", tag ? tag : "(no tag)", clsRef);
    Logger::log(hdr);

    DumpCtx ctx = { 0 };
    UProp_ForEachEditable(obj, &UProp_DumpCallback, &ctx);

    char ftr[64];
    _snprintf_s(ftr, sizeof(ftr), _TRUNCATE, "UProp dump end: %d properties", ctx.count);
    Logger::log(ftr);
}

// =====================================================================
//  Typed property accessors
// =====================================================================
//  Generic Get/Set pair driven by a (UObject*, offset, type-kind) tuple.
//  PropertyGrid rows store a pointer to one of these bindings as their
//  void* userdata; the dispatch functions decode by typeKind.
//
//  Memory: bindings come from a fixed-size pool reset on every Notify
//  tab refresh.  Bindings are tied to the lifetime of the currently
//  displayed UObject instances (the notifies), so refreshes invalidate
//  them in lockstep with row removal.
//
//  Supported (read+write):  Float, Int, Name
//  Read-only:               Object, Str, Byte, Struct, Array (display only)
//  Future:                  Bool (need UBoolProperty::BitMask offset),
//                           Object writes (needs picker dialog).
//
//  (enum PropKind is hoisted to the forward-decl block at the top of
//  the file so `case PROP_KIND_*:` labels in earlier functions compile.)

// Resolve UProperty subclass name -> kind enum.  Case-sensitive (the
// engine emits these strings via UClass::GetName).
static int PropKindFromTypeName(const char* tn)
{
    if (!tn || !*tn) return PROP_KIND_UNKNOWN;
    if (std::strcmp(tn, "FloatProperty")  == 0) return PROP_KIND_FLOAT;
    if (std::strcmp(tn, "IntProperty")    == 0) return PROP_KIND_INT;
    if (std::strcmp(tn, "ByteProperty")   == 0) return PROP_KIND_BYTE;
    if (std::strcmp(tn, "BoolProperty")   == 0) return PROP_KIND_BOOL;
    if (std::strcmp(tn, "ObjectProperty") == 0) return PROP_KIND_OBJECT;
    if (std::strcmp(tn, "ClassProperty")  == 0) return PROP_KIND_CLASS;
    if (std::strcmp(tn, "NameProperty")   == 0) return PROP_KIND_NAME;
    if (std::strcmp(tn, "StrProperty")    == 0) return PROP_KIND_STR;
    if (std::strcmp(tn, "StructProperty") == 0) return PROP_KIND_STRUCT;
    if (std::strcmp(tn, "ArrayProperty")  == 0) return PROP_KIND_ARRAY;
    return PROP_KIND_UNKNOWN;
}

// One binding per displayed property row.  POD - safe inside __try.
struct PropBinding
{
    void* obj;          // The UObject instance
    int   offset;       // Byte offset of the property data
    int   kind;         // PropKind
};

// Binding pool.  PropertyGrid rows hold raw pointers into this array, so
// it lives as long as the rows do.  Cleared at start of every refresh.
#define PROP_BINDING_POOL_SIZE  256
static PropBinding g_propBindings[PROP_BINDING_POOL_SIZE];
static int         g_propBindingCount = 0;

static void PropBindings_Reset()
{
    g_propBindingCount = 0;
}

static PropBinding* PropBindings_Alloc(void* obj, int offset, int kind)
{
    if (g_propBindingCount >= PROP_BINDING_POOL_SIZE) return nullptr;
    PropBinding* b = &g_propBindings[g_propBindingCount++];
    b->obj    = obj;
    b->offset = offset;
    b->kind   = kind;
    return b;
}

// --- Typed getters (callable from inside __try; POD only) -----------------

static bool PropRead_Float(void* obj, int off, float* out)
{
    __try { *out = *(float*)((char*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0.0f; return false; }
}
static bool PropRead_Int(void* obj, int off, int* out)
{
    __try { *out = *(int*)((char*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}
static bool PropRead_Byte(void* obj, int off, int* out)
{
    __try { *out = *(unsigned char*)((char*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}
static bool PropRead_Object(void* obj, int off, void** out)
{
    __try { *out = *(void**)((char*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return false; }
}
static bool PropRead_NameIdx(void* obj, int off, int* out)
{
    __try { *out = *(int*)((char*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}

// --- Typed setters (POD only inside __try) --------------------------------

static bool PropWrite_Float(void* obj, int off, float v)
{
    __try { *(float*)((char*)obj + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool PropWrite_Int(void* obj, int off, int v)
{
    __try { *(int*)((char*)obj + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool PropWrite_NameIdx(void* obj, int off, int idx)
{
    __try { *(int*)((char*)obj + off) = idx; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool PropWrite_Byte(void* obj, int off, int v)
{
    __try { *(unsigned char*)((char*)obj + off) = (unsigned char)v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- PropertyGrid getter/setter dispatchers -------------------------------
//  These are what AddEditableRow / AddRow plug in to.

static void PropBinding_Get(char* buf, int size, void* userdata)
{
    if (!buf || size <= 0) return;
    buf[0] = '\0';
    if (!userdata) return;
    PropBinding* b = (PropBinding*)userdata;
    if (!b->obj) return;

    switch (b->kind)
    {
    case PROP_KIND_FLOAT:
    {
        float v = 0.0f;
        if (PropRead_Float(b->obj, b->offset, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%.6f", v);
        break;
    }
    case PROP_KIND_INT:
    {
        int v = 0;
        if (PropRead_Int(b->obj, b->offset, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%d", v);
        break;
    }
    case PROP_KIND_BYTE:
    {
        int v = 0;
        if (PropRead_Byte(b->obj, b->offset, &v))
            _snprintf_s(buf, size, _TRUNCATE, "%d", v);
        break;
    }
    case PROP_KIND_BOOL:
    {
        // Without UBoolProperty::BitMask wired up we approximate by
        // testing the low byte for non-zero.  Adequate for single-bool
        // properties (no bitfield packing in AnimNotify_*).
        int v = 0;
        if (PropRead_Byte(b->obj, b->offset, &v))
            strncpy_s(buf, size, v ? "True" : "False", _TRUNCATE);
        break;
    }
    case PROP_KIND_OBJECT:
    case PROP_KIND_CLASS:
    {
        void* p = nullptr;
        if (PropRead_Object(b->obj, b->offset, &p))
            FormatUObjectRef(p, buf, size);
        break;
    }
    case PROP_KIND_NAME:
    {
        int idx = 0;
        if (PropRead_NameIdx(b->obj, b->offset, &idx))
            FNameIdx_GetStr(idx, buf, size);
        break;
    }
    case PROP_KIND_STR:
    {
        // FString in UE2 = TArray<char> { Data, Num, Max } at base offset.
        // For now display the raw Data pointer's bytes guarded by Num.
        __try
        {
            char* base = (char*)b->obj + b->offset;
            char* data = *(char**)base;
            int   num  = *(int*)(base + 4);
            if (data && num > 0 && num < size)
            {
                strncpy_s(buf, size, data, num);
                buf[(num < size - 1) ? num : (size - 1)] = '\0';
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
        break;
    }
    case PROP_KIND_STRUCT:
        strncpy_s(buf, size, "(struct)", _TRUNCATE);
        break;
    case PROP_KIND_ARRAY:
        strncpy_s(buf, size, "(array)", _TRUNCATE);
        break;
    default:
        strncpy_s(buf, size, "(unsupported)", _TRUNCATE);
        break;
    }
}

static void PropBinding_Set(const char* text, void* userdata)
{
    if (!text || !userdata) return;
    PropBinding* b = (PropBinding*)userdata;
    if (!b->obj) return;

    switch (b->kind)
    {
    case PROP_KIND_FLOAT:
        PropWrite_Float(b->obj, b->offset, (float)atof(text));
        break;
    case PROP_KIND_INT:
    case PROP_KIND_BYTE:
        PropWrite_Int(b->obj, b->offset, atoi(text));
        break;
    case PROP_KIND_NAME:
    {
        // Empty string -> NAME_None (index 0).  Otherwise intern.
        int idx = (text[0]) ? FName_Intern(text, FNAME_ADD) : 0;
        PropWrite_NameIdx(b->obj, b->offset, idx);
        break;
    }
    case PROP_KIND_BOOL:
        // True/False dropdown commits.  We treat the whole byte at the
        // property's offset as the bool value; this works for single-
        // bool properties like AnimNotify_Effect's Attach.  Packed
        // bitfields would need UBoolProperty::BitMask plumbing, but
        // none of the SCCT AnimNotify_* classes ship packed bools.
        PropWrite_Byte(b->obj, b->offset,
                       (_stricmp(text, "True") == 0) ? 1 : 0);
        break;
    // Object/Class/Str/Struct/Array writes deferred - read-only for now.
    default:
        break;
    }
}

// Returns true if this kind supports inline edit (i.e., AddEditableRow vs
// AddRow).  Read-only kinds use a non-editable display row.
static bool PropKindIsEditable(int kind)
{
    switch (kind)
    {
    case PROP_KIND_FLOAT:
    case PROP_KIND_INT:
    case PROP_KIND_BYTE:
    case PROP_KIND_NAME:
        return true;
    default:
        return false;
    }
}

// =====================================================================
//  USkeletalMesh::DefaultAnim offset auto-probe
// =====================================================================
//  DefaultAnim is a single UMeshAnimation* field (not a TArray) on
//  USkeletalMesh.  Heuristic: scan candidate offsets for a non-null
//  pointer that dereferences to a UObject whose class name is exactly
//  "MeshAnimation".  Since no other USkeletalMesh field has that type,
//  this is unambiguous when DefaultAnim is set.  If every loaded
//  USkeletalMesh has DefaultAnim=null we fall back to displaying "None"
//  (offset stays unset).

static int   g_uskeletalMeshDefaultAnimOffset = 0;

// Returns true if `clsName` looks like a UMeshAnimation or any subclass.
// SCCT may use a custom subclass so we widen beyond the exact name.
static bool ClassNameLooksLikeAnim(const char* n)
{
    if (!n) return false;
    if (_stricmp(n, "MeshAnimation") == 0) return true;
    // Substring heuristic - "Anim" covers UMeshAnimation subclasses
    // like SkelMeshAnim, AnimSet, etc.  We skip "AnimNotify*" so we
    // don't false-positive on FMeshAnimNotify pointers if any leak in.
    if (std::strstr(n, "Anim") && !std::strstr(n, "Notify"))
        return true;
    return false;
}

// POD-only diagnostic record so we can log outside the SEH block (the
// Logger pulls in std::wofstream which requires C++ unwinding and
// thus can't appear inside __try - C2712).
struct AB_DiagEntry { int off; char clsName[64]; };

static int ProbeDefaultAnimOffsetForMeshRaw(int classOffset, void* mesh,
                                             int minOff, int maxOff,
                                             AB_DiagEntry* diags,
                                             int diagsCap, int* diagsCount)
{
    if (diagsCount) *diagsCount = 0;

    void** gNamesData = *(void***)GNAMES_DATA;
    int    gNamesNum  = *(int*)   GNAMES_NUM;
    if (!gNamesData) return -1;

    for (int off = minOff; off <= maxOff; off += 4)
    {
        // Per-offset SEH: an AV on one slot shouldn't kill the entire
        // probe, just skip this candidate and try the next.  POD-only
        // body so __try is legal (no C++ unwinding needed).
        void* ptr = nullptr;
        int   clsIdx = -1;

        __try
        {
            ptr = *(void**)((char*)mesh + off);
            if (ptr)
            {
                void* cls = *(void**)((char*)ptr + classOffset);
                if (cls)
                    clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }

        if (!ptr || clsIdx <= 0 || clsIdx >= gNamesNum) continue;
        void* clsEntry = gNamesData[clsIdx];
        if (!clsEntry) continue;
        const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;

        if (diags && diagsCount && *diagsCount < diagsCap)
        {
            diags[*diagsCount].off = off;
            strncpy_s(diags[*diagsCount].clsName,
                      sizeof(diags[*diagsCount].clsName),
                      clsName, _TRUNCATE);
            (*diagsCount)++;
        }

        if (ClassNameLooksLikeAnim(clsName))
            return off;
    }
    return -1;
}

static int GO_ProbeDefaultAnimOffset()
{
    if (g_uobjClassOffset == 0) return -1;

    void** gObjData = *(void***)GOBJECTS_DATA;
    int    gObjNum  = *(int*)   GOBJECTS_NUM;
    if (!gObjData || gObjNum <= 0) return -1;

    void** gNamesData = *(void***)GNAMES_DATA;
    int    gNamesNum  = *(int*)   GNAMES_NUM;
    if (!gNamesData || gNamesNum <= 0) return -1;

    int    skelMeshes        = 0;
    void*  firstSkelMesh     = nullptr;
    for (int i = 0; i < gObjNum; ++i)
    {
        void* obj = gObjData[i];
        if (!obj) continue;

        void* cls = *(void**)((char*)obj + g_uobjClassOffset);
        if (!cls) continue;
        int clsIdx = *(int*)((char*)cls + UOBJ_FNAME_OFFSET);
        if (clsIdx <= 0 || clsIdx >= gNamesNum) continue;
        void* clsEntry = gNamesData[clsIdx];
        if (!clsEntry) continue;
        const char* clsName = (char*)clsEntry + FNAME_ENTRY_STR_OFFSET;
        if (_stricmp(clsName, "SkeletalMesh") != 0) continue;

        if (!firstSkelMesh) firstSkelMesh = obj;
        skelMeshes++;

        int off = ProbeDefaultAnimOffsetForMeshRaw(g_uobjClassOffset, obj,
                                                   0x40, 0x400,
                                                   nullptr, 0, nullptr);
        if (off > 0) return off;
    }

    // No luck with the strict probe.  If we had at least one mesh,
    // dump everything we saw so we know what classes actually exist
    // at those offsets and can adjust the heuristic.  The logging is
    // done OUTSIDE the SEH-protected probe (Logger pulls in fstream =
    // C++ unwinding, which can't co-exist with __try, hence the POD
    // buffer hand-off).
    if (firstSkelMesh)
    {
        static AB_DiagEntry diags[64];
        int diagCount = 0;
        ProbeDefaultAnimOffsetForMeshRaw(g_uobjClassOffset, firstSkelMesh,
                                          0x40, 0x400,
                                          diags, 64, &diagCount);

        char hdr[160];
        _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                    "AB-probe DefaultAnim: %d SkeletalMesh(es) scanned, "
                    "no DefaultAnim hit. %d candidate pointer field(s) found:",
                    skelMeshes, diagCount);
        Logger::log(hdr);

        for (int k = 0; k < diagCount; ++k)
        {
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "AB-probe DefaultAnim: off=0x%X cls=%s",
                        diags[k].off, diags[k].clsName);
            Logger::log(line);
        }
    }
    return -1;
}

static void Get_DefaultAnim(char* b, int n, void*)
{
    if (b && n > 0) b[0] = '\0';
    if (!b || n <= 0) return;

    if (!g_currentMeshPtr || g_uskeletalMeshDefaultAnimOffset <= 0)
    {
        strncpy_s(b, n, "None", _TRUNCATE);
        return;
    }

    __try
    {
        void* anim = *(void**)((char*)g_currentMeshPtr +
                               g_uskeletalMeshDefaultAnimOffset);
        FormatUObjectRef(anim, b, n);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        strncpy_s(b, n, "None", _TRUNCATE);
    }
}

// Material array accessors - read directly from the cached USkeletalMesh.
static int Material_Count(void*)
{
    if (!g_currentMeshPtr || g_uskeletalMeshMaterialOffset <= 0) return 0;
    int num = 0;
    __try {
        num = *(int*)((char*)g_currentMeshPtr + g_uskeletalMeshMaterialOffset + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (num >= 0 && num <= 256) ? num : 0;
}

// Each entry as the canonical Unreal reference:
//   ClassName'TopPkg.Group.Name'   (e.g. Texture'GenericSD.TCTexture.ToiletCar')
//   None                            (unassigned slot)
static void Material_Get(int i, char* b, int n, void*)
{
    if (b && n > 0) b[0] = '\0';
    if (!b || n <= 0 || !g_currentMeshPtr || g_uskeletalMeshMaterialOffset <= 0)
        return;

    __try
    {
        void* data = *(void**)((char*)g_currentMeshPtr + g_uskeletalMeshMaterialOffset);
        int   num  = *(int*)  ((char*)g_currentMeshPtr + g_uskeletalMeshMaterialOffset + 4);
        if (!data || i < 0 || i >= num) return;

        void* mat = *(void**)((char*)data + i * sizeof(void*));
        FormatUObjectRef(mat, b, n);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (b && n > 0) b[0] = '\0';
    }
}

// Material array element setter.  Parses the typed/pasted
// ref via GO_FindObjectByRef and writes the pointer into Material[i].
// Empty / "None" clears the slot to null.  Unknown refs leave the slot
// unchanged (silent no-op, no crash).
static void Material_Set(int i, const char* text, void* /*userdata*/)
{
    if (!g_currentMeshPtr || g_uskeletalMeshMaterialOffset <= 0) return;
    if (!text) return;

    bool  clear  = (!text[0] || _stricmp(text, "None") == 0);
    void* newPtr = clear ? nullptr : GO_FindObjectByRef(text);
    if (!newPtr && !clear) return;

    __try
    {
        void* data = *(void**)((char*)g_currentMeshPtr + g_uskeletalMeshMaterialOffset);
        int   num  = *(int*)  ((char*)g_currentMeshPtr + g_uskeletalMeshMaterialOffset + 4);
        if (!data || i < 0 || i >= num) return;
        *(void**)((char*)data + i * sizeof(void*)) = newPtr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Per-element Clear button callback - writes nullptr into Material[i]
// and posts a Mesh-tab refresh so the row's display updates.
static void Material_ClearElement(HWND /*grid*/, int /*rowIdx*/, void* ud)
{
    int elemIdx = (int)(intptr_t)ud;
    Material_Set(elemIdx, "None", nullptr);
    PostMessageA(g_hWnd, WM_AB_REFRESH_MESH_TAB, 0, 0);
}

// PropertyGrid populateChildren for the Material array - adds a Clear
// button to each element row (selection-gated so it only shows on the
// currently-selected element).  Use button skipped pending TextureBrowser
// integration similar to the SoundBrowser PeekSelectedSound path.
static void Material_PopulateChildren(HWND grid, int elemRow, int elemIdx, void* /*ud*/)
{
    if (!grid || elemRow < 0) return;
    PropertyGrid::AddRowButton(grid, elemRow, "Clear",
                               Material_ClearElement,
                               (void*)(intptr_t)elemIdx,
                               PropertyGrid::BTN_VIS_SELECTED);
}

// SEH-isolated UObject* write.  Pulled into its own POD function so
// callers (DefaultAnim_UseCurrent, etc.) can use std::string locals
// without tripping C2712 ("Cannot use __try in functions that require
// object unwinding").
static bool WriteObjectPtrSafe(void* obj, int offset, void* newPtr)
{
    if (!obj) return false;
    __try { *(void**)((char*)obj + offset) = newPtr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Use callback for DefaultAnimation - resolves the current
// (PackageCombo, AnimSetCombo) selection to a UMeshAnimation* and
// writes it into USkeletalMesh::DefaultAnim.
static void DefaultAnim_UseCurrent(HWND grid, int rowIdx, void* userdata)
{
    if (!userdata) return;
    ObjectPickerBinding* b = (ObjectPickerBinding*)userdata;
    if (!b->obj) return;

    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();
    if (pkg.empty() || anim.empty()) return;

    void* meshAnim = GO_FindUMeshAnimRaw(g_uobjClassOffset,
                                         pkg.c_str(), anim.c_str());
    if (!meshAnim) return;

    if (!WriteObjectPtrSafe(b->obj, b->offset, meshAnim)) return;
    PropertyGrid::RefreshRow(grid, rowIdx);
}

// =====================================================================
//  Mesh tab - Mesh + Redigest categories
// =====================================================================
//  USkeletalMesh field offsets, confirmed for SCCT build 2110 by
//  matching a raw memory dump of mesh 'ATT_01' against the same mesh's
//  values in the editor - the four bounding-volume floats matched to
//  six decimal places.  Offsets are fixed per build (identical for
//  every USkeletalMesh instance), so they're safe to hard-code.
#define USKM_OFF_MINVISBOUND      0x28   // FVector
#define USKM_OFF_MAXVISBOUND      0x34   // FVector
#define USKM_OFF_VISSPHERECENTER  0x40   // FVector
#define USKM_OFF_VISSPHERERADIUS  0x4C   // FLOAT
#define USKM_OFF_SCALE            0x74   // FVector
#define USKM_OFF_TRANSLATION      0x80   // FVector  (UMesh::Origin)
#define USKM_OFF_ROTATION         0x8C   // FRotator (UMesh::RotOrigin, 3x INT)
#define USKM_OFF_LODSTYLE         0xDC   // INT

// Mesh-tab-isolated binding pools.  Kept separate from the Notify tab's
// shared g_propBindings / g_structMemberBindings pools so a Mesh tab
// rebuild can't invalidate the Notify grid's still-live row bindings.
#define MESH_TAB_PROP_POOL    64
#define MESH_TAB_STRUCT_POOL  48
static PropBinding         g_meshPropPool  [MESH_TAB_PROP_POOL];
static int                 g_meshPropCount   = 0;
static StructMemberBinding g_meshStructPool[MESH_TAB_STRUCT_POOL];
static int                 g_meshStructCount = 0;

static void MeshTabBindings_Reset()
{
    g_meshPropCount   = 0;
    g_meshStructCount = 0;
}

static PropBinding* MeshProp_Alloc(void* obj, int offset, int kind)
{
    if (g_meshPropCount >= MESH_TAB_PROP_POOL) return nullptr;
    PropBinding* b = &g_meshPropPool[g_meshPropCount++];
    b->obj = obj; b->offset = offset; b->kind = kind;
    return b;
}

static StructMemberBinding* MeshStruct_Alloc(void* obj, int structBase,
                                             int memberIdx, int kind,
                                             HWND grid, int parentRow)
{
    if (g_meshStructCount >= MESH_TAB_STRUCT_POOL) return nullptr;
    StructMemberBinding* b = &g_meshStructPool[g_meshStructCount++];
    b->obj          = obj;
    b->structBase   = structBase;
    b->memberIdx    = memberIdx;
    b->kind         = kind;
    b->grid         = grid;
    b->parentRowIdx = parentRow;
    return b;
}

// Adds an expandable Vector (X/Y/Z floats) or Rotator (Pitch/Yaw/Roll
// ints) row under the current category.  The collapsed parent shows the
// "(X=..,Y=..,Z=..)" summary; expanding reveals editable components,
// and each child write refreshes that summary (StructMember_Set).
static void MeshTab_AddVectorRow(HWND grid, void* obj, int offset,
                                 const char* name, bool isRotator)
{
    char summary[160] = "";
    if (isRotator)
    {
        int p = 0, yw = 0, r = 0;
        PropRead_Int(obj, offset + 0, &p);
        PropRead_Int(obj, offset + 4, &yw);
        PropRead_Int(obj, offset + 8, &r);
        _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                    "(Pitch=%d,Yaw=%d,Roll=%d)", p, yw, r);
    }
    else
    {
        float x = 0, y = 0, z = 0;
        PropRead_Float(obj, offset + 0, &x);
        PropRead_Float(obj, offset + 4, &y);
        PropRead_Float(obj, offset + 8, &z);
        _snprintf_s(summary, sizeof(summary), _TRUNCATE,
                    "(X=%.6f,Y=%.6f,Z=%.6f)", x, y, z);
    }

    int parent = PropertyGrid::AddRow(grid, name, summary);
    if (parent < 0) return;

    const char* lbl[3];
    int kindElem;
    if (isRotator)
    { lbl[0] = "Pitch"; lbl[1] = "Yaw"; lbl[2] = "Roll"; kindElem = PROP_KIND_INT; }
    else
    { lbl[0] = "X";     lbl[1] = "Y";   lbl[2] = "Z";    kindElem = PROP_KIND_FLOAT; }

    for (int i = 0; i < 3; ++i)
    {
        StructMemberBinding* smb = MeshStruct_Alloc(
            obj, offset, i, kindElem, grid, parent);
        if (smb)
            PropertyGrid::AddEditableRowAt(grid, parent, lbl[i],
                StructMember_Get, StructMember_Set, smb);
    }
}

// Adds a plain editable scalar (float / int) row under the current
// category.
static void MeshTab_AddScalarRow(HWND grid, void* obj, int offset,
                                 int kind, const char* name)
{
    PropBinding* b = MeshProp_Alloc(obj, offset, kind);
    if (b)
        PropertyGrid::AddEditableRow(grid, name,
            PropBinding_Get, PropBinding_Set, b);
    else
        PropertyGrid::AddRow(grid, name, "");
}

static void RefreshMeshTab()
{
    HWND grid = GetDlgItem(g_hWnd, IDC_AB_MESH_GRID);
    if (!grid) return;

    // First-use probe of the Material TArray offset.  Logged to the
    // editor log for diagnostics.
    if (g_uskeletalMeshMaterialOffset == 0)
    {
        int off = GO_ProbeMaterialOffset();
        if (off > 0)
        {
            char msg[96];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "AnimationBrowser: UMesh::Material offset = 0x%X", off);
            Logger::log(msg);
            g_uskeletalMeshMaterialOffset = off;
        }
        else
        {
            Logger::log("AnimationBrowser: UMesh::Material offset probe failed - "
                        "load a .ukx with non-empty Material to enable probe.");
            g_uskeletalMeshMaterialOffset = -1;
        }
    }

    // First-use probe of the DefaultAnim pointer offset.  Requires a
    // USkeletalMesh in memory whose DefaultAnim is non-null; if every
    // loaded mesh has DefaultAnim=null we leave the probe state at 0
    // so subsequent refreshes can re-attempt after a mesh has been
    // linked to an animation.
    if (g_uskeletalMeshDefaultAnimOffset == 0)
    {
        int off = GO_ProbeDefaultAnimOffset();
        if (off > 0)
        {
            char msg[96];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "AnimationBrowser: USkeletalMesh::DefaultAnim offset = 0x%X", off);
            Logger::log(msg);
            g_uskeletalMeshDefaultAnimOffset = off;
        }
        // Don't latch to -1 here; user can link a default anim later.
    }

    // Cache the currently-selected USkeletalMesh for Material accessors.
    std::string pkg  = PackageComboText();
    std::string mesh = MeshComboText();
    g_currentMeshPtr = (!pkg.empty() && !mesh.empty() && g_uobjClassOffset != 0)
        ? GO_FindSkeletalMeshRaw(g_uobjClassOffset, pkg.c_str(), mesh.c_str())
        : nullptr;

    PropertyGrid::BeginUpdate(grid);
    PropertyGrid::Clear(grid);

    // Reset this tab's isolated binding pools before any
    // new bindings are allocated.  The Notify tab has its own pools so
    // we don't clobber its bindings here.
    MeshObjPickerBindings_Reset();
    MeshTabBindings_Reset();

    // Category order mirrors the MeshEditProps UnrealScript class:
    // Mesh, Redigest, Animation, Skin.  (LOD / Collision / Attach /
    // Impostor categories follow once their offsets are mapped.)
    PropertyGrid::AddCategory(grid, "Mesh");
    if (g_currentMeshPtr)
    {
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_SCALE,
                             "Scale",           /*isRotator=*/false);
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_TRANSLATION,
                             "Translation",     /*isRotator=*/false);
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_ROTATION,
                             "Rotation",        /*isRotator=*/true);
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_MINVISBOUND,
                             "MinVisBound",     /*isRotator=*/false);
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_MAXVISBOUND,
                             "MaxVisBound",     /*isRotator=*/false);
        MeshTab_AddVectorRow(grid, g_currentMeshPtr, USKM_OFF_VISSPHERECENTER,
                             "VisSphereCenter", /*isRotator=*/false);
        MeshTab_AddScalarRow(grid, g_currentMeshPtr, USKM_OFF_VISSPHERERADIUS,
                             PROP_KIND_FLOAT, "VisSphereRadius");
    }
    else
    {
        PropertyGrid::AddRow(grid, "(no mesh selected)", "");
    }

    PropertyGrid::AddCategory(grid, "Redigest");
    if (g_currentMeshPtr)
        MeshTab_AddScalarRow(grid, g_currentMeshPtr, USKM_OFF_LODSTYLE,
                             PROP_KIND_INT, "LODStyle");
    else
        PropertyGrid::AddRow(grid, "LODStyle", "");

    PropertyGrid::AddCategory(grid, "Animation");

    // DefaultAnimation: editable + Clear + Use (UE2-style).
    //   - Editable: type/paste a "MeshAnimation'Pkg.Group.Name'" ref.
    //   - Clear:    sets DefaultAnim back to null.
    //   - Use:      writes the UMeshAnimation* matching the current
    //               (Package + Animation Set) combo selection.
    if (g_currentMeshPtr && g_uskeletalMeshDefaultAnimOffset > 0)
    {
        ObjectPickerBinding* defAnimB = MeshObjPickerBindings_Alloc(
            g_currentMeshPtr, g_uskeletalMeshDefaultAnimOffset);
        if (defAnimB)
        {
            int row = PropertyGrid::AddEditableRowAt(
                grid, /*parent=*/-1, "DefaultAnimation",
                ObjPicker_Get, ObjPicker_Set, defAnimB);
            if (row >= 0)
            {
                // Clear sits to the left of Use, both selection-gated.
                PropertyGrid::AddRowButton(
                    grid, row, "Clear",
                    ObjPicker_Clear, defAnimB,
                    PropertyGrid::BTN_VIS_SELECTED);
                PropertyGrid::AddRowButton(
                    grid, row, "Use",
                    DefaultAnim_UseCurrent, defAnimB,
                    PropertyGrid::BTN_VIS_SELECTED);
            }
        }
    }
    else
    {
        // No mesh selected (or offset not yet probed) - show a read-only
        // "None" so the row layout still appears.
        PropertyGrid::AddRow(grid, "DefaultAnimation", "None");
    }

    PropertyGrid::AddCategory(grid, "Skin");
    PropertyGrid::ArrayOps matOps = {
        Material_Count,
        Material_Get,
        Material_Set,                 // per-element editable
        nullptr, nullptr, nullptr,
        Material_PopulateChildren     // Clear button per element
    };
    PropertyGrid::AddArray(grid, "Material", matOps, nullptr);

    PropertyGrid::EndUpdate(grid);
}

static void CommitSequenceRate()
{
    if (!g_currentSeqPtr || g_uobjClassOffset == 0) return;
    HWND rateH = GetDlgItem(g_hWnd, IDC_AB_SEQ_RATE_EDIT);
    if (!rateH) return;
    char buf[32] = "";
    GetWindowTextA(rateH, buf, sizeof(buf));
    float v = static_cast<float>(atof(buf));
    GO_WriteMeshAnimSeqRateRaw(g_uobjClassOffset, g_currentSeqPtr, v);
}

static void RefreshSequenceList()
{
    if (!g_hSeqList) return;
    SendMessageA(g_hSeqList, LB_RESETCONTENT, 0, 0);

    if (g_uobjClassOffset == 0) return;

    // Use the authoritative listbox-row read so we pick up dropdown
    // changes before the edit fields catch up to them.
    std::string pkgStr  = PackageComboText();
    std::string animStr = AnimComboText();
    if (pkgStr.empty() || animStr.empty()) return;

    if (!GO_EnumSequencesRaw(g_uobjClassOffset,
                             pkgStr.c_str(), animStr.c_str(), &s_seqBuf))
    {
        Logger::log("AnimationBrowser: sequence enum AV'd");
        return;
    }

    for (int i = 0; i < s_seqBuf.count; ++i)
        SendMessageA(g_hSeqList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(s_seqBuf.items[i]));

    // Auto-select the top-most sequence so the right pane shows
    // something useful on package/mesh/anim-set changes and on
    // File > Open instead of leaving the user with an empty grid until
    // they manually click a row.  LB_SETCURSEL does NOT fire
    // LBN_SELCHANGE, so callers that want the dependent tabs refreshed
    // (Sequence / Notify) must call RefreshSequenceTab afterwards -
    // every existing call site already does that.
    if (s_seqBuf.count > 0)
        SendMessageA(g_hSeqList, LB_SETCURSEL, 0, 0);
}

static void RefreshAll()
{
    RefreshPackages();
    RefreshMeshList();
    RefreshAnimList();
    RefreshSequenceList();
    RefreshSequenceTab();
    RefreshAnimSetTab();
    RefreshPrefsTab();
    RefreshMeshTab();
    UpdateWindowTitle();
}

// =====================================================================
//  silent background auto-refresh
// =====================================================================
//  StaticMeshBrowser-style live update of the package list when the
//  engine loads new packages (e.g. user opened a map that pulls in
//  animation/mesh packages we haven't seen yet).
//
//  Polled from a 1-second WM_TIMER and also kicked from WM_ACTIVATE so
//  switching back to the AB after loading a map gives immediate
//  feedback.
//
//  Dirty detection: we can't use GObjects.Num as the marker - loading a
//  map unloads the old map's objects (freeing slots) and refills those
//  same slots, so Num frequently stays identical while the contents
//  changed completely.  That's exactly the "Aquarius never appears"
//  bug.  Instead we re-walk the package set every poll (cheap - a
//  couple linear passes over GObjects) and only touch the combo when
//  the resulting list actually differs from what's displayed.
//
//  Won't disturb the user mid-interaction: suppressed while any of the
//  three combos have their dropdown open, and uses forceFirst=false
//  on RefreshMeshList/AnimList so the existing mesh/anim selections
//  stick around as long as they're still valid.

#define IDT_AB_AUTO_REFRESH  0xAB01

// True if hCombo's items exactly match `items` (case-insensitive, in
// order).  Used to decide whether a refresh would actually change
// anything before we disturb the control.
static bool ComboMatchesList(HWND hCombo, const std::vector<std::string>& items)
{
    if (!hCombo) return true;
    int count = static_cast<int>(SendMessageA(hCombo, CB_GETCOUNT, 0, 0));
    if (count != static_cast<int>(items.size())) return false;
    for (int i = 0; i < count; ++i)
    {
        int len = static_cast<int>(SendMessageA(hCombo, CB_GETLBTEXTLEN, i, 0));
        if (len < 0 || len > 255) return false;
        char buf[256] = "";
        SendMessageA(hCombo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
        if (_stricmp(buf, items[i].c_str()) != 0) return false;
    }
    return true;
}

// Builds the sorted, de-duplicated package list the same way
// RefreshPackages does, but returns it instead of filling the combo.
static void CollectPackageList(std::vector<std::string>& out)
{
    out.clear();
    GO_WalkByClass("SkeletalMesh",  nullptr, &out, nullptr);
    GO_WalkByClass("MeshAnimation", nullptr, &out, nullptr);
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const std::string& a, const std::string& b) {
                              return _stricmp(a.c_str(), b.c_str()) == 0;
                          }),
              out.end());
}

static void AutoRefreshIfDirty()
{
    if (!g_hWnd || !IsWindowVisible(g_hWnd)) return;

    // Don't yank the listbox out from under a user who has a dropdown
    // open or is mid-pick on one of the combos.
    if (g_hPackageCombo && SendMessageA(g_hPackageCombo, CB_GETDROPPEDSTATE, 0, 0)) return;
    if (g_hMeshCombo    && SendMessageA(g_hMeshCombo,    CB_GETDROPPEDSTATE, 0, 0)) return;
    if (g_hAnimCombo    && SendMessageA(g_hAnimCombo,    CB_GETDROPPEDSTATE, 0, 0)) return;

    // Re-walk the package set and bail if it's identical to what's
    // already shown - no flicker, no disruption on idle polls.
    std::vector<std::string> pkgs;
    CollectPackageList(pkgs);
    bool packagesChanged = !ComboMatchesList(g_hPackageCombo, pkgs);

    // Even when the package set is stable, the engine may have loaded
    // more meshes / anim sets *into* the currently-selected package
    // (the "map uses SOME assets from a package" case).  Re-walk those
    // too and refresh if they grew/shrank.
    std::string curPkg = PackageComboText();
    bool meshesChanged = false, animsChanged = false;
    if (!curPkg.empty())
    {
        std::vector<std::string> meshes, anims;
        GO_WalkByClass("SkeletalMesh",  &meshes, nullptr, curPkg.c_str());
        GO_WalkByClass("MeshAnimation", &anims,  nullptr, curPkg.c_str());
        std::sort(meshes.begin(), meshes.end(),
                  [](const std::string& a, const std::string& b) {
                      return _stricmp(a.c_str(), b.c_str()) < 0; });
        std::sort(anims.begin(), anims.end(),
                  [](const std::string& a, const std::string& b) {
                      return _stricmp(a.c_str(), b.c_str()) < 0; });
        meshesChanged = !ComboMatchesList(g_hMeshCombo, meshes);
        animsChanged  = !ComboMatchesList(g_hAnimCombo, anims);
    }

    if (!packagesChanged && !meshesChanged && !animsChanged) return;

    std::string prevPkg = curPkg;

    if (packagesChanged)
        RefreshPackages();   // preserves selection if entry still present

    if (PackageComboText() == prevPkg)
    {
        // Package selection survived - silently refill mesh / anim with
        // the new pool while keeping the user's current choices.
        RefreshMeshList(/*forceFirst=*/false);
        RefreshAnimList(/*forceFirst=*/false);
    }
    else
    {
        // RefreshPackages dropped our package (it's gone) - full cascade.
        RefreshMeshList();
        RefreshAnimList();
        RefreshSequenceList();
        RefreshSequenceTab();
        RefreshMeshTab();
        UpdateWindowTitle();
    }
}

// Adds `str` to a combo if not already present and selects it.  Used
// to seed the package combo after File > Open / Import.
static void ComboAddSelect(HWND hCombo, const char* str)
{
    if (!hCombo || !str || !*str) return;
    LRESULT r = SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                             reinterpret_cast<LPARAM>(str));
    if (r == CB_ERR)
        r = SendMessageA(hCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(str));
    if (r != CB_ERR)
        SendMessageA(hCombo, CB_SETCURSEL, r, 0);
}

// ---------------------------------------------------------------------
//  File menu handlers - mirror UT2004 WBrowserAnimation::OnCommand
//  (cases IDMN_FileOpen / IDMN_FileSave / IDMN_FILE_IMPORTMESH /
//   IDMN_FILE_IMPORTANIM / IDMN_FILE_IMPORTANIMMORE).
//  We strip out the Maya/Merge/Overwrite/KeepNotifies prompt for now;
//  a later pass will pop the surviving Import dialog (resource 140) and
//  read those flags from it.
// ---------------------------------------------------------------------
static void OnFileOpen(HWND hParent)
{
    char file[8192] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hParent;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file);
    ofn.lpstrFilter  = "Animated Mesh Packages (*.ukx)\0*.ukx\0All Files\0*.*\0\0";
    ofn.lpstrDefExt  = "ukx";
    ofn.lpstrTitle   = "Open Animated Mesh Package";
    ofn.lpstrInitialDir = "..\\Packages\\Animations";
    ofn.Flags        = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn)) return;

    char cmd[8192 + 32];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "OBJ LOAD FILE=\"%s\"", file);
    ExecEditorCommand(cmd);

    // GObjects now contains the freshly loaded package - rebuild combos.
    RefreshPackages();

    // Select the just-loaded package.
    const char* slash = std::strrchr(file, '\\');
    const char* base  = slash ? slash + 1 : file;
    char pkgName[260] = "";
    strncpy_s(pkgName, base, _TRUNCATE);
    char* dot = std::strrchr(pkgName, '.');
    if (dot) *dot = '\0';
    ComboAddSelect(g_hPackageCombo, pkgName);
    RefreshMeshList();
    RefreshAnimList();
    RefreshSequenceList();
    RefreshSequenceTab();
    // Property tabs were still bound to the previously-loaded package's
    // mesh/anim - in particular the Mesh tab's DefaultAnimation row keeps
    // showing the old reference until the user nudges the combos.  Mirror
    // the IDC_AB_PACKAGE_COMBO path so everything is in sync after Open.
    RefreshMeshTab();
    RefreshAnimSetTab();
    RefreshPrefsTab();
    RefreshNotifyTab();
    UpdateWindowTitle();
}

static void OnFileSave(HWND hParent)
{
    std::string pkg = GetComboText(g_hPackageCombo);
    if (pkg.empty())
    {
        MessageBoxA(hParent, "Select a package first.", "Save", MB_OK);
        return;
    }

    char file[8192];
    _snprintf_s(file, sizeof(file), _TRUNCATE, "%s.ukx", pkg.c_str());

    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hParent;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file);
    ofn.lpstrFilter  = "Animation Packages (*.ukx)\0*.ukx\0All Files\0*.*\0\0";
    ofn.lpstrDefExt  = "ukx";
    ofn.lpstrTitle   = "Save Animation Package";
    ofn.lpstrInitialDir = "..\\Packages\\Animations";
    ofn.Flags        = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameA(&ofn)) return;

    char cmd[8192 + 64];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "OBJ SAVEPACKAGE PACKAGE=\"%s\" FILE=\"%s\"", pkg.c_str(), file);
    ExecEditorCommand(cmd);
}

// =====================================================================
//  Import dialog 140 (surviving in the EXE's resources)
// =====================================================================
//  Layout (decoded from the EXE):
//    1067 EDIT     Package
//    1066 EDIT     Group   (disabled - UT2004 source keeps it unused too)
//    1065 EDIT     Name
//    1284 CHECK    "Merge sequences into existing"   (anim only)
//    1285 CHECK    "Assume Maya coordinates"         (mesh only)
//    1294 CHECK    "Overwrite existing sequences"    (anim only)
//    1295 CHECK    "Keep Notifies"                   (anim only)
//    1   OK   /   2 Cancel

#define IDD_AB_IMPORT       140
#define IDC_IMP_PACKAGE     1067
#define IDC_IMP_GROUP       1066
#define IDC_IMP_NAME        1065
#define IDC_IMP_MERGE       1284
#define IDC_IMP_MAYA        1285
#define IDC_IMP_OVERWRITE   1294
#define IDC_IMP_KEEPNOTIFY  1295

struct ImportDlgData
{
    char  file[MAX_PATH * 2];
    char  package[256];
    char  group[256];
    char  name[256];
    BOOL  bMergeSeqs;
    BOOL  bMayaCoords;
    BOOL  bOverwriteSeqs;
    BOOL  bKeepNotifies;
    BOOL  isAnim;
    BOOL  isAppend;
};

static INT_PTR CALLBACK ImportDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static ImportDlgData* d = nullptr;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        d = reinterpret_cast<ImportDlgData*>(lParam);

        SetWindowTextA(hDlg,
            d->isAnim ? (d->isAppend ? "Import Animation (Append)" : "Import Animation")
                      : "Import Mesh");

        SetDlgItemTextA(hDlg, IDC_IMP_PACKAGE, d->package);
        SetDlgItemTextA(hDlg, IDC_IMP_GROUP,   d->group);
        SetDlgItemTextA(hDlg, IDC_IMP_NAME,    d->name);

        if (d->isAppend) CheckDlgButton(hDlg, IDC_IMP_MERGE, BST_CHECKED);

        // Enable only the options that make sense for this import type.
        EnableWindow(GetDlgItem(hDlg, IDC_IMP_MAYA),       !d->isAnim);
        EnableWindow(GetDlgItem(hDlg, IDC_IMP_MERGE),       d->isAnim);
        EnableWindow(GetDlgItem(hDlg, IDC_IMP_OVERWRITE),   d->isAnim);
        EnableWindow(GetDlgItem(hDlg, IDC_IMP_KEEPNOTIFY),  d->isAnim);

        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            GetDlgItemTextA(hDlg, IDC_IMP_PACKAGE, d->package, sizeof(d->package));
            GetDlgItemTextA(hDlg, IDC_IMP_GROUP,   d->group,   sizeof(d->group));
            GetDlgItemTextA(hDlg, IDC_IMP_NAME,    d->name,    sizeof(d->name));
            d->bMergeSeqs     = (IsDlgButtonChecked(hDlg, IDC_IMP_MERGE)      == BST_CHECKED);
            d->bMayaCoords    = (IsDlgButtonChecked(hDlg, IDC_IMP_MAYA)       == BST_CHECKED);
            d->bOverwriteSeqs = (IsDlgButtonChecked(hDlg, IDC_IMP_OVERWRITE)  == BST_CHECKED);
            d->bKeepNotifies  = (IsDlgButtonChecked(hDlg, IDC_IMP_KEEPNOTIFY) == BST_CHECKED);
            if (!d->package[0] || !d->name[0])
            {
                MessageBoxA(hDlg, "Package and Name are required.",
                            "Message", MB_OK);
                return TRUE;
            }
            EndDialog(hDlg, IDOK);
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

static void OnImport(HWND hParent, bool isAnim, bool append)
{
    char file[8192] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hParent;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file);
    if (isAnim)
    {
        ofn.lpstrFilter = "Skeletal animation raw data (*.psa)\0*.psa\0All Files\0*.*\0\0";
        ofn.lpstrDefExt = "psa";
        ofn.lpstrTitle  = append ? "Append Animation"
                                 : "Import Animation";
    }
    else
    {
        ofn.lpstrFilter = "Skeletal mesh raw data (*.psk)\0*.psk\0All Files\0*.*\0\0";
        ofn.lpstrDefExt = "psk";
        ofn.lpstrTitle  = "Import Mesh";
    }
    ofn.lpstrInitialDir = "..\\Animations";
    ofn.Flags        = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn)) return;

    // Pre-fill the ImportDlgData from current state + filename.
    ImportDlgData d = {};
    strncpy_s(d.file, file, _TRUNCATE);
    d.isAnim   = isAnim   ? TRUE : FALSE;
    d.isAppend = append   ? TRUE : FALSE;

    {
        const char* slash = std::strrchr(file, '\\');
        const char* base  = slash ? slash + 1 : file;
        strncpy_s(d.name, base, _TRUNCATE);
        char* dot = std::strrchr(d.name, '.');
        if (dot) *dot = '\0';
    }
    strncpy_s(d.group, "Default", _TRUNCATE);

    std::string pkg = GetComboText(g_hPackageCombo);
    if (pkg.empty()) pkg = d.name;
    strncpy_s(d.package, pkg.c_str(), _TRUNCATE);

    // Append-anim: pre-target the currently selected Animation Set so
    // sequences land in the right object.
    if (append && isAnim)
    {
        std::string cur = GetComboText(g_hAnimCombo);
        if (!cur.empty()) strncpy_s(d.name, cur.c_str(), _TRUNCATE);
    }

    // Show the surviving import dialog from the EXE's resources.
    INT_PTR ret = DialogBoxParamA(GetModuleHandleA(nullptr),
                                  MAKEINTRESOURCEA(IDD_AB_IMPORT),
                                  hParent,
                                  ImportDlgProc,
                                  reinterpret_cast<LPARAM>(&d));
    if (ret != IDOK) return;

    // Compose NEWANIM IMPORT options from the dialog choices + the
    // transient Prefs/AnimSet knobs the user already set on the tabs.
    char extra[256] = "";
    if (d.bMayaCoords)
        strncat_s(extra, sizeof(extra), "YAW=-64 PITCH=0 ROLL=64 ", _TRUNCATE);
    else
        strncat_s(extra, sizeof(extra), "YAW=0 PITCH=0 ROLL=0 ",    _TRUNCATE);

    // Splinter Cell's PSK pipeline takes LODSTYLE=N to drive its mesh
    // section/digest setup.  Only forward when it differs from the
    // engine's typical default (10) so we don't override anything
    // unintentionally on a "normal" import.
    if (!isAnim && g_prefsLODStyle != 10)
    {
        char lsBuf[32];
        _snprintf_s(lsBuf, sizeof(lsBuf), _TRUNCATE,
                    "LODSTYLE=%d ", g_prefsLODStyle);
        strncat_s(extra, sizeof(extra), lsBuf, _TRUNCATE);
    }

    if (isAnim && g_animGlobalCompression < 1.0f)
    {
        char compBuf[32];
        _snprintf_s(compBuf, sizeof(compBuf), _TRUNCATE,
                    "COMP=%.4f ", g_animGlobalCompression);
        strncat_s(extra, sizeof(extra), compBuf, _TRUNCATE);
    }

    char cmd[8192 + 1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
        "NEWANIM IMPORT FILE=\"%s\" PACKAGE=\"%s\" NAME=\"%s\" %sBROWSER",
        d.file, d.package, d.name, extra);
    ExecEditorCommand(cmd);

    // GObjects now contains the freshly imported objects - rebuild combos
    // and select what we just brought in.
    RefreshPackages();
    ComboAddSelect(g_hPackageCombo, d.package);
    RefreshMeshList();
    RefreshAnimList();
    if (isAnim)
        ComboAddSelect(g_hAnimCombo, d.name);
    else
        ComboAddSelect(g_hMeshCombo, d.name);
    RefreshSequenceList();
    RefreshSequenceTab();

    // TODO: if d.bMergeSeqs / d.bOverwriteSeqs / d.bKeepNotifies,
    // post-process the imported UMeshAnimation to merge sequences into
    // the destination animation set (mirrors UT2004 WDlgNewMesh logic).
}

static void OnLoadEntirePackage(HWND /*hParent*/)
{
    // UE2's `OBJ LOAD` wants FILE= with a path on disk - PACKAGE= is
    // not accepted (engine logs `ExecWarning: Missing filename`).
    // Animation packages live as .ukx in the editor's ..\Animations\
    // dir, the same folder File > Open / Import default to.
    std::string pkg = GetComboText(g_hPackageCombo);
    if (pkg.empty()) return;
    char cmd[512];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "OBJ LOAD FILE=\"..\\Animations\\%s.ukx\"", pkg.c_str());
    ExecEditorCommand(cmd);
    RefreshAll();
}

// =====================================================================
//  menu-bar Rename / Delete handlers + Copy Shortcut
// =====================================================================
//  Mirrors SoundBrowser::SB_HandleRename / SB_HandleDelete but adapted
//  for USkeletalMesh and UMeshAnimation.  Unlike sounds, which live
//  under Package.Group.Name, meshes and animation sets in SCCT usually
//  sit directly under their package (Group is blank).  The rename
//  dialog still exposes the Group field so the user can re-home an
//  asset by filling it in.
//
//  Both Rename and Delete use the same editor exec commands UnrealEd 2
//  uses internally (`OBJ RENAME` / `OBJ DELETE`), so the engine handles
//  serialisation, reference fix-ups, and "object is in use" checks.

// Dialog resource IDs (baked into ChaosTheory_Editor.exe; identical to
// SoundBrowser's rename dialog).
#define IDDIALOG_RENAME      19805
#define IDEC_NEWPACKAGE      1066
#define IDEC_NEWGROUP        1067
#define IDEC_NAME            1065

struct RenameAssetData
{
    char title      [128];
    char oldName    [256];
    char oldGroup   [256];
    char oldPackage [256];
    char newName    [256];
    char newGroup   [256];
    char newPackage [256];
};

static INT_PTR CALLBACK RenameAssetDlgProc(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongA(hDlg, GWL_USERDATA, static_cast<LONG>(lParam));
        RenameAssetData* d = reinterpret_cast<RenameAssetData*>(lParam);
        if (!d) return TRUE;

        if (d->title[0])
            SetWindowTextA(hDlg, d->title);

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
            RenameAssetData* d = reinterpret_cast<RenameAssetData*>(
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

// Pops the EXE's rename dialog seeded with the supplied old names and
// runs OBJ RENAME on confirmation.  Returns true if the user clicked
// OK and the command was issued.  out* receive the user-chosen new
// identifiers so callers can re-select the renamed asset post-refresh.
static bool DoRenameAsset(HWND hParent, const char* title,
                          const char* oldPackage, const char* oldGroup,
                          const char* oldName,
                          char* outNewPackage, int outNewPackageSize,
                          char* outNewName,    int outNewNameSize)
{
    if (!oldName || !*oldName || !oldPackage || !*oldPackage) return false;

    RenameAssetData d = {};
    if (title) strncpy_s(d.title, sizeof(d.title), title, _TRUNCATE);
    strncpy_s(d.oldName,    sizeof(d.oldName),    oldName,                  _TRUNCATE);
    strncpy_s(d.oldPackage, sizeof(d.oldPackage), oldPackage,               _TRUNCATE);
    strncpy_s(d.oldGroup,   sizeof(d.oldGroup),   oldGroup ? oldGroup : "", _TRUNCATE);

    INT_PTR ret = DialogBoxParamA(GetModuleHandleA(nullptr),
                                  MAKEINTRESOURCEA(IDDIALOG_RENAME),
                                  hParent,
                                  RenameAssetDlgProc,
                                  reinterpret_cast<LPARAM>(&d));
    if (ret != IDOK) return false;

    if (!d.newName[0] || !d.newPackage[0])
    {
        MessageBoxA(hParent, "Package and Name are required.",
                    "Message", MB_OK);
        return false;
    }

    char cmd[MAX_PATH * 2 + 512];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "OBJ RENAME OLDNAME=\"%s\" OLDGROUP=\"%s\" OLDPACKAGE=\"%s\""
                " NEWNAME=\"%s\" NEWGROUP=\"%s\" NEWPACKAGE=\"%s\"",
                d.oldName, d.oldGroup, d.oldPackage,
                d.newName, d.newGroup, d.newPackage);
    ExecEditorCommand(cmd);

    if (outNewPackage && outNewPackageSize > 0)
        strncpy_s(outNewPackage, outNewPackageSize, d.newPackage, _TRUNCATE);
    if (outNewName && outNewNameSize > 0)
        strncpy_s(outNewName, outNewNameSize, d.newName, _TRUNCATE);
    return true;
}

// Runs OBJ DELETE on the asset (no confirmation prompt, matching UE2's
// instant-delete behavior).
//
// IMPORTANT: OBJ DELETE must be dispatched through UEditorEngine::Get,
// NOT FExec::Exec.  Exec silently ignores the DELETE verb - which is
// exactly why our earlier ExecEditorCommand("OBJ DELETE ...") did
// nothing at all.  SoundBrowser's working delete uses Get for the same
// reason (see CallEditorGet above).  On a successful delete the engine
// destroys the UObject immediately and nulls its GObjects slot, so our
// combo walker stops seeing it; if the object is still referenced the
// engine refuses and the asset survives the next RefreshAll.
//
// Returns true if the command was issued.
static bool DoDeleteAsset(HWND /*hParent*/, const char* className,
                          const char* assetName)
{
    if (!assetName || !*assetName || !className || !*className) return false;

    char cmd[512];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "DELETE CLASS=%s OBJECT=\"%s\"",
                className, assetName);
    CallEditorGet("OBJ", cmd);
    return true;
}

static void OnRenameMesh(HWND hParent)
{
    std::string pkg  = PackageComboText();
    std::string mesh = MeshComboText();
    if (pkg.empty() || mesh.empty()) return;

    char newPkg[256]  = "";
    char newName[256] = "";
    if (!DoRenameAsset(hParent, "Rename Mesh",
                       pkg.c_str(), "", mesh.c_str(),
                       newPkg,  sizeof(newPkg),
                       newName, sizeof(newName)))
        return;

    RefreshAll();
    if (newPkg[0])  ComboAddSelect(g_hPackageCombo, newPkg);
    if (newName[0]) ComboAddSelect(g_hMeshCombo,    newName);
    RefreshMeshTab();
    UpdateWindowTitle();
}

// Returns true if `name` is still present in `hCombo`'s item list.
// Used after OBJ DELETE / RefreshAll to detect "in use" failures (the
// engine silently leaves the object alive when something references
// it, so the only way to tell is to look for it again post-refresh).
static bool ComboHasItem(HWND hCombo, const char* name)
{
    if (!hCombo || !name || !*name) return false;
    LRESULT idx = SendMessageA(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                               reinterpret_cast<LPARAM>(name));
    return idx != CB_ERR;
}

static void OnDeleteMesh(HWND hParent)
{
    std::string pkg  = PackageComboText();
    std::string mesh = MeshComboText();
    if (mesh.empty()) return;
    if (!DoDeleteAsset(hParent, "SkeletalMesh", mesh.c_str())) return;
    RefreshAll();

    // If the engine refused the delete (object in use), the mesh will
    // still be in GObjects and therefore still in the combo.  Surface
    // it the same way SoundBrowser::SB_HandleDelete does.
    if (ComboHasItem(g_hMeshCombo, mesh.c_str()))
    {
        char errMsg[512];
        _snprintf_s(errMsg, sizeof(errMsg), _TRUNCATE,
                    "Can't delete mesh.\n\n"
                    "SkeletalMesh %s%s%s is in use",
                    pkg.empty() ? "" : pkg.c_str(),
                    pkg.empty() ? "" : ".",
                    mesh.c_str());
        MessageBoxA(hParent, errMsg, "Message", MB_OK);
        ComboAddSelect(g_hMeshCombo, mesh.c_str());
    }
}

static void OnRenameAnim(HWND hParent)
{
    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();
    if (pkg.empty() || anim.empty()) return;

    char newPkg[256]  = "";
    char newName[256] = "";
    if (!DoRenameAsset(hParent, "Rename Animation Set",
                       pkg.c_str(), "", anim.c_str(),
                       newPkg,  sizeof(newPkg),
                       newName, sizeof(newName)))
        return;

    RefreshAll();
    if (newPkg[0])  ComboAddSelect(g_hPackageCombo, newPkg);
    if (newName[0]) ComboAddSelect(g_hAnimCombo,    newName);
    RefreshSequenceList();
    RefreshSequenceTab();
    UpdateWindowTitle();
}

static void OnDeleteAnim(HWND hParent)
{
    std::string pkg  = PackageComboText();
    std::string anim = AnimComboText();
    if (anim.empty()) return;
    if (!DoDeleteAsset(hParent, "MeshAnimation", anim.c_str())) return;
    RefreshAll();

    if (ComboHasItem(g_hAnimCombo, anim.c_str()))
    {
        char errMsg[512];
        _snprintf_s(errMsg, sizeof(errMsg), _TRUNCATE,
                    "Can't delete animation set.\n\n"
                    "MeshAnimation %s%s%s is in use",
                    pkg.empty() ? "" : pkg.c_str(),
                    pkg.empty() ? "" : ".",
                    anim.c_str());
        MessageBoxA(hParent, errMsg, "Message", MB_OK);
        ComboAddSelect(g_hAnimCombo, anim.c_str());
    }
}

// Same payload as the Sequence-list right-click Copy Shortcut: the
// bare sequence name, which is what PlayAnim / LoopAnim / etc. take.
static void OnCopyShortcut(HWND /*hParent*/)
{
    if (g_curSeqName[0] != '\0')
        CopyAnsiToClipboard(g_curSeqName);
}

// ---------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------
static void PositionChildControls(int width, int height)
{
    // toolbar spans the full width at the very top.  TB_AUTOSIZE
    // auto-derives the height from the bitmap-tile size, but we pin the
    // y/width to the client rect so the buttons stay above the combos
    // through resizes.
    if (g_hToolbar)
    {
        SetWindowPos(g_hToolbar, nullptr,
                     0, 0, width, kToolbarHeight, SWP_NOZORDER);
        SendMessageA(g_hToolbar, TB_AUTOSIZE, 0, 0);
    }

    // Top row: Package label + combo
    SetWindowPos(GetDlgItem(g_hWnd, IDC_AB_PACKAGE_LABEL), nullptr,
                 8, kComboRowsTop + 4, 60, 16, SWP_NOZORDER);
    SetWindowPos(g_hPackageCombo, nullptr,
                 70, kComboRowsTop, 280, 200, SWP_NOZORDER);

    // Second row: Mesh label + combo, Anim label + combo side-by-side
    int row2 = kComboRowsTop + kComboRowHeight + 4;
    SetWindowPos(GetDlgItem(g_hWnd, IDC_AB_MESH_LABEL), nullptr,
                 8, row2 + 4, 60, 16, SWP_NOZORDER);
    SetWindowPos(g_hMeshCombo, nullptr,
                 70, row2, 240, 200, SWP_NOZORDER);

    SetWindowPos(GetDlgItem(g_hWnd, IDC_AB_ANIM_LABEL), nullptr,
                 320, row2 + 4, 70, 16, SWP_NOZORDER);
    SetWindowPos(g_hAnimCombo, nullptr,
                 395, row2, 240, 200, SWP_NOZORDER);

    // Sequence list label + listbox on the left
    int top = row2 + kComboRowHeight + 8;
    SetWindowPos(GetDlgItem(g_hWnd, IDC_AB_SEQ_LABEL), nullptr,
                 8, top, kSeqListWidth, 16, SWP_NOZORDER);
    SetWindowPos(g_hSeqList, nullptr,
                 8, top + 18, kSeqListWidth, height - top - 30, SWP_NOZORDER);

    // Property panel (tab control) on the right
    int propsX = kSeqListWidth + 16;
    int propsW = width  - propsX - 8;
    int propsH = height - top    - 12;
    SetWindowPos(g_hTabs, nullptr, propsX, top, propsW, propsH, SWP_NOZORDER);

    // Each tab page fills the tab control's display area.  TCM_ADJUSTRECT
    // computes the inner client rect (excluding the tab strip).
    RECT inner = { 0, 0, propsW, propsH };
    SendMessageA(g_hTabs, TCM_ADJUSTRECT, FALSE,
                 reinterpret_cast<LPARAM>(&inner));
    int pageX = propsX + inner.left;
    int pageY = top    + inner.top;
    int pageW = inner.right  - inner.left;
    int pageH = inner.bottom - inner.top;

    for (int i = 0; i < AB_TAB_COUNT; ++i)
    {
        if (g_hTabPage[i])
            SetWindowPos(g_hTabPage[i], nullptr,
                         pageX, pageY, pageW, pageH, SWP_NOZORDER);

        // Reposition this tab's property-panel children using their
        // recorded relative (x,y,w,h) inside the page rect.  Wide
        // controls (category headers, value EDITs) are stretched to the
        // page edge so the layout reflows on resize; narrow controls
        // (property name labels) keep their fixed width.
        for (auto& tc : g_tabContent[i])
        {
            int x = pageX + tc.relX;
            int y = pageY + tc.relY;
            int w = tc.w;
            int h = tc.h;
            if (tc.stretch)
            {
                w = pageW - tc.relX - kPropPadX;
                if (w < 40) w = 40;
            }
            if (tc.stretchHeight)
            {
                h = pageH - tc.relY - kPropPadX;
                if (h < 40) h = 40;
            }
            SetWindowPos(tc.hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
        }
    }
}

// Brings the requested tab page to the front, mirroring UT2004's
// WBrowserAnimation::OnCommand cases for IDMN_EDIT_MESHPROP / ANIMPROP /
// SEQUPROP / NOTIFICATIONS / PREFS.  Also toggles visibility of each
// tab's property controls (which are direct children of the main
// window, not of the page STATIC).
static void SelectTab(int tabIndex)
{
    if (!g_hTabs || tabIndex < 0 || tabIndex >= AB_TAB_COUNT) return;
    g_currentTab = tabIndex;
    SendMessageA(g_hTabs, TCM_SETCURSEL, tabIndex, 0);
    for (int i = 0; i < AB_TAB_COUNT; ++i)
    {
        int show = (i == tabIndex) ? SW_SHOW : SW_HIDE;
        ShowWindow(g_hTabPage[i], show);
        for (auto& tc : g_tabContent[i])
            ShowWindow(tc.hwnd, show);
    }
}

// ---------------------------------------------------------------------
//  Menu command dispatcher
// ---------------------------------------------------------------------
static void OnCommand(HWND hWnd, WORD id)
{
    switch (id)
    {
        case IDMN_FileOpen:               OnFileOpen(hWnd);            break;
        case IDMN_FileSave:               OnFileSave(hWnd);            break;
        case IDMN_FILE_IMPORTMESH:        OnImport(hWnd, false, false); break;
        case IDMN_FILE_IMPORTANIM:        OnImport(hWnd, true,  false); break;
        case IDMN_FILE_IMPORTANIMMORE:    OnImport(hWnd, true,  true);  break;
        case IDMN_FILE_IMPORTLOD:         OnImport(hWnd, false, false); break;
        case IDMN_AB_LOAD_ENTIRE_PACKAGE: OnLoadEntirePackage(hWnd);    break;
        case IDMN_REFRESH:                RefreshAll();                 break;

        // rename / delete / copy-shortcut menu items.
        case IDMN_EDIT_RENAMEMESH:        OnRenameMesh(hWnd);           break;
        case IDMN_EDIT_DELETEMESH:        OnDeleteMesh(hWnd);           break;
        case IDMN_EDIT_RENAMEANIM:        OnRenameAnim(hWnd);           break;
        case IDMN_EDIT_DELETEANIM:        OnDeleteAnim(hWnd);           break;
        case IDMN_EDIT_COPYSHORTCUT:      OnCopyShortcut(hWnd);         break;

        // Animation > {Mesh,Anim,Seq,Notify,Prefs} properties - jump to
        // the matching tab (same as UT2004 OnCommand).
        case IDMN_EDIT_MESHPROP:          SelectTab(AB_TAB_MESH);   break;
        case IDMN_EDIT_ANIMPROP:          SelectTab(AB_TAB_ANIM);   break;
        case IDMN_EDIT_SEQUPROP:          SelectTab(AB_TAB_SEQ);    break;
        case IDMN_EDIT_NOTIFICATIONS:     SelectTab(AB_TAB_NOTIFY); break;
        case IDMN_EDIT_PREFS:             SelectTab(AB_TAB_PREFS);  break;
        case IDMN_EDIT_GROUPS:            SelectTab(AB_TAB_SEQ);    break;

        // Items deferred to a later pass are no-ops for now so menu clicks
        // don't fall through to a beep.  They are listed explicitly to
        // make follow-up work easy to grep for.
        case IDMN_EDIT_LINKANIM:
        case IDMN_EDIT_UNLINKANIM:
        case IDMN_EDIT_APPLY:
        case IDMN_EDIT_UNDO:
        case IDMN_EDIT_ADDNOTIFY:
        case IDMN_EDIT_COPYNOTIFIES:
        case IDMN_EDIT_PASTENOTIFIES:
        case IDMN_EDIT_CLEARNOTIFIES:
        case IDMN_EDIT_CLEARGROUPS:
        case IDMN_EDIT_COPYGROUPS:
        case IDMN_EDIT_PASTEGROUPS:
        case IDMN_EDIT_COPYMESHPROPS:
        case IDMN_EDIT_PASTEMESHPROPS:
        case IDMN_MESH_REDIGESTLOD:
        case IDMN_MESH_CYCLELOD:
        case IDMN_MESH_IMPORTLOD:
        case IDMN_VIEW_INFO:
        case IDMN_VIEW_BONES:
        case IDMN_VIEW_INFLUENCES:
        case IDMN_VIEW_BOUNDS:
        case IDMN_VIEW_BACKFACE:
        case IDMN_VIEW_WIRE:
        case IDMN_VIEW_HIDESKIN:
        case IDMN_VIEW_REFPOSE:
        case IDMN_VIEW_PILLS:
        case IDMN_VIEW_RAWOFFSET:
        case IDMN_VIEW_BONENAMES:
        case IDMN_VIEW_LEVELANIM:
        case IDMN_EDIT_CHECKUNUSEDBONES:
        case IDMN_EDIT_CHECKSCRIPTREFS:
            // Hook points
            break;

        // Top-level child combo selection changes.
        case IDC_AB_PACKAGE_COMBO:
            // Package switch invalidates mesh and anim choices - rebuild
            // both with forceFirst (top of list), then the sequence list,
            // then the dependent property tabs.
            RefreshMeshList();
            RefreshAnimList();
            RefreshSequenceList();
            RefreshSequenceTab();
            RefreshMeshTab();
            UpdateWindowTitle();
            break;
        case IDC_AB_MESH_COMBO:
            // Mesh choice doesn't drive the anim-set list (we don't track
            // DefaultAnim wiring yet), but the Mesh tab's Material array
            // does depend on which USkeletalMesh is selected.
            RefreshMeshTab();
            UpdateWindowTitle();
            break;
        case IDC_AB_ANIM_COMBO:
            RefreshSequenceList();
            RefreshSequenceTab();
            UpdateWindowTitle();
            break;
        case IDC_AB_SEQ_LIST:
            // User picked a sequence in the left listbox - populate the
            // Sequence property tab from that FMeshAnimSeq.
            RefreshSequenceTab();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------
//  Window proc
// ---------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HINSTANCE hInst = GetModuleHandleA(nullptr);  // ChaosTheory_Editor.exe

        // Show the editor EXE's icon in our title bar / Alt-Tab / task-
        // bar - same one UnrealEd's main window and the other browsers
        // use.  ExtractIconExA against the running EXE's file path
        // reliably grabs the first icon group regardless of its
        // numeric resource ID; LoadImage with LR_SHARED kept returning
        // NULL here (probably because of the size + sharing constraint
        // interacting badly with how Ubisoft packed the icon group).
        {
            char exePath[MAX_PATH];
            DWORD len = GetModuleFileNameA(hInst, exePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                HICON hIconBig = nullptr;
                HICON hIconSm  = nullptr;
                ExtractIconExA(exePath, 0, &hIconBig, &hIconSm, 1);
                if (hIconSm)
                    SendMessageA(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
                if (hIconBig)
                    SendMessageA(hWnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
            }
        }

        // restore the Animation Browser toolbar Ubisoft kept
        // in the EXE's resources but never wired up.  Bitmap 29786 lives
        // in ChaosTheory_Editor.exe; we load it via the editor's HMODULE
        // and add buttons whose command IDs match our existing File
        // menu handlers (Open / Save / Import Mesh / Import Animation).
        // If the bitmap fails to load (rare - resource may be stripped
        // in a particular build) we silently skip toolbar creation and
        // the rest of the window lays out as before.
        InitCommonControls();
        g_hToolbar = CreateWindowExA(
            0, TOOLBARCLASSNAMEA, "",
            WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                CCS_NODIVIDER | CCS_NORESIZE,
            0, 0, 0, 0, hWnd, (HMENU)IDC_AB_TOOLBAR, hInst, nullptr);

        if (g_hToolbar)
        {
            SendMessageA(g_hToolbar, TB_BUTTONSTRUCTSIZE,
                         (WPARAM)sizeof(TBBUTTON), 0);

            // Each tile in resource 29786 is 16x16 sitting inside a
            // 22x22 button frame.  Sizes MUST be set before TB_ADDBITMAP
            // or comctl32 treats the whole strip as one tile per button
            // (huge, ugly).  Earlier we used a 16x15 source rect, but
            // that clipped the bottom pixel row of any tile that drew
            // into it (e.g. tile [21] Load Entire Package has visible
            // pixels there); other tiles happened to be padded so the
            // clip wasn't noticeable.
            SendMessageA(g_hToolbar, TB_SETBITMAPSIZE,  0,
                         MAKELONG(16, 16));
            SendMessageA(g_hToolbar, TB_SETBUTTONSIZE,  0,
                         MAKELONG(23, 23));

            // Register the EXE's bitmap as this toolbar's image strip.
            // We pass a generous tile count - comctl32 reads the real
            // count from the bitmap width / tile-width.
            TBADDBITMAP tbab = {};
            tbab.hInst = hInst;
            tbab.nID   = RES_AB_TOOLBAR_BITMAP;
            SendMessageA(g_hToolbar, TB_ADDBITMAP, 32, (LPARAM)&tbab);

            // Bitmap tile indices in resource 29786 (0-based):
            //    [0]  - dock-window arrow      (skipped; our AB isn't
            //                                   part of the editor's
            //                                   master browser frame)
            //    [1]  - Open
            //    [2]  - Save
            //    [21] - Load Entire Package
            //    [5]  - Import Animation
            //    [6]  - Import Mesh
            //    [44] - Animation Append (Import Animation w/ Merge)
            // iString = 0 (no caption); tooltips come from the
            // TBN_GETINFOTIPA handler in WM_NOTIFY below.
            TBBUTTON btns[] = {
                {  1, IDMN_FileOpen,              TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
                {  2, IDMN_FileSave,              TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
                { 21, IDMN_AB_LOAD_ENTIRE_PACKAGE,TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
                {  0, 0,                          TBSTATE_ENABLED, BTNS_SEP,    {0},0, 0 },
                {  6, IDMN_FILE_IMPORTMESH,       TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
                {  5, IDMN_FILE_IMPORTANIM,       TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
                { 44, IDMN_FILE_IMPORTANIMMORE,   TBSTATE_ENABLED, BTNS_BUTTON, {0},0, 0 },
            };
            SendMessageA(g_hToolbar, TB_ADDBUTTONS,
                         _countof(btns), (LPARAM)btns);
            SendMessageA(g_hToolbar, TB_AUTOSIZE, 0, 0);
        }

        CreateWindowExA(0, "STATIC", "Package",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_PACKAGE_LABEL, hInst, nullptr);
        g_hPackageCombo = CreateWindowExA(0, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                        CBS_SORT | WS_VSCROLL,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_PACKAGE_COMBO, hInst, nullptr);

        CreateWindowExA(0, "STATIC", "Mesh",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_MESH_LABEL, hInst, nullptr);
        g_hMeshCombo = CreateWindowExA(0, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_MESH_COMBO, hInst, nullptr);

        CreateWindowExA(0, "STATIC", "Animation Set",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_ANIM_LABEL, hInst, nullptr);
        g_hAnimCombo = CreateWindowExA(0, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_ANIM_COMBO, hInst, nullptr);

        CreateWindowExA(0, "STATIC", " Sequences ",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_SEQ_LABEL, hInst, nullptr);
        g_hSeqList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                        LBS_NOTIFY | LBS_HASSTRINGS,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_SEQ_LIST, hInst, nullptr);
        // Right-click on the sequence list opens the UT2004-style context
        // menu (Properties / Rename / Notifications / Groups / Delete /
        // Copy Shortcut).  Subclass so we intercept WM_RBUTTONDOWN before
        // the listbox's default handler eats it.
        SetWindowSubclass(g_hSeqList, SeqListSubclassProc, 1, 0);

        // Property panel: tab control with five pages, mirroring UT2004
        // WBrowserAnimation's PropSheet (Mesh / Animation Set / Sequence /
        // Notify / Prefs).  Each page is a STATIC child that holds the
        // tab's content - a later pass replaces the placeholders with the
        // real property editors.
        g_hTabs = CreateWindowExA(0, WC_TABCONTROLA, "",
                        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
                        0, 0, 0, 0, hWnd, (HMENU)IDC_AB_PROPS_TABS, hInst, nullptr);

        TCITEMA tci = {};
        tci.mask = TCIF_TEXT;
        struct TabSpec { int id; const char* label; };
        const TabSpec tabs[AB_TAB_COUNT] = {
            { IDC_AB_TAB_MESH_PAGE,   "Mesh"          },
            { IDC_AB_TAB_ANIM_PAGE,   "Animation Set" },
            { IDC_AB_TAB_SEQ_PAGE,    "Sequence"      },
            { IDC_AB_TAB_NOTIFY_PAGE, "Notify"        },
            { IDC_AB_TAB_PREFS_PAGE,  "Prefs"         },
        };
        for (int i = 0; i < AB_TAB_COUNT; ++i)
        {
            tci.pszText = const_cast<char*>(tabs[i].label);
            SendMessageA(g_hTabs, TCM_INSERTITEMA, i,
                         reinterpret_cast<LPARAM>(&tci));

            // Each page is a borderless STATIC child of the main window
            // (not the tab control) - Windows tab controls work best when
            // the pages are *siblings* of the tab strip, not children of
            // it, so message routing and font inheritance behave normally.
            // The page itself is just a blank background rect; actual
            // property controls are added separately by BuildXxxTab().
            g_hTabPage[i] = CreateWindowExA(0, "STATIC", "",
                            WS_CHILD | SS_LEFT | SS_NOPREFIX,
                            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)tabs[i].id,
                            hInst, nullptr);
        }
        ShowWindow(g_hTabPage[AB_TAB_MESH], SW_SHOW);  // default active tab

        // Build per-tab property contents.  Page width is filled in at
        // first WM_SIZE; we pass a reasonable default here.  Controls
        // are created at (0,0) and positioned by PositionChildControls.
        const int seedPageW = kInitialWidth - kSeqListWidth - 32;
        BuildSequenceTab(hWnd, seedPageW);
        BuildNotifyTab  (hWnd, seedPageW);
        BuildAnimSetTab (hWnd, seedPageW);
        BuildPrefsTab   (hWnd, seedPageW);
        BuildMeshTab    (hWnd, seedPageW);

        // Hide every tab's controls except the default (Mesh).  The
        // Sequence tab controls were just created; hide them until the
        // user selects that tab.
        for (int i = 0; i < AB_TAB_COUNT; ++i)
        {
            int show = (i == AB_TAB_MESH) ? SW_SHOW : SW_HIDE;
            for (auto& tc : g_tabContent[i])
                ShowWindow(tc.hwnd, show);
        }

        // Use the system font for child controls (combos default to a
        // bitmap font otherwise).
        HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND fontTargets[] = {
            g_hPackageCombo, g_hMeshCombo, g_hAnimCombo, g_hSeqList,
            GetDlgItem(hWnd, IDC_AB_PACKAGE_LABEL),
            GetDlgItem(hWnd, IDC_AB_MESH_LABEL),
            GetDlgItem(hWnd, IDC_AB_ANIM_LABEL),
            GetDlgItem(hWnd, IDC_AB_SEQ_LABEL),
            g_hTabs,
            g_hTabPage[0], g_hTabPage[1], g_hTabPage[2],
            g_hTabPage[3], g_hTabPage[4]
        };
        for (HWND h : fontTargets)
            SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        // 1Hz poll so newly-loaded packages (from a map the
        // user opened in the main editor) appear in our combos without
        // requiring an AB-close/reopen.  See AutoRefreshIfDirty.
        SetTimer(hWnd, IDT_AB_AUTO_REFRESH, 1000, nullptr);

        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_AB_AUTO_REFRESH) AutoRefreshIfDirty();
        return 0;

    case WM_ACTIVATE:
        // Instant refresh when the user switches back to us - covers
        // the typical "loaded a map, alt-tabbed to AB" workflow with
        // no perceptible delay.
        if (LOWORD(wParam) != WA_INACTIVE) AutoRefreshIfDirty();
        return 0;

    case WM_SIZE:
    {
        RECT rc; GetClientRect(hWnd, &rc);
        PositionChildControls(rc.right - rc.left, rc.bottom - rc.top);
        return 0;
    }

    // deferred rebuild of the Notify tab.  Posted from inside
    // a PropertyGrid setter (NewObject_Set) because doing the rebuild
    // synchronously would tear down the row whose inline editor is mid-
    // EndInlineEdit, dangling its reference.
    case WM_AB_REFRESH_NOTIFY_TAB:
        RefreshNotifyTab();
        return 0;

    case WM_AB_REFRESH_MESH_TAB:
        RefreshMeshTab();
        return 0;

    case WM_AB_REFRESH_SEQ_TAB:
        RefreshSequenceTab();
        return 0;

    case WM_COMMAND:
    {
        WORD code = HIWORD(wParam);
        WORD id   = LOWORD(wParam);

        // For editable (CBS_DROPDOWNLIST) combos the edit field isn't synced
        // to the new listbox selection until the dropdown CLOSES.  If
        // we refresh on CBN_SELCHANGE, GetWindowTextA still reads the
        // previous text and the cascade ends up using the stale package.
        // CBN_CLOSEUP fires after the dropdown is hidden and the edit
        // field has been updated, so GetWindowTextA returns the new
        // selection - that's the right hook for dropdown picks.
        // CBN_KILLFOCUS handles the "user typed and tabbed away" path.
        if (code == 0 /* menu */ ||
            code == CBN_CLOSEUP || code == CBN_KILLFOCUS ||
            code == LBN_SELCHANGE)
        {
            OnCommand(hWnd, id);
        }
        // Tab-grid editors (PropertyGrid widgets) own their own
        // commit lifecycle - they call their getter/setter callbacks
        // on focus loss internally, so we don't need EN_KILLFOCUS or
        // BN_CLICKED routing here.
        return 0;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->hwndFrom == g_hTabs && hdr->code == TCN_SELCHANGE)
        {
            int sel = static_cast<int>(SendMessageA(g_hTabs, TCM_GETCURSEL, 0, 0));
            SelectTab(sel);
        }
        // toolbar tooltip text - comctl32 sends TBN_GETINFOTIPA
        // when the user hovers a button.  iItem holds the button's
        // command ID; we just lookup-table our short label string.
        if (hdr && hdr->idFrom == IDC_AB_TOOLBAR &&
            hdr->code == TBN_GETINFOTIPA)
        {
            NMTBGETINFOTIPA* nm = reinterpret_cast<NMTBGETINFOTIPA*>(lParam);
            const char* tip = nullptr;
            switch (nm->iItem)
            {
            case IDMN_FileOpen:               tip = "Open Animated Mesh Package"; break;
            case IDMN_FileSave:               tip = "Save Animation Package";     break;
            case IDMN_AB_LOAD_ENTIRE_PACKAGE: tip = "Load Entire Package";        break;
            case IDMN_FILE_IMPORTMESH:        tip = "Import Mesh";                break;
            case IDMN_FILE_IMPORTANIM:        tip = "Import Animation";           break;
            case IDMN_FILE_IMPORTANIMMORE:    tip = "Append Animation";           break;
            }
            if (tip) strncpy_s(nm->pszText, nm->cchTextMax, tip, _TRUNCATE);
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        // Paint category headers with a light-gray background (matches
        // UT2004's WProperties category bar look).
        HWND hCtl = reinterpret_cast<HWND>(lParam);
        if (IsCategoryHeader(hCtl))
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor  (hdc, RGB(210, 210, 210));
            SetTextColor(hdc, RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(GetCategoryBrush());
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        // The menu is destroyed with the window - drop our cached handle
        // so the next Show reloads a fresh copy from the EXE resource.
        KillTimer(hWnd, IDT_AB_AUTO_REFRESH);
        g_hWnd  = nullptr;
        g_hMenu = nullptr;
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------
//  Public entry points
// ---------------------------------------------------------------------
//  Idempotent class registration.  Calling RegisterClassExA from inside
//  DllMain's InitOnceExecuteOnce is unreliable because USER32 isn't
//  guaranteed to be initialised for the process yet (this is the same
//  reason MSDN says only kernel32 functions are safe to call from
//  DLL_PROCESS_ATTACH).  Defer registration to the first Show() call.
//
//  Returns true on success (class is registered after this call).
static bool EnsureWndClassRegistered()
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSEXA existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExA(hInst, kWndClassName, &existing))
        return true;  // already registered, nothing to do

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWndClassName;
    ATOM a = RegisterClassExA(&wc);
    return a != 0;
}

void AnimationBrowser::Initialize()
{
    // Intentionally minimal: no USER32 calls here because Initialize
    // runs from DllMain's InitOnce.  Window-class registration and
    // common-control init happen lazily on the first Show().
}

void AnimationBrowser::Show(HWND hParent)
{
    if (g_hWnd && IsWindow(g_hWnd))
    {
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
        BringWindowToTop(g_hWnd);
        return;
    }

    // First-use lazy init: window class + common controls.
    static bool s_lazyInitDone = false;
    if (!s_lazyInitDone)
    {
        INITCOMMONCONTROLSEX icc = {};
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_WIN95_CLASSES | ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        s_lazyInitDone = true;
    }

    if (!EnsureWndClassRegistered())
    {
        char err[160];
        _snprintf_s(err, sizeof(err), _TRUNCATE,
                    "RegisterClassExA failed - GetLastError=%lu",
                    GetLastError());
        MessageBoxA(hParent, err, "AnimationBrowser DEBUG",
                    MB_OK);
        return;
    }

    HINSTANCE hInst = GetModuleHandleA(nullptr);

    // Load the original Animation Browser menu bar still embedded in
    // ChaosTheory_Editor.exe.  Every command ID lines up 1:1 with
    // UT2004's IDMN_* identifiers, which is what OnCommand expects.
    if (!g_hMenu)
        g_hMenu = LoadMenuA(hInst, MAKEINTRESOURCEA(ANIM_BROWSER_MENU_RES));

    // dropped WS_EX_TOOLWINDOW.  Tool windows have a
    // shortened title bar that explicitly does NOT draw the system
    // icon - which is why the loaded EXE icon wasn't showing.  The
    // other UnrealEd browsers (Texture / Sound) use a normal window
    // style; matching them gives us the icon + a regular taskbar entry.
    g_hWnd = CreateWindowExA(
        0,
        kWndClassName,
        "Animation Tool",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        kInitialWidth, kInitialHeight,
        hParent,
        g_hMenu,
        hInst,
        nullptr);

    if (!g_hWnd)
    {
        Logger::log("AnimationBrowser: CreateWindowEx failed");
        return;
    }

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);
    RefreshAll();
}
