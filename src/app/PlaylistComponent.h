#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace orionish::app
{

// Song timeline: one row per Generator, pattern placements as blocks.
// Click an empty spot to place the row's current pattern (bar-snapped);
// click a block to remove it. Clicking a row label selects that generator.
class PlaylistComponent : public juce::Component,
                          private juce::ValueTree::Listener
{
public:
    explicit PlaylistComponent (juce::UndoManager& um);
    ~PlaylistComponent() override;

    std::function<void (const juce::String&)> onSelectGenerator;

    void setSong (model::Song newSong);
    void setSelection (const juce::String& generatorId, const juce::String& patternId);
    void setPlayheadBeats (double beats);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int labelWidth = 120;
    static constexpr int rowHeight = 30;
    static constexpr double pixelsPerBeat = 14.0;
    static constexpr double snapBeats = 4.0;   // place on bar boundaries

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { refresh(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { refresh(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { refresh(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void refresh();
    void updateSize();
    double getContentLengthBeats() const;
    float beatToX (double beat) const  { return (float) (labelWidth + beat * pixelsPerBeat); }
    double xToBeat (float x) const     { return (x - (float) labelWidth) / pixelsPerBeat; }

    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("empty") };

    juce::String selectedGeneratorId, selectedPatternId;
    double playheadBeats = 0.0;
};

} // namespace orionish::app
