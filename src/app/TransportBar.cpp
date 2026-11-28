#include "TransportBar.h"
#include "PerfProbe.h"
#include "model/SampleSongs.h"
#include "Fonts.h"
#include "TouchUi.h"

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

    // The master knob's travel, 0..1, against dB: 0 dB straight up, the
    // right half up to the fader's +6, the left half the fader's own curve
    // from silence. Turning tracktion's fader position straight into an
    // angle put 0 dB at two o'clock, where it read as a knob turned past it.
    constexpr float masterKnobMaxDb = 6.0f;

    float masterKnobToDb (double travel)
    {
        if (travel >= 0.5)
            return (float) ((travel - 0.5) * 2.0) * masterKnobMaxDb;

        return te::volumeFaderPositionToDB ((float) (travel * 2.0) * te::decibelsToVolumeFaderPosition (0.0f));
    }

    double dbToMasterKnob (float db)
    {
        if (db >= 0.0f)
            return 0.5 + 0.5 * juce::jmin (db, masterKnobMaxDb) / masterKnobMaxDb;

        return 0.5 * te::decibelsToVolumeFaderPosition (db) / te::decibelsToVolumeFaderPosition (0.0f);
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

    // The number is a label, and a press on it would otherwise stop there;
    // it is passed on so a vertical drag on the number can scrub it.
    value.addMouseListener (this, false);
    value.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);

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

    // Disabled, there is nothing to step: no arrows (the number dims itself).
    if (! isEnabled())
        return;

    g.setColour (captionColour);
    drawArrow (upArrow, true);
    drawArrow (downArrow, false);
}

void NumberDisplay::mouseDown (const juce::MouseEvent& event)
{
    if (! onStep)
        return;

    // Presses on the number arrive through the label (see the constructor).
    const auto e = event.getEventRelativeTo (this);

    heldDirection = upArrow.contains (e.getPosition()) ? 1
                  : downArrow.contains (e.getPosition()) ? -1
                                                         : 0;
    scrubbing = heldDirection == 0 && ! value.isBeingEdited();
    scrubStepsDone = 0;

    if (heldDirection == 0)
        return;

    // The click lands now; the repeat only starts if the button is still down
    // when the delay is up.
    onStep (heldDirection);
    repeatCount = 0;
    startTimer (holdDelayMs);
}

void NumberDisplay::mouseUp (const juce::MouseEvent&)
{
    heldDirection = 0;
    scrubbing = false;
    stopTimer();
}

void NumberDisplay::mouseDrag (const juce::MouseEvent& event)
{
    const auto e = event.getEventRelativeTo (this);

    // Dragging up or down on the number steps it, a step every few pixels:
    // up is more. Counted from where the press landed rather than from the
    // last event, so the value follows the pointer back down exactly.
    if (scrubbing && onStep)
    {
        const auto steps = -e.getDistanceFromDragStartY() / pixelsPerScrubStep;

        for (; scrubStepsDone < steps; ++scrubStepsDone)
            onStep (1);

        for (; scrubStepsDone > steps; --scrubStepsDone)
            onStep (-1);

        return;
    }

    // Sliding off the arrow stops the repeat, the way a button cancels when
    // the pointer leaves it.
    if (heldDirection != 0 && ! getLocalBounds().contains (e.getPosition()))
    {
        heldDirection = 0;
        stopTimer();
    }
}

void NumberDisplay::timerCallback()
{
    if (heldDirection == 0 || ! onStep)
    {
        stopTimer();
        return;
    }

    onStep (heldDirection);

    // Accelerating, so a long hold covers a useful range without the first
    // few steps being uncatchable.
    const auto progress = juce::jmin (1.0f, (float) ++repeatCount / (float) repeatsToFullSpeed);
    startTimer (juce::roundToInt (firstRepeatMs + (fastestRepeatMs - firstRepeatMs) * progress));
}

//==============================================================================
void PlayModeSwitch::setPatternMode (bool shouldBePattern, juce::NotificationType notify)
{
    if (patternMode == shouldBePattern)
        return;

    patternMode = shouldBePattern;
    repaint();

    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);

    if (notify != juce::dontSendNotification && onChanged)
        onChanged (patternMode);
}

void PlayModeSwitch::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    const auto half = area.getWidth() * 0.5f;

    g.setColour (panelFill);
    g.fillRoundedRectangle (area, 5.0f);

    // The lit half is a filled segment, as a tool that is chosen is.
    const auto lit = patternMode ? area.withWidth (half) : area.withTrimmedLeft (half);
    g.setColour (accent);
    g.fillRoundedRectangle (lit, 5.0f);

    g.setColour (panelEdge.brighter (0.2f));
    g.drawRoundedRectangle (area, 5.0f, 1.0f);

    auto drawHalf = [&] (juce::Rectangle<float> r, const juce::String& text, bool on)
    {
        g.setColour (on ? juce::Colours::black : juce::Colour (0xffb8b8c0));
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText (text, r, juce::Justification::centred, false);
    };

    drawHalf (area.withWidth (half), "Pattern", patternMode);
    drawHalf (area.withTrimmedLeft (half), "Song", ! patternMode);
}

void PlayModeSwitch::mouseDown (const juce::MouseEvent& e)
{
    setPatternMode (e.getPosition().x < getWidth() / 2, juce::sendNotification);
}

std::unique_ptr<juce::AccessibilityHandler> PlayModeSwitch::createAccessibilityHandler()
{
    struct Handler : juce::AccessibilityHandler
    {
        explicit Handler (PlayModeSwitch& s)
            : juce::AccessibilityHandler (s, juce::AccessibilityRole::toggleButton,
                                          juce::AccessibilityActions()
                                              .addAction (juce::AccessibilityActionType::press,  [&s] { s.setPatternMode (! s.isPatternMode(), juce::sendNotification); })
                                              .addAction (juce::AccessibilityActionType::toggle, [&s] { s.setPatternMode (! s.isPatternMode(), juce::sendNotification); })),
              owner (s)
        {
        }

        juce::String getTitle() const override  { return "Song mode"; }
        juce::String getHelp() const override   { return "On: play the playlist. Off: loop the current patterns"; }

        juce::AccessibleState getCurrentState() const override
        {
            auto state = juce::AccessibilityHandler::getCurrentState().withCheckable();
            return owner.isPatternMode() ? state : state.withChecked();
        }

        PlayModeSwitch& owner;
    };

    return std::make_unique<Handler> (*this);
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
    recordButton.onClick = [this] { toggleRecord(); };
    captureButton.onClick = [this] { if (onCapture) onCapture(); };
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
    recordButton.setTooltip ("Record MIDI into the playing pattern (Cmd+R)");
    captureButton.setTooltip ("Capture: make a pattern of what you just played, recording or not (Shift+Cmd+R)");
    stopButton.setTooltip ("Stop and return to the start");

    for (auto* b : { &rewindButton, &playButton, &stopButton, &recordButton, &captureButton, &forwardButton,
                     &undoButton, &redoButton })
        b->setFlat (true);

    // The transport is what the hand goes to most: symbols to match, rather
    // than the toolbar size the button height would give them.
    for (auto* b : { &rewindButton, &playButton, &stopButton, &recordButton, &captureButton, &forwardButton })
        b->setIconSize (13.5f);

    playButton.setIconSize (15.0f);

    captureButton.setEnabled (false);

    // Play is lit while the song runs, the way the pattern lamp is.
    playButton.setColour (juce::TextButton::textColourOnId, accent);

    // Red rather than the app's accent: the one control whose colour is a
    // convention older than any of this.
    recordButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xffe0503c));

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
    loopCheck.onClick = [this]
    {
        if (! isPatternMode())
            songLoopChecked = loopCheck.getToggleState();

        syncLoopRange (false);
    };

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


    meterClick.onClick = [this] { if (onOpenAnalyzer) onOpenAnalyzer(); };
    masterMeter.setInterceptsMouseClicks (true, false);   // a meter ignores the mouse unless asked
    masterMeter.addMouseListener (&meterClick, false);
    masterMeter.setMouseCursor (juce::MouseCursor::PointingHandCursor);

    // Save, on the bar next to the document's name: Cmd-S is there too, but
    // the state it acts on is what the name shows, so the button sits by it.
    saveButton.onClick = [this] { if (onSave) onSave(); };

    masterKnob.setRange (0.0, 1.0);
    masterKnob.setDoubleClickReturnValue (true, dbToMasterKnob (model::MasterBus::defaultVolumeDb));
    masterKnob.setTooltip ("Master level");
    masterKnob.onDragStart = [this] { undoManager.beginNewTransaction(); };
    masterKnob.onValueChange = [this] { setMasterVolumeFromKnob(); };

    documentLabel.setJustificationType (juce::Justification::centredLeft);
    documentLabel.setFont (uiFont (fonts::normal, juce::Font::bold));
    documentLabel.setMinimumHorizontalScale (1.0f);   // ellipsis, not squashed text
    documentLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff0f0f4));

    for (auto* c : std::initializer_list<juce::Component*> {
             &logoButton, &undoButton, &redoButton, &browserButton, &mixerButton, &masterKnob, &masterMeter,
             &documentLabel, &saveButton })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> {
             &playMode, &rewindButton, &playButton, &stopButton, &recordButton, &captureButton, &forwardButton,
             &barDisplay, &bpmDisplay, &loopCheck, &loopStartDisplay, &loopLengthDisplay })
        strip.addAndMakeVisible (c);

    startTimerHz (30);
}

void TransportBar::setListeningToReference (bool listening)
{
    masterMeter.setTint (listening ? juce::Colour (0xff5fc4b8) : juce::Colour());
}

void TransportBar::showFileMenu()
{
    // The recent songs get IDs of their own above the fixed items, so one
    // callback can tell "the third recent song" from "Save As".
    enum { newId = 1, openId, openDemoId, saveId, saveAsId, exportId, analyzerId,
           settingsId, aboutId, noticesId, soundLibraryId,
           firstRecentId = 100, clearRecentId = 150, firstSampleSongId = 200 };

    juce::PopupMenu menu;
    menu.addItem (newId, "New");
    menu.addItem (openId, "Open...");

    const auto recent = getRecentSongs != nullptr ? getRecentSongs() : juce::StringArray();
    juce::PopupMenu recentMenu;

    for (int i = 0; i < recent.size(); ++i)
        recentMenu.addItem (firstRecentId + i, recent[i]);

    // At the end, as in every Mac app's Open Recent. No confirmation: what it
    // throws away is a list of shortcuts, not anything that was made.
    recentMenu.addSeparator();
    recentMenu.addItem (clearRecentId, "Clear Menu");

    // Shown greyed rather than hidden when there is nothing in it: a menu that
    // grows an item is harder to learn than one whose item is dim at first.
    menu.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());

    menu.addItem (openDemoId, "Open Demo Song");

    // The songs the app ships with, listed the same way the recents are.
    const auto samples = model::sampleSongNames();
    juce::PopupMenu sampleMenu;

    for (int i = 0; i < samples.size(); ++i)
        sampleMenu.addItem (firstSampleSongId + i, samples[i]);

    menu.addSubMenu ("Open Sample Song", sampleMenu, ! samples.isEmpty());
    menu.addSeparator();
    menu.addItem (saveId, "Save");
    menu.addItem (saveAsId, "Save As...");
    menu.addSeparator();
    menu.addItem (exportId, "Export Audio...");
    menu.addItem (analyzerId, "Analyzer...");
    menu.addSeparator();
    menu.addItem (soundLibraryId, "Sound Library...");

    // On macOS these three are in the application menu, at the top of the
    // screen. Anywhere else this menu is the only one there is, and About is
    // not optional: it is where the licence terms and the "Powered by
    // Tracktion Engine" line are shown.
   #if ! JUCE_MAC
    menu.addSeparator();
    menu.addItem (settingsId, "Settings...");
    menu.addItem (aboutId, "About Carve DAW");
    menu.addItem (noticesId, "Third-Party Notices...");
   #endif

    forThisPlatform (menu).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (logoButton),
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
            case analyzerId:  if (safe->onOpenAnalyzer) safe->onOpenAnalyzer(); break;
            case settingsId:  if (safe->onSettings) safe->onSettings(); break;
            case aboutId:     if (safe->onAbout)    safe->onAbout();    break;
            case noticesId:   if (safe->onNotices)  safe->onNotices();  break;
            case soundLibraryId: if (safe->onSoundLibrary) safe->onSoundLibrary(); break;
            case clearRecentId:  if (safe->onClearRecent)  safe->onClearRecent();  break;

            default:
                if (result >= firstSampleSongId)
                {
                    if (safe->onOpenSampleSong != nullptr)
                        safe->onOpenSampleSong (result - firstSampleSongId);
                }
                else if (result >= firstRecentId && safe->onOpenRecent != nullptr)
                {
                    safe->onOpenRecent (result - firstRecentId);
                }

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
    documentLabel.setText (documentName, juce::dontSendNotification);
    documentLabel.setTooltip (hasUnsavedChanges ? documentName + " (unsaved changes)" : documentName);

    if (unsaved != hasUnsavedChanges)
    {
        unsaved = hasUnsavedChanges;
        repaint (documentPanel);
    }
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
    return song.toBarsAndBeats (wantedLoopRange().getStart().inBeats()).bar + 1;
}

int TransportBar::getLoopLengthBars() const
{
    // The bar the end falls in, counted from the start bar; a hair before the
    // end so a range ending exactly on a bar line does not count the next one.
    const auto end = wantedLoopRange().getEnd().inBeats();
    const int endBar = song.toBarsAndBeats (std::max (0.0, end - 1.0e-6)).bar + 1;
    return std::max (1, endBar - getLoopStartBar() + 1);
}

te::BeatRange TransportBar::wantedLoopRange() const
{
    if (isPatternMode())
        return patternLoopRange();

    if (song.hasLoopRange())
        return { te::BeatPosition::fromBeats (song.getLoopStart()), te::BeatPosition::fromBeats (song.getLoopEnd()) };

    return { te::BeatPosition(), te::BeatPosition::fromBeats (std::max (4.0, getSongLengthBeats())) };
}

void TransportBar::syncLoopRange (bool wrapPlayhead)
{
    auto& transport = edit.getTransport();

    // Pattern mode always loops; Song mode as the LOOP box says.
    const bool shouldLoop = songLoopChecked || isPatternMode();

    if (transport.looping.get() != shouldLoop)
        transport.looping = shouldLoop;

    const auto wanted = edit.tempoSequence.toTime (wantedLoopRange());
    const auto current = transport.getLoopRange();

    if (std::abs ((current.getStart() - wanted.getStart()).inSeconds()) < 1.0e-6
         && std::abs ((current.getEnd() - wanted.getEnd()).inSeconds()) < 1.0e-6)
        return;

    transport.setLoopRange (wanted);

    // A loop that just moved can leave the playhead past its end; wrap it
    // back the way the loop would have, rather than letting it run on.
    if (wrapPlayhead && shouldLoop && transport.isPlaying() && transport.getPosition() >= wanted.getEnd())
        transport.setPosition (wanted.getStart());
}

void TransportBar::setLoopBars (int startBar, int lengthBars)
{
    if (isPatternMode())
        return;   // the fields show the patterns' loop, which is not the song's to set

    startBar = std::max (1, startBar);
    lengthBars = std::max (1, lengthBars);

    undoManager.beginNewTransaction();
    song.setLoopRange (song.beatOfBar (startBar - 1), song.beatOfBar (startBar - 1 + lengthBars), &undoManager);

    // Now rather than on the next tick, so a running loop picks it up
    // rather than finishing the old one first.
    syncLoopRange (true);
}

void TransportBar::setMasterVolumeFromKnob()
{
    // The model is the source of truth: the undo manager sees it, the mixer's
    // master strip reads it back, and EditSync pushes it into the Edit.
    song.getMasterBus().setVolumeDb (masterKnobToDb (masterKnob.getValue()),
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

    // Pattern mode loops the patterns alone, from beat zero, and always
    // loops -- a one-shot of a bar is not what the mode is for.
    if (isPatternMode())
        transport.setPosition (te::TimePosition());

    syncLoopRange (false);
    transport.ensureContextAllocated();
    transport.play (false);
}

void TransportBar::toggleRecord()
{
    if (! recordButton.isEnabled())
        return;

    const bool wanted = ! recording;

    // Recording needs the transport moving, so pressing record starts it --
    // the way it does on anything with a record button on it.
    if (wanted && ! edit.getTransport().isPlaying())
        togglePlay();

    if (onRecordToggled != nullptr)
        onRecordToggled (wanted);
}

void TransportBar::setRecording (bool isRecording)
{
    recording = isRecording;
    recordButton.setToggleState (recording, juce::dontSendNotification);
}

void TransportBar::setRecordAvailable (bool available)
{
    recordButton.setEnabled (available);
}

void TransportBar::setCaptureAvailable (bool available)
{
    captureButton.setEnabled (available);
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
    // Reached on every model change while the mode is on -- each note drawn
    // -- and cheap: an unchanged range is left alone.
    syncLoopRange (true);
}

void TransportBar::timerCallback()
{
    carve::perf::Scope perfProbe ("transport.timer");
    auto& transport = edit.getTransport();
    const bool playing = transport.isPlaying();

    playButton.setIcon (playing ? Icon::pause : Icon::play);
    playButton.setToggleState (playing, juce::dontSendNotification);

    bpmDisplay.setText (juce::String (song.getTempo(), 1));
    barDisplay.setText (padded (getCurrentBar(), 3));
    // The transport first, so what the fields say is what it is looping.
    syncLoopRange (true);
    loopStartDisplay.setText (padded (getLoopStartBar(), 3));
    loopLengthDisplay.setText (padded (getLoopLengthBars(), 3));

    // Pattern mode's loop is the patterns' length, always on: shown, not set.
    const bool songLoop = ! isPatternMode();
    loopStartDisplay.setEnabled (songLoop);
    loopLengthDisplay.setEnabled (songLoop);
    loopCheck.setEnabled (songLoop);
    loopCheck.setToggleState (songLoop ? songLoopChecked : true, juce::dontSendNotification);

    undoButton.setEnabled (undoManager.canUndo());
    redoButton.setEnabled (undoManager.canRedo());

    // Nothing tells us when the master volume moves -- there is no model
    // listener here -- so it is read back every tick, unless the knob is
    // the thing moving it.
    if (! masterKnob.isMouseButtonDown())
        masterKnob.setValue (dbToMasterKnob (song.getMasterBus().getVolumeDb()),
                             juce::dontSendNotification);

    // The level at the end of the graph, which only exists while there is a
    // playback context to render it.
    auto* context = edit.getCurrentPlaybackContext();
    masterMeter.update (context != nullptr ? &context->masterLevels : nullptr);
}

//==============================================================================
namespace
{
    void paintPanels (juce::Graphics& g, const std::vector<juce::Rectangle<int>>& panels,
                      const std::vector<std::pair<juce::Rectangle<int>, juce::String>>& captions)
    {
        for (const auto& panel : panels)
        {
            g.setColour (panelFill);
            g.fillRoundedRectangle (panel.toFloat(), 5.0f);
            g.setColour (panelEdge);
            g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 5.0f, 1.0f);
        }

        g.setFont (uiFont (fonts::small, juce::Font::bold));

        for (const auto& [area, text] : captions)
        {
            g.setColour (captionColour);
            g.drawText (text, area, juce::Justification::centredLeft);
        }
    }

    // The strip's measurements: the playlist toolbar's height, with the same
    // padding as its buttons.
    constexpr int stripHeight = toolStrip::height;
    constexpr int stripButtonHeight = toolStrip::buttonHeight;
    constexpr int playModeWidth = 116;
    constexpr int transportButtonWidth = 38;   // room for a finger: a stop pressed for a record is a lost take
    constexpr int numTransportButtons = 6;
    constexpr int stripGap = 10;
    constexpr int captionWidth = 34;    // "BPM", "BAR"
    constexpr int loopCheckWidth = 64;

    // "for", between the loop's start and length: measured, since the
    // touch build's fonts are larger than the desktop's.
    int forWidth()
    {
        return juce::GlyphArrangement::getStringWidthInt (uiFont (fonts::small, juce::Font::bold), "for") + 10;
    }
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (barBackground);

    // The document: its name in a panel of its own, with a lamp lit while
    // there are unsaved changes, and Save beside it.
    if (! documentPanel.isEmpty())
    {
        paintPanels (g, { documentPanel }, {});

        const auto lamp = juce::Rectangle<float> (8.0f, 8.0f)
                              .withCentre ({ (float) documentPanel.getX() + 12.0f, (float) documentPanel.getCentreY() });
        g.setColour (unsaved ? accent : juce::Colour (0xff3a3a40));
        g.fillEllipse (lamp);
    }

    g.setColour (juce::Colour (0xff17171a));
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void TransportBar::Strip::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1b1f));   // the playlist toolbar's
    paintPanels (g, panels, captions);
}

void TransportBar::resized()
{
    constexpr int meterWidth = 96;
    constexpr int saveWidth = 70;

    auto area = getLocalBounds().reduced (6, 5);
    const int h = area.getHeight();

    logoButton.setBounds (area.removeFromLeft (52).withSizeKeepingCentre (52, h));
    area.removeFromLeft (10);

    // The right-hand group first, so whatever is left is the document's.
    masterMeter.setVisible (area.getWidth() > 600);

    if (masterMeter.isVisible())
    {
        masterMeter.setBounds (area.removeFromRight (meterWidth).withSizeKeepingCentre (meterWidth, 14));
        area.removeFromRight (6);
    }

    masterKnob.setBounds (area.removeFromRight (34).withSizeKeepingCentre (34, 34));
    area.removeFromRight (12);
    mixerButton.setBounds (area.removeFromRight (76).withSizeKeepingCentre (76, 28));
    area.removeFromRight (6);
    browserButton.setBounds (area.removeFromRight (90).withSizeKeepingCentre (90, 28));
    area.removeFromRight (16);

    // Undo and redo stay by the document they undo.
    auto left = area;
    const int docWidth = juce::jlimit (160, 460, left.getWidth() - 2 * 28 - 12);
    documentPanel = left.removeFromLeft (docWidth).withSizeKeepingCentre (docWidth, 32);
    left.removeFromLeft (8);
    undoButton.setBounds (left.removeFromLeft (28).withSizeKeepingCentre (28, 28));
    left.removeFromLeft (2);
    redoButton.setBounds (left.removeFromLeft (28).withSizeKeepingCentre (28, 28));

    auto doc = documentPanel.reduced (4, 2);
    doc.removeFromLeft (16);   // the unsaved lamp
    saveButton.setBounds (doc.removeFromRight (saveWidth).withSizeKeepingCentre (saveWidth, 26));
    doc.removeFromRight (6);
    documentLabel.setBounds (doc);

    repaint();
}

int TransportBar::getTransportStripWidth() const
{
    return 6 + playModeWidth + stripGap
           + numTransportButtons * transportButtonWidth + stripGap
           + (6 + captionWidth + barDisplay.getWidth() + 6) + 6
           + (6 + captionWidth + bpmDisplay.getWidth() + 6) + 6
           + (6 + loopCheckWidth + loopStartDisplay.getWidth() + forWidth() + loopLengthDisplay.getWidth() + 6)
           + 6;
}

void TransportBar::layOutStrip()
{
    auto& panels = strip.panels;
    auto& captions = strip.captions;
    panels.clear();
    captions.clear();

    auto area = strip.getLocalBounds().withSizeKeepingCentre (strip.getWidth(), stripHeight).withTrimmedLeft (6);
    const auto row = [&area] (int width) { return area.removeFromLeft (width); };
    const auto centred = [] (juce::Rectangle<int> r, int w, int h) { return r.withSizeKeepingCentre (w, h); };

    playMode.setBounds (centred (row (playModeWidth), playModeWidth, stripButtonHeight));
    area.removeFromLeft (stripGap);

    for (auto* b : { &rewindButton, &playButton, &stopButton, &recordButton, &captureButton, &forwardButton })
        b->setBounds (centred (row (transportButtonWidth), transportButtonWidth, stripButtonHeight + 2));

    area.removeFromLeft (stripGap);

    auto panel = [&] (int width)
    {
        auto r = row (width);
        panels.push_back (centred (r, width, stripButtonHeight + 2));
        area.removeFromLeft (6);
        return r.reduced (6, 0);
    };

    {
        auto p = panel (6 + captionWidth + barDisplay.getWidth() + 6);
        captions.emplace_back (p.removeFromLeft (captionWidth), "BAR");
        barDisplay.setBounds (centred (p, barDisplay.getWidth(), 20));
    }

    {
        auto p = panel (6 + captionWidth + bpmDisplay.getWidth() + 6);
        captions.emplace_back (p.removeFromLeft (captionWidth), "BPM");
        bpmDisplay.setBounds (centred (p, bpmDisplay.getWidth(), 20));
    }

    // LOOP 001 for 002: from bar 1, two bars long.
    {
        auto p = panel (6 + loopCheckWidth + loopStartDisplay.getWidth() + forWidth() + loopLengthDisplay.getWidth() + 6);
        loopCheck.setBounds (p.removeFromLeft (loopCheckWidth));
        loopStartDisplay.setBounds (centred (p.removeFromLeft (loopStartDisplay.getWidth()), loopStartDisplay.getWidth(), 20));
        captions.emplace_back (p.removeFromLeft (forWidth()).withTrimmedLeft (4), "for");
        loopLengthDisplay.setBounds (centred (p.removeFromLeft (loopLengthDisplay.getWidth()), loopLengthDisplay.getWidth(), 20));
    }

    strip.repaint();
}

} // namespace carve::app
