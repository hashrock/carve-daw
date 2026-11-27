#include <limits>

#include "DrumPadGrid.h"
#include "Fonts.h"

namespace carve::app
{

namespace
{
    // Four pads per row. Wide enough for a truncated sample name, tall enough
    // to carry the note badge above it and to be hit without aiming -- these
    // are played by hand now, not only assigned to.
    constexpr int padRowHeight = 48;
    constexpr int padGap = 3;

    // The A-D tabs above the pads, and the gap under them.
    constexpr int bankRowHeight = 20;
    constexpr int bankTabWidth = 34;
    constexpr int bankRowGap = 5;
    constexpr int padsTop = bankRowHeight + bankRowGap;

    // How often the note monitor is looked at, and the least a pad stays lit
    // once hit. A tick of ~33ms with a 100ms flash is three frames: enough
    // to be seen, short enough that a hat roll still flickers per hit.
    constexpr int activityPollHz = 30;
    constexpr double minimumFlashSeconds = 0.1;
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

DrumPadGrid::~DrumPadGrid()
{
    stopTimer();
}

int DrumPadGrid::getPreferredHeight()
{
    return padsTop + drumkit::numRows * padRowHeight + (drumkit::numRows - 1) * padGap;
}

void DrumPadGrid::clearPads()
{
    pads.fill ({});
}

void DrumPadGrid::setPadState (int pad, const PadInfo& info)
{
    // Only the description changes: the hit bookkeeping belongs to the pad,
    // not to what is loaded on it, and a refresh mid-flash must not put the
    // light out.
    if (pad >= 0 && pad < drumkit::numPads)
        pads[(size_t) pad].info = info;
}

void DrumPadGrid::setActivitySource (plugins::NoteMonitorPlugin* source)
{
    monitor = source;

    // Start from the monitor's current counts rather than from zero, or every
    // pad that was ever hit would flash the moment the window opened.
    for (int pad = 0; pad < drumkit::numPads; ++pad)
    {
        auto& state = pads[(size_t) pad];
        state.seenHits = source != nullptr ? source->getActivity (drumkit::getNoteForPad (pad)).hits : 0;
        state.lastHitMs = 0.0;
        state.lit = false;
    }

    if (source != nullptr)
        startTimerHz (activityPollHz);
    else
        stopTimer();

    repaint();
}

void DrumPadGrid::timerCallback()
{
    auto source = monitor.get();

    if (source == nullptr)
    {
        // The monitor went with its track; nothing is sounding through it.
        setActivitySource (nullptr);
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    bool changed = false;

    for (int pad = 0; pad < drumkit::numPads; ++pad)
    {
        auto& state = pads[(size_t) pad];
        const auto activity = source->getActivity (drumkit::getNoteForPad (pad));

        if (activity.hits != state.seenHits)
        {
            state.seenHits = activity.hits;
            state.lastHitMs = now;
        }

        const auto sinceHit = state.lastHitMs > 0.0 ? (now - state.lastHitMs) / 1000.0
                                                    : std::numeric_limits<double>::max();
        const auto& info = state.info;

        // Lit for the minimum flash, then for as long as the sound goes on: a
        // one-shot for its own length, a gated sound while the note is held
        // and the sample has not run out (a held note outlasting a short hit
        // is silence). A length of zero means the length is unknown, so the
        // note alone decides.
        bool lit = sinceHit < minimumFlashSeconds;

        if (info.oneShot)
            lit = lit || (info.soundSeconds > 0.0 && sinceHit < info.soundSeconds);
        else
            lit = lit || (activity.held && (info.soundSeconds <= 0.0 || sinceHit < info.soundSeconds));

        if (lit != state.lit)
        {
            state.lit = lit;
            changed = true;
        }
    }

    if (changed)
        repaint();
}

void DrumPadGrid::setBank (int newBank)
{
    newBank = juce::jlimit (0, drumkit::numBanks - 1, newBank);

    if (newBank != bank)
    {
        bank = newBank;
        repaint();
    }
}

juce::Rectangle<int> DrumPadGrid::getBankTabBounds (int tab) const
{
    return { tab * (bankTabWidth + padGap), 0, bankTabWidth, bankRowHeight };
}

std::optional<int> DrumPadGrid::getBankTabAt (juce::Point<int> position) const
{
    for (int tab = 0; tab < drumkit::numBanks; ++tab)
        if (getBankTabBounds (tab).contains (position))
            return tab;

    return std::nullopt;
}

juce::Rectangle<int> DrumPadGrid::getPadBounds (int pad) const
{
    if (pad < 0 || pad >= drumkit::numPads || drumkit::getBankForPad (pad) != bank)
        return {};

    const int slot = pad % drumkit::padsPerBank;
    const int column = slot % drumkit::numColumns;

    // Pad 0 is bottom-left, so the lowest row is drawn last.
    const int row = drumkit::numRows - 1 - slot / drumkit::numColumns;

    const int usable = getWidth() - (drumkit::numColumns - 1) * padGap;
    const int cellWidth = juce::jmax (16, usable / drumkit::numColumns);

    return { column * (cellWidth + padGap), padsTop + row * (padRowHeight + padGap),
             cellWidth, padRowHeight };
}

std::optional<int> DrumPadGrid::getPadAt (juce::Point<int> position) const
{
    for (int slot = 0; slot < drumkit::padsPerBank; ++slot)
        if (const int pad = bank * drumkit::padsPerBank + slot; getPadBounds (pad).contains (position))
            return pad;

    return std::nullopt;
}

void DrumPadGrid::paint (juce::Graphics& g)
{
    // The bank tabs: the one on show filled, a bank with samples on it
    // marked with a dot, and one with a pad sounding lit, so a pattern
    // playing bank C is seen to while A is on show.
    for (int tab = 0; tab < drumkit::numBanks; ++tab)
    {
        bool filled = false, lit = false;

        for (int slot = 0; slot < drumkit::padsPerBank; ++slot)
        {
            const auto& state = pads[(size_t) (tab * drumkit::padsPerBank + slot)];
            filled = filled || state.info.sampleName.isNotEmpty();
            lit = lit || state.lit;
        }

        const auto bounds = getBankTabBounds (tab).toFloat();
        const bool shown = tab == bank;

        g.setColour (shown ? juce::Colour (0xff4a525c) : juce::Colour (0xff2b2b30));
        g.fillRoundedRectangle (bounds, 3.0f);

        if (lit && ! shown)
        {
            g.setColour (juce::Colour (0xffe0a24f));
            g.drawRoundedRectangle (bounds.reduced (0.75f), 3.0f, 1.5f);
        }

        g.setFont (uiFont (fonts::small));
        g.setColour (shown ? juce::Colour (0xffe8e8ee) : juce::Colour (0xff8a8a94));
        g.drawText (drumkit::getBankName (tab), bounds.withTrimmedRight (filled ? 8.0f : 0.0f),
                    juce::Justification::centred);

        if (filled)
        {
            g.setColour (lit ? juce::Colour (0xffe0a24f) : juce::Colour (0xff8a8a94));
            g.fillEllipse (bounds.getRight() - 10.0f, bounds.getCentreY() - 2.0f, 4.0f, 4.0f);
        }
    }

    for (int slot = 0; slot < drumkit::padsPerBank; ++slot)
    {
        const int pad = bank * drumkit::padsPerBank + slot;
        const auto& state = pads[(size_t) pad];
        const auto bounds = getPadBounds (pad);
        const bool isFilled = state.info.sampleName.isNotEmpty();

        // A sounding pad goes bright, the way a drum machine's does: the face
        // rather than the outline, so it reads from across the room and does
        // not fight the pattern accent below.
        g.setColour (state.lit ? juce::Colour (0xffe0a24f)
                   : isFilled  ? juce::Colour (0xff3f4750) : juce::Colour (0xff2b2b30));
        g.fillRoundedRectangle (bounds.toFloat(), 3.0f);

        // The pattern being edited plays this note: the same accent the slot
        // grid uses for a slot with notes in it.
        if (state.info.usedByPattern)
        {
            g.setColour (juce::Colour (0xffe08a3c));
            g.drawRoundedRectangle (bounds.toFloat().reduced (0.75f), 3.0f, 1.5f);
        }

        if (selectedPad == pad || (dragTargetPad.has_value() && *dragTargetPad == pad))
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (bounds.toFloat().reduced (0.75f), 3.0f, 1.5f);
        }

        auto text = bounds.reduced (4, 2);

        g.setFont (uiFont (fonts::small));
        g.setColour (juce::Colour (0xff8a8a94));
        g.drawText (drumkit::getNoteName (drumkit::getNoteForPad (pad)),
                    text.removeFromTop (15), juce::Justification::centredLeft);

        // An empty pad says what to do with it rather than showing nothing,
        // because a grid of blank squares doesn't look clickable.
        g.setFont (uiFont (fonts::small));
        g.setColour (state.lit ? juce::Colour (0xff1b1b1f)
                   : isFilled  ? juce::Colour (0xffd8d8de) : juce::Colour (0xff5e5e68));
        g.drawFittedText (isFilled ? state.info.sampleName : juce::String ("+"),
                          text, juce::Justification::centred, 2, 0.7f);
    }
}

void DrumPadGrid::mouseDown (const juce::MouseEvent& e)
{
    if (const auto tab = getBankTabAt (e.getPosition()))
    {
        setBank (*tab);
        return;
    }

    auto pad = getPadAt (e.getPosition());

    if (! pad)
        return;

    // A left click on a pad that has a sample plays it. Hearing the kit is the
    // thing you want most often once it is built, and there was no way to do
    // it short of drawing a note. Loading and clearing move to the right
    // button; an empty pad has nothing to play, so it still asks for a sample
    // whichever button hit it.
    const bool hasSample = pads[(size_t) *pad].info.sampleName.isNotEmpty();

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
    // Held over a tab, the drag goes to that bank, so a kit's later banks can
    // be filled by drag and drop too.
    if (const auto tab = getBankTabAt ({ x, y }))
        setBank (*tab);

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
        const int row = juce::jlimit (0, drumkit::numRows - 1, (y - padsTop) / (padRowHeight + padGap));
        pad = bank * drumkit::padsPerBank + (drumkit::numRows - 1 - row) * drumkit::numColumns + column;
    }

    if (onFilesDropped)
        onFilesDropped (*pad, files);
}

} // namespace carve::app
