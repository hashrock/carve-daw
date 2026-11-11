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

// A press on empty space in a viewport that scrolls under a finger. It may be
// the start of a scroll or it may be a tap, and only the release says which,
// so whatever a tap there does -- draw a note, paint a clip -- waits for it.
struct TouchTap
{
    void arm (juce::Point<float> where)
    {
        armed = true;
        position = where;
        pressedAt = juce::Time::getMillisecondCounter();
    }

    // Call from mouseUp. True if the press that was armed ended as a tap:
    // short, and without having moved. A long press is not one (it is the
    // right click; see LongPress.h).
    bool wasTap (const juce::MouseEvent& e)
    {
        return std::exchange (armed, false)
            && ! e.mouseWasDraggedSinceMouseDown()
            && juce::Time::getMillisecondCounter() - pressedAt < 400;
    }

    bool armed = false;
    juce::Point<float> position;
    juce::uint32 pressedAt = 0;
};

// A viewport whose content is all controls -- knobs, pads, envelopes -- where
// there is no empty space for a finger to scroll by. JUCE scrolls a viewport
// under a dragging finger unless told not to, which there would turn every
// knob drag into a scroll as well, so the scroll bars do it, widened to be
// something a finger can hold.
inline void scrollByBarsOnly (juce::Viewport& viewport)
{
    if (! isTouchUi)
        return;

    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    viewport.setScrollBarThickness (20);
}

struct BarScrolledViewport : juce::Viewport
{
    BarScrolledViewport() { scrollByBarsOnly (*this); }
};

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
