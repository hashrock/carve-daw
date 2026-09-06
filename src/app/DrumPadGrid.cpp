#include "DrumPadGrid.h"

namespace carve::app
{

namespace
{
    // Four pads per row. Wide enough for a truncated sample name, tall enough
    // to carry the note badge above it and to be hit without aiming -- these
    // are played by hand now, not only assigned to.
    constexpr int padRowHeight = 48;
    constexpr int padGap = 3;
} // namespace

//==============================================================================
// drumkit

std::optional<int> drumkit::getPadForNote (int midiNote)
{
    const int pad = midiNote - firstNote;

    if (pad >= 0 && pad < numPads)
        return pad;

    return std::nullopt;
}

std::optional<model::SamplerSound> drumkit::findSoundForPad (const model::Generator& generator, int pad)
{
    if (pad < 0 || pad >= numPads)
        return std::nullopt;

    const int note = getNoteForPad (pad);

    // The key range alone decides which pad a sound answers to: the root note
    // is free to differ so a sample can be assigned and then transposed, and a
    // sound spanning more than one note is nobody's pad.
    for (const auto& sound : generator.getSounds())
        if (sound.getMinNote() == note && sound.getMaxNote() == note)
            return sound;

    return std::nullopt;
}

juce::String drumkit::getNoteName (int midiNote)
{
    // Octave 3 for middle C, matching SamplerSound's default root of C3, so a
    // pad at 36 reads "C1" the way drum-machine documentation writes it.
    return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 3);
}

//==============================================================================
// DrumPadGrid

DrumPadGrid::DrumPadGrid()
{
    clearPads();
}

int DrumPadGrid::getPreferredHeight()
{
    return drumkit::numRows * padRowHeight + (drumkit::numRows - 1) * padGap;
}

void DrumPadGrid::clearPads()
{
    pads.fill ({});
}

void DrumPadGrid::setPadState (int pad, const juce::String& sampleName, bool usedByPattern)
{
    if (pad >= 0 && pad < drumkit::numPads)
        pads[(size_t) pad] = { sampleName, usedByPattern };
}

juce::Rectangle<int> DrumPadGrid::getPadBounds (int pad) const
{
    if (pad < 0 || pad >= drumkit::numPads)
        return {};

    const int column = pad % drumkit::numColumns;

    // Pad 0 is bottom-left, so the lowest row is drawn last.
    const int row = drumkit::numRows - 1 - pad / drumkit::numColumns;

    const int usable = getWidth() - (drumkit::numColumns - 1) * padGap;
    const int cellWidth = juce::jmax (16, usable / drumkit::numColumns);

    return { column * (cellWidth + padGap), row * (padRowHeight + padGap),
             cellWidth, padRowHeight };
}

std::optional<int> DrumPadGrid::getPadAt (juce::Point<int> position) const
{
    for (int pad = 0; pad < drumkit::numPads; ++pad)
        if (getPadBounds (pad).contains (position))
            return pad;

    return std::nullopt;
}

void DrumPadGrid::paint (juce::Graphics& g)
{
    for (int pad = 0; pad < drumkit::numPads; ++pad)
    {
        const auto& state = pads[(size_t) pad];
        const auto bounds = getPadBounds (pad);
        const bool isFilled = state.sampleName.isNotEmpty();

        g.setColour (isFilled ? juce::Colour (0xff3f4750) : juce::Colour (0xff2b2b30));
        g.fillRoundedRectangle (bounds.toFloat(), 3.0f);

        // The pattern being edited plays this note: the same accent the slot
        // grid uses for a slot with notes in it.
        if (state.usedByPattern)
        {
            g.setColour (juce::Colour (0xffe08a3c));
            g.drawRoundedRectangle (bounds.toFloat().reduced (0.75f), 3.0f, 1.5f);
        }

        if (dragTargetPad.has_value() && *dragTargetPad == pad)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (bounds.toFloat().reduced (0.75f), 3.0f, 1.5f);
        }

        auto text = bounds.reduced (4, 2);

        g.setFont (11.0f);
        g.setColour (juce::Colour (0xff8a8a94));
        g.drawText (drumkit::getNoteName (drumkit::getNoteForPad (pad)),
                    text.removeFromTop (13), juce::Justification::centredLeft);

        // An empty pad says what to do with it rather than showing nothing,
        // because a grid of blank squares doesn't look clickable.
        g.setFont (12.0f);
        g.setColour (isFilled ? juce::Colour (0xffd8d8de) : juce::Colour (0xff5e5e68));
        g.drawFittedText (isFilled ? state.sampleName : juce::String ("+"),
                          text, juce::Justification::centred, 2, 0.7f);
    }
}

void DrumPadGrid::mouseDown (const juce::MouseEvent& e)
{
    auto pad = getPadAt (e.getPosition());

    if (! pad)
        return;

    // A left click on a pad that has a sample plays it. Hearing the kit is the
    // thing you want most often once it is built, and there was no way to do
    // it short of drawing a note. Loading and clearing move to the right
    // button; an empty pad has nothing to play, so it still asks for a sample
    // whichever button hit it.
    const bool hasSample = pads[(size_t) *pad].sampleName.isNotEmpty();

    if (hasSample && ! e.mods.isPopupMenu())
    {
        if (onPadTriggered)
            onPadTriggered (*pad);

        return;
    }

    if (onPadClicked)
        onPadClicked (*pad);
}

bool DrumPadGrid::isInterestedInFileDrag (const juce::StringArray& files)
{
    return isInterestedInFiles && isInterestedInFiles (files);
}

void DrumPadGrid::setDragTarget (std::optional<int> pad)
{
    if (dragTargetPad != pad)
    {
        dragTargetPad = pad;
        repaint();
    }
}

void DrumPadGrid::fileDragEnter (const juce::StringArray&, int x, int y)
{
    setDragTarget (getPadAt ({ x, y }));
}

void DrumPadGrid::fileDragMove (const juce::StringArray&, int x, int y)
{
    setDragTarget (getPadAt ({ x, y }));
}

void DrumPadGrid::fileDragExit (const juce::StringArray&)
{
    setDragTarget (std::nullopt);
}

void DrumPadGrid::filesDropped (const juce::StringArray& files, int x, int y)
{
    setDragTarget (std::nullopt);

    // A drop landing in a gutter between pads still means the nearest pad, so
    // aim at the pad whose row and column the point falls in rather than
    // dropping the gesture on the floor.
    auto pad = getPadAt ({ x, y });

    if (! pad)
    {
        const int usable = getWidth() - (drumkit::numColumns - 1) * padGap;
        const int cellWidth = juce::jmax (16, usable / drumkit::numColumns);
        const int column = juce::jlimit (0, drumkit::numColumns - 1, x / juce::jmax (1, cellWidth + padGap));
        const int row = juce::jlimit (0, drumkit::numRows - 1, y / (padRowHeight + padGap));
        pad = (drumkit::numRows - 1 - row) * drumkit::numColumns + column;
    }

    if (onFilesDropped)
        onFilesDropped (*pad, files);
}

} // namespace carve::app
