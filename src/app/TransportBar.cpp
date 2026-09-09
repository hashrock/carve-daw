#include "TransportBar.h"

namespace carve::app
{

TransportBar::TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um)
    : edit (editToControl), song (std::move (songModel)), undoManager (um)
{
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this]
    {
        auto& transport = edit.getTransport();
        transport.stop (false, false);
        transport.setPosition (te::TimePosition());
    };

    mixerButton.onClick = [this] { if (onOpenMixer) onOpenMixer(); };

    browserButton.setClickingTogglesState (false);   // MainComponent sets the state
    browserButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff35608a));
    browserButton.setTooltip ("Sample browser (cmd-B)");
    browserButton.onClick = [this] { if (onToggleBrowser) onToggleBrowser(); };
    logoButton.onClick = [this] { showFileMenu(); };

    undoButton.onClick = [this] { undoManager.undo(); };
    redoButton.onClick = [this] { undoManager.redo(); };
    undoButton.setTooltip ("Undo (cmd-Z)");
    redoButton.setTooltip ("Redo (shift-cmd-Z)");

    // Loop, undo and redo are symbols on the bar rather than boxes: loop lit
    // in the accent colour while it is on, which is what a toggle looks like
    // when it has no frame to fill.
    for (auto* b : { &loopButton, &undoButton, &redoButton })
        b->setFlat (true);

    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xffe08a3c));
    loopButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff8a8a94));
    loopButton.setToggleState (true, juce::dontSendNotification);
    loopButton.setTooltip ("Loop playback");

    bpmLabel.setEditable (false, true, false);
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.onTextChange = [this]
    {
        // Still checked here rather than left to setTempo's clamp: text that
        // parses to nothing reads as 0, and clamping that to the slowest
        // tempo the model allows is not what a typo means.
        const auto bpm = bpmLabel.getText().getDoubleValue();
        if (bpm >= model::TempoChange::minBpm && bpm <= model::TempoChange::maxBpm)
        {
            undoManager.beginNewTransaction();
            song.setTempo (bpm, &undoManager);
        }
    };

    positionLabel.setJustificationType (juce::Justification::centredLeft);

    documentLabel.setJustificationType (juce::Justification::centredRight);
    documentLabel.setMinimumHorizontalScale (1.0f);   // ellipsis, not squashed text
    documentLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

    for (auto* c : std::initializer_list<juce::Component*> {
             &logoButton, &playButton, &stopButton, &loopButton, &undoButton, &redoButton,
             &bpmLabel, &positionLabel, &browserButton, &mixerButton, &documentLabel })
        addAndMakeVisible (c);

    startTimerHz (15);
}

void TransportBar::showFileMenu()
{
    enum { openId = 1, saveId, saveAsId, exportId };

    juce::PopupMenu menu;
    menu.addItem (openId, "Open...");
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
            case openId:    if (safe->onOpen)   safe->onOpen();   break;
            case saveId:    if (safe->onSave)   safe->onSave();   break;
            case saveAsId:  if (safe->onSaveAs) safe->onSaveAs(); break;
            case exportId:  if (safe->onExport) safe->onExport(); break;
            default: break;
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

void TransportBar::togglePlay()
{
    auto& transport = edit.getTransport();

    if (transport.isPlaying())
    {
        transport.stop (false, false);
        return;
    }

    // The playlist ruler can set an explicit loop range; without one, loop the
    // whole song as playback always did.
    const auto loop = song.hasLoopRange()
                          ? te::BeatRange (te::BeatPosition::fromBeats (song.getLoopStart()),
                                           te::BeatPosition::fromBeats (song.getLoopEnd()))
                          : te::BeatRange (te::BeatPosition(),
                                           te::BeatPosition::fromBeats (
                                               std::max (4.0, getSongLengthBeats())));

    transport.setLoopRange (edit.tempoSequence.toTime (loop));
    transport.looping = loopButton.getToggleState();
    transport.ensureContextAllocated();
    transport.play (false);
}

void TransportBar::timerCallback()
{
    auto& transport = edit.getTransport();
    const bool playing = transport.isPlaying();

    playButton.setButtonText (playing ? "Pause" : "Play");
    playButton.setIcon (playing ? Icon::pause : Icon::play);

    if (! bpmLabel.isBeingEdited())
        bpmLabel.setText (juce::String (song.getTempo(), 1) + " BPM", juce::dontSendNotification);

    const auto position = transport.getPosition();
    const auto beats = edit.tempoSequence.toBeats (position).inBeats();
    positionLabel.setText (juce::String::formatted ("%d.%d | %.1fs",
                                                    (int) (beats / 4.0) + 1,
                                                    (int) std::fmod (beats, 4.0) + 1,
                                                    position.inSeconds()),
                           juce::dontSendNotification);

    undoButton.setEnabled (undoManager.canUndo());
    redoButton.setEnabled (undoManager.canRedo());
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (6, 6);
    auto place = [&area] (juce::Component& c, int width, int gap = 6)
    {
        c.setBounds (area.removeFromLeft (width));
        area.removeFromLeft (gap);
    };

    place (logoButton, area.getHeight() + 14, 10);

    // Play and Stop carry a symbol as well as their word; the three symbols
    // after them stand alone, so they sit tighter.
    place (playButton, 82);
    place (stopButton, 74, 10);
    place (loopButton, 30, 2);
    place (undoButton, 30, 2);
    place (redoButton, 30, 12);
    place (bpmLabel, 90);
    place (positionLabel, 110);
    place (browserButton, 90);
    place (mixerButton, 76);

    // whatever is left goes to the document name
    documentLabel.setBounds (area.withTrimmedLeft (12));
}

} // namespace carve::app
