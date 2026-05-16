#pragma once

// Runtime-configurable realtime viewport frame rate cap.
// Default: 240.   0 = unlimited (no throttle applied).
// Range:   0 – 999.
// Modified at runtime by the Reloaded Options dialog (View > Reloaded Options).
// Persisted to Reloaded_Editor.ini beside ChaosTheory_Editor.exe.
extern int g_ReloadedMaxFPS;

// When true, UUNIAudioSubsystem::Update is skipped each frame so no new
// viewport sounds are registered or updated.  The Sound Browser is unaffected
// because it drives playback through a separate PlaySound vtable slot.
// Default: false.  Toggled by the Reloaded Options dialog.
// Persisted to Reloaded_Editor.ini beside ChaosTheory_Editor.exe.
extern bool g_ReloadedMuteSounds;

// When true, edactPasteSelected suppresses the GridSize position offset that
// is normally applied to duplicated actors (Duplicate==true branch).
// The paste offset (Duplicate==false) is unaffected.
// Default: false.  Toggled by the Reloaded Options dialog.
// Persisted to Reloaded_Editor.ini beside ChaosTheory_Editor.exe.
extern bool g_ReloadedNoDuplicateOffset;

class RealtimeFix
{
public:
    static void Initialize();
};
