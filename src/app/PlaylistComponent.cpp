#include "PlaylistComponent.h"

#include "TimelineView.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include <juce_audio_formats/juce_audio_formats.h>

namespace carve::app
{

namespace
{
    // Beats are doubles, so every "is this the same bar / does this overlap"
    // test needs a little slack.
    constexpr double beatTolerance = 1.0e-9;

    // The clipboard payload's root, named for this app: the system clipboard is
    // shared with every other program on the machine, so a paste has to be able
    // to tell that what it found is not ours and leave it alone.
    const juce::Identifier clipboardType ("CARVECLIPS");
    const juce::Identifier clipboardVersion ("version");
    constexpr int clipboardFormatVersion = 1;

    // Written next to each copied clip so a paste into a song where the
    // original generator has gone still has something to match on.
    const juce::Identifier clipboardGeneratorName ("generatorName");
    const juce::Identifier clipboardPatternName ("patternName");

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

    // What can be dropped on the grid. Deliberately the formats JUCE reads out
    // of the box, which is also all a wave clip can play: registerBasicFormats
    // is what both the peak reader and the model's length probe use.
    bool isAudioFile (const juce::String& path)
    {
        return juce::File (path).hasFileExtension ("wav;aiff;aif;flac;ogg;mp3;m4a;caf");
    }

    // "140" rather than "140.00", because a marker is only a few characters
    // wide -- but a tempo that really has a fraction in it still shows it.
    juce::String formatBpm (double bpm)
    {
        if (std::abs (bpm - std::round (bpm)) < 0.005)
            return juce::String ((int) std::round (bpm));

        return juce::String (bpm, 2).trimCharactersAtEnd ("0");
    }

    juce::String formatTimeSig (model::TimeSignature sig)
    {
        return juce::String (sig.numerator) + "/" + juce::String (sig.denominator);
    }

    // "0.65" rather than "0.650000", and whole numbers without the point: the
    // readout is glanced at mid-drag, not studied.
    juce::String formatAutoValue (float value)
    {
        if (std::abs (value - std::round (value)) < 0.005f)
            return juce::String ((int) std::round (value));

        return juce::String (value, 2).trimCharactersAtEnd ("0");
    }

    // The curve's value at a beat. Segments are straight lines here -- the
    // model's bend value is deliberately ignored this pass -- and the curve is
    // flat before the first point and after the last, which is also how the
    // engine plays it.
    float curveValueAtBeat (const std::vector<model::AutomationPoint>& points, double beat)
    {
        if (points.empty())
            return 0.0f;

        if (beat <= points.front().getBeat())
            return points.front().getValue();

        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto b0 = points[i - 1].getBeat(), b1 = points[i].getBeat();

            if (beat <= b1)
            {
                // Two points on one beat make a step; the later one wins from
                // that beat on, which is what dividing by ~zero would not say.
                if (b1 - b0 < beatTolerance)
                    return points[i].getValue();

                const auto t = (beat - b0) / (b1 - b0);
                return points[i - 1].getValue()
                         + (float) t * (points[i].getValue() - points[i - 1].getValue());
            }
        }

        return points.back().getValue();
    }

    // The colours the two kinds of change carry everywhere they are drawn: on
    // the lane, and as the line down the grid where they take effect.
    constexpr juce::uint32 tempoColour = 0xff4fa3c7, timeSigColour = 0xff9b7fd4;

    // What the signature menu offers. Anything else needs a text field for two
    // numbers, which is a lot of dialog for a case this rare.
    constexpr model::TimeSignature commonTimeSignatures[] = {
        { 4, 4 }, { 3, 4 }, { 2, 4 }, { 5, 4 }, { 6, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }
    };

    // Built where it is used rather than kept: a Font at namespace scope would
    // be constructed before the graphics side of JUCE is ready for it.
    juce::Font markerFont()  { return juce::Font (juce::FontOptions (10.0f)); }
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
    trimClip = {};
    dragAutoPoint = {};
    dragAutoRow = -1;
    rubberBand = {};
    fileDropTarget.reset();

    // UI state keyed by generator id, so a different song's ids mean nothing
    // to it -- start it afresh the way the selection is.
    expandedGenerators.clear();
    laneParamChoices.clear();

    // Keyed by path, so it would still be correct across songs -- but nothing
    // shrinks it otherwise, and a new song rarely wants the old one's files.
    peakCache.clear();

    clearSelection();
    refresh();

    // A fresh song is also the first moment the host has both its callbacks
    // wired up and something to describe, so state the shortcuts now.
    updateShortcutHelp();
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
    updateShortcutHelp();   // the drag a tool offers is half of what the bar lists
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
    selectionChanged();   // an undo or a reload can have taken clips out from under us
    updateSize();
    repaint();
}

double PlaylistComponent::getContentLengthBeats() const
{
    // Always a few bars past the last placement, so there is empty grid to
    // paint or drop into without scrolling to the very end first.
    const auto length = std::max (32.0, song.getLengthBeats() + 16.0);
    return std::ceil (length / 16.0) * 16.0;
}

void PlaylistComponent::updateSize()
{
    auto width = labelWidth + juce::roundToInt (getContentLengthBeats() * pixelsPerBeat);

    // Zoomed far enough out the song is narrower than the viewport; stay at
    // least as wide as it so the header band still spans the window.
    if (auto* viewport = getViewport())
        width = juce::jmax (width, viewport->getMaximumVisibleWidth());

    // Row heights vary now: an expanded row is its clip band plus its lane.
    int rowsHeight = 0;
    for (int row = 0; row < song.getNumGenerators(); ++row)
        rowsHeight += rowTotalHeight (row);

    setSize (width, headerHeight + juce::jmax (rowHeight, rowsHeight));
}

//==============================================================================
// Row geometry
//
// Rows are stacked, so a row's top is the sum of every row above it: an
// expanded row pushes everything below it down by its lane. O(rows) per call,
// which for the handful of generators a song holds is cheaper than keeping a
// cache correct across every model edit.

int PlaylistComponent::rowTotalHeight (int row) const
{
    return rowHeight + (isRowExpanded (row) ? automationLaneHeight : 0);
}

float PlaylistComponent::rowY (int row) const
{
    int y = headerHeight;
    for (int r = 0; r < row; ++r)
        y += rowTotalHeight (r);
    return (float) y;
}

int PlaylistComponent::rowAt (float y) const
{
    if (y < (float) headerHeight)
        return -1;

    const int numRows = song.getNumGenerators();
    float bottom = (float) headerHeight;

    for (int row = 0; row < numRows; ++row)
    {
        bottom += (float) rowTotalHeight (row);
        if (y < bottom)
            return row;
    }

    // Below the last row, like the old fixed-height division used to say.
    return numRows;
}

int PlaylistComponent::clipRowAt (float y) const
{
    const int row = rowAt (y);
    if (row < 0 || row >= song.getNumGenerators())
        return row;

    return y < rowY (row) + (float) rowHeight ? row : -1;
}

int PlaylistComponent::laneRowAt (float y) const
{
    const int row = rowAt (y);
    if (row < 0 || row >= song.getNumGenerators() || ! isRowExpanded (row))
        return -1;

    return y >= rowY (row) + (float) rowHeight ? row : -1;
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

    zoomAroundAnchor (*this, anchorX,
                      [this] (float x) { return xToBeat (x); },
                      [this] (double beat) { return beatToX (beat); },
                      [this, newPixelsPerBeat]
                      {
                          pixelsPerBeat = newPixelsPerBeat;
                          updateSize();
                      });

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
// Bars
//
// Every bar line in the component comes from here, so a song that changes time
// signature part way through draws, snaps and numbers its bars the same way in
// the ruler, the grid and every gesture.

double PlaylistComponent::snapToBar (double beat) const
{
    if (beat <= 0.0)
        return 0.0;

    return song.beatOfBar (song.toBarsAndBeats (beat).bar);
}

double PlaylistComponent::nextBarAfter (double beat) const
{
    // beatOfBar of the following bar is always past this beat, wherever in its
    // bar the beat sits, so there is no special case for a beat on a bar line.
    return song.beatOfBar ((beat <= 0.0 ? 0 : song.toBarsAndBeats (beat).bar) + 1);
}

double PlaylistComponent::nearestBar (double beat, const juce::ValueTree& ignore) const
{
    if (beat <= 0.0)
        return 0.0;

    // The bar grid restarts at every change, so the last one at or before the
    // beat is the only one that decides where the bar lines around it fall.
    model::TimeSignature signature;
    double from = 0.0;

    for (const auto& change : song.getTimeSigChanges())
    {
        if (change.state == ignore)
            continue;

        if (change.getStartBeat() > beat)
            break;

        from = change.getStartBeat();
        signature = change.getSignature();
    }

    const auto beatsPerBar = std::max (0.25, signature.getBeatsPerBar());
    const auto before = from + std::floor ((beat - from) / beatsPerBar + beatTolerance) * beatsPerBar;
    const auto after = before + beatsPerBar;

    return beat - before < after - beat ? before : after;
}

double PlaylistComponent::barLengthAt (double beat) const
{
    const auto start = snapToBar (beat);
    return std::max (0.25, nextBarAfter (start) - start);
}

void PlaylistComponent::forEachBar (double untilBeat,
                                    const std::function<void (int, double, double)>& fn) const
{
    const auto changes = song.getTimeSigChanges();
    model::TimeSignature signature;
    size_t next = 0;
    double beat = 0.0;

    // A change is only picked up on the bar line at or after it starts: the
    // lane snaps every one it writes to a bar line, and rounding a hand-written
    // one up is better than drawing a bar the model would not agree exists.
    for (int bar = 0; beat <= untilBeat; ++bar)
    {
        while (next < changes.size() && changes[next].getStartBeat() <= beat + beatTolerance)
            signature = changes[next++].getSignature();

        const auto length = std::max (0.25, signature.getBeatsPerBar());
        fn (bar, beat, length);
        beat += length;
    }
}

double PlaylistComponent::shortestBar() const
{
    // 4/4 until the first change, so a song whose changes are all longer bars
    // still thins its lines against the four beats it opens with.
    auto shortest = model::TimeSignature().getBeatsPerBar();

    for (const auto& change : song.getTimeSigChanges())
        shortest = std::min (shortest, change.getSignature().getBeatsPerBar());

    return std::max (0.25, shortest);
}

//==============================================================================
// Model queries

std::optional<model::Pattern> PlaylistComponent::patternForRow (const model::Generator& generator) const
{
    // An audio row plays files, never patterns. It starts with no patterns at
    // all, but it is one click away from having one -- the slot grid will
    // happily make one for whichever generator is selected, and a drop selects
    // the audio generator it just filled. Without this the paint tool would
    // then lay clips on the row that sit there playing nothing, because
    // EditSync only ever looks at its audio placements.
    if (generator.isAudio())
        return std::nullopt;

    // the row's current pattern: the selected one for the selected generator,
    // its first pattern otherwise
    auto pattern = generator.getId() == selectedGeneratorId
                       ? generator.findPattern (selectedPatternId)
                       : std::nullopt;
    if (! pattern && generator.getNumPatterns() > 0)
        pattern = generator.getPattern (0);
    return pattern;
}

// How much of the grid an audio clip covers. Its length is in seconds, so this
// depends on the tempo it plays through and therefore on where it sits.
double PlaylistComponent::audioLengthBeats (const model::AudioClip& clip) const
{
    return song.beatsFromSeconds (song.secondsFromBeats (clip.getStart()) + clip.getLengthSeconds())
             - clip.getStart();
}

std::optional<PlaylistComponent::Placement> PlaylistComponent::placementFor (const juce::ValueTree& state) const
{
    if (state.hasType (model::ids::AUDIOCLIP))
    {
        const model::AudioClip clip (state);
        return Placement { state, clip.getStart(), audioLengthBeats (clip) };
    }

    if (! state.hasType (model::ids::CLIP))
        return std::nullopt;

    const model::PlaylistClip clip (state);

    if (auto generator = song.findGenerator (clip.getGeneratorId()))
        if (auto pattern = generator->findPattern (clip.getPatternId()))
            if (const auto length = clip.getLength (pattern->getLengthBeats()); length > 0.0)
                return Placement { state, clip.getStart(), length };

    return std::nullopt;
}

std::vector<PlaylistComponent::Placement> PlaylistComponent::placementsFor (const model::Generator& generator) const
{
    // Both node types name their generator the same way, which is the whole
    // reason a row can be walked without caring which kind it holds.
    const auto generatorId = generator.getId();
    std::vector<Placement> result;

    for (const auto& child : song.getPlaylist().state)
        if (child[model::ids::generatorId].toString() == generatorId)
            if (auto placement = placementFor (child))
                result.push_back (*placement);

    return result;
}

std::optional<PlaylistComponent::Placement> PlaylistComponent::placementAt (const model::Generator& generator,
                                                                           double beat) const
{
    const auto placements = placementsFor (generator);

    // iterate in reverse so the most recently added clip wins on overlap
    for (auto i = placements.rbegin(); i != placements.rend(); ++i)
        if (beat >= i->start && beat < i->getEnd())
            return *i;

    return std::nullopt;
}

juce::Rectangle<float> PlaylistComponent::slotBounds (int row, double startBeats,
                                                      double lengthBeats) const
{
    return { beatToX (startBeats), rowY (row) + 2.0f,
             (float) (lengthBeats * pixelsPerBeat) - 1.0f, (float) rowHeight - 4.0f };
}

std::optional<juce::Rectangle<float>> PlaylistComponent::boundsForClip (int row,
                                                                       const juce::ValueTree& state) const
{
    if (auto placement = placementFor (state))
        return slotBounds (row, placement->start, placement->length);
    return std::nullopt;
}

juce::Rectangle<float> PlaylistComponent::patternMenuBounds (juce::Rectangle<float> clipRect) const
{
    // Only worth offering once the clip is wide enough to hold the chevron and
    // still show some of its name.
    if (clipRect.getWidth() < 46.0f)
        return {};

    return { clipRect.getRight() - menuButtonInset - menuButtonWidth, clipRect.getY() + 1.0f,
             menuButtonWidth, clipRect.getHeight() * 0.5f };
}

juce::Rectangle<float> PlaylistComponent::trimHandleBounds (juce::Rectangle<float> clipRect) const
{
    // Below this the grip would be most of the clip, and there would be no way
    // left to grab it to move it.
    if (clipRect.getWidth() < 24.0f)
        return {};

    return clipRect.removeFromRight (trimHandleWidth);
}

bool PlaylistComponent::isRangeFree (const model::Generator& generator, double startBeats,
                                     double lengthBeats, bool ignoreSelectedClips,
                                     const juce::ValueTree& alsoIgnore) const
{
    for (const auto& placement : placementsFor (generator))
    {
        if (ignoreSelectedClips && isSelected (placement.state))
            continue;
        if (alsoIgnore.isValid() && placement.state == alsoIgnore)
            continue;

        if (startBeats < placement.getEnd() - beatTolerance
                && placement.start < startBeats + lengthBeats - beatTolerance)
            return false;
    }
    return true;
}

//==============================================================================
// Selection (UI state only)

bool PlaylistComponent::isSelected (const juce::ValueTree& clip) const
{
    return std::find (selectedClips.begin(), selectedClips.end(), clip) != selectedClips.end();
}

void PlaylistComponent::selectClip (const juce::ValueTree& clip, bool extend)
{
    if (! extend)
    {
        if (isSelected (clip))
            return;   // keep a multi-selection intact when re-clicking part of it
        selectedClips.clear();
    }
    else if (isSelected (clip))
    {
        std::erase (selectedClips, clip);
        selectionChanged();
        return;
    }

    selectedClips.push_back (clip);
    selectionChanged();
}

void PlaylistComponent::clearSelection()
{
    if (selectedClips.empty())
        return;
    selectedClips.clear();
    selectionChanged();
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

void PlaylistComponent::selectionChanged()
{
    if (selectedClips == notifiedSelection)
        return;   // the rubber band recomputes the same set on every mouse move

    notifiedSelection = selectedClips;
    updateShortcutHelp();   // some shortcuts only exist while something is selected
    repaint();

    if (onClipSelectionChanged)
    {
        // Pattern clips only: what the host puts on the other end of this is a
        // panel of pattern-clip properties, and an audio placement has none of
        // them (its length is trimmed on the grid, and transposing it would
        // mean pitch-shifting). Selecting one still moves, copies and deletes.
        std::vector<model::PlaylistClip> clips;
        for (const auto& state : selectedClips)
            if (state.hasType (model::ids::CLIP))
                clips.emplace_back (state);

        onClipSelectionChanged (clips);
    }
}

void PlaylistComponent::updateShortcutHelp()
{
    // What the bar lists is what works *now*: the active tool, whether there is
    // a selection to act on, and whether a held modifier has re-pointed the
    // drag. Ordered most-useful-first, because the bar drops the overflow.
    std::vector<ShortcutHelpBar::Entry> entries;
    const auto mods = juce::ModifierKeys::getCurrentModifiers();

    // Over the tempo lane nothing the tools offer applies, so it gets the bar
    // to itself: none of these gestures exist anywhere else.
    if (hoverMarkerLane)
    {
        entries.push_back ({ "drag", "move tempo / time sig" });
        entries.push_back ({ "double click", "edit, or add a tempo change" });
        entries.push_back ({ "right click", "add or remove" });
        entries.push_back ({ "Alt+click", "remove" });
    }
    else if (hoverLaneRow >= 0)
    {
        // Same treatment for an automation lane: its gestures exist nowhere
        // else, so the bar lists them alone while the pointer is over one.
        entries.push_back ({ "click", "add automation point" });
        entries.push_back ({ "drag", "move point" });
        entries.push_back ({ "Alt+click", "delete point" });
        entries.push_back ({ "double click", "add point on the line" });
    }
    else if (mods.isAltDown())
    {
        // Alt takes the drag over from whichever tool is active, so leading
        // with the tool's own drag here would be a lie.
        entries.push_back ({ "drag", "erase clips" });
    }
    else if (tool == Tool::paint)
    {
        entries.push_back ({ "drag", "paint pattern" });
        entries.push_back ({ "drag clip", "move" });
        entries.push_back ({ "E", "select tool" });
    }
    else
    {
        entries.push_back ({ "drag", "rubber-band select" });
        entries.push_back ({ "drag clip", "move" });
        entries.push_back ({ "B", "paint tool" });
    }

    if (! selectedClips.empty())
    {
        entries.push_back ({ "Cmd+D", "duplicate" });
        entries.push_back ({ "Delete", "remove" });
        entries.push_back ({ "Cmd+C / Cmd+X", "copy / cut" });
    }

    // Paste is worth listing whenever there is something to paste, selection or
    // not -- it is the half of the clipboard that works on an empty grid.
    if (readClipboardPayload().isValid())
        entries.push_back ({ "Cmd+V", "paste at pointer" });

    entries.push_back ({ "drop audio", "place on a track" });

    entries.push_back ({ "Cmd+click", "extend selection" });
    entries.push_back ({ "double click", "edit pattern" });

    if (! mods.isAltDown())
        entries.push_back ({ "Alt+drag", "erase" });

    entries.push_back ({ "Cmd+scroll", "zoom" });

    // Without a host there is nothing to remember: caching now would swallow
    // the first real call, which comes once the host has wired itself up.
    if (! onShortcutHelpChanged || entries == notifiedShortcuts)
        return;

    notifiedShortcuts = entries;
    onShortcutHelpChanged (std::move (entries));
}

//==============================================================================
// Painting

const PlaylistComponent::WaveformPeaks& PlaylistComponent::peaksFor (const juce::File& file)
{
    const auto key = file.getFullPathName();

    if (const auto existing = peakCache.find (key); existing != peakCache.end())
        return existing->second;

    // Inserted before the read, and left empty if it fails: a file that has
    // gone missing must not be re-opened on every repaint.
    auto& peaks = peakCache[key];

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return peaks;

    peaks.lengthSeconds = (double) reader->lengthInSamples / reader->sampleRate;
    peaks.minima.resize ((size_t) waveformBuckets);
    peaks.maxima.resize ((size_t) waveformBuckets);

    // A fixed number of buckets over the whole file, rather than one per pixel:
    // the same peaks then serve every placement of the file at every zoom, and
    // the file is read exactly once.
    for (int bucket = 0; bucket < waveformBuckets; ++bucket)
    {
        const auto from = reader->lengthInSamples * bucket / waveformBuckets;
        const auto to = reader->lengthInSamples * (bucket + 1) / waveformBuckets;

        juce::Range<float> range;
        reader->readMaxLevels (from, std::max ((juce::int64) 1, to - from), &range, 1);

        peaks.minima[(size_t) bucket] = range.getStart();
        peaks.maxima[(size_t) bucket] = range.getEnd();
    }

    return peaks;
}

void PlaylistComponent::paintAudioClip (juce::Graphics& g, const model::AudioClip& clip,
                                        juce::Rectangle<float> r, juce::Colour colour, bool selected)
{
    g.setColour (selected ? colour.brighter (0.4f) : colour);
    g.fillRoundedRectangle (r, 3.0f);

    const auto& peaks = peaksFor (clip.getFile());

    if (! peaks.minima.empty() && peaks.lengthSeconds > 0.0 && r.getWidth() >= 2.0f)
    {
        // Which slice of the source this placement shows. Past the end of the
        // file there is nothing to draw and the clip plays silence, so the
        // waveform simply stops -- which is also how an over-long clip reads.
        const auto fromSeconds = clip.getOffsetSeconds();
        const auto toSeconds = fromSeconds + clip.getLengthSeconds();
        const auto buckets = (double) peaks.minima.size();
        const auto centreY = r.getCentreY();
        const auto halfHeight = r.getHeight() * 0.5f - 3.0f;
        const auto width = (int) r.getWidth();

        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (juce::Colours::black.withAlpha (0.45f));

        for (int x = 0; x < width; ++x)
        {
            const auto seconds = juce::jmap ((double) x, 0.0, (double) width, fromSeconds, toSeconds);
            const auto bucket = (int) (seconds / peaks.lengthSeconds * buckets);

            if (bucket < 0 || bucket >= (int) peaks.minima.size())
                continue;

            const auto top = centreY - peaks.maxima[(size_t) bucket] * halfHeight;
            const auto bottom = centreY - peaks.minima[(size_t) bucket] * halfHeight;
            g.fillRect (r.getX() + (float) x, top, 1.0f, std::max (1.0f, bottom - top));
        }
    }

    g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r, 3.0f, selected ? 1.6f : 1.0f);

    // The grip that trims the placement, drawn so it is findable without
    // hunting for the edge.
    const auto grip = trimHandleBounds (r);

    if (! grip.isEmpty())
    {
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRect (grip.reduced (1.0f, 4.0f));
    }

    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.setFont (11.0f);
    g.drawText (clip.getName(),
                r.withTrimmedRight (trimHandleWidth).reduced (4.0f, 0.0f).toNearestInt(),
                juce::Justification::centredLeft);
}

void PlaylistComponent::paintAutomationLane (juce::Graphics& g, int row,
                                            const model::Generator& generator, juce::Colour rowColour)
{
    const auto lane = automationLaneBounds (row);
    if (lane.isEmpty())
        return;

    // A tint rather than an opaque fill, so the bar lines painted under it
    // still show through: they are what the points snap near.
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (lane.withTrimmedLeft ((float) labelWidth));
    g.setColour (juce::Colour (0xff2f2f36));
    g.drawHorizontalLine ((int) lane.getY(), 0.0f, (float) getWidth());

    // The lane's label cell holds the parameter selector; opening it is the
    // only gesture the cell offers.
    g.setColour (juce::Colour (0xff26262c));
    g.fillRect (0.0f, lane.getY() + 1.0f, (float) labelWidth - 2.0f, lane.getHeight() - 1.0f);

    const auto info = displayedParamFor (generator.getId());
    const auto selector = laneSelectorBounds (row);

    g.setColour (juce::Colour (0xff35353d));
    g.fillRoundedRectangle (selector, 3.0f);
    g.setColour (juce::Colour (0xffb8b8c0));
    g.setFont (11.0f);
    g.drawText (info.label, selector.reduced (6.0f, 0.0f).withTrimmedRight (10.0f).toNearestInt(),
                juce::Justification::centredLeft);

    juce::Path chevron;
    const auto c = juce::Point<float> (selector.getRight() - 9.0f, selector.getCentreY());
    chevron.startNewSubPath (c.x - 3.0f, c.y - 1.5f);
    chevron.lineTo (c.x, c.y + 2.0f);
    chevron.lineTo (c.x + 3.0f, c.y - 1.5f);
    g.setColour (juce::Colour (0xff9a9aa4));
    g.strokePath (chevron, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

    // The curve, clipped to its own band so a point dragged to beat 0 does not
    // paint into the labels.
    const auto curveArea = automationCurveBounds (row);
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (curveArea.toNearestInt());

    const auto curveColour = rowColour.brighter (0.2f);
    auto maybeLane = displayedLaneFor (generator);
    const auto points = maybeLane ? maybeLane->getPoints() : std::vector<model::AutomationPoint>();

    if (points.empty())
    {
        // Where the parameter sits until a point says otherwise, plus a hint:
        // an empty strip would not say it is editable.
        const auto y = valueToLaneY (info.defaultValue, info, curveArea);
        g.setColour (curveColour.withAlpha (0.25f));
        g.drawHorizontalLine ((int) y, curveArea.getX(), curveArea.getRight());

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.setFont (10.0f);
        g.drawText ("click to add a point", curveArea.toNearestInt().reduced (8, 0),
                    juce::Justification::centredLeft);
        return;
    }

    // Straight segments -- the model's bend value is ignored for drawing this
    // pass -- flat to the edges beyond the first and last points, which is
    // also how the engine plays the curve.
    juce::Path path;
    path.startNewSubPath (curveArea.getX(),
                          valueToLaneY (points.front().getValue(), info, curveArea));

    for (const auto& point : points)
        path.lineTo (beatToX (point.getBeat()), valueToLaneY (point.getValue(), info, curveArea));

    path.lineTo (curveArea.getRight(), valueToLaneY (points.back().getValue(), info, curveArea));

    g.setColour (curveColour.withAlpha (0.9f));
    g.strokePath (path, juce::PathStrokeType (1.6f));

    for (const auto& point : points)
    {
        const auto x = beatToX (point.getBeat());
        const auto y = valueToLaneY (point.getValue(), info, curveArea);
        const bool dragging = dragAutoPoint.isValid() && dragAutoPoint == point.state;

        g.setColour (dragging ? juce::Colours::white : curveColour);
        g.fillEllipse (x - autoPointRadius, y - autoPointRadius,
                       autoPointRadius * 2.0f, autoPointRadius * 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (x - autoPointRadius, y - autoPointRadius,
                       autoPointRadius * 2.0f, autoPointRadius * 2.0f, 1.0f);
    }
}

std::vector<PlaylistComponent::Marker> PlaylistComponent::markers() const
{
    const auto lane = markerLaneBounds();
    const auto font = markerFont();
    std::vector<Marker> result;

    // A marker's left edge sits on its beat and its label runs to the right of
    // it, so the thing that marks the position is the edge the eye lines up
    // with the bar line under it.
    auto add = [&] (const juce::ValueTree& state, double beat, const juce::String& text)
    {
        const auto width = juce::GlyphArrangement::getStringWidth (font, text) + 10.0f;
        result.push_back ({ state, beat, text,
                            { beatToX (beat), lane.getY() + 2.0f, width, lane.getHeight() - 4.0f } });
    };

    for (const auto& change : song.getTempoChanges())
        add (change.state, change.getStartBeat(), formatBpm (change.getBpm()));

    for (const auto& change : song.getTimeSigChanges())
        add (change.state, change.getStartBeat(), formatTimeSig (change.getSignature()));

    return result;
}

std::optional<PlaylistComponent::Marker> PlaylistComponent::markerAt (juce::Point<float> position) const
{
    const auto all = markers();

    // In reverse, so whichever marker is drawn on top of an overlapping pair is
    // also the one that gets clicked.
    for (auto i = all.rbegin(); i != all.rend(); ++i)
        if (i->bounds.contains (position))
            return *i;

    return std::nullopt;
}

void PlaylistComponent::paintMarkerLane (juce::Graphics& g)
{
    const auto lane = markerLaneBounds();

    g.setColour (juce::Colour (0xff1e1e23));
    g.fillRect (lane);
    g.setColour (juce::Colour (0xff2f2f36));
    g.drawHorizontalLine ((int) lane.getBottom() - 1, 0.0f, (float) getWidth());

    // The label cell says what the lane is: nothing else here names it, and an
    // empty strip would otherwise read as padding.
    g.setFont (9.0f);
    g.setColour (juce::Colour (0xff6a6a74));
    g.drawText ("TEMPO / SIG", 8, (int) lane.getY(), labelWidth - 12, (int) lane.getHeight(),
                juce::Justification::centredLeft);

    // What the song opens with, dim and untouchable, so the lane says what it
    // holds in a song that never changes either. Dropped as soon as a change
    // of either kind sits on beat 0, which is where this would be drawn.
    if (! hasChangeAt (true, 0.0) && ! hasChangeAt (false, 0.0))
    {
        const auto opening = formatBpm (song.getTempo()) + " " + formatTimeSig (song.getTimeSigAt (0.0));

        g.setFont (markerFont());
        g.setColour (juce::Colour (0xff5c5c66));
        g.drawText (opening, (int) beatToX (0.0) + 4, (int) lane.getY(), 90, (int) lane.getHeight(),
                    juce::Justification::centredLeft);
    }

    for (const auto& marker : markers())
    {
        const auto colour = juce::Colour (marker.isTempo() ? tempoColour : timeSigColour);
        const bool dragging = dragMarker.isValid() && dragMarker == marker.state;

        g.setColour (colour.withAlpha (dragging ? 0.95f : 0.75f));
        g.fillRoundedRectangle (marker.bounds, 2.0f);

        // The tick is the marker's actual position; the block behind the label
        // only hangs off it.
        g.fillRect (marker.bounds.getX() - 1.0f, lane.getY(), 2.0f, lane.getHeight());

        g.setFont (markerFont());
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawText (marker.text, marker.bounds.reduced (4.0f, 0.0f).toNearestInt(),
                    juce::Justification::centredLeft);
    }
}

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
    // colliding at this zoom. Measured against the shortest bar in the song, so
    // the step that clears a 3/4 bar clears every other one too.
    const auto narrowestBarWidth = shortestBar() * pixelsPerBeat;
    int labelStep = 1;
    while ((double) labelStep * narrowestBarWidth < 46.0)
        labelStep *= 2;

    g.setFont (10.0f);

    forEachBar (getContentLengthBeats(), [&] (int bar, double startBeat, double lengthBeats)
    {
        if (bar % labelStep != 0)
            return;

        const auto x = beatToX (startBeat);
        g.setColour (juce::Colour (0xff4a4a52));
        g.drawVerticalLine ((int) x, ruler.getY() + 6.0f, ruler.getBottom());
        g.setColour (juce::Colour (0xff9a9aa4));

        // Bars are 0-based in the model and 1-based on screen, like every
        // other DAW.
        g.drawText (juce::String (bar + 1), (int) x + 3, (int) ruler.getY(),
                    juce::roundToInt ((double) labelStep * lengthBeats * pixelsPerBeat), rulerHeight,
                    juce::Justification::centredLeft);
    });

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
    // never close up into a solid block. Thinned against the shortest bar in
    // the song, for the same reason the ruler's labels are.
    const bool drawBeats = pixelsPerBeat >= 7.0;
    int barStep = 1;
    while ((double) barStep * shortestBar() * pixelsPerBeat < 9.0)
        barStep *= 2;

    forEachBar (lengthBeats, [&] (int bar, double barStart, double barLength)
    {
        if (bar % barStep == 0)
        {
            g.setColour (juce::Colour (0xff4a4a52));
            g.drawVerticalLine ((int) beatToX (barStart), gridTop, (float) getHeight());
        }

        if (drawBeats)
        {
            g.setColour (juce::Colour (0xff2c2c31));
            for (double beat = 1.0; beat < barLength - beatTolerance; beat += 1.0)
                g.drawVerticalLine ((int) beatToX (barStart + beat), gridTop, (float) getHeight());
        }
    });

    // Where the song changes tempo or meter, drawn the full height of the grid
    // so the change reads as applying to everything from there on rather than
    // being a note in the header band.
    for (const auto& marker : markers())
    {
        g.setColour (juce::Colour (marker.isTempo() ? tempoColour : timeSigColour).withAlpha (0.25f));
        g.drawVerticalLine ((int) beatToX (marker.beat), gridTop, (float) getHeight());
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
        // Trimmed on the right so a long name never runs under the disclosure.
        g.drawText (generator.getName(), 8, (int) y, labelWidth - 30, rowHeight,
                    juce::Justification::centredLeft);

        // Disclosure for the row's automation lane: a chevron pointing right
        // when closed and down at the lane when open.
        {
            const auto d = disclosureBounds (row).getCentre();
            juce::Path tri;

            if (isRowExpanded (row))
            {
                tri.startNewSubPath (d.x - 3.5f, d.y - 2.0f);
                tri.lineTo (d.x, d.y + 2.0f);
                tri.lineTo (d.x + 3.5f, d.y - 2.0f);
            }
            else
            {
                tri.startNewSubPath (d.x - 2.0f, d.y - 3.5f);
                tri.lineTo (d.x + 2.0f, d.y);
                tri.lineTo (d.x - 2.0f, d.y + 3.5f);
            }

            g.setColour (juce::Colour (0xff8a8a94));
            g.strokePath (tri, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        // clips
        const auto rowColour = juce::Colour::fromHSV (0.08f + 0.13f * (float) row, 0.55f, 0.75f, 1.0f);
        for (const auto& placement : placementsFor (generator))
        {
            const auto r = slotBounds (row, placement.start, placement.length);
            const bool selected = isSelected (placement.state);

            // An audio placement is a waveform with a trim grip, not a named
            // block with a pattern chevron, so it is drawn on its own.
            if (placement.isAudio())
            {
                paintAudioClip (g, model::AudioClip (placement.state), r, rowColour, selected);
                continue;
            }

            const model::PlaylistClip clip (placement.state);
            auto pattern = generator.findPattern (clip.getPatternId());
            if (! pattern)
                continue;

            const auto patternLength = pattern->getLengthBeats();
            const auto clipLength = placement.length;

            g.setColour (selected ? rowColour.brighter (0.4f) : rowColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 3.0f, selected ? 1.6f : 1.0f);

            // A clip longer than its pattern loops it, so mark where each
            // repeat starts: without this it reads as one long pattern.
            // Skipped when the repeats are too close together to tell apart.
            const auto repeatWidth = (float) (patternLength * pixelsPerBeat);

            if (patternLength > 0.0 && clipLength > patternLength + beatTolerance
                    && repeatWidth >= 4.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                for (auto x = r.getX() + repeatWidth; x < r.getRight() - 1.0f; x += repeatWidth)
                    g.fillRect (x, r.getY() + 2.0f, 1.0f, r.getHeight() - 4.0f);
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
        }

        if (isRowExpanded (row))
            paintAutomationLane (g, row, generator, rowColour);
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

    // Where a file drag would land. An audio row takes the files outright;
    // anywhere else says so, because the drop makes a generator first.
    if (fileDropTarget)
    {
        const auto r = slotBounds (fileDropTarget->ghostRow, fileDropTarget->startBeats,
                                   barLengthAt (fileDropTarget->startBeats));

        g.setColour (juce::Colour (0xffe08a3c).withAlpha (0.20f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colour (0xffe08a3c));
        g.drawRoundedRectangle (r, 3.0f, 1.4f);

        if (fileDropTarget->row < 0)
        {
            g.setFont (11.0f);
            g.drawText ("New audio track", r.reduced (4.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft);
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

    // The value under the pointer while an automation point is dragged: the
    // readout is the only place the actual number shows, the curve itself is
    // only a shape.
    if (dragMode == DragMode::autoPoint && dragAutoPoint.isValid())
    {
        const model::AutomationPoint point (dragAutoPoint);
        const auto curveArea = automationCurveBounds (dragAutoRow);

        if (! curveArea.isEmpty())
        {
            const auto x = beatToX (point.getBeat());
            const auto y = valueToLaneY (point.getValue(), dragAutoInfo, curveArea);
            const auto text = dragAutoInfo.label + ": " + formatAutoValue (point.getValue());
            const auto textWidth = juce::GlyphArrangement::getStringWidth (
                                       juce::Font (juce::FontOptions (10.0f)), text) + 10.0f;

            auto box = juce::Rectangle<float> (x + 8.0f, y - 22.0f, textWidth, 15.0f);

            // Keep it readable when the point sits at the very top or right.
            if (box.getRight() > (float) getWidth())
                box.setX (x - 8.0f - textWidth);
            if (box.getY() < curveArea.getY())
                box.setY (y + 8.0f);

            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (10.0f);
            g.drawText (text, box.toNearestInt(), juce::Justification::centred);
        }
    }

    // Header band last and opaque: it is pinned to the visible area, so it
    // covers whatever grid has scrolled underneath it.
    const auto header = headerBounds();
    g.setColour (juce::Colour (0xff1b1b1f));
    g.fillRect (toolbarBounds());
    paintMarkerLane (g);
    paintRuler (g);
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine ((int) header.getBottom() - 1, 0.0f, (float) getWidth());

    const auto statusX = zoomInButton.getRight() + 10;
    const auto statusArea = juce::Rectangle<int> (statusX, (int) header.getY(),
                                                  juce::jmax (0, getWidth() - statusX - 6), toolbarHeight);
    g.setFont (11.0f);

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
    auto placement = placementAt (generator, beat);
    if (! placement)
        return false;

    removePlacement (placement->state);
    return true;
}

void PlaylistComponent::removePlacement (const juce::ValueTree& state)
{
    auto playlist = song.getPlaylist();

    if (state.hasType (model::ids::AUDIOCLIP))
        playlist.removeAudioClip (model::AudioClip (state), &undoManager);
    else
        playlist.removeClip (model::PlaylistClip (state), &undoManager);
}

void PlaylistComponent::setPlacementStart (const juce::ValueTree& state, double startBeats)
{
    if (state.hasType (model::ids::AUDIOCLIP))
        model::AudioClip (state).setStart (startBeats, &undoManager);
    else
        model::PlaylistClip (state).setStart (startBeats, &undoManager);
}

void PlaylistComponent::trimSelectionTo (double targetEnd)
{
    if (! trimClip.isValid())
        return;

    model::AudioClip clip (trimClip);
    const auto length = std::max (0.0, targetEnd - clip.getStart());

    if (std::abs (clip.getStart() + length - trimLastEnd) < beatTolerance)
        return;   // still inside the beat we last wrote: no model write, no resync

    auto generator = song.findGenerator (clip.getGeneratorId());

    // Growing a clip over its neighbour would hide it, so refuse rather than
    // overlap -- the same rule painting follows.
    if (! generator || ! isRangeFree (*generator, clip.getStart(), length, false, trimClip))
        return;

    // The grid trims in beats but the clip is measured in seconds, so convert
    // through the tempo the clip actually plays through.
    clip.setLengthSeconds (song.secondsFromBeats (clip.getStart() + length)
                               - song.secondsFromBeats (clip.getStart()),
                           &undoManager);
    trimLastEnd = clip.getStart() + length;
}

void PlaylistComponent::dragSelectionTo (double targetStart)
{
    if (selectedClips.empty() || selectedClips.size() != dragOriginStarts.size()
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
        auto placement = placementFor (clips[i]);
        auto generator = song.findGenerator (clips[i][model::ids::generatorId].toString());

        if (! placement || ! generator)
            return;

        if (! isRangeFree (*generator, dragOriginStarts[i] + delta, placement->length, true))
            return;
    }

    for (size_t i = 0; i < clips.size(); ++i)
        setPlacementStart (clips[i], dragOriginStarts[i] + delta);

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
        if (auto placement = placementFor (state))
        {
            spanStart = std::min (spanStart, placement->start);
            spanEnd = std::max (spanEnd, placement->getEnd());
        }
    }
    if (spanEnd <= spanStart)
        return;

    // A whole number of bars, and at least one, measured from the bar the
    // selection starts in -- so the copies land on the grid however the meter
    // changes across them.
    const auto from = snapToBar (spanStart);
    auto to = nextBarAfter (from);

    while (to < spanEnd - beatTolerance)
        to = nextBarAfter (to);

    const auto offset = to - from;

    undoManager.beginNewTransaction();

    auto playlist = song.getPlaylist();
    std::vector<juce::ValueTree> copies;

    for (const auto& state : clips)
    {
        auto placement = placementFor (state);
        auto generator = song.findGenerator (state[model::ids::generatorId].toString());

        if (! placement || ! generator)
            continue;

        const auto length = placement->length;
        const auto start = placement->start + offset;

        if (! isRangeFree (*generator, start, length))
            continue;

        if (placement->isAudio())
        {
            // The same slice of the same file: length and offset are the whole
            // of what an audio placement says beyond where it sits.
            const model::AudioClip source (state);
            auto copy = playlist.addAudioClip (*generator, source.getFile(), start,
                                               source.getLengthSeconds(), &undoManager);

            if (source.getOffsetSeconds() > 0.0)
                copy.setOffsetSeconds (source.getOffsetSeconds(), &undoManager);

            copies.push_back (copy.state);
            continue;
        }

        model::PlaylistClip clip (state);
        auto pattern = generator->findPattern (clip.getPatternId());
        if (! pattern)
            continue;

        auto copy = playlist.addClip (*generator, *pattern, start, &undoManager);

        // A copy has to play the same thing for the same time: a new clip is
        // pattern-length and untransposed, so carry both over when they differ.
        if (clip.hasOwnLength())
            copy.setLength (length, &undoManager);
        if (clip.getTranspose() != 0)
            copy.setTranspose (clip.getTranspose(), &undoManager);

        copies.push_back (copy.state);
    }

    // Select the copies so ⌘D again continues the chain.
    if (! copies.empty())
    {
        selectedClips = std::move (copies);
        selectionChanged();
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
    for (const auto& state : clips)
        removePlacement (state);

    clearSelection();
}

//==============================================================================
// Clipboard

juce::ValueTree PlaylistComponent::makeClipboardPayload() const
{
    // Everything is measured from the earliest clip in the selection, so the
    // payload holds the shape of the group and nothing about where it was.
    double anchor = std::numeric_limits<double>::max();

    for (const auto& state : selectedClips)
        if (auto placement = placementFor (state))
            anchor = std::min (anchor, placement->start);

    if (anchor == std::numeric_limits<double>::max())
        return {};

    juce::ValueTree payload (clipboardType);
    payload.setProperty (clipboardVersion, clipboardFormatVersion, nullptr);

    for (const auto& state : selectedClips)
    {
        auto placement = placementFor (state);
        auto generator = song.findGenerator (state[model::ids::generatorId].toString());

        if (! placement || ! generator)
            continue;

        // A copy of the node itself rather than a hand-written list of
        // properties: a CLIP that grows another one later travels without
        // anyone having to remember this.
        auto entry = state.createCopy();
        entry.setProperty (model::ids::start, placement->start - anchor, nullptr);

        // An audio placement's id names *this* placement -- EditSync matches a
        // live wave clip by it -- so a copy must not carry it. Pasting mints a
        // fresh one.
        entry.removeProperty (model::ids::id, nullptr);

        // The relative path is worked out against whichever .carve the song is
        // saved to, so it means nothing outside the document it came from; the
        // absolute one in `file` is what a paste resolves.
        entry.removeProperty (model::ids::relPath, nullptr);

        entry.setProperty (clipboardGeneratorName, generator->getName(), nullptr);

        if (entry.hasType (model::ids::CLIP))
            if (auto pattern = generator->findPattern (model::PlaylistClip (state).getPatternId()))
                entry.setProperty (clipboardPatternName, pattern->getName(), nullptr);

        payload.appendChild (entry, nullptr);
    }

    return payload.getNumChildren() > 0 ? payload : juce::ValueTree();
}

juce::ValueTree PlaylistComponent::readClipboardPayload()
{
    const auto text = juce::SystemClipboard::getTextFromClipboard();

    // Anything at all can be on the clipboard, so every step here has to be
    // able to say "not ours" rather than assume: no XML, the wrong root, or a
    // version written by something newer all read as nothing.
    if (! text.trimStart().startsWith ("<"))
        return {};

    auto xml = juce::parseXML (text);

    if (xml == nullptr || ! xml->hasTagName (clipboardType.toString()))
        return {};

    auto payload = juce::ValueTree::fromXml (*xml);

    if (! payload.isValid() || (int) payload.getProperty (clipboardVersion, 0) != clipboardFormatVersion)
        return {};

    return payload;
}

double PlaylistComponent::pasteTargetBeat() const
{
    // The pointer when there is one over a row -- pasting where you are
    // looking is what every grid does -- and the playhead otherwise, which is
    // what a paste driven from the keyboard alone can only mean.
    if (mouseIsOver && hoverRow >= 0)
        return hoverStartBeats;

    return snapToBar (playheadBeats);
}

std::optional<model::Generator> PlaylistComponent::generatorForClipboardEntry (const juce::ValueTree& entry) const
{
    if (auto generator = song.findGenerator (entry[model::ids::generatorId].toString()))
        return generator;

    // Only reached by a paste into a different document, where the ids are all
    // someone else's. A name is the one thing the user would recognise, so it
    // is what the fallback matches on -- and if nothing matches, the clip is
    // dropped rather than guessed at.
    const auto name = entry[clipboardGeneratorName].toString();

    if (name.isNotEmpty())
        for (const auto& generator : song.getGenerators())
            if (generator.getName() == name)
                return generator;

    return std::nullopt;
}

void PlaylistComponent::copySelection()
{
    if (auto payload = makeClipboardPayload(); payload.isValid())
        juce::SystemClipboard::copyTextToClipboard (payload.toXmlString());
}

void PlaylistComponent::cutSelection()
{
    if (selectedClips.empty())
        return;

    copySelection();
    deleteSelection();   // one transaction of its own, so undo puts the clips back
}

void PlaylistComponent::pasteClips()
{
    const auto payload = readClipboardPayload();

    if (! payload.isValid())
        return;

    const auto target = pasteTargetBeat();

    undoManager.beginNewTransaction();

    auto playlist = song.getPlaylist();
    std::vector<juce::ValueTree> pasted;

    for (const auto& entry : payload)
    {
        auto generator = generatorForClipboardEntry (entry);

        if (! generator)
            continue;

        const auto start = std::max (0.0, target + (double) entry[model::ids::start]);

        if (entry.hasType (model::ids::AUDIOCLIP))
        {
            const model::AudioClip source (entry);
            const auto file = source.getFile();

            if (! file.existsAsFile())
                continue;   // the copy was made somewhere this song cannot reach

            // Seconds are what an audio placement is measured in, but the grid
            // it must not overlap on is in beats, so the length is converted
            // at the beat it would actually land on.
            const auto length = song.beatsFromSeconds (song.secondsFromBeats (start)
                                                           + source.getLengthSeconds()) - start;

            // Never stack: the same rule painting, dragging and duplicating
            // all follow, so a paste onto an occupied bar drops that clip
            // rather than burying what is already there.
            if (! isRangeFree (*generator, start, length))
                continue;

            auto copy = playlist.addAudioClip (*generator, file, start,
                                               source.getLengthSeconds(), &undoManager);

            if (source.getOffsetSeconds() > 0.0)
                copy.setOffsetSeconds (source.getOffsetSeconds(), &undoManager);

            pasted.push_back (copy.state);
            continue;
        }

        const model::PlaylistClip source (entry);
        auto pattern = generator->findPattern (source.getPatternId());

        // Same fallback as the generator's, and for the same reason: a paste
        // into another document knows the pattern only by what it was called.
        if (! pattern)
            if (const auto name = entry[clipboardPatternName].toString(); name.isNotEmpty())
                for (const auto& candidate : generator->getPatterns())
                    if (candidate.getName() == name)
                    {
                        pattern = candidate;
                        break;
                    }

        if (! pattern)
            continue;

        const auto length = source.getLength (pattern->getLengthBeats());

        if (! isRangeFree (*generator, start, length))
            continue;

        auto copy = playlist.addClip (*generator, *pattern, start, &undoManager);

        // A fresh clip is pattern-length and untransposed, so both only have to
        // be written when the original said otherwise -- which keeps a pasted
        // clip's XML identical to the one it was copied from.
        if (source.hasOwnLength())
            copy.setLength (length, &undoManager);
        if (source.getTranspose() != 0)
            copy.setTranspose (source.getTranspose(), &undoManager);

        pasted.push_back (copy.state);
    }

    // Select what landed: it is what the user will want to move, and it makes
    // clear which of the copied clips actually fitted.
    if (! pasted.empty())
    {
        selectedClips = std::move (pasted);
        selectionChanged();
    }
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

        // A longer pattern still has to fit; refuse rather than overlap. A clip
        // with its own length keeps it, so only then does the pattern's matter.
        if (! safeThis->isRangeFree (*owner, target.getStart(),
                                     target.getLength (chosen->getLengthBeats()),
                                     false, clipState))
            return;

        safeThis->undoManager.beginNewTransaction();
        target.setPatternId (chosen->getId(), &safeThis->undoManager);
    });
}

void PlaylistComponent::dragLoopTo (double beat)
{
    // Loop edges land on the nearest bar, not the one before, so the range
    // follows the pointer in both directions.
    const auto snapped = nearestBar (beat);
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

//==============================================================================
// The tempo lane
//
// A tempo change may sit on any beat -- a fill that speeds up does not care
// where the bar line is -- but a time signature change may only sit on a bar
// line, because a bar that changes meter part way through is not a bar. The
// model deliberately enforces neither, so this is where both rules live.

bool PlaylistComponent::hasChangeAt (bool tempo, double beat, const juce::ValueTree& ignore) const
{
    auto sitsOn = [&] (const juce::ValueTree& state, double at)
    {
        return state != ignore && std::abs (at - beat) < beatTolerance;
    };

    if (tempo)
    {
        for (const auto& change : song.getTempoChanges())
            if (sitsOn (change.state, change.getStartBeat()))
                return true;

        return false;
    }

    for (const auto& change : song.getTimeSigChanges())
        if (sitsOn (change.state, change.getStartBeat()))
            return true;

    return false;
}

void PlaylistComponent::addTempoChangeAt (double beat, double bpm)
{
    beat = std::max (0.0, beat);

    // Two changes of a kind on one beat would fight over which of them wins,
    // so the second is simply not made; the first is still there to edit.
    if (hasChangeAt (true, beat))
        return;

    undoManager.beginNewTransaction();
    song.addTempoChange (beat, bpm, &undoManager);
}

void PlaylistComponent::addTimeSigChangeAt (double beat, model::TimeSignature signature)
{
    beat = nearestBar (std::max (0.0, beat));

    if (hasChangeAt (false, beat))
        return;

    undoManager.beginNewTransaction();
    song.addTimeSigChange (beat, signature, &undoManager);
}

void PlaylistComponent::removeMarker (const juce::ValueTree& state)
{
    undoManager.beginNewTransaction();

    if (state.hasType (model::ids::TEMPO))
        song.removeTempoChange (model::TempoChange (state), &undoManager);
    else
        song.removeTimeSigChange (model::TimeSigChange (state), &undoManager);
}

void PlaylistComponent::dragMarkerTo (double beat)
{
    if (! dragMarker.isValid())
        return;

    const bool tempo = dragMarker.hasType (model::ids::TEMPO);

    // A tempo change lands on the nearest beat, the same coarseness an audio
    // trim uses: fine enough to put one anywhere the grid means anything, and
    // coarse enough that a drag writes once per beat rather than once per
    // pixel. A time signature change lands on the nearest bar line of the song
    // as it would be without this change in it -- the bars it defines itself
    // travel with it, so measuring against those would let it end up between
    // the bar lines everything else is drawn on.
    const auto target = tempo ? std::max (0.0, std::round (beat))
                              : nearestBar (std::max (0.0, beat), dragMarker);

    if (std::abs (target - markerLastBeat) < beatTolerance || hasChangeAt (tempo, target, dragMarker))
        return;   // nothing to write, or another change already owns that beat

    markerLastBeat = target;

    // Held by value: the write calls back into refresh() synchronously.
    const auto state = dragMarker;

    if (tempo)
        model::TempoChange (state).setStartBeat (target, &undoManager);
    else
        model::TimeSigChange (state).setStartBeat (target, &undoManager);
}

void PlaylistComponent::editTempoValue (juce::ValueTree existing, double beatForNew)
{
    const auto beat = existing.isValid() ? model::TempoChange (existing).getStartBeat() : beatForNew;
    const auto current = existing.isValid() ? model::TempoChange (existing).getBpm()
                                            : song.getTempoAt (beat);
    const auto bars = song.toBarsAndBeats (beat);

    auto* window = new juce::AlertWindow ("Tempo change",
                                          "Tempo in BPM from bar " + juce::String (bars.bar + 1),
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("bpm", formatBpm (current), "BPM");
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // The dialog is asynchronous, so nothing captured here may be dereferenced
    // without checking that we - and the change - are still around. The window
    // itself outlives the callback: it is deleted after the callbacks run.
    juce::Component::SafePointer<PlaylistComponent> safeThis (this);

    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis, window, existing, beat] (int result)
        {
            if (safeThis == nullptr || result == 0)
                return;

            const auto bpm = window->getTextEditorContents ("bpm").getDoubleValue();

            if (bpm <= 0.0)
                return;   // not a number, or nonsense: leave the song alone

            if (existing.isValid())
            {
                // It may have been dragged, undone or deleted while the dialog
                // was up, so a change that has left the song writes nothing
                // rather than being resurrected.
                if (existing.isAChildOf (safeThis->song.state))
                {
                    safeThis->undoManager.beginNewTransaction();
                    model::TempoChange (existing).setBpm (bpm, &safeThis->undoManager);
                }

                return;
            }

            safeThis->addTempoChangeAt (beat, bpm);
        }), true);
}

void PlaylistComponent::chooseTimeSigValue (juce::ValueTree existing, double beatForNew,
                                            juce::Rectangle<float> targetArea)
{
    const auto beat = existing.isValid() ? model::TimeSigChange (existing).getStartBeat() : beatForNew;
    const auto current = existing.isValid() ? model::TimeSigChange (existing).getSignature()
                                            : song.getTimeSigAt (beat);

    juce::PopupMenu menu;
    for (int i = 0; i < (int) std::size (commonTimeSignatures); ++i)
        menu.addItem (i + 1, formatTimeSig (commonTimeSignatures[i]), true,
                      commonTimeSignatures[i] == current);

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (targetArea.toNearestInt()));

    juce::Component::SafePointer<PlaylistComponent> safeThis (this);

    menu.showMenuAsync (options, [safeThis, existing, beat] (int result)
    {
        if (safeThis == nullptr || result <= 0 || result > (int) std::size (commonTimeSignatures))
            return;

        const auto chosen = commonTimeSignatures[result - 1];

        if (! existing.isValid())
        {
            safeThis->addTimeSigChangeAt (beat, chosen);
            return;
        }

        if (! existing.isAChildOf (safeThis->song.state))
            return;

        safeThis->undoManager.beginNewTransaction();
        model::TimeSigChange (existing).setSignature (chosen, &safeThis->undoManager);
    });
}

void PlaylistComponent::showMarkerMenu (const Marker& marker)
{
    juce::PopupMenu menu;
    menu.addItem (1, marker.isTempo() ? "Edit tempo..." : "Change signature...");
    menu.addItem (2, "Remove");

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (marker.bounds.toNearestInt()));

    juce::Component::SafePointer<PlaylistComponent> safeThis (this);
    const auto state = marker.state;
    const auto bounds = marker.bounds;

    menu.showMenuAsync (options, [safeThis, state, bounds] (int result)
    {
        if (safeThis == nullptr || result <= 0 || ! state.isAChildOf (safeThis->song.state))
            return;

        if (result == 2)
        {
            safeThis->removeMarker (state);
            return;
        }

        if (state.hasType (model::ids::TEMPO))
            safeThis->editTempoValue (state, 0.0);
        else
            safeThis->chooseTimeSigValue (state, 0.0, bounds);
    });
}

void PlaylistComponent::showLaneMenu (double beat, juce::Point<float> position)
{
    // The two kinds land differently, so the beats are worked out here and the
    // menu says which bar each one would go on.
    const auto tempoBeat = std::max (0.0, std::round (beat));
    const auto sigBeat = nearestBar (std::max (0.0, beat));

    juce::PopupMenu signatures;
    for (int i = 0; i < (int) std::size (commonTimeSignatures); ++i)
        signatures.addItem (10 + i, formatTimeSig (commonTimeSignatures[i]));

    juce::PopupMenu menu;
    menu.addItem (1, "Add tempo change at bar " + juce::String (song.toBarsAndBeats (tempoBeat).bar + 1) + "...");
    menu.addSubMenu ("Add time signature at bar " + juce::String (song.toBarsAndBeats (sigBeat).bar + 1),
                     signatures);

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (juce::Rectangle<int> ((int) position.x,
                                                                                             (int) position.y,
                                                                                             1, 1)));

    juce::Component::SafePointer<PlaylistComponent> safeThis (this);

    menu.showMenuAsync (options, [safeThis, tempoBeat, sigBeat] (int result)
    {
        if (safeThis == nullptr || result <= 0)
            return;

        if (result == 1)
        {
            safeThis->editTempoValue ({}, tempoBeat);
            return;
        }

        const auto index = result - 10;

        if (index >= 0 && index < (int) std::size (commonTimeSignatures))
            safeThis->addTimeSigChangeAt (sigBeat, commonTimeSignatures[index]);
    });
}

//==============================================================================
// Automation lanes
//
// One lane per expanded generator row, showing one parameter's curve at a
// time; the selector in the lane's label cell switches which. Points are the
// model's own values against the parameter's own range -- the lane maps
// linearly between the two and never has to know what the number means.

bool PlaylistComponent::isGeneratorExpanded (const juce::String& generatorId) const
{
    return std::find (expandedGenerators.begin(), expandedGenerators.end(), generatorId)
               != expandedGenerators.end();
}

bool PlaylistComponent::isRowExpanded (int row) const
{
    return row >= 0 && row < song.getNumGenerators()
               && isGeneratorExpanded (song.getGenerator (row).getId());
}

void PlaylistComponent::toggleRowExpanded (int row)
{
    if (row < 0 || row >= song.getNumGenerators())
        return;

    const auto id = song.getGenerator (row).getId();

    if (isGeneratorExpanded (id))
        std::erase (expandedGenerators, id);
    else
        expandedGenerators.push_back (id);

    // Pure UI state, like the selection: expanding a lane must not dirty the
    // document or touch the undo history.
    updateSize();
    repaint();
}

juce::Rectangle<float> PlaylistComponent::disclosureBounds (int row) const
{
    // The right end of the row's label cell, where the pattern chevron sits on
    // a clip: the same corner meaning "there is more here" in both places.
    return { (float) labelWidth - 20.0f, rowY (row) + (float) rowHeight * 0.5f - 7.0f,
             14.0f, 14.0f };
}

juce::Rectangle<float> PlaylistComponent::automationLaneBounds (int row) const
{
    if (! isRowExpanded (row))
        return {};

    return { 0.0f, rowY (row) + (float) rowHeight, (float) getWidth(), (float) automationLaneHeight };
}

juce::Rectangle<float> PlaylistComponent::automationCurveBounds (int row) const
{
    return automationLaneBounds (row).withTrimmedLeft ((float) labelWidth);
}

juce::Rectangle<float> PlaylistComponent::laneSelectorBounds (int row) const
{
    const auto lane = automationLaneBounds (row);
    if (lane.isEmpty())
        return {};

    return { 6.0f, lane.getCentreY() - 9.0f, (float) labelWidth - 14.0f, 18.0f };
}

std::vector<PlaylistComponent::AutomatableParamInfo> PlaylistComponent::automatableParamsFor (const juce::String& generatorId) const
{
    if (getAutomatableParams)
        if (auto params = getAutomatableParams (generatorId); ! params.empty())
            return params;

    // No host wired up, or nothing live to describe: volume and pan exist on
    // every track no matter what it hosts. Volume is the fader position, which
    // is why its default is 0.65 rather than a dB figure.
    return { { model::AutomationLane::volumeTarget, {}, "Volume", 0.0f, 1.0f, 0.65f },
             { model::AutomationLane::panTarget, {}, "Pan", -1.0f, 1.0f, 0.0f } };
}

PlaylistComponent::AutomatableParamInfo PlaylistComponent::displayedParamFor (const juce::String& generatorId) const
{
    const auto params = automatableParamsFor (generatorId);

    if (const auto choice = laneParamChoices.find (generatorId); choice != laneParamChoices.end())
        for (const auto& info : params)
            if (info.target == choice->second.first && info.param == choice->second.second)
                return info;

    // No choice yet, or the chosen parameter's plugin has gone: the first
    // offered parameter, which the fallback list makes volume.
    return params.front();
}

std::optional<model::AutomationLane> PlaylistComponent::displayedLaneFor (const model::Generator& generator) const
{
    const auto info = displayedParamFor (generator.getId());
    return generator.findAutomationLane (info.target, info.param);
}

float PlaylistComponent::valueToLaneY (float value, const AutomatableParamInfo& info,
                                       juce::Rectangle<float> curveArea) const
{
    const auto range = std::max (1.0e-6f, info.maxValue - info.minValue);
    const auto proportion = juce::jlimit (0.0f, 1.0f, (value - info.minValue) / range);
    const auto top = curveArea.getY() + autoLaneValuePad;
    const auto bottom = curveArea.getBottom() - autoLaneValuePad;
    return bottom - proportion * (bottom - top);
}

float PlaylistComponent::laneYToValue (float y, const AutomatableParamInfo& info,
                                       juce::Rectangle<float> curveArea) const
{
    const auto top = curveArea.getY() + autoLaneValuePad;
    const auto bottom = curveArea.getBottom() - autoLaneValuePad;
    const auto proportion = juce::jlimit (0.0f, 1.0f, (bottom - y) / std::max (1.0f, bottom - top));
    return info.minValue + proportion * (info.maxValue - info.minValue);
}

std::optional<model::AutomationPoint> PlaylistComponent::autoPointAt (int row,
                                                                     juce::Point<float> position) const
{
    if (row < 0 || row >= song.getNumGenerators())
        return std::nullopt;

    auto generator = song.getGenerator (row);
    auto lane = displayedLaneFor (generator);
    if (! lane)
        return std::nullopt;

    const auto info = displayedParamFor (generator.getId());
    const auto curveArea = automationCurveBounds (row);
    const auto points = lane->getPoints();

    // In reverse, so of two points drawn on top of each other the one drawn
    // last is also the one that gets grabbed -- same rule as clips and markers.
    for (auto i = points.rbegin(); i != points.rend(); ++i)
    {
        const juce::Point<float> centre (beatToX (i->getBeat()),
                                         valueToLaneY (i->getValue(), info, curveArea));
        if (centre.getDistanceFrom (position) <= autoPointHitRadius)
            return *i;
    }

    return std::nullopt;
}

bool PlaylistComponent::isNearCurveSegment (int row, juce::Point<float> position) const
{
    if (row < 0 || row >= song.getNumGenerators())
        return false;

    auto generator = song.getGenerator (row);
    auto lane = displayedLaneFor (generator);
    if (! lane || lane->getNumPoints() == 0)
        return false;

    const auto info = displayedParamFor (generator.getId());
    const auto curveArea = automationCurveBounds (row);
    const auto value = curveValueAtBeat (lane->getPoints(), xToBeat (position.x));

    return std::abs (valueToLaneY (value, info, curveArea) - position.y) <= autoSegmentHitDistance;
}

void PlaylistComponent::showLaneParamMenu (int row)
{
    if (row < 0 || row >= song.getNumGenerators())
        return;

    auto generator = song.getGenerator (row);
    const auto generatorId = generator.getId();
    const auto params = automatableParamsFor (generatorId);
    const auto current = displayedParamFor (generatorId);

    juce::PopupMenu menu;
    for (int i = 0; i < (int) params.size(); ++i)
    {
        const auto& info = params[(size_t) i];

        // A star on whatever already has points, so the parameters with
        // automation on them can be found without flipping through the list.
        auto lane = generator.findAutomationLane (info.target, info.param);
        const bool automated = lane && lane->getNumPoints() > 0;

        menu.addItem (i + 1, info.label + (automated ? " *" : ""), true,
                      info.target == current.target && info.param == current.param);
    }

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (laneSelectorBounds (row).toNearestInt()));

    // The menu is asynchronous, so nothing captured here may be dereferenced
    // without checking that we are still around.
    juce::Component::SafePointer<PlaylistComponent> safeThis (this);

    menu.showMenuAsync (options, [safeThis, generatorId, params] (int result)
    {
        if (safeThis == nullptr || result <= 0 || result > (int) params.size())
            return;

        const auto& chosen = params[(size_t) (result - 1)];

        // The choice is UI state: the AUTOCURVE itself is only made once a
        // point lands on it, so browsing parameters leaves the song alone.
        safeThis->laneParamChoices[generatorId] = { chosen.target, chosen.param };
        safeThis->repaint();
    });
}

void PlaylistComponent::dragAutoPointTo (juce::Point<float> position)
{
    if (! dragAutoPoint.isValid())
        return;

    const auto curveArea = automationCurveBounds (dragAutoRow);
    if (curveArea.isEmpty())
        return;

    // Beats land on the grid the way a tempo change does: the nearest beat is
    // fine enough for a curve and coarse enough that a drag writes once per
    // beat -- and point writes take EditSync's cheap path anyway. The value is
    // never snapped; the mapping clamps it to the parameter's range instead.
    const auto beat = std::max (0.0, std::round (xToBeat (position.x)));
    const auto value = laneYToValue (position.y, dragAutoInfo, curveArea);

    model::AutomationPoint point (dragAutoPoint);

    if (std::abs (beat - autoPointLastBeat) > beatTolerance)
    {
        point.setBeat (beat, &undoManager);
        autoPointLastBeat = beat;
    }

    const auto valueTolerance = 1.0e-4f * std::abs (dragAutoInfo.maxValue - dragAutoInfo.minValue);

    if (std::abs (value - autoPointLastValue) > valueTolerance)
    {
        point.setValue (value, &undoManager);
        autoPointLastValue = value;
    }
}

void PlaylistComponent::updateRubberBand (juce::Point<float> position)
{
    rubberBand = juce::Rectangle<float> (rubberBandAnchor, position);

    // Selection is UI state, so recomputing it from scratch on every move is
    // free: no model write, no resync.
    auto newSelection = rubberBandBaseSelection;

    for (int row = 0; row < song.getNumGenerators(); ++row)
    {
        for (const auto& placement : placementsFor (song.getGenerator (row)))
        {
            if (! slotBounds (row, placement.start, placement.length).intersects (rubberBand))
                continue;

            if (std::find (newSelection.begin(), newSelection.end(), placement.state) == newSelection.end())
                newSelection.push_back (placement.state);
        }
    }

    selectedClips = std::move (newSelection);
    selectionChanged();
    repaint();   // the band itself moved even when the selection did not
}

//==============================================================================
// Mouse

juce::MouseCursor PlaylistComponent::cursorFor (juce::Point<float> position,
                                                juce::ModifierKeys mods) const
{
    if (markerLaneBounds().contains (position))
        return markerAt (position) ? juce::MouseCursor::DraggingHandCursor   // drag moves the change
                                   : juce::MouseCursor::NormalCursor;

    if (rulerBounds().contains (position))
        return juce::MouseCursor::PointingHandCursor;   // drag sets the loop range

    // An automation lane's gestures are its own, so its cursors are too.
    if (const int laneRow = laneRowAt (position.y); laneRow >= 0 && ! headerBounds().contains (position))
    {
        if (laneSelectorBounds (laneRow).contains (position))
            return juce::MouseCursor::PointingHandCursor;

        if (position.x < (float) labelWidth)
            return juce::MouseCursor::NormalCursor;

        const auto point = autoPointAt (laneRow, position);

        if (mods.isRightButtonDown() || mods.isAltDown())
            return point ? eraseCursor : juce::MouseCursor::NormalCursor;

        return point ? juce::MouseCursor::DraggingHandCursor
                     : juce::MouseCursor::CrosshairCursor;
    }

    if (const int overRow = rowAt (position.y);
        overRow >= 0 && overRow < song.getNumGenerators()
            && ! headerBounds().contains (position)
            && disclosureBounds (overRow).contains (position))
        return juce::MouseCursor::PointingHandCursor;   // toggles the lane

    if (position.x < (float) labelWidth || headerBounds().contains (position))
        return juce::MouseCursor::NormalCursor;

    const int row = clipRowAt (position.y);
    if (row < 0 || row >= song.getNumGenerators())
        return juce::MouseCursor::NormalCursor;

    auto generator = song.getGenerator (row);
    const auto beat = xToBeat (position.x);
    const auto placement = placementAt (generator, beat);

    // Right button and alt erase whichever tool is active.
    if (mods.isRightButtonDown() || mods.isAltDown())
        return placement ? eraseCursor : juce::MouseCursor::NormalCursor;

    if (placement)
    {
        const auto bounds = slotBounds (row, placement->start, placement->length);

        if (placement->isAudio())
        {
            if (trimHandleBounds (bounds).contains (position))
                return juce::MouseCursor::LeftRightResizeCursor;
        }
        else if (patternMenuBounds (bounds).contains (position))
        {
            return juce::MouseCursor::PointingHandCursor;
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

    // The lane has gestures of its own, so entering or leaving it changes what
    // the help bar has to say.
    if (const bool inLane = markerLaneBounds().contains (position); inLane != hoverMarkerLane)
    {
        hoverMarkerLane = inLane;
        updateShortcutHelp();
    }

    // An automation lane has gestures of its own too, so the help bar follows
    // the pointer into one the same way it follows it into the tempo lane.
    const int laneRow = position.x >= (float) labelWidth && ! headerBounds().contains (position)
                            ? laneRowAt (position.y) : -1;
    if (laneRow != hoverLaneRow)
    {
        hoverLaneRow = laneRow;
        updateShortcutHelp();
    }

    const int row = position.x >= (float) labelWidth && ! headerBounds().contains (position)
                        ? clipRowAt (position.y) : -1;
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
    hoverMarkerLane = false;
    hoverLaneRow = -1;
    updateShortcutHelp();
    repaint();
}

void PlaylistComponent::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    // So that holding alt shows the erase cursor without moving the mouse.
    if (mouseIsOver)
        setMouseCursor (cursorFor (lastMousePosition, mods));

    // Alt re-points the drag, so the help bar has to follow the key. This is
    // called for every modifier; the entries only change for the ones that
    // matter, and updateShortcutHelp stays quiet for the rest.
    updateShortcutHelp();
}

void PlaylistComponent::mouseDown (const juce::MouseEvent& e)
{
    mouseIsOver = true;
    lastMousePosition = e.position;
    grabKeyboardFocus();   // ⌘D and the tool keys are ours once the grid is clicked

    dragMode = DragMode::none;

    if (markerLaneBounds().contains (e.position))
    {
        const auto marker = markerAt (e.position);

        // Alt erases on the grid, so it erases here too.
        if (marker && e.mods.isAltDown() && ! e.mods.isPopupMenu())
        {
            removeMarker (marker->state);
            return;
        }

        if (e.mods.isPopupMenu())
        {
            if (marker)
                showMarkerMenu (*marker);
            else
                showLaneMenu (xToBeat (e.position.x), e.position);

            return;
        }

        // A press on empty lane does nothing: adding is a double click or the
        // menu, so a stray click here cannot litter the song with changes.
        if (marker)
        {
            dragMode = DragMode::marker;
            dragMarker = marker->state;
            markerGrabOffsetBeats = xToBeat (e.position.x) - marker->beat;
            markerLastBeat = marker->beat;
            undoManager.beginNewTransaction();
            repaint();
        }

        return;
    }

    if (rulerBounds().contains (e.position) && ! e.mods.isRightButtonDown())
    {
        // Drag the ruler to set the loop range. Grabbing within a few pixels of
        // an existing edge drags that edge (the other one becomes the anchor);
        // anywhere else starts a fresh range from that bar. A click that never
        // leaves its bar clears the range - see mouseUp.
        loopAnchorBeats = nearestBar (xToBeat (e.position.x));
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

    // The disclosure and the automation lane both sit inside what used to be
    // plain row space, so they get first claim on the press.
    if (const int pressRow = rowAt (e.position.y);
        pressRow >= 0 && pressRow < song.getNumGenerators()
            && disclosureBounds (pressRow).contains (e.position))
    {
        toggleRowExpanded (pressRow);
        return;
    }

    if (const int laneRow = laneRowAt (e.position.y); laneRow >= 0)
    {
        auto generator = song.getGenerator (laneRow);

        if (laneSelectorBounds (laneRow).contains (e.position))
        {
            showLaneParamMenu (laneRow);
            return;
        }

        if (e.position.x < (float) labelWidth)
            return;   // the rest of the lane's label cell does nothing

        const auto info = displayedParamFor (generator.getId());
        const auto curveArea = automationCurveBounds (laneRow);
        auto point = autoPointAt (laneRow, e.position);

        // Alt or right-click deletes, the same gesture that erases clips. The
        // empty AUTOCURVE is left behind on the last point: undo of the delete
        // should put the point back, not have to resurrect the lane too.
        if (e.mods.isRightButtonDown() || e.mods.isAltDown() || e.mods.isPopupMenu())
        {
            if (point)
                if (auto lane = displayedLaneFor (generator))
                {
                    undoManager.beginNewTransaction();
                    lane->removePoint (*point, &undoManager);
                }

            return;
        }

        // On the line itself a single click deliberately does nothing -- a
        // double click inserts exactly on it (see mouseDoubleClick). Anywhere
        // else it adds a point where it was aimed and picks it straight up, so
        // add and place are one gesture and one undo.
        if (! point && isNearCurveSegment (laneRow, e.position))
            return;

        undoManager.beginNewTransaction();

        if (! point)
        {
            const auto beat = std::max (0.0, std::round (xToBeat (e.position.x)));
            const auto value = laneYToValue (e.position.y, info, curveArea);

            // The first point is what materialises the lane in the model.
            auto lane = displayedLaneFor (generator);
            if (! lane)
                lane = generator.addAutomationLane (info.target, info.param, &undoManager);

            point = lane->addPoint (beat, value, &undoManager);
        }

        dragMode = DragMode::autoPoint;
        dragAutoPoint = point->state;
        dragAutoInfo = info;
        dragAutoRow = laneRow;
        autoPointLastBeat = point->getBeat();
        autoPointLastValue = point->getValue();
        repaint();
        return;
    }

    const int row = clipRowAt (e.position.y);
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

    if (auto placement = placementAt (generator, clickBeat))
    {
        const auto bounds = slotBounds (row, placement->start, placement->length);
        const auto extend = e.mods.isCommandDown() || e.mods.isShiftDown();

        if (placement->isAudio())
        {
            // The grip on the right edge trims the placement. Select it too,
            // so the trim reads as happening to the clip it highlights.
            if (trimHandleBounds (bounds).contains (e.position))
            {
                selectClip (placement->state, extend);
                dragMode = DragMode::trim;
                trimClip = placement->state;
                trimLastEnd = placement->getEnd();
                return;
            }
        }
        // The chevron swaps which pattern the clip plays: it must not also
        // select the clip or start a move.
        else if (patternMenuBounds (bounds).contains (e.position))
        {
            showPatternMenu (model::PlaylistClip (placement->state), patternMenuBounds (bounds));
            return;
        }

        selectClip (placement->state, extend);

        // Drag from here moves the selection. Snapshot the starts now so the
        // move is always relative to where the gesture began.
        dragMode = DragMode::move;
        dragGrabOffsetBeats = clickBeat - placement->start;
        dragAnchorOriginStart = placement->start;
        dragLastStart = placement->start;
        dragOriginStarts.clear();
        for (const auto& state : selectedClips)
            if (auto selected = placementFor (state))
                dragOriginStarts.push_back (selected->start);
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

    const int row = clipRowAt (e.position.y);
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

        case DragMode::trim:
            // Beats, not bars: a dropped file is almost never a whole number of
            // bars long, so bar-snapping a trim would only ever be in the way.
            trimSelectionTo (std::max (0.0, std::round (beat)));
            break;

        case DragMode::loopRange:
            dragLoopTo (xToBeat (e.position.x));
            break;

        case DragMode::marker:
            dragMarkerTo (xToBeat (e.position.x) - markerGrabOffsetBeats);
            break;

        case DragMode::autoPoint:
            dragAutoPointTo (e.position);
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
    dragMarker = {};
    dragAutoPoint = {};
    dragAutoRow = -1;
    trimClip = {};
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
    if (e.mods.isPopupMenu())
        return;   // the press already opened a menu

    if (markerLaneBounds().contains (e.position))
    {
        if (auto marker = markerAt (e.position))
        {
            if (marker->isTempo())
                editTempoValue (marker->state, 0.0);
            else
                chooseTimeSigValue (marker->state, 0.0, marker->bounds);
        }
        else
        {
            // Empty lane adds a tempo change, because that is the kind that can
            // go where it was clicked; a time signature has to be picked from
            // the menu, which is also where it is told which bar it lands on.
            editTempoValue ({}, std::max (0.0, std::round (xToBeat (e.position.x))));
        }

        return;
    }

    if (e.position.x < (float) labelWidth || headerBounds().contains (e.position))
        return;

    // A double click on an automation lane's line splits the segment: the new
    // point takes the value the curve already has on that beat, so the shape
    // does not jump. The press before this either grabbed a point (nothing
    // more to do here) or was on the line, where a single click deliberately
    // does nothing.
    if (const int laneRow = laneRowAt (e.position.y); laneRow >= 0)
    {
        if (autoPointAt (laneRow, e.position))
            return;

        auto laneGenerator = song.getGenerator (laneRow);
        auto lane = displayedLaneFor (laneGenerator);

        if (! lane || ! isNearCurveSegment (laneRow, e.position))
            return;

        const auto beat = std::max (0.0, std::round (xToBeat (e.position.x)));

        // The first click of this double click may already have added a point
        // here; a second one on the same beat would only stack.
        for (const auto& existing : lane->getPoints())
            if (std::abs (existing.getBeat() - beat) < beatTolerance)
                return;

        undoManager.beginNewTransaction();
        lane->addPoint (beat, curveValueAtBeat (lane->getPoints(), beat), &undoManager);
        return;
    }

    const int row = clipRowAt (e.position.y);
    if (row < 0 || row >= song.getNumGenerators())
        return;

    auto generator = song.getGenerator (row);

    // Only a pattern clip has an editor to open; an audio placement is its
    // file, and there is nothing behind it to show.
    if (auto placement = placementAt (generator, xToBeat (e.position.x)))
        if (! placement->isAudio() && onEditPattern)
        {
            const model::PlaylistClip clip (placement->state);
            onEditPattern (clip.getGeneratorId(), clip.getPatternId());
        }
}

bool PlaylistComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0))
    {
        duplicateSelection();
        return true;
    }

    if (key == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0))
    {
        copySelection();
        return true;
    }

    if (key == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0))
    {
        cutSelection();
        return true;
    }

    if (key == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0))
    {
        pasteClips();
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

//==============================================================================
// Dropping audio files

bool PlaylistComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (isAudioFile (path))
            return true;

    return false;
}

std::optional<PlaylistComponent::FileDropTarget> PlaylistComponent::fileDropTargetFor (juce::Point<float> position) const
{
    if (position.x < (float) labelWidth || headerBounds().contains (position))
        return std::nullopt;

    const auto numRows = song.getNumGenerators();

    // The whole row band, lane included: files aimed at an expanded audio
    // row's lane still mean that row.
    const auto row = rowAt (position.y);

    FileDropTarget target;
    target.startBeats = snapToBar (xToBeat (position.x));

    // Only an audio generator's row can hold files; a drop anywhere else makes
    // one. The preview still follows the pointer, so the drop looks like it
    // lands where it was aimed.
    if (row >= 0 && row < numRows && song.getGenerator (row).isAudio())
        target.row = row;

    target.ghostRow = target.row >= 0 ? target.row
                                      : juce::jlimit (0, std::max (0, numRows - 1), row);
    return target;
}

void PlaylistComponent::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    fileDragMove (files, x, y);
}

void PlaylistComponent::fileDragMove (const juce::StringArray&, int x, int y)
{
    const auto target = fileDropTargetFor ({ (float) x, (float) y });

    if (target.has_value() != fileDropTarget.has_value()
         || (target && (target->row != fileDropTarget->row
                         || target->ghostRow != fileDropTarget->ghostRow
                         || std::abs (target->startBeats - fileDropTarget->startBeats) > beatTolerance)))
    {
        fileDropTarget = target;
        repaint();   // only on crossing into another slot, not on every move
    }
}

void PlaylistComponent::fileDragExit (const juce::StringArray&)
{
    fileDropTarget.reset();
    repaint();
}

void PlaylistComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    const auto target = fileDropTargetFor ({ (float) x, (float) y });

    fileDropTarget.reset();
    repaint();

    if (! target)
        return;

    std::vector<juce::File> audioFiles;
    for (const auto& path : files)
        if (isAudioFile (path))
            audioFiles.emplace_back (path);

    if (audioFiles.empty())
        return;

    // One transaction for the whole drop, generator and all, so a mistaken
    // drop is one undo away.
    undoManager.beginNewTransaction();

    // Held back until a file actually turns out to be readable, so a drop of
    // nothing but unreadable files doesn't leave an empty track behind.
    std::optional<model::Generator> generator;

    if (target->row >= 0)
        generator = song.getGenerator (target->row);

    auto playlist = song.getPlaylist();
    auto start = target->startBeats;
    std::vector<juce::ValueTree> placed;

    for (const auto& file : audioFiles)
    {
        const auto seconds = model::readAudioFileLengthSeconds (file);

        if (seconds <= 0.0)
            continue;   // nothing here we can read, so nothing to place

        if (! generator)
            generator = song.addGenerator (file.getFileNameWithoutExtension(),
                                           model::Generator::audioType, &undoManager);

        // How much grid the file covers depends on where it lands, so this is
        // recomputed as the search below moves it.
        auto lengthInBeats = [this, seconds] (double at)
        {
            return song.beatsFromSeconds (song.secondsFromBeats (at) + seconds) - at;
        };

        // Never stack, the same rule painting follows: slide past whatever the
        // row already holds rather than burying it.
        while (! isRangeFree (*generator, start, lengthInBeats (start)))
            start = nextBarAfter (start);

        const auto length = lengthInBeats (start);
        placed.push_back (playlist.addAudioClip (*generator, file, start, seconds, &undoManager).state);

        // Several files dropped at once lay out end to end, rounded up to the
        // bar so the next one still lands on the grid.
        const auto end = start + length;
        while (start < end - beatTolerance)
            start = nextBarAfter (start);
    }

    // Select what was placed: it is what the user will want to move or trim,
    // and it makes clear which row took the files.
    if (! placed.empty())
    {
        selectedClips = std::move (placed);
        selectionChanged();
    }

    if (generator && onSelectGenerator)
        onSelectGenerator (generator->getId());
}

} // namespace carve::app
