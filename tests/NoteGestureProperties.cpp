// Properties of the piano roll's note-moving gestures: quantise, and the clamp
// a move drag or a paste runs its block of notes through.
//
// All of it lives in NoteGestures.h, pulled out of PianoRollComponent so that
// it can be reached without a mouse event. These are the places in the roll
// where a gesture decides *where a note ends up* rather than merely which note
// it is talking about, and each is arithmetic over a handful of interacting
// limits -- a grid, a swing offset, the end of the pattern, the ends of the
// keyboard. The combinations that break such a thing are not the ones anybody
// writes by hand.
//
// The functions under test are the ones the component calls, not a copy of
// them: quantiseNotes(), dragSelectionTo() and pasteNotes() now read the notes,
// call these, and write the results back, so a property here is a property of
// the gesture.
//
// Written against RapidCheck's rc::check(), like SongTimeProperties.cpp; see
// the header comment there for why not its Catch integration.

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "app/NoteGestures.h"

namespace
{

using carve::app::gestures::boundsOf;
using carve::app::gestures::clampMoveBeats;
using carve::app::gestures::clampMovePitch;
using carve::app::gestures::NoteSpan;
using carve::app::gestures::PitchRange;
using carve::app::gestures::QuantiseSettings;
using carve::app::gestures::quantisedStart;

// Beats land on a 1/32 grid over a few dozen bars, the same reasoning as in
// SongTimeProperties: arbitrary doubles would only buy floating-point noise
// that no gesture in the app can produce.
constexpr double gridUnit = 1.0 / 32.0;
constexpr int maxGridSteps = 1024;          // 32 beats

// The gestures are doubles all the way through, so every comparison needs a
// hair of slack.
constexpr double tolerance = 1.0e-9;

rc::Gen<double> genBeat()
{
    return rc::gen::map (rc::gen::inRange (0, maxGridSteps + 1),
                         [] (int steps) { return steps * gridUnit; });
}

// How far a gesture is asking to move, in either direction.
rc::Gen<double> genDelta()
{
    return rc::gen::map (rc::gen::pair (genBeat(), genBeat()),
                         [] (const std::pair<double, double>& beats)
                         { return beats.first - beats.second; });
}

// A note length is never zero: the roll's own floor is 1/32 of a beat.
rc::Gen<double> genLength()
{
    return rc::gen::map (rc::gen::inRange (1, 129),
                         [] (int steps) { return steps * gridUnit; });
}

// Wider than the toolbar's own list (1/4 down to 1/32, plus the two triplets):
// the grid is only ever read as a positive number of beats, so there is no
// reason to hold the properties to the six values the spinner happens to
// offer.
rc::Gen<double> genGrid()
{
    static constexpr std::array units { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1.0 / 32.0,
                                        1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0 };

    return rc::gen::map (rc::gen::inRange (0, (int) units.size()),
                         [] (int i) { return units[(std::size_t) i]; });
}

// Strength and swing are clamped to 0..1 where they are set, so anything
// outside would only be testing the clamp.
rc::Gen<double> genUnitInterval()
{
    return rc::gen::map (rc::gen::inRange (0, 101), [] (int n) { return n / 100.0; });
}

rc::Gen<QuantiseSettings> genQuantiseSettings()
{
    return rc::gen::map (rc::gen::tuple (genGrid(), genUnitInterval(), genUnitInterval()),
                         [] (const std::tuple<double, double, double>& t)
                         {
                             return QuantiseSettings { std::get<0> (t), std::get<1> (t),
                                                       std::get<2> (t) };
                         });
}

rc::Gen<double> genPatternLength()
{
    return rc::gen::map (rc::gen::inRange (1, 257), [] (int quarters) { return quarters * 0.25; });
}

// The keyboard, generated rather than repeating PianoRollComponent's C1..C7:
// which rows the roll happens to draw is not part of the clamp's contract, and
// generating the range holds it to any of them.
rc::Gen<PitchRange> genPitchRange()
{
    return rc::gen::map (rc::gen::pair (rc::gen::inRange (0, 128), rc::gen::inRange (0, 128)),
                         [] (const std::pair<int, int>& pair)
                         {
                             return PitchRange { std::min (pair.first, pair.second),
                                                 std::max (pair.first, pair.second) };
                         });
}

// The block a gesture is moving: at least one note, because both the drag and
// the paste return early on an empty one, so the clamp is never asked about it.
rc::Gen<std::vector<NoteSpan>> genBlock (PitchRange keyboard)
{
    const auto note = rc::gen::map (rc::gen::tuple (genBeat(), genLength(),
                                                    rc::gen::inRange (keyboard.lowest,
                                                                      keyboard.highest + 1)),
                                    [] (const std::tuple<double, double, int>& t)
                                    {
                                        return NoteSpan { std::get<0> (t), std::get<1> (t),
                                                          std::get<2> (t) };
                                    });

    return rc::gen::nonEmpty (rc::gen::container<std::vector<NoteSpan>> (note));
}

} // namespace

//==============================================================================
// Quantise

TEST_CASE ("Quantise at no strength changes nothing", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("strength 0 is the identity", [] {
        auto settings = *genQuantiseSettings();
        settings.strength = 0.0;

        const auto start = *genBeat();

        RC_ASSERT (quantisedStart (start, *genLength(), settings, *genPatternLength()) == start);
    }));
}

TEST_CASE ("Quantise never moves a note before the start of the pattern", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("the result is never negative", [] {
        RC_ASSERT (quantisedStart (*genBeat(), *genLength(), *genQuantiseSettings(),
                                   *genPatternLength())
                       >= 0.0);
    }));
}

TEST_CASE ("Quantise lands full strength on the grid", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("strength 1 without swing puts the note on a division", [] {
        const QuantiseSettings settings { *genGrid(), 1.0, 0.0 };
        const auto length = *genLength();
        const auto patternLength = *genPatternLength();

        const auto quantised = quantisedStart (*genBeat(), length, settings, patternLength);

        const auto divisions = quantised / settings.gridBeats;
        const auto onGrid = std::abs (divisions - std::round (divisions)) < 1.0e-6;

        // ...unless the note ran into the end of the pattern, which is the one
        // thing allowed to take it back off the grid.
        const auto maxStart = std::max (0.0, patternLength - length);

        RC_ASSERT (onGrid || std::abs (quantised - maxStart) < tolerance);
    }));
}

TEST_CASE ("Quantise keeps a note inside the pattern", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("a note that fitted still fits", [] {
        const auto length = *genLength();
        const auto start = *genBeat();

        // Built to hold the note rather than generated and then filtered: a
        // note left hanging off the end by a shorten is deliberately left
        // alone, so it says nothing about this.
        const auto patternLength = start + length + *genPatternLength();

        const auto quantised = quantisedStart (start, length, *genQuantiseSettings(),
                                               patternLength);

        RC_ASSERT (quantised + length <= patternLength + tolerance);
    }));
}

TEST_CASE ("Quantise settles after one pass", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("full strength without swing is idempotent", [] {
        const QuantiseSettings settings { *genGrid(), 1.0, 0.0 };
        const auto length = *genLength();
        const auto patternLength = *genPatternLength();

        const auto once = quantisedStart (*genBeat(), length, settings, patternLength);
        const auto twice = quantisedStart (once, length, settings, patternLength);

        // Hitting Q a second time on an untouched selection must be a no-op,
        // or the command would walk the part somewhere new on every press.
        RC_ASSERT (std::abs (twice - once) < tolerance);
    }));
}

TEST_CASE ("Quantise keeps the order of a part", "[pianoroll][quantise]")
{
    REQUIRE (rc::check ("a note that was earlier is never quantised later", [] {
        const auto settings = *genQuantiseSettings();
        const auto patternLength = *genPatternLength();

        // One length for both, because the clamp against the end of the
        // pattern is a function of the note's own length: two notes of
        // different lengths can legitimately cross when the longer one is held
        // back and the shorter one is not.
        const auto length = *genLength();

        const auto a = *genBeat();
        const auto b = *genBeat();

        RC_ASSERT (quantisedStart (std::min (a, b), length, settings, patternLength)
                       <= quantisedStart (std::max (a, b), length, settings, patternLength)
                              + tolerance);
    }));
}

//==============================================================================
// The bounds a block gesture is clamped against

TEST_CASE ("A block's bounds contain the block", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("no note starts before, ends after, or sits outside", [] {
        const auto block = *genBlock (*genPitchRange());
        const auto bounds = boundsOf (block);

        for (const auto& note : block)
        {
            RC_ASSERT (note.start >= bounds.lowestStart);
            RC_ASSERT (note.start + note.length <= bounds.highestEnd);
            RC_ASSERT (note.pitch >= bounds.lowestPitch);
            RC_ASSERT (note.pitch <= bounds.highestPitch);
        }

        // Counted from zero, so a block entirely to the left of the pattern
        // reports an end of zero rather than a negative one -- which is what
        // stops the clamp's two limits crossing over.
        RC_ASSERT (bounds.highestEnd >= 0.0);
    }));
}

//==============================================================================
// The clamp a move drag and a paste share

TEST_CASE ("A block gesture that asks for nothing moves nothing", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("a zero request answers zero", [] {
        const auto keyboard = *genPitchRange();
        const auto bounds = boundsOf (*genBlock (keyboard));

        RC_ASSERT (clampMoveBeats (bounds, *genPatternLength(), 0.0) == 0.0);
        RC_ASSERT (clampMovePitch (bounds, keyboard, 0) == 0);
    }));
}

TEST_CASE ("A block gesture never overshoots what was asked for", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("the clamp only ever shortens the move", [] {
        // The pattern length is unconstrained on purpose: shortening a pattern
        // leaves notes past its end, and a paste can be longer than what it
        // lands in, so the clamp has to answer something sane for a block that
        // does not fit at all.
        const auto bounds = boundsOf (*genBlock (*genPitchRange()));
        const auto requested = *genDelta();

        const auto delta = clampMoveBeats (bounds, *genPatternLength(), requested);

        // Never further than asked, and never the other way: dragging right
        // must not move the block left, however hard the limits bite.
        RC_ASSERT (std::abs (delta) <= std::abs (requested) + tolerance);
        RC_ASSERT (delta * requested >= 0.0);
    }));
}

TEST_CASE ("A block gesture keeps the notes inside the pattern", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("a block that fitted still fits", [] {
        const auto block = *genBlock (*genPitchRange());
        const auto bounds = boundsOf (block);

        // Long enough to hold what is already in it; a block hanging off the
        // end is the case the property above covers.
        const auto patternLength = bounds.highestEnd + *genPatternLength();

        const auto delta = clampMoveBeats (bounds, patternLength, *genDelta());

        for (const auto& note : block)
        {
            RC_ASSERT (note.start + delta >= -tolerance);
            RC_ASSERT (note.start + note.length + delta <= patternLength + tolerance);
        }
    }));
}

TEST_CASE ("A move drag keeps the selection on the keyboard", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("every note lands on a drawn row", [] {
        const auto keyboard = *genPitchRange();
        const auto block = *genBlock (keyboard);

        const auto delta = clampMovePitch (boundsOf (block), keyboard,
                                           *rc::gen::inRange (-256, 257));

        for (const auto& note : block)
        {
            RC_ASSERT (note.pitch + delta >= keyboard.lowest);
            RC_ASSERT (note.pitch + delta <= keyboard.highest);
        }
    }));
}

// Nothing here asserts that the block keeps its shape. That is not a property
// of the clamp but of its signature: it answers one delta for the whole block,
// so the gaps inside cannot move whatever it answers. The clamp's job is to
// make that single answer *safe* for every note in the block, which is what the
// two containment properties above hold it to.

TEST_CASE ("A block gesture follows the pointer", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("asking to move further never moves the block less far", [] {
        const auto bounds = boundsOf (*genBlock (*genPitchRange()));
        const auto patternLength = *genPatternLength();

        const auto a = *genDelta();
        const auto b = *genDelta();

        // The gesture may stop following the pointer once it hits a limit, but
        // it must never turn around: dragging further right cannot land the
        // block further left than a shorter drag did.
        RC_ASSERT (clampMoveBeats (bounds, patternLength, std::min (a, b))
                       <= clampMoveBeats (bounds, patternLength, std::max (a, b)) + tolerance);
    }));
}

TEST_CASE ("A block gesture does not creep", "[pianoroll][drag]")
{
    REQUIRE (rc::check ("re-requesting the move it granted grants the same move", [] {
        const auto keyboard = *genPitchRange();
        const auto bounds = boundsOf (*genBlock (keyboard));
        const auto patternLength = *genPatternLength();

        // A move drag recomputes its delta from where the gesture began, never
        // from where the last event left the notes -- but a clamp that did not
        // hold still when handed its own answer would make even that walk: a
        // drag pinned against the end of the pattern would inch further on
        // every mouse event.
        const auto delta = clampMoveBeats (bounds, patternLength, *genDelta());
        RC_ASSERT (clampMoveBeats (bounds, patternLength, delta) == delta);

        const auto deltaPitch = clampMovePitch (bounds, keyboard, *rc::gen::inRange (-256, 257));
        RC_ASSERT (clampMovePitch (bounds, keyboard, deltaPitch) == deltaPitch);
    }));
}
