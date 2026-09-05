// Properties of the song document under a sequence of edits.
//
// Every state the app has that outlives a gesture lives in one place: the
// ValueTree behind carve::model::Song. Generators, patterns, notes, playlist
// placements, effect chains, automation, sends, tempo and time signature
// changes are all children of it, every edit goes through an UndoManager, and
// the file on disk is the same tree serialised. So the app's state machine is
// already a testable one -- no extraction needed, only a driver -- and that is
// what this file is: a generated sequence of edit commands, applied one
// transaction each, with the invariants checked after every one.
//
// What that buys over an example test is the sequences nobody writes: fill
// eleven slots, duplicate the eighth into the twelfth, place it, shorten the
// pattern, undo four steps, place another clip. The interesting failures in a
// document model are exactly there -- in what an operation does to a state
// some *other* operation left behind.
//
// Three kinds of property, in the three sections below:
//
//   * every reachable state is a valid one (slots hold one pattern, clips name
//     a pattern that exists, sorted lists come back sorted, ...)
//   * undo walks back to where the sequence started, and redo walks forward to
//     where it ended
//   * the document survives a trip through XML, at any reachable state
//
// Written against RapidCheck's rc::check(), like SongTimeProperties.cpp; see
// the header comment there for why not its Catch integration.

#include <algorithm>
#include <optional>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "model/SongModel.h"

namespace
{

using namespace carve::model;

// Beats land on a sixteenth-note grid, which is what every gesture in the app
// snaps to; the same reasoning as in SongTimeProperties.
constexpr double gridUnit = 0.25;
constexpr int maxGridSteps = 64;            // 16 beats

// Songs are kept small on purpose. The properties are about how operations
// interact, and a four-generator song reaches every interaction a forty-one
// one does while leaving a counterexample small enough to read.
constexpr int maxGenerators = 4;
constexpr int maxReturns = 4;

// What TempoChange::setBpm clamps to, so generating outside it would only be
// testing the clamp -- the same range SongTimeProperties generates over.
constexpr double minBpm = 20.0;
constexpr double maxBpm = 999.0;

// The stretch of keyboard the piano roll draws, which is the only place notes
// come from.
int pitchOf (double unitInterval)  { return 24 + (int) (unitInterval * 72.0); }

//==============================================================================
// The command language
//
// One step is (op, a, b, c, x), all primitives, so that RapidCheck can both
// print and shrink a counterexample -- the same choice SongTimeProperties makes
// for its tempo and signature specs. `a` and `b` select which generator,
// pattern, clip or effect the step is about (counting round the list), `c` is a
// small integer read as a beat or an index, and `x` is a 0..1 value scaled to
// whatever range the op needs.
//
// `c` reaches a little way below zero on purpose: several of the model's
// setters clamp, and a driver that only ever pointed inside the song would
// never ask them to.
enum Op
{
    addGenerator = 0,
    addPatternInSlot,
    duplicatePattern,
    removeUnusedPattern,
    setPatternLength,
    addNote,
    moveNote,
    removeNote,
    addClip,
    moveClip,
    setClipLength,
    clearClipLength,
    setClipTranspose,
    setClipPattern,
    removeClip,
    addAudioClip,
    removeAudioClip,
    addEffect,
    moveEffect,
    removeEffect,
    setMixer,
    setEffectState,
    setBusVolume,
    addTempoChange,
    removeTempoChange,
    addTimeSigChange,
    removeTimeSigChange,
    setLoopRange,
    clearLoopRange,
    addAutomationPoint,
    removeAutomationPoint,
    addReturn,
    removeReturn,
    setSendGain,
    removeSend,
    numOps
};

using Step = std::tuple<int, int, int, int, double>;
using Steps = std::vector<Step>;

rc::Gen<Step> genStep()
{
    return rc::gen::tuple (rc::gen::inRange (0, (int) numOps),
                           rc::gen::inRange (0, 64),
                           rc::gen::inRange (0, 64),
                           rc::gen::inRange (-8, maxGridSteps + 1),
                           rc::gen::map (rc::gen::inRange (0, 101),
                                         [] (int n) { return n / 100.0; }));
}

rc::Gen<Steps> genSteps()
{
    return rc::gen::container<Steps> (genStep());
}

//==============================================================================
// Picking what a step is about.

// Counting round the list, from either direction: a step's selector may be
// negative, and a negative modulo would index off the front.
int wrapIndex (int index, int size)
{
    return ((index % size) + size) % size;
}

template <typename T>
std::optional<T> pick (const std::vector<T>& items, int index)
{
    if (items.empty())
        return std::nullopt;

    return items[(std::size_t) wrapIndex (index, (int) items.size())];
}

// The nth generator the predicate accepts, counting round.
template <typename Predicate>
std::optional<Generator> pickGenerator (const Song& song, int index, Predicate&& wanted)
{
    const auto generators = song.getGenerators();
    int matches = 0;

    for (const auto& generator : generators)
        if (wanted (generator))
            ++matches;

    if (matches == 0)
        return std::nullopt;

    auto remaining = wrapIndex (index, matches);

    for (const auto& generator : generators)
        if (wanted (generator) && remaining-- == 0)
            return generator;

    return std::nullopt;
}

// Pattern ops skip audio generators. Not a limitation of the model -- it would
// happily hang a PATTERNS node off one -- but of the app: an audio row plays
// files off the playlist and PlaylistComponent::patternForRow refuses to give
// it a pattern at all, so a song with a pattern on an audio generator is not a
// state any gesture can reach, and asserting things about it would be
// asserting things about a song that cannot exist.
std::optional<Generator> pickPatternGenerator (const Song& song, int index)
{
    return pickGenerator (song, index, [] (const Generator& g) { return ! g.isAudio(); });
}

// A generator and one of its patterns, which is what most of the steps below
// are about: a note lives in a pattern, and a pattern only means anything
// alongside the generator that sounds it.
struct GeneratorAndPattern
{
    Generator generator;
    Pattern pattern;
};

std::optional<GeneratorAndPattern> pickPattern (const Song& song, int generatorIndex,
                                                int patternIndex)
{
    if (auto generator = pickPatternGenerator (song, generatorIndex))
        if (auto pattern = pick (generator->getPatterns(), patternIndex))
            return GeneratorAndPattern { *generator, *pattern };

    return std::nullopt;
}

// An effect, and the generator whose chain it sits in.
struct GeneratorAndEffect
{
    Generator generator;
    Effect effect;
};

std::optional<GeneratorAndEffect> pickEffect (const Song& song, int generatorIndex,
                                              int effectIndex)
{
    if (auto generator = pick (song.getGenerators(), generatorIndex))
        if (auto effect = pick (generator->getEffects(), effectIndex))
            return GeneratorAndEffect { *generator, *effect };

    return std::nullopt;
}

// Whether a tempo or time signature change already sits on this beat. One
// change per beat is what PlaylistComponent enforces on both the add and the
// drag path; SongTimeProperties explains what a duplicate would mean and why
// nothing in the app writes one. checkInvariants holds the songs built here to
// it, so the guard and the property it protects stay a pair.
//
// Asked about where the change will *land*, not where the step pointed. Both
// setStartBeat clamp at zero, so two steps aiming at different negative beats
// would otherwise both pass a guard checked against the raw request and then
// pile up on beat 0 -- which is exactly what this property caught the first
// time it was run against a driver that could point off the front of the song.
// The app is not exposed to it, because the ruler never asks for a bar before
// the first, but the guard and the clamp disagreeing is worth stating once.
template <typename Changes>
bool hasChangeAt (const Changes& changes, double beat)
{
    for (const auto& change : changes)
        if (change.getStartBeat() == std::max (0.0, beat))
            return true;

    return false;
}

// Somewhere for an audio placement to point. Never read -- nothing here opens
// a file -- but the model stores the path, so it has to be one a juce::File
// will keep verbatim rather than reject as relative.
juce::File audioFileFor (int index)
{
    return juce::File ("/carve-property-test/take" + juce::String (index % 8) + ".wav");
}

//==============================================================================
// One step, applied.
//
// A step that has nothing to act on -- remove a note from a song with no notes
// -- does nothing at all, rather than being generated around. That keeps the
// generator simple and, more to the point, keeps the shrinker useful: it can
// drop any step from a failing sequence without the rest of it changing
// meaning.
void apply (Song& song, juce::UndoManager& um, const Step& step)
{
    const auto [op, a, b, c, x] = step;

    // Where the step points, in beats. Occasionally negative, because several
    // of the model's setters clamp -- AudioClip::setStart and setLengthSeconds,
    // TempoChange::setStartBeat, AutomationPoint::setBeat, PlaylistClip::
    // setLength -- and a driver that only ever asked for positions inside the
    // song would leave every one of those clamps untested.
    const auto beat = c * gridUnit;

    // Where it points once the app's own gestures have had their say. Note
    // starts, pattern clip starts and pattern lengths are not clamped by the
    // model at all: the piano roll and the playlist hold that line, so a
    // negative one is a state no gesture can produce.
    const auto insideSong = std::max (0.0, beat);

    // One transaction per step, which is the rule every gesture in the app
    // follows: a drag, a paint stroke or a quantise is one press of ⌘Z.
    um.beginNewTransaction();

    switch (op)
    {
        case addGenerator:
        {
            // Capped rather than skipped past: an unbounded song would spend
            // the whole sequence adding tracks and never edit one.
            if (song.getNumGenerators() >= maxGenerators)
                return;

            static const char* types[] { "4osc", Generator::samplerType,
                                         Generator::drumKitType, Generator::audioType };

            song.addGenerator ("G" + juce::String (song.getNumGenerators()),
                               types[a % 4], &um);
            return;
        }

        case addPatternInSlot:
        {
            if (auto generator = pickPatternGenerator (song, a))
            {
                const auto slot = PatternSlot::fromFlatIndex (b % PatternSlot::numSlots);
                generator->getOrCreatePatternInSlot (slot, &um, gridUnit + insideSong);
            }
            return;
        }

        case duplicatePattern:
        {
            if (auto picked = pickPattern (song, a, b))
                if (auto destination = picked->generator.findFreeSlot (picked->pattern.getSlot()))
                    picked->generator.duplicatePattern (picked->pattern, *destination, &um);
            return;
        }

        case removeUnusedPattern:
        {
            // The guard GeneratorController applies before it drops a slot the
            // user only browsed past. Repeated here because it is the reason
            // the "every clip names a pattern that exists" invariant below
            // holds -- and therefore what that invariant is really testing: if
            // isPatternUsedInPlaylist ever answered "no" for a pattern that is
            // placed, this would leave a clip pointing at nothing.
            if (auto picked = pickPattern (song, a, b))
                if (picked->pattern.isEmpty() && ! song.isPatternUsedInPlaylist (picked->pattern))
                    picked->generator.removePattern (picked->pattern, &um);
            return;
        }

        case setPatternLength:
        {
            if (auto picked = pickPattern (song, a, b))
                picked->pattern.setLengthBeats (gridUnit + insideSong, &um);
            return;
        }

        case addNote:
        {
            if (auto picked = pickPattern (song, a, b))
                picked->pattern.addNote (insideSong, gridUnit * (1 + b % 8),
                                         pitchOf (x), 1 + (int) (x * 126.0), &um);
            return;
        }

        case moveNote:
        {
            if (auto picked = pickPattern (song, a, b))
                if (auto note = pick (picked->pattern.getNotes(), c))
                {
                    note->setStart (insideSong, &um);
                    note->setPitch (pitchOf (x), &um);
                }
            return;
        }

        case removeNote:
        {
            if (auto picked = pickPattern (song, a, b))
                if (auto note = pick (picked->pattern.getNotes(), c))
                    picked->pattern.removeNote (*note, &um);
            return;
        }

        case addClip:
        {
            if (auto picked = pickPattern (song, a, b))
                song.getPlaylist().addClip (picked->generator, picked->pattern, insideSong, &um);
            return;
        }

        case moveClip:
        {
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                clip->setStart (insideSong, &um);
            return;
        }

        case setClipLength:
        {
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                clip->setLength (beat, &um);
            return;
        }

        case clearClipLength:
        {
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                clip->clearLength (&um);
            return;
        }

        case setClipTranspose:
        {
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                clip->setTranspose ((int) (x * 96.0) - 48, &um);
            return;
        }

        case setClipPattern:
        {
            // Only a pattern of the clip's own generator: the header says so,
            // and a clip naming another generator's pattern is a state no
            // gesture can produce.
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                if (auto generator = song.findGenerator (clip->getGeneratorId()))
                    if (auto pattern = pick (generator->getPatterns(), b))
                        clip->setPatternId (pattern->getId(), &um);
            return;
        }

        case removeClip:
        {
            auto playlist = song.getPlaylist();

            if (auto clip = pick (playlist.getClips(), a))
                playlist.removeClip (*clip, &um);
            return;
        }

        case addAudioClip:
        {
            const auto isAudio = [] (const Generator& g) { return g.isAudio(); };

            if (auto generator = pickGenerator (song, a, isAudio))
                song.getPlaylist().addAudioClip (*generator, audioFileFor (b), beat,
                                                 x * 8.0 - 1.0, &um);
            return;
        }

        case removeAudioClip:
        {
            auto playlist = song.getPlaylist();

            if (auto clip = pick (playlist.getAudioClips(), a))
                playlist.removeAudioClip (*clip, &um);
            return;
        }

        case addEffect:
        {
            static const char* types[] { "compressor", "reverb", "delay", "4bandEq" };

            if (auto generator = pick (song.getGenerators(), a))
                generator->addEffect (types[b % 4], nullptr, &um);
            return;
        }

        case moveEffect:
        {
            if (auto picked = pickEffect (song, a, b))
                picked->generator.moveEffect (picked->effect, c, &um);
            return;
        }

        case removeEffect:
        {
            if (auto picked = pickEffect (song, a, b))
                picked->generator.removeEffect (picked->effect, &um);
            return;
        }

        case setMixer:
        {
            if (auto generator = pick (song.getGenerators(), a))
            {
                switch (b % 4)
                {
                    case 0:  generator->setVolumeDb ((float) (x * 12.0 - 60.0), &um); break;
                    case 1:  generator->setPan ((float) (x * 2.0 - 1.0), &um); break;
                    case 2:  generator->setMuted (c % 2 == 0, &um); break;
                    default: generator->setSoloed (c % 2 == 0, &um); break;
                }
            }
            return;
        }

        case setEffectState:
        {
            if (auto picked = pickEffect (song, a, b))
            {
                picked->effect.setEnabled (c % 2 == 0, &um);

                // A sidechain names a generator, so it is only ever set to one
                // that exists or cleared -- the effect panel lists the song's
                // generators and nothing else.
                if (auto source = pick (song.getGenerators(), c))
                    picked->effect.setSidechainSourceId (x < 0.5 ? source->getId()
                                                                 : juce::String(),
                                                        &um);
            }
            return;
        }

        case setBusVolume:
        {
            // The two faders no generator owns: a return's, and the master's.
            if (b % 2 == 0)
            {
                if (auto ret = pick (song.getReturns(), a))
                {
                    ret->setVolumeDb ((float) (x * 24.0 - 12.0), &um);
                    ret->setMuted (c % 2 == 0, &um);
                }
            }
            else
            {
                song.getMasterBus().setVolumeDb ((float) (x * 24.0 - 12.0), &um);
            }
            return;
        }

        case addTempoChange:
        {
            if (hasChangeAt (song.getTempoChanges(), beat))
                return;

            song.addTempoChange (beat, minBpm + x * (maxBpm - minBpm), &um);
            return;
        }

        case removeTempoChange:
        {
            if (auto change = pick (song.getTempoChanges(), a))
                song.removeTempoChange (*change, &um);
            return;
        }

        case addTimeSigChange:
        {
            if (hasChangeAt (song.getTimeSigChanges(), beat))
                return;

            song.addTimeSigChange (beat, { 1 + a % 16, 1 << (b % 5) }, &um);
            return;
        }

        case removeTimeSigChange:
        {
            if (auto change = pick (song.getTimeSigChanges(), a))
                song.removeTimeSigChange (*change, &um);
            return;
        }

        case setLoopRange:
            // Either way round: the ruler is dragged in both directions and
            // setLoopRange is what normalises it.
            song.setLoopRange (insideSong, a * gridUnit, &um);
            return;

        case clearLoopRange:
            song.clearLoopRange (&um);
            return;

        case addAutomationPoint:
        {
            static const char* targets[] { AutomationLane::volumeTarget,
                                           AutomationLane::panTarget,
                                           AutomationLane::instrumentTarget };

            if (auto generator = pick (song.getGenerators(), a))
            {
                const auto target = targets[b % 3];
                const juce::String param (target == AutomationLane::instrumentTarget
                                              ? "cutoff" : "");

                auto lane = generator->addAutomationLane (target, param, &um);
                lane.addPoint (beat, (float) x, &um);
            }
            return;
        }

        case removeAutomationPoint:
        {
            if (auto generator = pick (song.getGenerators(), a))
                if (auto lane = pick (generator->getAutomationLanes(), b))
                    if (auto point = pick (lane->getPoints(), c))
                        lane->removePoint (*point, &um);
            return;
        }

        case addReturn:
        {
            if ((int) song.getReturns().size() >= maxReturns)
                return;

            song.addReturn ("R" + juce::String (song.getReturns().size()), &um);
            return;
        }

        case removeReturn:
        {
            // Just the return, which is all the mixer's own delete button
            // does: the sends into it are left behind on purpose, and
            // EditSync spells out what happens to one ("dangling send: the
            // return was deleted") rather than treating it as impossible.
            if (auto ret = pick (song.getReturns(), a))
                song.removeReturn (*ret, &um);
            return;
        }

        case setSendGain:
        {
            if (auto generator = pick (song.getGenerators(), a))
                if (auto ret = pick (song.getReturns(), b))
                    generator->setSendGain (ret->getId(), (float) (x * 72.0 - 60.0), &um);
            return;
        }

        case removeSend:
        {
            if (auto generator = pick (song.getGenerators(), a))
                if (auto send = pick (generator->getSends(), b))
                    generator->removeSend (send->getReturnId(), &um);
            return;
        }

        default:
            return;
    }
}

//==============================================================================
// What the document looks like from outside.
//
// Everything a property compares goes through this rather than through the
// tree, and the difference matters. Several of the model's container nodes --
// EFFECTS, SENDS, AUTOMATION, the master bus -- are materialised lazily with a
// null UndoManager, deliberately, so that merely looking at an effect chain is
// not something the user can undo. The cost is that removing the last effect
// and undoing back past it leaves an empty <EFFECTS/> behind, and the raw XML
// therefore does *not* return to what it was.
//
// That is a difference nothing can observe: getEffects() answers the same
// either way. So the properties are stated against what the getters say, which
// is what the app, the renderer and EditSync all actually read.
void describeEffects (juce::String& out, const std::vector<Effect>& effects)
{
    for (const auto& effect : effects)
        out << "  fx " << effect.getId() << " " << effect.getType()
            << (effect.isEnabled() ? " on" : " off")
            << " sc:" << effect.getSidechainSourceId() << "\n";
}

juce::String describe (const Song& song)
{
    juce::String out;

    out << "song " << song.getName() << " tempo " << song.getTempo()
        << " loop " << song.getLoopStart() << ".." << song.getLoopEnd()
        << (song.hasLoopRange() ? " ranged" : " whole") << "\n";

    for (const auto& change : song.getTempoChanges())
        out << " tempo@" << change.getStartBeat() << " " << change.getBpm() << "\n";

    for (const auto& change : song.getTimeSigChanges())
        out << " sig@" << change.getStartBeat() << " " << change.getSignature().numerator
            << "/" << change.getSignature().denominator << "\n";

    for (const auto& generator : song.getGenerators())
    {
        out << " gen " << generator.getId() << " " << generator.getName()
            << " " << generator.getType()
            << " vol " << generator.getVolumeDb() << " pan " << generator.getPan()
            << (generator.isMuted() ? " mute" : "") << (generator.isSoloed() ? " solo" : "") << "\n";

        for (const auto& pattern : generator.getPatterns())
        {
            const auto slot = pattern.getSlot();

            out << "  pat " << pattern.getId() << " " << pattern.getName()
                << " len " << pattern.getLengthBeats()
                << " slot " << (slot ? slot->getKey() : juce::String ("-")) << "\n";

            for (const auto& note : pattern.getNotes())
                out << "   note " << note.getStart() << " " << note.getLength()
                    << " " << note.getPitch() << " " << note.getVelocity() << "\n";
        }

        describeEffects (out, generator.getEffects());

        for (const auto& lane : generator.getAutomationLanes())
        {
            out << "  lane " << lane.getTarget() << " " << lane.getParam() << "\n";

            for (const auto& point : lane.getPoints())
                out << "   pt " << point.getBeat() << " " << point.getValue()
                    << " " << point.getCurve() << "\n";
        }

        for (const auto& send : generator.getSends())
            out << "  send " << send.getReturnId() << " " << send.getGainDb() << "\n";
    }

    for (const auto& ret : song.getReturns())
    {
        out << " ret " << ret.getId() << " " << ret.getName() << " bus " << ret.getBusNumber()
            << " vol " << ret.getVolumeDb() << (ret.isMuted() ? " mute" : "") << "\n";

        describeEffects (out, ret.getEffects());
    }

    out << " master vol " << song.getMasterBus().getVolumeDb() << "\n";
    describeEffects (out, song.getMasterBus().getEffects());

    const auto playlist = song.getPlaylist();

    for (const auto& clip : playlist.getClips())
        out << " clip " << clip.getGeneratorId() << " " << clip.getPatternId()
            << " @" << clip.getStart()
            << (clip.hasOwnLength() ? juce::String (" len ") + juce::String (clip.getLength (0.0))
                                    : juce::String (" len -"))
            << " tr " << clip.getTranspose() << "\n";

    for (const auto& clip : playlist.getAudioClips())
        out << " audio " << clip.getId() << " " << clip.getGeneratorId()
            << " @" << clip.getStart() << " " << clip.getLengthSeconds()
            << "+" << clip.getOffsetSeconds()
            << " " << clip.getFile().getFullPathName() << "\n";

    return out;
}



//==============================================================================
// What has to be true of a song however it was reached.

// Nothing may hold two of these. Collected into a vector rather than a set
// because the lists are a handful of entries long and a counterexample is
// easier to read in insertion order.
template <typename T>
void requireUnique (std::vector<T>& seen, const T& key)
{
    RC_ASSERT (std::find (seen.begin(), seen.end(), key) == seen.end());
    seen.push_back (key);
}

// Both change lists come back in beat order, whatever order the changes were
// added in; every walk over them assumes it, and collectSorted is the only
// thing that makes it true. One change per beat is the app's rule rather than
// the model's -- see hasChangeAt -- and holds for the songs a sequence builds.
template <typename Changes>
void checkChangeList (const Changes& changes)
{
    double previousBeat = -1.0;

    for (const auto& change : changes)
    {
        RC_ASSERT (change.getStartBeat() > previousBeat);

        // TempoChange::setStartBeat and TimeSigChange::setStartBeat both clamp
        // at zero, so a change dragged left off the ruler stops there.
        RC_ASSERT (change.getStartBeat() >= 0.0);
        previousBeat = change.getStartBeat();
    }
}

void checkInvariants (const Song& song)
{
    for (const auto& generator : song.getGenerators())
    {
        // One pattern per slot. Enforced by getOrCreatePatternInSlot, which
        // hands back what is already there, and by findFreeSlot, which is what
        // duplicatePattern is given a destination by -- so this holds both of
        // them to it at once.
        std::vector<juce::String> slots;

        for (const auto& pattern : generator.getPatterns())
            if (auto slot = pattern.getSlot())
            {
                RC_ASSERT (slot->isValid());
                requireUnique (slots, slot->getKey());
            }

        // Ids are what EditSync matches a live plugin to its model entry by:
        // two effects sharing one would have the resync pick whichever it
        // found first.
        std::vector<juce::String> effectIds;

        for (const auto& effect : generator.getEffects())
        {
            RC_ASSERT (effect.getId().isNotEmpty());
            requireUnique (effectIds, effect.getId());
        }

        // At most one send per (generator, return), which is what makes a send
        // addressable by return id at all -- setSendGain reuses the existing
        // node rather than appending a second.
        //
        // Not asserted: that the return it names still exists. Deleting a
        // return leaves the sends into it behind, and EditSync handles that
        // outright ("dangling send: the return was deleted"), so a dangling
        // send is a state the app reaches on purpose.
        std::vector<juce::String> sendTargets;

        for (const auto& send : generator.getSends())
            requireUnique (sendTargets, send.getReturnId());

        // One lane per parameter: addAutomationLane hands back the existing
        // one, and two lanes for one parameter would fight over the curve.
        std::vector<juce::String> laneKeys;

        for (const auto& lane : generator.getAutomationLanes())
        {
            requireUnique (laneKeys, lane.getTarget() + "\n" + lane.getParam());

            // "beat order is what every consumer assumes", and a point dragged
            // left off the lane stops at zero rather than going negative.
            double previous = -1.0;

            for (const auto& point : lane.getPoints())
            {
                RC_ASSERT (point.getBeat() >= 0.0);
                RC_ASSERT (point.getBeat() >= previous);
                previous = point.getBeat();
            }
        }
    }

    // Bus numbers are what an AuxSend and its AuxReturn find each other by, so
    // they have to be unique and positive across the song.
    std::vector<int> busNumbers;

    for (const auto& ret : song.getReturns())
    {
        RC_ASSERT (ret.getBusNumber() > 0);
        requireUnique (busNumbers, ret.getBusNumber());
    }

    const auto playlist = song.getPlaylist();

    // A placement names a generator and one of *its* patterns. Anything else
    // is a clip the playlist draws nothing for and the engine plays nothing
    // from -- see PlaylistComponent::placementFor, which answers nothing at
    // all for one.
    //
    // Where it sits is not asserted: PlaylistClip::setStart writes whatever it
    // is given, and the rule that a clip never runs off the front of the song
    // is PlaylistComponent::dragSelectionTo's rather than the document's.
    for (const auto& clip : playlist.getClips())
    {
        auto generator = song.findGenerator (clip.getGeneratorId());
        RC_ASSERT (generator.has_value());
        RC_ASSERT (generator->findPattern (clip.getPatternId()).has_value());

        // A placement with its own length carries a real one; setLength floors
        // it rather than letting a trim collapse the clip to nothing.
        if (clip.hasOwnLength())
            RC_ASSERT (clip.getLength (0.0) > 0.0);
    }

    for (const auto& clip : playlist.getAudioClips())
    {
        // Without an id EditSync tears the wave clip down and rebuilds it on
        // every resync, re-reading the file and cutting whatever it plays.
        RC_ASSERT (clip.getId().isNotEmpty());
        RC_ASSERT (song.findGenerator (clip.getGeneratorId()).has_value());

        // Unlike a pattern clip, an audio one clamps its own position and
        // length: it is measured in seconds against a file, and neither a
        // negative start nor a zero length names any of it.
        RC_ASSERT (clip.getStart() >= 0.0);
        RC_ASSERT (clip.getOffsetSeconds() >= 0.0);
        RC_ASSERT (clip.getLengthSeconds() >= AudioClip::minLengthSeconds);
    }

    checkChangeList (song.getTempoChanges());
    checkChangeList (song.getTimeSigChanges());

    for (const auto& change : song.getTimeSigChanges())
        RC_ASSERT (change.getSignature().getBeatsPerBar() > 0.0);

    // setLoopRange normalises a right-to-left drag, so a range is never
    // inverted however it was dragged.
    RC_ASSERT (song.getLoopStart() >= 0.0);
    RC_ASSERT (song.getLoopEnd() >= song.getLoopStart() || ! song.hasLoopRange());
}

//==============================================================================

// A song for a sequence to run against: two instrument tracks and an audio one,
// which is the smallest shape that reaches every op above without the sequence
// having to spend its first steps building one.
Song makeSong()
{
    auto song = Song::create ("property test");

    for (const auto& [name, type] : { std::pair { "Bass", "4osc" },
                                      std::pair { "Kit", Generator::drumKitType } })
    {
        auto generator = song.addGenerator (name, type, nullptr);

        // One pattern already in its first slot. Without it the steps that
        // write notes and place clips have nothing to act on until a step
        // happens to make one, and a short sequence would spend itself
        // getting to the state the interesting properties are about.
        generator.getOrCreatePatternInSlot ({ 0, 0 }, nullptr, 4.0);
    }

    song.addGenerator ("Vox", Generator::audioType, nullptr);
    return song;
}

// The UndoManager the properties drive.
//
// Its unit limit is raised past the default 30000, which is a policy about how
// much history to keep rather than anything about what undo means: a sequence
// long enough to hit it would have its oldest transactions quietly dropped,
// and the properties below would then be measuring the pruning rather than the
// undo.
juce::UndoManager makeUndoManager()
{
    return juce::UndoManager (1000 * 1000 * 1000, 1);
}

// A song and the history of the edits made to it, which is what every property
// below starts by building.
struct Edited
{
    Song song = makeSong();
    juce::UndoManager undo = makeUndoManager();

    template <typename AfterEachStep>
    void run (const Steps& steps, AfterEachStep&& afterEachStep)
    {
        for (const auto& step : steps)
        {
            apply (song, undo, step);
            afterEachStep (song);
        }
    }

    void run (const Steps& steps)  { run (steps, [] (const Song&) {}); }

    void undoEverything()  { while (undo.undo()) {} }
    void redoEverything()  { while (undo.redo()) {} }
};

} // namespace

//==============================================================================
// Every state a sequence of edits can reach

TEST_CASE ("A song stays a valid song however it is edited", "[song][state]")
{
    REQUIRE (rc::check ("the invariants hold after every step", [] {
        Edited edited;

        checkInvariants (edited.song);
        edited.run (*genSteps(), checkInvariants);
    }));
}

TEST_CASE ("A song stays valid all the way back", "[song][state]")
{
    REQUIRE (rc::check ("the invariants hold after every undo too", [] {
        Edited edited;
        edited.run (*genSteps());

        // Undo is not simply the reverse of the sequence: a step that found
        // nothing to act on left no transaction behind, so what is being
        // walked back is the edits that actually happened.
        while (edited.undo.undo())
            checkInvariants (edited.song);
    }));
}

//==============================================================================
// Undo and redo

TEST_CASE ("Undo walks a song back to where it started", "[song][undo]")
{
    REQUIRE (rc::check ("undoing everything restores the document", [] {
        Edited edited;
        const auto before = describe (edited.song);

        edited.run (*genSteps());
        edited.undoEverything();

        RC_ASSERT (describe (edited.song) == before);
    }));
}

TEST_CASE ("Redo walks a song forward again", "[song][undo]")
{
    REQUIRE (rc::check ("undo then redo is the identity", [] {
        Edited edited;
        edited.run (*genSteps());

        const auto after = describe (edited.song);

        edited.undoEverything();
        edited.redoEverything();

        RC_ASSERT (describe (edited.song) == after);
    }));
}

TEST_CASE ("Undoing one step undoes one step", "[song][undo]")
{
    REQUIRE (rc::check ("a step and its undo leave the document alone", [] {
        Edited edited;
        edited.run (*genSteps());

        // Whatever state the sequence left, one more edit on top of it has to
        // be undoable on its own -- which is the promise every gesture in the
        // app makes by opening a transaction for itself.
        //
        // The last step gets an undo manager of its own, so that "undo
        // everything this manager knows about" is exactly "undo that one step"
        // whether or not it found anything to act on. Which manager an edit is
        // recorded in is a per-call argument in this model, so a second one
        // costs nothing.
        auto lastStep = makeUndoManager();
        const auto before = describe (edited.song);

        apply (edited.song, lastStep, *genStep());

        while (lastStep.undo())
        {}

        RC_ASSERT (describe (edited.song) == before);
    }));
}

//==============================================================================
// The document on disk

TEST_CASE ("A song survives a trip through XML", "[song][xml]")
{
    REQUIRE (rc::check ("saving and loading changes nothing observable", [] {
        Edited edited;
        edited.run (*genSteps());

        const auto loaded = Song::fromXml (edited.song.toXmlString());

        RC_ASSERT (loaded.has_value());
        RC_ASSERT (describe (*loaded) == describe (edited.song));
        checkInvariants (*loaded);
    }));
}

TEST_CASE ("A loaded song saves back to itself", "[song][xml]")
{
    REQUIRE (rc::check ("a second trip through XML is a no-op", [] {
        Edited edited;
        edited.run (*genSteps());

        // The first pass is allowed to change the file: fromXml fills in
        // anything a hand-written song may be missing (audio clip ids), and
        // the getters materialise a master bus. From there it has to be a
        // fixed point, or every open-and-save would rewrite the document.
        const auto once = Song::fromXml (edited.song.toXmlString());
        RC_ASSERT (once.has_value());

        const auto onceXml = once->toXmlString();
        const auto twice = Song::fromXml (onceXml);

        RC_ASSERT (twice.has_value());
        RC_ASSERT (twice->toXmlString() == onceXml);
    }));
}
