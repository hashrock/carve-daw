#pragma once

#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace orionish::app
{

// Song timeline: one row per Generator, pattern placements as blocks.
//
// Two tools, picked from the strip in the header band (or with B / E):
//   paint   left-drag lays the row's current pattern into empty bars
//   select  left-drag on empty space does nothing (rubber band goes here later)
// In both tools a left click on a clip selects it (⌘ or ⇧ extends the
// selection) and dragging it moves it in time; a double click asks the host to
// open that pattern in the editor; right-drag (or alt-drag) erases.
//
// Selection is pure UI state: it lives here and never reaches the model, so
// selecting never dirties the document or triggers an EditSync resync.
class PlaylistComponent : public juce::Component,
                          private juce::ValueTree::Listener
{
public:
    enum class Tool
    {
        paint,
        select
    };

    explicit PlaylistComponent (juce::UndoManager& um);
    ~PlaylistComponent() override;

    std::function<void (const juce::String&)> onSelectGenerator;

    // A clip was double-clicked: (generatorId, patternId) of the pattern the
    // host should open in the pattern editor.
    std::function<void (const juce::String&, const juce::String&)> onEditPattern;

    void setSong (model::Song newSong);
    void setSelection (const juce::String& generatorId, const juce::String& patternId);
    void setPlayheadBeats (double beats);

    void setTool (Tool newTool);
    Tool getTool() const  { return tool; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void moved() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    static constexpr int labelWidth = 120;
    static constexpr int rowHeight = 44;
    // Band above the rows holding the tool strip. The ruler and the loop
    // markers will live here too, so the rows already start below it.
    static constexpr int headerHeight = 26;
    static constexpr double pixelsPerBeat = 14.0;
    static constexpr double snapBeats = 4.0;   // place on bar boundaries

    enum class DragMode
    {
        none,
        paint,
        erase,
        move
    };

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { refresh(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { refresh(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { refresh(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void refresh();
    void updateSize();
    void layOutToolStrip();
    double getContentLengthBeats() const;

    float beatToX (double beat) const  { return (float) (labelWidth + beat * pixelsPerBeat); }
    double xToBeat (float x) const     { return (x - (float) labelWidth) / pixelsPerBeat; }
    float rowY (int row) const         { return (float) (headerHeight + row * rowHeight); }
    int yToRow (float y) const         { return (int) std::floor ((y - (float) headerHeight) / (float) rowHeight); }
    static double snapToBar (double beat)  { return std::max (0.0, std::floor (beat / snapBeats) * snapBeats); }

    // Top-left of the part of us the viewport is showing, in our own
    // coordinates. A scrolled Viewport moves its content to a negative
    // position, so this pins the header band on screen while the grid scrolls.
    juce::Point<int> visibleOrigin() const  { return { std::max (0, -getX()), std::max (0, -getY()) }; }

    // Where the pinned band actually sits. Everything that hit-tests the grid
    // has to exclude this, not just the reserved strip at the very top.
    juce::Rectangle<float> headerBounds() const
    {
        return { 0.0f, (float) visibleOrigin().y, (float) getWidth(), (float) headerHeight };
    }

    std::optional<model::Pattern> patternForRow (const model::Generator&) const;
    std::optional<model::PlaylistClip> clipAt (const model::Generator&, double beat) const;
    juce::Rectangle<float> slotBounds (int row, double startBeats, double lengthBeats) const;
    bool isRangeFree (const model::Generator&, double startBeats, double lengthBeats,
                      bool ignoreSelectedClips = false) const;

    bool paintClip (int row, double beat);
    bool eraseClip (int row, double beat);
    void duplicateSelection();
    void deleteSelection();
    void dragSelectionTo (double targetStart);

    bool isSelected (const juce::ValueTree& clip) const;
    void selectClip (const model::PlaylistClip&, bool extend);
    void clearSelection();
    void pruneSelection();

    juce::MouseCursor cursorFor (juce::Point<float> position, juce::ModifierKeys mods) const;
    void updateHover (juce::Point<float> position);

    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("empty") };

    juce::String selectedGeneratorId, selectedPatternId;
    double playheadBeats = 0.0;

    Tool tool = Tool::paint;
    juce::TextButton paintToolButton { "Paint" }, selectToolButton { "Select" };

    // The clips the user has selected, as the CLIP trees themselves: a
    // ValueTree compares by identity, so this survives any edit that does not
    // remove the clip and needs no ids in the model.
    std::vector<juce::ValueTree> selectedClips;

    DragMode dragMode = DragMode::none;
    double dragGrabOffsetBeats = 0.0;    // where in the grabbed clip the pointer went down
    double dragAnchorOriginStart = 0.0;  // that clip's start when the drag began
    double dragLastStart = 0.0;          // last start we wrote, so a drag only writes on a change
    std::vector<double> dragOriginStarts;   // parallel to selectedClips

    // Where the pointer is, and the bar the paint tool would fill from there.
    // Kept snapped so hovering only repaints when crossing into another slot.
    juce::Point<float> lastMousePosition;
    bool mouseIsOver = false;
    int hoverRow = -1;
    double hoverStartBeats = 0.0;

    const juce::MouseCursor eraseCursor;
};

} // namespace orionish::app
