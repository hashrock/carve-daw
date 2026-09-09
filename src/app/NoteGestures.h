#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>

// The arithmetic behind the piano roll's gestures that move notes: quantise,
// and the clamp a move or a paste runs its block of notes through.
//
// None of it needs a mouse event, an UndoManager or a ValueTree -- each is a
// function from (where the notes are, what the user asked for) to (where they
// should go), and the component's job around them is to read the notes, call
// these, and write the results back. So they live here instead, free of JUCE
// and of the model, which is what lets the test target reach them without
// linking a thing. See tests/NoteGestureProperties.cpp.

namespace carve::app::gestures
{

// One note as these gestures see it. Velocity is missing on purpose: nothing
// here moves a note anywhere because of how hard it was hit.
struct NoteSpan
{
    double start = 0.0;
    double length = 0.0;
    int pitch = 0;
};

// The four questions a gesture asks of the block it is moving. A group gesture
// is clamped against these rather than note by note, which is what keeps the
// block's shape when it runs into an end of the pattern or of the keyboard
// instead of collapsing everything onto the boundary.
struct Bounds
{
    double lowestStart = 0.0;
    double highestEnd = 0.0;
    int lowestPitch = 0;
    int highestPitch = 0;
};

// Anything with .start, .length and .pitch: the roll's own selection, or the
// notes a paste has just read off the clipboard.
template <typename Notes>
Bounds boundsOf (const Notes& notes)
{
    if (std::begin (notes) == std::end (notes))
        return {};

    const auto& first = *std::begin (notes);

    // highestEnd counts from zero rather than from the first note, so a block
    // sitting entirely to the left of the pattern reports an end of zero
    // rather than a negative one -- see clampMoveBeats for what that is for.
    Bounds bounds { first.start, 0.0, first.pitch, first.pitch };

    for (const auto& note : notes)
    {
        bounds.lowestStart = std::min (bounds.lowestStart, note.start);
        bounds.highestEnd = std::max (bounds.highestEnd, note.start + note.length);
        bounds.lowestPitch = std::min (bounds.lowestPitch, note.pitch);
        bounds.highestPitch = std::max (bounds.highestPitch, note.pitch);
    }

    return bounds;
}

// The stretch of keyboard the roll draws, which is what a move drag may not
// take a note off.
struct PitchRange
{
    int lowest = 0;
    int highest = 127;
};

// How quantise is set up: the grid the roll is drawing, and the two knobs over
// it. `gridBeats` is always positive -- PianoRollComponent::setGridBeats
// refuses anything else -- and strength is 0..1, clamped where it is set.
//
// Swing is on the scale every sequencer prints it on: the share of a pair of
// divisions that the first one takes. 0.5 is straight; 2/3 lands the off-beat
// a third of a division late, which is the triplet feel a swung eighth is
// written as; 0.75 is a dotted feel. Clamped to straightSwing..maxSwing where
// it is set: below 0.5 would be a swing that rushes, and at 1.0 the off-beat
// has been pushed onto the next on-beat and there is no pair left to swing.
struct QuantiseSettings
{
    double gridBeats = 0.25;
    double strength = 1.0;
    double swing = 0.5;

    static constexpr double straightSwing = 0.5;
    static constexpr double maxSwing = 1.0;
};

// The grid quantise aims at, once swing has moved every second division: the
// on-beat of the pair the note falls in, that pair's off-beat, and the on-beat
// of the next pair. Only these three can be nearest to a start inside the
// pair, so they are all a caller has to choose among.
//
// Measured against the swung positions rather than the straight ones and then
// offset: with the offset added afterwards, a note already sitting on a swung
// off-beat past the midpoint of its pair (any swing over 0.75) would be
// counted to the next on-beat and pulled straight again, so quantising a
// second time would undo the first.
struct SwungPair
{
    double onBeat;
    double offBeat;
    double nextOnBeat;
};

inline SwungPair swungPairAround (double start, const QuantiseSettings& settings)
{
    const auto period = 2.0 * settings.gridBeats;
    const auto onBeat = std::floor (start / period) * period;

    return { onBeat, onBeat + period * settings.swing, onBeat + period };
}

// Where quantise puts one note.
//
// Strength 0 leaves the note alone and 1 puts it exactly on the swung grid.
inline double quantisedStart (double start, double lengthBeats,
                              const QuantiseSettings& settings, double patternLengthBeats)
{
    const auto pair = swungPairAround (start, settings);

    // Nearest of the three, and the later one when the note lies exactly
    // between two, so that a note halfway never goes away from zero.
    auto target = pair.onBeat;

    for (const auto candidate : { pair.offBeat, pair.nextOnBeat })
        if (std::abs (candidate - start) <= std::abs (target - start))
            target = candidate;

    // Partial strength moves the note towards the grid rather than onto it,
    // which is what leaves a played-in part still sounding played in.
    auto newStart = std::max (0.0, start + settings.strength * (target - start));

    // A note that fitted inside the pattern stays inside it: swing on the last
    // division would otherwise push it past the end, where it is never heard.
    // A note already sitting outside is left alone rather than dragged back in
    // behind the user's back.
    const auto maxStart = std::max (0.0, patternLengthBeats - lengthBeats);

    if (start <= maxStart)
        newStart = std::min (newStart, maxStart);

    return newStart;
}

// How far a block of notes actually moves, in beats.
//
// The min/max against zero are what keep the two limits from crossing over: a
// block hanging off either end of the pattern -- notes left outside by a
// shorten, or a paste longer than the pattern it is landing in -- gives a
// negative headroom at that end, and without them the low limit could exceed
// the high one. With them, zero always sits between the two, so the clamp is
// well defined and a request of zero always answers zero.
inline double clampMoveBeats (const Bounds& bounds, double patternLengthBeats,
                              double requestedDelta)
{
    return std::clamp (requestedDelta,
                       std::min (0.0, -bounds.lowestStart),
                       std::max (0.0, patternLengthBeats - bounds.highestEnd));
}

// The same clamp on the pitch axis, against the keyboard rather than against
// the pattern's length.
inline int clampMovePitch (const Bounds& bounds, PitchRange keyboard, int requestedDelta)
{
    return std::clamp (requestedDelta,
                       std::min (0, keyboard.lowest - bounds.lowestPitch),
                       std::max (0, keyboard.highest - bounds.highestPitch));
}

} // namespace carve::app::gestures
