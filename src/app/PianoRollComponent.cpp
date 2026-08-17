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

    // vertical grid
    for (double beat = 0.0; beat <= lengthBeats + 1.0e-9; beat += gridBeats)
    {
        const bool isBar = std::abs (std::fmod (beat, 4.0)) < 1.0e-9;
        const bool isBeat = std::abs (std::fmod (beat, 1.0)) < 1.0e-9;
        g.setColour (isBar ? juce::Colour (0xff55555e)
                           : isBeat ? juce::Colour (0xff3a3a40)
                                    : juce::Colour (0xff2c2c31));
        g.drawVerticalLine ((int) beatToX (beat), 0.0f, (float) getHeight());
    }

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

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! pattern || e.position.x < (float) keyboardWidth)
        return;

    undoManager.beginNewTransaction();

    if (auto note = noteAt (e.position))
    {
        if (e.mods.isRightButtonDown() || e.mods.isAltDown())
        {
            pattern->removeNote (*note, &undoManager);
            return;
        }

        draggedNote = note;
        const auto bounds = noteBounds (*note);
        if (e.position.x > bounds.getRight() - 6.0f)
        {
            dragMode = DragMode::resize;
        }
        else
        {
            dragMode = DragMode::move;
            grabOffsetBeats = xToBeat (e.position.x) - note->getStart();
        }
        return;
    }

    if (e.mods.isRightButtonDown() || e.mods.isAltDown())
        return;

    const auto start = juce::jlimit (0.0, pattern->getLengthBeats() - gridBeats,
                                     snap (xToBeat (e.position.x)));
    const auto pitch = juce::jlimit (lowestPitch, highestPitch, yToPitch (e.position.y));
    const auto length = juce::jmin (lastNoteLength, pattern->getLengthBeats() - start);

    draggedNote = pattern->addNote (start, length, pitch, 100, &undoManager);
    dragMode = DragMode::move;
    grabOffsetBeats = xToBeat (e.position.x) - start;
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! pattern || ! draggedNote || dragMode == DragMode::none)
        return;

    if (dragMode == DragMode::move)
    {
        const auto start = juce::jlimit (0.0, pattern->getLengthBeats() - draggedNote->getLength(),
                                         snap (xToBeat (e.position.x) - grabOffsetBeats));
        const auto pitch = juce::jlimit (lowestPitch, highestPitch, yToPitch (e.position.y));
        draggedNote->setStart (start, &undoManager);
        draggedNote->setPitch (pitch, &undoManager);
    }
    else if (dragMode == DragMode::resize)
    {
        const auto rawLength = xToBeat (e.position.x) - draggedNote->getStart();
        const auto snapped = std::ceil (rawLength / gridBeats) * gridBeats;
        const auto length = juce::jlimit (gridBeats,
                                          pattern->getLengthBeats() - draggedNote->getStart(),
                                          snapped);
        draggedNote->setLength (length, &undoManager);
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    if (draggedNote && dragMode == DragMode::resize)
        lastNoteLength = draggedNote->getLength();

    dragMode = DragMode::none;
    draggedNote.reset();
}

} // namespace orionish::app
