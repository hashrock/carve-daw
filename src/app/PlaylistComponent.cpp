#include "PlaylistComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace orionish::app
{

namespace
{
    // Beats are doubles, so every "is this the same bar / does this overlap"
    // test needs a little slack.
    constexpr double beatTolerance = 1.0e-9;

    // JUCE has no eraser cursor, so draw one: a red cross with a dark halo so
    // it stays readable over both the grid and a bright clip. Drawn at 2x so it
    // stays sharp on a retina display.
    juce::MouseCursor makeEraseCursor()
    {
        juce::Image image (juce::Image::ARGB, 32, 32, true);
        {
            juce::Graphics g (image);
            juce::Path cross;
            cross.startNewSubPath (8.0f, 8.0f);
            cross.lineTo (24.0f, 24.0f);
            cross.startNewSubPath (24.0f, 8.0f);
            cross.lineTo (8.0f, 24.0f);

            g.setColour (juce::Colours::black.withAlpha (0.75f));
            g.strokePath (cross, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            g.setColour (juce::Colour (0xffff6b6b));
            g.strokePath (cross, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }
        return { image, 8, 8, 2.0f };   // hotspot is in scaled, not pixel, coordinates
    }
} // namespace

PlaylistComponent::PlaylistComponent (juce::UndoManager& um)
    : undoManager (um), eraseCursor (makeEraseCursor())
{
    song.state.addListener (this);

    paintToolButton.setTooltip ("Paint (B): drag to lay the selected pattern into empty bars");
    selectToolButton.setTooltip ("Select (E): click and drag clips, or rubber-band empty space");
    // Plain ASCII: a char* literal with anything else in it trips a jassert in
    // juce::String.
    zoomOutButton.setTooltip ("Zoom out (- key, or cmd-scroll)");
    zoomInButton.setTooltip ("Zoom in (= key, or cmd-scroll)");

    for (auto* b : { &paintToolButton, &selectToolButton, &zoomOutButton, &zoomInButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe08a3c));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        // Buttons never take focus themselves: the component owns ⌘D and the
        // tool shortcuts, and clicking one must not steal the keyboard from
        // the grid.
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    paintToolButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    selectToolButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    zoomOutButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    zoomInButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    paintToolButton.onClick = [this] { setTool (Tool::paint); };
    selectToolButton.onClick = [this] { setTool (Tool::select); };
    zoomOutButton.onClick = [this] { zoomBy (1.0 / 1.5); };
    zoomInButton.onClick = [this] { zoomBy (1.5); };

    setWantsKeyboardFocus (true);
    setTool (tool);
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

    dragMode = DragMode::none;
    resizePattern = {};
    rubberBand = {};
    clearSelection();
    refresh();
}

void PlaylistComponent::setSelection (const juce::String& generatorId, const juce::String& patternId)
{
    selectedGeneratorId = generatorId;
    selectedPatternId = patternId;
    repaint();
}

void PlaylistComponent::setTool (Tool newTool)
{
    tool = newTool;
    paintToolButton.setToggleState (tool == Tool::paint, juce::dontSendNotification);
    selectToolButton.setToggleState (tool == Tool::select, juce::dontSendNotification);
    setMouseCursor (cursorFor (lastMousePosition, juce::ModifierKeys::getCurrentModifiers()));
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
    pruneSelection();
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
    auto width = labelWidth + juce::roundToInt (getContentLengthBeats() * pixelsPerBeat);

    // Zoomed far enough out the song is narrower than the viewport; stay at
    // least as wide as it so the header band still spans the window.
    if (auto* viewport = getViewport())
        width = juce::jmax (width, viewport->getMaximumVisibleWidth());

    setSize (width, headerHeight + juce::jmax (1, song.getNumGenerators()) * rowHeight);
}

juce::Viewport* PlaylistComponent::getViewport() const
{
    return findParentComponentOfClass<juce::Viewport>();
}

void PlaylistComponent::setPixelsPerBeat (double newPixelsPerBeat, float anchorX)
{
    newPixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, newPixelsPerBeat);
    if (std::abs (newPixelsPerBeat - pixelsPerBeat) < 1.0e-6)
        return;

    // Keep whatever is under anchorX pinned to the same place on screen, so
    // zooming feels like it happens around the pointer rather than around the
    // start of the song.
    auto* viewport = getViewport();
    const auto anchorBeat = xToBeat (anchorX);
    const auto anchorScreenX = juce::roundToInt (anchorX) - (viewport != nullptr ? viewport->getViewPositionX() : 0);

    pixelsPerBeat = newPixelsPerBeat;
    updateSize();

    if (viewport != nullptr)
        viewport->setViewPosition (juce::jmax (0, juce::roundToInt (beatToX (anchorBeat)) - anchorScreenX),
                                   viewport->getViewPositionY());

    updateHover (lastMousePosition);
    repaint();
}

void PlaylistComponent::zoomBy (double factor)
{
    // No pointer to zoom about, so hold the middle of the visible span still.
    auto anchorX = (float) (visibleOrigin().x + getWidth() / 2);
    if (auto* viewport = getViewport())
        anchorX = (float) (viewport->getViewPositionX() + viewport->getViewWidth() / 2);

    setPixelsPerBeat (pixelsPerBeat * factor, anchorX);
}

void PlaylistComponent::resized()
{
    layOutToolStrip();
}

void PlaylistComponent::moved()
{
    // The viewport scrolls us by moving us, so the pinned header band has to be
    // repositioned and redrawn at its new place in our coordinates.
    layOutToolStrip();
    repaint();
}

void PlaylistComponent::layOutToolStrip()
{
    const auto origin = visibleOrigin();
    const auto h = toolbarHeight - 8;
    paintToolButton.setBounds (origin.x + 6, origin.y + 4, 54, h);
    selectToolButton.setBounds (paintToolButton.getRight(), origin.y + 4, 54, h);
    zoomOutButton.setBounds (selectToolButton.getRight() + 12, origin.y + 4, 24, h);
    zoomInButton.setBounds (zoomOutButton.getRight(), origin.y + 4, 24, h);
}

//==============================================================================
// Model queries

std::optional<model::Pattern> PlaylistComponent::patternForRow (const model::Generator& generator) const
{
    // the row's current pattern: the selected one for the selected generator,
    // its first pattern otherwise
    auto pattern = generator.getId() == selectedGeneratorId
                       ? generator.findPattern (selectedPatternId)
                       : std::nullopt;
    if (! pattern && generator.getNumPatterns() > 0)
        pattern = generator.getPattern (0);
    return pattern;
}

std::optional<model::PlaylistClip> PlaylistComponent::clipAt (const model::Generator& generator,
                                                              double beat) const
{
    auto playlist = song.getPlaylist();

    // iterate in reverse so the most recently added clip wins on overlap
    for (int i = playlist.getNumClips(); --i >= 0;)
    {
        auto clip = playlist.getClip (i);
        if (clip.getGeneratorId() != generator.getId())
            continue;
        if (auto pattern = generator.findPattern (clip.getPatternId()))
            if (beat >= clip.getStart() && beat < clip.getStart() + pattern->getLengthBeats())
                return clip;
    }
    return std::nullopt;
}

juce::Rectangle<float> PlaylistComponent::slotBounds (int row, double startBeats,
                                                      double lengthBeats) const
{
    return { beatToX (startBeats), rowY (row) + 2.0f,
             (float) (lengthBeats * pixelsPerBeat) - 1.0f, (float) rowHeight - 4.0f };
}

std::optional<juce::Rectangle<float>> PlaylistComponent::boundsForClip (int row,
                                                                       const model::PlaylistClip& clip) const
{
    if (auto generator = song.findGenerator (clip.getGeneratorId()))
        if (auto pattern = generator->findPattern (clip.getPatternId()))
            return slotBounds (row, clip.getStart(), pattern->getLengthBeats());
    return std::nullopt;
}

juce::Rectangle<float> PlaylistComponent::patternMenuBounds (juce::Rectangle<float> clipRect) const
{
    // Only worth offering once the clip is wide enough to hold the chevron
    // next to the resize edge and still show some of its name.
    if (clipRect.getWidth() < 46.0f)
        return {};

    return { clipRect.getRight() - resizeHandleWidth - menuButtonWidth, clipRect.getY() + 1.0f,
             menuButtonWidth, clipRect.getHeight() * 0.5f };
}

juce::Rectangle<float> PlaylistComponent::resizeHandleBounds (juce::Rectangle<float> clipRect) const
{
    if (clipRect.getWidth() < 16.0f)
        return {};   // too narrow to split into "body" and "edge"

    return clipRect.removeFromRight (resizeHandleWidth);
}

bool PlaylistComponent::isRangeFree (const model::Generator& generator, double startBeats,
                                     double lengthBeats, bool ignoreSelectedClips,
                                     const juce::ValueTree& alsoIgnore) const
{
    for (const auto& clip : song.getPlaylist().getClips())
    {
        if (clip.getGeneratorId() != generator.getId())
            continue;
        if (ignoreSelectedClips && isSelected (clip.state))
            continue;
        if (alsoIgnore.isValid() && clip.state == alsoIgnore)
            continue;

        if (auto pattern = generator.findPattern (clip.getPatternId()))
        {
            const auto clipEnd = clip.getStart() + pattern->getLengthBeats();
            if (startBeats < clipEnd - beatTolerance
                    && clip.getStart() < startBeats + lengthBeats - beatTolerance)
                return false;
        }
    }
    return true;
}

//==============================================================================
// Selection (UI state only)

bool PlaylistComponent::isSelected (const juce::ValueTree& clip) const
{
    return std::find (selectedClips.begin(), selectedClips.end(), clip) != selectedClips.end();
}

void PlaylistComponent::selectClip (const model::PlaylistClip& clip, bool extend)
{
    if (! extend)
    {
        if (isSelected (clip.state))
            return;   // keep a multi-selection intact when re-clicking part of it
        selectedClips.clear();
    }
    else if (isSelected (clip.state))
    {
        std::erase (selectedClips, clip.state);
        repaint();
        return;
    }

    selectedClips.push_back (clip.state);
    repaint();
}

void PlaylistComponent::clearSelection()
{
    if (selectedClips.empty())
        return;
    selectedClips.clear();
    repaint();
}

void PlaylistComponent::pruneSelection()
{
    // Clips deleted (by us, by undo, or by a song reload) leave detached trees
    // behind; drop anything that is no longer part of the playlist.
    const auto playlistState = song.getPlaylist().state;
    std::erase_if (selectedClips, [&] (const juce::ValueTree& clip)
    {
        return ! clip.isAChildOf (playlistState);
    });
}

//==============================================================================
// Painting

void PlaylistComponent::paintRuler (juce::Graphics& g)
{
    const auto ruler = rulerBounds();
    g.setColour (juce::Colour (0xff17171b));
    g.fillRect (ruler);

    // The loop range, as a bar with a grab handle at each end. An empty range
    // means "loop the whole song", so there is nothing to draw for it.
    if (song.hasLoopRange())
    {
        const auto x1 = beatToX (song.getLoopStart());
        const auto x2 = beatToX (song.getLoopEnd());

        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.5f));
        g.fillRect (x1, ruler.getY() + 1.0f, x2 - x1, 5.0f);
        g.setColour (juce::Colour (0xffe08a3c));
        for (auto x : { x1, x2 })
            g.fillRect (x - 1.0f, ruler.getY() + 1.0f, 2.0f, ruler.getHeight() - 2.0f);
    }

    // Bar numbers, thinned to whatever power-of-two step keeps the labels from
    // colliding at this zoom.
    const auto barWidth = snapBeats * pixelsPerBeat;
    int labelStep = 1;
    while ((double) labelStep * barWidth < 46.0)
        labelStep *= 2;

    const auto lengthBeats = getContentLengthBeats();
    g.setFont (10.0f);

    for (int bar = 0; (double) bar * snapBeats <= lengthBeats; bar += labelStep)
    {
        const auto x = beatToX ((double) bar * snapBeats);
        g.setColour (juce::Colour (0xff4a4a52));
        g.drawVerticalLine ((int) x, ruler.getY() + 6.0f, ruler.getBottom());
        g.setColour (juce::Colour (0xff9a9aa4));
        g.drawText (juce::String (bar + 1), (int) x + 3, (int) ruler.getY(),
                    juce::roundToInt ((double) labelStep * barWidth), rulerHeight,
                    juce::Justification::centredLeft);
    }

    // Playhead marker, so the ruler shows where playback is even when the rows
    // are scrolled out of view.
    const auto playheadX = beatToX (playheadBeats);
    juce::Path marker;
    marker.addTriangle (playheadX - 4.0f, ruler.getBottom() - 6.0f,
                        playheadX + 4.0f, ruler.getBottom() - 6.0f,
                        playheadX, ruler.getBottom());
    g.setColour (juce::Colours::orangered);
    g.fillPath (marker);
}

void PlaylistComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const auto lengthBeats = getContentLengthBeats();
    const int numRows = song.getNumGenerators();
    const auto gridTop = (float) headerHeight;

    // Vertical grid, thinned as we zoom out: beat lines only while a beat is
    // still a few pixels wide, then every 2nd / 4th / ... bar, so the lines
    // never close up into a solid block.
    const auto barWidth = snapBeats * pixelsPerBeat;
    const bool drawBeats = pixelsPerBeat >= 7.0;
    int barStep = 1;
    while ((double) barStep * barWidth < 9.0)
        barStep *= 2;

    for (int bar = 0; (double) bar * snapBeats <= lengthBeats; ++bar)
    {
        const auto barStart = (double) bar * snapBeats;

        if (bar % barStep == 0)
        {
            g.setColour (juce::Colour (0xff4a4a52));
            g.drawVerticalLine ((int) beatToX (barStart), gridTop, (float) getHeight());
        }

        if (drawBeats)
        {
            g.setColour (juce::Colour (0xff2c2c31));
            for (double beat = 1.0; beat < snapBeats; beat += 1.0)
                g.drawVerticalLine ((int) beatToX (barStart + beat), gridTop, (float) getHeight());
        }
    }

    // The loop range washes over the whole grid, not just the ruler, so it
    // reads as "this is the part that plays".
    if (song.hasLoopRange())
    {
        const auto x1 = beatToX (song.getLoopStart());
        const auto x2 = beatToX (song.getLoopEnd());
        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.06f));
        g.fillRect (x1, gridTop, x2 - x1, (float) getHeight() - gridTop);
        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.3f));
        g.drawVerticalLine ((int) x1, gridTop, (float) getHeight());
        g.drawVerticalLine ((int) x2, gridTop, (float) getHeight());
    }

    // rows + clips
    for (int row = 0; row < numRows; ++row)
    {
        auto generator = song.getGenerator (row);
        const auto y = rowY (row);
        const bool isSelectedRow = generator.getId() == selectedGeneratorId;

        g.setColour (juce::Colour (0xff3a3a40));
        g.drawHorizontalLine ((int) y, 0.0f, (float) getWidth());

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

            const auto r = slotBounds (row, clip.getStart(), pattern->getLengthBeats());
            const bool selected = isSelected (clip.state);
            // Every clip playing the pattern whose edge is under the pointer,
            // so it is obvious *before* the drag that resizing hits them all.
            const bool lengthHinted = lengthHintPatternId.isNotEmpty()
                                          && pattern->getId() == lengthHintPatternId;

            g.setColour (selected ? rowColour.brighter (0.4f) : rowColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 3.0f, selected ? 1.6f : 1.0f);

            if (lengthHinted)
            {
                g.setColour (juce::Colour (0xffffd479));
                g.drawRoundedRectangle (r.reduced (0.8f), 3.0f, 1.6f);
            }

            const auto menuRect = patternMenuBounds (r);

            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.setFont (11.0f);
            g.drawText (pattern->getName(),
                        r.withTrimmedRight (menuRect.isEmpty() ? 0.0f : r.getRight() - menuRect.getX())
                         .reduced (4.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft);

            // Pattern chevron: click it to play a different pattern of this
            // generator from this clip.
            if (! menuRect.isEmpty())
            {
                g.setColour (juce::Colours::black.withAlpha (0.25f));
                g.fillRoundedRectangle (menuRect, 2.0f);

                juce::Path chevron;
                const auto c = menuRect.getCentre();
                chevron.startNewSubPath (c.x - 3.5f, c.y - 1.5f);
                chevron.lineTo (c.x, c.y + 2.0f);
                chevron.lineTo (c.x + 3.5f, c.y - 1.5f);
                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            }

            // Resize grip on the right edge, shown once the pattern is hinted
            // so the handle is discoverable without cluttering every clip.
            if (lengthHinted)
            {
                const auto grip = resizeHandleBounds (r);
                if (! grip.isEmpty())
                {
                    g.setColour (juce::Colour (0xffffd479));
                    g.fillRect (grip.getRight() - 2.0f, grip.getY() + 3.0f, 2.0f, grip.getHeight() - 6.0f);
                }
            }
        }
    }

    // ghost of the bar the paint tool would fill, so the tool's effect is
    // visible before committing to it
    if (tool == Tool::paint && mouseIsOver && dragMode == DragMode::none
            && hoverRow >= 0 && hoverRow < numRows)
    {
        auto generator = song.getGenerator (hoverRow);
        if (auto pattern = patternForRow (generator))
            if (isRangeFree (generator, hoverStartBeats, pattern->getLengthBeats()))
            {
                const auto r = slotBounds (hoverRow, hoverStartBeats, pattern->getLengthBeats());
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.drawRoundedRectangle (r, 3.0f, 1.0f);
            }
    }

    // playhead
    g.setColour (juce::Colours::orangered);
    g.drawVerticalLine ((int) beatToX (playheadBeats), (float) headerHeight, (float) getHeight());

    // Rubber band over the grid but under the header, so a band dragged up
    // into the ruler does not paint over it.
    if (dragMode == DragMode::rubberBand && ! rubberBand.isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRect (rubberBand);
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawRect (rubberBand, 1.0f);
    }

    // Header band last and opaque: it is pinned to the visible area, so it
    // covers whatever grid has scrolled underneath it.
    const auto header = headerBounds();
    g.setColour (juce::Colour (0xff1b1b1f));
    g.fillRect (toolbarBounds());
    paintRuler (g);
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine ((int) header.getBottom() - 1, 0.0f, (float) getWidth());

    const auto statusX = zoomInButton.getRight() + 10;
    const auto statusArea = juce::Rectangle<int> (statusX, (int) header.getY(),
                                                  juce::jmax (0, getWidth() - statusX - 6), toolbarHeight);
    g.setFont (11.0f);

    // While a pattern length is in play, say so plainly: this is the one edit
    // here that reaches beyond the clip being dragged.
    if (auto hintGenerator = song.findGenerator (lengthHintGeneratorId))
        if (auto hintPattern = hintGenerator->findPattern (lengthHintPatternId))
        {
            const auto placements = countPlacements (lengthHintGeneratorId, lengthHintPatternId);
            g.setColour (juce::Colour (0xffffd479));
            g.drawText (hintPattern->getName() + ": "
                            + juce::String (hintPattern->getLengthBeats() / snapBeats, 0) + " bars"
                            + (placements > 1 ? " - drag resizes all " + juce::String (placements) + " placements"
                                              : juce::String()),
                        statusArea, juce::Justification::centredLeft);
            return;
        }

    // what the paint tool is currently holding
    if (auto generator = song.findGenerator (selectedGeneratorId))
        if (auto pattern = patternForRow (*generator))
        {
            g.setColour (juce::Colour (0xff9a9aa4));
            g.drawText (generator->getName() + " / " + pattern->getName(),
                        statusArea, juce::Justification::centredLeft);
        }
}

//==============================================================================
// Editing

bool PlaylistComponent::paintClip (int row, double beat)
{
    auto generator = song.getGenerator (row);
    auto pattern = patternForRow (generator);
    if (! pattern || pattern->getLengthBeats() <= 0.0)
        return false;

    const auto start = snapToBar (beat);

    // Never stack. This is also what keeps a paint drag cheap: once a slot is
    // filled the test fails, so sweeping back and forth writes nothing.
    if (! isRangeFree (generator, start, pattern->getLengthBeats()))
        return false;

    song.getPlaylist().addClip (generator, *pattern, start, &undoManager);
    return true;
}

bool PlaylistComponent::eraseClip (int row, double beat)
{
    auto generator = song.getGenerator (row);
    auto clip = clipAt (generator, beat);
    if (! clip)
        return false;

    song.getPlaylist().removeClip (*clip, &undoManager);
    return true;
}

void PlaylistComponent::dragSelectionTo (double targetStart)
{
    if (selectedClips.size() != dragOriginStarts.size()
            || std::abs (targetStart - dragLastStart) < beatTolerance)
        return;   // nothing to write: a drag inside the same bar must not resync the Edit

    // Every model write calls back into refresh(), which rewrites
    // selectedClips, so hold our own copy for the duration.
    const auto clips = selectedClips;

    // Never let the leftmost clip run off the front of the song.
    const auto minOrigin = *std::min_element (dragOriginStarts.begin(), dragOriginStarts.end());
    const auto delta = std::max (targetStart - dragAnchorOriginStart, -minOrigin);

    // All-or-nothing: if any clip would land on top of an unselected one, the
    // whole gesture stays where it is rather than half-moving the selection.
    for (size_t i = 0; i < clips.size(); ++i)
    {
        model::PlaylistClip clip (clips[i]);
        auto generator = song.findGenerator (clip.getGeneratorId());
        if (! generator)
            return;
        auto pattern = generator->findPattern (clip.getPatternId());
        if (! pattern)
            return;
        if (! isRangeFree (*generator, dragOriginStarts[i] + delta, pattern->getLengthBeats(), true))
            return;
    }

    for (size_t i = 0; i < clips.size(); ++i)
        model::PlaylistClip (clips[i]).setStart (dragOriginStarts[i] + delta, &undoManager);

    dragLastStart = dragAnchorOriginStart + delta;
}

void PlaylistComponent::duplicateSelection()
{
    if (selectedClips.empty())
        return;

    // Adding clips calls back into refresh(), which rewrites selectedClips, so
    // hold our own copy for the duration.
    const auto clips = selectedClips;

    // Offset by the span of the selection, rounded up to a bar, so repeated ⌘D
    // lays down a chain of copies rather than piling them up.
    double spanStart = std::numeric_limits<double>::max(), spanEnd = 0.0;
    for (const auto& state : clips)
    {
        model::PlaylistClip clip (state);
        if (auto generator = song.findGenerator (clip.getGeneratorId()))
            if (auto pattern = generator->findPattern (clip.getPatternId()))
            {
                spanStart = std::min (spanStart, clip.getStart());
                spanEnd = std::max (spanEnd, clip.getStart() + pattern->getLengthBeats());
            }
    }
    if (spanEnd <= spanStart)
        return;

    const auto offset = std::max (snapBeats, std::ceil ((spanEnd - spanStart) / snapBeats) * snapBeats);

    undoManager.beginNewTransaction();

    std::vector<juce::ValueTree> copies;
    for (const auto& state : clips)
    {
        model::PlaylistClip clip (state);
        auto generator = song.findGenerator (clip.getGeneratorId());
        if (! generator)
            continue;
        auto pattern = generator->findPattern (clip.getPatternId());
        if (! pattern)
            continue;

        const auto start = clip.getStart() + offset;
        if (! isRangeFree (*generator, start, pattern->getLengthBeats()))
            continue;

        copies.push_back (song.getPlaylist().addClip (*generator, *pattern, start, &undoManager).state);
    }

    // Select the copies so ⌘D again continues the chain.
    if (! copies.empty())
    {
        selectedClips = std::move (copies);
        repaint();
    }
}

void PlaylistComponent::deleteSelection()
{
    if (selectedClips.empty())
        return;

    // Each removal calls back into refresh(), which drops the clip from
    // selectedClips, so iterate a copy rather than the live vector.
    const auto clips = selectedClips;

    undoManager.beginNewTransaction();
    auto playlist = song.getPlaylist();
    for (const auto& state : clips)
        playlist.removeClip (model::PlaylistClip (state), &undoManager);

    clearSelection();
}

int PlaylistComponent::countPlacements (const juce::String& generatorId,
                                        const juce::String& patternId) const
{
    int count = 0;
    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getGeneratorId() == generatorId && clip.getPatternId() == patternId)
            ++count;
    return count;
}

void PlaylistComponent::showPatternMenu (const model::PlaylistClip& clip,
                                         juce::Rectangle<float> buttonBounds)
{
    auto generator = song.findGenerator (clip.getGeneratorId());
    if (! generator || generator->getNumPatterns() == 0)
        return;

    // Only this generator's own patterns are offered: a clip plays its
    // generator's track, so anything else would have nowhere to sound.
    const auto patterns = generator->getPatterns();
    const auto currentId = clip.getPatternId();

    juce::PopupMenu menu;
    for (int i = 0; i < (int) patterns.size(); ++i)
        menu.addItem (i + 1, patterns[(size_t) i].getName(), true,
                      patterns[(size_t) i].getId() == currentId);

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (buttonBounds.toNearestInt()));

    // The menu is asynchronous, so nothing captured here may be dereferenced
    // without checking that we - and the clip - are still around.
    juce::Component::SafePointer<PlaylistComponent> safeThis (this);
    const auto clipState = clip.state;

    menu.showMenuAsync (options, [safeThis, clipState, patterns] (int result)
    {
        if (safeThis == nullptr || result <= 0 || result > (int) patterns.size())
            return;

        model::PlaylistClip target (clipState);
        if (! clipState.isAChildOf (safeThis->song.getPlaylist().state))
            return;

        auto owner = safeThis->song.findGenerator (target.getGeneratorId());
        if (! owner)
            return;

        // Re-look the pattern up rather than trusting the copy the menu was
        // built from: it may have been renamed, resized or deleted since.
        auto chosen = owner->findPattern (patterns[(size_t) (result - 1)].getId());
        if (! chosen || chosen->getId() == target.getPatternId())
            return;

        // A longer pattern still has to fit; refuse rather than overlap.
        if (! safeThis->isRangeFree (*owner, target.getStart(), chosen->getLengthBeats(),
                                     false, clipState))
            return;

        safeThis->undoManager.beginNewTransaction();
        target.state.setProperty (model::ids::patternId, chosen->getId(), &safeThis->undoManager);
    });
}

double PlaylistComponent::maxLengthForPattern (const model::Generator& generator,
                                               const model::Pattern& pattern) const
{
    // Growing a pattern grows every placement of it at once, so the ceiling is
    // the tightest gap in front of any of them.
    double maxLength = 256.0;
    const auto clips = song.getPlaylist().getClips();

    for (const auto& clip : clips)
    {
        if (clip.getGeneratorId() != generator.getId() || clip.getPatternId() != pattern.getId())
            continue;

        for (const auto& other : clips)
        {
            if (other.getGeneratorId() != generator.getId() || other.state == clip.state)
                continue;
            if (other.getStart() > clip.getStart() + beatTolerance)
                maxLength = std::min (maxLength, other.getStart() - clip.getStart());
        }
    }

    return std::max (snapBeats, maxLength);
}

void PlaylistComponent::resizePatternTo (double newLengthBeats)
{
    if (! resizePattern.isValid())
        return;

    model::Pattern pattern (resizePattern);
    const auto length = juce::jlimit (snapBeats, resizeMaxLength, snapLengthToBar (newLengthBeats));

    // Only on a real change: each write rebuilds every clip of this pattern in
    // the Edit, so a drag inside one bar has to stay silent.
    if (std::abs (length - pattern.getLengthBeats()) < beatTolerance)
        return;

    pattern.setLengthBeats (length, &undoManager);
}

void PlaylistComponent::setLengthHint (const juce::String& generatorId, const juce::String& patternId)
{
    if (generatorId == lengthHintGeneratorId && patternId == lengthHintPatternId)
        return;

    lengthHintGeneratorId = generatorId;
    lengthHintPatternId = patternId;
    repaint();
}

void PlaylistComponent::dragLoopTo (double beat)
{
    // Loop edges land on the nearest bar, not the one before, so the range
    // follows the pointer in both directions.
    const auto snapped = std::max (0.0, std::round (beat / snapBeats) * snapBeats);
    const auto start = std::min (loopAnchorBeats, snapped);
    const auto end = std::max (loopAnchorBeats, snapped);

    if (end - start < beatTolerance)
        return;   // still in the bar the drag started in: no range yet

    loopDragMoved = true;

    if (song.hasLoopRange()
            && std::abs (start - song.getLoopStart()) < beatTolerance
            && std::abs (end - song.getLoopEnd()) < beatTolerance)
        return;

    song.setLoopRange (start, end, &undoManager);
}

void PlaylistComponent::updateRubberBand (juce::Point<float> position)
{
    rubberBand = juce::Rectangle<float> (rubberBandAnchor, position);

    // Selection is UI state, so recomputing it from scratch on every move is
    // free: no model write, no resync.
    auto newSelection = rubberBandBaseSelection;

    for (int row = 0; row < song.getNumGenerators(); ++row)
    {
        auto generator = song.getGenerator (row);

        for (const auto& clip : song.getPlaylist().getClips())
        {
            if (clip.getGeneratorId() != generator.getId())
                continue;

            const auto bounds = boundsForClip (row, clip);
            if (! bounds || ! bounds->intersects (rubberBand))
                continue;

            if (std::find (newSelection.begin(), newSelection.end(), clip.state) == newSelection.end())
                newSelection.push_back (clip.state);
        }
    }

    selectedClips = std::move (newSelection);
    repaint();
}

//==============================================================================
// Mouse

juce::MouseCursor PlaylistComponent::cursorFor (juce::Point<float> position,
                                                juce::ModifierKeys mods) const
{
    if (rulerBounds().contains (position))
        return juce::MouseCursor::PointingHandCursor;   // drag sets the loop range

    if (position.x < (float) labelWidth || headerBounds().contains (position))
        return juce::MouseCursor::NormalCursor;

    const int row = yToRow (position.y);
    if (row < 0 || row >= song.getNumGenerators())
        return juce::MouseCursor::NormalCursor;

    auto generator = song.getGenerator (row);
    const auto beat = xToBeat (position.x);
    const auto clip = clipAt (generator, beat);

    // Right button and alt erase whichever tool is active.
    if (mods.isRightButtonDown() || mods.isAltDown())
        return clip ? eraseCursor : juce::MouseCursor::NormalCursor;

    if (clip)
    {
        if (const auto bounds = boundsForClip (row, *clip))
        {
            if (patternMenuBounds (*bounds).contains (position))
                return juce::MouseCursor::PointingHandCursor;
            if (resizeHandleBounds (*bounds).contains (position))
                return juce::MouseCursor::LeftRightResizeCursor;
        }

        return juce::MouseCursor::DraggingHandCursor;   // click selects, drag moves
    }

    if (tool == Tool::paint && patternForRow (generator))
        return juce::MouseCursor::CrosshairCursor;

    return juce::MouseCursor::NormalCursor;
}

void PlaylistComponent::updateHover (juce::Point<float> position)
{
    lastMousePosition = position;
    setMouseCursor (cursorFor (position, juce::ModifierKeys::getCurrentModifiers()));

    const int row = position.x >= (float) labelWidth && ! headerBounds().contains (position)
                        ? yToRow (position.y) : -1;
    const auto start = snapToBar (xToBeat (position.x));

    // Outline every placement of a pattern whose right edge is under the
    // pointer, so the reach of a resize is visible before the drag starts. A
    // drag in progress keeps the hint it set for itself.
    if (dragMode != DragMode::patternLength)
    {
        juce::String hintGeneratorId, hintPatternId;

        if (row >= 0 && row < song.getNumGenerators())
        {
            auto generator = song.getGenerator (row);
            if (auto clip = clipAt (generator, xToBeat (position.x)))
                if (const auto bounds = boundsForClip (row, *clip))
                    if (resizeHandleBounds (*bounds).contains (position))
                    {
                        hintGeneratorId = clip->getGeneratorId();
                        hintPatternId = clip->getPatternId();
                    }
        }

        setLengthHint (hintGeneratorId, hintPatternId);
    }

    if (row != hoverRow || std::abs (start - hoverStartBeats) > beatTolerance)
    {
        hoverRow = row;
        hoverStartBeats = start;
        repaint();   // only on crossing into another slot, not on every mouse move
    }
}

void PlaylistComponent::mouseMove (const juce::MouseEvent& e)
{
    mouseIsOver = true;
    updateHover (e.position);
}

void PlaylistComponent::mouseExit (const juce::MouseEvent&)
{
    mouseIsOver = false;
    hoverRow = -1;
    repaint();
}

void PlaylistComponent::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    // So that holding alt shows the erase cursor without moving the mouse.
    if (mouseIsOver)
        setMouseCursor (cursorFor (lastMousePosition, mods));
}

void PlaylistComponent::mouseDown (const juce::MouseEvent& e)
{
    mouseIsOver = true;
    lastMousePosition = e.position;
    grabKeyboardFocus();   // ⌘D and the tool keys are ours once the grid is clicked

    dragMode = DragMode::none;

    if (rulerBounds().contains (e.position) && ! e.mods.isRightButtonDown())
    {
        // Drag the ruler to set the loop range. Grabbing within a few pixels of
        // an existing edge drags that edge (the other one becomes the anchor);
        // anywhere else starts a fresh range from that bar. A click that never
        // leaves its bar clears the range - see mouseUp.
        loopAnchorBeats = std::max (0.0, std::round (xToBeat (e.position.x) / snapBeats) * snapBeats);
        loopDragMoved = false;

        if (song.hasLoopRange())
        {
            constexpr float grabPixels = 5.0f;
            if (std::abs (e.position.x - beatToX (song.getLoopStart())) <= grabPixels)
                loopAnchorBeats = song.getLoopEnd();
            else if (std::abs (e.position.x - beatToX (song.getLoopEnd())) <= grabPixels)
                loopAnchorBeats = song.getLoopStart();
        }

        dragMode = DragMode::loopRange;
        undoManager.beginNewTransaction();
        return;
    }

    if (headerBounds().contains (e.position))
        return;   // header band; the tool buttons handle their own clicks

    const int row = yToRow (e.position.y);
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
    const bool erasing = e.mods.isRightButtonDown() || e.mods.isAltDown();

    // One transaction per gesture, so a whole paint or erase drag is one undo.
    undoManager.beginNewTransaction();

    if (erasing)
    {
        dragMode = DragMode::erase;
        eraseClip (row, clickBeat);
        return;
    }

    if (auto clip = clipAt (generator, clickBeat))
    {
        const auto bounds = boundsForClip (row, *clip);

        // The chevron swaps which pattern the clip plays: it must not also
        // select the clip or start a move.
        if (bounds && patternMenuBounds (*bounds).contains (e.position))
        {
            showPatternMenu (*clip, patternMenuBounds (*bounds));
            return;
        }

        // The right edge resizes the PATTERN, so every placement of it grows
        // or shrinks with this one drag. The hint outline stays up throughout.
        if (bounds && resizeHandleBounds (*bounds).contains (e.position))
            if (auto pattern = generator.findPattern (clip->getPatternId()))
            {
                dragMode = DragMode::patternLength;
                resizePattern = pattern->state;
                resizeStartBeats = clip->getStart();
                resizeMaxLength = maxLengthForPattern (generator, *pattern);
                setLengthHint (generator.getId(), pattern->getId());
                return;
            }

        selectClip (*clip, e.mods.isCommandDown() || e.mods.isShiftDown());

        // Drag from here moves the selection. Snapshot the starts now so the
        // move is always relative to where the gesture began.
        dragMode = DragMode::move;
        dragGrabOffsetBeats = clickBeat - clip->getStart();
        dragAnchorOriginStart = clip->getStart();
        dragLastStart = clip->getStart();
        dragOriginStarts.clear();
        for (const auto& state : selectedClips)
            dragOriginStarts.push_back (model::PlaylistClip (state).getStart());
        return;
    }

    if (tool == Tool::paint)
    {
        dragMode = DragMode::paint;
        paintClip (row, clickBeat);
        updateHover (e.position);
        return;
    }

    // Select tool on empty space: rubber band. ⌘ or ⇧ adds to what is already
    // selected, so the band starts from the current selection rather than
    // replacing it.
    if (! (e.mods.isCommandDown() || e.mods.isShiftDown()))
        clearSelection();

    dragMode = DragMode::rubberBand;
    rubberBandAnchor = e.position;
    rubberBand = {};
    rubberBandBaseSelection = selectedClips;
}

void PlaylistComponent::mouseDrag (const juce::MouseEvent& e)
{
    lastMousePosition = e.position;

    const int row = yToRow (e.position.y);
    const auto beat = xToBeat (e.position.x);
    const bool insideGrid = e.position.x >= (float) labelWidth
                                && ! headerBounds().contains (e.position)
                                && row >= 0 && row < song.getNumGenerators();

    switch (dragMode)
    {
        case DragMode::paint:
            // paintClip is a no-op unless the pointer has entered an empty
            // slot, which is what keeps the model (and EditSync) quiet.
            if (insideGrid && paintClip (row, beat))
                updateHover (e.position);
            break;

        case DragMode::erase:
            if (insideGrid)
                eraseClip (row, beat);
            break;

        case DragMode::move:
            dragSelectionTo (snapToBar (beat - dragGrabOffsetBeats));
            break;

        case DragMode::loopRange:
            dragLoopTo (xToBeat (e.position.x));
            break;

        case DragMode::patternLength:
            resizePatternTo (beat - resizeStartBeats);
            break;

        case DragMode::rubberBand:
            updateRubberBand (e.position);
            break;

        case DragMode::none:
            break;
    }
}

void PlaylistComponent::mouseUp (const juce::MouseEvent& e)
{
    // A ruler click that never grew into a drag means "no loop range", which is
    // also how the range is cleared.
    if (dragMode == DragMode::loopRange && ! loopDragMoved && song.hasLoopRange())
        song.clearLoopRange (&undoManager);

    dragMode = DragMode::none;
    dragOriginStarts.clear();
    resizePattern = {};
    rubberBand = {};
    rubberBandBaseSelection.clear();
    updateHover (e.position);
    repaint();
}

void PlaylistComponent::mouseWheelMove (const juce::MouseEvent& e,
                                        const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        setPixelsPerBeat (pixelsPerBeat * std::pow (2.0, (double) wheel.deltaY * 1.5), e.position.x);
        return;
    }

    // Everything else belongs to the viewport: plain scrolling pans the grid,
    // and a shifted or horizontal wheel pans it in time.
    Component::mouseWheelMove (e, wheel);
}

void PlaylistComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() || e.position.x < (float) labelWidth
            || headerBounds().contains (e.position))
        return;

    const int row = yToRow (e.position.y);
    if (row < 0 || row >= song.getNumGenerators())
        return;

    auto generator = song.getGenerator (row);
    if (auto clip = clipAt (generator, xToBeat (e.position.x)))
        if (onEditPattern)
            onEditPattern (clip->getGeneratorId(), clip->getPatternId());
}

bool PlaylistComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0))
    {
        duplicateSelection();
        return true;
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }

    if (key == juce::KeyPress ('b'))
    {
        setTool (Tool::paint);
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

    // MainComponent owns Space and ⌘Z / ⇧⌘Z globally: let everything else
    // bubble up to it.
    return false;
}

} // namespace orionish::app
