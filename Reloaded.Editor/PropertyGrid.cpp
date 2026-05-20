#include "pch.h"
#include "PropertyGrid.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <map>
#include <string>
#include <vector>

namespace PropertyGrid
{
namespace
{

constexpr char  kClassName[]    = "ReloadedPropertyGrid";
constexpr int   kRowHeight      = 20;
constexpr int   kCatHeight      = 20;
constexpr int   kIndentPx       = 14;
constexpr int   kGlyphSize      = 9;
constexpr int   kGlyphPadLeft   = 3;
constexpr COLORREF kColCatBg    = RGB(212, 212, 212);
constexpr COLORREF kColRowBg    = RGB(255, 255, 255);
constexpr COLORREF kColAltBg    = RGB(248, 248, 248);
constexpr COLORREF kColLine     = RGB(168, 168, 168);
constexpr COLORREF kColText     = RGB(  0,   0,   0);
constexpr COLORREF kColSelBg    = RGB(  0, 102, 204);
constexpr COLORREF kColSelText  = RGB(255, 255, 255);

enum class RowKind : unsigned char
{
    Category,
    Scalar,
    ArrayHeader,   // [-] ArrayName       (N elements)
    ArrayElement,  //     [N]  value
};

// Phase 4.1: per-row clickable action button (Empty/Add/New/etc.).
struct RowButton
{
    std::string      label;
    void           (*callback)(HWND grid, int rowIdx, void* userdata);
    void*            userdata;
    ButtonVisibility visibility;
};

struct Row
{
    RowKind     kind;
    std::string name;
    std::string value;
    int         depth;        // 0=category, 1=under category, 2=array element
    int         parentIdx;    // -1 for categories; index of owning row otherwise
    bool        expanded;     // categories + array headers
    bool        editable;     // scalar / array-element: true if value is editable
    ValueGetter getter;       // editable: refreshes `value` from underlying data
    ValueSetter setter;       // editable: applies user-entered text
    void*       userdata;     // opaque pointer passed to getter/setter
    int         arrayBindingIdx; // -1 if not part of an array; otherwise index into state's arrayBindings
    int         arrayElemIndex;  // ArrayElement: index within the array
    std::vector<std::string> enumOptions; // non-empty => render dropdown editor
    std::vector<RowButton>   buttons;     // Phase 4.1 inline action buttons
};

// Phase 4.1: action-button layout constants.  Buttons are drawn right-
// aligned in the value cell; multiple buttons stack with kBtnGap between.
constexpr int kBtnW   = 50;
constexpr int kBtnH   = 14;     // kRowHeight (20) - 6px vertical padding
constexpr int kBtnGap = 2;
constexpr int kBtnRightPad = 4;

// Computes the rect of button `btnIdx` (0 = leftmost) on a row whose top
// is at `yTop` and whose grid client width is `clientW`.  Buttons are
// right-aligned together as a group, with leftmost first.
static void GetButtonRect(int btnIdx, int btnCount, int clientW, int yTop,
                          RECT* out)
{
    int totalW   = btnCount * kBtnW + (btnCount - 1) * kBtnGap;
    int firstL   = clientW - kBtnRightPad - totalW;
    int left     = firstL + btnIdx * (kBtnW + kBtnGap);
    int btnTop   = yTop + (kRowHeight - kBtnH) / 2;
    *out         = { left, btnTop, left + kBtnW, btnTop + kBtnH };
}

// Edit-control subclass key.  We stash a pointer back to the grid's
// HWND in the EDIT's GWLP_USERDATA so the subclass procedure can route
// keystrokes (Enter / Esc) back to the grid.
struct InlineEditCtx
{
    HWND   gridHwnd;
    int    rowIdx;
    WNDPROC origProc;
    bool   commitOnDestroy;
};

struct ArrayBinding
{
    ArrayOps ops;
    void*    userdata;
};

struct State
{
    std::vector<Row>          rows;
    std::vector<ArrayBinding> arrayBindings;
    // Keyed by category/array-header name.  Survives Clear() so when
    // the tab is rebuilt (e.g. user picks a different mesh) the user's
    // expanded/collapsed choices are preserved - same UX as UT2004's
    // WObjectProperties.
    std::map<std::string, bool> expandedMemory;
    int  splitterX   = 140;     // pixels from left edge to the name/value divider
    int  scrollY     = 0;       // pixel scroll offset
    int  selIdx      = -1;      // selected row in row[] (NOT visible-row index)
    int  lastCatIdx  = -1;      // last AddCategory return; AddRow parents to it
    int  updateDepth = 0;       // BeginUpdate/EndUpdate nesting
    bool draggingSplitter = false;
    int  splitterDragOffset = 0;
    HWND inlineEdit  = nullptr; // active inline editor, or nullptr
    int  editingRow  = -1;
    InlineEditCtx editCtx{};
    // Phase 4.1: press-and-release tracking for action buttons.  The
    // press arms the button (drawn pushed-in); release commits the click
    // only if the cursor is still over the same button (matches the
    // standard Win32 button behavior UT2004 uses).  pressedBtnHover is
    // the visual flag - paint shows the depressed look only while the
    // cursor is over the armed button.
    int  pressedBtnRow   = -1;
    int  pressedBtnIdx   = -1;
    bool pressedBtnHover = false;
    HFONT hFontReg   = nullptr;
    HFONT hFontBold  = nullptr;
    HBRUSH hBrushCat = nullptr;
    HBRUSH hBrushRow = nullptr;
    HBRUSH hBrushAlt = nullptr;
    HBRUSH hBrushSel = nullptr;
    HPEN   hPenLine  = nullptr;
};

static State* GetState(HWND hWnd)
{
    return reinterpret_cast<State*>(GetWindowLongPtrA(hWnd, GWLP_USERDATA));
}

// Forward decl - IsDescendantOf is defined further down with the row
// helpers but the button-visibility check needs it here.
static bool IsDescendantOf(State* s, int idx, int ancestor);

// Phase 4.1: decide whether a particular button should currently render.
// Visibility is keyed off the row's relationship to the current selection.
static bool IsButtonVisible(const RowButton& b, int thisRowIdx, State* s)
{
    switch (b.visibility)
    {
    case BTN_VIS_ALWAYS: return true;
    case BTN_VIS_SELECTED:
        return thisRowIdx == s->selIdx;
    case BTN_VIS_SELECTED_OR_DESCENDANT:
        if (thisRowIdx == s->selIdx) return true;
        if (s->selIdx < 0) return false;
        return IsDescendantOf(s, s->selIdx, thisRowIdx);
    }
    return false;
}

// Number of buttons that are visible right now on `rowIdx`.  Used by
// paint, hit-test, and GetValueCellRect to lay out the row correctly.
static int VisibleButtonCount(const Row& r, int rowIdx, State* s)
{
    int n = 0;
    for (const auto& b : r.buttons)
        if (IsButtonVisible(b, rowIdx, s)) ++n;
    return n;
}

// Returns the ACTUAL r.buttons index of the button at (x, y) on row
// `rowIdx` (which sits at yTop in client coords).  -1 if no visible
// button is hit.  Skips invisible buttons (per ButtonVisibility) so
// hit-test geometry matches the painted layout exactly.
static int HitTestRowButton(const Row& r, int rowIdx, State* s,
                            int rowYTop, int clientW, int x, int y)
{
    int visBtnCount = VisibleButtonCount(r, rowIdx, s);
    if (visBtnCount <= 0) return -1;

    int visBi = 0;
    for (int bi = 0; bi < (int)r.buttons.size(); ++bi)
    {
        if (!IsButtonVisible(r.buttons[bi], rowIdx, s)) continue;
        RECT brc;
        GetButtonRect(visBi, visBtnCount, clientW, rowYTop, &brc);
        if (x >= brc.left && x < brc.right &&
            y >= brc.top  && y < brc.bottom)
            return bi;
        ++visBi;
    }
    return -1;
}

// Layout helper: walks rows[] and computes the visible row sequence
// (skipping rows whose parent category is collapsed).  Returns the
// per-visible-row info: (rowIdx, yTop) for each entry.
struct LayoutEntry { int rowIdx; int yTop; int height; };

// A row is visible iff every ancestor on its parent chain is expanded.
// (Categories themselves are always visible.)
static bool IsRowVisible(State* s, int idx)
{
    int p = s->rows[idx].parentIdx;
    while (p >= 0)
    {
        if (!s->rows[p].expanded) return false;
        p = s->rows[p].parentIdx;
    }
    return true;
}

// Returns true if any row has parentIdx == rowIdx (used to decide whether
// to draw the +/- glyph on an ArrayElement).  Linear scan; cheap enough
// for the ~64-row grids we ship.
static bool HasChildren(State* s, int rowIdx)
{
    for (size_t i = rowIdx + 1; i < s->rows.size(); ++i)
    {
        if (s->rows[i].parentIdx == rowIdx) return true;
        // Stop at the next sibling - rows are ordered depth-first so any
        // child sits contiguously after its parent until the next non-
        // descendant row.
        if (s->rows[i].parentIdx < rowIdx && s->rows[i].parentIdx >= 0
            && s->rows[i].parentIdx != s->rows[rowIdx].parentIdx)
            break;
    }
    return false;
}

// Walk up the parent chain from `idx` and return true if it reaches
// `ancestor`.  Linear in tree depth.
static bool IsDescendantOf(State* s, int idx, int ancestor)
{
    int cur = s->rows[idx].parentIdx;
    int guard = 0;
    while (cur >= 0 && guard < 64)
    {
        if (cur == ancestor) return true;
        cur = s->rows[cur].parentIdx;
        ++guard;
    }
    return false;
}

// Returns the row index just past the last descendant of `parentRowIdx`.
// In other words: where a new child of `parentRowIdx` should be inserted
// so it sits at the end of `parentRowIdx`'s existing subtree but BEFORE
// the parent's next sibling.  When `parentRowIdx == -1` (no parent),
// returns rows.size() (append).
static int EndOfSubtree(State* s, int parentRowIdx)
{
    if (parentRowIdx < 0) return (int)s->rows.size();
    int end = parentRowIdx + 1;
    while (end < (int)s->rows.size() && IsDescendantOf(s, end, parentRowIdx))
        ++end;
    return end;
}

// Insert a finished Row into rows[] at the right position so it lands at
// the end of its parent's subtree.  Adjusts the parentIdx field of every
// row that gets shifted by the insertion.  Used by AddRowAt / friends.
static int InsertRowInSubtree(State* s, int parentRowIdx, Row&& r)
{
    int insertAt = EndOfSubtree(s, parentRowIdx);

    // Shift parentIdx of every row whose parent index is >= insertAt; the
    // insertion bumps each such row down by one slot.
    for (auto& rr : s->rows)
        if (rr.parentIdx >= insertAt) rr.parentIdx++;

    s->rows.insert(s->rows.begin() + insertAt, std::move(r));
    return insertAt;
}

static void ComputeLayout(State* s, std::vector<LayoutEntry>& out)
{
    out.clear();
    out.reserve(s->rows.size());
    int y = 0;
    for (size_t i = 0; i < s->rows.size(); ++i)
    {
        if (!IsRowVisible(s, static_cast<int>(i))) continue;
        const Row& r = s->rows[i];
        int h = (r.kind == RowKind::Category) ? kCatHeight : kRowHeight;
        out.push_back({ static_cast<int>(i), y, h });
        y += h;
    }
}

static int TotalContentHeight(State* s)
{
    int y = 0;
    for (size_t i = 0; i < s->rows.size(); ++i)
    {
        if (!IsRowVisible(s, static_cast<int>(i))) continue;
        const Row& r = s->rows[i];
        y += (r.kind == RowKind::Category) ? kCatHeight : kRowHeight;
    }
    return y;
}

static void UpdateScrollBar(HWND hWnd, State* s)
{
    RECT rc; GetClientRect(hWnd, &rc);
    int clientH = rc.bottom - rc.top;
    int content = TotalContentHeight(s);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = (content > 0) ? content - 1 : 0;
    si.nPage  = clientH > 0 ? clientH : 1;
    si.nPos   = s->scrollY;
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

    // Clamp scroll position if content shrank.
    int maxPos = (content > clientH) ? content - clientH : 0;
    if (s->scrollY > maxPos) s->scrollY = maxPos;
}

static void EnsureGdiObjects(State* s)
{
    if (!s->hFontReg)
    {
        LOGFONTA lf = {};
        lf.lfHeight  = -11;
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = ANSI_CHARSET;
        strncpy_s(lf.lfFaceName, "MS Shell Dlg", _TRUNCATE);
        s->hFontReg = CreateFontIndirectA(&lf);
        lf.lfWeight = FW_BOLD;
        s->hFontBold = CreateFontIndirectA(&lf);
    }
    if (!s->hBrushCat) s->hBrushCat = CreateSolidBrush(kColCatBg);
    if (!s->hBrushRow) s->hBrushRow = CreateSolidBrush(kColRowBg);
    if (!s->hBrushAlt) s->hBrushAlt = CreateSolidBrush(kColAltBg);
    if (!s->hBrushSel) s->hBrushSel = CreateSolidBrush(kColSelBg);
    if (!s->hPenLine)  s->hPenLine  = CreatePen(PS_SOLID, 1, kColLine);
}

static void FreeGdiObjects(State* s)
{
    if (s->hFontReg)   { DeleteObject(s->hFontReg);   s->hFontReg   = nullptr; }
    if (s->hFontBold)  { DeleteObject(s->hFontBold);  s->hFontBold  = nullptr; }
    if (s->hBrushCat)  { DeleteObject(s->hBrushCat);  s->hBrushCat  = nullptr; }
    if (s->hBrushRow)  { DeleteObject(s->hBrushRow);  s->hBrushRow  = nullptr; }
    if (s->hBrushAlt)  { DeleteObject(s->hBrushAlt);  s->hBrushAlt  = nullptr; }
    if (s->hBrushSel)  { DeleteObject(s->hBrushSel);  s->hBrushSel  = nullptr; }
    if (s->hPenLine)   { DeleteObject(s->hPenLine);   s->hPenLine   = nullptr; }
}

// Draw a small box with `-` (expanded) or `+` (collapsed) glyph - the
// same visual indicator UT2004's tree uses for collapse state.
static void DrawExpander(HDC hdc, int x, int y, bool expanded, HPEN penLine)
{
    HPEN oldPen = (HPEN)SelectObject(hdc, penLine);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(WHITE_BRUSH));
    Rectangle(hdc, x, y, x + kGlyphSize, y + kGlyphSize);
    // Horizontal line of '+' / '-'
    MoveToEx(hdc, x + 2, y + kGlyphSize / 2, nullptr);
    LineTo  (hdc, x + kGlyphSize - 2, y + kGlyphSize / 2);
    // Vertical line of '+' only
    if (!expanded)
    {
        MoveToEx(hdc, x + kGlyphSize / 2, y + 2, nullptr);
        LineTo  (hdc, x + kGlyphSize / 2, y + kGlyphSize - 2);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
}

static void OnPaint(HWND hWnd, State* s)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    RECT rc; GetClientRect(hWnd, &rc);
    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    EnsureGdiObjects(s);

    // Off-screen DC for flicker-free draw.
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientW, clientH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background.
    RECT bg = { 0, 0, clientW, clientH };
    FillRect(memDC, &bg, s->hBrushRow);

    SetBkMode(memDC, TRANSPARENT);

    std::vector<LayoutEntry> layout;
    ComputeLayout(s, layout);

    for (const auto& le : layout)
    {
        int yTop = le.yTop - s->scrollY;
        int yBot = yTop + le.height;
        if (yBot <= 0 || yTop >= clientH) continue;

        const Row& r = s->rows[le.rowIdx];
        RECT rowRc = { 0, yTop, clientW, yBot };

        bool isSelected = (le.rowIdx == s->selIdx);

        if (r.kind == RowKind::Category)
        {
            // Gray bar for the whole row.
            FillRect(memDC, &rowRc, s->hBrushCat);
            // Hairline below.
            HPEN oldPen = (HPEN)SelectObject(memDC, s->hPenLine);
            MoveToEx(memDC, 0,       yBot - 1, nullptr);
            LineTo  (memDC, clientW, yBot - 1);
            SelectObject(memDC, oldPen);

            int gx = kGlyphPadLeft;
            int gy = yTop + (kCatHeight - kGlyphSize) / 2;
            DrawExpander(memDC, gx, gy, r.expanded, s->hPenLine);

            SelectObject(memDC, s->hFontBold);
            SetTextColor(memDC, kColText);
            RECT textRc = { gx + kGlyphSize + 6, yTop, clientW - 4, yBot };
            DrawTextA(memDC, r.name.c_str(), -1, &textRc,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        else
        {
            // Scalar / ArrayHeader / ArrayElement row.  Alternating tint
            // matches UT2004's "every other row is slightly gray" look.
            HBRUSH bgBrush = (le.rowIdx & 1) ? s->hBrushAlt : s->hBrushRow;
            if (isSelected) bgBrush = s->hBrushSel;
            FillRect(memDC, &rowRc, bgBrush);

            // Splitter line down the middle.
            HPEN oldPen = (HPEN)SelectObject(memDC, s->hPenLine);
            MoveToEx(memDC, s->splitterX, yTop, nullptr);
            LineTo  (memDC, s->splitterX, yBot);
            SelectObject(memDC, oldPen);

            int indentPx = (r.depth) * kIndentPx + kGlyphPadLeft;

            // Array headers carry a +/- glyph in the indent area so the
            // user can collapse the element rows underneath - same look
            // UT2004 uses for the `(N)` arrays on the right pane.
            // ArrayElement and Scalar rows that have children (Phase
            // 3.11/3.12) get the same glyph treatment for inline
            // expand/collapse (used by Notify[i] -> Notify -> Sound/
            // Volume/Radius/BoneName drill-down).
            bool drawGlyph =
                (r.kind == RowKind::ArrayHeader) ||
                ((r.kind == RowKind::ArrayElement ||
                  r.kind == RowKind::Scalar) && HasChildren(s, le.rowIdx));
            if (drawGlyph)
            {
                int gx = indentPx - kIndentPx + 1;
                int gy = yTop + (kRowHeight - kGlyphSize) / 2;
                DrawExpander(memDC, gx, gy, r.expanded, s->hPenLine);
            }

            // Name column
            SelectObject(memDC, s->hFontReg);
            SetTextColor(memDC, isSelected ? kColSelText : kColText);
            RECT nameRc = { indentPx, yTop, s->splitterX - 4, yBot };
            DrawTextA(memDC, r.name.c_str(), -1, &nameRc,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Phase 4.1: per-row action buttons.  Only buttons currently
            // visible (per their ButtonVisibility flag) take up space and
            // are drawn.  Compute leftmost visible-button edge so the
            // value text and (optional) dropdown arrow don't underdraw.
            int visBtnCount = VisibleButtonCount(r, le.rowIdx, s);
            int valRight = clientW - 4;
            if (visBtnCount > 0)
            {
                int totalBtnW = visBtnCount * kBtnW + (visBtnCount - 1) * kBtnGap;
                valRight = clientW - kBtnRightPad - totalBtnW - 4;
            }

            // Phase 4.1: dropdown arrow on selected enum rows.  Visual
            // cue that the value cell opens a combo on click (mirrors
            // UT2004's WObjectProperties for enum/object pickers).
            bool showDropdownArrow =
                (!r.enumOptions.empty()) && (le.rowIdx == s->selIdx);
            const int kArrowW = 14;
            if (showDropdownArrow) valRight -= kArrowW + 2;

            // Value column (clipped to the left of arrow + buttons).
            RECT valRc = { s->splitterX + 4, yTop, valRight, yBot };
            DrawTextA(memDC, r.value.c_str(), -1, &valRc,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Draw the dropdown arrow (just to the right of value text).
            if (showDropdownArrow)
            {
                RECT arc = {
                    valRight + 2,
                    yTop + (kRowHeight - kBtnH) / 2,
                    valRight + 2 + kArrowW,
                    yTop + (kRowHeight - kBtnH) / 2 + kBtnH
                };
                DrawFrameControl(memDC, &arc, DFC_SCROLL,
                                 DFCS_SCROLLDOWN | DFCS_FLAT);
            }

            // Draw the buttons (classic 3D-bordered push buttons).  Show
            // the currently-armed button (mouse pressed but not yet
            // released) with the pushed-in look.  Iterate actual-button
            // indices but lay out using the visible-position counter.
            int visBi = 0;
            for (int bi = 0; bi < (int)r.buttons.size(); ++bi)
            {
                if (!IsButtonVisible(r.buttons[bi], le.rowIdx, s)) continue;

                RECT brc;
                GetButtonRect(visBi, visBtnCount, clientW, yTop, &brc);
                UINT btnState = DFCS_BUTTONPUSH;
                if (le.rowIdx == s->pressedBtnRow &&
                    bi          == s->pressedBtnIdx &&
                    s->pressedBtnHover)
                    btnState |= DFCS_PUSHED;
                DrawFrameControl(memDC, &brc, DFC_BUTTON, btnState);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, kColText);
                if (btnState & DFCS_PUSHED)
                { brc.left += 1; brc.top += 1; brc.right += 1; brc.bottom += 1; }
                DrawTextA(memDC, r.buttons[bi].label.c_str(), -1, &brc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                ++visBi;
            }
        }
    }

    // Blit to the real DC.
    BitBlt(hdc, 0, 0, clientW, clientH, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hWnd, &ps);
}

// Find which visible row contains client-area Y `clientY`.
static int HitTestY(State* s, int clientY, int* outRowYTop)
{
    int adj = clientY + s->scrollY;
    int y = 0;
    for (size_t i = 0; i < s->rows.size(); ++i)
    {
        if (!IsRowVisible(s, static_cast<int>(i))) continue;
        const Row& r = s->rows[i];
        int h = (r.kind == RowKind::Category) ? kCatHeight : kRowHeight;
        if (adj >= y && adj < y + h)
        {
            if (outRowYTop) *outRowYTop = y - s->scrollY;
            return static_cast<int>(i);
        }
        y += h;
    }
    return -1;
}

// =====================================================================
//  Splitter (drag-resizable column divider) + inline editor lifecycle
// =====================================================================
constexpr int kSplitterGrabHalfWidth = 3;
constexpr int kSplitterMinName       = 60;
constexpr int kSplitterMinValue      = 60;

static bool HitSplitter(State* s, int x, int clientW)
{
    return clientW > kSplitterMinName + kSplitterMinValue &&
           x >= s->splitterX - kSplitterGrabHalfWidth &&
           x <= s->splitterX + kSplitterGrabHalfWidth;
}

// Compute (xLeft, yTop, w, h) for the value cell of a given row in
// client coords.  Returns false if the row isn't a visible scalar.
// Phase 4.1: shrinks the cell to leave room for action buttons so the
// inline editor (EDIT/COMBO overlay) doesn't draw on top of them.
static bool GetValueCellRect(HWND hWnd, State* s, int rowIdx, RECT* out)
{
    std::vector<LayoutEntry> layout;
    ComputeLayout(s, layout);
    for (const auto& le : layout)
    {
        if (le.rowIdx == rowIdx)
        {
            RECT cr; GetClientRect(hWnd, &cr);
            int yTop = le.yTop - s->scrollY;
            int right = cr.right - 2;
            int btnCount = VisibleButtonCount(s->rows[rowIdx], rowIdx, s);
            if (btnCount > 0)
            {
                int totalW = btnCount * kBtnW + (btnCount - 1) * kBtnGap;
                right = cr.right - kBtnRightPad - totalW - 4;
            }
            out->left   = s->splitterX + 2;
            out->top    = yTop + 1;
            out->right  = right;
            out->bottom = yTop + le.height - 1;
            return true;
        }
    }
    return false;
}

// Forward decls so the subclass procedure can call EndInlineEdit.
static void EndInlineEdit(HWND hWnd, State* s, bool commit);

static LRESULT CALLBACK InlineEditSubclassProc(HWND hEdit, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    InlineEditCtx* ctx = reinterpret_cast<InlineEditCtx*>(
        GetWindowLongPtrA(hEdit, GWLP_USERDATA));
    if (!ctx) return DefWindowProcA(hEdit, msg, wParam, lParam);

    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN)
        {
            State* s = GetState(ctx->gridHwnd);
            if (s) EndInlineEdit(ctx->gridHwnd, s, /*commit=*/true);
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            State* s = GetState(ctx->gridHwnd);
            if (s)
            {
                ctx->commitOnDestroy = false;
                EndInlineEdit(ctx->gridHwnd, s, /*commit=*/false);
            }
            return 0;
        }
        if (wParam == VK_TAB)
        {
            State* s = GetState(ctx->gridHwnd);
            if (s) EndInlineEdit(ctx->gridHwnd, s, /*commit=*/true);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_HASSETSEL |
               DLGC_WANTARROWS;
    }

    return CallWindowProcA(ctx->origProc, hEdit, msg, wParam, lParam);
}

static void EndInlineEdit(HWND hWnd, State* s, bool commit)
{
    if (!s->inlineEdit) return;
    HWND ed = s->inlineEdit;

    if (commit && s->editingRow >= 0 && s->editingRow < (int)s->rows.size())
    {
        Row& r = s->rows[s->editingRow];
        char buf[256] = "";
        GetWindowTextA(ed, buf, sizeof(buf));

        if (r.kind == RowKind::ArrayElement && r.arrayBindingIdx >= 0)
        {
            ArrayBinding& ab = s->arrayBindings[r.arrayBindingIdx];
            if (ab.ops.set) ab.ops.set(r.arrayElemIndex, buf, ab.userdata);
            if (ab.ops.get)
            {
                char rb[256] = "";
                ab.ops.get(r.arrayElemIndex, rb, sizeof(rb), ab.userdata);
                r.value = rb;
            }
        }
        else if (r.editable && r.setter)
        {
            r.setter(buf, r.userdata);
            if (r.getter)
            {
                char rb[256] = "";
                r.getter(rb, sizeof(rb), r.userdata);
                r.value = rb;
            }
        }
    }

    s->inlineEdit = nullptr;
    s->editingRow = -1;
    s->editCtx.gridHwnd = nullptr;

    DestroyWindow(ed);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void BeginInlineEdit(HWND hWnd, State* s, int rowIdx)
{
    if (rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;
    Row& r = s->rows[rowIdx];
    bool isArrayElem = (r.kind == RowKind::ArrayElement &&
                        r.arrayBindingIdx >= 0);
    bool isEditableScalar = (r.kind == RowKind::Scalar && r.editable);
    if (!isArrayElem && !isEditableScalar) return;
    if (!r.editable) return;

    if (s->inlineEdit && s->editingRow != rowIdx)
        EndInlineEdit(hWnd, s, /*commit=*/true);
    if (s->inlineEdit) return;

    RECT cell;
    if (!GetValueCellRect(hWnd, s, rowIdx, &cell)) return;

    // Refresh value from the appropriate accessor so the editor starts
    // with the canonical text (trailing zeros for floats etc.).
    if (isArrayElem)
    {
        ArrayBinding& ab = s->arrayBindings[r.arrayBindingIdx];
        if (ab.ops.get)
        {
            char rb[256] = "";
            ab.ops.get(r.arrayElemIndex, rb, sizeof(rb), ab.userdata);
            r.value = rb;
        }
    }
    else if (r.getter)
    {
        char rb[256] = "";
        r.getter(rb, sizeof(rb), r.userdata);
        r.value = rb;
    }

    HINSTANCE hInst = GetModuleHandleA(nullptr);
    HWND ed;

    if (!r.enumOptions.empty())
    {
        // Enum-style editor: CBS_DROPDOWNLIST so the user can only pick
        // one of the allowed strings.  UnrealEd 2's WObjectProperties
        // caps the dropdown at 27 visible rows (gives a scrollbar for
        // larger sets, like the EffectClass roster).  Anything bigger
        // tries to expand to fill the whole monitor.
        constexpr int kMaxDropdownRows = 27;
        constexpr int kRowPx           = 18;
        int cellH = cell.bottom - cell.top;
        int visRows = (int)r.enumOptions.size();
        if (visRows > kMaxDropdownRows) visRows = kMaxDropdownRows;
        int dropdownPx = visRows * kRowPx + 6;
        ed = CreateWindowExA(0, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                WS_VSCROLL,
            cell.left, cell.top,
            cell.right - cell.left,
            cellH + dropdownPx,
            hWnd, (HMENU)(INT_PTR)0xE001, hInst, nullptr);
        SendMessageA(ed, WM_SETFONT,
                     reinterpret_cast<WPARAM>(s->hFontReg), TRUE);

        // Seed with options + select the current value.
        for (const auto& opt : r.enumOptions)
            SendMessageA(ed, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(opt.c_str()));
        LRESULT idx = SendMessageA(ed, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                   reinterpret_cast<LPARAM>(r.value.c_str()));
        if (idx == CB_ERR) idx = 0;
        SendMessageA(ed, CB_SETCURSEL, idx, 0);

        // Auto-pop the dropdown so the user sees the options immediately
        // (matches UnrealEd 2 behavior where the dropdown shows on click).
        SendMessageA(ed, CB_SHOWDROPDOWN, TRUE, 0);

        // Scroll the listbox so the current selection is the topmost
        // visible row.  Without this, picking from a long list (e.g.
        // EffectClass roster) opens at the alphabetical start with the
        // selection somewhere far below the visible window.
        SendMessageA(ed, CB_SETTOPINDEX, idx, 0);
    }
    else
    {
        ed = CreateWindowExA(0, "EDIT", r.value.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
            cell.left, cell.top, cell.right - cell.left, cell.bottom - cell.top,
            hWnd, (HMENU)(INT_PTR)0xE001, hInst, nullptr);
        SendMessageA(ed, WM_SETFONT,
                     reinterpret_cast<WPARAM>(s->hFontReg), TRUE);
    }

    s->editCtx.gridHwnd        = hWnd;
    s->editCtx.rowIdx          = rowIdx;
    s->editCtx.commitOnDestroy = true;
    s->editCtx.origProc        = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrA(ed, GWLP_WNDPROC));
    SetWindowLongPtrA(ed, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(&s->editCtx));
    SetWindowLongPtrA(ed, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(&InlineEditSubclassProc));

    s->inlineEdit = ed;
    s->editingRow = rowIdx;

    SetFocus(ed);
    if (r.enumOptions.empty())
        SendMessageA(ed, EM_SETSEL, 0, -1);   // select all in EDIT
    InvalidateRect(hWnd, nullptr, FALSE);
}

// =====================================================================
//  Array rows - rebuild on mutation
// =====================================================================

static Row MakeArrayElement(State* s, int arrayHeaderIdx, int elemIdx)
{
    Row e;
    e.kind            = RowKind::ArrayElement;
    char nameBuf[16];
    _snprintf_s(nameBuf, sizeof(nameBuf), _TRUNCATE, "[%d]", elemIdx);
    e.name            = nameBuf;
    e.depth           = 2;
    e.parentIdx       = arrayHeaderIdx;

    // Phase 4.1: restore per-element expanded state across refreshes,
    // keyed by "<arrayHeaderName>[<elemIdx>]".  This is what keeps the
    // user's expanded notify elements expanded after a sibling is
    // added/created via the New button.  Defaults to collapsed when no
    // prior state exists (matches UT2004's freshly-loaded behavior).
    bool defExpanded = false;
    {
        char key[160];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s[%d]",
                    s->rows[arrayHeaderIdx].name.c_str(), elemIdx);
        auto it = s->expandedMemory.find(key);
        if (it != s->expandedMemory.end()) defExpanded = it->second;
    }
    e.expanded        = defExpanded;

    int abi           = s->rows[arrayHeaderIdx].arrayBindingIdx;
    e.arrayBindingIdx = abi;
    e.arrayElemIndex  = elemIdx;
    e.editable        = (abi >= 0) ? (s->arrayBindings[abi].ops.set != nullptr) : false;
    e.getter          = nullptr;
    e.setter          = nullptr;
    e.userdata        = nullptr;

    char vbuf[256] = "";
    if (abi >= 0 && s->arrayBindings[abi].ops.get)
        s->arrayBindings[abi].ops.get(elemIdx, vbuf, sizeof(vbuf),
                                      s->arrayBindings[abi].userdata);
    e.value = vbuf;
    return e;
}

// Rebuilds the element rows underneath the given array header.  Used
// initially in AddArray, and again after Insert/Delete/Empty.  Handles
// the parentIdx fix-up for rows that come after the array's elements.
//
// Phase 3.12 note: an array can have grand-descendants (e.g. Notify[N]'s
// own Notify/NotifyFrame/property rows).  The old erase range only
// covered direct children (parentIdx == arrayHeaderIdx) and would leave
// orphaned grand-descendants behind with stale parentIdx pointing at
// freed slots.  We now erase the ENTIRE subtree under the header and
// let populateChildren rebuild it from scratch.
static void RebuildArrayElements(State* s, int arrayHeaderIdx)
{
    if (arrayHeaderIdx < 0 || arrayHeaderIdx >= (int)s->rows.size()) return;
    int abi = s->rows[arrayHeaderIdx].arrayBindingIdx;
    if (abi < 0 || abi >= (int)s->arrayBindings.size()) return;
    ArrayBinding& ab = s->arrayBindings[abi];

    int oldFirst = arrayHeaderIdx + 1;
    int oldLast  = oldFirst - 1;
    // Walk while the next row is anywhere in the header's subtree (direct
    // child, grandchild, etc.).  Stops at the first row outside the
    // subtree (next array, next category, etc.).
    while (oldLast + 1 < (int)s->rows.size() &&
           IsDescendantOf(s, oldLast + 1, arrayHeaderIdx))
        oldLast++;
    int oldN = (oldLast - oldFirst) + 1;

    // Cancel any inline edit pointing at a row that's about to disappear.
    if (s->inlineEdit && s->editingRow >= oldFirst && s->editingRow <= oldLast)
    {
        s->editCtx.commitOnDestroy = false;
        DestroyWindow(s->inlineEdit);
        s->inlineEdit = nullptr;
        s->editingRow = -1;
    }

    if (oldN > 0)
        s->rows.erase(s->rows.begin() + oldFirst,
                      s->rows.begin() + oldLast + 1);

    int newN = ab.ops.count ? ab.ops.count(ab.userdata) : 0;
    for (int i = 0; i < newN; ++i)
    {
        Row e = MakeArrayElement(s, arrayHeaderIdx, i);
        s->rows.insert(s->rows.begin() + oldFirst + i, std::move(e));
    }

    // Fix up parentIdx of every row whose original parent was past the
    // deletion range.  Newly inserted elements already point at the
    // (unchanged) header; categories and unrelated rows aren't touched.
    int delta = newN - oldN;
    if (delta != 0)
    {
        for (auto& rr : s->rows)
            if (rr.parentIdx > oldLast) rr.parentIdx += delta;
    }
}

// HWND-aware variant: rebuilds the elements then, if the ArrayOps has a
// populateChildren callback (Phase 3.11), invokes it once per element so
// the consumer can add inline child rows under each.  Used by AddArray
// and by the context-menu Insert/Delete/Empty paths so children survive
// array mutations.
static void RebuildArrayElementsWithChildren(HWND grid, State* s, int arrayHeaderIdx)
{
    RebuildArrayElements(s, arrayHeaderIdx);
    if (arrayHeaderIdx < 0 || arrayHeaderIdx >= (int)s->rows.size()) return;
    int abi = s->rows[arrayHeaderIdx].arrayBindingIdx;
    if (abi < 0 || abi >= (int)s->arrayBindings.size()) return;
    ArrayBinding& ab = s->arrayBindings[abi];
    if (!ab.ops.populateChildren) return;

    // Walk forward from arrayHeaderIdx looking for direct children.  For
    // each, invoke populateChildren which may append to s->rows (shifting
    // later sibling indices).  Re-locate by element index after each call.
    int n = ab.ops.count ? ab.ops.count(ab.userdata) : 0;
    for (int i = 0; i < n; ++i)
    {
        int elemRow = -1;
        for (size_t k = arrayHeaderIdx + 1; k < s->rows.size(); ++k)
        {
            if (s->rows[k].parentIdx == arrayHeaderIdx &&
                s->rows[k].kind == RowKind::ArrayElement &&
                s->rows[k].arrayElemIndex == i)
            { elemRow = (int)k; break; }
        }
        if (elemRow < 0) continue;
        ab.ops.populateChildren(grid, elemRow, i, ab.userdata);
    }
}

// Refreshes only the cached value strings for element rows of an
// array; does NOT re-add or remove rows.  Cheaper than RebuildArray
// when the size hasn't changed.
static void RefreshArrayValuesOnly(State* s, int arrayHeaderIdx)
{
    int abi = s->rows[arrayHeaderIdx].arrayBindingIdx;
    if (abi < 0) return;
    ArrayBinding& ab = s->arrayBindings[abi];
    for (size_t i = arrayHeaderIdx + 1; i < s->rows.size(); ++i)
    {
        if (s->rows[i].parentIdx != arrayHeaderIdx) break;
        char vbuf[256] = "";
        if (ab.ops.get)
            ab.ops.get(s->rows[i].arrayElemIndex, vbuf, sizeof(vbuf),
                       ab.userdata);
        s->rows[i].value = vbuf;
    }
}

// =====================================================================
//  Context menu (right-click on array header / element)
// =====================================================================
constexpr UINT kMenuEmptyArray  = 0xE100;
constexpr UINT kMenuAddItem     = 0xE101;
constexpr UINT kMenuInsertAbove = 0xE102;
constexpr UINT kMenuDelete      = 0xE103;

static void ShowArrayContextMenu(HWND hWnd, State* s, int rowIdx, int screenX, int screenY)
{
    if (rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;

    // Snapshot all values out of the Row reference BEFORE the menu opens.
    // TrackPopupMenu enters a modal loop that can paint, scroll, etc.;
    // we don't want any code path that resizes s->rows (now or in
    // future) to dangle our reference and feed wrong indices to the
    // array ops.
    const Row* pr = &s->rows[rowIdx];
    RowKind  rowKind = pr->kind;
    int      rowParentIdx = pr->parentIdx;
    int      rowElemIdx   = pr->arrayElemIndex;
    int      abi          = pr->arrayBindingIdx;

    if (rowKind != RowKind::ArrayHeader && rowKind != RowKind::ArrayElement) return;
    if (abi < 0 || abi >= (int)s->arrayBindings.size()) return;

    ArrayBinding ab = s->arrayBindings[abi];  // value copy of ops + userdata

    HMENU menu = CreatePopupMenu();
    if (rowKind == RowKind::ArrayHeader)
    {
        if (ab.ops.insert) AppendMenuA(menu, MF_STRING, kMenuAddItem, "Add Item");
        if (ab.ops.empty)  AppendMenuA(menu, MF_STRING, kMenuEmptyArray, "Empty Array");
    }
    else
    {
        if (ab.ops.insert) AppendMenuA(menu, MF_STRING, kMenuInsertAbove, "Insert Above");
        if (ab.ops.del)    AppendMenuA(menu, MF_STRING, kMenuDelete, "Delete");
    }

    if (GetMenuItemCount(menu) == 0)
    {
        DestroyMenu(menu);
        return;
    }

    s->selIdx = rowIdx;
    InvalidateRect(hWnd, nullptr, FALSE);

    UINT cmd = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
        screenX, screenY, 0, hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 0) return;

    int arrayHeaderIdx = (rowKind == RowKind::ArrayHeader) ? rowIdx : rowParentIdx;
    if (arrayHeaderIdx < 0) return;

    bool changed = false;
    // Snapshot the array's name *before* the rebuild - the rebuild may
    // mutate s->rows and we want to use it when shifting persistence.
    std::string arrayName = s->rows[arrayHeaderIdx].name;

    switch (cmd)
    {
    case kMenuAddItem:
        if (ab.ops.insert)
        {
            int n = ab.ops.count ? ab.ops.count(ab.userdata) : 0;
            changed = ab.ops.insert(n, ab.userdata);
            // Append - no key shift needed (the new slot is past the
            // end of existing persistence entries).
        }
        break;
    case kMenuEmptyArray:
        if (ab.ops.empty)
        {
            changed = ab.ops.empty(ab.userdata);
            if (changed)
            {
                // Wipe ALL per-element persistence for this array.
                std::string prefix = arrayName + "[";
                for (auto it = s->expandedMemory.begin();
                     it != s->expandedMemory.end();)
                {
                    if (it->first.compare(0, prefix.size(), prefix) == 0)
                        it = s->expandedMemory.erase(it);
                    else
                        ++it;
                }
            }
        }
        break;
    case kMenuInsertAbove:
        if (ab.ops.insert)
        {
            changed = ab.ops.insert(rowElemIdx, ab.userdata);
            if (changed)
                ShiftArrayElementExpansion(hWnd, arrayName.c_str(),
                                           rowElemIdx, +1);
        }
        break;
    case kMenuDelete:
        if (ab.ops.del)
        {
            changed = ab.ops.del(rowElemIdx, ab.userdata);
            if (changed)
                ShiftArrayElementExpansion(hWnd, arrayName.c_str(),
                                           rowElemIdx, -1);
        }
        break;
    }

    if (changed)
    {
        RebuildArrayElementsWithChildren(hWnd, s, arrayHeaderIdx);
        UpdateScrollBar(hWnd, s);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

static void OnLButtonDown(HWND hWnd, State* s, int x, int y)
{
    SetFocus(hWnd);

    RECT cr; GetClientRect(hWnd, &cr);

    // Splitter drag - take priority over row hit testing.
    if (HitSplitter(s, x, cr.right - cr.left))
    {
        // Commit any pending inline edit before starting the drag.
        if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
        s->draggingSplitter   = true;
        s->splitterDragOffset = x - s->splitterX;
        SetCapture(hWnd);
        return;
    }

    int rowYTop = 0;
    int idx = HitTestY(s, y, &rowYTop);
    if (idx < 0)
    {
        if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
        return;
    }

    Row& r = s->rows[idx];

    // Phase 4.1: per-row button hit-test runs first (buttons sit in the
    // value column and would otherwise be swallowed by the inline-edit
    // open path below).  Press only ARMS the button; the actual click
    // fires on WM_LBUTTONUP if the cursor is still over the same button
    // (matches standard Win32 BS_PUSHBUTTON / UT2004 behavior).
    if (!r.buttons.empty())
    {
        int btnIdx = HitTestRowButton(r, idx, s, rowYTop,
                                      cr.right - cr.left, x, y);
        if (btnIdx >= 0)
        {
            if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
            s->pressedBtnRow   = idx;
            s->pressedBtnIdx   = btnIdx;
            s->pressedBtnHover = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
    }

    if (r.kind == RowKind::Category)
    {
        if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
        r.expanded = !r.expanded;
        UpdateScrollBar(hWnd, s);
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    if (r.kind == RowKind::ArrayHeader)
    {
        // Click on the +/- glyph (or anywhere in the name column) toggles
        // the array's expanded state.  Clicking the value column just
        // selects.
        if (x < s->splitterX)
        {
            if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
            r.expanded = !r.expanded;
            UpdateScrollBar(hWnd, s);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
        s->selIdx = idx;
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    // ArrayElement and Scalar rows with children behave like ArrayHeaders
    // for the expand glyph: click in the name column (left of splitter)
    // on a row that has children toggles its expanded state instead of
    // entering inline edit.  Value-column clicks still select / open the
    // inline editor as before.  This is what powers the multi-level
    // drill-down on Notifys ([N] -> Notify -> obj header -> props).
    if ((r.kind == RowKind::ArrayElement || r.kind == RowKind::Scalar) &&
        x < s->splitterX && HasChildren(s, idx))
    {
        if (s->inlineEdit) EndInlineEdit(hWnd, s, true);
        r.expanded = !r.expanded;
        s->selIdx = idx;

        // Phase 4.1: persist ArrayElement expansion across refreshes so
        // user-toggled state survives sibling Add/Delete/Create.
        if (r.kind == RowKind::ArrayElement && r.parentIdx >= 0 &&
            r.parentIdx < (int)s->rows.size() &&
            s->rows[r.parentIdx].kind == RowKind::ArrayHeader)
        {
            char key[160];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "%s[%d]",
                        s->rows[r.parentIdx].name.c_str(), r.arrayElemIndex);
            s->expandedMemory[key] = r.expanded;
        }

        UpdateScrollBar(hWnd, s);
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    // Scalar / ArrayElement: select.
    s->selIdx = idx;

    // Click on value cell of an editable row opens the inline editor.
    if (r.editable && x > s->splitterX + 2)
    {
        BeginInlineEdit(hWnd, s, idx);
    }
    else if (s->inlineEdit)
    {
        EndInlineEdit(hWnd, s, true);
    }

    InvalidateRect(hWnd, nullptr, FALSE);
}

static void OnRButtonDown(HWND hWnd, State* s, int x, int y)
{
    int idx = HitTestY(s, y, nullptr);
    if (idx < 0) return;

    POINT pt = { x, y };
    ClientToScreen(hWnd, &pt);
    ShowArrayContextMenu(hWnd, s, idx, pt.x, pt.y);
}

static void OnMouseMove(HWND hWnd, State* s, int x, int y)
{
    // Phase 4.1: while a button is armed (mouse down on a button), track
    // whether the cursor is still over it - if the user drags off, we
    // "unpress" the button visually and skip firing the callback when
    // they release.  Drag back over and it re-arms.
    if (s->pressedBtnRow >= 0 && s->pressedBtnIdx >= 0)
    {
        std::vector<LayoutEntry> layout;
        ComputeLayout(s, layout);
        int rowYTop = -1;
        for (const auto& le : layout)
            if (le.rowIdx == s->pressedBtnRow)
            { rowYTop = le.yTop - s->scrollY; break; }

        bool stillOver = false;
        if (rowYTop >= 0 &&
            s->pressedBtnRow < (int)s->rows.size())
        {
            const Row& pr = s->rows[s->pressedBtnRow];
            RECT cr; GetClientRect(hWnd, &cr);
            int hit = HitTestRowButton(pr, s->pressedBtnRow, s, rowYTop,
                                       cr.right - cr.left, x, y);
            stillOver = (hit == s->pressedBtnIdx);
        }

        if (stillOver != s->pressedBtnHover)
        {
            s->pressedBtnHover = stillOver;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return;
    }

    if (!s->draggingSplitter) return;
    RECT cr; GetClientRect(hWnd, &cr);
    int clientW = cr.right - cr.left;
    int newX = x - s->splitterDragOffset;
    if (newX < kSplitterMinName)            newX = kSplitterMinName;
    if (newX > clientW - kSplitterMinValue) newX = clientW - kSplitterMinValue;
    if (newX == s->splitterX) return;
    s->splitterX = newX;

    // Reposition the inline editor if it's open while dragging.
    if (s->inlineEdit && s->editingRow >= 0)
    {
        RECT cell;
        if (GetValueCellRect(hWnd, s, s->editingRow, &cell))
            SetWindowPos(s->inlineEdit, nullptr,
                         cell.left, cell.top,
                         cell.right - cell.left, cell.bottom - cell.top,
                         SWP_NOZORDER);
    }
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void OnLButtonUp(HWND hWnd, State* s, int x, int y)
{
    if (s->draggingSplitter)
    {
        s->draggingSplitter = false;
        ReleaseCapture();
        return;
    }

    // Phase 4.1: commit the armed button click only if the cursor is
    // still over the same button at release time.  Otherwise the user
    // dragged off to cancel - drop the press silently.
    if (s->pressedBtnRow >= 0 && s->pressedBtnIdx >= 0)
    {
        int armedRow = s->pressedBtnRow;
        int armedBtn = s->pressedBtnIdx;
        s->pressedBtnRow   = -1;
        s->pressedBtnIdx   = -1;
        s->pressedBtnHover = false;
        ReleaseCapture();

        bool fire = false;
        if (armedRow >= 0 && armedRow < (int)s->rows.size())
        {
            std::vector<LayoutEntry> layout;
            ComputeLayout(s, layout);
            int rowYTop = -1;
            for (const auto& le : layout)
                if (le.rowIdx == armedRow)
                { rowYTop = le.yTop - s->scrollY; break; }

            if (rowYTop >= 0)
            {
                RECT cr; GetClientRect(hWnd, &cr);
                int hit = HitTestRowButton(s->rows[armedRow], armedRow, s,
                                           rowYTop, cr.right - cr.left,
                                           x, y);
                fire = (hit == armedBtn);
            }
        }

        if (fire)
        {
            // Snapshot the callback + userdata before invoking; the
            // callback may rebuild rows[] (e.g. RefreshNotifyTab) and
            // invalidate every Row reference here.
            const Row& pr = s->rows[armedRow];
            if (armedBtn < (int)pr.buttons.size())
            {
                auto cb = pr.buttons[armedBtn].callback;
                void* ud = pr.buttons[armedBtn].userdata;
                if (cb) cb(hWnd, armedRow, ud);
            }
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

static BOOL OnSetCursor(HWND hWnd, State* s)
{
    POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
    RECT cr; GetClientRect(hWnd, &cr);
    if (HitSplitter(s, pt.x, cr.right - cr.left))
    {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;
    }
    return FALSE;
}

static void OnVScroll(HWND hWnd, State* s, WPARAM wParam)
{
    // Close any open inline editor BEFORE moving rows under it.
    // Otherwise the EDIT/COMBO HWND stays at its old client-coords
    // position while the row paints elsewhere, leaving the text
    // "floating" over an unrelated row.  Commit the typed value
    // first so the user doesn't lose their edit.
    if (s->inlineEdit) EndInlineEdit(hWnd, s, true);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    GetScrollInfo(hWnd, SB_VERT, &si);

    int newPos = si.nPos;
    int line = kRowHeight;
    int page = si.nPage > 0 ? static_cast<int>(si.nPage) : line * 4;

    switch (LOWORD(wParam))
    {
    case SB_LINEUP:        newPos -= line; break;
    case SB_LINEDOWN:      newPos += line; break;
    case SB_PAGEUP:        newPos -= page; break;
    case SB_PAGEDOWN:      newPos += page; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: newPos = si.nTrackPos; break;
    case SB_TOP:           newPos = 0; break;
    case SB_BOTTOM:        newPos = si.nMax; break;
    }

    int maxPos = si.nMax - static_cast<int>(si.nPage) + 1;
    if (maxPos < 0) maxPos = 0;
    if (newPos < 0) newPos = 0;
    if (newPos > maxPos) newPos = maxPos;
    if (newPos == s->scrollY) return;

    s->scrollY = newPos;
    si.fMask = SIF_POS;
    si.nPos  = newPos;
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void OnMouseWheel(HWND hWnd, State* s, short zDelta)
{
    if (s->inlineEdit) EndInlineEdit(hWnd, s, true);

    // 3 rows per notch.
    int delta = -(zDelta / WHEEL_DELTA) * kRowHeight * 3;
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    GetScrollInfo(hWnd, SB_VERT, &si);
    int newPos = s->scrollY + delta;
    int maxPos = si.nMax - static_cast<int>(si.nPage) + 1;
    if (maxPos < 0) maxPos = 0;
    if (newPos < 0) newPos = 0;
    if (newPos > maxPos) newPos = maxPos;
    if (newPos == s->scrollY) return;
    s->scrollY = newPos;
    si.fMask = SIF_POS;
    si.nPos = newPos;
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        State* s = new State();
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    State* s = GetState(hWnd);

    switch (msg)
    {
    case WM_PAINT:
        if (s) OnPaint(hWnd, s);
        return 0;

    case WM_ERASEBKGND:
        return 1; // we paint the full rect ourselves; no flicker

    case WM_SIZE:
        if (s) UpdateScrollBar(hWnd, s);
        return 0;

    case WM_VSCROLL:
        if (s) OnVScroll(hWnd, s, wParam);
        return 0;

    case WM_MOUSEWHEEL:
        if (s) OnMouseWheel(hWnd, s, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_LBUTTONDOWN:
        if (s) OnLButtonDown(hWnd, s,
                             GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_RBUTTONDOWN:
        if (s) OnRButtonDown(hWnd, s,
                             GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        if (s) OnMouseMove(hWnd, s,
                           GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        if (s) OnLButtonUp(hWnd, s,
                           GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_SETCURSOR:
        if (s && reinterpret_cast<HWND>(wParam) == hWnd && OnSetCursor(hWnd, s))
            return TRUE;
        return DefWindowProcA(hWnd, msg, wParam, lParam);

    case WM_COMMAND:
        // The inline editor is our only child; route its commit-worthy
        // notifications to the commit path.  Defer to a posted message
        // so DestroyWindow doesn't happen inside WM_KILLFOCUS / dropdown
        // dispatch (which can confuse focus traversal).
        if (s && s->inlineEdit &&
            reinterpret_cast<HWND>(lParam) == s->inlineEdit)
        {
            WORD code = HIWORD(wParam);
            if (code == EN_KILLFOCUS ||      // EDIT lost focus
                code == CBN_KILLFOCUS ||     // COMBOBOX lost focus
                code == CBN_SELCHANGE)       // COMBOBOX dropdown pick
            {
                PostMessageA(hWnd, WM_APP + 1, 0, 0);
            }
        }
        return 0;

    case WM_APP + 1:
        if (s && s->inlineEdit) EndInlineEdit(hWnd, s, true);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;

    case WM_DESTROY:
        if (s)
        {
            FreeGdiObjects(s);
            delete s;
            SetWindowLongPtrA(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

} // anonymous namespace

void RegisterClass()
{
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (RegisterClassExA(&wc) != 0)
        registered = true;
}

HWND Create(HWND parent, int controlId, int x, int y, int w, int h)
{
    RegisterClass();
    return CreateWindowExA(WS_EX_CLIENTEDGE, kClassName, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
        x, y, w, h, parent, (HMENU)(INT_PTR)controlId,
        GetModuleHandleA(nullptr), nullptr);
}

void BeginUpdate(HWND grid)
{
    State* s = GetState(grid);
    if (s) s->updateDepth++;
}

void EndUpdate(HWND grid)
{
    State* s = GetState(grid);
    if (!s) return;
    if (s->updateDepth > 0) s->updateDepth--;
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
}

void Clear(HWND grid)
{
    State* s = GetState(grid);
    if (!s) return;
    // Cancel any in-progress edit before nuking its row.
    if (s->inlineEdit)
    {
        s->editCtx.commitOnDestroy = false;
        DestroyWindow(s->inlineEdit);
        s->inlineEdit = nullptr;
        s->editingRow = -1;
    }
    // Snapshot expansion state of categories and array headers BEFORE
    // wiping the rows, so the next AddCategory / AddArray with the
    // same name restores the user's previous choice.
    for (const auto& r : s->rows)
    {
        if (r.kind == RowKind::Category || r.kind == RowKind::ArrayHeader)
            s->expandedMemory[r.name] = r.expanded;
    }
    s->rows.clear();
    s->arrayBindings.clear();
    s->lastCatIdx = -1;
    s->selIdx     = -1;
    // Keep scrollY so the user's vertical position is preserved too.
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
}

int AddCategory(HWND grid, const char* name)
{
    State* s = GetState(grid);
    if (!s || !name) return -1;

    // Restore previous expansion choice for a category of this name
    // (defaults to expanded since UT2004 ships categories open).
    bool expanded = true;
    auto it = s->expandedMemory.find(name);
    if (it != s->expandedMemory.end()) expanded = it->second;

    Row r;
    r.kind             = RowKind::Category;
    r.name             = name;
    r.depth            = 0;
    r.parentIdx        = -1;
    r.expanded         = expanded;
    r.editable         = false;
    r.getter           = nullptr;
    r.setter           = nullptr;
    r.userdata         = nullptr;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    s->rows.push_back(std::move(r));
    int idx = static_cast<int>(s->rows.size()) - 1;
    s->lastCatIdx = idx;
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

int AddRow(HWND grid, const char* name, const char* value)
{
    State* s = GetState(grid);
    if (!s || !name) return -1;
    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = value ? value : "";
    r.depth            = 1;
    r.parentIdx        = s->lastCatIdx;
    r.expanded         = true;
    r.editable         = false;
    r.getter           = nullptr;
    r.setter           = nullptr;
    r.userdata         = nullptr;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    s->rows.push_back(std::move(r));
    int idx = static_cast<int>(s->rows.size()) - 1;
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

int AddEditableRow(HWND grid, const char* name,
                   ValueGetter getter, ValueSetter setter, void* userdata)
{
    State* s = GetState(grid);
    if (!s || !name || !getter || !setter) return -1;
    char buf[256] = "";
    getter(buf, sizeof(buf), userdata);

    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = buf;
    r.depth            = 1;
    r.parentIdx        = s->lastCatIdx;
    r.expanded         = true;
    r.editable         = true;
    r.getter           = getter;
    r.setter           = setter;
    r.userdata         = userdata;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    s->rows.push_back(std::move(r));
    int idx = static_cast<int>(s->rows.size()) - 1;
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

void RefreshRow(HWND grid, int rowIdx)
{
    State* s = GetState(grid);
    if (!s || rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;
    Row& r = s->rows[rowIdx];
    if (r.editable && r.getter)
    {
        char buf[256] = "";
        r.getter(buf, sizeof(buf), r.userdata);
        r.value = buf;
    }
    InvalidateRect(grid, nullptr, FALSE);
}

int AddEnumRow(HWND grid, const char* name,
               const char* const* options, int numOptions,
               ValueGetter getter, ValueSetter setter, void* userdata)
{
    State* s = GetState(grid);
    if (!s || !name || !getter || !setter || !options || numOptions <= 0)
        return -1;

    char buf[256] = "";
    getter(buf, sizeof(buf), userdata);

    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = buf;
    r.depth            = 1;
    r.parentIdx        = s->lastCatIdx;
    r.expanded         = true;
    r.editable         = true;
    r.getter           = getter;
    r.setter           = setter;
    r.userdata         = userdata;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    r.enumOptions.reserve(numOptions);
    for (int i = 0; i < numOptions; ++i)
        if (options[i]) r.enumOptions.emplace_back(options[i]);

    s->rows.push_back(std::move(r));
    int idx = static_cast<int>(s->rows.size()) - 1;
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

int AddArray(HWND grid, const char* name, const ArrayOps& ops, void* userdata)
{
    State* s = GetState(grid);
    if (!s || !name) return -1;

    ArrayBinding ab;
    ab.ops      = ops;
    ab.userdata = userdata;
    s->arrayBindings.push_back(ab);
    int abi = static_cast<int>(s->arrayBindings.size()) - 1;

    // Restore previous expansion choice for an array of this name
    // (defaults to collapsed since UT2004 ships arrays closed).
    bool expanded = false;
    auto it = s->expandedMemory.find(name);
    if (it != s->expandedMemory.end()) expanded = it->second;

    Row r;
    r.kind             = RowKind::ArrayHeader;
    r.name             = name;
    r.value            = "";
    r.depth            = 1;
    r.parentIdx        = s->lastCatIdx;
    r.expanded         = expanded;
    r.editable         = false;
    r.getter           = nullptr;
    r.setter           = nullptr;
    r.userdata         = nullptr;
    r.arrayBindingIdx  = abi;
    r.arrayElemIndex   = 0;
    s->rows.push_back(std::move(r));
    int headerIdx = static_cast<int>(s->rows.size()) - 1;

    // Populate child element rows from ops.count(), then give the
    // (optional) populateChildren callback a chance to attach inline
    // child rows under each element.  Phase 3.11: this is what powers
    // UAnimNotify_* property expansion under Notify[i].
    int n = ops.count ? ops.count(userdata) : 0;
    for (int i = 0; i < n; ++i)
        s->rows.push_back(MakeArrayElement(s, headerIdx, i));

    if (ops.populateChildren)
    {
        for (int i = 0; i < n; ++i)
        {
            // Re-locate element row each pass - earlier populateChildren
            // calls can have appended their own rows, shifting indices.
            int elemRow = -1;
            for (size_t k = headerIdx + 1; k < s->rows.size(); ++k)
            {
                if (s->rows[k].parentIdx == headerIdx &&
                    s->rows[k].kind == RowKind::ArrayElement &&
                    s->rows[k].arrayElemIndex == i)
                { elemRow = (int)k; break; }
            }
            if (elemRow < 0) continue;
            ops.populateChildren(grid, elemRow, i, userdata);
        }
    }

    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return headerIdx;
}

// =====================================================================
//  Phase 3.11: parent-explicit row APIs
// =====================================================================
//  Identical shape to AddRow / AddEditableRow / AddEnumRow but the new
//  row's parentIdx is supplied explicitly instead of inheriting
//  lastCatIdx.  Used by ArrayOps.populateChildren to attach property
//  rows to a particular ArrayElement.

int AddRowAt(HWND grid, int parentRowIdx, const char* name, const char* value)
{
    State* s = GetState(grid);
    if (!s || !name) return -1;
    if (parentRowIdx < -1 || parentRowIdx >= (int)s->rows.size()) return -1;
    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = value ? value : "";
    r.depth            = (parentRowIdx >= 0) ? s->rows[parentRowIdx].depth + 1 : 1;
    r.parentIdx        = parentRowIdx;
    r.expanded         = true;
    r.editable         = false;
    r.getter           = nullptr;
    r.setter           = nullptr;
    r.userdata         = nullptr;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    int idx = InsertRowInSubtree(s, parentRowIdx, std::move(r));
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

int AddEditableRowAt(HWND grid, int parentRowIdx, const char* name,
                     ValueGetter getter, ValueSetter setter, void* userdata)
{
    State* s = GetState(grid);
    if (!s || !name || !getter || !setter) return -1;
    if (parentRowIdx < -1 || parentRowIdx >= (int)s->rows.size()) return -1;
    char buf[256] = "";
    getter(buf, sizeof(buf), userdata);

    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = buf;
    r.depth            = (parentRowIdx >= 0) ? s->rows[parentRowIdx].depth + 1 : 1;
    r.parentIdx        = parentRowIdx;
    r.expanded         = true;
    r.editable         = true;
    r.getter           = getter;
    r.setter           = setter;
    r.userdata         = userdata;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    int idx = InsertRowInSubtree(s, parentRowIdx, std::move(r));
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

int AddEnumRowAt(HWND grid, int parentRowIdx, const char* name,
                 const char* const* options, int numOptions,
                 ValueGetter getter, ValueSetter setter, void* userdata)
{
    State* s = GetState(grid);
    if (!s || !name || !getter || !setter || !options || numOptions <= 0)
        return -1;
    if (parentRowIdx < -1 || parentRowIdx >= (int)s->rows.size()) return -1;

    char buf[256] = "";
    getter(buf, sizeof(buf), userdata);

    Row r;
    r.kind             = RowKind::Scalar;
    r.name             = name;
    r.value            = buf;
    r.depth            = (parentRowIdx >= 0) ? s->rows[parentRowIdx].depth + 1 : 1;
    r.parentIdx        = parentRowIdx;
    r.expanded         = true;
    r.editable         = true;
    r.getter           = getter;
    r.setter           = setter;
    r.userdata         = userdata;
    r.arrayBindingIdx  = -1;
    r.arrayElemIndex   = 0;
    r.enumOptions.reserve(numOptions);
    for (int i = 0; i < numOptions; ++i)
        if (options[i]) r.enumOptions.emplace_back(options[i]);

    int idx = InsertRowInSubtree(s, parentRowIdx, std::move(r));
    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
    return idx;
}

// =====================================================================
//  Phase 4.1: AddRowButton + SetRowExpanded public APIs
// =====================================================================
void AddRowButton(HWND grid, int rowIdx, const char* label,
                  RowButtonFn callback, void* userdata,
                  ButtonVisibility visibility)
{
    State* s = GetState(grid);
    if (!s || !label) return;
    if (rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;

    RowButton b;
    b.label      = label;
    b.callback   = callback;
    b.userdata   = userdata;
    b.visibility = visibility;
    s->rows[rowIdx].buttons.push_back(std::move(b));

    if (s->updateDepth == 0)
        InvalidateRect(grid, nullptr, FALSE);
}

// Phase 4.1: shift element-expansion persistence keys so the user's
// per-element expand state follows their DATA across Insert/Delete
// reorders (otherwise the empty slot at the inserted index looks
// "expanded with no data" while the user's actual data sits collapsed
// at idx+1).
void ShiftArrayElementExpansion(HWND grid, const char* arrayName,
                                int fromIdx, int delta)
{
    State* s = GetState(grid);
    if (!s || !arrayName || delta == 0) return;

    std::string prefix = std::string(arrayName) + "[";

    struct Entry { int idx; bool expanded; };
    std::vector<Entry> entries;
    for (const auto& kv : s->expandedMemory)
    {
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        const char* numStart = kv.first.c_str() + prefix.size();
        const char* numEnd   = std::strchr(numStart, ']');
        if (!numEnd) continue;
        int idx = std::atoi(numStart);
        entries.push_back({ idx, kv.second });
    }

    // Drop all original entries first; we'll re-insert them at shifted
    // indices below.
    for (const auto& e : entries)
    {
        char key[160];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s%d]",
                    prefix.c_str(), e.idx);
        s->expandedMemory.erase(key);
    }

    for (const auto& e : entries)
    {
        int newIdx = e.idx;
        if (delta > 0)
        {
            // Insert at fromIdx: everything >= fromIdx shifts up.
            if (e.idx >= fromIdx) newIdx = e.idx + delta;
        }
        else
        {
            // Delete at fromIdx: drop the slot at fromIdx; shift the
            // rest down.
            if (e.idx == fromIdx) continue;
            if (e.idx >  fromIdx) newIdx = e.idx + delta;
        }
        char key[160];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s%d]",
                    prefix.c_str(), newIdx);
        s->expandedMemory[key] = e.expanded;
    }
}

void SetRowValue(HWND grid, int rowIdx, const char* value)
{
    State* s = GetState(grid);
    if (!s || rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;
    s->rows[rowIdx].value = value ? value : "";
    if (s->updateDepth == 0)
        InvalidateRect(grid, nullptr, FALSE);
}

void SetRowExpanded(HWND grid, int rowIdx, bool expanded)
{
    State* s = GetState(grid);
    if (!s) return;
    if (rowIdx < 0 || rowIdx >= (int)s->rows.size()) return;

    Row& r = s->rows[rowIdx];
    if (r.expanded == expanded) return;
    r.expanded = expanded;

    // Mirror the click-handler's persistence behavior so the new state
    // sticks across the next refresh.
    if (r.kind == RowKind::ArrayElement && r.parentIdx >= 0 &&
        r.parentIdx < (int)s->rows.size() &&
        s->rows[r.parentIdx].kind == RowKind::ArrayHeader)
    {
        char key[160];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s[%d]",
                    s->rows[r.parentIdx].name.c_str(), r.arrayElemIndex);
        s->expandedMemory[key] = expanded;
    }

    if (s->updateDepth == 0)
    {
        UpdateScrollBar(grid, s);
        InvalidateRect(grid, nullptr, FALSE);
    }
}

void SetBounds(HWND grid, int x, int y, int w, int h)
{
    if (!grid) return;
    SetWindowPos(grid, nullptr, x, y, w, h, SWP_NOZORDER);
}

} // namespace PropertyGrid
