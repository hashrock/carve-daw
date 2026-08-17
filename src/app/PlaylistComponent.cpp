#include "PlaylistComponent.h"

#include <algorithm>
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
    selectToolButton.setTooltip ("Select (E): click and drag clips");

    for (auto* b : { &paintToolButton, &selectToolButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe08a3c));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        addAndMakeVisible (b);
    }

    // Buttons never take focus themselves: the component owns ⌘D and the tool
    // shortcuts, and clicking a tool must not steal the keyboard from the grid.
    paintToolButton.setWantsKeyboardFocus (false);
    selectToolButton.setWantsKeyboardFocus (false);
    paintToolButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    selectToolButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    paintToolButton.onClick = [this] { setTool (Tool::paint); };
    selectToolButton.onClick = [this] { setTool (Tool::select); };

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
    setSize (labelWidth + juce::roundToInt (getContentLengthBeats() * pixelsPerBeat),
             headerHeight + juce::jmax (1, song.getNumGenerators()) * rowHeight);
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
    paintToolButton.setBounds (origin.x + 6, origin.y + 4, 54, headerHeight - 8);
    selectToolButton.setBounds (paintToolButton.getRight(), origin.y + 4, 54, headerHeight - 8);
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

bool PlaylistComponent::isRangeFree (const model::Generator& generator, double startBeats,
                                     double lengthBeats, bool ignoreSelectedClips) const
{
    for (const auto& clip : song.getPlaylist().getClips())
    {
        if (clip.getGeneratorId() != generator.getId())
            continue;
        if (ignoreSelectedClips && isSelected (clip.state))
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

void PlaylistComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    const auto lengthBeats = getContentLengthBeats();
    const int numRows = song.getNumGenerators();

    // vertical grid
    for (double beat = 0.0; beat <= lengthBeats; beat += 1.0)
    {
        const bool isBar = std::fmod (beat, 4.0) < beatTolerance;
        g.setColour (isBar ? juce::Colour (0xff4a4a52) : juce::Colour (0xff2c2c31));
        g.drawVerticalLine ((int) beatToX (beat), (float) headerHeight, (float) getHeight());
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

            g.setColour (selected ? rowColour.brighter (0.4f) : rowColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 3.0f, selected ? 1.6f : 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.setFont (11.0f);
            g.drawText (pattern->getName(), r.reduced (4.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft);
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

    // Header band last and opaque: it is pinned to the visible area, so it
    // covers whatever grid has scrolled underneath it.
    const auto header = headerBounds();
    g.setColour (juce::Colour (0xff1b1b1f));
    g.fillRect (header);
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine ((int) header.getBottom() - 1, 0.0f, (float) getWidth());

    // what the paint tool is currently holding
    if (auto generator = song.findGenerator (selectedGeneratorId))
        if (auto pattern = patternForRow (*generator))
        {
            g.setColour (juce::Colour (0xff9a9aa4));
            g.setFont (11.0f);
            g.drawText (generator->getName() + " / " + pattern->getName(),
                        selectToolButton.getRight() + 10, (int) header.getY(),
                        juce::jmax (0, getWidth() - selectToolButton.getRight() - 16), headerHeight,
                        juce::Justification::centredLeft);
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

//==============================================================================
// Mouse

juce::MouseCursor PlaylistComponent::cursorFor (juce::Point<float> position,
                                                juce::ModifierKeys mods) const
{
    if (position.x < (float) labelWidth || headerBounds().contains (position))
        return juce::MouseCursor::NormalCursor;

    const int row = yToRow (position.y);
    if (row < 0 || row >= song.getNumGenerators())
        return juce::MouseCursor::NormalCursor;

    auto generator = song.getGenerator (row);
    const auto beat = xToBeat (position.x);
    const bool overClip = clipAt (generator, beat).has_value();

    // Right button and alt erase whichever tool is active.
    if (mods.isRightButtonDown() || mods.isAltDown())
        return overClip ? eraseCursor : juce::MouseCursor::NormalCursor;

    if (overClip)
        return juce::MouseCursor::DraggingHandCursor;   // click selects, drag moves

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

    clearSelection();   // select tool on empty space; rubber band lands here later
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

        case DragMode::none:
            break;
    }
}

void PlaylistComponent::mouseUp (const juce::MouseEvent& e)
{
    dragMode = DragMode::none;
    dragOriginStarts.clear();
    updateHover (e.position);
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

    // MainComponent owns Space and ⌘Z / ⇧⌘Z globally: let everything else
    // bubble up to it.
    return false;
}

} // namespace orionish::app
