#include <cmath>

#include "PianoRollComponent.h"

namespace orionish::app
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

    if (pattern)
        pattern->state.addListener (this);

    updateSize();
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

void PianoRollComponent::patternChanged()
{
    // The pattern length drives the component width, so any property change
    // may need a resize. setSize() early-outs when nothing moved, which makes
    // this cheap enough to run for note edits too.
    updateSize();
    repaint();
}

void PianoRollComponent::updateSize()
{
    const double lengthBeats = pattern ? pattern->getLengthBeats() : 16.0;
    setSize (keyboardWidth + juce::roundToInt (lengthBeats * pixelsPerBeat),
             (highestPitch - lowestPitch + 1) * rowHeight);
}

double PianoRollComponent::xToBeat (float x) const     { return (x - (float) keyboardWidth) / pixelsPerBeat; }
float PianoRollComponent::beatToX (double beat) const  { return (float) (keyboardWidth + beat * pixelsPerBeat); }
int PianoRollComponent::yToPitch (float y) const       { return highestPitch - (int) (y / rowHeight); }
float PianoRollComponent::pitchToY (int pitch) const   { return (float) ((highestPitch - pitch) * rowHeight); }

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

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const double lengthBeats = pattern ? pattern->getLengthBeats() : 16.0;
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

    // Vertical grid. Stepped by index rather than by accumulating gridBeats so
    // that a triplet grid does not drift across a long pattern.
    const int numGridLines = (int) std::floor (lengthBeats / gridBeats + 1.0e-9);
    for (int i = 0; i <= numGridLines; ++i)
    {
        const double beat = (double) i * gridBeats;
        const bool isBar = isMultipleOf (beat, beatsPerBar);
        const bool isBeat = isMultipleOf (beat, 1.0);
        g.setColour (isBar ? juce::Colour (0xff55555e)
                           : isBeat ? juce::Colour (0xff3a3a40)
                                    : juce::Colour (0xff2c2c31));
        g.drawVerticalLine ((int) beatToX (beat), 0.0f, (float) getHeight());
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
            g.setColour (juce::Colour (0xffe08a3c).withMultipliedBrightness (brightness));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
        }
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

    return juce::MouseCursor::NormalCursor;
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
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    updateCursor (e);
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
        draggedNote = note;

        if (isOverResizeZone (*note, e.position))
        {
            dragMode = DragMode::resize;
        }
        else
        {
            // Remember where inside the note it was grabbed, in both axes, so
            // the note keeps its position under the cursor for the whole drag
            // instead of snapping its centre to the pointer.
            dragMode = DragMode::move;
            grabOffsetBeats = xToBeat (e.position.x) - note->getStart();
            grabPitchOffset = note->getPitch() - yToPitch (e.position.y);
        }

        updateCursor (e);
        return;
    }

    const auto maxStart = juce::jmax (0.0, pattern->getLengthBeats() - minLengthBeats());
    const auto start = juce::jlimit (0.0, maxStart, snapDown (xToBeat (e.position.x)));
    const auto pitch = juce::jlimit (lowestPitch, highestPitch, yToPitch (e.position.y));
    const auto length = juce::jmax (minLengthBeats(),
                                    juce::jmin (lastNoteLength, pattern->getLengthBeats() - start));

    draggedNote = pattern->addNote (start, length, pitch, 100, &undoManager);
    dragMode = DragMode::move;
    grabOffsetBeats = xToBeat (e.position.x) - start;
    grabPitchOffset = 0;   // a new note is created on the row under the cursor
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

    if (! draggedNote)
        return;

    if (dragMode == DragMode::move)
    {
        const auto maxStart = juce::jmax (0.0, pattern->getLengthBeats() - draggedNote->getLength());
        const auto start = juce::jlimit (0.0, maxStart,
                                         snapDown (xToBeat (e.position.x) - grabOffsetBeats));
        const auto pitch = juce::jlimit (lowestPitch, highestPitch,
                                         yToPitch (e.position.y) + grabPitchOffset);

        // Only write when the result actually moved: every property change
        // triggers a full EditSync resync, and a drag produces a lot of events.
        if (! juce::exactlyEqual (start, draggedNote->getStart()))
            draggedNote->setStart (start, &undoManager);

        if (pitch != draggedNote->getPitch())
            draggedNote->setPitch (pitch, &undoManager);
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
    updateCursor (e);
}

} // namespace orionish::app
