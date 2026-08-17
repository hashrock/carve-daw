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
//   click empty cell          add a note (snapped, if snapping is on)
//   drag a note               move it (pitch + start), keeping the grab point
//   drag a note's right edge  resize it
//   right/alt-drag            erase every note the cursor sweeps over
class PianoRollComponent : public juce::Component,
                           private juce::ValueTree::Listener
{
public:
    explicit PianoRollComponent (juce::UndoManager& um);
    ~PianoRollComponent() override;

    void setPattern (std::optional<model::Pattern> newPattern);

    // View settings, driven by the toolbar above the roll. The grid unit and
    // the snap flag describe how the song is *edited*, not what it is, so
    // neither of them touches the model.
    void setGridBeats (double beats);
    void setSnapEnabled (bool shouldSnap);
    double getGridBeats() const  { return gridBeats; }
    bool isSnapEnabled() const   { return snapEnabled; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;

private:
    static constexpr int keyboardWidth = 48;
    static constexpr int rowHeight = 12;
    static constexpr int lowestPitch = 24;    // C1
    static constexpr int highestPitch = 96;   // C7
    static constexpr double pixelsPerBeat = 96.0;
    static constexpr double beatsPerBar = 4.0;      // no time signature in the model yet
    static constexpr float resizeZoneWidth = 6.0f;

    // Shortest note the mouse can produce with snapping off. Small enough to
    // feel free, large enough to stay clickable.
    static constexpr double freeMinLengthBeats = 1.0 / 32.0;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { patternChanged(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { patternChanged(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { patternChanged(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { patternChanged(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void patternChanged();
    void updateSize();
    double xToBeat (float x) const;
    float beatToX (double beat) const;
    int yToPitch (float y) const;
    float pitchToY (int pitch) const;
    juce::Rectangle<float> noteBounds (const model::Note&) const;
    std::optional<model::Note> noteAt (juce::Point<float>) const;
    bool isOverResizeZone (const model::Note&, juce::Point<float>) const;

    double snapDown (double beat) const;
    double snapUp (double beat) const;
    double minLengthBeats() const;

    static bool isEraseGesture (const juce::ModifierKeys& mods)  { return mods.isRightButtonDown() || mods.isAltDown(); }
    juce::MouseCursor cursorFor (juce::Point<float>, const juce::ModifierKeys&) const;
    void updateCursor (const juce::MouseEvent&);

    void eraseAt (juce::Point<float>);
    void eraseAlong (juce::Point<float> from, juce::Point<float> to);

    juce::UndoManager& undoManager;
    std::optional<model::Pattern> pattern;

    double gridBeats = 0.25;   // 16th-note grid
    bool snapEnabled = true;

    enum class DragMode { none, move, resize, erase };
    DragMode dragMode = DragMode::none;
    std::optional<model::Note> draggedNote;
    double grabOffsetBeats = 0.0;
    int grabPitchOffset = 0;                 // note pitch minus the pitch under the cursor
    juce::Point<float> lastErasePosition;
    double lastNoteLength = 0.5;
};

} // namespace orionish::app
