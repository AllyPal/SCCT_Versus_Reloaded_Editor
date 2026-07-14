#pragma once

namespace AnimationBrowser
{
    // Called once from DllMain to register the window class and initialise state.
    void Initialize();

    // Brings the Animation Browser to front, creating it on first use.
    void Show(HWND hParent);
}
