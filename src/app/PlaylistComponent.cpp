#include "PlaylistComponent.h"

namespace orionish::app
{

PlaylistComponent::PlaylistComponent (juce::UndoManager& um)
    : undoManager (um)
{
    song.state.addListener (this);
    updateSize();
}

PlaylistComponent::~PlaylistComponent()
{
    song.state.removeListener (this);
}

void PlaylistComponent::setSong (model::Song newSong)
{
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);
    refresh();
}

void PlaylistComponent::setSelection (const juce::String& generatorId, const juce::String& patternId)
{
    selectedGeneratorId = generatorId;
    selectedPatternId = patternId;
    repaint();
}

void PlaylistComponent::setPlayheadBeats (double beats)
{
    if (std::abs (beats - playheadBeats) > 1.0e-3)
    {
        playheadBeats = beats;
        repaint();
    }
}

void PlaylistComponent::refresh()
{
    updateSize();
    repaint();
}

double PlaylistComponent::getContentLengthBeats() const
{
    double length = 32.0;
    for (const auto& clip : song.getPlaylist().getClips())
        if (auto generator = song.findGenerator (clip.getGeneratorId()))
            if (auto pattern = generator->findPattern (clip.getPatternId()))
                length = std::max (length, clip.getStart() + pattern->getLengthBeats() + 16.0);
    return std::ceil (length / 16.0) * 16.0;
}

void PlaylistComponent::updateSize()
{
    setSize (labelWidth + juce::roundToInt (getContentLengthBeats() * pixelsPerBeat),
             juce::jmax (1, song.getNumGenerators()) * rowHeight);
}

void PlaylistComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const auto lengthBeats = getContentLengthBeats();
    const int numRows = song.getNumGenerators();

    // vertical grid
    for (double beat = 0.0; beat <= lengthBeats; beat += 1.0)
    {
        const bool isBar = std::fmod (beat, 4.0) < 1.0e-9;
        g.setColour (isBar ? juce::Colour (0xff4a4a52) : juce::Colour (0xff2c2c31));
        g.drawVerticalLine ((int) beatToX (beat), 0.0f, (float) getHeight());
    }

    // rows + clips
    for (int row = 0; row < numRows; ++row)
    {
        auto generator = song.getGenerator (row);
        const auto y = (float) (row * rowHeight);
        const bool isSelectedRow = generator.getId() == selectedGeneratorId;

        g.setColour (juce::Colour (0xff3a3a40));
        g.drawHorizontalLine (row * rowHeight, 0.0f, (float) getWidth());

        // label cell
        g.setColour (isSelectedRow ? juce::Colour (0xff35353d) : juce::Colour (0xff2b2b30));
        g.fillRect (0.0f, y + 1.0f, (float) labelWidth - 2.0f, (float) rowHeight - 1.0f);
        g.setColour (isSelectedRow ? juce::Colours::white : juce::Colour (0xffb8b8c0));
        g.setFont (13.0f);
        g.drawText (generator.getName(), 8, (int) y, labelWidth - 12, rowHeight,
                    juce::Justification::centredLeft);

        // clips
        const auto rowColour = juce::Colour::fromHSV (0.08f + 0.13f * (float) row, 0.55f, 0.75f, 1.0f);
        for (const auto& clip : song.getPlaylist().getClips())
        {
            if (clip.getGeneratorId() != generator.getId())
                continue;
            auto pattern = generator.findPattern (clip.getPatternId());
            if (! pattern)
                continue;

            juce::Rectangle<float> r (beatToX (clip.getStart()), y + 2.0f,
                                      (float) (pattern->getLengthBeats() * pixelsPerBeat) - 1.0f,
                                      (float) rowHeight - 4.0f);
            g.setColour (rowColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.setFont (11.0f);
            g.drawText (pattern->getName(), r.reduced (4.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft);
        }
    }

    // playhead
    g.setColour (juce::Colours::orangered);
    g.drawVerticalLine ((int) beatToX (playheadBeats), 0.0f, (float) getHeight());
}

void PlaylistComponent::mouseDown (const juce::MouseEvent& e)
{
    const int row = (int) (e.position.y / (float) rowHeight);
    if (row < 0 || row >= song.getNumGenerators())
        return;

    auto generator = song.getGenerator (row);

    if (e.position.x < (float) labelWidth)
    {
        if (onSelectGenerator)
            onSelectGenerator (generator.getId());
        return;
    }

    const auto clickBeat = xToBeat (e.position.x);

    // toggle: clicking an existing clip removes it
    for (const auto& clip : song.getPlaylist().getClips())
    {
        if (clip.getGeneratorId() != generator.getId())
            continue;
        auto pattern = generator.findPattern (clip.getPatternId());
        if (! pattern)
            continue;
        if (clickBeat >= clip.getStart() && clickBeat < clip.getStart() + pattern->getLengthBeats())
        {
            undoManager.beginNewTransaction();
            song.getPlaylist().removeClip (clip, &undoManager);
            return;
        }
    }

    // place the row's current pattern (selected one for the selected
    // generator, first pattern otherwise)
    auto pattern = generator.getId() == selectedGeneratorId
                       ? generator.findPattern (selectedPatternId)
                       : std::nullopt;
    if (! pattern && generator.getNumPatterns() > 0)
        pattern = generator.getPattern (0);
    if (! pattern)
        return;

    const auto start = std::max (0.0, std::floor (clickBeat / snapBeats) * snapBeats);
    undoManager.beginNewTransaction();
    song.getPlaylist().addClip (generator, *pattern, start, &undoManager);
}

} // namespace orionish::app
