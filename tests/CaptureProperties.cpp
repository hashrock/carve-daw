// Properties of Capture: which notes are "the last phrase", and where they
// land in the pattern made of them (src/model/Capture.h).
//
// The notes are generated as a performance would arrive -- in order of their
// starts, with arbitrary lengths and arbitrary rests between them -- because
// that is the only order the MIDI input ever hands them over in.

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "PropertyGenerators.h"
#include "model/Capture.h"
#include "model/SongModel.h"

namespace
{

using carve::model::capture::PlayedNote;
namespace capture = carve::model::capture;

constexpr double tolerance = 1.0e-6;

// (rest before it, length, pitch), all in sixteenths so a counterexample
// reads as music. The rests run long enough to open a gap now and then.
using NoteSpecs = std::vector<std::tuple<int, int, int>>;

std::vector<PlayedNote> perform (double startBeat, const NoteSpecs& specs)
{
    std::vector<PlayedNote> played;
    auto beat = startBeat;

    for (const auto& [rest, length, pitch] : specs)
    {
        beat += rest * 0.25;
        played.push_back ({ beat, beat + length * 0.25, pitch, 100, {} });
    }

    return played;
}

rc::Gen<NoteSpecs> genNoteSpecs()
{
    return rc::gen::nonEmpty (rc::gen::container<NoteSpecs> (
        rc::gen::tuple (rc::gen::inRange (0, 64), rc::gen::inRange (1, 32), rc::gen::inRange (0, 128))));
}

rc::Gen<double> genBeatsPerBar()
{
    return rc::gen::element (2.0, 3.0, 3.5, 4.0, 6.0);
}

} // namespace

TEST_CASE ("The last phrase is the run after the last gap", "[capture]")
{
    REQUIRE (rc::check ("a suffix with no gap inside and one before it", []
    {
        const auto specs = *genNoteSpecs();
        const auto gap = (double) *rc::gen::inRange (1, 16);
        const auto played = perform (0.0, specs);
        const auto phrase = capture::lastPhrase (played, gap);

        RC_ASSERT (! phrase.empty());
        RC_ASSERT (phrase.size() <= played.size());

        const auto first = played.size() - phrase.size();

        // A suffix of what was played, in its order.
        for (size_t i = 0; i < phrase.size(); ++i)
            RC_ASSERT (phrase[i].startBeat == played[first + i].startBeat);

        // No gap inside it...
        auto soundingUntil = phrase.front().endBeat;

        for (size_t i = 1; i < phrase.size(); ++i)
        {
            RC_ASSERT (phrase[i].startBeat - soundingUntil < gap);
            soundingUntil = std::max (soundingUntil, phrase[i].endBeat);
        }

        // ...and one in front of it, unless it is everything.
        if (first > 0)
        {
            double before = played.front().endBeat;

            for (size_t i = 1; i < first; ++i)
                before = std::max (before, played[i].endBeat);

            RC_ASSERT (phrase.front().startBeat - before >= gap);
        }
    }));
}

TEST_CASE ("A captured pattern keeps the timing it was played with", "[capture]")
{
    REQUIRE (rc::check ("whole bars, same spacing, nothing outside", []
    {
        const auto specs = *genNoteSpecs();
        const auto beatsPerBar = *genBeatsPerBar();
        const auto started = *carve::test::genGridBeat (0.25, 4096);
        const bool onGrid = *rc::gen::arbitrary<bool>();

        auto played = perform (started, specs);

        // The phase every later note would have read off the same playhead.
        const auto phase = std::fmod (played.front().startBeat, beatsPerBar);

        if (onGrid)
            for (auto& note : played)
                note.barPhase = std::fmod (note.startBeat, beatsPerBar);

        const auto pattern = capture::toPattern (played, beatsPerBar);

        RC_ASSERT (pattern.notes.size() == played.size());

        // Whole bars, at least one.
        const auto bars = pattern.lengthBeats / beatsPerBar;
        RC_ASSERT (bars >= 1.0 - tolerance);
        RC_ASSERT (std::abs (bars - std::round (bars)) < tolerance);

        // The first note is the downbeat, or where it was in its bar.
        RC_ASSERT (std::abs (pattern.notes.front().startBeat - (onGrid ? phase : 0.0)) < tolerance);

        double lastStart = 0.0;

        for (size_t i = 0; i < played.size(); ++i)
        {
            const auto& note = pattern.notes[i];

            // Same distances apart as they were played.
            RC_ASSERT (std::abs ((note.startBeat - pattern.notes.front().startBeat)
                                 - (played[i].startBeat - played.front().startBeat)) < tolerance);

            // Inside the pattern, and never longer than played.
            RC_ASSERT (note.startBeat >= 0.0);
            RC_ASSERT (note.startBeat < pattern.lengthBeats);
            RC_ASSERT (note.startBeat + note.lengthBeats <= pattern.lengthBeats + tolerance
                       || note.lengthBeats <= carve::model::Note::minLengthBeats);
            RC_ASSERT (note.lengthBeats <= played[i].endBeat - played[i].startBeat + tolerance);
            RC_ASSERT (note.pitch == played[i].pitch);

            lastStart = std::max (lastStart, note.startBeat);
        }

        // And no longer than it has to be: the last bar has a note in it.
        RC_ASSERT (lastStart >= pattern.lengthBeats - beatsPerBar - tolerance);
    }));
}
