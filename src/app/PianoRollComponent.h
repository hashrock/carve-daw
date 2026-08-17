#pragma once

#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace orionish::app
{

// Piano roll editor for one Pattern. Sized to its content; put it in a
// Viewport.
//
// Interactions:
//   click empty cell        add a note (grid-snapped)
//   drag a note             move it (pitch + start)
//   drag a note's right edge  resize it
//   right/alt-click a note  delete it
class PianoRollComponent : public juce::Component,
                           private juce::ValueTree::Listener
{
public:
    explicit PianoRollComponent (juce::UndoManager& um);
    ~PianoRollComponent() override;

    void setPattern (std::optional<model::Pattern> newPattern);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    static constexpr int keyboardWidth = 48;
    static constexpr int rowHeight = 12;
    static constexpr int lowestPitch = 24;    // C1
    static constexpr int highestPitch = 96;   // C7
    static constexpr double pixelsPerBeat = 96.0;
    static constexpr double gridBeats = 0.25;  // 16th-note grid

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { repaint(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { repaint(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void updateSize();
    double xToBeat (float x) const;
    float beatToX (double beat) const;
    int yToPitch (float y) const;
    float pitchToY (int pitch) const;
    juce::Rectangle<float> noteBounds (const model::Note&) const;
    std::optional<model::Note> noteAt (juce::Point<float>) const;
    static double snap (double beat)  { return std::floor (beat / gridBeats) * gridBeats; }

    juce::UndoManager& undoManager;
    std::optional<model::Pattern> pattern;

    enum class DragMode { none, move, resize };
    DragMode dragMode = DragMode::none;
    std::optional<model::Note> draggedNote;
    double grabOffsetBeats = 0.0;
    double lastNoteLength = 0.5;
};

} // namespace orionish::app
