#pragma once

#include <functional>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "CarveLogo.h"
#include "IconButton.h"
#include "LevelMeterView.h"
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
// A number in a recessed display with a pair of step arrows beside it -- the
// BPM, the bar counter, the loop's start and length. Orion's blue LCDs, in
// this app's colours. Double-click the number to type one.
class NumberDisplay : public juce::Component
{
public:
    explicit NumberDisplay (int digits = 3);

    // +1 / -1 from the arrows.
    std::function<void (int direction)> onStep;

    // A value typed into the display (already trimmed; may fail to parse).
    std::function<void (const juce::String&)> onEdited;

    void setText (const juce::String& text);
    bool isBeingEdited() const  { return value.isBeingEdited(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    // The arrows' column, so callers can size the whole thing from a digit count.
    static constexpr int arrowsWidth = 12;

private:
    juce::Rectangle<int> upArrow, downArrow;
    juce::Label value;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NumberDisplay)
};

//==============================================================================
// Pattern / Song: two rows with a lamp each, one lit. Pattern plays the
// pattern being edited on its own, looped; Song plays the playlist.
class PlayModeSwitch : public juce::Component
{
public:
    std::function<void (bool patternMode)> onChanged;

    bool isPatternMode() const  { return patternMode; }
    void setPatternMode (bool shouldBePattern, juce::NotificationType);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    bool patternMode = false;
};

//==============================================================================
// Orion's toolbar, in Carve's clothes: the logo menu, tempo, Pattern / Song,
// the transport group, the bar counter, the loop with its start and length,
// undo / redo, the mixer, and the master level with a meter. Groups are drawn
// as recessed panels with dividers between them, which is what makes a row of
// this many controls readable at a glance.
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um);

    std::function<void()> onSave, onSaveAs, onOpen, onOpenMixer, onExport;

    // Pattern mode was switched on or off. The owner rebuilds the Edit around
    // the selected pattern (or the playlist again) -- see sync::Audition.
    std::function<void (bool patternMode)> onPlayModeChanged;

    // The length in beats of the pattern Pattern mode is looping, or 0 when
    // there is none; asked for whenever playback starts in that mode.
    std::function<double()> getAuditionLengthBeats;

    bool isPatternMode() const  { return playMode.isPatternMode(); }

    static constexpr int preferredHeight = 48;

    void setSong (model::Song newSong);

    // The window title is the authoritative place for this, but the title bar
    // is easy to lose behind the floating editors, so the transport bar shows
    // it too. Owned by MainComponent, which tracks the document.
    void setDocumentState (const juce::String& documentName, bool hasUnsavedChanges);

    void togglePlay();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showFileMenu();
    double getSongLengthBeats() const;

    // Bars are 1-based on the bar and in the loop fields, as on every ruler.
    int getCurrentBar() const;
    void seekToBar (int bar);
    void stepBar (int direction);

    // The loop fields edit the song's loop range in whole bars. Without an
    // explicit range the song loops as a whole, which is what the fields show
    // until one is typed or stepped in.
    int getLoopStartBar() const;
    int getLoopLengthBars() const;
    void setLoopBars (int startBar, int lengthBars);

    void setMasterVolumeFromKnob();

    te::Edit& edit;
    model::Song song;
    juce::UndoManager& undoManager;

    LogoButton logoButton;
    NumberDisplay bpmDisplay { 5 };
    PlayModeSwitch playMode;
    IconButton rewindButton { "", Icon::rewind },
               playButton   { "", Icon::play },
               stopButton   { "", Icon::stop },
               forwardButton { "", Icon::fastForward };
    NumberDisplay barDisplay { 3 };
    juce::ToggleButton loopCheck { "LOOP" };
    NumberDisplay loopStartDisplay { 3 }, loopLengthDisplay { 3 };
    IconButton undoButton { "", Icon::undo }, redoButton { "", Icon::redo };
    IconButton mixerButton { "Mixer", Icon::mixer };
    juce::Slider masterKnob;
    LevelMeterView masterMeter { LevelMeterView::Orientation::horizontal };
    juce::Label documentLabel;

    // Laid out by resized(), drawn by paint(): the recessed group panels, the
    // small captions inside them, and the dividers between.
    std::vector<juce::Rectangle<int>> panels;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> captions;
    std::vector<int> dividers;
};

} // namespace carve::app
