#include <algorithm>
#include <cmath>

#include "PianoRollComponent.h"

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
} // namespace

PianoRollComponent::PianoRollComponent (juce::UndoManager& um)
    : undoManager (um)
{
    // the tool keys, the zoom keys and Backspace are ours once the roll is clicked
    setWantsKeyboardFocus (true);
    updateSize();
}

PianoRollComponent::~PianoRollComponent()
{
    if (pattern)
        pattern->state.removeListener (this);
}

void PianoRollComponent::setPattern (std::optional<model::Pattern> newPattern)
{
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
    repaint();
}

juce::Viewport* PianoRollComponent::getViewport() const
{
    return findParentComponentOfClass<juce::Viewport>();
}

void PianoRollComponent::updateSize()
{
    auto width = keyboardWidth + juce::roundToInt (getLengthBeats() * pixelsPerBeat);

    // Zoomed far enough out a short pattern is narrower than the viewport;
    // stay at least as wide as it so the roll's own background, and not the
    // viewport's, fills the window.
    if (auto* viewport = getViewport())
        width = juce::jmax (width, viewport->getMaximumVisibleWidth());

    setSize (width, (highestPitch - lowestPitch + 1) * rowHeight);
}

double PianoRollComponent::getLengthBeats() const
{
    return pattern ? pattern->getLengthBeats() : defaultLengthBeats;
}

double PianoRollComponent::xToBeat (float x) const     { return (x - (float) keyboardWidth) / pixelsPerBeat; }
float PianoRollComponent::beatToX (double beat) const  { return (float) (keyboardWidth + beat * pixelsPerBeat); }
int PianoRollComponent::yToPitch (float y) const       { return highestPitch - (int) (y / rowHeight); }
float PianoRollComponent::pitchToY (int pitch) const   { return (float) ((highestPitch - pitch) * rowHeight); }

void PianoRollComponent::setPixelsPerBeat (double newPixelsPerBeat, float anchorX)
{
    newPixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, newPixelsPerBeat);

    if (std::abs (newPixelsPerBeat - pixelsPerBeat) < 1.0e-6)
        return;

    // Keep whatever is under anchorX pinned to the same place on screen, so
    // zooming feels like it happens around the pointer rather than around the
    // start of the pattern.
    auto* viewport = getViewport();
    const auto anchorBeat = xToBeat (anchorX);
    const auto anchorScreenX = juce::roundToInt (anchorX)
                                   - (viewport != nullptr ? viewport->getViewPositionX() : 0);

    pixelsPerBeat = newPixelsPerBeat;
    updateSize();

    if (viewport != nullptr)
        viewport->setViewPosition (juce::jmax (0, juce::roundToInt (beatToX (anchorBeat)) - anchorScreenX),
                                   viewport->getViewPositionY());

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

double PianoRollComponent::snapDown (double beat) const
{
    if (! snapEnabled)
        return beat;

    return std::floor (beat / gridBeats + 1.0e-9) * gridBeats;
}

double PianoRollComponent::snapUp (double beat) const
{
    if (! snapEnabled)
        return beat;

    return std::ceil (beat / gridBeats - 1.0e-9) * gridBeats;
}

double PianoRollComponent::minLengthBeats() const
{
    return snapEnabled ? gridBeats : freeMinLengthBeats;
}

juce::Rectangle<float> PianoRollComponent::noteBounds (const model::Note& note) const
{
    return { beatToX (note.getStart()), pitchToY (note.getPitch()),
             (float) (note.getLength() * pixelsPerBeat), (float) rowHeight };
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

    // Clamp the gesture rather than each note: the selection keeps its shape
    // when it runs into an end of the pattern or of the keyboard, instead of
    // collapsing onto the boundary.
    auto lowestStart = dragOriginStarts.front();
    auto highestEnd = 0.0;
    auto lowestPitchInSet = dragOriginPitches.front();
    auto highestPitchInSet = dragOriginPitches.front();

    for (size_t i = 0; i < notes.size(); ++i)
    {
        lowestStart = juce::jmin (lowestStart, dragOriginStarts[i]);
        highestEnd = juce::jmax (highestEnd, dragOriginStarts[i] + model::Note (notes[i]).getLength());
        lowestPitchInSet = juce::jmin (lowestPitchInSet, dragOriginPitches[i]);
        highestPitchInSet = juce::jmax (highestPitchInSet, dragOriginPitches[i]);
    }

    // jmin/jmax around zero so that a note already sitting outside the pattern
    // (left behind by a shorten) cannot make the limits cross over.
    const auto deltaBeats = juce::jlimit (juce::jmin (0.0, -lowestStart),
                                          juce::jmax (0.0, pattern->getLengthBeats() - highestEnd),
                                          anchorStart - dragAnchorOriginStart);
    const auto deltaPitch = juce::jlimit (juce::jmin (0, lowestPitch - lowestPitchInSet),
                                          juce::jmax (0, highestPitch - highestPitchInSet),
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
        previewNote (anchorNewPitch, draggedNote ? draggedNote->getVelocity() : newNoteVelocity);
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
void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const auto lengthBeats = getLengthBeats();
    const auto gridRight = beatToX (lengthBeats);

    // rows
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const auto y = pitchToY (pitch);
        if (isBlackKey (pitch))
        {
            g.setColour (juce::Colour (0xff1c1c20));
            g.fillRect ((float) keyboardWidth, y, gridRight - keyboardWidth, (float) rowHeight);
        }
        if (pitch % 12 == 0)   // octave line above each C row
        {
            g.setColour (juce::Colour (0xff3a3a40));
            g.drawHorizontalLine ((int) y + rowHeight, (float) keyboardWidth, gridRight);
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

    for (int beat = 0; (double) beat <= lengthBeats; ++beat)
    {
        const bool isBar = (beat % (int) beatsPerBar) == 0;

        if (! isBar && ! drawBeats)
            continue;

        g.setColour (isBar ? juce::Colour (0xff55555e) : juce::Colour (0xff3a3a40));

        g.drawVerticalLine ((int) beatToX ((double) beat), 0.0f, (float) getHeight());
    }

    // end of the pattern, which an off-grid length would otherwise not mark
    g.setColour (juce::Colour (0xff6a6a74));
    g.drawVerticalLine ((int) gridRight, 0.0f, (float) getHeight());

    // notes
    if (pattern)
    {
        for (const auto& note : pattern->getNotes())
        {
            auto r = noteBounds (note).reduced (0.0f, 1.0f);
            const auto brightness = 0.55f + 0.45f * (float) note.getVelocity() / 127.0f;
            const bool selected = isSelected (note.state);

            g.setColour (juce::Colour (0xffe08a3c).withMultipliedBrightness (brightness)
                             .brighter (selected ? 0.4f : 0.0f));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (r, 2.0f, selected ? 1.6f : 1.0f);
        }
    }

    if (dragMode == DragMode::rubberBand && ! rubberBand.isEmpty())
    {
        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.8f));
        g.drawRect (rubberBand, 1.0f);
    }

    // keyboard column
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const auto y = pitchToY (pitch);
        g.setColour (isBlackKey (pitch) ? juce::Colour (0xff2a2a2e) : juce::Colour (0xffd8d8dc));
        g.fillRect (0.0f, y, (float) keyboardWidth - 2.0f, (float) rowHeight - 0.5f);

        if (pitch % 12 == 0)
        {
            g.setColour (juce::Colour (0xff707078));
            g.setFont (9.0f);
            g.drawText ("C" + juce::String (pitch / 12 - 1),
                        2, (int) y, keyboardWidth - 8, rowHeight, juce::Justification::centredRight);
        }
    }
}

juce::MouseCursor PianoRollComponent::cursorFor (juce::Point<float> position,
                                                 const juce::ModifierKeys& mods) const
{
    // the keyboard column is not editable, and neither is an empty editor
    if (! pattern || position.x < (float) keyboardWidth)
        return juce::MouseCursor::NormalCursor;

    if (isEraseGesture (mods))
        return juce::MouseCursor::CrosshairCursor;

    if (auto note = noteAt (position))
        return isOverResizeZone (*note, position) ? juce::MouseCursor::LeftRightResizeCursor
                                                  : juce::MouseCursor::DraggingHandCursor;

    // empty grid: the draw tool would add a note here, the select tool would
    // start a rubber band
    return tool == Tool::select ? juce::MouseCursor::CrosshairCursor
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
    if (! pattern || e.position.x < (float) keyboardWidth)
        return;

    grabKeyboardFocus();   // the tool keys and Backspace are ours once the roll is clicked

    // One transaction per gesture: everything mouseDrag does afterwards is
    // appended to it, so a whole move or erase sweep undoes in a single step.
    undoManager.beginNewTransaction();

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
            previewNote (note->getPitch(), note->getVelocity());
            updateCursor (e);
            return;
        }

        selectNote (*note, e.mods.isCommandDown() || e.mods.isShiftDown());

        // Cmd-clicking a note that was selected takes it back out again, and
        // then there is nothing under the pointer left to drag.
        if (! isSelected (note->state))
        {
            dragMode = DragMode::none;
            updateCursor (e);
            return;
        }

        draggedNote = note;
        previewNote (note->getPitch(), note->getVelocity());

        // Remember where inside the note it was grabbed, in both axes, so
        // the note keeps its position under the cursor for the whole drag
        // instead of snapping its centre to the pointer.
        dragMode = DragMode::move;
        grabOffsetBeats = xToBeat (e.position.x) - note->getStart();
        grabPitchOffset = note->getPitch() - yToPitch (e.position.y);
        beginSelectionDrag();
        updateCursor (e);
        return;
    }

    if (tool == Tool::select)
    {
        // Empty grid with the select tool: rubber band. Cmd or Shift adds to
        // what is already selected, so the band starts from the current
        // selection rather than replacing it.
        if (! (e.mods.isCommandDown() || e.mods.isShiftDown()))
            clearSelection();

        dragMode = DragMode::rubberBand;
        rubberBandAnchor = e.position;
        rubberBand = {};
        rubberBandBaseSelection = selectedNotes;
        updateCursor (e);
        return;
    }

    const auto maxStart = juce::jmax (0.0, pattern->getLengthBeats() - minLengthBeats());
    const auto start = juce::jlimit (0.0, maxStart, snapDown (xToBeat (e.position.x)));
    const auto pitch = juce::jlimit (lowestPitch, highestPitch, yToPitch (e.position.y));
    const auto length = juce::jmax (minLengthBeats(),
                                    juce::jmin (lastNoteLength, pattern->getLengthBeats() - start));

    draggedNote = pattern->addNote (start, length, pitch, newNoteVelocity, &undoManager);
    previewNote (pitch, newNoteVelocity);

    // The new note becomes the selection, so that the drag that follows moves
    // only it and Backspace takes it away again.
    setSelection ({ draggedNote->state });

    dragMode = DragMode::move;
    grabOffsetBeats = xToBeat (e.position.x) - start;
    grabPitchOffset = 0;   // a new note is created on the row under the cursor
    beginSelectionDrag();
    updateCursor (e);
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! pattern)
        return;

    if (dragMode == DragMode::erase)
    {
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
        dragSelectionTo (snapDown (xToBeat (e.position.x) - grabOffsetBeats),
                         yToPitch (e.position.y) + grabPitchOffset);
    }
    else if (dragMode == DragMode::resize)
    {
        const auto maxLength = juce::jmax (minLengthBeats(),
                                           pattern->getLengthBeats() - draggedNote->getStart());
        const auto length = juce::jlimit (minLengthBeats(), maxLength,
                                          snapUp (xToBeat (e.position.x) - draggedNote->getStart()));

        if (! juce::exactlyEqual (length, draggedNote->getLength()))
            draggedNote->setLength (length, &undoManager);
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent& e)
{
    if (draggedNote && dragMode == DragMode::resize)
        lastNoteLength = draggedNote->getLength();

    dragMode = DragMode::none;
    draggedNote.reset();
    dragOriginStarts.clear();
    dragOriginPitches.clear();
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
    // purely a read-out: clicks belong to whatever is behind it
    setInterceptsMouseClicks (false, false);
}

void PianoRollRuler::setScrollOffset (int offsetX)
{
    if (offsetX != scrollOffset)
    {
        scrollOffset = offsetX;
        repaint();
    }
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
    const auto beatsPerBar = PianoRollComponent::beatsPerBar;
    const auto pixelsPerBeat = roll.getPixelsPerBeat();

    // Thinned the same way the roll's grid is: beat ticks only while they have
    // room, and bar numbers only every 2nd / 4th / ... bar once the labels
    // would otherwise run into each other.
    const bool drawBeatTicks = pixelsPerBeat >= 18.0;
    int barStep = 1;
    while ((double) barStep * beatsPerBar * pixelsPerBeat < 34.0)
        barStep *= 2;

    // Only walk the beats that can actually land on screen. Stepping by index
    // keeps the marks on the same coordinates the roll's grid uses.
    const auto firstVisibleBeat = roll.xToBeat ((float) scrollOffset);
    const int firstBeat = juce::jmax (0, (int) std::floor (firstVisibleBeat));
    const int lastBeat = (int) std::floor (juce::jmin (lengthBeats,
                                                       roll.xToBeat ((float) scrollOffset + width)));

    g.setFont (10.0f);

    for (int beat = firstBeat; beat <= lastBeat; ++beat)
    {
        const auto x = roll.beatToX ((double) beat) - (float) scrollOffset;
        const bool isBar = (beat % (int) beatsPerBar) == 0;

        if (isBar && (beat / (int) beatsPerBar) % barStep != 0)
            continue;

        if (! isBar && ! drawBeatTicks)
            continue;

        g.setColour (isBar ? juce::Colour (0xff6a6a74) : juce::Colour (0xff43434a));
        g.drawVerticalLine ((int) x, isBar ? height * 0.35f : height * 0.65f, height);

        if (isBar)
        {
            // Bars are numbered from one, as musicians count them.
            g.setColour (juce::Colour (0xffa0a0aa));
            g.drawText (juce::String (beat / (int) beatsPerBar + 1),
                        juce::Rectangle<float> (x + 3.0f, 1.0f,
                                                (float) (beatsPerBar * pixelsPerBeat) - 6.0f,
                                                height * 0.6f),
                        juce::Justification::centredLeft, false);
        }
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

} // namespace carve::app
