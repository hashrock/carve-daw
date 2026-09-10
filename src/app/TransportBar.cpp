#include "TransportBar.h"

namespace carve::app
{

namespace
{
    const juce::Colour barBackground (0xff26262c);
    const juce::Colour panelFill (0xff1e1e23);
    const juce::Colour panelEdge (0xff36363d);
    const juce::Colour captionColour (0xff8a8a94);
    const juce::Colour lcdFill (0xff0f1a25);
    const juce::Colour lcdEdge (0xff070b10);
    const juce::Colour lcdText (0xff9fd8ff);
    const juce::Colour accent (0xffe08a3c);

    juce::Font lcdFont()
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold));
    }

    juce::String padded (int value, int digits)
    {
        auto text = juce::String (std::abs (value)).paddedLeft ('0', digits);
        return value < 0 ? "-" + text : text;
    }
} // namespace

//==============================================================================
NumberDisplay::NumberDisplay (int digits)
{
    value.setFont (lcdFont());
    value.setColour (juce::Label::textColourId, lcdText);
    value.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    value.setColour (juce::Label::textWhenEditingColourId, juce::Colours::white);
    value.setColour (juce::TextEditor::backgroundColourId, lcdFill);
    value.setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.5f));
    value.setJustificationType (juce::Justification::centredRight);
    value.setEditable (false, true, false);
    value.setBorderSize ({ 0, 3, 0, 3 });
    value.onTextChange = [this]
    {
        if (onEdited)
            onEdited (value.getText().trim());
    };
    addAndMakeVisible (value);

    setSize (digits * 9 + 8 + arrowsWidth, 20);
}

void NumberDisplay::setText (const juce::String& text)
{
    if (! value.isBeingEdited())
        value.setText (text, juce::dontSendNotification);
}

void NumberDisplay::resized()
{
    auto area = getLocalBounds();
    auto arrows = area.removeFromRight (arrowsWidth);
    upArrow = arrows.removeFromTop (arrows.getHeight() / 2);
    downArrow = arrows;
    value.setBounds (area);
}

void NumberDisplay::paint (juce::Graphics& g)
{
    // The recessed glass the number sits in.
    const auto glass = getLocalBounds().withTrimmedRight (arrowsWidth).toFloat();
    g.setColour (lcdEdge);
    g.fillRoundedRectangle (glass, 3.0f);
    g.setColour (lcdFill);
    g.fillRoundedRectangle (glass.reduced (1.0f), 2.5f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRoundedRectangle (glass.reduced (1.0f).withHeight (glass.getHeight() * 0.45f), 2.5f);

    // The step arrows.
    auto drawArrow = [&g] (juce::Rectangle<int> r, bool up)
    {
        const auto c = r.toFloat().getCentre();
        juce::Path p;
        if (up) p.addTriangle (c.x - 3.5f, c.y + 2.0f, c.x + 3.5f, c.y + 2.0f, c.x, c.y - 2.0f);
        else    p.addTriangle (c.x - 3.5f, c.y - 2.0f, c.x + 3.5f, c.y - 2.0f, c.x, c.y + 2.0f);
        g.fillPath (p);
    };

    g.setColour (captionColour);
    drawArrow (upArrow, true);
    drawArrow (downArrow, false);
}

void NumberDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (! onStep)
        return;

    if (upArrow.contains (e.getPosition()))
        onStep (1);
    else if (downArrow.contains (e.getPosition()))
        onStep (-1);
}

//==============================================================================
void PlayModeSwitch::setPatternMode (bool shouldBePattern, juce::NotificationType notify)
{
    if (patternMode == shouldBePattern)
        return;

    patternMode = shouldBePattern;
    repaint();

    if (notify != juce::dontSendNotification && onChanged)
        onChanged (patternMode);
}

void PlayModeSwitch::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    const int rowHeight = area.getHeight() / 2;

    auto drawRow = [&] (juce::Rectangle<int> row, const juce::String& text, bool lit)
    {
        const auto lamp = row.removeFromLeft (14).toFloat().withSizeKeepingCentre (7.0f, 7.0f);

        if (lit)
        {
            g.setColour (accent.withAlpha (0.35f));
            g.fillEllipse (lamp.expanded (2.5f));
            g.setColour (accent);
        }
        else
        {
            g.setColour (juce::Colour (0xff3a3a40));
        }

        g.fillEllipse (lamp);

        g.setColour (lit ? juce::Colour (0xffe6e6ea) : captionColour);
        g.setFont (juce::FontOptions (12.0f, lit ? juce::Font::bold : juce::Font::plain));
        g.drawText (text, row.withTrimmedLeft (2), juce::Justification::centredLeft);
    };

    drawRow (area.removeFromTop (rowHeight), "Pattern", patternMode);
    drawRow (area, "Song", ! patternMode);
}

void PlayModeSwitch::mouseDown (const juce::MouseEvent& e)
{
    setPatternMode (e.getPosition().y < getHeight() / 2, juce::sendNotification);
}

//==============================================================================
TransportBar::TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um)
    : edit (editToControl), song (std::move (songModel)), undoManager (um)
{
    logoButton.onClick = [this] { showFileMenu(); };

    // --- tempo
    bpmDisplay.onStep = [this] (int direction)
    {
        undoManager.beginNewTransaction();
        song.setTempo (std::round (song.getTempo()) + direction, &undoManager);
    };
    bpmDisplay.onEdited = [this] (const juce::String& text)
    {
        // Still checked here rather than left to setTempo's clamp: text that
        // parses to nothing reads as 0, and clamping that to the slowest
        // tempo the model allows is not what a typo means.
        const auto bpm = text.getDoubleValue();
        if (bpm >= model::TempoChange::minBpm && bpm <= model::TempoChange::maxBpm)
        {
            undoManager.beginNewTransaction();
            song.setTempo (bpm, &undoManager);
        }
    };

    // --- pattern / song
    playMode.onChanged = [this] (bool patternMode)
    {
        if (onPlayModeChanged)
            onPlayModeChanged (patternMode);

        // A switch mid-playback takes effect on the loop the transport is in.
        if (edit.getTransport().isPlaying())
            togglePlay(), togglePlay();
    };

    // --- transport
    rewindButton.onClick = [this] { stepBar (-1); };
    forwardButton.onClick = [this] { stepBar (1); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this]
    {
        auto& transport = edit.getTransport();
        transport.stop (false, false);
        transport.setPosition (te::TimePosition());
    };

    browserButton.setClickingTogglesState (false);   // MainComponent sets the state
    browserButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff35608a));
    browserButton.setTooltip ("Sample browser (cmd-B)");
    browserButton.onClick = [this] { if (onToggleBrowser) onToggleBrowser(); };
    rewindButton.setTooltip ("Back a bar");
    forwardButton.setTooltip ("Forward a bar");
    playButton.setTooltip ("Play / pause (Space)");
    stopButton.setTooltip ("Stop and return to the start");

    for (auto* b : { &rewindButton, &playButton, &stopButton, &forwardButton, &undoButton, &redoButton })
        b->setFlat (true);

    // Play is lit while the song runs, the way the pattern lamp is.
    playButton.setColour (juce::TextButton::textColourOnId, accent);

    // --- bar
    barDisplay.onStep = [this] (int direction) { stepBar (direction); };
    barDisplay.onEdited = [this] (const juce::String& text)
    {
        if (const int bar = text.getIntValue(); bar >= 1)
            seekToBar (bar);
    };

    // --- loop
    loopCheck.setToggleState (true, juce::dontSendNotification);
    loopCheck.setTooltip ("Loop the range below, or the whole song");
    loopCheck.onClick = [this] { edit.getTransport().looping = loopCheck.getToggleState() || isPatternMode(); };

    loopStartDisplay.onStep = [this] (int d) { setLoopBars (getLoopStartBar() + d, getLoopLengthBars()); };
    loopLengthDisplay.onStep = [this] (int d) { setLoopBars (getLoopStartBar(), getLoopLengthBars() + d); };
    loopStartDisplay.onEdited = [this] (const juce::String& t) { setLoopBars (t.getIntValue(), getLoopLengthBars()); };
    loopLengthDisplay.onEdited = [this] (const juce::String& t) { setLoopBars (getLoopStartBar(), t.getIntValue()); };

    // --- undo / redo
    undoButton.onClick = [this] { undoManager.undo(); };
    redoButton.onClick = [this] { undoManager.redo(); };
    undoButton.setTooltip ("Undo (cmd-Z)");
    redoButton.setTooltip ("Redo (shift-cmd-Z)");

    // --- mixer, master
    mixerButton.onClick = [this] { if (onOpenMixer) onOpenMixer(); };

    // Save, on the bar next to the document's name: Cmd-S is there too, but
    // the state it acts on is what the name shows, so the button sits by it.
    addAndMakeVisible (saveButton);
    saveButton.onClick = [this] { if (onSave) onSave(); };

    masterKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    masterKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    masterKnob.setRange (0.0, 1.0);
    masterKnob.setDoubleClickReturnValue (true, te::decibelsToVolumeFaderPosition (model::MasterBus::defaultVolumeDb));
    masterKnob.setColour (juce::Slider::rotarySliderFillColourId, accent);
    masterKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a3a40));
    masterKnob.setColour (juce::Slider::thumbColourId, juce::Colour (0xffe6e6ea));
    masterKnob.setTooltip ("Master level");
    masterKnob.onDragStart = [this] { undoManager.beginNewTransaction(); };
    masterKnob.onValueChange = [this] { setMasterVolumeFromKnob(); };

    documentLabel.setJustificationType (juce::Justification::centredRight);
    documentLabel.setMinimumHorizontalScale (1.0f);   // ellipsis, not squashed text
    documentLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

    for (auto* c : std::initializer_list<juce::Component*> {
             &logoButton, &bpmDisplay, &playMode,
             &rewindButton, &playButton, &stopButton, &forwardButton,
             &barDisplay, &loopCheck, &loopStartDisplay, &loopLengthDisplay,
             &undoButton, &redoButton, &browserButton, &mixerButton, &masterKnob, &masterMeter, &documentLabel })
        addAndMakeVisible (c);

    startTimerHz (30);
}

void TransportBar::showFileMenu()
{
    // The recent songs get IDs of their own above the fixed items, so one
    // callback can tell "the third recent song" from "Save As".
    enum { newId = 1, openId, openDemoId, saveId, saveAsId, exportId, firstRecentId = 100 };

    juce::PopupMenu menu;
    menu.addItem (newId, "New");
    menu.addItem (openId, "Open...");

    const auto recent = getRecentSongs != nullptr ? getRecentSongs() : juce::StringArray();
    juce::PopupMenu recentMenu;

    for (int i = 0; i < recent.size(); ++i)
        recentMenu.addItem (firstRecentId + i, recent[i]);

    // Shown greyed rather than hidden when there is nothing in it: a menu that
    // grows an item is harder to learn than one whose item is dim at first.
    menu.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());

    menu.addItem (openDemoId, "Open Demo Song");
    menu.addSeparator();
    menu.addItem (saveId, "Save");
    menu.addItem (saveAsId, "Save As...");
    menu.addSeparator();
    menu.addItem (exportId, "Export Audio...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (logoButton),
                        [safe = juce::Component::SafePointer (this)] (int result)
    {
        if (safe == nullptr)
            return;

        switch (result)
        {
            case newId:       if (safe->onNew)      safe->onNew();      break;
            case openId:      if (safe->onOpen)     safe->onOpen();     break;
            case openDemoId:  if (safe->onOpenDemo) safe->onOpenDemo(); break;
            case saveId:      if (safe->onSave)     safe->onSave();     break;
            case saveAsId:    if (safe->onSaveAs)   safe->onSaveAs();   break;
            case exportId:    if (safe->onExport)   safe->onExport();   break;

            default:
                if (result >= firstRecentId && safe->onOpenRecent != nullptr)
                    safe->onOpenRecent (result - firstRecentId);

                break;
        }
    });
}

void TransportBar::setSong (model::Song newSong)
{
    song = std::move (newSong);
}

void TransportBar::setBrowserShown (bool shown)
{
    browserButton.setToggleState (shown, juce::dontSendNotification);
}

void TransportBar::setDocumentState (const juce::String& documentName, bool hasUnsavedChanges)
{
    // Same "edited" marker the window title carries.
    documentLabel.setText (documentName + (hasUnsavedChanges ? " *" : ""),
                           juce::dontSendNotification);
}

double TransportBar::getSongLengthBeats() const
{
    // The model knows about audio placements too; measuring only pattern clips
    // here would cut an audio-only song short.
    return song.getLengthBeats();
}

//==============================================================================
int TransportBar::getCurrentBar() const
{
    const auto beats = edit.tempoSequence.toBeats (edit.getTransport().getPosition()).inBeats();
    return song.toBarsAndBeats (std::max (0.0, beats)).bar + 1;
}

void TransportBar::seekToBar (int bar)
{
    const auto beat = song.beatOfBar (std::max (0, bar - 1));
    edit.getTransport().setPosition (edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat)));
}

void TransportBar::stepBar (int direction)
{
    seekToBar (getCurrentBar() + direction);
}

int TransportBar::getLoopStartBar() const
{
    return song.hasLoopRange() ? song.toBarsAndBeats (song.getLoopStart()).bar + 1 : 1;
}

int TransportBar::getLoopLengthBars() const
{
    // The bar the end falls in, counted from the start bar; a hair before the
    // end so a range ending exactly on a bar line does not count the next one.
    const auto end = song.hasLoopRange() ? song.getLoopEnd() : std::max (4.0, getSongLengthBeats());
    const int endBar = song.toBarsAndBeats (std::max (0.0, end - 1.0e-6)).bar + 1;
    return std::max (1, endBar - getLoopStartBar() + 1);
}

void TransportBar::setLoopBars (int startBar, int lengthBars)
{
    startBar = std::max (1, startBar);
    lengthBars = std::max (1, lengthBars);

    undoManager.beginNewTransaction();
    song.setLoopRange (song.beatOfBar (startBar - 1), song.beatOfBar (startBar - 1 + lengthBars), &undoManager);

    // The transport keeps its own copy of the range; hand it the new one so a
    // running loop picks it up rather than finishing the old one first.
    if (edit.getTransport().isPlaying() && ! isPatternMode())
        edit.getTransport().setLoopRange (edit.tempoSequence.toTime (
            te::BeatRange (te::BeatPosition::fromBeats (song.getLoopStart()),
                           te::BeatPosition::fromBeats (song.getLoopEnd()))));
}

void TransportBar::setMasterVolumeFromKnob()
{
    // The model is the source of truth: the undo manager sees it, the mixer's
    // master strip reads it back, and EditSync pushes it into the Edit.
    song.getMasterBus().setVolumeDb (te::volumeFaderPositionToDB ((float) masterKnob.getValue()),
                                     &undoManager);
}

//==============================================================================
void TransportBar::togglePlay()
{
    auto& transport = edit.getTransport();

    if (transport.isPlaying())
    {
        transport.stop (false, false);
        return;
    }

    te::BeatRange loop;

    if (isPatternMode())
    {
        // Pattern mode loops the patterns alone, from beat zero, and always
        // loops -- a one-shot of a bar is not what the mode is for.
        loop = patternLoopRange();
        transport.setPosition (te::TimePosition());
    }
    else if (song.hasLoopRange())
    {
        loop = { te::BeatPosition::fromBeats (song.getLoopStart()),
                 te::BeatPosition::fromBeats (song.getLoopEnd()) };
    }
    else
    {
        loop = { te::BeatPosition(), te::BeatPosition::fromBeats (std::max (4.0, getSongLengthBeats())) };
    }

    transport.setLoopRange (edit.tempoSequence.toTime (loop));
    transport.looping = loopCheck.getToggleState() || isPatternMode();
    transport.ensureContextAllocated();
    transport.play (false);
}

te::BeatRange TransportBar::patternLoopRange() const
{
    // Never shorter than a beat: a song with nothing to audition still needs
    // a range for the transport to loop over.
    const auto length = getAuditionLengthBeats ? getAuditionLengthBeats() : 0.0;
    return { te::BeatPosition(), te::BeatPosition::fromBeats (std::max (1.0, length)) };
}

void TransportBar::refreshPatternLoop()
{
    auto& transport = edit.getTransport();

    if (! isPatternMode() || ! transport.isPlaying())
        return;

    const auto wanted = edit.tempoSequence.toTime (patternLoopRange());

    // This is reached on every model change while the mode is on -- each
    // note drawn -- so an unchanged range is left alone rather than re-set.
    if (std::abs ((transport.getLoopRange().getEnd() - wanted.getEnd()).inSeconds()) < 1.0e-6)
        return;

    transport.setLoopRange (wanted);

    // A loop that just got shorter can leave the playhead past its end; wrap
    // it back the way the loop would have, rather than waiting for the
    // engine to decide.
    if (transport.getPosition() >= wanted.getEnd())
        transport.setPosition (wanted.getStart());
}

void TransportBar::timerCallback()
{
    auto& transport = edit.getTransport();
    const bool playing = transport.isPlaying();

    playButton.setIcon (playing ? Icon::pause : Icon::play);
    playButton.setToggleState (playing, juce::dontSendNotification);

    bpmDisplay.setText (juce::String (song.getTempo(), 1));
    barDisplay.setText (padded (getCurrentBar(), 3));
    loopStartDisplay.setText (padded (getLoopStartBar(), 3));
    loopLengthDisplay.setText (padded (getLoopLengthBars(), 3));

    undoButton.setEnabled (undoManager.canUndo());
    redoButton.setEnabled (undoManager.canRedo());

    // Nothing tells us when the master volume moves -- there is no model
    // listener here -- so it is read back every tick, unless the knob is
    // the thing moving it.
    if (! masterKnob.isMouseButtonDown())
        masterKnob.setValue (te::decibelsToVolumeFaderPosition (song.getMasterBus().getVolumeDb()),
                             juce::dontSendNotification);

    // The level at the end of the graph, which only exists while there is a
    // playback context to render it.
    auto* context = edit.getCurrentPlaybackContext();
    masterMeter.update (context != nullptr ? &context->masterLevels : nullptr);
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (barBackground);

    for (const auto& panel : panels)
    {
        g.setColour (panelFill);
        g.fillRoundedRectangle (panel.toFloat(), 5.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 5.0f, 1.0f);
    }

    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    for (const auto& [area, text] : captions)
    {
        g.setColour (captionColour);
        g.drawText (text, area, juce::Justification::centredLeft);
    }

    // Dividers: a dark line with a light one beside it, the bevel a real
    // panel would have between its sections.
    for (int x : dividers)
    {
        g.setColour (juce::Colour (0xff17171a));
        g.fillRect (x, 6, 1, getHeight() - 12);
        g.setColour (juce::Colour (0xff3a3a40));
        g.fillRect (x + 1, 6, 1, getHeight() - 12);
    }

    g.setColour (juce::Colour (0xff17171a));
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void TransportBar::resized()
{
    panels.clear();
    captions.clear();
    dividers.clear();

    auto area = getLocalBounds().reduced (6, 5);
    const int h = area.getHeight();

    auto take = [&area] (int width, int gap = 0)
    {
        auto r = area.removeFromLeft (width);
        area.removeFromLeft (gap);
        return r;
    };

    auto divider = [&]
    {
        area.removeFromLeft (4);
        dividers.push_back (area.getX());
        area.removeFromLeft (8);
    };

    auto panel = [&] (int width)
    {
        auto r = take (width);
        panels.push_back (r);
        return r.reduced (6, 0);
    };

    logoButton.setBounds (take (52, 6));
    divider();

    // BPM
    {
        auto p = panel (6 + 28 + bpmDisplay.getWidth() + 6);
        captions.emplace_back (p.removeFromLeft (28), "BPM");
        bpmDisplay.setBounds (p.withSizeKeepingCentre (bpmDisplay.getWidth(), 22));
    }
    divider();

    playMode.setBounds (take (66));
    divider();

    // Transport
    {
        auto p = panel (6 + 4 * 32 + 6);
        for (auto* b : { &rewindButton, &playButton, &stopButton, &forwardButton })
            b->setBounds (p.removeFromLeft (32).withSizeKeepingCentre (32, 28));
    }
    divider();

    // Bar
    {
        auto p = panel (6 + 28 + barDisplay.getWidth() + 6);
        captions.emplace_back (p.removeFromLeft (28), "BAR");
        barDisplay.setBounds (p.withSizeKeepingCentre (barDisplay.getWidth(), 22));
    }
    divider();

    // Loop: the checkbox, then start over length in two rows
    {
        const int fieldWidth = loopStartDisplay.getWidth();
        auto p = panel (6 + 60 + 4 + 44 + fieldWidth + 6);
        loopCheck.setBounds (p.removeFromLeft (60));
        p.removeFromLeft (4);

        const int rowHeight = (h - 2) / 2;
        auto top = p.removeFromTop (rowHeight);
        auto bottom = p.removeFromBottom (rowHeight);
        captions.emplace_back (top.removeFromLeft (44), "Start");
        captions.emplace_back (bottom.removeFromLeft (44), "Length");
        loopStartDisplay.setBounds (top.withSizeKeepingCentre (fieldWidth, 17));
        loopLengthDisplay.setBounds (bottom.withSizeKeepingCentre (fieldWidth, 17));
    }
    divider();

    undoButton.setBounds (take (28, 2).withSizeKeepingCentre (28, 28));
    redoButton.setBounds (take (28).withSizeKeepingCentre (28, 28));
    divider();

    browserButton.setBounds (take (90, 6).withSizeKeepingCentre (90, 28));
    mixerButton.setBounds (take (76, 10).withSizeKeepingCentre (76, 28));

    // Master: knob and a lying-down meter
    masterKnob.setBounds (take (34, 6).withSizeKeepingCentre (34, 34));
    masterMeter.setBounds (take (110, 12).withSizeKeepingCentre (110, 14));

    // Save at the far right, then whatever is left goes to the document name
    saveButton.setBounds (area.removeFromRight (70).withSizeKeepingCentre (70, 28));
    area.removeFromRight (8);
    documentLabel.setBounds (area);

    repaint();
}

} // namespace carve::app
