#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "IconButton.h"
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
// The header band holds the tool strip, the zoom control, the tempo lane and
// the bar ruler. Dragging the ruler sets the song's loop range; the lane above
// it holds the song's tempo and time signature changes, one marker each.
//
// Bars come from the model, never from a constant: a 3/4 section draws
// three-beat bars, and everything that snaps to a bar - painting, the loop
// range, a file drop - follows the same lines the ruler numbers.
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

    // A row label was double-clicked: the host should open that generator's
    // window (whichever tab it was last on).
    std::function<void (const juce::String&)> onOpenGenerator;

    // The "+ Generator" button in the tool strip; the host owns the add menu.
    // The rectangle is where to hang it (the button, in screen coords).
    std::function<void (juce::Rectangle<int>)> onAddGenerator;

    // A click on the ruler asked for the transport to move to this beat (a
    // drag still sets the loop range; a right click clears it).
    std::function<void (double beat)> onSeek;

    // A clip was double-clicked: (generatorId, patternId) of the pattern the
    // host should open in the pattern editor.
    std::function<void (const juce::String&, const juce::String&)> onEditPattern;

    // Fired whenever the set of selected clips changes; empty when nothing is selected.
    std::function<void (const std::vector<model::PlaylistClip>&)> onClipSelectionChanged;

    // The shortcuts that apply right now, for whichever help bar the host owns.
    // Only ours: the host appends its own global ones.
    std::function<void (std::vector<ShortcutHelpBar::Entry>)> onShortcutHelpChanged;

    // One parameter an automation lane can drive: volume, pan, or a parameter
    // of the generator's instrument or one of its insert effects. Names and
    // ranges for the latter only exist in the live engine, which this
    // component deliberately has no access to -- hence the callback.
    struct AutomatableParamInfo
    {
        juce::String target;      // AutomationLane::volumeTarget / panTarget /
                                  // instrumentTarget, or an insert effect's id
        juce::String param;       // paramID; empty for volume and pan
        juce::String label;       // "Volume", "Cutoff (4OSC)", ...
        float minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
    };

    // Asked whenever a lane needs the parameters a generator offers. Unset --
    // or answering nothing -- falls back to volume and pan, which every track
    // has regardless of what it hosts.
    std::function<std::vector<AutomatableParamInfo> (const juce::String& generatorId)> getAutomatableParams;

    void setSong (model::Song newSong);
    void setSelection (const juce::String& generatorId, const juce::String& patternId);
    void setPlayheadBeats (double beats);

    // Called by the host when the viewport we sit in changes size. Our width
    // is clamped to at least the viewport's, so the header band spans the
    // window -- but nothing else tells us the viewport grew.
    void hostViewportResized()  { updateSize(); }

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

    // The band under the last row, which holds the "+ Generator" button.
    static constexpr int addRowHeight = 38;
    // Band above the rows: a strip of controls, the tempo lane, then the bar
    // ruler. All three are pinned to the top of the visible area, so the rows
    // start below the set of them.
    //
    // The lane is its own strip rather than an overlay on the ruler: the ruler
    // is already a drag target for the loop range, and a marker sitting in it
    // would mean every ruler press had to decide which of the two it meant.
    static constexpr int toolbarHeight = 26;
    static constexpr int markerLaneHeight = 16;
    static constexpr int rulerHeight = 18;
    static constexpr int headerHeight = toolbarHeight + markerLaneHeight + rulerHeight;

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

    // The automation lane an expanded row carries under its clip band, and the
    // points on its curve. The hit radius is larger than the drawn dot, or
    // grabbing one would take pixel aim; the segment distance is how close to
    // the line a double click has to land to split it.
    static constexpr int automationLaneHeight = 40;
    static constexpr float autoPointRadius = 3.5f;
    static constexpr float autoPointHitRadius = 6.0f;
    static constexpr float autoSegmentHitDistance = 5.0f;
    static constexpr float autoLaneValuePad = 4.0f;   // top/bottom inset of the value range

    enum class DragMode
    {
        none,
        paint,
        erase,
        move,
        trim,        // an audio clip's right edge
        loopRange,   // on the ruler
        marker,      // a tempo or time signature change on the lane above it
        autoPoint,   // a point on a row's automation lane
        rubberBand   // select tool on empty space
    };

    // One tempo or time signature change as the lane draws and hit-tests it.
    // Both kinds differ only in what they say and where they may land, so the
    // lane holds them in one list rather than branching everywhere.
    struct Marker
    {
        juce::ValueTree state;
        double beat = 0.0;
        juce::String text;
        juce::Rectangle<float> bounds;

        bool isTempo() const  { return state.hasType (model::ids::TEMPO); }
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

    // Rows are no longer all one height: an expanded row carries its
    // automation lane under its clip band, pushing everything below it down.
    // Everything that maps y to a row has to say which band it means, so the
    // clip gestures and the lane gestures cannot claim each other's pixels.
    int rowTotalHeight (int row) const;
    float rowY (int row) const;    // top of the row's clip band
    int rowAt (float y) const;     // clip band or lane; numRows below the last row, -1 above
    int clipRowAt (float y) const; // like rowAt, but -1 when y is in an automation lane
    int laneRowAt (float y) const; // -1 unless y is in an automation lane

    // Bars, all of them from the model, so a section in another time signature
    // has bars of its length everywhere at once. snapToBar is what painting and
    // a file drop land on; nearestBar is for the gestures that have to follow
    // the pointer in both directions, a clip move among them.
    double snapToBar (double beat) const;
    double nextBarAfter (double beat) const;
    double barLengthAt (double beat) const;

    // The nearest bar line, optionally as it would be without one time
    // signature change in the song. Where such a change may land is decided by
    // every *other* change, never by itself: bar lines it defines move with it,
    // and snapping to those would let a drag leave it off the grid entirely.
    double nearestBar (double beat, const juce::ValueTree& ignore = {}) const;

    // Every bar line from the start of the song up to untilBeat, as
    // (0-based bar, its start, its length). The one walk over the time
    // signature changes that the ruler and the grid both draw from.
    void forEachBar (double untilBeat, const std::function<void (int, double, double)>&) const;

    // The shortest bar anywhere in the song, which is what the ruler and the
    // grid thin their lines against: a step that keeps 3/4 bars apart keeps
    // the 4/4 ones apart too.
    double shortestBar() const;

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

    // The row-label column is pinned to the left edge of the visible area the
    // same way the header band is pinned to its top, so scrolling right never
    // takes the row names off screen. Everything that used to test against
    // labelWidth tests against this edge instead.
    float labelBandLeft() const   { return (float) visibleOrigin().x; }
    float labelBandRight() const  { return labelBandLeft() + (float) labelWidth; }
    bool inLabelBand (juce::Point<float> p) const  { return p.x < labelBandRight(); }

    juce::Rectangle<float> toolbarBounds() const  { return headerBounds().withHeight ((float) toolbarHeight); }

    juce::Rectangle<float> markerLaneBounds() const
    {
        return headerBounds().withTrimmedTop ((float) toolbarHeight).withHeight ((float) markerLaneHeight);
    }

    juce::Rectangle<float> rulerBounds() const
    {
        return headerBounds().withTrimmedTop ((float) (toolbarHeight + markerLaneHeight));
    }

    std::optional<model::Pattern> patternForRow (const model::Generator&) const;

    // Resolves a playlist node into a Placement. Everything that draws,
    // hit-tests, moves or measures a clip has to go through this, or a clip
    // whose length isn't its pattern's ends up drawn in one place and clicked
    // in another. Empty when the clip's generator, or the pattern it names,
    // has gone.
    double audioLengthBeats (const model::AudioClip&) const;
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

    // The clipboard. It goes through juce::SystemClipboard as XML rather than
    // through a member of ours, so a copy made here survives being pasted from
    // another window -- and so nothing has to be told when the song is
    // replaced. Anything on the clipboard that is not ours reads as nothing.
    void copySelection();
    void cutSelection();
    void pasteClips();

    // The selected placements as one XML payload, each with its start relative
    // to the earliest of them: the payload says the shape of the group and the
    // paste says where it goes. Invalid when nothing is selected.
    juce::ValueTree makeClipboardPayload() const;
    static juce::ValueTree readClipboardPayload();

    // Where a paste lands: the bar under the pointer while it is over a row,
    // and the bar the playhead sits in otherwise -- so it follows the mouse
    // when there is one to follow and the transport when there isn't.
    double pasteTargetBeat() const;

    // The generator a clipboard entry belongs to: by id, or -- for a paste into
    // a song that no longer holds that id -- by the name it was copied under.
    std::optional<model::Generator> generatorForClipboardEntry (const juce::ValueTree&) const;

    void showPatternMenu (const model::PlaylistClip&, juce::Rectangle<float> buttonBounds);

    void dragLoopTo (double beat);
    void updateRubberBand (juce::Point<float> position);

    // The tempo lane. Markers are rebuilt from the model on demand rather than
    // cached: there are a handful of them, and a cache would be one more thing
    // an undo could leave pointing at a change that has gone.
    std::vector<Marker> markers() const;
    std::optional<Marker> markerAt (juce::Point<float> position) const;

    // Whether a change of the same kind already sits on this beat, ignoring
    // one tree (the marker being dragged). Two changes on the same beat would
    // fight over which one wins, so nothing is allowed to make a pair.
    bool hasChangeAt (bool tempo, double beat, const juce::ValueTree& ignore = {}) const;

    void addTempoChangeAt (double beat, double bpm);
    void addTimeSigChangeAt (double beat, model::TimeSignature);
    void removeMarker (const juce::ValueTree&);
    void dragMarkerTo (double beat);

    // Asks for a value, then writes it. Editing an existing change passes its
    // tree; adding one passes an empty tree and the beat it would go on, so a
    // cancelled dialog leaves nothing behind.
    void editTempoValue (juce::ValueTree existing, double beatForNew);
    void chooseTimeSigValue (juce::ValueTree existing, double beatForNew,
                             juce::Rectangle<float> targetArea);
    void showMarkerMenu (const Marker&);
    void showLaneMenu (double beat, juce::Point<float> position);

    // The automation lane under an expanded row. Which rows are expanded and
    // which parameter each lane shows are UI state like the selection; the
    // AUTOCURVE itself is only made once a point lands on it, so browsing
    // parameters leaves the song alone.
    bool isGeneratorExpanded (const juce::String& generatorId) const;
    bool isRowExpanded (int row) const;
    void toggleRowExpanded (int row);

    juce::Rectangle<float> disclosureBounds (int row) const;
    juce::Rectangle<float> automationLaneBounds (int row) const;   // empty when collapsed
    juce::Rectangle<float> automationCurveBounds (int row) const;  // the part right of the labels
    juce::Rectangle<float> laneSelectorBounds (int row) const;

    std::vector<AutomatableParamInfo> automatableParamsFor (const juce::String& generatorId) const;
    AutomatableParamInfo displayedParamFor (const juce::String& generatorId) const;
    std::optional<model::AutomationLane> displayedLaneFor (const model::Generator&) const;

    // Values map linearly between the parameter's own min and max, so the lane
    // never has to know what the number means.
    float valueToLaneY (float value, const AutomatableParamInfo&, juce::Rectangle<float> curveArea) const;
    float laneYToValue (float y, const AutomatableParamInfo&, juce::Rectangle<float> curveArea) const;

    std::optional<model::AutomationPoint> autoPointAt (int row, juce::Point<float> position) const;
    bool isNearCurveSegment (int row, juce::Point<float> position) const;

    void showLaneParamMenu (int row);
    void paintAutomationLane (juce::Graphics&, int row, const model::Generator&, juce::Colour rowColour);
    void dragAutoPointTo (juce::Point<float> position);

    bool isSelected (const juce::ValueTree& clip) const;
    void selectClip (const juce::ValueTree&, bool extend);
    void clearSelection();
    void pruneSelection();

    // The single funnel for "the selection may have moved": repaints and tells
    // the host, but only when the set actually differs from what it last heard.
    void selectionChanged();
    void updateShortcutHelp();

    void paintMarkerLane (juce::Graphics&);
    void paintRuler (juce::Graphics&);
    void paintRowLabels (juce::Graphics&);
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
    IconButton paintToolButton { "Paint", Icon::pencil }, selectToolButton { "Select", Icon::select };
    IconButton zoomOutButton { "", Icon::zoomOut }, zoomInButton { "", Icon::zoomIn };
    IconButton addTrackButton { "Generator", Icon::plus };

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
    // ever left the bar it went down in (a click without a drag seeks instead)
    // and the bar the press itself landed on, which is where the seek goes --
    // the anchor may be the far loop edge when the press grabbed the near one.
    double loopAnchorBeats = 0.0;
    double rulerPressBeats = 0.0;
    bool loopDragMoved = false;

    // Marker drag: the change being moved, where in it the pointer went down,
    // and the beat we last wrote so dragging within one beat writes nothing.
    juce::ValueTree dragMarker;
    double markerGrabOffsetBeats = 0.0;
    double markerLastBeat = 0.0;

    // Which generators show their automation lane, and which parameter each
    // one shows. Both keyed by id, like the selection, so neither touches the
    // model or the undo history.
    std::vector<juce::String> expandedGenerators;
    std::map<juce::String, std::pair<juce::String, juce::String>> laneParamChoices;   // id -> (target, param)

    // Automation point drag: the PT being moved, the lane's parameter and row
    // as they were at the press, and the last beat/value written so dragging
    // within one beat writes nothing.
    juce::ValueTree dragAutoPoint;
    AutomatableParamInfo dragAutoInfo;
    int dragAutoRow = -1;
    double autoPointLastBeat = 0.0;
    float autoPointLastValue = 0.0f;

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

    // The lane offers its own gestures, so the help bar lists those instead
    // while the pointer is over it.
    bool hoverMarkerLane = false;

    // Same again for an automation lane: -1 when the pointer is anywhere else.
    int hoverLaneRow = -1;

    const juce::MouseCursor eraseCursor;
};

} // namespace carve::app
