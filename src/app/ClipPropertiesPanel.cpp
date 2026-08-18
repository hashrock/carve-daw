#include "ClipPropertiesPanel.h"

namespace carve::app
{

namespace
{
    void styleHeader (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (11.0f));
        label.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    }

    void styleField (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (12.0f));
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
    }

    void styleStepper (juce::Slider& slider, double minimum, double maximum, double interval)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setRange (minimum, maximum, interval);
        slider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 54, 20);
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

    patternLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    patternLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    loopLabel.setFont (juce::FontOptions (10.0f));
    loopLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));

    // Bars, because the playlist grid and its snapping are bar-based. A quarter
    // of a bar is one beat, the finest thing the playlist can show usefully.
    styleStepper (lengthSlider, 0.25, 256.0, 0.25);
    styleStepper (patternLengthSlider, 0.25, 64.0, 0.25);
    styleStepper (transposeSlider, -48.0, 48.0, 1.0);

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
             &patternHeader, &patternLengthLabel, &patternLengthSlider })
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
             &patternLengthLabel, &patternLengthSlider })
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

    lengthSlider.setValue (first.getLength (patternLength) / beatsPerBar,
                           juce::dontSendNotification);
    transposeSlider.setValue (first.getTranspose(), juce::dontSendNotification);
    patternLengthSlider.setValue (patternLength / beatsPerBar, juce::dontSendNotification);

    const auto repeats = first.getLength (patternLength) / juce::jmax (0.0625, patternLength);
    loopLabel.setText (repeats > 1.001 ? "plays " + juce::String (repeats, 2).trimCharactersAtEnd ("0.")
                                             + "x through"
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
    const auto beats = lengthSlider.getValue() * beatsPerBar;

    for (auto& clip : liveSelection())
        clip.setLength (beats, &undoManager);
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
    const auto beats = patternLengthSlider.getValue() * beatsPerBar;

    // Deliberately only the first selection's pattern: a multi-clip selection
    // can span several patterns, and quietly resizing all of them is exactly
    // the surprise this panel replaced.
    for (const auto& clip : liveSelection())
    {
        if (auto pattern = patternFor (clip))
            pattern->setLengthBeats (beats, &undoManager);

        return;
    }
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
}

} // namespace carve::app
