#pragma once

#include <map>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ShortcutHelpBar.h"
#include "model/SongModel.h"

namespace carve::app
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
// A clip carries one handle: the chevron in its top-right corner swaps which of
// the generator's patterns it plays. How long the placement runs for is a
// property of the clip, not a gesture here - a clip shorter than its pattern is
// cut off, a longer one repeats it, and the repeats are drawn as dividers.
//
// An "audio" generator's row holds files rather than patterns. Those are drawn
// as their waveform, they have no pattern to swap and no transpose (that would
// mean pitch-shifting), and their one handle is instead a grip on the right
// edge that trims the placement. Dropping audio files anywhere on the grid
// places them: onto an audio row when the drop lands on one, onto a new audio
// generator otherwise.
//
// The header band holds the tool strip, the zoom control and the bar ruler.
// Dragging the ruler sets the song's loop range.
//
// Selection is pure UI state: it lives here and never reaches the model, so
// selecting never dirties the document or triggers an EditSync resync.
class PlaylistComponent : public juce::Component,
                          public juce::FileDragAndDropTarget,
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

    // Fired whenever the set of selected clips changes; empty when nothing is selected.
    std::function<void (const std::vector<model::PlaylistClip>&)> onClipSelectionChanged;

    // The shortcuts that apply right now, for whichever help bar the host owns.
    // Only ours: the host appends its own global ones.
    std::function<void (std::vector<ShortcutHelpBar::Entry>)> onShortcutHelpChanged;

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

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

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

    // The pattern chevron in a clip's top-right corner, and its inset from the
    // clip's edge.
    static constexpr float menuButtonWidth = 15.0f;
    static constexpr float menuButtonInset = 3.0f;

    // The grip on an audio clip's right edge that trims it. Narrow, because it
    // sits inside the clip and everything to its left still drags the clip.
    static constexpr float trimHandleWidth = 6.0f;

    enum class DragMode
    {
        none,
        paint,
        erase,
        move,
        trim,        // an audio clip's right edge
        loopRange,   // on the ruler
        rubberBand   // select tool on empty space
    };

    // A placement on the grid, whichever node type it is. The two kinds differ
    // in almost everything they draw and almost nothing about how they are
    // hit-tested, selected, moved or deleted, so all of that goes through this
    // rather than branching on the type in a dozen places.
    struct Placement
    {
        juce::ValueTree state;
        double start = 0.0;
        double length = 0.0;

        bool isAudio() const  { return state.hasType (model::ids::AUDIOCLIP); }
        double getEnd() const  { return start + length; }
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

    // Resolves a playlist node into a Placement. Everything that draws,
    // hit-tests, moves or measures a clip has to go through this, or a clip
    // whose length isn't its pattern's ends up drawn in one place and clicked
    // in another. Empty when the clip's generator, or the pattern it names,
    // has gone.
    std::optional<Placement> placementFor (const juce::ValueTree&) const;
    std::vector<Placement> placementsFor (const model::Generator&) const;
    std::optional<Placement> placementAt (const model::Generator&, double beat) const;

    juce::Rectangle<float> slotBounds (int row, double startBeats, double lengthBeats) const;
    std::optional<juce::Rectangle<float>> boundsForClip (int row, const juce::ValueTree&) const;
    bool isRangeFree (const model::Generator&, double startBeats, double lengthBeats,
                      bool ignoreSelectedClips = false,
                      const juce::ValueTree& alsoIgnore = {}) const;

    // The one handle a pattern clip carries, given the clip's painted bounds,
    // and the one an audio clip carries instead. Both are empty when the clip
    // is too narrow to hold them.
    juce::Rectangle<float> patternMenuBounds (juce::Rectangle<float> clipRect) const;
    juce::Rectangle<float> trimHandleBounds (juce::Rectangle<float> clipRect) const;

    bool paintClip (int row, double beat);
    bool eraseClip (int row, double beat);
    void trimSelectionTo (double targetEnd);

    // The two writes that have to know which kind of placement they are
    // holding, so that nothing else does.
    void removePlacement (const juce::ValueTree&);
    void setPlacementStart (const juce::ValueTree&, double startBeats);
    void duplicateSelection();
    void deleteSelection();
    void dragSelectionTo (double targetStart);

    void showPatternMenu (const model::PlaylistClip&, juce::Rectangle<float> buttonBounds);

    void dragLoopTo (double beat);
    void updateRubberBand (juce::Point<float> position);

    bool isSelected (const juce::ValueTree& clip) const;
    void selectClip (const juce::ValueTree&, bool extend);
    void clearSelection();
    void pruneSelection();

    // The single funnel for "the selection may have moved": repaints and tells
    // the host, but only when the set actually differs from what it last heard.
    void selectionChanged();
    void updateShortcutHelp();

    void paintRuler (juce::Graphics&);
    void paintAudioClip (juce::Graphics&, const model::AudioClip&, juce::Rectangle<float>,
                         juce::Colour, bool selected);

    // Min/max peaks for one audio file, at a fixed resolution over its whole
    // length. Reading a file is far too slow to do inside paint(), and every
    // placement of the same file draws from the same peaks, so they are read
    // once and kept. Empty for anything that could not be read.
    struct WaveformPeaks
    {
        std::vector<float> minima, maxima;   // parallel, one entry per bucket
        double lengthSeconds = 0.0;
    };

    static constexpr int waveformBuckets = 1024;

    const WaveformPeaks& peaksFor (const juce::File&);

    // Where a file drag would land, so the drop can be previewed. An audio row
    // takes the files; anything else means a new generator, which is drawn as
    // a strip under the last row.
    struct FileDropTarget
    {
        int row = -1;        // the audio row the files land on; -1 means a new generator
        int ghostRow = 0;    // which row to preview on, which is the pointer's when row is -1
        double startBeats = 0.0;
    };

    std::optional<FileDropTarget> fileDropTargetFor (juce::Point<float> position) const;

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

    // What the host was last told, so a gesture that recomputes the same
    // selection every mouse move (the rubber band) only notifies on a change.
    std::vector<juce::ValueTree> notifiedSelection;
    std::vector<ShortcutHelpBar::Entry> notifiedShortcuts;

    DragMode dragMode = DragMode::none;
    double dragGrabOffsetBeats = 0.0;    // where in the grabbed clip the pointer went down
    double dragAnchorOriginStart = 0.0;  // that clip's start when the drag began
    double dragLastStart = 0.0;          // last start we wrote, so a drag only writes on a change
    std::vector<double> dragOriginStarts;   // parallel to selectedClips

    // Trim drag: the one audio clip being trimmed, and the end we last wrote,
    // so dragging within the same beat writes nothing.
    juce::ValueTree trimClip;
    double trimLastEnd = 0.0;

    // A file drag currently over us, and where it would land.
    std::optional<FileDropTarget> fileDropTarget;

    std::map<juce::String, WaveformPeaks> peakCache;

    // Loop drag: the end of the range that stays put, plus whether the pointer
    // ever left the bar it went down in (a click without a drag clears).
    double loopAnchorBeats = 0.0;
    bool loopDragMoved = false;

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

    const juce::MouseCursor eraseCursor;
};

} // namespace carve::app
