// Properties of the break slicer's cuts (Generator::insertBreakSlice and
// friends).
//
// A slice is a note number: slice i plays on breakFirstNote + i. So the cuts
// and the notes are one structure, and the edit that changes how many cuts
// there are has to move notes to keep it whole. What is checked here is that
// promise, from the side of the music: after any cut is added, every note
// still starts the sound it started before; after one is taken away, every
// note that was not on it does too, and the ones that were land on the slice
// that grew over it. And whatever the sequence of edits, the cuts stay a
// valid set -- sorted, spaced, inside the loop, never empty.
//
// Written against rc::check() for the same reason as SongStateProperties.cpp.

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "model/SongModel.h"
#include "plugins/BreakAnalysis.h"

namespace
{

using namespace carve::model;

constexpr double loopSeconds = 7.0;   // about an Amen

// Where the sound a note starts begins, in the loop: its slice's start, or
// nothing for a note that is not on a slice.
std::optional<double> soundStartFor (int pitch, const std::vector<double>& slices)
{
    const int index = pitch - Generator::breakFirstNote;

    if (index < 0 || index >= (int) slices.size())
        return std::nullopt;

    return slices[(size_t) index];
}

std::vector<std::optional<double>> soundStartsOf (const Generator& generator)
{
    const auto slices = generator.getBreakSlices();
    std::vector<std::optional<double>> starts;

    for (const auto& pattern : generator.getPatterns())
        for (const auto& note : pattern.getNotes())
            starts.push_back (soundStartFor (note.getPitch(), slices));

    return starts;
}

std::vector<int> pitchesOf (const Generator& generator)
{
    std::vector<int> pitches;

    for (const auto& pattern : generator.getPatterns())
        for (const auto& note : pattern.getNotes())
            pitches.push_back (note.getPitch());

    return pitches;
}

void requireValidCuts (const Generator& generator)
{
    const auto slices = generator.getBreakSlices();
    RC_ASSERT (! slices.empty());
    RC_ASSERT ((int) slices.size() <= Generator::maxBreakSlices);
    RC_ASSERT (slices.front() >= 0.0);
    RC_ASSERT (slices.back() <= loopSeconds - Generator::minBreakSliceSeconds + 1.0e-9);

    for (size_t i = 1; i < slices.size(); ++i)
        RC_ASSERT (slices[i] - slices[i - 1] >= Generator::minBreakSliceSeconds - 1.0e-9);
}

// A break with a loaded loop, an even division, the pattern a load makes,
// and a second pattern of random notes -- some on slices, some above them,
// some below -- so the remapping meets notes it must leave alone.
Generator makeBreak (Song& song, int divisions, const std::vector<int>& extraPitches)
{
    auto generator = song.addGenerator ("Amen", Generator::breakType, nullptr);
    generator.setBreakFile (juce::File ("/tmp/amen.wav"), loopSeconds, 4.0, nullptr);
    generator.divideBreakEvenly (divisions, loopSeconds, nullptr);
    generator.createBreakPattern (loopSeconds, 4.0, nullptr);

    auto extra = generator.createPattern (nullptr, 4.0);
    double beat = 0.0;

    for (auto pitch : extraPitches)
    {
        extra.addNote (beat, 0.25, pitch, 100, nullptr);
        beat += 0.25;
    }

    return generator;
}

rc::Gen<int> genPitch()  { return rc::gen::inRange (20, 120); }

} // namespace

TEST_CASE ("A new cut leaves every note playing what it played", "[break]")
{
    const bool ok = rc::check ([]
    {
        auto song = Song::create ("Break");
        const auto divisions = *rc::gen::inRange (1, 33);
        const auto extra = *rc::gen::container<std::vector<int>> (genPitch());
        auto generator = makeBreak (song, divisions, extra);

        const auto before = soundStartsOf (generator);
        const auto at = *rc::gen::map (rc::gen::inRange (0, 7000), [] (int ms) { return ms / 1000.0; });
        const auto index = generator.insertBreakSlice (at, loopSeconds, nullptr);

        if (index >= 0)
            RC_ASSERT (std::abs (generator.getBreakSlices()[(size_t) index] - at) < 1.0e-9);

        RC_ASSERT (soundStartsOf (generator) == before);
        requireValidCuts (generator);
    });

    REQUIRE (ok);
}

TEST_CASE ("A removed cut hands its notes to the slice that grows over it", "[break]")
{
    const bool ok = rc::check ([]
    {
        auto song = Song::create ("Break");
        const auto divisions = *rc::gen::inRange (1, 33);
        const auto extra = *rc::gen::container<std::vector<int>> (genPitch());
        auto generator = makeBreak (song, divisions, extra);

        const auto slicesBefore = generator.getBreakSlices();
        const auto pitchesBefore = pitchesOf (generator);
        const auto index = *rc::gen::inRange (0, divisions);
        const bool removed = generator.removeBreakSlice (index, nullptr);

        RC_ASSERT (removed == (divisions > 1));

        const auto slicesAfter = generator.getBreakSlices();
        const auto pitchesAfter = pitchesOf (generator);
        RC_ASSERT (pitchesAfter.size() == pitchesBefore.size());

        for (size_t i = 0; i < pitchesBefore.size(); ++i)
        {
            const int was = pitchesBefore[i] - Generator::breakFirstNote;

            if (! removed || was < 0)
            {
                // Nothing removed, or below every slice: untouched.
                RC_ASSERT (pitchesAfter[i] == pitchesBefore[i]);
            }
            else if (was == index)
            {
                // To the slice before, which now runs over the removed one --
                // or, for the first, to the new first.
                const int expected = index > 0 ? index - 1 : 0;
                RC_ASSERT (pitchesAfter[i] == Generator::breakFirstNote + expected);
            }
            else
            {
                // The same sound -- or, above the slices, still none.
                RC_ASSERT (soundStartFor (pitchesAfter[i], slicesAfter)
                            == soundStartFor (pitchesBefore[i], slicesBefore));
            }
        }

        requireValidCuts (generator);
    });

    REQUIRE (ok);
}

TEST_CASE ("Cuts stay a valid set under any sequence of edits, and undo all the way", "[break][undo]")
{
    const bool ok = rc::check ([]
    {
        auto song = Song::create ("Break");
        auto generator = makeBreak (song, *rc::gen::inRange (1, 33), {});
        const auto originalXml = song.toXmlString();

        juce::UndoManager undo;
        const auto steps = *rc::gen::inRange (1, 40);

        for (int step = 0; step < steps; ++step)
        {
            undo.beginNewTransaction();
            const auto count = (int) generator.getBreakSlices().size();
            const auto at = *rc::gen::map (rc::gen::inRange (-100, 7100), [] (int ms) { return ms / 1000.0; });

            switch (*rc::gen::inRange (0, 4))
            {
                case 0:  generator.insertBreakSlice (at, loopSeconds, &undo); break;
                case 1:  generator.removeBreakSlice (*rc::gen::inRange (0, count), &undo); break;
                case 2:  generator.moveBreakSlice (*rc::gen::inRange (0, count), at, loopSeconds, &undo); break;
                default: generator.divideBreakEvenly (*rc::gen::inRange (1, 80), loopSeconds, &undo); break;
            }

            requireValidCuts (generator);
        }

        while (undo.undo()) {}

        RC_ASSERT (song.toXmlString() == originalXml);
    });

    REQUIRE (ok);
}

TEST_CASE ("A loaded break opens on a pattern that plays it as recorded", "[break]")
{
    auto song = Song::create ("Break");
    auto generator = song.addGenerator ("Amen", Generator::breakType, nullptr);
    generator.setBreakFile (juce::File ("/tmp/amen.wav"), loopSeconds, 4.0, nullptr);

    // An Amen is four bars at about 137 BPM.
    REQUIRE (generator.getBreakBars() == 4);
    REQUIRE (std::abs (generator.getBreakBpm (loopSeconds, 4.0) - 137.14) < 0.01);
    REQUIRE ((int) generator.getBreakSlices().size() == Generator::defaultBreakSlices);

    auto pattern = generator.createBreakPattern (loopSeconds, 4.0, nullptr);
    REQUIRE (pattern.getLengthBeats() == 16.0);
    REQUIRE (pattern.getNumNotes() == Generator::defaultBreakSlices);

    for (int i = 0; i < pattern.getNumNotes(); ++i)
    {
        const auto note = pattern.getNote (i);
        REQUIRE (note.getPitch() == Generator::breakFirstNote + i);
        REQUIRE (std::abs (note.getStart() - i * 1.0) < 1.0e-9);
        REQUIRE (std::abs (note.getLength() - 1.0) < 1.0e-9);
    }

    // And it all comes back from a file.
    auto loaded = Song::fromXml (song.toXmlString());
    REQUIRE (loaded.has_value());
    auto restored = loaded->findGenerator (generator.getId());
    REQUIRE (restored.has_value());
    REQUIRE (restored->getBreakSlices() == generator.getBreakSlices());
    REQUIRE (restored->getBreakBars() == 4);
}

TEST_CASE ("A break's bars are guessed from its tempo", "[break]")
{
    REQUIRE (Generator::guessBreakBars (7.0, 4.0) == 4);     // Amen
    REQUIRE (Generator::guessBreakBars (1.75, 4.0) == 1);    // one bar of it
    REQUIRE (Generator::guessBreakBars (3.5, 4.0) == 2);
    REQUIRE (Generator::guessBreakBars (14.0, 4.0) == 8);
    REQUIRE (Generator::guessBreakBars (0.0, 4.0) == 4);

    // A tempo in the name wins over the guess, and a take number is not one.
    REQUIRE (Generator::guessBreakBars (6.075941, 4.0, "cw_amen05_158") == 4);
    REQUIRE (Generator::guessBreakBars (2.742857, 4.0, "cw_amen01_175") == 2);
    REQUIRE (Generator::guessBreakBars (5.2, 4.0, "think 92bpm") == 2);
    REQUIRE (Generator::guessBreakBars (3.5, 4.0, "loop 1234") == 2);
}

TEST_CASE ("Detect finds the hits of a loop, and cuts just before them", "[break][analysis]")
{
    using namespace carve::plugins::breakanalysis;

    // Eight hits of decaying noise, at uneven times, over a low hum that
    // never stops -- the room and the kick tail a real loop has.
    constexpr double rate = 44100.0;
    const std::vector<double> hits { 0.0, 0.21, 0.5, 0.62, 1.0, 1.37, 1.5, 1.81 };
    juce::AudioBuffer<float> loop (2, (int) (2.0 * rate));
    juce::Random random (1);

    for (int i = 0; i < loop.getNumSamples(); ++i)
    {
        const double t = i / rate;
        float value = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * t);

        for (auto hit : hits)
            if (t >= hit)
                value += (random.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-(t - hit) * 40.0);

        loop.setSample (0, i, value);
        loop.setSample (1, i, value);
    }

    const auto cuts = detectTransients (loop, rate, 0.5, 64);
    REQUIRE (cuts.size() == hits.size());

    for (size_t i = 0; i < hits.size(); ++i)
    {
        REQUIRE (cuts[i] <= hits[i] + 0.001);
        REQUIRE (cuts[i] >= hits[i] - 0.01);
    }

    // And no more than asked for: the strongest, in order.
    const auto fewer = detectTransients (loop, rate, 0.5, 4);
    REQUIRE (fewer.size() == 4);
    REQUIRE (std::is_sorted (fewer.begin(), fewer.end()));
}

TEST_CASE ("An untouched as-recorded pattern can follow the cuts, and a touched one is the user's", "[break]")
{
    const bool ok = rc::check ([]
    {
        auto song = Song::create ("Break");
        auto generator = makeBreak (song, *rc::gen::inRange (1, 33), {});
        auto pattern = generator.getPatterns().front();
        RC_ASSERT (generator.playsBreakAsRecorded (pattern, loopSeconds, 4.0));

        // Any edit of the cuts, then the carry the controller does.
        const auto at = *rc::gen::map (rc::gen::inRange (0, 7000), [] (int ms) { return ms / 1000.0; });
        const auto count = (int) generator.getBreakSlices().size();

        switch (*rc::gen::inRange (0, 5))
        {
            case 0:  generator.insertBreakSlice (at, loopSeconds, nullptr); break;
            case 1:  generator.removeBreakSlice (*rc::gen::inRange (0, count), nullptr); break;
            case 2:  generator.moveBreakSlice (*rc::gen::inRange (0, count), at, loopSeconds, nullptr); break;
            case 3:  generator.setBreakBars (*rc::gen::inRange (1, 17), nullptr); break;
            default: generator.divideBreakEvenly (*rc::gen::inRange (1, 80), loopSeconds, nullptr); break;
        }

        generator.fillBreakAsRecorded (pattern, loopSeconds, 4.0, nullptr);
        RC_ASSERT (generator.playsBreakAsRecorded (pattern, loopSeconds, 4.0));
        RC_ASSERT (pattern.getNumNotes() == (int) generator.getBreakSlices().size());

        // A note moved by hand makes it the user's.
        auto note = pattern.getNote (*rc::gen::inRange (0, pattern.getNumNotes()));
        note.setPitch (note.getPitch() + *rc::gen::element (-1, 1, 7), nullptr);
        RC_ASSERT (! generator.playsBreakAsRecorded (pattern, loopSeconds, 4.0));
    });

    REQUIRE (ok);
}
