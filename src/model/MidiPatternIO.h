#pragma once

#include <optional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "SongModel.h"

// One pattern out to a standard MIDI file, and one back in.
//
// A beat in this model is a quarter note, which is exactly what a MIDI file's
// time division counts, so the whole conversion is a division by
// ticks-per-quarter-note. Tempo does not enter into it: the tempo written into
// an exported file is there for whatever opens it next, and the tempo in an
// imported one is ignored, because neither changes where a note sits in beats.
//
// Deliberately one pattern at a time rather than a whole song. A pattern
// belongs to one generator, and a MIDI file says nothing about which
// instrument should play it -- so importing a multi-track file as several
// generators would be guessing at every step. All of a file's tracks are
// merged into the one pattern instead; see readFile.
namespace carve::model::midiio
{

// What exports are written with. 960 divides every note value the piano roll
// can draw -- down to a 1/64 triplet -- into whole ticks, so a pattern that
// goes out comes back at exactly the same beats.
inline constexpr short ticksPerQuarterNote = 960;

// The customary extension and wildcard, so every chooser in the app offers the
// same thing.
inline const char* const fileWildcard = "*.mid;*.midi";

// Writes the pattern as a single-track (format 0) MIDI file: its notes, the
// song's tempo and time signature at the head, and an end-of-track at the
// pattern's length so the length survives the trip too. False if the file
// could not be written.
bool writePattern (const Pattern&, const juce::File& destination,
                   double tempoBpm, TimeSignature);

// A MIDI file read into this model's terms, before anything has been written
// to the tree. Kept as a separate step so the caller can report what it got
// (and how many tracks it merged) before committing to a slot.
struct ImportedPattern
{
    struct ImportedNote
    {
        double start = 0.0;      // beats from the start of the file
        double length = 0.0;     // beats
        int pitch = 60;
        int velocity = 100;
    };

    std::vector<ImportedNote> notes;

    // The file's end-of-track, which is where an export of ours puts the
    // pattern length. Never less than the end of the last note.
    double lengthBeats = 0.0;

    // How many tracks the file held. All of them are merged into the notes
    // above; this is only so the UI can say so.
    int numTracks = 0;

    // The file's own name, which is the only thing a MIDI file reliably offers
    // to name a pattern after.
    juce::String name;
};

// Empty for anything that is not a readable MIDI file, so a chooser that
// hands back the wrong thing changes nothing.
std::optional<ImportedPattern> readFile (const juce::File& source);

// Writes an import into a pattern, replacing every note it held. The pattern
// grows to a whole number of bars of `beatsPerBar` if the import runs past its
// current length -- dropping notes that did not fit would be the one thing an
// import must never do quietly -- but never shrinks, so importing into a slot
// sized for a section leaves it that size.
void applyToPattern (const ImportedPattern&, Pattern&, double beatsPerBar, juce::UndoManager*);

} // namespace carve::model::midiio
