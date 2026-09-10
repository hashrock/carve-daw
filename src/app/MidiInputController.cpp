#include "MidiInputController.h"

#include "sync/EngineIds.h"

namespace carve::app
{

namespace
{
    // Shorter than this is a mistake rather than a note: the same floor the
    // model puts under a drawn one.
    constexpr double minNoteLengthBeats = model::Note::minLengthBeats;

    // The physical MIDI inputs this app has already offered to switch on,
    // kept beside the rest of the app's settings (see EngineSetup.h). It is
    // what tells "a keyboard nobody has ever seen" from "a keyboard somebody
    // switched off".
    constexpr const char* seenMidiInputsKey = "midiInputsSeen";

    int toVelocity (float normalised)
    {
        return juce::jlimit (1, 127, juce::roundToInt (normalised * 127.0f));
    }
} // namespace

MidiInputController::MidiInputController (te::Engine& engineToUse, te::Edit& editToUse,
                                          model::Song songToUse, juce::UndoManager& um)
    : engine (engineToUse), edit (editToUse), song (std::move (songToUse)), undoManager (um)
{
    engine.getDeviceManager().addChangeListener (this);
    refreshDevices();

    // Slow: this is here to notice the transport stopping under a recording,
    // not to follow the playhead.
    startTimer (200);
}

MidiInputController::~MidiInputController()
{
    stopTimer();
    cancelPendingUpdate();
    engine.getDeviceManager().removeChangeListener (this);
    stopListening();
}

void MidiInputController::setSong (model::Song newSong)
{
    // Nothing recorded into the old song's patterns from here on, and no
    // note left hanging on its way out.
    finishHeldNotes (lastPlayheadBeat);
    setRecording (false);

    song = std::move (newSong);
}

void MidiInputController::setTarget (const juce::String& generatorId, const juce::String& patternId)
{
    targetGeneratorId = generatorId;
    targetPatternId = patternId;
}

void MidiInputController::setPatternMode (bool shouldUsePatternMode)
{
    patternMode = shouldUsePatternMode;
}

void MidiInputController::setRecording (bool shouldRecord)
{
    if (recording == shouldRecord)
        return;

    recording = shouldRecord;

    if (recording)
    {
        // One transaction for the take, so undo takes the pass rather than
        // the last note of it.
        undoManager.beginNewTransaction ("Record MIDI");
    }
    else
    {
        finishHeldNotes (lastPlayheadBeat);
    }

    if (onRecordingChanged != nullptr)
        onRecordingChanged (recording);
}

//==============================================================================
// Devices

void MidiInputController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshDevices();
}

void MidiInputController::refreshDevices()
{
    auto wanted = engine.getDeviceManager().getMidiInDevices();

    // Physical devices only. A virtual one (tracktion's "all MIDI ins") never
    // sees the driver callback that feeds a keyboard state, so listening to
    // it would be listening to nothing -- and listening to both would record
    // every note twice.
    std::erase_if (wanted, [] (const std::shared_ptr<te::MidiInputDevice>& device)
    {
        return device == nullptr
                || device->getDeviceType() != te::InputDevice::physicalMidiDevice;
    });

    // A device the app has never seen is switched on, once. Opening a MIDI
    // input is not the audio input this app deliberately leaves shut (see
    // EngineSetup.h) -- a different driver and a different bug -- and a
    // keyboard just plugged in should play without a trip to the settings.
    // After that the setting is the user's: Settings > MIDI input turns them
    // off and on, and this follows.
    auto& properties = engine.getPropertyStorage().getPropertiesFile();
    auto seen = juce::StringArray::fromTokens (properties.getValue (seenMidiInputsKey), "\n", "");
    bool seenChanged = false;

    for (auto& device : wanted)
    {
        if (seen.contains (device->getDeviceID()))
            continue;

        seen.add (device->getDeviceID());
        seenChanged = true;

        if (! device->isEnabled())
            device->setEnabled (true);   // asks the engine for a rescan, asynchronously
    }

    if (seenChanged)
        properties.setValue (seenMidiInputsKey, seen.joinIntoString ("\n"));

    // Only the ones that are on: a device switched off in the settings is one
    // this must not be holding a listener on.
    std::erase_if (wanted, [] (const std::shared_ptr<te::MidiInputDevice>& device)
    {
        return ! device->isEnabled();
    });

    for (auto& device : listeningTo)
        if (std::find (wanted.begin(), wanted.end(), device) == wanted.end())
            device->keyboardState.removeListener (this);

    for (auto& device : wanted)
        if (std::find (listeningTo.begin(), listeningTo.end(), device) == listeningTo.end())
            device->keyboardState.addListener (this);

    listeningTo = std::move (wanted);
}

void MidiInputController::stopListening()
{
    for (auto& device : listeningTo)
        device->keyboardState.removeListener (this);

    listeningTo.clear();
}

//==============================================================================
// The MIDI thread

void MidiInputController::handleNoteOn (juce::MidiKeyboardState*, int, int midiNoteNumber, float velocity)
{
    push ({ true, midiNoteNumber, toVelocity (velocity), juce::Time::getMillisecondCounterHiRes() });
}

void MidiInputController::handleNoteOff (juce::MidiKeyboardState*, int, int midiNoteNumber, float)
{
    push ({ false, midiNoteNumber, 0, juce::Time::getMillisecondCounterHiRes() });
}

void MidiInputController::push (Event event)
{
    {
        const juce::ScopedLock sl (eventLock);

        // A cap, because this queue is drained by the message thread and the
        // message thread can be busy: a stuck note is better than unbounded
        // growth behind a modal dialog.
        if (events.size() > 256)
            return;

        events.push_back (event);
    }

    triggerAsyncUpdate();
}

//==============================================================================
// The message thread

void MidiInputController::handleAsyncUpdate()
{
    std::vector<Event> pending;

    {
        const juce::ScopedLock sl (eventLock);
        pending.swap (events);
    }

    if (pending.empty())
        return;

    // Straight off the playhead rather than off the transport's copy of it,
    // which is only refreshed for the UI.
    double playheadSeconds = -1.0;

    if (auto* context = edit.getCurrentPlaybackContext())
        playheadSeconds = context->getPosition().inSeconds();

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();

    for (const auto& event : pending)
        handleEvent (event, nowMs, playheadSeconds);
}

void MidiInputController::handleEvent (const Event& event, double nowMs, double playheadSeconds)
{
    sound (event.pitch, event.velocity, event.isNoteOn);

    const bool rolling = edit.getTransport().isPlaying() && playheadSeconds >= 0.0;

    // Back to where it was played: the playhead as it is now, less however
    // long the event waited to be looked at.
    const auto seconds = playheadSeconds - (nowMs - event.wallClockMs) * 0.001;
    const auto beat = rolling
                          ? edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (juce::jmax (0.0, seconds))).inBeats()
                          : lastPlayheadBeat;

    if (rolling)
        lastPlayheadBeat = beat;

    if (event.isNoteOn)
    {
        held.push_back ({ event.pitch, event.velocity, beat, recording && rolling });
        return;
    }

    for (auto it = held.begin(); it != held.end(); ++it)
    {
        if (it->pitch != event.pitch)
            continue;

        if (it->recorded)
            writeNote (*it, beat);

        held.erase (it);
        return;
    }
}

void MidiInputController::timerCallback()
{
    if (! recording)
        return;

    if (auto* context = edit.getCurrentPlaybackContext(); context != nullptr && edit.getTransport().isPlaying())
    {
        lastPlayheadBeat = edit.tempoSequence.toBeats (context->getPosition()).inBeats();
        return;
    }

    // The transport stopped under the recording -- by the stop button, or by
    // running off the end of a song that is not looping.
    setRecording (false);
}

//==============================================================================
te::AudioTrack* MidiInputController::targetTrack() const
{
    if (targetGeneratorId.isEmpty())
        return nullptr;

    const auto generators = song.getGenerators();

    // Generator order == track order, returns after them (the EditSync
    // invariant the note preview leans on too).
    juce::Array<te::AudioTrack*> tracks;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isReturnTrack (*track))
            tracks.add (track);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        if (generators[(size_t) i].getId() == targetGeneratorId)
            return tracks[i];

    return nullptr;
}

void MidiInputController::sound (int pitch, int velocity, bool noteOn)
{
    auto* track = targetTrack();

    if (track == nullptr)
        return;

    // Live MIDI needs somewhere to go: there is no playback context until the
    // transport has been started once.
    edit.getTransport().ensureContextAllocated();

    // Not playGuideNote: that one keeps a list of what it started and can only
    // end the lot of them at once, which is no way to play a keyboard. This is
    // the path underneath it, one message per key, so notes end as they are
    // let go and holding one chord while playing another works.
    track->injectLiveMidiMessage (noteOn ? juce::MidiMessage::noteOn (1, pitch, (juce::uint8) velocity)
                                         : juce::MidiMessage::noteOff (1, pitch),
                                  {});
}

//==============================================================================
std::optional<MidiInputController::Destination> MidiInputController::destinationFor (double editBeat) const
{
    const auto generator = song.findGenerator (targetGeneratorId);

    if (! generator)
        return {};

    if (patternMode)
    {
        const auto pattern = generator->findPattern (targetPatternId);

        if (! pattern)
            return {};

        const auto length = pattern->getLengthBeats();

        if (length <= 0.0)
            return {};

        // Pattern mode starts every pattern at beat zero and repeats it to
        // fill the audition loop, so the pattern's own time is the transport's
        // wrapped into it.
        return Destination { *pattern, std::fmod (juce::jmax (0.0, editBeat), length), length, 0 };
    }

    for (const auto& clip : song.getPlaylist().getClips())
    {
        if (clip.getGeneratorId() != targetGeneratorId)
            continue;

        const auto pattern = generator->findPattern (clip.getPatternId());

        if (! pattern)
            continue;

        const auto patternLength = pattern->getLengthBeats();
        const auto start = clip.getStart();
        const auto end = start + clip.getLength (patternLength);

        if (editBeat < start || editBeat >= end || patternLength <= 0.0)
            continue;

        // A placement longer than its pattern repeats it, so the offset wraps
        // the same way pattern mode's does. The transpose comes back off: the
        // note is written as the pattern's, and the placement puts it back up
        // where it was played.
        return Destination { *pattern, std::fmod (editBeat - start, patternLength),
                             patternLength, clip.getTranspose() };
    }

    // Nothing of this generator's is playing here. Dropping the note is the
    // only honest answer: there is no pattern for it to be in.
    return {};
}

void MidiInputController::writeNote (const Held& note, double endBeat)
{
    auto destination = destinationFor (note.startBeat);

    if (! destination)
        return;

    auto length = endBeat - note.startBeat;

    // Held across the loop, so the end came back round before the start.
    if (length <= 0.0)
        length += destination->patternLength;

    length = juce::jlimit (minNoteLengthBeats, destination->patternLength, length);

    const auto pitch = juce::jlimit (0, 127, note.pitch - destination->transpose);

    destination->pattern.addNote (destination->beat, length, pitch, note.velocity, &undoManager);
}

void MidiInputController::finishHeldNotes (double endBeat)
{
    // The keys may well still be down, and a key still down is still a note
    // being played: what ends here is only the recording of it. The release
    // that follows still sounds, because sounding does not go through this
    // list at all.
    for (auto& note : held)
    {
        if (note.recorded)
            writeNote (note, endBeat);

        note.recorded = false;
    }
}

} // namespace carve::app
