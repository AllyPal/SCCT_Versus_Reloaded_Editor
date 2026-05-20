#pragma once

namespace AnimationBrowser
{
    // Called once from DllMain to register the window class and initialise state.
    void Initialize();

    // Brings the Animation Browser to front, creating it on first use.
    // Hooked from General.cpp::MenuBarDispatch when the user picks
    // View > Show Animation Browser (menu ID 40067, added via Resource Hacker).
    void Show(HWND hParent);
}
