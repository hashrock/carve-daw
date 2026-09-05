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
// refuses anything else -- and strength and swing are both 0..1, clamped where
// they are set.
struct QuantiseSettings
{
    double gridBeats = 0.25;
    double strength = 1.0;
    double swing = 0.0;
};

// Where quantise puts one note.
//
// Strength 0 leaves the note alone and 1 puts it exactly on the grid; swing
// then pushes every other division late, landing the off-beat a third of a
// division after the on-beat at 1.0, which is the triplet feel a swung eighth
// is written as.
inline double quantisedStart (double start, double lengthBeats,
                              const QuantiseSettings& settings, double patternLengthBeats)
{
    // Which division of the grid the note belongs to. floor(x + 0.5) rather
    // than round() so that a note lying exactly between two divisions always
    // goes to the later one instead of away from zero.
    const auto division = std::floor (start / settings.gridBeats + 0.5);

    const auto swingOffset = std::fmod (division, 2.0) > 0.5
                                 ? settings.swing * settings.gridBeats / 3.0
                                 : 0.0;
    const auto target = division * settings.gridBeats + swingOffset;

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
