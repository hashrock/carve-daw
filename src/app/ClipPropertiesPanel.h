#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace carve::app
{

// Properties of the playlist clips that are currently selected.
//
// This is where a placement's own length and transpose are edited, and it is
// also the only place the *pattern's* length can be changed now that dragging
// a clip's right edge is gone -- that gesture silently rewrote every placement
// of the pattern, which the panel can at least say out loud.
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
    static constexpr double beatsPerBar = 4.0;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

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
                patternHeader, patternLengthLabel, loopLabel;
    juce::Slider lengthSlider, transposeSlider, patternLengthSlider;
    juce::TextButton resetLengthButton { "Match pattern" };

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipPropertiesPanel)
};

} // namespace carve::app
