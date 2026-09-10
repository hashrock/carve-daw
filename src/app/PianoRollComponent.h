#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "SelectionModifiers.h"
#include "TimeSigSupport.h"
#include "model/SongModel.h"

namespace carve::app
{

// Piano roll editor for one Pattern. Sized to its content; put it in a
// Viewport.
//
// Two tools, picked from the strip in the toolbar above the roll (or with
// D / E):
//   draw    click empty grid to add a note
//   select  drag empty grid to rubber-band notes
// In both tools a left click on a note selects it (Cmd or Shift extends the
// selection) and dragging it moves the whole selection, Cmd-dragging it moves
// a copy of the selection, a note's right edge resizes it (and the rest of the
// selection with it), right/alt-drag erases every note the cursor sweeps over,
// a right click on the selection deletes all of it, and so does Backspace.
// Holding Shift is the select tool for as long as it is held. The playlist
// reads all of these gestures the same way: SelectionModifiers.h is the list.
//
// Cmd-scroll zooms in time about the pointer. Cmd+C / Cmd+X / Cmd+V copy, cut
// and paste the selection through the system clipboard, Cmd+A selects every
// note, and Q quantises. Holding Ctrl ignores the grid for as long as it is
// held, whatever the toolbar's Snap says.
//
// A velocity lane sits under the roll (see PianoRollVelocityLane): it is a
// separate component, but the notes it edits and the writes it makes belong
// here, so it drives the roll rather than the model.
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
    // have both of them do the same arithmetic -- which is also why this is a
    // question rather than a constant now: the column is wider when its rows
    // are named sounds, since "Pad 12" and a sample's name do not fit in the
    // three characters a note name needs.
    int getKeyboardWidth() const;

    static constexpr int noteKeyboardWidth = 48;
    static constexpr int drumKeyboardWidth = 108;

    explicit PianoRollComponent (juce::UndoManager& um);
    ~PianoRollComponent() override;

    void setPattern (std::optional<model::Pattern> newPattern);

    // What a row is, for the generators whose notes are named things rather
    // than pitches: a drum machine's circuits, a kit's pads. Without this the
    // roll for a drum kit is a keyboard with fifteen silent lanes and one that
    // happens to be a snare, and the only way to find out which is to draw a
    // note and listen.
    struct DrumRow
    {
        juce::String name;
        bool assigned = true;   // a pad with no sample in it is still a row

        bool operator== (const DrumRow&) const = default;
    };

    // Keyed by MIDI note. Empty means an ordinary keyboard, which is what a
    // synth gets.
    void setDrumMap (std::map<int, DrumRow> newMap);

    // The rows worth looking at: the span of the drum map, or nothing at all
    // when there is no map and one row is as good as another.
    std::optional<juce::Range<int>> getDrumPitchRange() const;

    // Where a pitch sits in the roll's own coordinates, for a container that
    // has to scroll to it.
    int pitchToViewY (int pitch) const;

    // The notes of the pattern on screen, empty when there is none. The
    // velocity lane draws one bar per note and needs them all.
    std::vector<model::Note> getNotes() const;
    bool isNoteSelected (const model::Note& note) const  { return isSelected (note.state); }

    // The song, wanted only for its time signature map. A pattern is not a
    // position in the song, so the roll cannot work out its signature from
    // what it edits and has to be told which song the pattern belongs to.
    // Optional because the window exists before a song reaches it; until one
    // does, the roll counts 4/4.
    void setSong (model::Song newSong);

    // Which signature the roll draws its bar lines in.
    //
    // A pattern can be placed at several points in the song, and those points
    // can be under different signatures, so no signature is *the* pattern's.
    // What is picked is the one the song opens in: it is the only signature
    // that is a property of the song rather than of one placement, and it does
    // not move when the user drags a clip across a signature change. A song
    // written throughout in 6/8 therefore gets three-beat bars here; a 4/4 song
    // with a 7/8 bridge still gets 4/4, and the toolbar says which it is.
    model::TimeSignature getGridTimeSig() const;
    double getBeatsPerBar() const  { return getGridTimeSig().getBeatsPerBar(); }

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

    // Fired when the notes themselves changed, so the velocity lane can
    // redraw: it is a sibling rather than a child, so our own repaint() misses
    // it, and a velocity edit changes nothing the two callbacks above report.
    std::function<void()> onNotesChanged;

    // View settings, driven by the toolbar above the roll. The grid unit and
    // the snap flag describe how the song is *edited*, not what it is, so
    // neither of them touches the model.
    void setGridBeats (double beats);
    void setSnapEnabled (bool shouldSnap);
    double getGridBeats() const  { return gridBeats; }
    bool isSnapEnabled() const   { return snapEnabled; }

    // Whether a gesture made with these modifiers snaps: the toolbar's toggle,
    // unless the override key is held (see SelectionModifiers.h). Every place
    // that snaps asks this, and so does the window's help bar.
    bool isSnapActive (const juce::ModifierKeys& mods) const
    {
        return snapEnabled && ! selection::isSnapOverrideModifier (mods);
    }

    void setTool (Tool newTool);
    Tool getTool() const  { return tool; }

    // Holding shift is the select tool for as long as it is held, whatever the
    // toolbar says (see SelectionModifiers.h). Every gesture that asks "which
    // tool is this?" asks this instead -- and so does the window's shortcut
    // bar, which has to say what the roll would really do.
    Tool getEffectiveTool (const juce::ModifierKeys& mods) const
    {
        return selection::isSelectToolOverride (mods) ? Tool::select : tool;
    }
    int getNumSelectedNotes() const  { return (int) selectedNotes.size(); }

    // Every note of the pattern, which is what Cmd+A asks for.
    void selectAllNotes();

    //==============================================================================
    // Velocity lane support. The lane owns the geometry of its bars, because
    // only it knows how tall it is; the pattern, the selection and the undo
    // manager live here, so the writes stay here too.
    //
    // What a lane drag is allowed to touch: with a selection up, only the
    // selected notes answer to it, so that sweeping across a phrase cannot
    // quietly rewrite the neighbours the user had just ruled out. With nothing
    // selected, whatever the pointer sweeps is fair game -- which is the quick
    // way to dial in a hi-hat line without selecting anything first.
    bool isVelocityEditable (const model::Note& note) const;

    // One transaction per lane drag, the same rule the roll's own gestures
    // follow, so a whole sweep undoes in one step. It also snapshots the
    // selection's velocities, which is what offsetSelectionVelocity measures
    // from.
    void beginVelocityGesture();
    void setNoteVelocity (const model::Note& note, int velocity);

    // A lane drag that began on one of several selected notes moves them all
    // by the same amount: a chord that was voiced quieter on top should stay
    // voiced that way when the whole thing is brought down. Measured from the
    // velocities at the start of the gesture and clamped note by note, so a
    // note pinned at the floor does not drag the rest with it and comes back
    // up with them when the pointer returns.
    void offsetSelectionVelocity (int delta);

    //==============================================================================
    // Quantise. Note starts are pulled towards the nearest grid unit by
    // `strength` (0 leaves them alone, 1 puts them exactly on it), and every
    // other division is pushed late by `swing`, on the scale sequencers print
    // it on: 0.5 is straight, 2/3 the triplet feel, 0.75 dotted. Strength is
    // 0..1, swing 0.5..1 (see gestures::QuantiseSettings); both are clamped.
    //
    // The settings live here rather than in the panel that edits them, so that
    // the Q key can repeat the last quantise without the panel being open.
    void setQuantiseSettings (double strength, double swing);
    double getQuantiseStrength() const  { return quantiseStrength; }
    double getQuantiseSwing() const     { return quantiseSwing; }

    // Quantises the whole pattern, selection or no selection. Fixing the
    // timing is something you do to a part rather than to the handful of notes
    // that happen to be picked out, and a quantise that did only those left
    // the rest of the pattern behind -- which is the one result nobody wants.
    void quantiseNotes();

    //==============================================================================
    // Clipboard, as XML on the system clipboard so a copy in one pattern
    // editor can be pasted into another. Content that is not ours is ignored.
    bool copySelection() const;
    void cutSelection();
    void pasteNotes();

    // Zoom about the middle of what is on screen; the pointer-anchored version
    // is the cmd-scroll gesture and stays private.
    void zoomBy (double factor);
    double getPixelsPerBeat() const  { return pixelsPerBeat; }

    // Where in this pattern the transport currently is, in pattern beats, or
    // nothing while it is playing somewhere the pattern is not placed. The
    // owner works this out from the playlist; the roll only draws the line.
    void setPlayheadBeat (std::optional<double>);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    // A note row is as short as a note needs to be. A drum lane is not: it
    // carries the sound's name, and a name the app has to shrink to fit is
    // exactly the text the minimum font size exists to stop (see Fonts.h).
    static constexpr int noteRowHeight = 12;
    static constexpr int drumRowHeight = 16;

    int getRowHeight() const  { return drumMap.empty() ? noteRowHeight : drumRowHeight; }

    static constexpr int lowestPitch = 24;    // C1
    static constexpr int highestPitch = 96;   // C7
    static constexpr float resizeZoneWidth = 6.0f;

    // How far outside a selected note's bounds a right click still counts as
    // on it: the width of the selection outline, and a little more, since the
    // rows are short and a secondary click on a trackpad lands a pixel or two
    // from where the pointer was.
    static constexpr float selectedNoteHitSlack = 3.0f;
    static constexpr double defaultLengthBeats = 16.0;   // grid shown with no pattern loaded
    static constexpr int defaultNoteVelocity = 100;

    // Tag of our clipboard payload. Anything else on the clipboard -- text
    // from another app, notes from another program -- is left alone.
    static constexpr const char* clipboardTag = "CARVENOTES";

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
    void gridTimeSigChanged();
    void updateSize();
    int yToPitch (float y) const;
    float pitchToY (int pitch) const;
    juce::Rectangle<float> noteBounds (const model::Note&) const;
    std::optional<model::Note> noteAt (juce::Point<float>) const;
    bool isOverResizeZone (const model::Note&, juce::Point<float>) const;

    // Whether a press is on one of the selected notes, counting the selection
    // outline drawn around them as part of the note. See mouseDown for why
    // the exact hit test is not enough here.
    bool isOnSelectedNote (juce::Point<float>) const;

    // The grid a gesture made with these modifiers lands on, or the beat
    // untouched when it is not snapping.
    double snapDown (double beat, const juce::ModifierKeys& mods) const;
    double snapUp (double beat, const juce::ModifierKeys& mods) const;
    double snapNearest (double beat, const juce::ModifierKeys& mods) const;
    double minLengthBeats (const juce::ModifierKeys& mods) const;

    juce::Viewport* getViewport() const;
    void setPixelsPerBeat (double newPixelsPerBeat, float anchorX);

    // Which key means what is shared with the playlist: SelectionModifiers.h.
    static bool isEraseGesture (const juce::ModifierKeys& mods)       { return selection::isEraseGesture (mods); }
    static bool isDuplicateModifier (const juce::ModifierKeys& mods)  { return selection::isDuplicateModifier (mods); }

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

    // Lengthens or shortens every note the resize drag is touching by the
    // same amount, measured from where each was when the drag began and
    // clamped against the end of the pattern note by note.
    void resizeSelectionBy (double deltaBeats, const juce::ModifierKeys& mods);
    void deleteSelection();
    void updateRubberBand (juce::Point<float> position);

    // Where a paste puts the notes: see the .cpp for why it is the bar under
    // the pointer.
    double getPasteTargetBeat (double originBeat) const;

    void notifyNotesChanged() const
    {
        if (onNotesChanged)
            onNotesChanged();
    }

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
    std::optional<model::Song> song;
    TimeSigWatcher timeSigWatcher;

    double gridBeats = 0.25;   // 16th-note grid
    bool snapEnabled = true;
    double pixelsPerBeat = defaultPixelsPerBeat;
    std::optional<double> playheadBeat;
    Tool tool = Tool::draw;

    enum class DragMode { none, move, resize, erase, rubberBand };
    DragMode dragMode = DragMode::none;
    std::optional<model::Note> draggedNote;
    double grabOffsetBeats = 0.0;
    juce::Point<float> dragStartPosition;    // where the press landed, which a move measures from
    juce::Point<float> lastErasePosition;

    // New notes are drawn at the length of the last resize and the last
    // velocity the user set in the lane: having just dialled in a set of
    // ghost notes, the next one drawn should be one too. Static, because the
    // editor window is destroyed on close and built again on the next open,
    // and what the user last asked for should outlive the window it was
    // asked in.
    inline static double lastNoteLength = 0.5;
    inline static int lastNoteVelocity = defaultNoteVelocity;

    std::map<int, DrumRow> drumMap;

    double quantiseStrength = 1.0;
    double quantiseSwing = 0.5;   // straight: see gestures::QuantiseSettings

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

    // Resize drag: the notes it writes to -- the whole selection when the
    // grabbed note is part of it, otherwise just that note -- and how long
    // each was at the press, so the whole drag is measured from there rather
    // than creeping from wherever the last event left them.
    std::vector<juce::ValueTree> resizeTargets;
    std::vector<double> resizeOriginLengths;   // parallel to resizeTargets

    // Velocity gesture: what the selection was at the press, for the same
    // reason. Parallel to selectedNotes as it stood then; a selection that
    // changes under the gesture ends it (see offsetSelectionVelocity).
    std::vector<juce::ValueTree> velocityOriginNotes;
    std::vector<int> velocityOrigins;

    // Cmd went down on a note that was already selected. Which gesture that is
    // depends on what happens next: a drag copies the selection and moves the
    // copy, a click without one takes the note out of the selection. So it is
    // held here until the mouse says which.
    bool pendingDuplicate = false;
    std::optional<model::Note> pendingToggleNote;

    // Replaces the selection with a copy of itself, for a duplicate drag.
    // Returns false if there was nothing to copy, which leaves the drag a
    // plain move.
    bool duplicateSelectionForDrag();

    // The modifier pressed part way through a move: the notes being dragged
    // carry on, and copies are put down where they started, so the result is
    // the same as having held it from the press. Released again before the
    // mouse goes up, those copies are taken back. Held here so they can be.
    std::vector<juce::ValueTree> originCopies;
    void syncDragDuplicate (const juce::ModifierKeys& mods);

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

    // A press (or a scrub drag) landed on this pattern beat: the owner moves
    // the transport there. The ruler itself has no idea where in the song the
    // pattern is placed, so all it reports is the beat.
    std::function<void (double patternBeat)> onSeek;

    // x coordinate of the roll that sits at this component's left edge
    void setScrollOffset (int offsetX);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void seekTo (float x);

    const PianoRollComponent& roll;
    int scrollOffset = 0;
};

// Velocity lane drawn under the roll: one bar per note, standing where the
// note does and as tall as its velocity.
//
// Like the ruler it is a sibling of the roll's Viewport rather than a strip
// inside the roll, so it stays put while the roll scrolls through the
// pitches, and it lines up with the grid by drawing in the roll's own
// coordinates shifted by the viewport's horizontal scroll offset.
//
// Unlike the ruler it is an editor: dragging in it writes velocities. A drag
// begun on the bar of one of several selected notes moves all of them by the
// same amount; any other drag sets whatever it sweeps to the pointer's height.
// Only the geometry is worked out here -- which notes are fair game, and the
// model writes themselves, belong to the roll.
class PianoRollVelocityLane : public juce::Component
{
public:
    explicit PianoRollVelocityLane (PianoRollComponent& rollToEdit);

    static constexpr int preferredHeight = 64;

    // x coordinate of the roll that sits at this component's left edge
    void setScrollOffset (int offsetX);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    // Height of the bars' area: the rest is the baseline strip along the
    // bottom, which keeps a velocity-1 bar visible instead of nothing.
    static constexpr float topMargin = 4.0f;
    static constexpr float baselineHeight = 3.0f;

    // Narrow enough to sit inside a 1/32 note at the default zoom, wide
    // enough to stay grabbable when the roll is zoomed right out.
    static constexpr float minBarWidth = 3.0f;

    float barsTop() const     { return topMargin; }
    float barsBottom() const  { return (float) getHeight() - baselineHeight; }

    // Horizontal reach of a note's bar. Hit-testing uses this alone and
    // ignores the pointer's height: a bar for a quiet note is only a few
    // pixels tall, and having to hit it would make the lane unusable exactly
    // where it is most wanted.
    juce::Range<float> barSpan (const model::Note&) const;
    juce::Rectangle<float> barBounds (const model::Note&) const;
    int velocityAtY (float y) const;
    bool isOverEditableBar (juce::Point<float>) const;
    bool isOverSelectedBar (juce::Point<float>) const;

    // Whether a press here starts a group move of the selection rather than a
    // sweep: on a selected note's bar, with more than one note selected. A
    // single selected note is still set to the pointer's height, because that
    // is where the bar is dragged *to* rather than *by*.
    bool startsGroupDrag (juce::Point<float>) const;

    // Applies the pointer's velocity to every editable note the drag swept,
    // interpolated across the sweep so a diagonal drag draws a ramp rather
    // than flattening everything it passed to the last value.
    void applySweep (juce::Point<float> from, juce::Point<float> to);

    PianoRollComponent& roll;
    int scrollOffset = 0;
    bool dragging = false;
    juce::Point<float> lastDragPosition;

    // Group move: the velocity the pointer stood at when it pressed, which
    // the selection's change is measured from on every drag event.
    bool groupDragging = false;
    int groupDragOriginVelocity = 0;
};

} // namespace carve::app
