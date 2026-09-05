// Properties of the song's time axis: beats to seconds, beats to bars, and
// back again.
//
// These four conversions (Song::secondsFromBeats, beatsFromSeconds,
// toBarsAndBeats, beatOfBar) are the only place in the app that turns a
// musical position into a physical one, and every one of them is a walk over a
// list of changes with a floating-point cursor. That shape is what property
// testing is for: the interesting cases are combinations of tempo and time
// signature changes that nobody writes out by hand.
//
// Written against RapidCheck's rc::check() rather than its Catch integration,
// which targets Catch2 v2; a failing property fails the enclosing test case.

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "model/SongModel.h"

namespace
{

using carve::model::Song;
using carve::model::TimeSignature;

// Beats land on a sixteenth-note grid, which is what every gesture in the app
// snaps to, over a range of a few hundred bars. Arbitrary doubles would only
// buy floating-point noise the app can never produce.
constexpr double gridUnit = 0.25;
constexpr int maxGridSteps = 2048;          // 512 beats

// What the model itself allows: TempoChange::setBpm clamps to this, so
// generating outside it would only be testing the clamp.
constexpr double minBpm = 20.0;
constexpr double maxBpm = 999.0;

// Beats are doubles all the way through, so every comparison needs a hair of
// slack. Generous next to the ~1e-12 these walks actually accumulate.
constexpr double tolerance = 1.0e-6;

// A tempo change, as (beat, bpm). Kept as primitives rather than as a model
// type so RapidCheck can both print and shrink a counterexample.
using TempoSpecs = std::vector<std::pair<double, double>>;

// A time signature change, as (step since the previous change, numerator,
// denominator). A step is counted in bars or in grid units depending on which
// shape the song is being built in -- see sigChanges. Bars have to be a step
// rather than an absolute beat because where a bar line falls is a function of
// every signature before it.
using SigSteps = std::vector<std::tuple<int, int, int>>;

// What those steps turn into: the changes as the model stores them.
using SigChanges = std::vector<std::pair<double, TimeSignature>>;

rc::Gen<double> genBeat()
{
    return rc::gen::map (rc::gen::inRange (0, maxGridSteps),
                         [] (int steps) { return steps * gridUnit; });
}

// A pair of beats already in order, for the properties that say a later
// position is never an earlier anything.
rc::Gen<std::pair<double, double>> genOrderedBeats()
{
    return rc::gen::map (rc::gen::pair (genBeat(), genBeat()),
                         [] (const std::pair<double, double>& beats)
                         {
                             return std::make_pair (std::min (beats.first, beats.second),
                                                    std::max (beats.first, beats.second));
                         });
}

rc::Gen<double> genBpm()
{
    // Tenths, so a fractional tempo is reachable without generating a double
    // that no tempo field could hold.
    return rc::gen::map (rc::gen::inRange ((int) (minBpm * 10.0), (int) (maxBpm * 10.0) + 1),
                         [] (int tenths) { return tenths / 10.0; });
}

// One change per beat.
//
// PlaylistComponent enforces this on both the add and the drag path, and says
// why: two changes on one beat "would fight over which of them wins". Below
// that, collectSorted orders the changes with a non-stable std::sort, so which
// of such a pair wins is not document order but whatever the sort left -- and
// the walks here would then be pinned to that. Songs the app writes never hold
// one, so these properties are scoped to those.
//
// That is a scope, not a guarantee: Song::fromXml builds the tree straight off
// disk and sanitises nothing, so a hand-edited or merged .carve file can hold
// such a pair and get an arbitrary winner. Closing that would mean deciding
// what a duplicate means -- a model-level call, and a wider change than the
// conversions these properties cover.
//
// Bar alignment is the other rule that view enforces, but that one is not
// arbitrary below it: toBarsAndBeats and beatOfBar both spell out what a
// mid-bar change does (it cuts the bar it lands in short), so the properties
// here hold it to that rather than generating around it. See sigChanges.
TempoSpecs onePerBeat (TempoSpecs specs)
{
    std::sort (specs.begin(), specs.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    specs.erase (std::unique (specs.begin(), specs.end(),
                              [] (const auto& a, const auto& b) { return a.first == b.first; }),
                 specs.end());

    return specs;
}

rc::Gen<TempoSpecs> genTempoChanges()
{
    return rc::gen::map (rc::gen::container<TempoSpecs> (rc::gen::pair (genBeat(), genBpm())),
                         onePerBeat);
}

rc::Gen<SigSteps> genSigSteps()
{
    // Denominators are the ones a signature is written with; a numerator past
    // 16 is not a signature anyone plays in.
    const auto denominator = rc::gen::map (rc::gen::inRange (0, 5),
                                           [] (int i) { return 1 << i; });

    return rc::gen::container<SigSteps> (rc::gen::tuple (rc::gen::inRange (0, 17),
                                                         rc::gen::inRange (1, 17),
                                                         denominator));
}

// Turns the generated steps into the (beat, signature) changes the model
// stores.
//
// Both shapes come out of the same steps: bar-aligned, which is all the app
// writes, and mid-bar, which the model defines behaviour for and a song
// arriving from anywhere else can hold. Reading one generated value two ways
// rather than generating two keeps a counterexample down to the steps plus
// which way they were read.
SigChanges sigChanges (const SigSteps& steps, bool barAligned)
{
    SigChanges changes;
    double beat = 0.0;
    TimeSignature current;                  // 4/4 until the first change

    for (const auto& [step, numerator, denominator] : steps)
    {
        // Only the first change may sit where the cursor already is, which is
        // beat 0 -- a change there states what the song is in rather than
        // changing it. Every one after that has to advance, by onePerBeat's
        // rule.
        const auto advance = changes.empty() ? step : std::max (1, step);

        beat += advance * (barAligned ? current.getBeatsPerBar() : gridUnit);
        current = { numerator, denominator };
        changes.emplace_back (beat, current);
    }

    return changes;
}

Song makeSong (double tempo, const TempoSpecs& tempoChanges, const SigChanges& changes)
{
    auto song = Song::create ("property test");
    song.setTempo (tempo, nullptr);

    for (const auto& [beat, bpm] : tempoChanges)
        song.addTempoChange (beat, bpm, nullptr);

    for (const auto& [beat, sig] : changes)
        song.addTimeSigChange (beat, sig, nullptr);

    return song;
}

// The song the bar properties below run against: nothing but signature
// changes, in whichever of the two shapes sigChanges builds. Drawn here rather
// than at each of the call sites, which only ever want one or the other.
Song generateSigSong()
{
    return makeSong (120.0, {}, sigChanges (*genSigSteps(), *rc::gen::arbitrary<bool>()));
}

} // namespace

//==============================================================================
// Beats and seconds

TEST_CASE ("Time is monotonic in beats", "[song][time]")
{
    REQUIRE (rc::check ("a later beat is never an earlier second", [] {
        const auto song = makeSong (*genBpm(), *genTempoChanges(), {});
        const auto [earlier, later] = *genOrderedBeats();

        RC_ASSERT (song.secondsFromBeats (earlier) <= song.secondsFromBeats (later) + tolerance);
    }));
}

TEST_CASE ("Beats survive a trip through seconds", "[song][time]")
{
    REQUIRE (rc::check ("beatsFromSeconds undoes secondsFromBeats", [] {
        const auto song = makeSong (*genBpm(), *genTempoChanges(), {});
        const auto beat = *genBeat();

        RC_ASSERT (std::abs (song.beatsFromSeconds (song.secondsFromBeats (beat)) - beat) < tolerance);
    }));
}

TEST_CASE ("Seconds survive a trip through beats", "[song][time]")
{
    REQUIRE (rc::check ("secondsFromBeats undoes beatsFromSeconds", [] {
        const auto song = makeSong (*genBpm(), *genTempoChanges(), {});

        // The beat generator read as seconds: the same range, which at the
        // tempos in play covers the same stretch of song.
        const auto seconds = *genBeat();

        RC_ASSERT (std::abs (song.secondsFromBeats (song.beatsFromSeconds (seconds)) - seconds) < tolerance);
    }));
}

TEST_CASE ("The tempo at a beat is the tempo time passes at", "[song][time]")
{
    REQUIRE (rc::check ("getTempoAt is the slope of secondsFromBeats", [] {
        const auto song = makeSong (*genBpm(), *genTempoChanges(), {});
        const auto beat = *genBeat();

        // Deliberately not a second walk over the change list: that would only
        // check two copies of the same scan against each other. What getTempoAt
        // is *for* is saying how fast time passes at a beat, so this measures
        // that -- over the stretch up to the next change, where the tempo it
        // reports is the only one in force.
        auto until = beat + 4.0;            // a bar of 4/4, when nothing follows

        for (const auto& change : song.getTempoChanges())
            if (change.getStartBeat() > beat)
            {
                until = change.getStartBeat();
                break;
            }

        const auto slope = (song.secondsFromBeats (until) - song.secondsFromBeats (beat))
                               / (until - beat);

        RC_ASSERT (std::abs (slope - 60.0 / song.getTempoAt (beat)) < tolerance);
    }));
}

//==============================================================================
// Beats and bars

TEST_CASE ("Bar lines round-trip", "[song][bars]")
{
    REQUIRE (rc::check ("toBarsAndBeats undoes beatOfBar", [] {
        const auto song = generateSigSong();
        const auto bar = *rc::gen::inRange (0, 256);

        const auto bars = song.toBarsAndBeats (song.beatOfBar (bar));

        RC_ASSERT (bars.bar == bar);
        RC_ASSERT (std::abs (bars.beat) < tolerance);
    }));
}

TEST_CASE ("Bar lines advance", "[song][bars]")
{
    REQUIRE (rc::check ("a later bar starts later", [] {
        const auto song = generateSigSong();
        const auto bar = *rc::gen::inRange (0, 256);

        RC_ASSERT (song.beatOfBar (bar) < song.beatOfBar (bar + 1));
    }));
}

TEST_CASE ("A position is its bar line plus its offset into the bar", "[song][bars]")
{
    REQUIRE (rc::check ("beatOfBar and toBarsAndBeats agree", [] {
        const auto song = generateSigSong();
        const auto beat = *genBeat();

        const auto bars = song.toBarsAndBeats (beat);

        RC_ASSERT (std::abs (song.beatOfBar (bars.bar) + bars.beat - beat) < tolerance);
    }));
}

TEST_CASE ("An offset into a bar stays inside that bar", "[song][bars]")
{
    REQUIRE (rc::check ("toBarsAndBeats never reports a whole bar", [] {
        const auto song = generateSigSong();
        const auto beat = *genBeat();

        const auto bars = song.toBarsAndBeats (beat);
        const auto beatsPerBar = song.getTimeSigAt (beat).getBeatsPerBar();

        RC_ASSERT (bars.beat >= 0.0);
        RC_ASSERT (bars.beat < beatsPerBar + tolerance);
    }));
}

TEST_CASE ("Bar numbers are monotonic in beats", "[song][bars]")
{
    REQUIRE (rc::check ("a later beat is never an earlier bar", [] {
        const auto song = generateSigSong();
        const auto [earlier, later] = *genOrderedBeats();

        RC_ASSERT (song.toBarsAndBeats (earlier).bar <= song.toBarsAndBeats (later).bar);
    }));
}

TEST_CASE ("A bar is as long as its signature says", "[song][bars]")
{
    REQUIRE (rc::check ("bar-aligned changes leave every bar a whole bar", [] {
        // Bar-aligned only: a change that lands mid-bar cuts the bar it lands
        // in short on purpose, so this says nothing about that shape.
        const auto song = makeSong (120.0, {}, sigChanges (*genSigSteps(), true));
        const auto bar = *rc::gen::inRange (0, 256);

        const auto start = song.beatOfBar (bar);
        const auto length = song.beatOfBar (bar + 1) - start;

        RC_ASSERT (std::abs (length - song.getTimeSigAt (start).getBeatsPerBar()) < tolerance);
    }));
}

TEST_CASE ("A bar-aligned signature change lands on a bar line", "[song][bars]")
{
    REQUIRE (rc::check ("every change the app could write starts a bar", [] {
        const auto changes = sigChanges (*genSigSteps(), true);
        const auto song = makeSong (120.0, {}, changes);

        for (const auto& [beat, sig] : changes)
            RC_ASSERT (std::abs (song.toBarsAndBeats (beat).beat) < tolerance);
    }));
}
