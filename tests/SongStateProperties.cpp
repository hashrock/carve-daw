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
// What that buys over an example test is the sequences nobody writes: make
// eleven patterns, clone the eighth, place it, shorten the pattern, undo four
// steps, place another clip. The interesting failures in a document model are
// exactly there -- in what an operation does to a state some *other* operation
// left behind.
//
// Three kinds of property, in the three sections below:
//
//   * every reachable state is a valid one (an automatic pattern name is one
//     nothing else has, clips name a pattern that exists, sorted lists come
//     back sorted, ...)
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

#include "PropertyGenerators.h"

namespace
{

using namespace carve::model;

using carve::test::genBpm;

// Beats land on a sixteenth-note grid, which is what every gesture in the app
// snaps to; the same reasoning as in SongTimeProperties.
constexpr double gridUnit = 0.25;
constexpr int maxGridSteps = 64;            // 16 beats

// Songs are kept small on purpose. The properties are about how operations
// interact, and a four-generator song reaches every interaction a forty-one
// one does while leaving a counterexample small enough to read.
constexpr int maxGenerators = 4;
constexpr int maxReturns = 4;

// Wider than the keyboard the piano roll draws, so that Note::setPitch's own
// clamp is what keeps a note on the MIDI range rather than the generator.
int pitchOf (double unitInterval)  { return (int) (unitInterval * 200.0) - 36; }

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
    createPattern,
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
    moveTempoChange,
    removeTempoChange,
    addTimeSigChange,
    moveTimeSigChange,
    removeTimeSigChange,
    setLoopRange,
    clearLoopRange,
    addAutomationPoint,
    removeAutomationPoint,
    addReturn,
    removeReturn,
    setSendGain,
    removeGenerator,
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
                           carve::test::genUnitInterval());
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

    // Where the step points, in beats. Occasionally negative, and every setter
    // it reaches clamps -- which is the point: a driver that only ever asked
    // for positions inside the song would leave every one of those clamps
    // untested, and the invariants below unfalsifiable.
    const auto beat = c * gridUnit;

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

        case createPattern:
        {
            if (auto generator = pickPatternGenerator (song, a))
                generator->createPattern (&um, gridUnit + beat);

            return;
        }

        case duplicatePattern:
        {
            // Into another generator as often as into its own: the copy is
            // made by the *destination*, and the two paths name and place it
            // differently, so both have to be reachable from here.
            if (auto picked = pickPattern (song, a, b))
                if (auto destination = pickPatternGenerator (song, c))
                    destination->duplicatePattern (picked->pattern, &um);

            return;
        }

        case removeUnusedPattern:
        {
            // The guard GeneratorController applies before it deletes a
            // pattern without taking its placements with it. Repeated here
            // because it is the reason the "every clip names a pattern that
            // exists" invariant below holds -- and therefore what that
            // invariant is really testing: if isPatternUsedInPlaylist ever
            // answered "no" for a pattern that is placed, this would leave a
            // clip pointing at nothing.
            if (auto picked = pickPattern (song, a, b))
                if (picked->pattern.isEmpty() && ! song.isPatternUsedInPlaylist (picked->pattern))
                    picked->generator.removePattern (picked->pattern, &um);
            return;
        }

        case setPatternLength:
        {
            if (auto picked = pickPattern (song, a, b))
                picked->pattern.setLengthBeats (gridUnit + beat, &um);
            return;
        }

        case addNote:
        {
            if (auto picked = pickPattern (song, a, b))
                picked->pattern.addNote (beat, gridUnit * (1 + b % 8),
                                         pitchOf (x), 1 + (int) (x * 126.0), &um);
            return;
        }

        case moveNote:
        {
            if (auto picked = pickPattern (song, a, b))
                if (auto note = pick (picked->pattern.getNotes(), c))
                {
                    note->setStart (beat, &um);
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
                song.getPlaylist().addClip (picked->generator, picked->pattern, beat, &um);
            return;
        }

        case moveClip:
        {
            if (auto clip = pick (song.getPlaylist().getClips(), a))
                clip->setStart (beat, &um);
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

        case removeGenerator:
        {
            // Any of them, clips and sidechains and all -- and a song with no
            // generators left is still a song.
            if (auto generator = pick (song.getGenerators(), a))
                song.removeGenerator (*generator, &um);
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
            // No guard: addTempoChange keeps the change already on the beat
            // and makes no second, so asking twice is the model's business
            // rather than the driver's. That is what makes the "one change per
            // beat" invariant below a property of the document.
            song.addTempoChange (beat,
                                 TempoChange::minBpm
                                     + x * (TempoChange::maxBpm - TempoChange::minBpm),
                                 &um);
            return;
        }

        case moveTempoChange:
        {
            // The drag path. It may aim at a beat another change already owns,
            // which is exactly the case setStartBeat has to refuse: a marker
            // dragged onto another must not stack on it.
            if (auto change = pick (song.getTempoChanges(), a))
                change->setStartBeat (beat, &um);
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
            song.addTimeSigChange (beat, { 1 + a % 16, 1 << (b % 5) }, &um);
            return;
        }

        case moveTimeSigChange:
        {
            if (auto change = pick (song.getTimeSigChanges(), a))
                change->setStartBeat (beat, &um);
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
            song.setLoopRange (beat, beat + (a - 32) * gridUnit, &um);
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
            out << "  pat " << pattern.getId() << " " << pattern.getName()
                << " len " << pattern.getLengthBeats() << "\n";

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
// thing that makes it true.
//
// Strictly ascending, because at most one change may sit on a beat: two of a
// kind on one beat would fight over which of them wins, and which won would be
// decided by nothing the user can see. addTempoChange and addTimeSigChange
// refuse to make such a pair and dropDuplicateChanges drops one arriving from
// disk, so this is the document's rule rather than a rule the driver keeps.
template <typename Changes>
void checkChangeList (const Changes& changes)
{
    double previousBeat = -1.0;

    for (const auto& change : changes)
    {
        RC_ASSERT (change.getStartBeat() > previousBeat + sameBeatTolerance);

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
        // Every pattern name in this generator is distinct. Nothing in the
        // model enforces it -- a user may rename two patterns the same -- but
        // nothing in the sequence renames one either, so every name here came
        // from makeUniquePatternName, and this is what holds it to its word:
        // both createPattern and duplicatePattern go through it, on their own
        // generator and across generators, so the check covers every path a
        // name is made on.
        std::vector<juce::String> patternNames;

        for (const auto& pattern : generator.getPatterns())
        {
            requireUnique (patternNames, pattern.getName());

            // An automatic name is never empty, so the picker always has
            // something to show for a pattern.
            RC_ASSERT (pattern.getName().isNotEmpty());

            // A pattern with no length has no bars to draw, and every
            // placement that inherits its length would go with it.
            RC_ASSERT (pattern.getLengthBeats() >= Pattern::minLengthBeats);

            // What a note may be. The piano roll held these lines already; the
            // model holds them now, so a MIDI import or a hand-written file
            // cannot get past them either.
            for (const auto& note : pattern.getNotes())
            {
                RC_ASSERT (note.getStart() >= 0.0);
                RC_ASSERT (note.getLength() >= Note::minLengthBeats);
                RC_ASSERT (note.getPitch() >= Note::lowestMidiNote);
                RC_ASSERT (note.getPitch() <= Note::highestMidiNote);
                RC_ASSERT (note.getVelocity() >= Note::quietestVelocity);
                RC_ASSERT (note.getVelocity() <= Note::loudestVelocity);
            }
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
        // node rather than appending a second -- and the return it names is
        // still there, because removeReturn takes its sends with it.
        //
        // EditSync still recognises a send naming nothing ("dangling send: the
        // return was deleted"): that is what a song saved before removeReturn
        // cleaned up can hold, and nothing in the model can produce now.
        std::vector<juce::String> sendTargets;

        for (const auto& send : generator.getSends())
        {
            requireUnique (sendTargets, send.getReturnId());
            RC_ASSERT (song.findReturn (send.getReturnId()).has_value());
        }

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
    for (const auto& clip : playlist.getClips())
    {
        auto generator = song.findGenerator (clip.getGeneratorId());
        RC_ASSERT (generator.has_value());
        RC_ASSERT (generator->findPattern (clip.getPatternId()).has_value());

        // A placement before the start of the song is never played and never
        // drawn, whichever of the two kinds of placement it is.
        RC_ASSERT (clip.getStart() >= 0.0);

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

        // Measured in seconds against a file, so neither a negative start,
        // a negative offset into it, nor a zero length names any of it.
        RC_ASSERT (clip.getStart() >= 0.0);
        RC_ASSERT (clip.getOffsetSeconds() >= 0.0);
        RC_ASSERT (clip.getLengthSeconds() >= AudioClip::minLengthSeconds);
    }

    checkChangeList (song.getTempoChanges());
    checkChangeList (song.getTimeSigChanges());

    for (const auto& change : song.getTimeSigChanges())
        RC_ASSERT (change.getSignature().getBeatsPerBar() > 0.0);

    // setLoopRange normalises a right-to-left drag and clamps both ends, so a
    // range is never inverted and never starts before the song, however it was
    // dragged.
    RC_ASSERT (song.getLoopStart() >= 0.0);
    RC_ASSERT (song.getLoopEnd() >= song.getLoopStart());

    // Every beats-to-seconds walk divides by the tempo. Asserted against what
    // is stored rather than against getTempo(), which floors what it returns:
    // the point is that nothing put a tempo the walks could not use into the
    // document, not that the getter would cover for one.
    RC_ASSERT ((double) song.state[ids::tempo] >= TempoChange::minBpm);
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

        // One pattern to start with, the way GeneratorController gives a new
        // generator one. Without it the steps that write notes and place clips
        // have nothing to act on until a step happens to make one, and a short
        // sequence would spend itself getting to the state the interesting
        // properties are about.
        generator.createPattern (nullptr, 4.0);
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

//==============================================================================
// Media that has gone missing
//
// The one property that has to touch the disk: relinking is a search of a
// real folder for real files, so the test lays a small tree of empty files
// out under the temp directory and takes it away again.

TEST_CASE ("Missing media is found again by name under a folder", "[song][media]")
{
    REQUIRE (rc::check ("relinking repoints exactly the nodes whose file name is under the folder", [] {
        auto song = Song::create ("property test");
        auto audio = song.addGenerator ("Audio", Generator::audioType, nullptr);
        auto sampler = song.addGenerator ("Sampler", Generator::samplerType, nullptr);

        // Every node points into a folder that does not exist, so all of them
        // start out missing; a sound is in the mix because it is relinked
        // through the same FileRef as a placement.
        const auto clipCount = *rc::gen::inRange (0, 6);
        const auto soundCount = *rc::gen::inRange (0, 3);

        for (int i = 0; i < clipCount; ++i)
            song.getPlaylist().addAudioClip (audio, audioFileFor (*rc::gen::inRange (0, 8)),
                                             i * 4.0, 1.0, nullptr);

        for (int i = 0; i < soundCount; ++i)
            sampler.addSound (audioFileFor (*rc::gen::inRange (0, 8)), nullptr);

        const auto missingBefore = song.findMissingMedia();
        RC_ASSERT ((int) missingBefore.size() == clipCount + soundCount);

        // Which of the eight possible names to put under the folder, and how
        // deep: the search is recursive, and a name at the top must not shadow
        // the same name further down or vice versa -- either is fine, as long
        // as the node ends up on a file of that name that exists.
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("carve-relink-" + juce::Uuid().toString());
        std::vector<juce::String> present;

        for (int i = 0; i < 8; ++i)
            if (*rc::gen::arbitrary<bool>())
            {
                const auto name = audioFileFor (i).getFileName();
                const auto depth = *rc::gen::inRange (0, 3);
                auto dir = folder;

                for (int d = 0; d < depth; ++d)
                    dir = dir.getChildFile ("sub" + juce::String (d));

                RC_ASSERT (dir.createDirectory().wasOk());
                RC_ASSERT (dir.getChildFile (name).create().wasOk());
                present.push_back (name);
            }

        RC_ASSERT (folder.createDirectory().wasOk());

        const auto relinked = song.relinkMissingMedia (folder, nullptr);

        int expected = 0;

        for (const auto& node : missingBefore)
        {
            const auto name = FileRef::getFileName (node);
            const auto file = FileRef::getFile (node);
            const bool shouldBeFound = std::find (present.begin(), present.end(), name) != present.end();

            if (shouldBeFound)
            {
                ++expected;
                RC_ASSERT (file.existsAsFile());
                RC_ASSERT (file.getFileName() == name);
                RC_ASSERT (file.isAChildOf (folder));

                // Stale the moment the file changes: the next save writes a
                // fresh one against the .carve's own folder.
                RC_ASSERT (! node.hasProperty (ids::relPath));
            }
            else
            {
                RC_ASSERT (! file.existsAsFile());
            }
        }

        RC_ASSERT (relinked == expected);
        RC_ASSERT ((int) song.findMissingMedia().size() == (int) missingBefore.size() - expected);

        // A second search of the same folder finds nothing new.
        RC_ASSERT (song.relinkMissingMedia (folder, nullptr) == 0);

        folder.deleteRecursively();
    }));
}

TEST_CASE ("A node with no file is not missing", "[song][media]")
{
    auto song = Song::create ("empty refs");
    auto audio = song.addGenerator ("Audio", Generator::audioType, nullptr);
    song.getPlaylist().addAudioClip (audio, juce::File(), 0.0, 1.0, nullptr);

    REQUIRE (song.findMissingMedia().empty());
    REQUIRE (song.relinkMissingMedia (juce::File::getSpecialLocation (juce::File::tempDirectory), nullptr) == 0);
}

//==============================================================================
// One change per beat
//
// The rule used to live in PlaylistComponent, which refused to make a second
// change on a beat that already had one. Below it the model had no opinion:
// two changes on one beat both survived, and which of them was in force fell
// out of a non-stable sort. A hand-edited or merged .carve could hold such a
// pair, and the app had no way to notice.
//
// It is the document's rule now, on all three paths that can write a change:
// addTempoChange and addTimeSigChange make no second, setStartBeat refuses to
// drag one onto another, and fromXml drops one arriving from disk. These pin
// the first and the third; the drag is a step in the sequences above, so
// checkChangeList holds it.

namespace
{

// Beats that reach either side of zero, so the clamp inside the add methods is
// part of what is being pinned rather than something the generator steps
// around -- and that sometimes land a hair away from a beat already asked for,
// which is what sameBeatTolerance exists for. A drag writes computed positions
// (a rounded beat, a bar line walked to from the start of the song), so a
// near-miss duplicate is the realistic case, not the exact one.
rc::Gen<double> genSignedBeat()
{
    return rc::gen::map (rc::gen::pair (rc::gen::inRange (-16, maxGridSteps + 1),
                                        rc::gen::inRange (-1, 2)),
                         [] (const std::pair<int, int>& pair)
                         {
                             return pair.first * gridUnit + pair.second * 1.0e-12;
                         });
}

// Where a change asked for at this beat lands: both setStartBeat methods, and
// both add methods, clamp at the start of the song.
double landsOn (double beat)  { return std::max (0.0, beat); }

// What a change of either kind is carrying, so the check below can be written
// once for both.
double payloadOf (const TempoChange& change)          { return change.getBpm(); }
TimeSignature payloadOf (const TimeSigChange& change)  { return change.getSignature(); }

// The payload the change on this beat should be carrying. `lastWins` is the
// difference between the two paths: adding refuses the second request, so the
// first one's payload stays, while loading keeps the change that was in force,
// which is the last in the document.
template <typename Payload>
std::optional<Payload> payloadOn (const std::vector<std::pair<double, Payload>>& specs,
                                  double beat, bool lastWins)
{
    std::optional<Payload> found;

    for (const auto& [asked, payload] : specs)
        if (isSameBeat (landsOn (asked), beat))
            if (lastWins || ! found)
                found = payload;

    return found;
}

// The changes stand on exactly the beats that were asked for, one each.
//
// Counted rather than compared against a list this rebuilds, so it says
// nothing about *how* the model got there -- only that every request is
// represented once and nothing else is.
template <typename Changes, typename Payload>
void checkChangesMatch (const Changes& changes,
                        const std::vector<std::pair<double, Payload>>& specs,
                        bool lastWins)
{
    checkChangeList (changes);

    for (const auto& change : changes)
    {
        const auto expected = payloadOn (specs, change.getStartBeat(), lastWins);

        // Every change stands where something asked for one...
        RC_ASSERT (expected.has_value());

        // ...carrying what that request wanted.
        RC_ASSERT (*expected == payloadOf (change));
    }

    // ...and every request is represented, exactly once.
    for (const auto& [asked, payload] : specs)
    {
        int found = 0;

        for (const auto& change : changes)
            if (isSameBeat (landsOn (asked), change.getStartBeat()))
                ++found;

        RC_ASSERT (found == 1);
    }
}

using TempoSpecs = std::vector<std::pair<double, double>>;
using SigSpecs = std::vector<std::pair<double, TimeSignature>>;

rc::Gen<TempoSpecs> genTempoSpecs()
{
    return rc::gen::container<TempoSpecs> (rc::gen::pair (genSignedBeat(), genBpm()));
}

rc::Gen<SigSpecs> genSigSpecs()
{
    const auto signature = rc::gen::map (rc::gen::pair (rc::gen::inRange (1, 17),
                                                        rc::gen::inRange (0, 5)),
                                         [] (const std::pair<int, int>& pair)
                                         {
                                             return TimeSignature { pair.first, 1 << pair.second };
                                         });

    return rc::gen::container<SigSpecs> (rc::gen::pair (genSignedBeat(), signature));
}

} // namespace

TEST_CASE ("A tempo change never shares its beat", "[song][changes]")
{
    REQUIRE (rc::check ("adding on an occupied beat leaves the change already there", [] {
        const auto specs = *genTempoSpecs();

        auto song = Song::create ("property test");

        for (const auto& [beat, bpm] : specs)
            song.addTempoChange (beat, bpm, nullptr);

        checkChangesMatch (song.getTempoChanges(), specs, false);
    }));
}

TEST_CASE ("A time signature change never shares its beat", "[song][changes]")
{
    REQUIRE (rc::check ("adding on an occupied beat leaves the change already there", [] {
        const auto specs = *genSigSpecs();

        auto song = Song::create ("property test");

        for (const auto& [beat, signature] : specs)
            song.addTimeSigChange (beat, signature, nullptr);

        checkChangesMatch (song.getTimeSigChanges(), specs, false);
    }));
}

TEST_CASE ("A song arriving with a duplicate loses it", "[song][changes][xml]")
{
    REQUIRE (rc::check ("loading keeps the change that was in force", [] {
        // Written straight into the tree rather than through the model, which
        // is the whole point: this is the shape a hand-edited or merged .carve
        // can have and nothing inside the app can produce.
        auto song = Song::create ("property test");
        auto tempos = juce::ValueTree (ids::TEMPOS);
        song.state.appendChild (tempos, nullptr);

        const auto specs = *genTempoSpecs();

        for (const auto& [beat, bpm] : specs)
        {
            juce::ValueTree change (ids::TEMPO);
            change.setProperty (ids::start, landsOn (beat), nullptr);
            change.setProperty (ids::bpm, bpm, nullptr);
            tempos.appendChild (change, nullptr);
        }

        const auto loaded = Song::fromXml (song.toXmlString());
        RC_ASSERT (loaded.has_value());

        // Last-wins, because that is the one getTempoAt and secondsFromBeats
        // had in force: both write over what they have for every change at or
        // before the beat. So the load drops only what nothing could hear.
        checkChangesMatch (loaded->getTempoChanges(), specs, true);
    }));
}

//==============================================================================
// Values a .carve can arrive with

TEST_CASE ("A song arriving out of range comes back inside it", "[song][xml]")
{
    REQUIRE (rc::check ("loading holds every value to what its setter would have", [] {
        // Written straight onto the tree, so nothing here has been past a
        // setter -- which is exactly the state a hand-written or hand-edited
        // .carve is in, and the one the clamps in the setters cannot help
        // with. The generators reach well outside every range on purpose.
        auto song = Song::create ("property test");
        auto generator = song.addGenerator ("Wild", "4osc", nullptr);
        auto pattern = generator.createPattern (nullptr, 4.0);

        const auto wild = [] { return *rc::gen::inRange (-500, 501); };

        pattern.state.setProperty (ids::lengthBeats, wild() * 0.25, nullptr);

        const auto noteCount = *rc::gen::inRange (0, 8);

        for (int i = 0; i < noteCount; ++i)
        {
            juce::ValueTree note (ids::NOTE);
            note.setProperty (ids::start, wild() * 0.25, nullptr);
            note.setProperty (ids::length, wild() * 0.25, nullptr);
            note.setProperty (ids::pitch, wild(), nullptr);
            note.setProperty (ids::velocity, wild(), nullptr);
            pattern.state.appendChild (note, nullptr);
        }

        auto clip = song.getPlaylist().addClip (generator, pattern, 0.0, nullptr);
        clip.state.setProperty (ids::start, wild() * 0.25, nullptr);
        clip.state.setProperty (ids::length, wild() * 0.25, nullptr);

        song.state.setProperty (ids::tempo, wild(), nullptr);
        song.state.setProperty (ids::loopStart, wild() * 0.25, nullptr);
        song.state.setProperty (ids::loopEnd, wild() * 0.25, nullptr);

        const auto loaded = Song::fromXml (song.toXmlString());
        RC_ASSERT (loaded.has_value());

        // The same invariants every edited song is held to. Which is the
        // point: where a value came from should not decide whether the rest of
        // the app can trust it.
        checkInvariants (*loaded);

        // Nothing was dropped on the way: clamping moves values, it does not
        // remove notes.
        RC_ASSERT (loaded->getGenerator (0).getPattern (0).getNumNotes() == noteCount);
    }));
}


//==============================================================================
// Counting and indexing children by type
//
// Several nodes in the document hold more than one kind of child -- a playlist
// holds pattern placements and audio placements -- and the ones that hold only
// one today are a feature away from holding two. So the accessors that count
// and index are held to the same answer as the ones that collect, whatever
// else is sitting alongside.

TEST_CASE ("Counting children ignores the children of other kinds", "[song][state]")
{
    REQUIRE (rc::check ("a foreign child changes no count and shifts no index", [] {
        auto song = Song::create ("property test");
        auto generator = song.addGenerator ("Gen", "4osc", nullptr);
        auto pattern = generator.createPattern (nullptr, 16.0);

        const auto noteCount = *rc::gen::inRange (0, 8);

        for (int i = 0; i < noteCount; ++i)
            pattern.addNote (i * 0.5, 0.25, 60 + i, 100, nullptr);

        // Audio placements really are children of the playlist node, so this
        // half of the property is about today's document rather than a
        // hypothetical one.
        const auto clipCount = *rc::gen::inRange (0, 4);
        const auto audioCount = *rc::gen::inRange (0, 4);

        auto playlist = song.getPlaylist();

        for (int i = 0; i < clipCount; ++i)
            playlist.addClip (generator, pattern, i * 16.0, nullptr);

        for (int i = 0; i < audioCount; ++i)
            playlist.addAudioClip (generator, juce::File(), 1000.0 + i, 1.0, nullptr);

        // Whatever a later feature might hang off these nodes, standing in for
        // an automation lane under a pattern or a marker on the playlist.
        const auto foreignCount = *rc::gen::inRange (0, 3);

        for (int i = 0; i < foreignCount; ++i)
        {
            pattern.state.appendChild (juce::ValueTree ("SOMETHINGELSE"), nullptr);
            playlist.state.appendChild (juce::ValueTree ("SOMETHINGELSE"), nullptr);
        }

        RC_ASSERT (pattern.getNumNotes() == noteCount);
        RC_ASSERT (playlist.getNumClips() == clipCount);

        // The indexed accessor and the collecting one name the same children
        // in the same order, which is what makes the two interchangeable.
        const auto notes = pattern.getNotes();
        RC_ASSERT ((int) notes.size() == noteCount);

        for (int i = 0; i < noteCount; ++i)
            RC_ASSERT (pattern.getNote (i).state == notes[(size_t) i].state);

        const auto clips = playlist.getClips();
        RC_ASSERT ((int) clips.size() == clipCount);

        for (int i = 0; i < clipCount; ++i)
            RC_ASSERT (playlist.getClip (i).state == clips[(size_t) i].state);
    }));
}


//==============================================================================
// Placing a copy of a clip

TEST_CASE ("A placed copy says exactly what the original said", "[song][state]")
{
    REQUIRE (rc::check ("a copy carries over what was stated and nothing more", [] {
        auto song = Song::create ("property test");
        auto generator = song.addGenerator ("Gen", "4osc", nullptr);
        auto pattern = generator.createPattern (nullptr, 4.0);
        auto playlist = song.getPlaylist();

        // The source is built by hand rather than through addClip, so that
        // what the copy is checked against does not come from the code under
        // test. "Absent" is the case that matters: a clip with no length of
        // its own plays for as long as its pattern, and a copy that grew a
        // length property would stop following the pattern when it changed.
        const auto ownLength = *rc::gen::maybe (rc::gen::map (rc::gen::inRange (1, 65),
                                                              [] (int steps) { return steps * 0.25; }));
        const auto transpose = *rc::gen::inRange (-60, 61);

        juce::ValueTree sourceState (ids::CLIP);
        sourceState.setProperty (ids::generatorId, generator.getId(), nullptr);
        sourceState.setProperty (ids::patternId, pattern.getId(), nullptr);
        sourceState.setProperty (ids::start, 0.0, nullptr);

        if (ownLength)
            sourceState.setProperty (ids::length, *ownLength, nullptr);

        if (transpose != 0)
            sourceState.setProperty (ids::transpose, transpose, nullptr);

        playlist.state.appendChild (sourceState, nullptr);

        const PlaylistClip source (sourceState);
        const auto start = *rc::gen::inRange (0, 64) * 0.25;

        auto copy = playlist.addClip (generator, pattern, start,
                                      ClipPlacement::of (source), nullptr);

        RC_ASSERT (copy.getGeneratorId() == generator.getId());
        RC_ASSERT (copy.getPatternId() == pattern.getId());
        RC_ASSERT (copy.getStart() == start);

        // Exactly the properties the original had, and with the same values --
        // so a duplicated clip's XML matches the clip it came from.
        RC_ASSERT (copy.state.hasProperty (ids::length) == (bool) ownLength);
        RC_ASSERT (copy.state.hasProperty (ids::transpose) == (transpose != 0));

        if (ownLength)
            RC_ASSERT (copy.getLength (pattern.getLengthBeats())
                           == PlaylistClip::clampLength (*ownLength));

        RC_ASSERT (copy.getTranspose() == PlaylistClip::clampTranspose (transpose));

        // And it plays for the same time either way, which is the thing a
        // duplicate is actually judged on.
        RC_ASSERT (copy.getLength (pattern.getLengthBeats())
                       == source.getLength (pattern.getLengthBeats()));
    }));
}
