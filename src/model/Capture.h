#pragma once

#include <optional>
#include <vector>

// Turning what was just played into a pattern, after the fact.
//
// Recording asks for a decision before the idea: arm, count in, play. A
// sketch goes the other way round -- the good take is the one played while
// nothing was listening -- so everything that comes in from a keyboard is
// kept, recording or not, and Capture makes a pattern of the last phrase of
// it. Nothing to arm, nothing to miss.
//
// This half is the arithmetic, kept free of the engine and of the MIDI thread
// so the properties can hold it to its word: which notes make "the last
// phrase", and where they land in the pattern made of them. The other half --
// stamping the notes as they arrive -- is MidiInputController's.
namespace carve::model::capture
{

// One note as it was played. The clock is beats, but a clock that only ever
// runs forward: a pattern-mode loop going round, or a transport that is not
// moving at all, would both fold "when" back on itself, so it counts beats of
// wall time at the tempo of the moment rather than reading the playhead.
struct PlayedNote
{
    double startBeat = 0.0;
    double endBeat = 0.0;
    int pitch = 60;
    int velocity = 100;

    // Where in its bar the note started, read off the playhead -- or nothing
    // when the transport was stopped and there were no bars to be in.
    std::optional<double> barPhase;
};

// The last phrase: the trailing run of notes with no stretch of silence of
// gapBeats or more inside it. Silence is measured from the end of everything
// sounding so far, so a long held note bridges whatever is played under it.
// Returns the notes in the order they were played.
std::vector<PlayedNote> lastPhrase (const std::vector<PlayedNote>& played, double gapBeats);

// A phrase laid out as a pattern. The phrase is in order of the starts, as
// lastPhrase hands it over.
struct CapturedPattern
{
    struct Note
    {
        double startBeat = 0.0;
        double lengthBeats = 0.0;
        int pitch = 60;
        int velocity = 100;
    };

    std::vector<Note> notes;
    double lengthBeats = 0.0;
};

// Where the phrase starts and how long the pattern is.
//
// Played over a running transport, the bars are the song's, so the phrase
// keeps its place in them: the pattern starts on the bar line before the
// first note, and the first note sits as far into it as it was played. Played
// over silence there are no bars to keep, so the first note is the downbeat.
//
// The pattern is whole bars, as many as it takes to hold every note's start,
// which is what makes the result loop the way it was played. Ends are not
// counted: the last note of a one-bar riff let go a moment past the bar line
// is still a one-bar riff, so a note that runs over is cut at the end.
CapturedPattern toPattern (const std::vector<PlayedNote>& phrase, double beatsPerBar);

} // namespace carve::model::capture
