#pragma once

#include <functional>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "TouchUi.h"

namespace carve::app
{

// What a bounce is doing while it renders: what is being bounced, how far it
// has got, and a Cancel. Not modal, for the reason ExportWindow is not -- the
// render needs the message loop. The owner destroys it, deferred, from
// onCancel, and directly once the bounce has finished.
class BounceProgressWindow : public juce::DocumentWindow
{
public:
    BounceProgressWindow (const juce::String& what, std::function<float()> getProgress,
                          std::function<void()> onCancelCallback)
        : juce::DocumentWindow ("Bounce", juce::Colour (0xff232327), juce::DocumentWindow::closeButton),
          content (what, std::move (getProgress))
    {
        onCancel = std::move (onCancelCallback);
        content.cancelButton.onClick = [this] { cancel(); };

        setContentNonOwned (&content, true);
        useSystemTitleBar (*this);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setAlwaysOnTop (true);
        setVisible (true);
    }

    void closeButtonPressed() override  { cancel(); }

private:
    void cancel()
    {
        if (auto callback = std::exchange (onCancel, nullptr))
            callback();
    }

    struct Content : public juce::Component,
                     private juce::Timer
    {
        Content (const juce::String& what, std::function<float()> getProgressToUse)
            : getProgress (std::move (getProgressToUse))
        {
            label.setText ("Bouncing " + what + "...", juce::dontSendNotification);
            label.setFont (uiFont (fonts::small));
            label.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));

            addAndMakeVisible (label);
            addAndMakeVisible (bar);
            addAndMakeVisible (cancelButton);
            setSize (320, 104);
            startTimerHz (20);
        }

        void timerCallback() override
        {
            progress = getProgress ? (double) getProgress() : 0.0;
            bar.repaint();
        }

        void paint (juce::Graphics& g) override  { g.fillAll (juce::Colour (0xff232327)); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (14, 12);
            label.setBounds (area.removeFromTop (20));
            area.removeFromTop (6);
            bar.setBounds (area.removeFromTop (16));
            cancelButton.setBounds (area.removeFromBottom (26).removeFromRight (84));
        }

        std::function<float()> getProgress;
        double progress = 0.0;   // before the bar, which keeps a reference to it
        juce::Label label;
        juce::ProgressBar bar { progress };
        juce::TextButton cancelButton { "Cancel" };
    };

    std::function<void()> onCancel;
    Content content;
};

} // namespace carve::app
