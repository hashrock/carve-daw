#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "TimeSigSupport.h"
#include "model/SongModel.h"

namespace carve::app
{

// Properties of the playlist clips that are currently selected.
//
// For a pattern placement this is where its own length and transpose are
// edited, and it is also the only place the *pattern's* length can be changed
// now that dragging a clip's right edge is gone -- that gesture silently
// rewrote every placement of the pattern, which the panel can at least say
// out loud.
//
// Both lengths are shown in bars, but they are bars of different things. A
// clip is a placement: it starts at a known beat, so it can be measured along
// the song's own bar map and reads as the number of bars it actually covers,
// even across a signature change. A pattern is not anywhere -- the same one
// can be placed under 4/4 and under 7/8 -- so its bars are only the reading at
// the selected clip, and the panel prints the beat count the model really
// stores next to it, plus a warning when other placements would read
// differently.
//
// An audio placement is a different node with different properties (see
// AudioClip), so it gets a different set of controls: the file it plays,
// where it starts in beats, how much of the file it plays in seconds, and
// the two things that can go wrong with a file on disk -- it changed (Reload)
// or it moved (Relocate). A selection holding both kinds shows the pattern
// clips; the audio ones still move, copy and delete with the rest.
class ClipPropertiesPanel : public juce::Component,
                            private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    explicit ClipPropertiesPanel (juce::UndoManager&);
    ~ClipPropertiesPanel() override;

    void setSong (model::Song newSong);
    void setSelection (const std::vector<model::PlaylistClip>& clips,
                       const std::vector<model::AudioClip>& audioClips);

    // The Reload button: the host owns the engine sync, which is what has to
    // re-read the file. Nothing in the model changes, so this is a request
    // rather than an edit.
    std::function<void (const juce::String& placementId)> onReloadAudioClip;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

    // Where a beat falls in the song's bars, as a continuous coordinate: bar
    // index plus the fraction of that bar already gone. Subtracting two of
    // these is what makes a clip's length in bars come out right when a
    // signature change falls inside it, and beatAtBarPosition() is the inverse
    // the spinner needs to write a bar count back as beats.
    double barPositionOf (double beat) const;
    double beatAtBarPosition (double barPosition) const;

    // The signature the selected clip sits in, which is the one its pattern's
    // length is read in. 4/4 with nothing selected.
    model::TimeSignature selectedTimeSig() const;

    // Whether the pattern of the selected clip is placed under more than one
    // signature, i.e. whether "N bars" is only true of the clip in front of us.
    bool selectedPatternSpansSignatures() const;

    // Drops clips that have since been deleted or whose pattern has gone.
    std::vector<model::PlaylistClip> liveSelection() const;
    std::vector<model::AudioClip> liveAudioSelection() const;
    std::optional<model::Pattern> patternFor (const model::PlaylistClip&) const;
    int countPlacementsOfSelectedPattern() const;

    // How long the selected clip's file is, for the note under the length
    // spinner. Reading a header is cheap but refresh() runs on every song
    // change, so the answer is kept until the file itself changes.
    double fileLengthSeconds (const juce::File&);

    void refresh();
    void refreshPatternClip (const std::vector<model::PlaylistClip>&);
    void refreshAudioClip (const std::vector<model::AudioClip>&);
    void applyClipLength();
    void applyTranspose();
    void applyPatternLength();
    void applyAudioStart();
    void applyAudioLength();
    void reloadAudioFile();
    void relocateAudioFile();

    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("Untitled") };
    std::vector<model::PlaylistClip> selection;
    std::vector<model::AudioClip> audioSelection;

    // The pattern's name, the audio file's name, or "No clip selected".
    juce::Label titleLabel;

    juce::Label clipHeader, lengthLabel, transposeLabel,
                patternHeader, patternLengthLabel, loopLabel, patternLengthNote;
    juce::Slider lengthSlider, transposeSlider, patternLengthSlider;
    juce::TextButton resetLengthButton { "Match pattern" };

    juce::Label audioHeader, missingLabel, startLabel, audioLengthLabel, audioLengthNote;
    juce::Slider startSlider, audioLengthSlider;
    juce::TextButton reloadButton { "Reload" }, relocateButton { "Relocate..." };
    std::shared_ptr<juce::FileChooser> chooser;

    juce::File cachedLengthFile;
    juce::Time cachedLengthStamp;
    double cachedLengthSeconds = 0.0;

    // Serves tooltips for this panel's own controls only -- the full path
    // behind a file name that never fits in 210 pixels. The app has no
    // tooltip window of its own, and a general one here would start showing
    // tips on every button that has ever been given one.
    class PanelTooltipWindow : public juce::TooltipWindow
    {
    public:
        explicit PanelTooltipWindow (juce::Component& owner) : panel (owner) {}

        juce::String getTipFor (juce::Component& c) override
        {
            return panel.isParentOf (&c) ? juce::TooltipWindow::getTipFor (c) : juce::String();
        }

    private:
        juce::Component& panel;
    };

    PanelTooltipWindow tooltipWindow { *this };

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipPropertiesPanel)
};

} // namespace carve::app
