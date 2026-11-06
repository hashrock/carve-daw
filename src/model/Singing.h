#pragma once

#include <algorithm>
#include <tuple>
#include <vector>

#include "SongModel.h"

// How a singing generator reads a pattern: in what order its notes are sung,
// and which of them carry straight on from the one before. One definition,
// because two places need the same answer -- the sync, which picks each
// note's recording by it, and the piano roll, which marks the notes nothing
// was recorded for. Had they each had their own, the roll would one day mark
// a note the singer sings, or miss one it cannot.
namespace carve::model::singing
{

// A note carries straight on from the one before when it starts where that
// one ends, or before. A gap of a 64th or more is a breath, and the note
// after it starts a phrase.
inline constexpr double joinBeats = 1.0 / 16.0;

inline bool followsOn (double previousStart, double previousLength, double start)
{
    return previousStart < start && previousStart + previousLength >= start - joinBeats;
}

// In the order they are sung: by start, a chord bottom up.
inline std::vector<Note> inSingingOrder (std::vector<Note> notes)
{
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b)
    {
        return std::make_tuple (a.getStart(), a.getPitch()) < std::make_tuple (b.getStart(), b.getPitch());
    });

    return notes;
}

// The lyric a note follows on from, or empty when it starts a phrase -- what
// Voicebank::resolve takes as its second argument. `ordered` is
// inSingingOrder's, and `index` the note's place in it.
inline juce::String previousLyricOf (const std::vector<Note>& ordered, size_t index)
{
    if (index == 0 || index >= ordered.size())
        return {};

    const auto& previous = ordered[index - 1];
    const auto& note = ordered[index];

    if (! followsOn (previous.getStart(), previous.getLength(), note.getStart()))
        return {};

    const auto lyric = previous.getLyric();
    return lyric.isNotEmpty() ? lyric : Note::getDefaultLyric();
}

} // namespace carve::model::singing
