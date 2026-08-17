#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PianoRollComponent.h"

namespace orionish::app
{

// Floating pattern editor (Orion-style): the playlist stays in the main
// window and each pattern is edited in this popup. Shows whichever pattern
// is currently selected.
class PianoRollWindow : public juce::DocumentWindow
{
public:
    PianoRollWindow (juce::UndoManager& um,
                     std::function<void()> onCloseCallback,
                     std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Pattern Editor",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          onKey (std::move (keyHandler)),
          pianoRoll (um)
    {
        viewport.setViewedComponent (&pianoRoll, false);
        viewport.setScrollBarsShown (true, true);
        viewport.setSize (860, 520);

        setContentNonOwned (&viewport, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);

        // start scrolled to the middle of the pitch range
        viewport.setViewPosition (0, juce::jmax (0, pianoRoll.getHeight() / 2 - 200));
    }

    void setPattern (std::optional<model::Pattern> pattern, const juce::String& title)
    {
        pianoRoll.setPattern (std::move (pattern));
        setName (title.isNotEmpty() ? "Pattern Editor — " + title : "Pattern Editor");
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (onKey != nullptr && onKey (key))
            return true;
        return juce::DocumentWindow::keyPressed (key);
    }

private:
    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    juce::Viewport viewport;
    PianoRollComponent pianoRoll;
};

} // namespace orionish::app
