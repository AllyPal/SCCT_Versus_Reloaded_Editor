#pragma once

namespace PropertyGrid
{
    // Registers the "ReloadedPropertyGrid" window class.  Idempotent;
    // safe to call from every Create.
    void RegisterClass();

    // Creates a grid child window.
    HWND Create(HWND parent, int controlId, int x, int y, int w, int h);

    // Row population.  Wrap a batch of Add* calls in BeginUpdate/
    // EndUpdate to suppress redraws until the whole batch is in.
    void BeginUpdate(HWND grid);
    void EndUpdate  (HWND grid);

    // Clears all rows and resets selection / scroll.
    void Clear(HWND grid);

    // Adds a category header.  Subsequent AddRow calls are placed under
    // it until the next AddCategory.  Returns the row index.
    int  AddCategory(HWND grid, const char* name);

    // Adds a scalar property row under the most recently added category.
    // `value` is the formatted display string.  Read-only - clicking the
    // value cell highlights the row but does not open an editor.
    int  AddRow(HWND grid, const char* name, const char* value);

    // Adds an editable row.  When the user clicks (or double-clicks) the
    // value cell, an EDIT control is overlaid with the current value;
    // on focus loss the new text is sent to `setter`.  `getter` is
    // called to refresh the cached display string.
    //   getter(buf, size, userdata)  - fill buf with current value
    //   setter(text, userdata)       - apply user-entered text
    typedef void (*ValueGetter)(char* buf, int size, void* userdata);
    typedef void (*ValueSetter)(const char* text, void* userdata);
    int  AddEditableRow(HWND grid, const char* name,
                        ValueGetter getter, ValueSetter setter,
                        void* userdata);

    // Trigger a redraw + value refresh of an existing row.  Useful when
    // the underlying data changed for reasons other than direct edits
    // (e.g. the user picked a different sequence).
    void RefreshRow(HWND grid, int rowIdx);

    // Adds a row whose value is constrained to a fixed set of strings.
    // Clicking the value cell pops a CBS_DROPDOWNLIST combobox seeded
    // with `options`, pre-selected to the current value (returned by
    // `getter`).  Picking a value auto-commits via `setter`.  This is
    // UnrealEd 2's editor style for bool / enum properties.
    int  AddEnumRow(HWND grid, const char* name,
                    const char* const* options, int numOptions,
                    ValueGetter getter, ValueSetter setter, void* userdata);

    // -----------------------------------------------------------------
    //  Array rows (Turn 3) - mirror UT2004 WProperties array UI:
    //    [-] ArrayName              (count)
    //        [0]  value
    //        [1]  value
    //  Right-click on the header opens [Add Item] / [Empty Array].
    //  Right-click on an element  opens [Insert Above] / [Delete].
    //  Editing element values uses the same inline EDIT lifecycle as
    //  AddEditableRow.
    typedef int  (*ArrayCountFn) (void* userdata);
    typedef void (*ArrayElemGet) (int index, char* buf, int size, void* userdata);
    typedef void (*ArrayElemSet) (int index, const char* text, void* userdata);
    typedef bool (*ArrayInsertFn)(int beforeIndex, void* userdata); // append when beforeIndex == count
    typedef bool (*ArrayDeleteFn)(int index, void* userdata);
    typedef bool (*ArrayEmptyFn) (void* userdata);

    // Optional per-element children populator.  When set,
    // the grid invokes this after creating each ArrayElement row so the
    // consumer can attach inline-expandable child rows to that element
    // (e.g., the individual properties of a Notify[i]'s UAnimNotify_*).
    // The callback should call AddRowAt / AddEditableRowAt / AddEnumRowAt
    // with `parentRowIdx == elementRowIdx`.  Children are wiped and
    // re-populated automatically on Insert/Delete/Empty.
    typedef void (*ArrayChildPopulator)(HWND grid, int elementRowIdx,
                                        int elementIndex, void* userdata);

    struct ArrayOps
    {
        ArrayCountFn  count;
        ArrayElemGet  get;
        ArrayElemSet  set;     // null if elements are read-only
        ArrayInsertFn insert;  // null if array is fixed-size
        ArrayDeleteFn del;
        ArrayEmptyFn  empty;
        ArrayChildPopulator populateChildren; // null = no children
    };

    // Adds an array row under the most recent category and populates
    // its element rows by calling ops.count() / ops.get(...).  Returns
    // the array header's row index.
    int  AddArray(HWND grid, const char* name, const ArrayOps& ops, void* userdata);

    // -----------------------------------------------------------------
    //  Parent-explicit row APIs
    //  These mirror AddRow / AddEditableRow / AddEnumRow but parent the
    //  new row to a caller-specified row index instead of the most
    //  recently added category.  Used by ArrayOps.populateChildren to
    //  attach inline properties to a specific ArrayElement.
    int  AddRowAt(HWND grid, int parentRowIdx,
                  const char* name, const char* value);
    int  AddEditableRowAt(HWND grid, int parentRowIdx, const char* name,
                          ValueGetter getter, ValueSetter setter,
                          void* userdata);
    int  AddEnumRowAt(HWND grid, int parentRowIdx, const char* name,
                      const char* const* options, int numOptions,
                      ValueGetter getter, ValueSetter setter, void* userdata);

    // -----------------------------------------------------------------
    //  Per-row action buttons - small clickable buttons
    //  drawn right-aligned in the value cell.  Used for UT2004's
    //  "Empty" / "Add" on array headers, "New" on the null-NotifyObject
    //  class picker, etc.  Buttons render in the order added (leftmost
    //  added first, right-aligned together).
    //
    //  Visibility controls when each button is rendered relative to the
    //  current selection (matches UT2004's context-sensitive behavior
    //  where Delete/Insert only show on the currently-selected element).
    enum ButtonVisibility : int
    {
        BTN_VIS_ALWAYS                = 0,
        BTN_VIS_SELECTED              = 1,
        BTN_VIS_SELECTED_OR_DESCENDANT= 2,
    };

    typedef void (*RowButtonFn)(HWND grid, int rowIdx, void* userdata);
    void AddRowButton(HWND grid, int rowIdx, const char* label,
                      RowButtonFn callback, void* userdata,
                      ButtonVisibility visibility = BTN_VIS_ALWAYS);

    // Set the expanded state of any row (Category, ArrayHeader, or any
    // Scalar/ArrayElement with children).  Used to auto-expand a newly
    // created notify after its class is picked.
    void SetRowExpanded(HWND grid, int rowIdx, bool expanded);

    // replace a row's displayed value text directly.  Used
    // by composite-property setters (Vector / Rotator member edits) to
    // refresh their parent row's summary string when a child component
    // changes.
    void SetRowValue(HWND grid, int rowIdx, const char* value);

    // shifts per-element expansion-state keys in expandedMemory
    // after an Insert/Delete reorders an array's element indices.  Call
    // from the consumer right after the underlying TArray mutation but
    // BEFORE the grid rebuild, so the rebuild's MakeArrayElement reads
    // the shifted keys when restoring expand state.  Without this the
    // expansion state stays at the now-vacant index and the user's data
    // appears to vanish even though it just shifted by one slot.
    //   delta = +1 for an Insert at fromIdx (slots [fromIdx..] shift up)
    //   delta = -1 for a Delete at fromIdx (slot [fromIdx] removed,
    //             slots [fromIdx+1..] shift down)
    void ShiftArrayElementExpansion(HWND grid, const char* arrayName,
                                    int fromIdx, int delta);

    // Reposition / resize the grid.
    void SetBounds(HWND grid, int x, int y, int w, int h);
}
