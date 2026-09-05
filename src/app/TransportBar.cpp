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
    exportButton.onClick = [this] { if (onExport) onExport(); };

    undoButton.onClick = [this] { undoManager.undo(); };
    redoButton.onClick = [this] { undoManager.redo(); };
    saveButton.onClick = [this] { if (onSave) onSave(); };
    openButton.onClick = [this] { if (onOpen) onOpen(); };

    loopButton.setToggleState (true, juce::dontSendNotification);

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
             &playButton, &stopButton, &loopButton, &bpmLabel, &positionLabel,
             &mixerButton, &exportButton, &documentLabel,
             &undoButton, &redoButton, &saveButton, &openButton })
        addAndMakeVisible (c);

    startTimerHz (15);
}

void TransportBar::setSong (model::Song newSong)
{
    song = std::move (newSong);
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
    playButton.setButtonText (transport.isPlaying() ? "Pause" : "Play");

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

    place (playButton, 70);
    place (stopButton, 60);
    place (loopButton, 60);
    place (bpmLabel, 90);
    place (positionLabel, 110);

    place (mixerButton, 60);
    place (exportButton, 64);

    auto right = area;
    openButton.setBounds (right.removeFromRight (60));
    right.removeFromRight (6);
    saveButton.setBounds (right.removeFromRight (60));
    right.removeFromRight (18);
    redoButton.setBounds (right.removeFromRight (60));
    right.removeFromRight (6);
    undoButton.setBounds (right.removeFromRight (60));

    // whatever is left in the middle
    right.removeFromRight (12);
    documentLabel.setBounds (right);
}

} // namespace carve::app
