#pragma once

#include <functional>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ExportComponent.h"

namespace carve::app
{

// The export dialog in a floating window, in the same shape as MixerWindow and
// PianoRollWindow: the owner passes a close callback and destroys the window
// from it, deferred.
//
// Deliberately not modal. The render leans on the message loop -- tracktion
// builds its render graph through callBlocking, which posts to the message
// thread and waits for it -- so a modal loop here would be a deadlock waiting
// to happen. A plain always-on-top window that the rest of the app can be used
// around is both safer and nicer.
class ExportWindow : public juce::DocumentWindow
{
public:
    ExportWindow (te::Edit& edit, model::Song song, std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("Export Audio",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (edit, std::move (song))
    {
        content.onFinished = [this] { requestClose(); };

        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setAlwaysOnTop (true);
        setVisible (true);
        toFront (true);
    }

    // Loading a song while the dialog is open would otherwise leave it
    // exporting the song that has just been replaced.
    void setSong (model::Song song)
    {
        content.setSong (std::move (song));
    }

    void closeButtonPressed() override
    {
        requestClose();
    }

private:
    void requestClose()
    {
        // Nothing is left running behind the window: a render in progress is
        // stopped here, where there is still something on screen to say so.
        content.cancelRender();

        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    std::function<void()> onClose;
    ExportComponent content;
};

} // namespace carve::app
