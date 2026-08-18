#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "TimeSigSupport.h"
#include "model/SongModel.h"

namespace carve::app
{

// Properties of the playlist clips that are currently selected.
//
// This is where a placement's own length and transpose are edited, and it is
// also the only place the *pattern's* length can be changed now that dragging
// a clip's right edge is gone -- that gesture silently rewrote every placement
// of the pattern, which the panel can at least say out loud.
//
// Both lengths are shown in bars, but they are bars of different things. A
// clip is a placement: it starts at a known beat, so it can be measured along
// the song's own bar map and reads as the number of bars it actually covers,
// even across a signature change. A pattern is not anywhere -- the same one
// can be placed under 4/4 and under 7/8 -- so its bars are only the reading at
// the selected clip, and the panel prints the beat count the model really
// stores next to it, plus a warning when other placements would read
// differently.
class ClipPropertiesPanel : public juce::Component,
                            private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    explicit ClipPropertiesPanel (juce::UndoManager&);
    ~ClipPropertiesPanel() override;

    void setSong (model::Song newSong);
    void setSelection (const std::vector<model::PlaylistClip>& clips);

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
    std::optional<model::Pattern> patternFor (const model::PlaylistClip&) const;
    int countPlacementsOfSelectedPattern() const;

    void refresh();
    void applyClipLength();
    void applyTranspose();
    void applyPatternLength();

    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("Untitled") };
    std::vector<model::PlaylistClip> selection;

    juce::Label patternLabel, clipHeader, lengthLabel, transposeLabel,
                patternHeader, patternLengthLabel, loopLabel, patternLengthNote;
    juce::Slider lengthSlider, transposeSlider, patternLengthSlider;
    juce::TextButton resetLengthButton { "Match pattern" };

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipPropertiesPanel)
};

} // namespace carve::app
