#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MixerComponent.h"

namespace orionish::app
{

// Floating mixer, in the same spirit as the Pattern Editor window: the main
// window keeps the playlist, everything else is a popup.
class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow (te::Edit& edit, juce::UndoManager& um,
                 std::function<void()> onCloseCallback,
                 std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Mixer",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          onKey (std::move (keyHandler)),
          mixer (edit, um)
    {
        viewport.setViewedComponent (&mixer, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setSize (juce::jlimit (280, 760, mixer.getWidth() + 4), 340);

        setContentNonOwned (&viewport, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void setSong (model::Song song)
    {
        mixer.setSong (std::move (song));
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

    void resized() override
    {
        juce::DocumentWindow::resized();

        // strips fill the window height; width stays content-driven so the
        // viewport scrolls horizontally when there are many generators
        mixer.setSize (mixer.getWidth(), viewport.getMaximumVisibleHeight());
    }

private:
    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    juce::Viewport viewport;
    MixerComponent mixer;
};

} // namespace orionish::app
