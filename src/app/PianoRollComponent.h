#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace orionish::app
{

// Piano roll editor for one Pattern. Sized to its content; put it in a
// Viewport.
//
// Two tools, picked from the strip in the toolbar above the roll (or with
// D / E):
//   draw    click empty grid to add a note
//   select  drag empty grid to rubber-band notes
// In both tools a left click on a note selects it (Cmd or Shift extends the
// selection) and dragging it moves the whole selection, a note's right edge
// resizes it, right/alt-drag erases every note the cursor sweeps over, and
// Backspace deletes the selection.
//
// Cmd-scroll zooms in time about the pointer.
//
// Selection is pure UI state: it lives here and never reaches the model, so
// selecting never dirties the document or triggers an EditSync resync.
class PianoRollComponent : public juce::Component,
                           private juce::ValueTree::Listener
{
public:
    enum class Tool
    {
        draw,
        select
    };

    // Horizontal geometry. Public because the ruler above the roll has to line
    // up with the grid to the pixel, and the only way to guarantee that is to
    // have both of them do the same arithmetic.
    static constexpr int keyboardWidth = 48;
    static constexpr double beatsPerBar = 4.0;      // no time signature in the model yet

    explicit PianoRollComponent (juce::UndoManager& um);
    ~PianoRollComponent() override;

    void setPattern (std::optional<model::Pattern> newPattern);

    double xToBeat (float x) const;
    float beatToX (double beat) const;

    // Length of the pattern being edited, or of the placeholder grid shown
    // when there is none.
    double getLengthBeats() const;

    // Fired when a gesture asks for the pitch under the cursor to be heard:
    // clicking an existing note, creating one, or dragging one onto a new row.
    // Left unset the roll stays silent; the owner wires this to the engine.
    std::function<void (int pitch, int velocity)> onPreviewNote;

    // Fired when the zoom changed, so the ruler can redraw at the new scale:
    // it derives its geometry from ours, and nothing else tells it.
    std::function<void()> onViewChanged;

    // Fired when anything the shortcut help bar describes changed - the tool,
    // the selection, or a held modifier that changes what a drag would do.
    std::function<void()> onShortcutContextChanged;

    // View settings, driven by the toolbar above the roll. The grid unit and
    // the snap flag describe how the song is *edited*, not what it is, so
    // neither of them touches the model.
    void setGridBeats (double beats);
    void setSnapEnabled (bool shouldSnap);
    double getGridBeats() const  { return gridBeats; }
    bool isSnapEnabled() const   { return snapEnabled; }

    void setTool (Tool newTool);
    Tool getTool() const  { return tool; }
    int getNumSelectedNotes() const  { return (int) selectedNotes.size(); }

    // Zoom about the middle of what is on screen; the pointer-anchored version
    // is the cmd-scroll gesture and stays private.
    void zoomBy (double factor);
    double getPixelsPerBeat() const  { return pixelsPerBeat; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    static constexpr int rowHeight = 12;
    static constexpr int lowestPitch = 24;    // C1
    static constexpr int highestPitch = 96;   // C7
    static constexpr float resizeZoneWidth = 6.0f;
    static constexpr double defaultLengthBeats = 16.0;   // grid shown with no pattern loaded
    static constexpr int newNoteVelocity = 100;

    // Zoom limits, in pixels per beat. The default is what the roll used to be
    // fixed at; the bottom of the range is where a bar is still ~24px wide,
    // which is about as far out as the grid stays meaningful, and the top is
    // roughly one bar per screenful.
    static constexpr double defaultPixelsPerBeat = 96.0;
    static constexpr double minPixelsPerBeat = 6.0;
    static constexpr double maxPixelsPerBeat = 512.0;

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
    int yToPitch (float y) const;
    float pitchToY (int pitch) const;
    juce::Rectangle<float> noteBounds (const model::Note&) const;
    std::optional<model::Note> noteAt (juce::Point<float>) const;
    bool isOverResizeZone (const model::Note&, juce::Point<float>) const;

    double snapDown (double beat) const;
    double snapUp (double beat) const;
    double minLengthBeats() const;

    juce::Viewport* getViewport() const;
    void setPixelsPerBeat (double newPixelsPerBeat, float anchorX);

    static bool isEraseGesture (const juce::ModifierKeys& mods)  { return mods.isRightButtonDown() || mods.isAltDown(); }
    juce::MouseCursor cursorFor (juce::Point<float>, const juce::ModifierKeys&) const;
    void updateCursor (const juce::MouseEvent&);

    void eraseAt (juce::Point<float>);
    void eraseAlong (juce::Point<float> from, juce::Point<float> to);

    // Selection (UI state only)
    bool isSelected (const juce::ValueTree& note) const;
    void selectNote (const model::Note&, bool extend);
    void setSelection (std::vector<juce::ValueTree> newSelection);
    void clearSelection();
    void pruneSelection();
    void beginSelectionDrag();
    void dragSelectionTo (double anchorStart, int anchorPitch);
    void deleteSelection();
    void updateRubberBand (juce::Point<float> position);

    void notifyShortcutContext() const
    {
        if (onShortcutContextChanged)
            onShortcutContextChanged();
    }

    void previewNote (int pitch, int velocity) const
    {
        if (onPreviewNote)
            onPreviewNote (pitch, velocity);
    }

    juce::UndoManager& undoManager;
    std::optional<model::Pattern> pattern;

    double gridBeats = 0.25;   // 16th-note grid
    bool snapEnabled = true;
    double pixelsPerBeat = defaultPixelsPerBeat;
    Tool tool = Tool::draw;

    enum class DragMode { none, move, resize, erase, rubberBand };
    DragMode dragMode = DragMode::none;
    std::optional<model::Note> draggedNote;
    double grabOffsetBeats = 0.0;
    int grabPitchOffset = 0;                 // note pitch minus the pitch under the cursor
    juce::Point<float> lastErasePosition;
    double lastNoteLength = 0.5;

    // The notes the user has selected, as the NOTE trees themselves: a
    // ValueTree compares by identity, so this survives any edit that does not
    // remove the note and needs no ids in the model.
    std::vector<juce::ValueTree> selectedNotes;

    // Move drag: where the whole selection started, so every move is measured
    // from the beginning of the gesture rather than from the last event.
    std::vector<double> dragOriginStarts;    // parallel to selectedNotes
    std::vector<int> dragOriginPitches;      // ditto
    double dragAnchorOriginStart = 0.0;
    int dragAnchorOriginPitch = 0;
    int dragLastPitch = 0;                   // last pitch previewed during the drag

    // Rubber band, plus the selection it started from so Cmd/Shift adds to it.
    juce::Rectangle<float> rubberBand;
    juce::Point<float> rubberBandAnchor;
    std::vector<juce::ValueTree> rubberBandBaseSelection;
};

// Bar/beat ruler drawn above the roll.
//
// It is a sibling of the roll's Viewport rather than a strip inside the roll,
// because it has to stay put while the roll scrolls vertically. Alignment is
// kept by drawing in the roll's own coordinates shifted by the viewport's
// horizontal scroll offset, so the two cannot disagree about where a beat is;
// the owner feeds it that offset whenever the visible area moves.
class PianoRollRuler : public juce::Component
{
public:
    explicit PianoRollRuler (const PianoRollComponent& rollToFollow);

    static constexpr int preferredHeight = 20;

    // x coordinate of the roll that sits at this component's left edge
    void setScrollOffset (int offsetX);

    void paint (juce::Graphics&) override;

private:
    const PianoRollComponent& roll;
    int scrollOffset = 0;
};

} // namespace orionish::app
