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
//   select  left-drag on empty space rubber-band selects
// In both tools a left click on a clip selects it (⌘ or ⇧ extends the
// selection) and dragging it moves it in time; a double click asks the host to
// open that pattern in the editor; right-drag (or alt-drag) erases.
//
// A clip also carries two handles: the chevron in its top-right corner swaps
// which of the generator's patterns it plays, and its right edge resizes the
// *pattern*, i.e. every placement of it at once.
//
// The header band holds the tool strip, the zoom control and the bar ruler.
// Dragging the ruler sets the song's loop range.
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
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    static constexpr int labelWidth = 120;
    static constexpr int rowHeight = 44;
    // Band above the rows: a strip of controls with the bar ruler under it.
    // Both are pinned to the top of the visible area, so the rows start below
    // the pair of them.
    static constexpr int toolbarHeight = 26;
    static constexpr int rulerHeight = 18;
    static constexpr int headerHeight = toolbarHeight + rulerHeight;
    static constexpr double snapBeats = 4.0;   // place on bar boundaries

    // Zoom limits, in pixels per beat. The bottom of the range is where a
    // 4-beat bar is still ~12px wide, which is about as far out as the grid
    // stays meaningful; the top is roughly one bar per screenful of a clip.
    static constexpr double defaultPixelsPerBeat = 14.0;
    static constexpr double minPixelsPerBeat = 3.0;
    static constexpr double maxPixelsPerBeat = 64.0;

    // Grab zone on a clip's right edge, and the size of its pattern chevron.
    static constexpr float resizeHandleWidth = 6.0f;
    static constexpr float menuButtonWidth = 15.0f;

    enum class DragMode
    {
        none,
        paint,
        erase,
        move,
        loopRange,      // on the ruler
        patternLength,  // on a clip's right edge
        rubberBand      // select tool on empty space
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

    // The one funnel between beats and pixels: zoom only has to change
    // pixelsPerBeat for the grid, the clips, the ruler and every hit test to
    // follow.
    float beatToX (double beat) const  { return (float) ((double) labelWidth + beat * pixelsPerBeat); }
    double xToBeat (float x) const     { return ((double) x - (double) labelWidth) / pixelsPerBeat; }
    float rowY (int row) const         { return (float) (headerHeight + row * rowHeight); }
    int yToRow (float y) const         { return (int) std::floor ((y - (float) headerHeight) / (float) rowHeight); }
    static double snapToBar (double beat)  { return std::max (0.0, std::floor (beat / snapBeats) * snapBeats); }
    // Lengths round to the nearest bar (never to zero) so a resized edge
    // follows the pointer instead of always trailing it.
    static double snapLengthToBar (double beats)
    {
        return std::max (snapBeats, std::round (beats / snapBeats) * snapBeats);
    }

    juce::Viewport* getViewport() const;
    void setPixelsPerBeat (double newPixelsPerBeat, float anchorX);
    void zoomBy (double factor);

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

    juce::Rectangle<float> toolbarBounds() const  { return headerBounds().withHeight ((float) toolbarHeight); }
    juce::Rectangle<float> rulerBounds() const    { return headerBounds().withTrimmedTop ((float) toolbarHeight); }

    std::optional<model::Pattern> patternForRow (const model::Generator&) const;
    std::optional<model::PlaylistClip> clipAt (const model::Generator&, double beat) const;
    juce::Rectangle<float> slotBounds (int row, double startBeats, double lengthBeats) const;
    std::optional<juce::Rectangle<float>> boundsForClip (int row, const model::PlaylistClip&) const;
    bool isRangeFree (const model::Generator&, double startBeats, double lengthBeats,
                      bool ignoreSelectedClips = false,
                      const juce::ValueTree& alsoIgnore = {}) const;

    // The two handles a clip carries, given the clip's painted bounds. Both are
    // empty when the clip is too narrow to hold them.
    juce::Rectangle<float> patternMenuBounds (juce::Rectangle<float> clipRect) const;
    juce::Rectangle<float> resizeHandleBounds (juce::Rectangle<float> clipRect) const;

    bool paintClip (int row, double beat);
    bool eraseClip (int row, double beat);
    void duplicateSelection();
    void deleteSelection();
    void dragSelectionTo (double targetStart);

    void showPatternMenu (const model::PlaylistClip&, juce::Rectangle<float> buttonBounds);
    int countPlacements (const juce::String& generatorId, const juce::String& patternId) const;
    double maxLengthForPattern (const model::Generator&, const model::Pattern&) const;
    void resizePatternTo (double newLengthBeats);

    void dragLoopTo (double beat);
    void updateRubberBand (juce::Point<float> position);

    bool isSelected (const juce::ValueTree& clip) const;
    void selectClip (const model::PlaylistClip&, bool extend);
    void clearSelection();
    void pruneSelection();

    void paintRuler (juce::Graphics&);

    juce::MouseCursor cursorFor (juce::Point<float> position, juce::ModifierKeys mods) const;
    void updateHover (juce::Point<float> position);

    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("empty") };

    juce::String selectedGeneratorId, selectedPatternId;
    double playheadBeats = 0.0;
    double pixelsPerBeat = defaultPixelsPerBeat;

    Tool tool = Tool::paint;
    juce::TextButton paintToolButton { "Paint" }, selectToolButton { "Select" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" };

    // The clips the user has selected, as the CLIP trees themselves: a
    // ValueTree compares by identity, so this survives any edit that does not
    // remove the clip and needs no ids in the model.
    std::vector<juce::ValueTree> selectedClips;

    DragMode dragMode = DragMode::none;
    double dragGrabOffsetBeats = 0.0;    // where in the grabbed clip the pointer went down
    double dragAnchorOriginStart = 0.0;  // that clip's start when the drag began
    double dragLastStart = 0.0;          // last start we wrote, so a drag only writes on a change
    std::vector<double> dragOriginStarts;   // parallel to selectedClips

    // Loop drag: the end of the range that stays put, plus whether the pointer
    // ever left the bar it went down in (a click without a drag clears).
    double loopAnchorBeats = 0.0;
    bool loopDragMoved = false;

    // Pattern-length drag: the PATTERN being resized, where the held clip
    // starts, and the ceiling that keeps its placements from swallowing their
    // neighbours.
    juce::ValueTree resizePattern;
    double resizeStartBeats = 0.0;
    double resizeMaxLength = 0.0;

    // Rubber band, plus the selection it started from so ⌘/⇧ adds to it.
    juce::Rectangle<float> rubberBand;
    juce::Point<float> rubberBandAnchor;
    std::vector<juce::ValueTree> rubberBandBaseSelection;

    // Where the pointer is, and the bar the paint tool would fill from there.
    // Kept snapped so hovering only repaints when crossing into another slot.
    juce::Point<float> lastMousePosition;
    bool mouseIsOver = false;
    int hoverRow = -1;
    double hoverStartBeats = 0.0;

    // The pattern whose length the pointer is about to change (or is
    // changing). Every placement of it is outlined while this is set, so the
    // "this edits all of them" part is visible before the drag, not after.
    juce::String lengthHintGeneratorId, lengthHintPatternId;
    void setLengthHint (const juce::String& generatorId, const juce::String& patternId);

    const juce::MouseCursor eraseCursor;
};

} // namespace orionish::app
