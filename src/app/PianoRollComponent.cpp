#include <algorithm>
#include <cmath>

#include "PianoRollComponent.h"
#include "Fonts.h"

#include "NoteGestures.h"
#include "TimelineView.h"

namespace carve::app
{

namespace
{
    bool isBlackKey (int pitch)
    {
        const int n = pitch % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    // Grid positions land on thirds for triplet grids, so exact comparisons
    // against bar/beat boundaries need a tolerance. remainder() (unlike fmod)
    // is signed and centred on zero, which makes one comparison enough on both
    // sides of the boundary.
    bool isMultipleOf (double beat, double unit)
    {
        return std::abs (std::remainder (beat, unit)) < 1.0e-6;
    }

    // Start of the copied block, carried on the clipboard payload so that a
    // paste can move the whole block as one.
    const char* const originAttribute = "origin";

    // The one note colour, at full velocity. Everything quieter is derived
    // from it by noteColourFor.
    const juce::Colour noteColour { 0xffe08a3c };

    // How a note's velocity is read off its colour.
    //
    // Brightness alone could not do it. The floor is pinned by the grid the
    // note is drawn on -- go much below the grid's own brightness and a quiet
    // note stops being visible at all -- which leaves under a factor of two to
    // spread the whole velocity range over, and real music spends most of its
    // time in the top half of that range. Two shades of the same orange, a few
    // percent apart, is what the velocity lane was added to work around.
    //
    // So saturation carries the difference instead, with brightness only
    // helping: a quiet note is washed out and a loud one is vivid, which reads
    // at a glance and at any size. The velocity lane is still where a value is
    // *edited*; this is only meant to say loud from quiet on the grid.
    juce::Colour noteColourFor (int velocity)
    {
        // Normalised over the velocities a note may actually hold (1..127, as
        // 0 is a note-off), so the quietest possible note sits at the bottom
        // of the ramp rather than a hundredth of the way up it.
        const auto v = juce::jlimit (0.0f, 1.0f,
                                     (float) (velocity - model::Note::quietestVelocity)
                                         / (float) (model::Note::loudestVelocity - model::Note::quietestVelocity));

        return noteColour.withSaturation (juce::jmap (v, 0.20f, noteColour.getSaturation()))
                         .withBrightness (juce::jmap (v, 0.60f, noteColour.getBrightness()));
    }
} // namespace

PianoRollComponent::PianoRollComponent (juce::UndoManager& um)
    : undoManager (um)
{
    // the tool keys, the zoom keys and Backspace are ours once the roll is clicked
    setWantsKeyboardFocus (true);

    timeSigWatcher.onChanged = [this] { gridTimeSigChanged(); };

    updateSize();
}

PianoRollComponent::~PianoRollComponent()
{
    if (pattern)
        pattern->state.removeListener (this);
}

void PianoRollComponent::setSong (model::Song newSong)
{
    song = std::move (newSong);
    timeSigWatcher.setSong (song->state);
    gridTimeSigChanged();
}

// Only where the bar lines fall changes: the pattern, and so the roll's own
// size, is untouched. The ruler is told because it copies our arithmetic
// rather than doing its own, and nothing else would tell it.
void PianoRollComponent::gridTimeSigChanged()
{
    repaint();

    if (onViewChanged)
        onViewChanged();
}

model::TimeSignature PianoRollComponent::getGridTimeSig() const
{
    // Beat 0: see the header for why the song's opening signature is the one a
    // pattern -- which sits at no particular point in the song -- is drawn in.
    return song ? song->getTimeSigAt (0.0) : model::TimeSignature();
}

void PianoRollComponent::setDrumMap (std::map<int, DrumRow> newMap)
{
    if (drumMap == newMap)
        return;

    const auto widthBefore = getKeyboardWidth();
    const auto rowHeightBefore = getRowHeight();
    drumMap = std::move (newMap);

    // The column's width is part of the roll's own width, so a map arriving
    // resizes it -- and the ruler, which measures from the same number. A map
    // arriving or leaving also changes how tall a row is, which is the roll's
    // height.
    if (getKeyboardWidth() != widthBefore || getRowHeight() != rowHeightBefore)
        updateSize();

    repaint();
}

int PianoRollComponent::getKeyboardWidth() const
{
    return drumMap.empty() ? noteKeyboardWidth : drumKeyboardWidth;
}

int PianoRollComponent::pitchToViewY (int pitch) const
{
    return (int) pitchToY (juce::jlimit (lowestPitch, highestPitch, pitch));
}

std::optional<juce::Range<int>> PianoRollComponent::getDrumPitchRange() const
{
    if (drumMap.empty())
        return std::nullopt;

    return juce::Range<int> (drumMap.begin()->first, drumMap.rbegin()->first);
}

void PianoRollComponent::setPattern (std::optional<model::Pattern> newPattern)
{
    // The same pattern again is nothing to do -- and everything below would
    // be harm: the host retargets the editor after every sync, and a sync
    // follows every note edit, so this arrives between the mouse events of a
    // drag. Dropping the drag and the selection here is what made a resize
    // stop after its first step and left the next note at the old length.
    if (pattern && newPattern && pattern->state == newPattern->state)
        return;

    if (pattern)
        pattern->state.removeListener (this);

    pattern = std::move (newPattern);
    dragMode = DragMode::none;
    draggedNote.reset();

    // the selection belongs to the pattern that was on screen, not to the roll
    selectedNotes.clear();
    rubberBandBaseSelection.clear();
    rubberBand = {};

    if (pattern)
        pattern->state.addListener (this);

    updateSize();
    notifyShortcutContext();
    notifyNotesChanged();
    repaint();
}

void PianoRollComponent::setGridBeats (double beats)
{
    // exactlyEqual because the question is "would this write change anything",
    // not "are these two numbers close"; it also keeps -Wfloat-equal quiet
    if (beats > 0.0 && ! juce::exactlyEqual (beats, gridBeats))
    {
        gridBeats = beats;
        repaint();
    }
}

void PianoRollComponent::setSnapEnabled (bool shouldSnap)
{
    snapEnabled = shouldSnap;
}

void PianoRollComponent::setTool (Tool newTool)
{
    if (tool == newTool)
        return;

    tool = newTool;
    notifyShortcutContext();

    if (isMouseOver (false))
        setMouseCursor (cursorFor (getMouseXYRelative().toFloat(),
                                   juce::ModifierKeys::getCurrentModifiers()));
}

void PianoRollComponent::patternChanged()
{
    // The pattern length drives the component width, so any property change
    // may need a resize. setSize() early-outs when nothing moved, which makes
    // this cheap enough to run for note edits too.
    pruneSelection();
    updateSize();
    notifyNotesChanged();
    repaint();
}

juce::Viewport* PianoRollComponent::getViewport() const
{
    return findParentComponentOfClass<juce::Viewport>();
}

void PianoRollComponent::updateSize()
{
    auto width = getKeyboardWidth() + juce::roundToInt (getLengthBeats() * pixelsPerBeat);

    // Zoomed far enough out a short pattern is narrower than the viewport;
    // stay at least as wide as it so the roll's own background, and not the
    // viewport's, fills the window.
    if (auto* viewport = getViewport())
        width = juce::jmax (width, viewport->getMaximumVisibleWidth());

    setSize (width, (highestPitch - lowestPitch + 1) * getRowHeight());
}

double PianoRollComponent::getLengthBeats() const
{
    return pattern ? pattern->getLengthBeats() : defaultLengthBeats;
}

std::vector<model::Note> PianoRollComponent::getNotes() const
{
    return pattern ? pattern->getNotes() : std::vector<model::Note>();
}

double PianoRollComponent::xToBeat (float x) const     { return (x - (float) getKeyboardWidth()) / pixelsPerBeat; }
float PianoRollComponent::beatToX (double beat) const  { return (float) (getKeyboardWidth() + beat * pixelsPerBeat); }

void PianoRollComponent::setPlayheadBeat (std::optional<double> beat)
{
    // Tolerant compare: this arrives on a timer, and repainting the roll for a
    // sub-pixel move would burn paint for nothing while stopped.
    if (playheadBeat.has_value() != beat.has_value()
         || (beat.has_value() && std::abs (*playheadBeat - *beat) > 1.0e-3))
    {
        playheadBeat = beat;
        repaint();
    }
}
int PianoRollComponent::yToPitch (float y) const       { return highestPitch - (int) (y / getRowHeight()); }
float PianoRollComponent::pitchToY (int pitch) const   { return (float) ((highestPitch - pitch) * getRowHeight()); }

void PianoRollComponent::setPixelsPerBeat (double newPixelsPerBeat, float anchorX)
{
    newPixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, newPixelsPerBeat);

    if (std::abs (newPixelsPerBeat - pixelsPerBeat) < 1.0e-6)
        return;

    zoomAroundAnchor (*this, anchorX,
                      [this] (float x) { return xToBeat (x); },
                      [this] (double beat) { return beatToX (beat); },
                      [this, newPixelsPerBeat]
                      {
                          pixelsPerBeat = newPixelsPerBeat;
                          updateSize();
                      });

    // the ruler works in our coordinates, so it has to be told the scale moved
    if (onViewChanged)
        onViewChanged();

    repaint();
}

void PianoRollComponent::zoomBy (double factor)
{
    // No pointer to zoom about, so hold the middle of the visible span still.
    auto anchorX = (float) (getWidth() / 2);

    if (auto* viewport = getViewport())
        anchorX = (float) (viewport->getViewPositionX() + viewport->getViewWidth() / 2);

    setPixelsPerBeat (pixelsPerBeat * factor, anchorX);
}

double PianoRollComponent::snapDown (double beat, const juce::ModifierKeys& mods) const
{
    if (! isSnapActive (mods))
        return beat;

    return std::floor (beat / gridBeats + 1.0e-9) * gridBeats;
}

double PianoRollComponent::snapUp (double beat, const juce::ModifierKeys& mods) const
{
    if (! isSnapActive (mods))
        return beat;

    return std::ceil (beat / gridBeats - 1.0e-9) * gridBeats;
}

// What a move drag snaps to. Nearest rather than down, because a move is
// measured from where the note already is: a note sitting on a grid line --
// which is where nearly every note is -- would otherwise fall a whole
// division the instant the pointer moved one pixel left, while needing a
// full division of travel to move right. Nearest makes the two directions
// alike, and puts half a division of slack around standing still.
double PianoRollComponent::snapNearest (double beat, const juce::ModifierKeys& mods) const
{
    if (! isSnapActive (mods))
        return beat;

    return std::floor (beat / gridBeats + 0.5) * gridBeats;
}

double PianoRollComponent::minLengthBeats (const juce::ModifierKeys& mods) const
{
    return isSnapActive (mods) ? gridBeats : freeMinLengthBeats;
}

juce::Rectangle<float> PianoRollComponent::noteBounds (const model::Note& note) const
{
    return { beatToX (note.getStart()), pitchToY (note.getPitch()),
             (float) (note.getLength() * pixelsPerBeat), (float) getRowHeight() };
}

std::optional<model::Note> PianoRollComponent::noteAt (juce::Point<float> position) const
{
    if (! pattern)
        return std::nullopt;

    // iterate in reverse so the most recently added note wins on overlap
    for (int i = pattern->getNumNotes(); --i >= 0;)
    {
        auto note = pattern->getNote (i);
        if (noteBounds (note).contains (position))
            return note;
    }
    return std::nullopt;
}

bool PianoRollComponent::isOnSelectedNote (juce::Point<float> position) const
{
    for (const auto& state : selectedNotes)
        if (noteBounds (model::Note (state)).expanded (selectedNoteHitSlack).contains (position))
            return true;

    return false;
}

bool PianoRollComponent::isOverResizeZone (const model::Note& note, juce::Point<float> position) const
{
    const auto bounds = noteBounds (note);

    // never let the resize zone swallow a short note whole, or it becomes
    // impossible to drag
    const auto zone = juce::jmin (resizeZoneWidth, bounds.getWidth() * 0.4f);
    return position.x > bounds.getRight() - zone;
}

//==============================================================================
// Selection (UI state only)

bool PianoRollComponent::isSelected (const juce::ValueTree& note) const
{
    return std::find (selectedNotes.begin(), selectedNotes.end(), note) != selectedNotes.end();
}

void PianoRollComponent::selectNote (const model::Note& note, bool extend)
{
    if (! extend)
    {
        if (isSelected (note.state))
            return;   // keep a multi-selection intact when re-clicking part of it
        selectedNotes.clear();
    }
    else if (isSelected (note.state))
    {
        std::erase (selectedNotes, note.state);
        notifyShortcutContext();
        repaint();
        return;
    }

    selectedNotes.push_back (note.state);
    notifyShortcutContext();
    repaint();
}

void PianoRollComponent::setSelection (std::vector<juce::ValueTree> newSelection)
{
    if (newSelection == selectedNotes)
        return;

    selectedNotes = std::move (newSelection);
    notifyShortcutContext();
    repaint();
}

void PianoRollComponent::clearSelection()
{
    setSelection ({});
}

void PianoRollComponent::selectAllNotes()
{
    if (! pattern)
        return;

    std::vector<juce::ValueTree> all;

    for (const auto& note : pattern->getNotes())
        all.push_back (note.state);

    setSelection (std::move (all));
}

void PianoRollComponent::pruneSelection()
{
    // Notes removed - by us, by undo, or by a pattern reload - leave detached
    // trees behind; drop anything that is no longer part of the pattern.
    const auto sizeBefore = selectedNotes.size();

    if (pattern)
    {
        const auto patternState = pattern->state;
        std::erase_if (selectedNotes, [&] (const juce::ValueTree& note)
        {
            return ! note.isAChildOf (patternState);
        });
    }
    else
    {
        selectedNotes.clear();
    }

    if (selectedNotes.size() != sizeBefore)
        notifyShortcutContext();
}

void PianoRollComponent::beginSelectionDrag()
{
    // Snapshot where the selection started, so every move during the drag is
    // measured from the beginning of the gesture rather than from the last
    // event; that is what keeps the set's shape while it is dragged.
    dragOriginStarts.clear();
    dragOriginPitches.clear();

    for (const auto& state : selectedNotes)
    {
        const model::Note note (state);
        dragOriginStarts.push_back (note.getStart());
        dragOriginPitches.push_back (note.getPitch());
    }

    if (draggedNote)
    {
        dragAnchorOriginStart = draggedNote->getStart();
        dragAnchorOriginPitch = draggedNote->getPitch();
        dragLastPitch = dragAnchorOriginPitch;
    }
}

void PianoRollComponent::dragSelectionTo (double anchorStart, int anchorPitch)
{
    if (! pattern || selectedNotes.empty() || selectedNotes.size() != dragOriginStarts.size())
        return;

    // Every write calls back into patternChanged(), which rewrites
    // selectedNotes, so work from our own copy for the whole gesture.
    const auto notes = selectedNotes;

    // Where the gesture started, which is what it is clamped against: the
    // selection keeps its shape when it runs into an end of the pattern or of
    // the keyboard, instead of collapsing onto the boundary.
    std::vector<gestures::NoteSpan> origins;
    origins.reserve (notes.size());

    for (size_t i = 0; i < notes.size(); ++i)
        origins.push_back ({ dragOriginStarts[i], model::Note (notes[i]).getLength(),
                             dragOriginPitches[i] });

    const auto bounds = gestures::boundsOf (origins);

    const auto deltaBeats = gestures::clampMoveBeats (bounds, pattern->getLengthBeats(),
                                                      anchorStart - dragAnchorOriginStart);
    const auto deltaPitch = gestures::clampMovePitch (bounds, { lowestPitch, highestPitch },
                                                      anchorPitch - dragAnchorOriginPitch);

    for (size_t i = 0; i < notes.size(); ++i)
    {
        model::Note note (notes[i]);
        const auto start = dragOriginStarts[i] + deltaBeats;
        const auto pitch = dragOriginPitches[i] + deltaPitch;

        // Only write when the result actually moved: every property change
        // triggers a full EditSync resync, and a drag produces a lot of events.
        if (! juce::exactlyEqual (start, note.getStart()))
            note.setStart (start, &undoManager);

        if (pitch != note.getPitch())
            note.setPitch (pitch, &undoManager);
    }

    // Preview only when the grabbed note actually lands on a new row, so that
    // dragging along a row does not machine-gun the same pitch.
    const auto anchorNewPitch = dragAnchorOriginPitch + deltaPitch;

    if (anchorNewPitch != dragLastPitch)
    {
        dragLastPitch = anchorNewPitch;
        previewNote (anchorNewPitch, draggedNote ? draggedNote->getVelocity() : lastNoteVelocity);
    }
}

bool PianoRollComponent::duplicateSelectionForDrag()
{
    if (! pattern || selectedNotes.empty())
        return false;

    // Every addNote calls back into patternChanged(), which rewrites
    // selectedNotes, so work from our own copy.
    const auto originals = selectedNotes;
    const auto anchorState = draggedNote ? draggedNote->state : juce::ValueTree();

    std::vector<juce::ValueTree> copies;
    copies.reserve (originals.size());

    for (const auto& state : originals)
    {
        const model::Note note (state);
        auto copy = pattern->addNote (note.getStart(), note.getLength(),
                                      note.getPitch(), note.getVelocity(), &undoManager);

        // The copy of the note the pointer grabbed carries the drag from here,
        // so the block keeps moving relative to where it was picked up.
        if (state == anchorState)
            draggedNote = copy;

        copies.push_back (copy.state);
    }

    // The copies are what is being dragged, and what stays selected at the end
    // -- the originals are left exactly where they were.
    setSelection (std::move (copies));
    beginSelectionDrag();
    return true;
}

void PianoRollComponent::syncDragDuplicate (const juce::ModifierKeys& mods)
{
    if (! pattern || dragMode != DragMode::move || selectedNotes.size() != dragOriginStarts.size())
        return;

    const bool wantCopies = isDuplicateModifier (mods);

    if (wantCopies && originCopies.empty())
    {
        // Copies go where the drag began, not where the notes are now, so the
        // originals appear to have stayed put and the dragged ones to be new
        // -- the same picture a copy held from the press gives.
        const auto notes = selectedNotes;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const model::Note note (notes[i]);
            originCopies.push_back (pattern->addNote (dragOriginStarts[i], note.getLength(),
                                                      dragOriginPitches[i], note.getVelocity(),
                                                      &undoManager).state);
        }
    }
    else if (! wantCopies && ! originCopies.empty())
    {
        // Let go of the key before the mouse: back to a plain move.
        for (const auto& state : originCopies)
            pattern->removeNote (model::Note (state), &undoManager);

        originCopies.clear();
    }
}

void PianoRollComponent::resizeSelectionBy (double deltaBeats, const juce::ModifierKeys& mods)
{
    if (! pattern || resizeTargets.size() != resizeOriginLengths.size())
        return;

    // Every write calls back into patternChanged(), which rewrites
    // selectedNotes, so work from our own copy for the whole gesture.
    const auto targets = resizeTargets;
    const auto minLength = minLengthBeats (mods);

    for (size_t i = 0; i < targets.size(); ++i)
    {
        model::Note note (targets[i]);

        // The same change for every note rather than the same length: a
        // chord whose notes were voiced to different lengths should keep
        // that voicing when the whole thing is stretched, and a selection
        // resized to one length is only a drag away for anyone who wants it.
        //
        // Clamped per note rather than once against the grabbed one: the
        // selection can reach further into the pattern than the note under the
        // pointer, and a note may not run off the end of it.
        const auto maxLength = juce::jmax (minLength, pattern->getLengthBeats() - note.getStart());
        const auto clamped = juce::jlimit (minLength, maxLength, resizeOriginLengths[i] + deltaBeats);

        // Only write when the result actually changed: every property change
        // triggers a full EditSync resync, and a drag produces a lot of events.
        if (! juce::exactlyEqual (clamped, note.getLength()))
            note.setLength (clamped, &undoManager);
    }
}

void PianoRollComponent::deleteSelection()
{
    if (! pattern || selectedNotes.empty())
        return;

    // Each removal calls back into patternChanged(), which drops the note from
    // selectedNotes, so iterate a copy rather than the live vector.
    const auto notes = selectedNotes;

    undoManager.beginNewTransaction();

    for (const auto& state : notes)
        pattern->removeNote (model::Note (state), &undoManager);

    clearSelection();
}

void PianoRollComponent::updateRubberBand (juce::Point<float> position)
{
    rubberBand = juce::Rectangle<float> (rubberBandAnchor, position);

    if (! pattern)
        return;

    // Selection is UI state, so recomputing it from scratch on every move is
    // free: no model write, no resync.
    auto newSelection = rubberBandBaseSelection;

    for (const auto& note : pattern->getNotes())
        if (noteBounds (note).intersects (rubberBand)
             && std::find (newSelection.begin(), newSelection.end(), note.state) == newSelection.end())
            newSelection.push_back (note.state);

    setSelection (std::move (newSelection));
    repaint();   // the band itself moved even when the selection did not
}

//==============================================================================
// Velocity

bool PianoRollComponent::isVelocityEditable (const model::Note& note) const
{
    // See the header: a selection narrows the lane to it, no selection leaves
    // the lane open to everything under the pointer.
    return selectedNotes.empty() || isSelected (note.state);
}

void PianoRollComponent::beginVelocityGesture()
{
    undoManager.beginNewTransaction();

    velocityOriginNotes = selectedNotes;
    velocityOrigins.clear();

    for (const auto& state : velocityOriginNotes)
        velocityOrigins.push_back (model::Note (state).getVelocity());
}

void PianoRollComponent::offsetSelectionVelocity (int delta)
{
    // The selection this gesture began on is the one it moves. Anything that
    // changed it under the drag -- an undo, a pattern reload -- ends the
    // gesture rather than having it write to notes the user never aimed at.
    if (! pattern || velocityOriginNotes.empty() || velocityOriginNotes != selectedNotes)
        return;

    // Every write calls back into patternChanged(), so work from our own copy.
    const auto notes = velocityOriginNotes;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto velocity = model::Note::clampVelocity (velocityOrigins[i] + delta);
        model::Note note (notes[i]);

        // Only write when the result actually changed: every property change
        // triggers a full EditSync resync, and a drag produces a lot of events.
        if (velocity != note.getVelocity())
            note.setVelocity (velocity, &undoManager);
    }
}

void PianoRollComponent::setNoteVelocity (const model::Note& note, int velocity)
{
    // The floor is 1, not 0: velocity 0 is a note-off in MIDI, so a bar
    // dragged all the way down has to mean "as quiet as it goes" rather than
    // "silently gone".
    velocity = model::Note::clampVelocity (velocity);

    // Remembered even when the write below turns out to be a no-op: the user
    // aimed at this value, so it is the one the next drawn note should get.
    lastNoteVelocity = velocity;

    if (velocity != note.getVelocity())
        model::Note (note.state).setVelocity (velocity, &undoManager);
}

//==============================================================================
// Quantise

void PianoRollComponent::setQuantiseSettings (double strength, double swing)
{
    quantiseStrength = juce::jlimit (0.0, 1.0, strength);
    quantiseSwing = juce::jlimit (gestures::QuantiseSettings::straightSwing,
                                  gestures::QuantiseSettings::maxSwing, swing);
}

void PianoRollComponent::quantiseNotes()
{
    if (! pattern)
        return;

    // The whole pattern, whatever is selected: quantising is something done to
    // a part, and one that moved only the notes that happened to be picked out
    // would leave the rest sitting where they were.
    //
    // Copied out, because every write calls our own listener back
    // synchronously and that rewrites the selection underneath us.
    auto notes = pattern->getNotes();

    if (notes.empty())
        return;

    const gestures::QuantiseSettings settings { gridBeats, quantiseStrength, quantiseSwing };
    const auto patternLength = pattern->getLengthBeats();

    // One transaction for the whole operation, however many notes it moves.
    undoManager.beginNewTransaction();

    for (auto& note : notes)
    {
        const auto start = note.getStart();
        const auto newStart = gestures::quantisedStart (start, note.getLength(), settings,
                                                        patternLength);

        if (! juce::exactlyEqual (newStart, start))
            note.setStart (newStart, &undoManager);
    }
}

//==============================================================================
// Clipboard

bool PianoRollComponent::copySelection() const
{
    if (selectedNotes.empty())
        return false;

    juce::XmlElement xml (clipboardTag);

    // Where the copied block began, so that a paste can put it down as a block
    // rather than moving every note to the same place.
    auto origin = model::Note (selectedNotes.front()).getStart();

    for (const auto& state : selectedNotes)
        origin = juce::jmin (origin, model::Note (state).getStart());

    xml.setAttribute (originAttribute, origin);

    // The NOTE trees as they stand: the payload is the model's own format, so
    // the notes survive the round trip without a second description of them.
    for (const auto& state : selectedNotes)
        xml.addChildElement (state.createXml().release());

    juce::SystemClipboard::copyTextToClipboard (xml.toString());
    return true;
}

void PianoRollComponent::cutSelection()
{
    // Delete only once the copy is actually on the clipboard, so a cut that
    // could not copy cannot lose the notes.
    if (copySelection())
        deleteSelection();
}

// Paste lands under the pointer, snapped to the grid, and only while the
// pointer is over the grid; otherwise the notes go back exactly where they
// were copied from.
//
// The roll has no playhead to paste at -- a pattern sits at no particular
// point in the song, so there is nothing for one to follow -- so the pointer
// is the cursor here, and it is where the user is already looking. The grid
// rather than the bar line: a paste is often a phrase moved a beat or two, and
// rounding that down to the bar put it somewhere it had to be dragged out of
// again. Snap off means exactly the pointer, like every other gesture.
double PianoRollComponent::getPasteTargetBeat (double originBeat) const
{
    if (! isMouseOver (true))
        return originBeat;

    const auto position = getMouseXYRelative().toFloat();

    if (position.x < (float) getKeyboardWidth())
        return originBeat;

    // The modifiers as they are now: a paste comes from the keyboard, so there
    // is no mouse event to read them off, but Ctrl held while it is pressed
    // should mean what it does everywhere else.
    return snapDown (juce::jmax (0.0, xToBeat (position.x)), juce::ModifierKeys::getCurrentModifiers());
}

void PianoRollComponent::pasteNotes()
{
    if (! pattern)
        return;

    const auto xml = juce::parseXML (juce::SystemClipboard::getTextFromClipboard());

    // Anything that is not ours -- plain text, XML from another program -- is
    // left alone rather than guessed at.
    if (xml == nullptr || ! xml->hasTagName (clipboardTag))
        return;

    // .start/.length/.pitch so that gestures::boundsOf can read these
    // directly, the same way it reads a selection being dragged.
    struct PastedNote : gestures::NoteSpan { int velocity = 0; };
    std::vector<PastedNote> notes;

    const auto noteTag = model::ids::NOTE.toString();

    for (auto* child : xml->getChildIterator())
    {
        if (! child->hasTagName (noteTag))
            continue;

        // Clamped on the way in: this is text off a clipboard anyone can
        // write to, so it is something to make sense of rather than to trust.
        notes.push_back ({ { juce::jmax (0.0, child->getDoubleAttribute (model::ids::start.toString())),
                             juce::jmax (freeMinLengthBeats,
                                         child->getDoubleAttribute (model::ids::length.toString())),
                             juce::jlimit (lowestPitch, highestPitch,
                                           child->getIntAttribute (model::ids::pitch.toString())) },
                           model::Note::clampVelocity (
                               child->getIntAttribute (model::ids::velocity.toString(),
                                                       defaultNoteVelocity)) });
    }

    if (notes.empty())
        return;

    const auto bounds = gestures::boundsOf (notes);
    const auto origin = xml->getDoubleAttribute (originAttribute, bounds.lowestStart);

    // Clamp the block rather than each note, so it keeps its shape when it
    // lands against an end of the pattern -- the same call, and so the same
    // rule, a move drag follows.
    const auto offset = gestures::clampMoveBeats (bounds, pattern->getLengthBeats(),
                                                  getPasteTargetBeat (origin) - origin);

    undoManager.beginNewTransaction();

    std::vector<juce::ValueTree> pasted;

    for (const auto& note : notes)
        pasted.push_back (pattern->addNote (note.start + offset, note.length,
                                            note.pitch, note.velocity, &undoManager).state);

    // The paste becomes the selection, so it can be dragged somewhere else or
    // deleted again without having to be found first.
    setSelection (std::move (pasted));
}

//==============================================================================
void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const auto lengthBeats = getLengthBeats();
    const auto gridRight = beatToX (lengthBeats);

    // rows
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const auto y = pitchToY (pitch);

        if (! drumMap.empty())
        {
            // With a drum map the black keys mean nothing -- the rows are
            // sounds, not a scale -- so what is shaded instead is every lane
            // that has nothing on it. What is left lit is what can be played.
            const auto row = drumMap.find (pitch);
            const auto playable = row != drumMap.end() && row->second.assigned;

            if (! playable)
            {
                g.setColour (juce::Colour (0xff18181c));
                g.fillRect ((float) getKeyboardWidth(), y, gridRight - getKeyboardWidth(), (float) getRowHeight());
            }
        }
        else if (isBlackKey (pitch))
        {
            g.setColour (juce::Colour (0xff1c1c20));
            g.fillRect ((float) getKeyboardWidth(), y, gridRight - getKeyboardWidth(), (float) getRowHeight());
        }
        if (pitch % 12 == 0)   // octave line above each C row
        {
            g.setColour (juce::Colour (0xff3a3a40));
            g.drawHorizontalLine ((int) y + getRowHeight(), (float) getKeyboardWidth(), gridRight);
        }
    }

    // Vertical grid, thinned as the zoom closes it up: the sub-grid goes as
    // soon as its lines are a few pixels apart and the beat lines follow, so
    // that what is left never merges into a solid block. Bars survive at every
    // zoom, because they are what the roll is read against.
    const bool drawSubGrid = gridBeats * pixelsPerBeat >= 5.0;
    const bool drawBeats = pixelsPerBeat >= 9.0;

    if (drawSubGrid)
    {
        // Stepped by index rather than by accumulating gridBeats so that a
        // triplet grid does not drift across a long pattern. Whole beats are
        // skipped here because the pass below draws them in their own colour.
        const int numGridLines = (int) std::floor (lengthBeats / gridBeats + 1.0e-9);
        g.setColour (juce::Colour (0xff2c2c31));

        for (int i = 0; i <= numGridLines; ++i)
        {
            const double beat = (double) i * gridBeats;
            if (! isMultipleOf (beat, 1.0))
                g.drawVerticalLine ((int) beatToX (beat), 0.0f, (float) getHeight());
        }
    }

    // Beats and bars are two passes rather than one, because a bar is not a
    // whole number of beats in every signature -- a 7/8 bar is three and a
    // half -- so a bar line does not have to land on one of these. Bars come
    // second so that they win wherever the two do coincide.
    const auto beatsPerBar = getBeatsPerBar();

    if (drawBeats)
    {
        g.setColour (juce::Colour (0xff3a3a40));

        for (int beat = 0; (double) beat <= lengthBeats; ++beat)
            if (! isMultipleOf ((double) beat, beatsPerBar))
                g.drawVerticalLine ((int) beatToX ((double) beat), 0.0f, (float) getHeight());
    }

    g.setColour (juce::Colour (0xff55555e));

    for (int bar = 0; (double) bar * beatsPerBar <= lengthBeats; ++bar)
        g.drawVerticalLine ((int) beatToX ((double) bar * beatsPerBar), 0.0f, (float) getHeight());

    // end of the pattern, which an off-grid length would otherwise not mark
    g.setColour (juce::Colour (0xff6a6a74));
    g.drawVerticalLine ((int) gridRight, 0.0f, (float) getHeight());

    // notes
    if (pattern)
    {
        for (const auto& note : pattern->getNotes())
        {
            auto r = noteBounds (note).reduced (0.0f, 1.0f);
            const bool selected = isSelected (note.state);

            g.setColour (noteColourFor (note.getVelocity()).brighter (selected ? 0.4f : 0.0f));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (r, 2.0f, selected ? 1.6f : 1.0f);
        }
    }

    if (dragMode == DragMode::rubberBand && ! rubberBand.isEmpty())
    {
        g.setColour (noteColour.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (noteColour.withAlpha (0.8f));
        g.drawRect (rubberBand, 1.0f);
    }

    // playhead, in the playlist's colour, drawn under the keyboard column so
    // it disappears behind it rather than crossing the key names
    if (playheadBeat && *playheadBeat >= 0.0 && *playheadBeat <= lengthBeats)
    {
        g.setColour (juce::Colours::orangered);
        g.drawVerticalLine ((int) beatToX (*playheadBeat), 0.0f, (float) getHeight());
    }

    // keyboard column -- or, for a drum generator, the names of its sounds
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const auto y = pitchToY (pitch);
        const auto row = drumMap.find (pitch);
        const auto named = row != drumMap.end();

        if (named)
        {
            // Lit for a sound that is there, dark for a pad waiting to be
            // filled: the same distinction the shading in the grid makes, so
            // the eye can start at either end.
            g.setColour (row->second.assigned ? juce::Colour (0xffd8d8dc) : juce::Colour (0xff34343a));
            g.fillRect (0.0f, y, (float) getKeyboardWidth() - 2.0f, (float) getRowHeight() - 0.5f);

            g.setColour (row->second.assigned ? juce::Colour (0xff2a2a2e) : juce::Colour (0xff6a6a74));
            g.setFont (uiFont (fonts::small));   // a lane is one row tall; drawFittedText shrinks what will not fit
            g.drawFittedText (row->second.name, 3, (int) y, getKeyboardWidth() - 6, getRowHeight(),
                              juce::Justification::centredLeft, 1, 0.7f);
            continue;
        }

        g.setColour (drumMap.empty() ? (isBlackKey (pitch) ? juce::Colour (0xff2a2a2e)
                                                           : juce::Colour (0xffd8d8dc))
                                     : juce::Colour (0xff232327));   // outside the kit
        g.fillRect (0.0f, y, (float) getKeyboardWidth() - 2.0f, (float) getRowHeight() - 0.5f);

        if (drumMap.empty() && pitch % 12 == 0)
        {
            g.setColour (juce::Colour (0xff707078));
            g.setFont (uiFont (fonts::small));
            g.drawText ("C" + juce::String (pitch / 12 - 1),
                        2, (int) y, getKeyboardWidth() - 8, getRowHeight(), juce::Justification::centredRight);
        }
    }
}

juce::MouseCursor PianoRollComponent::cursorFor (juce::Point<float> position,
                                                 const juce::ModifierKeys& mods) const
{
    // the keyboard column is not editable, and neither is an empty editor
    if (! pattern || position.x < (float) getKeyboardWidth())
        return juce::MouseCursor::NormalCursor;

    if (isEraseGesture (mods))
        return juce::MouseCursor::CrosshairCursor;

    if (auto note = noteAt (position))
        return isOverResizeZone (*note, position) ? juce::MouseCursor::LeftRightResizeCursor
                                                  : juce::MouseCursor::DraggingHandCursor;

    // empty grid: the draw tool would add a note here, the select tool would
    // start a rubber band
    return getEffectiveTool (mods) == Tool::select ? juce::MouseCursor::CrosshairCursor
                                                : juce::MouseCursor::NormalCursor;
}

void PianoRollComponent::updateCursor (const juce::MouseEvent& e)
{
    setMouseCursor (cursorFor (e.position, e.mods));
}

void PianoRollComponent::modifierKeysChanged (const juce::ModifierKeys& modifiers)
{
    // holding alt turns the pointer into the erase cursor without moving the
    // mouse, so react to the modifier itself as well as to mouseMove
    if (dragMode == DragMode::none && isMouseOver (false))
        setMouseCursor (cursorFor (getMouseXYRelative().toFloat(), modifiers));

    // A move in progress becomes a copy the moment the key goes down, not on
    // the next mouse movement -- the copies should appear under a still hand.
    if (dragMode == DragMode::move && ! pendingDuplicate)
        syncDragDuplicate (modifiers);

    // alt and cmd change what the next drag will do, and the help bar says so
    notifyShortcutContext();
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    updateCursor (e);
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e,
                                         const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        setPixelsPerBeat (pixelsPerBeat * std::pow (2.0, (double) wheel.deltaY * 1.5), e.position.x);
        return;
    }

    // Everything else belongs to the viewport: plain scrolling pans the roll,
    // and a shifted or horizontal wheel pans it in time.
    Component::mouseWheelMove (e, wheel);
}

void PianoRollComponent::eraseAt (juce::Point<float> position)
{
    if (! pattern)
        return;

    // notes may overlap, so keep going until the cursor is over empty grid;
    // bounded by the note count so a stuck removal cannot hang the UI
    for (int guard = pattern->getNumNotes(); --guard >= 0;)
    {
        auto note = noteAt (position);
        if (! note)
            break;

        pattern->removeNote (*note, &undoManager);
    }
}

void PianoRollComponent::eraseAlong (juce::Point<float> from, juce::Point<float> to)
{
    // Sample along the segment rather than only at the end points: mouse
    // events arrive far apart during a fast sweep, and a note is only 12px
    // tall, so testing the end point alone would skip whole rows.
    const auto distance = from.getDistanceFrom (to);
    const int steps = juce::jlimit (1, 64, (int) std::ceil (distance / 4.0f));

    for (int i = 1; i <= steps; ++i)
        eraseAt (from + (to - from) * ((float) i / (float) steps));
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! pattern || e.position.x < (float) getKeyboardWidth())
        return;

    grabKeyboardFocus();   // the tool keys and Backspace are ours once the roll is clicked

    // One transaction per gesture: everything mouseDrag does afterwards is
    // appended to it, so a whole move or erase sweep undoes in a single step.
    undoManager.beginNewTransaction();

    // A right click on a note that is part of the selection takes the whole
    // selection away. Sweeping notes out one by one is what the erase gesture
    // below is for; once a set has been picked out deliberately, "delete this"
    // is the only thing a right click on it can mean.
    //
    // Tested against the selection with some slack rather than with noteAt:
    // the exact test made this a coin toss. A press a pixel off a selected
    // note -- on its outline, in the gap between rows -- fell through to the
    // eraser, and the eraser then took out the one note under the pointer
    // (there, or a pixel later, when the secondary click jittered) and left
    // the rest selected. The outline is what the user is aiming at, so it
    // counts. Nor does an unselected note lying over a selected one get in
    // the way, as it did with noteAt: the selection is what is asked about.
    if (e.mods.isRightButtonDown() && isOnSelectedNote (e.position))
    {
        deleteSelection();
        updateCursor (e);
        return;
    }

    if (isEraseGesture (e.mods))
    {
        dragMode = DragMode::erase;
        lastErasePosition = e.position;
        eraseAt (e.position);
        updateCursor (e);
        return;
    }

    if (auto note = noteAt (e.position))
    {
        // Resizing is about the one note under the pointer, so it neither
        // reads nor changes the selection.
        if (isOverResizeZone (*note, e.position))
        {
            draggedNote = note;
            dragMode = DragMode::resize;

            // Dragging the edge of a note that is part of the selection sets
            // the length of the whole selection: giving a chord's notes the
            // same length one at a time is exactly the tedium a selection is
            // for. A note outside the selection is still only itself, which is
            // what keeps a quick resize from disturbing what is selected.
            resizeTargets = isSelected (note->state) && selectedNotes.size() > 1
                                ? selectedNotes
                                : std::vector<juce::ValueTree> { note->state };

            resizeOriginLengths.clear();

            for (const auto& state : resizeTargets)
                resizeOriginLengths.push_back (model::Note (state).getLength());

            previewNote (note->getPitch(), note->getVelocity());
            updateCursor (e);
            return;
        }

        // Cmd on a note is two gestures at once, and which one it is only
        // becomes clear when the mouse either moves or does not: a drag from
        // here duplicates the selection and moves the copy, a click alone is
        // the selection toggle it has always been. So arm both and let
        // mouseDrag/mouseUp decide.
        //
        // Whichever tool is up. Drawing a part is exactly when copying the bar
        // just drawn is worth having, and reaching for the select tool first
        // is the interruption Shift-to-select exists to avoid.
        if (isDuplicateModifier (e.mods))
        {
            pendingDuplicate = true;

            // Only a note that was already selected has a toggle to hold back;
            // adding an unselected one to the selection is what a duplicate
            // drag needs anyway, so that half happens now.
            if (isSelected (note->state))
                pendingToggleNote = note;
            else
                selectNote (*note, true);
        }
        else
        {
            selectNote (*note, selection::isExtendModifier (e.mods));

            // Cmd-clicking a note that was selected takes it back out again,
            // and then there is nothing under the pointer left to drag.
            if (! isSelected (note->state))
            {
                dragMode = DragMode::none;
                updateCursor (e);
                return;
            }
        }

        draggedNote = note;
        previewNote (note->getPitch(), note->getVelocity());

        // Remember where inside the note it was grabbed, in both axes, so
        // the note keeps its position under the cursor for the whole drag
        // instead of snapping its centre to the pointer.
        dragMode = DragMode::move;
        grabOffsetBeats = xToBeat (e.position.x) - note->getStart();
        dragStartPosition = e.position;
        beginSelectionDrag();
        updateCursor (e);
        return;
    }

    if (getEffectiveTool (e.mods) == Tool::select)
    {
        // Empty grid with the select tool: rubber band. Cmd adds to what is
        // already selected, so the band starts from the current selection
        // rather than replacing it. Shift does not: holding it is what asked
        // for the select tool in the first place, so it cannot also mean
        // "add", and a shift-band from the draw tool has to be able to start a
        // fresh selection.
        if (! selection::isRubberBandExtendModifier (e.mods))
            clearSelection();

        dragMode = DragMode::rubberBand;
        rubberBandAnchor = e.position;
        rubberBand = {};
        rubberBandBaseSelection = selectedNotes;
        updateCursor (e);
        return;
    }

    const auto minLength = minLengthBeats (e.mods);
    const auto maxStart = juce::jmax (0.0, pattern->getLengthBeats() - minLength);
    const auto start = juce::jlimit (0.0, maxStart, snapDown (xToBeat (e.position.x), e.mods));
    const auto pitch = juce::jlimit (lowestPitch, highestPitch, yToPitch (e.position.y));
    const auto length = juce::jmax (minLength,
                                    juce::jmin (lastNoteLength, pattern->getLengthBeats() - start));

    draggedNote = pattern->addNote (start, length, pitch, lastNoteVelocity, &undoManager);
    previewNote (pitch, lastNoteVelocity);

    // The new note becomes the selection, so that the drag that follows moves
    // only it and Backspace takes it away again.
    setSelection ({ draggedNote->state });

    dragMode = DragMode::move;
    grabOffsetBeats = xToBeat (e.position.x) - start;
    dragStartPosition = e.position;
    beginSelectionDrag();
    updateCursor (e);
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! pattern)
        return;

    if (dragMode == DragMode::erase)
    {
        // A press that has not yet travelled past JUCE's drag threshold is
        // still a click as far as the user is concerned: the pixel or two a
        // trackpad's secondary click drifts must not sweep the eraser across
        // a neighbouring note that was never aimed at.
        if (! e.mouseWasDraggedSinceMouseDown())
            return;

        eraseAlong (lastErasePosition, e.position);
        lastErasePosition = e.position;
        return;
    }

    if (dragMode == DragMode::rubberBand)
    {
        updateRubberBand (e.position);
        return;
    }

    if (! draggedNote)
        return;

    if (dragMode == DragMode::move)
    {
        // The press armed a duplicate and the mouse has now moved, so this is
        // one. Done once, on the first drag event: from here it is an ordinary
        // move of the copies.
        if (pendingDuplicate)
        {
            pendingDuplicate = false;
            pendingToggleNote.reset();
            duplicateSelectionForDrag();
        }
        else
        {
            // The modifier can also arrive (or go) once the drag is under way.
            syncDragDuplicate (e.mods);
        }

        // The row moves on how far the pointer has travelled since the press,
        // rounded to the nearest row -- not on which row the pointer is over.
        // The latter changes the moment the pointer crosses a row line, so a
        // note grabbed near the top of its row jumped to the next one after a
        // pixel of movement; this way it takes half a row either way.
        const auto rowsMoved = (int) std::lround ((dragStartPosition.y - e.position.y)
                                                      / (float) getRowHeight());

        dragSelectionTo (snapNearest (xToBeat (e.position.x) - grabOffsetBeats, e.mods),
                         dragAnchorOriginPitch + rowsMoved);
    }
    else if (dragMode == DragMode::resize && ! resizeOriginLengths.empty())
    {
        // The pointer sets the length of the note it grabbed; the rest of the
        // selection changes by the same amount, not to the same edge or the
        // same length, so a resize keeps the shape of what was selected.
        const auto grabbedLength = snapUp (xToBeat (e.position.x) - draggedNote->getStart(), e.mods);
        resizeSelectionBy (grabbedLength - resizeOriginLengths.front(), e.mods);
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent& e)
{
    if (draggedNote && dragMode == DragMode::resize)
        lastNoteLength = draggedNote->getLength();

    // An armed duplicate that never became a drag was a plain Cmd click, and
    // those toggle the note out of the selection.
    if (pendingToggleNote)
        selectNote (*pendingToggleNote, true);

    pendingDuplicate = false;
    pendingToggleNote.reset();
    originCopies.clear();   // they stay in the pattern; only the handle on them goes

    dragMode = DragMode::none;
    draggedNote.reset();
    dragOriginStarts.clear();
    dragOriginPitches.clear();
    resizeTargets.clear();
    resizeOriginLengths.clear();
    rubberBand = {};
    rubberBandBaseSelection.clear();
    updateCursor (e);
    repaint();
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }

    if (key == juce::KeyPress ('d'))
    {
        setTool (Tool::draw);
        return true;
    }

    if (key == juce::KeyPress ('e'))
    {
        setTool (Tool::select);
        return true;
    }

    if (key == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0))
    {
        copySelection();
        return true;
    }

    if (key == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0))
    {
        cutSelection();
        return true;
    }

    if (key == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0))
    {
        pasteNotes();
        return true;
    }

    // Ctrl as well as Cmd, for hands that learned it on a PC: Ctrl+A has no
    // other meaning here, unlike the other Cmd shortcuts.
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0)
         || key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0))
    {
        selectAllNotes();
        return true;
    }

    if (key == juce::KeyPress ('q'))
    {
        // Repeats whatever the panel was last left on, so that the panel is
        // opened once to dial a feel in and then stays shut.
        quantiseNotes();
        return true;
    }

    if (key == juce::KeyPress ('-') || key == juce::KeyPress ('='))
    {
        zoomBy (key == juce::KeyPress ('=') ? 1.5 : 1.0 / 1.5);
        return true;
    }

    // MainComponent owns Space and Cmd+Z / Shift+Cmd+Z globally: let everything
    // else bubble up through the window to it.
    return false;
}

//==============================================================================
PianoRollRuler::PianoRollRuler (const PianoRollComponent& rollToFollow)
    : roll (rollToFollow)
{
}

void PianoRollRuler::setScrollOffset (int offsetX)
{
    if (offsetX != scrollOffset)
    {
        scrollOffset = offsetX;
        repaint();
    }
}

void PianoRollRuler::seekTo (float x)
{
    if (! onSeek)
        return;

    // The ruler shows the roll shifted by the scroll offset, so a click maps
    // back through the roll's own geometry. Clamped rather than rejected:
    // pressing past either end means "from the start" / "from the end".
    const auto beat = juce::jlimit (0.0, roll.getLengthBeats(),
                                    roll.xToBeat (x + (float) scrollOffset));
    onSeek (beat);
}

void PianoRollRuler::mouseDown (const juce::MouseEvent& e)
{
    seekTo (e.position.x);
}

void PianoRollRuler::mouseDrag (const juce::MouseEvent& e)
{
    seekTo (e.position.x);   // scrub
}

void PianoRollRuler::paint (juce::Graphics& g)
{
    const auto height = (float) getHeight();
    const auto width = (float) getWidth();

    g.fillAll (juce::Colour (0xff2a2a2f));

    // Everything left of beat 0 sits over the roll's keyboard column, which
    // scrolls away with the content, so the blank corner shrinks with it.
    const auto originX = roll.beatToX (0.0) - (float) scrollOffset;

    if (originX > 0.0f)
    {
        g.setColour (juce::Colour (0xff232327));
        g.fillRect (0.0f, 0.0f, juce::jmin (originX, width), height);
    }

    const auto lengthBeats = roll.getLengthBeats();
    const auto beatsPerBar = roll.getBeatsPerBar();
    const auto pixelsPerBeat = roll.getPixelsPerBeat();

    // Thinned the same way the roll's grid is: beat ticks only while they have
    // room, and bar numbers only every 2nd / 4th / ... bar once the labels
    // would otherwise run into each other.
    const bool drawBeatTicks = pixelsPerBeat >= 18.0;
    int barStep = 1;
    while ((double) barStep * beatsPerBar * pixelsPerBeat < 34.0)
        barStep *= 2;

    // Only walk what can actually land on screen. Stepping by index keeps the
    // marks on the same coordinates the roll's grid uses -- and, as there, the
    // beats and the bars are walked separately, because a bar line only falls
    // on a whole beat in signatures whose bar is a whole number of them.
    const auto firstVisibleBeat = roll.xToBeat ((float) scrollOffset);
    const auto lastVisibleBeat = juce::jmin (lengthBeats,
                                             (double) roll.xToBeat ((float) scrollOffset + width));

    g.setFont (uiFont (fonts::small));

    if (drawBeatTicks)
    {
        const int firstBeat = juce::jmax (0, (int) std::floor (firstVisibleBeat));
        const int lastBeat = (int) std::floor (lastVisibleBeat);

        g.setColour (juce::Colour (0xff43434a));

        for (int beat = firstBeat; beat <= lastBeat; ++beat)
            if (! isMultipleOf ((double) beat, beatsPerBar))
                g.drawVerticalLine ((int) (roll.beatToX ((double) beat) - (float) scrollOffset),
                                    height * 0.65f, height);
    }

    const int firstBar = juce::jmax (0, (int) std::floor (firstVisibleBeat / beatsPerBar));
    const int lastBar = (int) std::floor (lastVisibleBeat / beatsPerBar + 1.0e-9);

    for (int bar = firstBar; bar <= lastBar; ++bar)
    {
        if (bar % barStep != 0)
            continue;

        const auto x = roll.beatToX ((double) bar * beatsPerBar) - (float) scrollOffset;

        g.setColour (juce::Colour (0xff6a6a74));
        g.drawVerticalLine ((int) x, height * 0.35f, height);

        // Bars are numbered from one, as musicians count them. These are the
        // pattern's own bars, not the song's: the pattern is not anchored
        // anywhere, so bar 1 is wherever the pattern starts.
        g.setColour (juce::Colour (0xffa0a0aa));
        g.drawText (juce::String (bar + 1),
                    juce::Rectangle<float> (x + 3.0f, 1.0f,
                                            (float) (beatsPerBar * pixelsPerBeat) - 6.0f,
                                            height * 0.6f),
                    juce::Justification::centredLeft, false);
    }

    // end of the pattern, matching the marker the roll draws
    const auto endX = roll.beatToX (lengthBeats) - (float) scrollOffset;

    if (endX >= 0.0f && endX <= width)
    {
        g.setColour (juce::Colour (0xff6a6a74));
        g.drawVerticalLine ((int) endX, 0.0f, height);
    }

    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, width);
}

//==============================================================================
PianoRollVelocityLane::PianoRollVelocityLane (PianoRollComponent& rollToEdit)
    : roll (rollToEdit)
{
}

void PianoRollVelocityLane::setScrollOffset (int offsetX)
{
    if (offsetX != scrollOffset)
    {
        scrollOffset = offsetX;
        repaint();
    }
}

juce::Range<float> PianoRollVelocityLane::barSpan (const model::Note& note) const
{
    const auto x = roll.beatToX (note.getStart()) - (float) scrollOffset;

    // A bar is as wide as its note, down to a floor: zoomed out, or on a very
    // short note, the note's own width is less than a pixel and the bar would
    // be neither visible nor hittable.
    const auto width = juce::jmax (minBarWidth,
                                   (float) (note.getLength() * roll.getPixelsPerBeat()) - 1.0f);

    return { x, x + width };
}

juce::Rectangle<float> PianoRollVelocityLane::barBounds (const model::Note& note) const
{
    const auto span = barSpan (note);
    const auto bottom = barsBottom();

    // A minimum of two pixels so that the quietest note is still something to
    // look at rather than a gap in the row of bars.
    const auto height = juce::jmax (2.0f, (bottom - barsTop()) * (float) note.getVelocity()
                                              / (float) model::Note::loudestVelocity);

    return { span.getStart(), bottom - height, span.getLength(), height };
}

int PianoRollVelocityLane::velocityAtY (float y) const
{
    const auto bottom = barsBottom();
    const auto proportion = (bottom - y) / juce::jmax (1.0f, bottom - barsTop());

    return model::Note::clampVelocity (juce::roundToInt (proportion * (float) model::Note::loudestVelocity));
}

bool PianoRollVelocityLane::isOverEditableBar (juce::Point<float> position) const
{
    for (const auto& note : roll.getNotes())
        if (roll.isVelocityEditable (note) && barSpan (note).contains (position.x))
            return true;

    return false;
}

bool PianoRollVelocityLane::isOverSelectedBar (juce::Point<float> position) const
{
    for (const auto& note : roll.getNotes())
        if (roll.isNoteSelected (note) && barSpan (note).contains (position.x))
            return true;

    return false;
}

bool PianoRollVelocityLane::startsGroupDrag (juce::Point<float> position) const
{
    return roll.getNumSelectedNotes() > 1 && isOverSelectedBar (position);
}

void PianoRollVelocityLane::paint (juce::Graphics& g)
{
    const auto width = (float) getWidth();
    const auto height = (float) getHeight();
    const auto bottom = barsBottom();

    g.fillAll (juce::Colour (0xff1f1f24));

    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine (0, 0.0f, width);

    // Everything left of beat 0 sits over the roll's keyboard column, which
    // scrolls away with the content -- the same arrangement the ruler above
    // the roll uses, so the two edges move together.
    const auto originX = roll.beatToX (0.0) - (float) scrollOffset;

    if (originX > 0.0f)
    {
        const auto gutter = juce::jmin (originX, width);

        g.setColour (juce::Colour (0xff232327));
        g.fillRect (0.0f, 1.0f, gutter, height - 1.0f);

        g.setColour (juce::Colour (0xff707078));
        g.setFont (uiFont (fonts::small));
        g.drawText ("Vel", juce::Rectangle<float> (2.0f, 0.0f, gutter - 6.0f, height),
                    juce::Justification::centredRight, false);
    }

    // Bar lines only: the lane is read against the grid above it rather than
    // on its own, and beat lines at this height would be more noise than help.
    const auto beatsPerBar = roll.getBeatsPerBar();
    const auto lengthBeats = roll.getLengthBeats();

    g.setColour (juce::Colour (0xff2f2f35));

    for (int bar = 0; (double) bar * beatsPerBar <= lengthBeats; ++bar)
    {
        const auto x = roll.beatToX ((double) bar * beatsPerBar) - (float) scrollOffset;

        if (x >= originX && x <= width)
            g.drawVerticalLine ((int) x, 1.0f, height);
    }

    g.setColour (juce::Colour (0xff3a3a40));
    g.fillRect (0.0f, bottom, width, baselineHeight);

    for (const auto& note : roll.getNotes())
    {
        const auto bounds = barBounds (note);

        if (bounds.getRight() < originX || bounds.getX() > width)
            continue;

        // Dimmed when a selection rules the note out, because then a drag in
        // here will not touch it -- the lane says what it will do before it
        // does it.
        // Full strength whatever the velocity: the bar's height already says
        // the value here, and a washed-out bar would only be harder to grab.
        auto colour = noteColour;

        if (roll.isNoteSelected (note))
            colour = colour.brighter (0.4f);

        if (! roll.isVelocityEditable (note))
            colour = colour.withAlpha (0.25f);

        g.setColour (colour);
        g.fillRect (bounds);

        // A brighter cap, so a row of bars can be read as a shape rather than
        // as a block of colour.
        g.setColour (colour.brighter (0.5f));
        g.fillRect (bounds.withHeight (1.5f));
    }
}

void PianoRollVelocityLane::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOverEditableBar (e.position) ? juce::MouseCursor::UpDownResizeCursor
                                                   : juce::MouseCursor::NormalCursor);
}

void PianoRollVelocityLane::mouseDown (const juce::MouseEvent& e)
{
    // The gesture starts anywhere in the lane rather than only on a bar: a
    // sweep across a phrase is usually begun just before the first note of it,
    // and an empty transaction costs nothing.
    roll.beginVelocityGesture();
    dragging = true;
    lastDragPosition = e.position;

    // A press on the selection's own bars moves the selection, and moves it
    // by nothing until the pointer does: the bars must not jump to wherever
    // in the lane the press happened to land.
    groupDragging = startsGroupDrag (e.position);
    groupDragOriginVelocity = velocityAtY (e.position.y);

    if (! groupDragging)
        applySweep (e.position, e.position);
}

void PianoRollVelocityLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    if (groupDragging)
    {
        // How far the pointer has come in velocity terms, so the lane's height
        // means the same thing whether one note or a chord is being dragged.
        roll.offsetSelectionVelocity (velocityAtY (e.position.y) - groupDragOriginVelocity);
        return;
    }

    applySweep (lastDragPosition, e.position);
    lastDragPosition = e.position;
}

void PianoRollVelocityLane::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    groupDragging = false;
}

void PianoRollVelocityLane::applySweep (juce::Point<float> from, juce::Point<float> to)
{
    const auto left = juce::jmin (from.x, to.x);
    const auto right = juce::jmax (from.x, to.x);

    // Where the pointer was when it passed over a given x. Mouse events arrive
    // far apart during a fast drag, and giving every note the segment swept
    // the same velocity would flatten the ramp the user just drew.
    const auto yAt = [from, to] (float x)
    {
        if (std::abs (to.x - from.x) < 1.0f)
            return to.y;

        return from.y + (to.y - from.y) * juce::jlimit (0.0f, 1.0f, (x - from.x) / (to.x - from.x));
    };

    // Copied out before anything is written: each write calls the roll's own
    // listener back synchronously, and that may rewrite the selection.
    const auto notes = roll.getNotes();

    for (const auto& note : notes)
    {
        if (! roll.isVelocityEditable (note))
            continue;

        const auto span = barSpan (note);

        if (span.getEnd() < left || span.getStart() > right)
            continue;

        roll.setNoteVelocity (note, velocityAtY (yAt (span.getStart())));
    }
}

} // namespace carve::app
