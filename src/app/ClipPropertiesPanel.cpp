#include <cmath>

#include "ClipPropertiesPanel.h"

namespace carve::app
{

namespace
{
    void styleHeader (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (12.0f));
        label.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    }

    void styleField (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (13.0f));
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
    }

    void styleStepper (juce::Slider& slider, double minimum, double maximum, double interval)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setRange (minimum, maximum, interval);
        slider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        // Wide enough for a bar count with its unit spelled out: a number of
        // bars with no unit beside it is exactly the ambiguity this panel had.
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 76, 20);
    }
} // namespace

ClipPropertiesPanel::ClipPropertiesPanel (juce::UndoManager& um)
    : undoManager (um)
{
    styleHeader (clipHeader, "This clip");
    styleHeader (patternHeader, "Pattern");
    styleField (lengthLabel, "Length");
    styleField (transposeLabel, "Transpose");
    styleField (patternLengthLabel, "Length");

    patternLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    patternLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    for (auto* l : { &loopLabel, &patternLengthNote })
    {
        l->setFont (juce::FontOptions (12.0f));
        l->setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    }

    // Bars, because the playlist grid and its snapping are bar-based. A quarter
    // of a bar is the finest step the playlist can show usefully -- one beat in
    // 4/4, and whatever a quarter of the bar is in anything else.
    styleStepper (lengthSlider, 0.25, 256.0, 0.25);
    styleStepper (patternLengthSlider, 0.25, 64.0, 0.25);
    styleStepper (transposeSlider, -48.0, 48.0, 1.0);

    lengthSlider.setTextValueSuffix (" bars");
    patternLengthSlider.setTextValueSuffix (" bars");

    lengthSlider.onValueChange = [this] { applyClipLength(); };
    transposeSlider.onValueChange = [this] { applyTranspose(); };
    patternLengthSlider.onValueChange = [this] { applyPatternLength(); };

    resetLengthButton.onClick = [this]
    {
        if (isRefreshing)
            return;

        undoManager.beginNewTransaction();

        for (auto& clip : liveSelection())
            clip.clearLength (&undoManager);
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &patternLabel, &clipHeader, &lengthLabel, &lengthSlider, &loopLabel,
             &transposeLabel, &transposeSlider, &resetLengthButton,
             &patternHeader, &patternLengthLabel, &patternLengthSlider,
             &patternLengthNote })
        addAndMakeVisible (c);

    song.state.addListener (this);
    refresh();
}

ClipPropertiesPanel::~ClipPropertiesPanel()
{
    cancelPendingUpdate();
    song.state.removeListener (this);
}

void ClipPropertiesPanel::setSong (model::Song newSong)
{
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);

    selection.clear();
    refresh();
}

void ClipPropertiesPanel::setSelection (const std::vector<model::PlaylistClip>& clips)
{
    selection = clips;
    refresh();
}

double ClipPropertiesPanel::barPositionOf (double beat) const
{
    const auto position = song.toBarsAndBeats (beat);
    const auto beatsPerBar = song.getTimeSigAt (beat).getBeatsPerBar();

    return (double) position.bar + position.beat / juce::jmax (0.0625, beatsPerBar);
}

double ClipPropertiesPanel::beatAtBarPosition (double barPosition) const
{
    const auto bar = (int) std::floor (barPosition);
    const auto barStart = song.beatOfBar (bar);

    // The signature is read at the bar's own start rather than at barPosition,
    // because a change lands on a bar line: asking mid-bar would step into the
    // next signature for a position that is still inside this bar.
    return barStart + (barPosition - (double) bar) * song.getTimeSigAt (barStart).getBeatsPerBar();
}

model::TimeSignature ClipPropertiesPanel::selectedTimeSig() const
{
    const auto clips = liveSelection();

    return clips.empty() ? model::TimeSignature() : song.getTimeSigAt (clips.front().getStart());
}

bool ClipPropertiesPanel::selectedPatternSpansSignatures() const
{
    const auto clips = liveSelection();

    if (clips.empty() || ! songSignatureVaries (song))
        return false;

    const auto patternId = clips.front().getPatternId();
    const auto sig = selectedTimeSig();

    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getPatternId() == patternId && song.getTimeSigAt (clip.getStart()) != sig)
            return true;

    return false;
}

std::vector<model::PlaylistClip> ClipPropertiesPanel::liveSelection() const
{
    std::vector<model::PlaylistClip> result;

    // A clip removed from the playlist keeps a valid ValueTree, just an orphaned
    // one, so a stale selection would otherwise write into nothing.
    for (const auto& clip : selection)
        if (clip.state.getParent().isValid() && patternFor (clip))
            result.push_back (clip);

    return result;
}

std::optional<model::Pattern> ClipPropertiesPanel::patternFor (const model::PlaylistClip& clip) const
{
    if (auto generator = song.findGenerator (clip.getGeneratorId()))
        return generator->findPattern (clip.getPatternId());

    return std::nullopt;
}

int ClipPropertiesPanel::countPlacementsOfSelectedPattern() const
{
    const auto clips = liveSelection();
    if (clips.empty())
        return 0;

    const auto patternId = clips.front().getPatternId();
    int count = 0;

    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getPatternId() == patternId)
            ++count;

    return count;
}

void ClipPropertiesPanel::refresh()
{
    cancelPendingUpdate();

    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    const auto clips = liveSelection();
    const bool hasSelection = ! clips.empty();

    for (auto* c : std::initializer_list<juce::Component*> {
             &clipHeader, &lengthLabel, &lengthSlider, &loopLabel, &transposeLabel,
             &transposeSlider, &resetLengthButton, &patternHeader,
             &patternLengthLabel, &patternLengthSlider, &patternLengthNote })
        c->setVisible (hasSelection);

    if (! hasSelection)
    {
        patternLabel.setText ("No clip selected", juce::dontSendNotification);
        patternLabel.setColour (juce::Label::textColourId, juce::Colour (0xff707078));
        return;
    }

    patternLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    const auto& first = clips.front();
    const auto pattern = *patternFor (first);
    const auto patternLength = pattern.getLengthBeats();

    patternLabel.setText (clips.size() == 1
                              ? pattern.getName()
                              : juce::String ((int) clips.size()) + " clips selected",
                          juce::dontSendNotification);

    const auto clipStart = first.getStart();
    const auto clipLength = first.getLength (patternLength);

    // The clip sits at a known beat, so its length is measured along the song's
    // bars: four bars reads as four even if the signature changes inside it.
    lengthSlider.setValue (barPositionOf (clipStart + clipLength) - barPositionOf (clipStart),
                           juce::dontSendNotification);
    transposeSlider.setValue (first.getTranspose(), juce::dontSendNotification);

    // The pattern does not sit anywhere, so there is no bar map to measure it
    // along -- only the signature wherever the selected clip happens to be.
    const auto sig = selectedTimeSig();
    patternLengthSlider.setValue (patternLength / sig.getBeatsPerBar(), juce::dontSendNotification);

    // ... which is why the beats the model actually stores are printed beside
    // it, and why a pattern also placed under another signature says so: its
    // bar count here is not the one the user would read at that other clip.
    patternLengthNote.setText ("bars of " + timeSigText (sig)
                                   + (selectedPatternSpansSignatures() ? " here, " : ", ")
                                   + trimmedNumber (patternLength) + " beats",
                               juce::dontSendNotification);

    const auto repeats = clipLength / juce::jmax (model::Pattern::minLengthBeats, patternLength);
    loopLabel.setText (repeats > 1.001 ? "plays " + trimmedNumber (repeats) + "x through"
                                       : repeats < 0.999 ? "cut short" : juce::String(),
                       juce::dontSendNotification);

    const auto placements = countPlacementsOfSelectedPattern();
    styleHeader (patternHeader, placements > 1
                                    ? "Pattern - affects all " + juce::String (placements) + " placements"
                                    : "Pattern");
}

void ClipPropertiesPanel::applyClipLength()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();
    const auto bars = lengthSlider.getValue();

    // Converted per clip rather than once: the bar count is what the user asked
    // for, and with several clips selected across a signature change the same
    // number of bars is a different number of beats at each of them.
    for (auto& clip : liveSelection())
    {
        const auto start = clip.getStart();
        clip.setLength (beatAtBarPosition (barPositionOf (start) + bars) - start, &undoManager);
    }
}

void ClipPropertiesPanel::applyTranspose()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();
    const auto semitones = (int) transposeSlider.getValue();

    for (auto& clip : liveSelection())
        clip.setTranspose (semitones, &undoManager);
}

void ClipPropertiesPanel::applyPatternLength()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();

    // The signature of the clip the user is looking at, which is the one the
    // spinner beside it is labelled with.
    const auto beats = patternLengthSlider.getValue() * selectedTimeSig().getBeatsPerBar();

    // Deliberately only the first selection's pattern: a multi-clip selection
    // can span several patterns, and quietly resizing all of them is exactly
    // the surprise this panel replaced. It is also the pattern the spinner was
    // filled in from, and whose signature the bars above were counted in.
    const auto clips = liveSelection();

    if (clips.empty())
        return;

    if (auto pattern = patternFor (clips.front()))
        pattern->setLengthBeats (beats, &undoManager);
}

void ClipPropertiesPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2b2b30));
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawVerticalLine (0, 0.0f, (float) getHeight());
}

void ClipPropertiesPanel::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    patternLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto row = [&area] (juce::Component& label, juce::Component& control)
    {
        auto line = area.removeFromTop (22);
        label.setBounds (line.removeFromLeft (66));
        control.setBounds (line);
        area.removeFromTop (4);
    };

    clipHeader.setBounds (area.removeFromTop (16));
    area.removeFromTop (2);
    row (lengthLabel, lengthSlider);
    loopLabel.setBounds (area.removeFromTop (14).withTrimmedLeft (66));
    area.removeFromTop (4);
    row (transposeLabel, transposeSlider);
    resetLengthButton.setBounds (area.removeFromTop (22).withTrimmedLeft (66));

    area.removeFromTop (14);
    patternHeader.setBounds (area.removeFromTop (16));
    area.removeFromTop (2);
    row (patternLengthLabel, patternLengthSlider);
    patternLengthNote.setBounds (area.removeFromTop (14));
}

} // namespace carve::app
