#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// Live MIDI in, and recording it.
//
// Two jobs, because the second is only useful with the first: what a keyboard
// plays reaches the selected generator's instrument, and while the transport
// is rolling with record armed, it also lands in the pattern that is playing
// there. Overdub, not replace -- what is already in the pattern stays, which
// is what makes recording over a loop work the way everybody expects.
//
// Where a note goes depends on the mode the app is in, and in both cases it
// is "wherever it was heard":
//
//  - Pattern mode auditions one pattern per generator from beat zero, so a
//    note recorded at transport beat B belongs at B modulo that pattern's
//    length;
//  - Song mode plays the playlist, so the note belongs in the pattern of the
//    clip under the playhead, at its offset into that clip -- with the clip's
//    transpose taken back off, so what is written down plays back as what was
//    played. A note over a gap in that generator's row has nowhere to go and
//    is dropped.
//
// Timing. The device callbacks arrive on the MIDI thread and everything they
// touch -- the model, the Edit, the live MIDI injection -- is message thread
// only, so each event is stamped with the wall clock where it lands and
// carried across. The message thread reads the playhead (which is a direct
// read, not the UI's copy of it) and subtracts the time the event spent
// waiting, which puts it back where it was played to within a millisecond or
// so rather than within a message-loop tick.
class MidiInputController : private juce::MidiKeyboardStateListener,
                            private juce::AsyncUpdater,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MidiInputController (te::Engine&, te::Edit&, model::Song, juce::UndoManager&);
    ~MidiInputController() override;

    void setSong (model::Song);

    // The generator that sounds what comes in, and the pattern a recording
    // lands in while the app is in pattern mode. Both come from the app's one
    // selection, so this is called wherever that moves.
    void setTarget (const juce::String& generatorId, const juce::String& patternId);

    void setPatternMode (bool);

    bool isRecording() const  { return recording; }

    // Arming and disarming. Recording needs the transport rolling: the caller
    // starts it, and this turns itself off when it stops.
    void setRecording (bool shouldRecord);

    // Whether anything is listening: false when the machine has no MIDI input
    // at all, which is what the record button greys itself out on.
    bool hasMidiInput() const  { return ! listeningTo.empty(); }

    // Fired when recording turns itself off, so the button can follow.
    std::function<void (bool recording)> onRecordingChanged;

private:
    // One key press or release, as it arrived.
    struct Event
    {
        bool isNoteOn = false;
        int pitch = 0;
        int velocity = 0;
        double wallClockMs = 0.0;
    };

    // A key that is down: where it started, so the release can write the note.
    struct Held
    {
        int pitch = 0;
        int velocity = 0;
        double startBeat = 0.0;   // edit beats, not pattern beats
        bool recorded = false;    // false when the press was not being recorded
    };

    // Where a note played at a given moment belongs.
    struct Destination
    {
        model::Pattern pattern { juce::ValueTree() };
        double beat = 0.0;        // into the pattern
        double patternLength = 1.0;
        int transpose = 0;
    };

    void handleNoteOn (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void refreshDevices();
    void stopListening();

    void push (Event);
    void handleEvent (const Event&, double nowMs, double playheadSeconds);

    te::AudioTrack* targetTrack() const;
    void sound (int pitch, int velocity, bool noteOn);

    std::optional<Destination> destinationFor (double editBeat) const;
    void writeNote (const Held&, double endBeat);
    void finishHeldNotes (double endBeat);

    te::Engine& engine;
    te::Edit& edit;
    model::Song song;
    juce::UndoManager& undoManager;

    juce::String targetGeneratorId, targetPatternId;
    bool patternMode = true;
    bool recording = false;

    // The devices being listened to, held so they outlive the listener
    // registration rather than only for as long as the device manager's list
    // happens to.
    std::vector<std::shared_ptr<te::MidiInputDevice>> listeningTo;

    juce::CriticalSection eventLock;
    std::vector<Event> events;

    std::vector<Held> held;

    // Where the playhead was the last time it was looked at, so a note still
    // down when the transport stops has an end to be given.
    double lastPlayheadBeat = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiInputController)
};

} // namespace carve::app
