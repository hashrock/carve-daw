#pragma once

#include <functional>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

#include "SettingsComponent.h"

namespace carve::app
{

// The settings panel in a floating window, in the same shape as the export and
// mixer windows: the owner passes a close callback and destroys the window
// from it, deferred. Not modal -- changing the audio device restarts playback,
// and there is no reason the rest of the app cannot be used around it.
class SettingsWindow : public juce::DocumentWindow
{
public:
    SettingsWindow (te::Engine& engine, std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("Settings",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (engine)
    {
        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (420, 380, 900, 900);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;
    SettingsComponent content;
};

} // namespace carve::app
