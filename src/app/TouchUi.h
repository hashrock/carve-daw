#pragma once

#include <functional>

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

// Two fingers scroll, one finger edits.
//
// An editor inside a viewport -- the piano roll, the playlist -- uses a
// one-finger drag for everything a mouse drag does there: moving a note,
// drawing one, a rubber band. So the viewport is told not to scroll under a
// dragging finger (see leaveDragsToContent), and the editor passes its mouse
// events through one of these first. When a second finger comes down the
// gesture the first one began is called off, and from there the two fingers
// move the viewport until both are lifted.
//
// Each of mouseDown / mouseDrag / mouseUp returns true when the event was
// part of a scroll and the editor should do nothing with it.
class TwoFingerScroll
{
public:
    // cancelGesture undoes whatever the first finger's press started.
    bool mouseDown (const juce::MouseEvent& e, juce::Viewport* viewport,
                    const std::function<void()>& cancelGesture)
    {
        if (! isTouchUi)
            return false;

        const auto finger = e.source.getIndex();

        if (finger > 1)
            return true;   // a third finger is nobody's

        start[finger] = current[finger] = e.getScreenPosition();

        if (finger == 0)
        {
            down[0] = true;
            scrolling = swallowing = false;
            return false;
        }

        // Before the second finger is counted as down: calling the gesture
        // off may come back through mouseUp with this same event.
        if (! swallowing && cancelGesture)
            cancelGesture();

        down[1] = true;

        // Measured from where both fingers are now, not from where the
        // first one landed.
        start[0] = current[0];
        swallowing = true;
        scrolling = viewport != nullptr;

        if (viewport != nullptr)
            origin = viewport->getViewPosition();

        return true;
    }

    bool mouseDrag (const juce::MouseEvent& e, juce::Viewport* viewport)
    {
        if (! isTouchUi)
            return false;

        const auto finger = e.source.getIndex();

        if (finger > 1)
            return true;

        // In screen coordinates: the editor itself moves as it scrolls.
        current[finger] = e.getScreenPosition();

        if (! swallowing)
            return false;

        if (scrolling && viewport != nullptr)
            viewport->setViewPosition (origin - ((current[0] - start[0]) + (current[1] - start[1])) / 2);

        return true;
    }

    bool mouseUp (const juce::MouseEvent& e)
    {
        if (! isTouchUi)
            return false;

        const auto finger = e.source.getIndex();

        if (finger > 1)
            return true;

        down[finger] = false;

        if (! swallowing)
            return false;

        // One finger left is not a scroll, and not a new gesture either:
        // nothing more happens until both are up.
        scrolling = false;
        swallowing = down[0] || down[1];
        return true;
    }

private:
    bool down[2] = { false, false };
    juce::Point<int> start[2], current[2], origin;
    bool swallowing = false, scrolling = false;
};

// For a viewport whose content takes drags itself. JUCE scrolls a viewport
// under a dragging finger unless told not to, which turns every note moved
// into a scroll as well.
inline void leaveDragsToContent (juce::Viewport& viewport)
{
    if (isTouchUi)
        viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
}

// A viewport whose content is all controls -- knobs, pads, envelopes -- and
// has no two-finger scroll of its own: the scroll bars do it, widened to be
// something a finger can hold.
inline void scrollByBarsOnly (juce::Viewport& viewport)
{
    if (! isTouchUi)
        return;

    leaveDragsToContent (viewport);
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
