#include "MidiPatternIO.h"

namespace carve::model::midiio
{

namespace
{
    // Beats are doubles all the way through, so every comparison against a bar
    // or a note end needs a hair of slack.
    constexpr double beatEpsilon = 1.0e-6;

    // Nothing may be written with no duration at all: a zero-length note is
    // one the piano roll cannot grab and the engine never sounds.
    constexpr double minNoteLengthBeats = 1.0 / 64.0;

    // What a note-on with no note-off gets. A sixteenth is short enough to
    // read as the broken note it came from rather than to drone over the
    // pattern -- and files with hanging notes are broken, not unusual.
    constexpr double unmatchedNoteLengthBeats = 0.25;

    constexpr int trackNameMetaEventType = 3;

    // Exports go out on channel 1. A pattern plays one generator, so there is
    // nothing for the other fifteen channels to mean.
    constexpr int exportChannel = 1;
} // namespace

bool writePattern (const Pattern& pattern, const juce::File& destination,
                   double tempoBpm, TimeSignature timeSig)
{
    juce::MidiMessageSequence track;
    const auto ticks = [] (double beats) { return beats * (double) ticksPerQuarterNote; };

    // The meta events, all at tick 0. Nothing in this model reads any of them
    // back -- a beat is a quarter note whatever the tempo is -- but without
    // them the file opens at 120bpm in 4/4 wherever it lands next.
    track.addEvent (juce::MidiMessage::textMetaEvent (trackNameMetaEventType, pattern.getName()));
    track.addEvent (juce::MidiMessage::tempoMetaEvent (
                        (int) std::llround (60.0e6 / juce::jmax (1.0, tempoBpm))));
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (timeSig.numerator, timeSig.denominator));

    double lastNoteEnd = 0.0;

    for (const auto& note : pattern.getNotes())
    {
        const auto pitch = juce::jlimit (0, 127, note.getPitch());
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, note.getVelocity());
        const auto start = juce::jmax (0.0, note.getStart());
        const auto end = start + juce::jmax (minNoteLengthBeats, note.getLength());

        track.addEvent (juce::MidiMessage::noteOn (exportChannel, pitch, velocity), ticks (start));
        track.addEvent (juce::MidiMessage::noteOff (exportChannel, pitch), ticks (end));

        lastNoteEnd = juce::jmax (lastNoteEnd, end);
    }

    track.updateMatchedPairs();

    // The end of track is the pattern's length, not the last note's: a pattern
    // whose final bar is silent is still that long, and this is the only place
    // a MIDI file can say so. Never before the last note off, though -- writing
    // events past the end of track is what makes a file others refuse.
    track.addEvent (juce::MidiMessage::endOfTrack(),
                    ticks (juce::jmax (pattern.getLengthBeats(), lastNoteEnd)));

    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarterNote);
    file.addTrack (track);

    destination.deleteFile();
    juce::FileOutputStream stream (destination);

    if (! stream.openedOk())
        return false;

    if (! file.writeTo (stream, 0))
        return false;

    // The stream reports a write failure (a full disk, a read-only folder) only
    // once it has been flushed, so the status is checked after rather than
    // taking writeTo's word for it.
    stream.flush();
    return stream.getStatus().wasOk();
}

std::optional<ImportedPattern> readFile (const juce::File& source)
{
    juce::FileInputStream stream (source);

    if (! stream.openedOk())
        return std::nullopt;

    juce::MidiFile file;

    if (! file.readFrom (stream) || file.getNumTracks() == 0)
        return std::nullopt;

    // A positive time format is ticks per quarter note, which is what almost
    // every file uses and what converts to beats exactly. A negative one is
    // SMPTE -- frames per second, with no musical grid in it at all -- so
    // those go the long way round, through seconds and the file's own tempo,
    // which is the closest to a beat such a file has.
    double toBeats = 0.0;

    if (file.getTimeFormat() > 0)
    {
        toBeats = 1.0 / (double) file.getTimeFormat();
    }
    else
    {
        juce::MidiMessageSequence tempoEvents;
        file.findAllTempoEvents (tempoEvents);

        double bpm = 120.0;   // what a file that never states a tempo means by one

        if (auto* first = tempoEvents.getEventPointer (0))
            bpm = 60.0 / juce::jmax (1.0e-6, first->message.getTempoSecondsPerQuarterNote());

        file.convertTimestampTicksToSeconds();
        toBeats = bpm / 60.0;
    }

    ImportedPattern result;
    result.name = source.getFileNameWithoutExtension();
    result.numTracks = file.getNumTracks();

    // Every track merged into one. A Carve pattern plays a single generator, so
    // the alternative would be to invent a generator per track and guess which
    // instrument each wanted -- and a file exported from here has one track
    // anyway.
    juce::MidiMessageSequence merged;

    for (int i = 0; i < file.getNumTracks(); ++i)
    {
        const auto* track = file.getTrack (i);
        merged.addSequence (*track, 0.0);

        // getEndTime() counts the end-of-track meta event, which is where an
        // export of ours put the pattern length.
        result.lengthBeats = juce::jmax (result.lengthBeats, track->getEndTime() * toBeats);
    }

    merged.updateMatchedPairs();

    for (int i = 0; i < merged.getNumEvents(); ++i)
    {
        auto* event = merged.getEventPointer (i);
        const auto& message = event->message;

        // isNoteOn() is false for velocity 0, which is how half the world
        // writes a note off, so those never arrive here as notes.
        if (! message.isNoteOn())
            continue;

        const auto start = juce::jmax (0.0, message.getTimeStamp() * toBeats);
        const auto end = event->noteOffObject != nullptr
                             ? event->noteOffObject->message.getTimeStamp() * toBeats
                             : start + unmatchedNoteLengthBeats;

        // Channel is dropped on purpose: a pattern plays one instrument, and a
        // drum kit maps pads by note number, so the note is all that carries.
        result.notes.push_back ({ start,
                                  juce::jmax (minNoteLengthBeats, end - start),
                                  message.getNoteNumber(),
                                  juce::jlimit (1, 127, (int) message.getVelocity()) });

        result.lengthBeats = juce::jmax (result.lengthBeats, result.notes.back().start
                                                                 + result.notes.back().length);
    }

    return result;
}

void applyToPattern (const ImportedPattern& imported, Pattern& pattern,
                     double beatsPerBar, juce::UndoManager* um)
{
    pattern.clearNotes (um);

    const auto bar = juce::jmax (0.25, beatsPerBar);
    const auto barsNeeded = juce::jmax (1.0, std::ceil ((imported.lengthBeats - beatEpsilon) / bar));

    // Grows to hold the import, rounded up to a whole bar so the pattern still
    // lines up with the grid; never shrinks, because a slot the user sized for
    // a section should stay that size when a shorter phrase is dropped in.
    if (const auto length = barsNeeded * bar; length > pattern.getLengthBeats() + beatEpsilon)
        pattern.setLengthBeats (length, um);

    // Times stay absolute. Leading silence in the file is silence in the
    // pattern: shifting the first note to zero would move everything that came
    // in with it off the beat it was written on.
    for (const auto& note : imported.notes)
        pattern.addNote (note.start, note.length, note.pitch, note.velocity, um);
}

} // namespace carve::model::midiio
