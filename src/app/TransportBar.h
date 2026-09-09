#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "CarveLogo.h"
#include "IconButton.h"
#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

//==============================================================================
// The Carve mark at the top left, which is also the app's menu: the file
// operations hang off it. They are the things done once a session -- open,
// save, export -- and three permanent buttons for them were three more boxes
// in a bar that is looked at all the time.
class LogoButton : public juce::Button
{
public:
    LogoButton()
        : juce::Button ("Carve"),
          logo (createCarveLogo (juce::Colour (0xffe6e6ea)))
    {
        setTooltip ("Open, save, export");
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        if (highlighted || down)
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.16f : 0.08f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
        }

        auto area = getLocalBounds().toFloat().reduced (4.0f);
        auto caret = area.removeFromRight (10.0f);

        if (logo != nullptr)
            logo->drawWithin (g, area.reduced (1.0f), juce::RectanglePlacement::centred, 1.0f);

        // The caret says there is a menu here, which a bare mark does not.
        juce::Path arrow;
        arrow.addTriangle (caret.getCentreX() - 3.5f, caret.getCentreY() - 1.5f,
                           caret.getCentreX() + 3.5f, caret.getCentreY() - 1.5f,
                           caret.getCentreX(),        caret.getCentreY() + 2.5f);
        g.setColour (juce::Colour (0xff9a9aa4));
        g.fillPath (arrow);
    }

private:
    std::unique_ptr<juce::Drawable> logo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LogoButton)
};

//==============================================================================
// The logo menu, play/stop, the loop / undo / redo symbols, BPM, position
// readout, the mixer and the document name.
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um);

    std::function<void()> onSave, onSaveAs, onOpen, onOpenMixer, onExport, onToggleBrowser;

    // The Browser button is a lit toggle showing whether the panel is up;
    // MainComponent owns the panel and tells the bar, so a cmd-B and a click
    // both land the same way.
    void setBrowserShown (bool shown);

    void setSong (model::Song newSong);

    // The window title is the authoritative place for this, but the title bar
    // is easy to lose behind the floating editors, so the transport bar shows
    // it too. Owned by MainComponent, which tracks the document.
    void setDocumentState (const juce::String& documentName, bool hasUnsavedChanges);

    void togglePlay();

    void resized() override;

private:
    void timerCallback() override;
    double getSongLengthBeats() const;

    te::Edit& edit;
    model::Song song;
    juce::UndoManager& undoManager;

    void showFileMenu();

    LogoButton logoButton;
    IconButton playButton   { "Play",   Icon::play },
               stopButton   { "Stop",   Icon::stop },
               loopButton   { "",       Icon::loop },     // flat symbols: loop is a lit toggle
               undoButton   { "",       Icon::undo },
               redoButton   { "",       Icon::redo },
               mixerButton  { "Mixer",  Icon::mixer },
               browserButton { "Browser", Icon::folder };
    juce::Label bpmLabel, positionLabel, documentLabel;
};

} // namespace carve::app
