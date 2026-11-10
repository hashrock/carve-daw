#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// What differs on a screen that is touched rather than pointed at. Today that
// is the iPad build, which is a prototype: one full-screen window, no window
// manager, no menu bar, and no right mouse button.
#if JUCE_IOS
inline constexpr bool isTouchUi = true;
#else
inline constexpr bool isTouchUi = false;
#endif

// The part of the display a window may use: inside the status bar, the home
// indicator and the rounded corners.
inline juce::Rectangle<int> usableScreenArea()
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return display->safeAreaInsets.subtractedFrom (display->userArea).reduced (isTouchUi ? 4 : 0);

    return {};
}

// The title bar of every window but the main one. On the desktop it is the
// system's. iOS has no system title bar to ask for -- a second window would
// come up as a bare rectangle with nothing to close it by or move it with --
// so there the window draws its own, tall enough for a finger, and stays
// above the main window, which fills the screen and would otherwise swallow
// it at the first tap. It is also made to fit the screen, since nothing there
// can be dragged back from beyond the edge.
//
// Call it where setUsingNativeTitleBar (true) would have gone.
inline void useSystemTitleBar (juce::DocumentWindow& window)
{
    if (! isTouchUi)
    {
        window.setUsingNativeTitleBar (true);
        return;
    }

    window.setUsingNativeTitleBar (false);
    window.setTitleBarHeight (30);
    window.setTitleBarButtonsRequired (juce::DocumentWindow::closeButton, true);
    window.setAlwaysOnTop (true);

    // After the constructor that called this has finished sizing and placing
    // the window, whatever order it does that in.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::DocumentWindow> (&window)]
    {
        if (safe == nullptr)
            return;

        if (const auto area = usableScreenArea(); ! area.isEmpty())
            safe->setBounds (safe->getBounds().constrainedWithin (area));
    });
}

} // namespace carve::app
